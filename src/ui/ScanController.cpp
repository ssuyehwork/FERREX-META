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
    connect(&reader, &MftReader::entriesChangedBatch, this, &ScanController::processBatchUpdates);

    connect(&m_sortWatcher, &QFutureWatcher<std::shared_ptr<ResultSet>>::finished, this, [this]() {
        if (m_sortWatcher.isCanceled()) return;
        
        std::shared_ptr<ResultSet> newSet = m_sortWatcher.result();
        if (!newSet || newSet->records.empty()) return;

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
    return static_cast<int>(m_resultSet->records.size());
}

static SearchResultRecord bakeRecord(uint64_t key) {
    auto& reader = MftReader::instance();
    int idx = reader.getIndexByKey(key);
    SearchResultRecord rec;
    rec.key = key;
    if (idx != -1) {
        rec.name = reader.getName(idx);
        rec.path = reader.getFullPath(idx);
        rec.size = reader.getSize(idx);
        rec.mtime = reader.getModifyTime(idx);
        rec.isDirectory = reader.isDirectory(idx);
        rec.extension = reader.getExtQString(idx).toLower();

        auto meta = MetadataManager::instance().getMeta(rec.path.toStdWString());
        if (!meta.color.empty()) {
            rec.color = UiHelper::parseColorName(QString::fromStdWString(meta.color));
        }
    }
    return rec;
}

void ScanController::performSearch() {
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
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_resultSet = newSet;
        }
        emit resultsSwapped(newSet);
        emit searchFinished(0, timer.elapsed());
        return;
    }

    auto future = QtConcurrent::run([this, text, state]() {
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

        int64_t searchMs = subTimer.elapsed();
        auto rs = std::make_shared<ResultSet>();
        
        // 2026-07-22 工业级性能重构：后台预烘焙（Pre-bake）
        // 在后台线程完整填充 SearchResultRecord，消除 UI 线程渲染时的路径回溯与锁竞争
        rs->records.reserve(keys.size());
        for (uint64_t k : keys) {
            rs->records.push_back(bakeRecord(k));
        }
        
        updateKeyToPosMapping(*rs);
        int64_t bakeMs = subTimer.elapsed() - searchMs;

        qInfo() << "[ScanController] 异步搜索完成. 引擎耗时:" << searchMs << "ms, 预烘焙耗时:" << bakeMs << "ms, 结果数:" << rs->records.size();

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
        emit searchFinished(static_cast<int>(m_resultSet->records.size()), timer.elapsed());
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
    
    // 2026-07-22 工业级性能重构：基于预烘焙数据的无锁排序
    auto future = QtConcurrent::run([this, snap, column, order]() {
        auto newSet = std::make_shared<ResultSet>(*snap);
        if (newSet->records.empty()) return newSet;

        std::sort(newSet->records.begin(), newSet->records.end(), [column, order](const SearchResultRecord& a, const SearchResultRecord& b) {
            bool less = false;
            switch (column) {
                case 0: less = a.name.compare(b.name, Qt::CaseInsensitive) < 0; break;
                case 1: less = a.path.compare(b.path, Qt::CaseInsensitive) < 0; break;
                case 2: less = a.size < b.size; break;
                case 3: less = a.mtime < b.mtime; break;
            }
            return (order == 0) ? less : !less;
        });

        updateKeyToPosMapping(*newSet);
        return newSet;
    });

    m_sortBaseSnap = snap;
    m_sortWatcher.setFuture(future);
}

void ScanController::updateKeyToPosMapping(ResultSet& rs) {
    rs.keyToPos.clear();
    rs.keyToPos.reserve(rs.records.size());
    for (size_t i = 0; i < rs.records.size(); ++i) {
        rs.keyToPos[rs.records[i].key] = static_cast<int>(i);
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
                    newSet->records.push_back(bakeRecord(ev.key));
                    changed = true;
                }
            } else if (ev.type == MftReader::ChangeEvent::Removed) {
                if (itPos != snap->keyToPos.end()) {
                    if (!newSet) newSet = std::make_shared<ResultSet>(*snap);
                    newSet->records[itPos->second].key = 0; // 标记删除
                    changed = true;
                }
            } else if (ev.type == MftReader::ChangeEvent::Updated) {
                bool matches = checkMatch(ev.index);
                if (itPos != snap->keyToPos.end()) {
                    if (!matches) {
                        if (!newSet) newSet = std::make_shared<ResultSet>(*snap);
                        newSet->records[itPos->second].key = 0; // 标记失效
                        changed = true;
                    } else {
                        // 更新烘焙数据
                        if (!newSet) newSet = std::make_shared<ResultSet>(*snap);
                        newSet->records[itPos->second] = bakeRecord(ev.key);
                        changed = true;
                    }
                } else if (matches) {
                    if (!newSet) newSet = std::make_shared<ResultSet>(*snap);
                    newSet->records.push_back(bakeRecord(ev.key));
                    changed = true;
                }
            }
        }

        if (changed && newSet) {
            // 清理标记为删除的项目
            newSet->records.erase(std::remove_if(newSet->records.begin(), newSet->records.end(), [](const SearchResultRecord& r){ return r.key == 0; }), newSet->records.end());
            
            // 2026-07-22 工业级性能重构：基于预烘焙数据的增量排序
            std::sort(newSet->records.begin(), newSet->records.end(), [column, order](const SearchResultRecord& a, const SearchResultRecord& b) {
                bool less = false;
                switch (column) {
                    case 0: less = a.name.compare(b.name, Qt::CaseInsensitive) < 0; break;
                    case 1: less = a.path.compare(b.path, Qt::CaseInsensitive) < 0; break;
                    case 2: less = a.size < b.size; break;
                    case 3: less = a.mtime < b.mtime; break;
                }
                return (order == 0) ? less : !less;
            });

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
