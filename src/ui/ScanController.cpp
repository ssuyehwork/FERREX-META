#include "ScanController.h"
#include "../mft/MftReader.h"
#include <QtConcurrent/QtConcurrent>
#include <QElapsedTimer>

namespace FERREX {

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

    connect(&m_sortWatcher, &QFutureWatcher<std::shared_ptr<ResultSet>>::finished, this, [this]() {
        if (m_sortWatcher.isCanceled()) return;
        
        std::shared_ptr<ResultSet> newSet = m_sortWatcher.result();
        if (!newSet || newSet->keys.empty()) return;

        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            // 2026-06-xx 物理防线：校验基准快照。如果期间执行了搜索，m_resultSet 会更新，
            // 此时后台异步完成的增量排序结果已经失效（基于旧数据），必须舍弃，防止搜索结果被“秒消失”。
            if (m_resultSet != m_sortBaseSnap) {
                qDebug() << "[ScanController] 舍弃过时的重排序结果";
                return;
            }
            m_resultSet = newSet;
        }
        emit resultsSwapped(newSet);
    });
}

ScanController::~ScanController() {
    m_watcher.cancel();
    m_watcher.waitForFinished();
    m_sortWatcher.cancel();
    m_sortWatcher.waitForFinished();
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
    if (m_sortWatcher.isRunning()) m_sortWatcher.cancel();

    emit searchStarted();
    
    QElapsedTimer timer;
    timer.start();

    // 2026-06-xx 性能优化：对于明确为空且未开启自动显示的请求，直接在 UI 线程构造空结果，避免线程调度开销
    const QString text = m_searchText;
    const ScanFilterState state = m_filterState;

    if (!state.autoDisplay && text.isEmpty() && state.extensionList.isEmpty()) {
        auto newSet = std::make_shared<ResultSet>();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_resultSet = newSet;
        }
        emit resultsSwapped(newSet);
        emit searchFinished(0, timer.elapsed());
        return;
    }

    auto future = QtConcurrent::run([this, text, state]() {
        std::vector<uint64_t> keys;
        // 如果开启自动显示且查询为空，则执行全量搜索（带过滤）
        if (state.autoDisplay && text.isEmpty() && state.extensionList.isEmpty()) {
            keys = MftReader::instance().search("", state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
        }
        else {
            keys = MftReader::instance().search(text, state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
        }

        auto rs = std::make_shared<ResultSet>();
        rs->keys = std::move(keys);
        updateKeyToPosMapping(*rs);
        return rs;
    });

    disconnect(&m_watcher, &QFutureWatcher<std::shared_ptr<ResultSet>>::finished, this, nullptr);
    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<ResultSet>>::finished, this, [this, timer]() {
        if (m_watcher.isCanceled()) return;
        
        std::shared_ptr<ResultSet> newSet = m_watcher.result();

        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_resultSet = newSet;
        }
        emit resultsSwapped(newSet);
        emit searchFinished(static_cast<int>(m_resultSet->keys.size()), timer.elapsed());
    });

    m_watcher.setFuture(future);
}

// 2026-06-xx 极致性能重构：排序键投影 (Key Projection) 结构体
struct SortProxy {
    uint64_t key;
    int64_t iVal = 0;
    std::string sVal;
};

