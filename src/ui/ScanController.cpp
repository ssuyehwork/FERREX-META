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

    m_batchTimer = new QTimer(this);
    m_batchTimer->setSingleShot(true);
    m_batchTimer->setInterval(200); // 200ms 批量处理间隔
    connect(m_batchTimer, &QTimer::timeout, this, &ScanController::processBatchUpdates);

    auto& reader = MftReader::instance();
    connect(&reader, &MftReader::entriesChangedBatch, this, &ScanController::processBatchUpdates);

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

void ScanController::onMftEntryAdded(uint32_t) {}
void ScanController::onMftEntryRemoved(uint64_t) {}
void ScanController::onMftEntryUpdated(uint32_t) {}

void ScanController::processBatchUpdates() {
    auto events = MftReader::instance().pullChangeJournal();
    if (events.empty()) return;

    // 如果积压事件过多（例如超过 1000 个），直接触发全量重搜，避免频繁执行低效的 O(N) 增量更新
    if (events.size() > 1000) {
        qDebug() << "[ScanController] 积压事件过多 (" << events.size() << ")，触发全量搜索优化";
        triggerSearch(true);
        return;
    }

    std::lock_guard<std::mutex> lock(m_resultsMutex);
    auto newSet = std::make_shared<ResultSet>(*m_resultSet);
    bool changed = false;

    for (const auto& ev : events) {
        auto itPos = newSet->keyToPos.find(ev.key);

        // 匹配逻辑（复用）
        auto checkMatch = [&](uint32_t idx) {
            if (idx == (uint32_t)-1) return false;
            bool m = MftReader::instance().matchEntry((int)idx, m_searchText, m_filterState.useRegex, m_filterState.caseSensitive,
                                                     m_filterState.extensionList, m_filterState.includeHidden, m_filterState.includeSystem,
                                                     m_filterState.includeDollar);
            if (m_searchText.isEmpty() && m_filterState.extensionList.isEmpty()) {
                m = m_filterState.autoDisplay && m;
            }
            return m;
        };

        if (ev.type == MftReader::ChangeEvent::Added) {
            if (itPos != newSet->keyToPos.end()) continue;
            if (checkMatch(ev.index)) {
                newSet->keys.push_back(ev.key);
                changed = true;
            }
        } else if (ev.type == MftReader::ChangeEvent::Removed) {
            if (itPos != newSet->keyToPos.end()) {
                newSet->keys[itPos->second] = 0; // 标记删除
                changed = true;
            }
        } else if (ev.type == MftReader::ChangeEvent::Updated) {
            bool matches = checkMatch(ev.index);
            if (itPos != newSet->keyToPos.end()) {
                if (!matches) {
                    newSet->keys[itPos->second] = 0; // 标记删除
                    changed = true;
                }
                // 命中且匹配的情况，由于复合 Key 不变，SoA 数据由 MftReader 负责，此处无需操作
            } else if (matches) {
                newSet->keys.push_back(ev.key);
                changed = true;
            }
        }
    }

    if (changed) {
        // 物理清理标记删除的项
        newSet->keys.erase(std::remove(newSet->keys.begin(), newSet->keys.end(), 0), newSet->keys.end());

        // 重新排序
        std::sort(newSet->keys.begin(), newSet->keys.end(), [this](uint64_t a, uint64_t b) {
            return compareKeys(a, b, m_currentSortColumn, m_currentSortOrder);
        });

        updateKeyToPosMapping(*newSet);
        m_resultSet = newSet;
        emit resultsSwapped(newSet);
    }
}

} // namespace ArcMeta
