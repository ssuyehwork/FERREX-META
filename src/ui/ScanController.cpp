#include "ScanController.h"
#include "../mft/MftReader.h"
#include <QtConcurrent/QtConcurrent>
#include <QElapsedTimer>

namespace ArcMeta {

ScanController::ScanController(QObject* parent) : QObject(parent) {
    m_resultSet = std::make_shared<ResultSet>();
    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(300);

    m_incrementalTimer = new QTimer(this);
    m_incrementalTimer->setInterval(150);

    connect(m_debounceTimer, &QTimer::timeout, this, [this]() {
        performSearch();
    });
    connect(m_incrementalTimer, &QTimer::timeout, this, &ScanController::processPendingIncrementalUpdates);

    auto& reader = MftReader::instance();
    connect(&reader, &MftReader::changesApplied, this, &ScanController::onMftChangesApplied);

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

    connect(&m_incrementalWatcher, &QFutureWatcher<IncrementalResult>::finished, this, [this]() {
        if (m_incrementalWatcher.isCanceled()) return;
        IncrementalResult res = m_incrementalWatcher.result();
        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);
            m_resultSet = res.newSet;
        }

        // Plan-Smooth: 如果变动量过大（>50），全量刷新视图比发送成千上万个信号要快得多且不卡顿
        if (res.diffs.size() > 50) {
            emit resultsSwapped(res.newSet);
        } else {
            for (const auto& d : res.diffs) {
                if (d.type == IncrementalResult::Diff::Add) emit entryAdded(res.newSet, d.key, d.row);
                else if (d.type == IncrementalResult::Diff::Rem) emit entryRemoved(res.newSet, d.key, d.row);
                else if (d.type == IncrementalResult::Diff::Upd) emit entryUpdated(res.newSet, d.key, d.row);
            }
        }
    });
}

ScanController::~ScanController() {
    m_watcher.cancel(); m_watcher.waitForFinished();
    m_sortWatcher.cancel(); m_sortWatcher.waitForFinished();
    m_incrementalWatcher.cancel(); m_incrementalWatcher.waitForFinished();
}

void ScanController::setSearchText(const QString& text) {
    if (m_searchText == text) return;
    m_searchText = text;
}

void ScanController::setFilterState(const ScanFilterState& state) {
    m_filterState = state;
}