bool ScanController::compareKeys(uint64_t a, uint64_t b, int column, int order) {
    // 降级兼容路径：仅在增量插入时使用，性能非瓶颈
    auto& reader = MftReader::instance();
    int idxA = reader.getIndexByKey(a);
    int idxB = reader.getIndexByKey(b);
    if (idxA == -1 || idxB == -1) return false;

    bool less = false;
    switch (column) {
        case 0: less = reader.getName(idxA).compare(reader.getName(idxB), Qt::CaseInsensitive) < 0; break;
        case 1: less = reader.getFullPath(idxA).compare(reader.getFullPath(idxB), Qt::CaseInsensitive) < 0; break;
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

    std::shared_ptr<ResultSet> snap = snapshot();
    
    // 2026-06-xx 极致架构优化：去锁化投影排序。
    // 理由：通过物理拷贝文件名/数值至投影结构，彻底杜绝排序过程中的锁竞争与野指针风险。
    auto future = QtConcurrent::run([this, snap, column, order]() {
        auto newSet = std::make_shared<ResultSet>();
        newSet->keys = snap->keys;
        if (newSet->keys.empty()) return newSet;

        auto& reader = MftReader::instance();
        std::vector<SortProxy> proxies;
        proxies.reserve(newSet->keys.size());

        // 1. 投影阶段：申请单次大范围读锁，直接从 SoA 池物理拷贝数据
        // 理由：彻底消除 O(N) 次的锁申请/释放开销，并绕过 QString 中转，极致压榨 CPU 性能。
        {
            QReadLocker lock(&reader.m_dataLock);
            for (uint64_t k : newSet->keys) {
                auto it = reader.m_frn_to_idx.find(k);
                SortProxy p; p.key = k;
                if (it != reader.m_frn_to_idx.end()) {
                    uint32_t idx = it->second;
                    if (column == 0) p.sVal = reinterpret_cast<const char*>(reader.m_string_pool.data() + reader.m_name_offsets[idx]);
                    else if (column == 1) {
                        // 路径投影相对复杂，暂时维持现状或使用缓存。为了安全与一致性，此处调用 getPathFast
                        p.sVal = QString::fromStdWString(reader.getPathFast(static_cast<size_t>(reader.m_parent_frns[idx] >> 48), reader.m_frns[idx])).toStdString();
                    }
                    else if (column == 2) p.iVal = reader.m_sizes[idx];
                    else if (column == 3) p.iVal = reader.m_timestamps[idx];
                }
                proxies.push_back(std::move(p));
            }
        }

        // 2. 排序阶段：完全去锁化计算 (顺序执行，以确保最大环境兼容性)
        std::sort(proxies.begin(), proxies.end(), [column, order](const SortProxy& a, const SortProxy& b) {
            bool less = false;
            if (column == 0 || column == 1) {
                less = _stricmp(a.sVal.c_str(), b.sVal.c_str()) < 0;
            } else {
                less = a.iVal < b.iVal;
            }
            return (order == 0) ? less : !less;
        });

        // 3. 写回结果并构建映射
        for (size_t i = 0; i < newSet->keys.size(); ++i) newSet->keys[i] = proxies[i].key;
        updateKeyToPosMapping(*newSet);
        return newSet;
    });

    m_sortBaseSnap = snap;
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

    // 2026-06-xx 极致性能重构：将增量变动处理与重排序移至后台线程，彻底解决 200万+ 数据下的 UI 假死
    if (m_sortWatcher.isRunning()) {
        // 如果当前正在执行重排序，则暂缓处理，等待下一次聚合通知（Debounce 效应）
        return;
    }

    if (events.size() > 2000) {
        qDebug() << "[ScanController] 积压事件超限 (" << events.size() << ")，切换至全量异步搜索";
        triggerSearch(true);
        return;
    }

    std::shared_ptr<ResultSet> snap = snapshot();
    auto future = QtConcurrent::run([this, snap, events, text = m_searchText, state = m_filterState, 
                                     column = m_currentSortColumn, order = m_currentSortOrder]() {
        // 2026-06-xx 极致性能优化：延迟拷贝。
        // 理由：直接对 snap 进行 * 解引用拷贝会克隆整个 unordered_map (200万项)，
        // 这在 UI 线程频繁触发时会导致严重的亚秒级停顿（假死）。
        std::shared_ptr<ResultSet> newSet;
        bool changed = false;

        auto& reader = MftReader::instance();
        for (const auto& ev : events) {
            // 在未确定变动前，使用旧 snap 的映射进行 O(1) 预判
            auto itPos = snap->keyToPos.find(ev.key);
            
            auto checkMatch = [&](uint32_t idx) {
                if (idx == (uint32_t)-1) return false;
                bool m = reader.matchEntry((int)idx, text, state.useRegex, state.caseSensitive, 
                                           state.extensionList, state.includeHidden, state.includeSystem,
                                           state.includeDollar);
                if (text.isEmpty() && state.extensionList.isEmpty()) m = state.autoDisplay && m;
                return m;
            };

            if (ev.type == MftReader::ChangeEvent::Added) {
                if (itPos == snap->keyToPos.end() && checkMatch(ev.index)) {
                    if (!newSet) newSet = std::make_shared<ResultSet>(*snap);
                    newSet->keys.push_back(ev.key);
                    changed = true;
                }
            } else if (ev.type == MftReader::ChangeEvent::Removed) {
                if (itPos != snap->keyToPos.end()) {
                    if (!newSet) newSet = std::make_shared<ResultSet>(*snap);
                    newSet->keys[itPos->second] = 0;
                    changed = true;
                }
            } else if (ev.type == MftReader::ChangeEvent::Updated) {
                bool matches = checkMatch(ev.index);
                if (itPos != snap->keyToPos.end()) {
                    if (!matches) {
                        if (!newSet) newSet = std::make_shared<ResultSet>(*snap);
                        newSet->keys[itPos->second] = 0;
                        changed = true;
                    }
                } else if (matches) {
                    if (!newSet) newSet = std::make_shared<ResultSet>(*snap);
                    newSet->keys.push_back(ev.key);
                    changed = true;
                }
            }
        }

        if (changed && newSet) {
            newSet->keys.erase(std::remove(newSet->keys.begin(), newSet->keys.end(), 0), newSet->keys.end());
            
            // 执行后台安全重排序 (复用投影排序逻辑)
            std::vector<SortProxy> proxies;
            proxies.reserve(newSet->keys.size());
            {
                QReadLocker lock(&reader.m_dataLock);
                for (uint64_t k : newSet->keys) {
                    auto it = reader.m_frn_to_idx.find(k);
                    SortProxy p; p.key = k;
                    if (it != reader.m_frn_to_idx.end()) {
                        uint32_t idx = it->second;
                        if (column == 0) p.sVal = reinterpret_cast<const char*>(reader.m_string_pool.data() + reader.m_name_offsets[idx]);
                        else if (column == 1) p.sVal = QString::fromStdWString(reader.getPathFast(static_cast<size_t>(reader.m_parent_frns[idx] >> 48), reader.m_frns[idx])).toStdString();
                        else if (column == 2) p.iVal = reader.m_sizes[idx];
                        else if (column == 3) p.iVal = reader.m_timestamps[idx];
                    }
                    proxies.push_back(std::move(p));
                }
            }

            std::sort(proxies.begin(), proxies.end(), [column, order](const SortProxy& a, const SortProxy& b) {
                bool less = (column == 0 || column == 1) ? (_stricmp(a.sVal.c_str(), b.sVal.c_str()) < 0) : (a.iVal < b.iVal);
                return (order == 0) ? less : !less;
            });

            for (size_t i = 0; i < newSet->keys.size(); ++i) newSet->keys[i] = proxies[i].key;
            updateKeyToPosMapping(*newSet);
            return newSet;
        }
        
        // 2026-06-xx 物理修复：如果没有实际变动，必须返回原 snap 副本而非空指针
        // 理由：sortWatcher 的结果会直接替换 m_resultSet，防止 UI 突然清空
        return std::make_shared<ResultSet>(*snap);
    });

    // 2026-06-xx 物理对标：异步重排序时，如果后台任务忙，跳过此批次以实现 Debounce 效果
    m_sortBaseSnap = snap;
    m_sortWatcher.setFuture(future);
}

} // namespace FERREX
