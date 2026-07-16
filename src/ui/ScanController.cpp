#include "ScanController.h"
#include "../mft/MftReader.h"
#include "../meta/MetadataManager.h"
#include "UiHelper.h"
#include <QtConcurrent/QtConcurrent>
#include <QElapsedTimer>
#include <QDebug>

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
    connect(&reader, &MftReader::entriesChangedBatch, this, [this]() {
        if (!m_batchTimer->isActive()) {
            m_batchTimer->start();
        }
    });

    connect(&m_sortWatcher, &QFutureWatcher<std::shared_ptr<ResultSet>>::finished, this, [this]() {
        if (m_sortWatcher.isCanceled()) return;
        
        std::shared_ptr<ResultSet> newSet = m_sortWatcher.result();
        if (!newSet || newSet->keys.empty()) return;

        {
            std::unique_lock<std::shared_mutex> lock(m_resultsMutex);
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
    std::shared_lock<std::shared_mutex> lock(m_resultsMutex);
    return m_resultSet;
}

int ScanController::resultCount() const {
    std::shared_lock<std::shared_mutex> lock(m_resultsMutex);
    return static_cast<int>(m_resultSet->keys.size());
}

void ScanController::performSearch() {
    // 还原旧版本-3：彻底剥离多媒体异步过滤（还原设计一），使其总是返回纯粹的全量数据
    // [已物理移除] performSearch 中的 if (state.galleryOnly) { ... } 过滤数据块，恢复数据库检索只进行基本的过滤（不再参与自适应或网格的媒体剪裁）
    MftReader::instance().setSearchCanceled(true); // 物理通知底层搜寻终止
    ++m_currentSortId; // 瞬间使之前所有未运行完的异步排序任务和批处理增量失效并快速退出

    if (m_watcher.isRunning()) {
        m_watcher.cancel();
        qInfo() << "[ScanController] 取消正在运行的搜索任务";
    }
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
            std::unique_lock<std::shared_mutex> lock(m_resultsMutex);
            m_resultSet = newSet;
        }
        emit resultsSwapped(newSet);
        emit searchFinished(0, timer.elapsed());
        return;
    }

    auto future = QtConcurrent::run([this, text, state]() {
        // 在新搜索后台线程体启动之初，重置取消状态为 false。因为此时上一个任务必然已经响应了取消或者已被 cancel 中断
        MftReader::instance().setSearchCanceled(false);

        QElapsedTimer subTimer;
        subTimer.start();
        
        std::vector<uint64_t> keys;
        // 如果开启自动显示且查询为空，则执行全量搜索（带过滤）
        if (state.autoDisplay && text.isEmpty() && state.extensionList.isEmpty()) {
            keys = MftReader::instance().search("", state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
        }
        else {
            keys = MftReader::instance().search(text, state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
        }

        if (MftReader::instance().isSearchCanceled()) {
            return std::shared_ptr<ResultSet>(nullptr);
        }

        int64_t searchMs = subTimer.elapsed();
        auto rs = std::make_shared<ResultSet>();
        rs->keys = std::move(keys);
        updateKeyToPosMapping(*rs);

        // [性能重构方案物理对齐]：彻底解耦筛选与显示渲染。后台搜索线程不应该去高开销获取元数据，
        // 否则会频繁霸占 MftReader.m_dataLock 锁，造成前台 UI 在 data() 渲染时由于锁等待而卡死。
        // 将后台元数据装饰块完全移除，回归极速过滤模式。
        qInfo() << "[ScanController] 异步搜索完成. 引擎耗时:" << searchMs << "ms, 结果数:" << rs->keys.size();

        return rs;
    });

    disconnect(&m_watcher, &QFutureWatcher<std::shared_ptr<ResultSet>>::finished, this, nullptr);
    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<ResultSet>>::finished, this, [this, timer]() {
        if (m_watcher.isCanceled()) return;
        
        std::shared_ptr<ResultSet> newSet = m_watcher.result();
        if (!newSet) return; // 拦截空指针，绝对不覆盖当前界面的搜索数据

        {
            std::unique_lock<std::shared_mutex> lock(m_resultsMutex);
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
    uint32_t mySortId = ++m_currentSortId; // 分配当前任务的唯一排序递增版本
    
    // 2026-06-xx 极致架构优化：去锁化投影排序。
    // 理由：通过物理拷贝文件名/数值至投影结构，彻底杜绝排序过程中的锁竞争与野指针风险。
    auto future = QtConcurrent::run([this, snap, column, order, mySortId]() {
        auto newSet = std::make_shared<ResultSet>();
        newSet->keys = snap->keys;
        if (newSet->keys.empty()) return newSet;

        auto& reader = MftReader::instance();

        // 1. 投影准备阶段：申请极其短暂的读锁，直接物理拷贝 SoA 投影
        // 理由：彻底避免在整个 getPathFast/排序 循环中持续霸占读锁，防止 GUI 线程因写锁排队而被挂起。
        std::unordered_map<uint64_t, uint32_t> local_frn_to_idx;
        std::vector<uint32_t> local_name_offsets;
        std::vector<uint8_t> local_string_pool;
        std::vector<uint64_t> local_parent_frns;
        std::vector<uint64_t> local_frns;
        std::vector<uint32_t> local_parent_indices;
        std::vector<int64_t> local_sizes;
        std::vector<int64_t> local_timestamps;

        if (mySortId != m_currentSortId.load(std::memory_order_relaxed)) return std::shared_ptr<ResultSet>(nullptr);

        {
            QReadLocker lock(&reader.m_dataLock);
            local_frn_to_idx = reader.m_frn_to_idx;
            if (column == 0 || column == 1) {
                local_name_offsets = reader.m_name_offsets;
                local_string_pool = reader.m_string_pool;
            }
            if (column == 1) {
                local_parent_frns = reader.m_parent_frns;
                local_frns = reader.m_frns;
                local_parent_indices = reader.m_parent_indices;
            }
            if (column == 2) {
                local_sizes = reader.m_sizes;
            }
            if (column == 3) {
                local_timestamps = reader.m_timestamps;
            }
        }

        auto getPathFastLocal = [&](size_t driveIdx, uint64_t frn) -> std::wstring {
            uint64_t compositeKey = (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);
            auto idxIt = local_frn_to_idx.find(compositeKey);
            if (idxIt == local_frn_to_idx.end()) return L"";

            uint32_t curIdx = idxIt->second;
            std::vector<std::wstring> segments;
            int depth = 0;
            while (curIdx != 0xFFFFFFFF && depth < 64) {
                if (curIdx >= local_name_offsets.size()) break;
                const char* p = reinterpret_cast<const char*>(local_string_pool.data() + local_name_offsets[curIdx]);
                segments.push_back(QString::fromUtf8(p).toStdWString());

                if (curIdx >= local_parent_frns.size()) break;
                uint64_t parentFrn = local_parent_frns[curIdx] & 0x0000FFFFFFFFFFFFull;
                if (parentFrn == 5 || parentFrn == 0) break;

                if (curIdx >= local_parent_indices.size()) break;
                curIdx = local_parent_indices[curIdx];
                depth++;
            }

            if (segments.empty()) return L"";
            std::wstring fullPath;
            for (auto rit = segments.rbegin(); rit != segments.rend(); ++rit) {
                if (!fullPath.empty()) fullPath += L"\\";
                fullPath += *rit;
            }
            return fullPath;
        };

        std::vector<SortProxy> proxies;
        proxies.reserve(newSet->keys.size());

        size_t keyIndex = 0;
        for (uint64_t k : newSet->keys) {
            if ((keyIndex++ & 4095) == 0 && mySortId != m_currentSortId.load(std::memory_order_relaxed)) {
                return std::shared_ptr<ResultSet>(nullptr);
            }

            auto it = local_frn_to_idx.find(k);
            SortProxy p; p.key = k;
            if (it != local_frn_to_idx.end()) {
                uint32_t idx = it->second;
                if (column == 0) {
                    if (idx < local_name_offsets.size()) {
                        p.sVal = reinterpret_cast<const char*>(local_string_pool.data() + local_name_offsets[idx]);
                    }
                }
                else if (column == 1) {
                    if (idx < local_parent_frns.size() && idx < local_frns.size()) {
                        p.sVal = QString::fromStdWString(getPathFastLocal(static_cast<size_t>(local_parent_frns[idx] >> 48), local_frns[idx])).toStdString();
                    }
                }
                else if (column == 2) {
                    if (idx < local_sizes.size()) {
                        p.iVal = local_sizes[idx];
                    }
                }
                else if (column == 3) {
                    if (idx < local_timestamps.size()) {
                        p.iVal = local_timestamps[idx];
                    }
                }
            }
            proxies.push_back(std::move(p));
        }

        if (mySortId != m_currentSortId.load(std::memory_order_relaxed)) return std::shared_ptr<ResultSet>(nullptr);

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

        if (mySortId != m_currentSortId.load(std::memory_order_relaxed)) return std::shared_ptr<ResultSet>(nullptr);

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

    // [性能重构方案物理对齐]：当积压的 USN 事件数超过 2000 时，不进行繁重的后台增量排序，直接降级进行快速的全量异步重索，彻底规避锁冲突
    if (static_cast<int>(events.size()) > 2000) {
        qDebug() << "[ScanController] 积压事件数超过2000阈值 (" << events.size() << ")，快速降级至全量异步重新检索";
        triggerSearch(true);
        return;
    }

    std::shared_ptr<ResultSet> snap = snapshot();
    uint32_t mySortId = ++m_currentSortId; // 分配当前任务的唯一排序递增版本
    auto future = QtConcurrent::run([this, snap, events, text = m_searchText, state = m_filterState, 
                                     column = m_currentSortColumn, order = m_currentSortOrder, mySortId]() {
        // 2026-06-xx 极致性能优化：延迟拷贝。
        // 理由：直接对 snap 进行 * 解引用拷贝会克隆整个 unordered_map (200万项)，
        // 这在 UI 线程频繁触发时会导致严重的亚秒级停顿（假死）。
        std::shared_ptr<ResultSet> newSet;
        bool changed = false;

        auto& reader = MftReader::instance();
        size_t eventIndex = 0;
        for (const auto& ev : events) {
            if ((eventIndex++ & 4095) == 0 && mySortId != m_currentSortId.load(std::memory_order_relaxed)) {
                return std::shared_ptr<ResultSet>(nullptr);
            }

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

        if (mySortId != m_currentSortId.load(std::memory_order_relaxed)) return std::shared_ptr<ResultSet>(nullptr);

        if (changed && newSet) {
            newSet->keys.erase(std::remove(newSet->keys.begin(), newSet->keys.end(), 0), newSet->keys.end());
            
            // 执行后台安全增量重排序 (因为事件积压限制在 2000 个以内，故不拷贝千万级巨型 SoA Map，直接在后台线程下通过短时读锁检索)
            std::vector<SortProxy> proxies;
            proxies.reserve(newSet->keys.size());
            {
                QReadLocker lock(&reader.m_dataLock);
                size_t kIndex = 0;
                for (uint64_t k : newSet->keys) {
                    if ((kIndex++ & 4095) == 0 && mySortId != m_currentSortId.load(std::memory_order_relaxed)) {
                        return std::shared_ptr<ResultSet>(nullptr);
                    }

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

            if (mySortId != m_currentSortId.load(std::memory_order_relaxed)) return std::shared_ptr<ResultSet>(nullptr);

            std::sort(proxies.begin(), proxies.end(), [column, order](const SortProxy& a, const SortProxy& b) {
                bool less = (column == 0 || column == 1) ? (_stricmp(a.sVal.c_str(), b.sVal.c_str()) < 0) : (a.iVal < b.iVal);
                return (order == 0) ? less : !less;
            });

            if (mySortId != m_currentSortId.load(std::memory_order_relaxed)) return std::shared_ptr<ResultSet>(nullptr);

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