void ScanController::triggerSearch(bool immediate) {
    if (immediate) { m_debounceTimer->stop(); performSearch(); }
    else m_debounceTimer->start();
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
    if (m_incrementalWatcher.isRunning()) m_incrementalWatcher.cancel();

    emit searchStarted();
    QElapsedTimer timer; timer.start();

    auto future = QtConcurrent::run([text = m_searchText, state = m_filterState]() {
        if (text.isEmpty() && state.extensionList.isEmpty() && !state.autoDisplay) return std::vector<uint64_t>();
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
    QReadLocker lock(&MftReader::instance().m_dataLock);
    return compareKeysNoLock(a, b, column, order);
}

bool ScanController::compareKeysNoLock(uint64_t a, uint64_t b, int column, int order) {
    auto& reader = MftReader::instance();
    int idxA = reader.getIndexByKeyNoLock(a);
    int idxB = reader.getIndexByKeyNoLock(b);
    if (idxA == -1 || idxB == -1) return false;
    bool less = false;
    switch (column) {
        case 0: less = QString::compare(reader.getNameNoLock(idxA), reader.getNameNoLock(idxB), Qt::CaseInsensitive) < 0; break;
        case 1: less = QString::compare(reader.getFullPath(idxA), reader.getFullPath(idxB), Qt::CaseInsensitive) < 0; break;
        case 2: less = reader.getSizeNoLock(idxA) < reader.getSizeNoLock(idxB); break;
        case 3: less = reader.getModifyTimeNoLock(idxA) < reader.getModifyTimeNoLock(idxB); break;
        default: return false;
    }
    return (order == 0) ? less : !less;
}

void ScanController::sort(int column, int order) {
    m_currentSortColumn = column; m_currentSortOrder = order;
    if (m_sortWatcher.isRunning()) m_sortWatcher.cancel();
    std::shared_ptr<ResultSet> currentSet = snapshot();

    auto future = QtConcurrent::run([this, column, order, currentSet]() mutable {
        std::vector<uint64_t> keys = currentSet->keys;
        if (keys.empty()) return keys;

        // Plan-Smooth: 极致性能优化。
        // 不在 std::sort 内部加锁，更不在内部调用 getName/getFullPath (创建 QString)。
        // 我们预先提取排序列的原始 POD 数据或指针。

        auto& reader = MftReader::instance();

        if (column == 2 || column == 3) { // Size 或 Time (Int64 POD)
            struct SortItem { int64_t val; uint64_t key; };
            std::vector<SortItem> items;
            items.reserve(keys.size());

            {
                QReadLocker lock(&reader.m_dataLock);
                for (uint64_t k : keys) {
                    int idx = reader.getIndexByKeyNoLock(k);
                    if (idx != -1) {
                        int64_t v = (column == 2) ? reader.getSizeNoLock(idx) : reader.getModifyTimeNoLock(idx);
                        items.push_back({ v, k });
                    }
                }
            }

            std::sort(items.begin(), items.end(), [order](const SortItem& a, const SortItem& b) {
                return (order == 0) ? (a.val < b.val) : (a.val > b.val);
            });

            std::vector<uint64_t> sortedKeys; sortedKeys.reserve(items.size());
            for (const auto& it : items) sortedKeys.push_back(it.key);
            return sortedKeys;
        } else {
            // String 排序 (Name 或 Path) - 依然很重，但我们要避免在循环中创建 QString
            // 这里使用 std::string_view 或原始指针
            struct SortItem { std::string val; uint64_t key; };
            std::vector<SortItem> items;
            items.reserve(keys.size());

            {
                QReadLocker lock(&reader.m_dataLock);
                for (uint64_t k : keys) {
                    int idx = reader.getIndexByKeyNoLock(k);
                    if (idx != -1) {
                        // getNameNoLock 返回 QString，我们转为 std::string 以便脱离锁进行比较
                        // getFullPath 比较特殊，它需要递归，我们必须在锁内完成
                        if (column == 0) items.push_back({ reader.getNameNoLock(idx).toStdString(), k });
                        else items.push_back({ QString::fromStdWString(reader.getPathFast(static_cast<size_t>(reader.getKeyByIndexNoLock(idx) >> 48), reader.getFrnNoLock(idx))).toStdString(), k });
                    }
                }
            }

            // 此时已释放 m_dataLock，std::sort 不再阻塞 USN 线程
            std::sort(items.begin(), items.end(), [order](const SortItem& a, const SortItem& b) {
                int cmp = _stricmp(a.val.c_str(), b.val.c_str());
                return (order == 0) ? (cmp < 0) : (cmp > 0);
            });

            std::vector<uint64_t> sortedKeys; sortedKeys.reserve(items.size());
            for (const auto& it : items) sortedKeys.push_back(it.key);
            return sortedKeys;
        }
    });
    m_sortWatcher.setFuture(future);
}

void ScanController::updateKeyToPosMapping(ResultSet& rs) {
    rs.keyToPos.clear(); rs.keyToPos.reserve(rs.keys.size());
    for (size_t i = 0; i < rs.keys.size(); ++i) rs.keyToPos[rs.keys[i]] = static_cast<int>(i);
}

void ScanController::onMftChangesApplied(const std::vector<MftReader::Change>& changes) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (const auto& c : changes) {
        PendingUpdate up;
        if (c.type == MftReader::Change::Add) up.type = PendingUpdate::Added;
        else if (c.type == MftReader::Change::Rem) up.type = PendingUpdate::Removed;
        else up.type = PendingUpdate::Updated;
        up.key = c.key;
        up.mftIndex = c.index;
        m_pendingUpdates.push_back(up);
    }
    if (!m_incrementalTimer->isActive()) m_incrementalTimer->start();
}

void ScanController::processPendingIncrementalUpdates() {
    if (m_incrementalWatcher.isRunning()) return; // 上一次还在跑，等下一次
    std::vector<PendingUpdate> updates;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        updates = std::move(m_pendingUpdates);
        m_pendingUpdates.clear();
        m_incrementalTimer->stop();
    }
    if (updates.empty()) return;

    std::shared_ptr<ResultSet> currentSet;
    { std::lock_guard<std::mutex> lock(m_resultsMutex); currentSet = m_resultSet; }

    auto future = QtConcurrent::run([this, updates, currentSet, text = m_searchText, state = m_filterState, col = m_currentSortColumn, ord = m_currentSortOrder]() {
        IncrementalResult res;
        res.newSet = std::make_shared<ResultSet>(*currentSet);

        QRegularExpression re;
        if (state.useRegex && !text.isEmpty()) re = QRegularExpression(text, state.caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        std::vector<std::string> el;
        for (const auto& e : state.extensionList) el.push_back((e.startsWith('.') ? e : "." + e).toLower().toStdString());

        // 在后台线程持有读锁进行批量 matchEntry
        QReadLocker lock(&MftReader::instance().m_dataLock);

        // Plan-Smooth: 策略降级。如果当前结果集很大且增量也多，逐条执行 O(N) 的删除/插入会导致后台线程处理变慢。
        // 若增量 > 100 且 结果集 > 50000，直接全量重新过滤生成新结果集并宣告 Swapped。
        if (updates.size() > 100 && res.newSet->keys.size() > 50000) {
            std::vector<uint64_t> allMatched = MftReader::instance().search(text, state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
            // 这里我们只需要简单的 resultsSwapped 效果
            res.newSet->keys = std::move(allMatched);
            std::sort(res.newSet->keys.begin(), res.newSet->keys.end(), [col, ord](uint64_t a, uint64_t b) { return compareKeys(a, b, col, ord); });
            updateKeyToPosMapping(*res.newSet);
            // 通过填充大量 diff 触发前端 Swapped 逻辑
            for (int k=0; k<100; ++k) res.diffs.push_back({ IncrementalResult::Diff::Rem, 0, 0 });
            return res;
        }

        bool posMappingDirty = false;

        std::unordered_set<uint64_t> keysToRemove;
        struct PendingAdd { uint64_t key; };
        std::vector<uint64_t> keysToAdd;

        for (size_t i = 0; i < updates.size(); ++i) {
            if (i > 0 && i % 500 == 0) { lock.unlock(); lock.relock(); }

            const auto& up = updates[i];
            if (up.type == PendingUpdate::Removed) {
                keysToRemove.insert(up.key);
            } else {
                bool matches = MftReader::instance().matchEntryOptimized((int)up.mftIndex, text, re, state.useRegex, state.caseSensitive, el, state.includeHidden, state.includeSystem, state.includeDollar);
                if (text.isEmpty() && state.extensionList.isEmpty()) matches = state.autoDisplay && matches;

                auto itPos = res.newSet->keyToPos.find(up.key);
                if (itPos != res.newSet->keyToPos.end()) {
                    if (!matches) keysToRemove.insert(up.key);
                    else res.diffs.push_back({ IncrementalResult::Diff::Upd, up.key, itPos->second });
                } else if (matches) {
                    keysToAdd.push_back(up.key);
                }
            }
        }

        // Plan-Smooth: 统一执行 O(N) 变更，避免在循环内多次执行 vector 移动
        if (!keysToRemove.empty() || !keysToAdd.empty()) {
            std::vector<uint64_t> nextKeys;
            nextKeys.reserve(res.newSet->keys.size() + keysToAdd.size());
            for (uint64_t k : res.newSet->keys) {
                if (keysToRemove.find(k) == keysToRemove.end()) nextKeys.push_back(k);
            }
            for (uint64_t k : keysToAdd) nextKeys.push_back(k);

            std::sort(nextKeys.begin(), nextKeys.end(), [col, ord](uint64_t a, uint64_t b) {
                return compareKeysNoLock(a, b, col, ord);
            });

            res.newSet->keys = std::move(nextKeys);
            updateKeyToPosMapping(*res.newSet);
            // 批量变更后，通知前端执行全量刷新 (通过模拟大量 diff)
            for (int k=0; k<100; ++k) res.diffs.push_back({ IncrementalResult::Diff::Rem, 0, 0 });
        }
        if (posMappingDirty) updateKeyToPosMapping(*res.newSet);
        return res;
    });
    m_incrementalWatcher.setFuture(future);
}

} // namespace ArcMeta
