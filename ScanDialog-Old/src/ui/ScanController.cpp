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

    connect(m_debounceTimer, &QTimer::timeout, this, [this]() {
        performSearch();
    });

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

void ScanController::onMftEntryAdded(uint64_t key) {
    std::lock_guard<std::mutex> lock(m_resultsMutex);
    if (m_resultSet->keyToPos.count(key)) return;

    int idx = MftReader::instance().getIndexByKey(key);
    if (idx == -1) return;

    bool matches = MftReader::instance().matchEntry(idx, m_searchText, m_filterState.useRegex, m_filterState.caseSensitive, 
                                                   m_filterState.extensionList, m_filterState.includeHidden, m_filterState.includeSystem,
                                                   m_filterState.includeDollar);
    
    // 如果查询为空，只有在开启自动显示的情况下才认为匹配
    if (m_searchText.isEmpty() && m_filterState.extensionList.isEmpty()) {
        matches = m_filterState.autoDisplay && matches;
    }

    if (matches) {
        
        // 2026-06-xx 物理加固：采用 Copy-On-Write 机制确保 ResultSet 快照的不可变性，杜绝 UI 线程数据竞争
        auto newSet = std::make_shared<ResultSet>(*m_resultSet);
        auto itInsert = std::lower_bound(newSet->keys.begin(), newSet->keys.end(), key, [this](uint64_t a, uint64_t b) {
            return compareKeys(a, b, m_currentSortColumn, m_currentSortOrder);
        });

        int row = static_cast<int>(std::distance(newSet->keys.begin(), itInsert));
        newSet->keys.insert(itInsert, key);
        updateKeyToPosMapping(*newSet); 
        m_resultSet = newSet;

        emit entryAdded(newSet, key, row);
    }
}

void ScanController::onMftEntryRemoved(uint64_t key) {
    std::lock_guard<std::mutex> lock(m_resultsMutex);
    auto itPos = m_resultSet->keyToPos.find(key);
    if (itPos != m_resultSet->keyToPos.end()) {
        int row = itPos->second;
        
        auto newSet = std::make_shared<ResultSet>(*m_resultSet);
        newSet->keys.erase(newSet->keys.begin() + row);
        updateKeyToPosMapping(*newSet);
        m_resultSet = newSet;

        emit entryRemoved(newSet, key, row);
    }
}

void ScanController::onMftEntryUpdated(uint64_t key) {
    std::lock_guard<std::mutex> lock(m_resultsMutex);
    auto itPos = m_resultSet->keyToPos.find(key);
    int idx = MftReader::instance().getIndexByKey(key);
    
    bool matches = (idx != -1) && MftReader::instance().matchEntry(idx, m_searchText, m_filterState.useRegex, m_filterState.caseSensitive, 
                                                                  m_filterState.extensionList, m_filterState.includeHidden, m_filterState.includeSystem,
                                                                  m_filterState.includeDollar);
    
    // 如果查询为空，只有在开启自动显示的情况下才认为匹配
    if (m_searchText.isEmpty() && m_filterState.extensionList.isEmpty()) {
        matches = m_filterState.autoDisplay && matches;
    }

    if (itPos != m_resultSet->keyToPos.end()) {
        int row = itPos->second;
        if (matches) {
            // 2026-06-xx 工业级增强：检测排序字段是否发生物理偏移
            // 简单起见，如果匹配则重新执行一次一致性位置校验，若位置变动则执行“物理迁移”
            auto newSet = std::make_shared<ResultSet>(*m_resultSet);
            newSet->keys.erase(newSet->keys.begin() + row);
            
            auto itInsert = std::lower_bound(newSet->keys.begin(), newSet->keys.end(), key, [this](uint64_t a, uint64_t b) {
                return compareKeys(a, b, m_currentSortColumn, m_currentSortOrder);
            });
            int newRow = static_cast<int>(std::distance(newSet->keys.begin(), itInsert));
            
            if (newRow == row) {
                // 位置未变，仅发射更新信号
                emit entryUpdated(m_resultSet, key, row);
            } else {
                // 位置变了，执行删除并插入的原子组合
                newSet->keys.insert(itInsert, key);
                updateKeyToPosMapping(*newSet);
                m_resultSet = newSet;
                
                emit entryRemoved(newSet, key, row);
                emit entryAdded(newSet, key, newRow);
            }
        } else {
            auto newSet = std::make_shared<ResultSet>(*m_resultSet);
            newSet->keys.erase(newSet->keys.begin() + row);
            updateKeyToPosMapping(*newSet);
            m_resultSet = newSet;
            emit entryRemoved(newSet, key, row);
        }
    } else if (matches) {
        auto newSet = std::make_shared<ResultSet>(*m_resultSet);
        auto itInsert = std::lower_bound(newSet->keys.begin(), newSet->keys.end(), key, [this](uint64_t a, uint64_t b) {
            return compareKeys(a, b, m_currentSortColumn, m_currentSortOrder);
        });
        int row = static_cast<int>(std::distance(newSet->keys.begin(), itInsert));
        newSet->keys.insert(itInsert, key);
        updateKeyToPosMapping(*newSet);
        m_resultSet = newSet;
        emit entryAdded(newSet, key, row);
    }
}

} // namespace ArcMeta
