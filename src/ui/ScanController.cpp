#include "ScanController.h"
#include "../mft/MftReader.h"
#include <QtConcurrent/QtConcurrent>
#include <QElapsedTimer>

namespace ArcMeta {

ScanController::ScanController(QObject* parent) : QObject(parent) {
    m_resultSet = std::make_shared<ResultSet>();
    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(300); // 300ms 黄金防抖时间

    m_incrementalTimer = new QTimer(this);
    m_incrementalTimer->setInterval(150); // 150ms 聚合更新增量，解决大规模写入时的假死

    connect(m_debounceTimer, &QTimer::timeout, this, [this]() {
        performSearch();
    });
    connect(m_incrementalTimer, &QTimer::timeout, this, &ScanController::processPendingIncrementalUpdates);

    auto& reader = MftReader::instance();
    connect(&reader, &MftReader::entryAdded, this, &ScanController::onMftEntryAdded);
    connect(&reader, &MftReader::entryRemoved, this, &ScanController::onMftEntryRemoved);
    connect(&reader, &MftReader::entryUpdated, this, &ScanController::onMftEntryUpdated);

    connect(&m_sortWatcher, &QFutureWatcher<std::vector<uint64_t>>::finished, this, [this]() {
        if (m_sortWatcher.isCanceled()) return;
        auto newSet = std::make_shared<ResultSet>();
        newSet->keys = m_sortWatcher.result();
        updateKeyToPosMapping(*newSet);
        
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_resultSet = newSet;
        }
        emit resultsSwapped(newSet);
    });
}

ScanController::~ScanController() {
    m_watcher.cancel();
    m_watcher.waitForFinished();
}

void ScanController::setSearchText(const QString& text) {
    if (m_searchText == text) return;
    m_searchText = text;
}

void ScanController::setFilterState(const ScanFilterState& state) {
    // 简单比对逻辑省略，直接赋值
    m_filterState = state;
}

void ScanController::triggerSearch(bool immediate) {
    if (immediate) {
        m_debounceTimer->stop();
        performSearch();
    } else {
        m_debounceTimer->start();
    }
}

std::shared_ptr<ResultSet> ScanController::snapshot() const {
    std::lock_guard<std::mutex> lock(m_resultsMutex);
    return m_resultSet;
}

int ScanController::resultCount() const {
    std::lock_guard<std::mutex> lock(m_resultsMutex);
    return static_cast<int>(m_resultSet->keys.size());
}

void ScanController::performSearch() {
    if (m_watcher.isRunning()) m_watcher.cancel();

    emit searchStarted();
    
    QElapsedTimer timer;
    timer.start();

    auto future = QtConcurrent::run([text = m_searchText, state = m_filterState]() {
        // 如果开启自动显示且查询为空，则执行全量搜索（带过滤）
        if (state.autoDisplay && text.isEmpty() && state.extensionList.isEmpty()) {
            return MftReader::instance().search("", state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
        }
        // 否则，如果不是自动显示且查询为空，返回空结果
        if (!state.autoDisplay && text.isEmpty() && state.extensionList.isEmpty()) {
            return std::vector<uint64_t>();
        }
        return MftReader::instance().search(text, state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
    });

    disconnect(&m_watcher, &QFutureWatcher<std::vector<uint64_t>>::finished, this, nullptr);
    connect(&m_watcher, &QFutureWatcher<std::vector<uint64_t>>::finished, this, [this, timer]() {
        if (m_watcher.isCanceled()) return;
        
        auto newSet = std::make_shared<ResultSet>();
        newSet->keys = m_watcher.result();
        updateKeyToPosMapping(*newSet);

        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_resultSet = newSet;
        }
        emit searchFinished(static_cast<int>(m_resultSet->keys.size()), timer.elapsed());
    });

    m_watcher.setFuture(future);
}

bool ScanController::compareKeys(uint64_t a, uint64_t b, int column, int order) {
    auto& reader = MftReader::instance();
    int idxA = reader.getIndexByKey(a);
    int idxB = reader.getIndexByKey(b);
    if (idxA == -1 || idxB == -1) return false;

    bool less = false;
    switch (column) {
        case 0: less = QString::compare(reader.getName(idxA), reader.getName(idxB), Qt::CaseInsensitive) < 0; break;
        case 1: less = QString::compare(reader.getFullPath(idxA), reader.getFullPath(idxB), Qt::CaseInsensitive) < 0; break;
        case 2: less = reader.getSize(idxA) < reader.getSize(idxB); break;
        case 3: less = reader.getModifyTime(idxA) < reader.getModifyTime(idxB); break;
        default: return false;
    }
    return (order == 0 /* Qt::AscendingOrder */) ? less : !less;
}

void ScanController::sort(int column, int order) {
    m_currentSortColumn = column;
    m_currentSortOrder = order;

    if (m_sortWatcher.isRunning()) m_sortWatcher.cancel();

    std::vector<uint64_t> currentKeys;
    {
        std::lock_guard<std::mutex> lock(m_resultsMutex);
        currentKeys = m_resultSet->keys;
    }

    auto future = QtConcurrent::run([keys = std::move(currentKeys), column, order]() mutable {
        std::sort(keys.begin(), keys.end(), [column, order](uint64_t a, uint64_t b) {
            return compareKeys(a, b, column, order);
        });
        return keys;
    });

    m_sortWatcher.setFuture(future);
}

void ScanController::updateKeyToPosMapping(ResultSet& rs) {
    rs.keyToPos.clear();
    rs.keyToPos.reserve(rs.keys.size());
    for (size_t i = 0; i < rs.keys.size(); ++i) {
        rs.keyToPos[rs.keys[i]] = static_cast<int>(i);
    }
}

void ScanController::onMftEntryAdded(uint32_t index) {
    uint64_t key = MftReader::instance().getKeyByIndex(index);
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingUpdates.push_back({ PendingUpdate::Added, key, index });
    if (!m_incrementalTimer->isActive()) m_incrementalTimer->start();
}

void ScanController::onMftEntryRemoved(uint64_t key) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingUpdates.push_back({ PendingUpdate::Removed, key, 0 });
    if (!m_incrementalTimer->isActive()) m_incrementalTimer->start();
}

void ScanController::onMftEntryUpdated(uint32_t index) {
    uint64_t key = MftReader::instance().getKeyByIndex(index);
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingUpdates.push_back({ PendingUpdate::Updated, key, index });
    if (!m_incrementalTimer->isActive()) m_incrementalTimer->start();
}

void ScanController::processPendingIncrementalUpdates() {
    std::vector<PendingUpdate> updates;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        updates = std::move(m_pendingUpdates);
        m_pendingUpdates.clear();
        m_incrementalTimer->stop();
    }

    if (updates.empty()) return;

    std::lock_guard<std::mutex> lock(m_resultsMutex);
    auto newSet = std::make_shared<ResultSet>(*m_resultSet);
    bool changed = false;

    for (const auto& up : updates) {
        if (up.type == PendingUpdate::Removed) {
            auto itPos = newSet->keyToPos.find(up.key);
            if (itPos != newSet->keyToPos.end()) {
                int row = itPos->second;
                newSet->keys.erase(newSet->keys.begin() + row);
                updateKeyToPosMapping(*newSet);
                changed = true;
                emit entryRemoved(newSet, up.key, row);
            }
        } else {
            bool matches = MftReader::instance().matchEntry((int)up.mftIndex, m_searchText, m_filterState.useRegex, m_filterState.caseSensitive, 
                                                            m_filterState.extensionList, m_filterState.includeHidden, m_filterState.includeSystem,
                                                            m_filterState.includeDollar);
            if (m_searchText.isEmpty() && m_filterState.extensionList.isEmpty()) matches = m_filterState.autoDisplay && matches;

            auto itPos = newSet->keyToPos.find(up.key);
            if (itPos != newSet->keyToPos.end()) {
                int row = itPos->second;
                if (matches) {
                    auto itInsert = std::lower_bound(newSet->keys.begin(), newSet->keys.end(), up.key, [this](uint64_t a, uint64_t b) {
                        return compareKeys(a, b, m_currentSortColumn, m_currentSortOrder);
                    });
                    int newRow = static_cast<int>(std::distance(newSet->keys.begin(), itInsert));
                    if (newRow == row) emit entryUpdated(newSet, up.key, row);
                    else {
                        newSet->keys.erase(newSet->keys.begin() + row);
                        newSet->keys.insert(std::lower_bound(newSet->keys.begin(), newSet->keys.end(), up.key, [this](uint64_t a, uint64_t b) {
                            return compareKeys(a, b, m_currentSortColumn, m_currentSortOrder);
                        }), up.key);
                        updateKeyToPosMapping(*newSet);
                        changed = true;
                        emit entryRemoved(newSet, up.key, row);
                        emit entryAdded(newSet, up.key, newRow);
                    }
                } else {
                    newSet->keys.erase(newSet->keys.begin() + row);
                    updateKeyToPosMapping(*newSet);
                    changed = true;
                    emit entryRemoved(newSet, up.key, row);
                }
            } else if (matches) {
                auto itInsert = std::lower_bound(newSet->keys.begin(), newSet->keys.end(), up.key, [this](uint64_t a, uint64_t b) {
                    return compareKeys(a, b, m_currentSortColumn, m_currentSortOrder);
                });
                int row = static_cast<int>(std::distance(newSet->keys.begin(), itInsert));
                newSet->keys.insert(itInsert, up.key);
                updateKeyToPosMapping(*newSet);
                changed = true;
                emit entryAdded(newSet, up.key, row);
            }
        }
    }
    if (changed) m_resultSet = newSet;
}

} // namespace ArcMeta
