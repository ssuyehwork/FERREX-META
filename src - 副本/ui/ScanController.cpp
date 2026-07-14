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
    m_debounceTimer->setInterval(300); 

    connect(m_debounceTimer, &QTimer::timeout, this, [this]() {
        performSearch();
    });

    m_batchTimer = new QTimer(this);
    m_batchTimer->setSingleShot(true);
    m_batchTimer->setInterval(200); 
    connect(m_batchTimer, &QTimer::timeout, this, &ScanController::processBatchUpdates);

    auto& reader = MftReader::instance();
    connect(&reader, &MftReader::entriesChangedBatch, this, &ScanController::processBatchUpdates);

    connect(&m_sortWatcher, &QFutureWatcher<std::shared_ptr<ResultSet>>::finished, this, [this]() {
        if (m_sortWatcher.isCanceled()) return;
        
        std::shared_ptr<ResultSet> newSet = m_sortWatcher.result();
        if (!newSet || newSet->keys.empty()) return;

        {
            std::lock_guard<std::mutex> lock(m_resultsMutex);

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

    if (m_watcher.isRunning()) {
        m_watcher.cancel();
        qInfo() << "[ScanController] 取消正在运行的搜索任务";
    }
    if (m_sortWatcher.isRunning()) m_sortWatcher.cancel();

    emit searchStarted();
    
    QElapsedTimer timer;
    timer.start();

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
        
        if (state.autoDisplay && text.isEmpty() && state.extensionList.isEmpty()) {
            keys = MftReader::instance().search("", state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
        }
        else {
            keys = MftReader::instance().search(text, state.useRegex, state.caseSensitive, state.extensionList, state.includeHidden, state.includeSystem, state.includeDollar);
        }

        int64_t searchMs = subTimer.elapsed();
        auto rs = std::make_shared<ResultSet>();
        rs->keys = std::move(keys);
        updateKeyToPosMapping(*rs);

        
        subTimer.restart();
        size_t decorCount = std::min(rs->keys.size(), static_cast<size_t>(2000)); 
        for (size_t i = 0; i < decorCount; ++i) {
            uint64_t k = rs->keys[i];
            auto& reader = MftReader::instance();
            int idx = reader.getIndexByKey(k);
            if (idx == -1) continue;
            QString path = reader.getFullPath(idx);
            if (path.isEmpty()) continue;

            auto meta = MetadataManager::instance().getMeta(path.toStdWString());
            if (!meta.color.empty()) {
                QColor c = UiHelper::parseColorName(QString::fromStdWString(meta.color));
                if (c.isValid()) rs->metadata[k] = RenderMeta(c);
            }
        }
        int64_t decorMs = subTimer.elapsed();

        qInfo() << "[ScanController] 异步搜索完成. 引擎耗时:" << searchMs << "ms, 元数据装饰耗时:" << decorMs << "ms, 结果数:" << rs->keys.size();

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

struct SortProxy {
    uint64_t key;
    int64_t iVal = 0;
    std::string sVal;
};

bool ScanController::compareKeys(uint64_t a, uint64_t b, int column, int order) {
    
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
    return (order == 0 ) ? less : !less;
}

void ScanController::sort(int column, int order) {
    m_currentSortColumn = column;
    m_currentSortOrder = order;

    if (m_sortWatcher.isRunning()) m_sortWatcher.cancel();

    std::shared_ptr<ResultSet> snap = snapshot();

    
    auto future = QtConcurrent::run([this, snap, column, order]() {
        auto newSet = std::make_shared<ResultSet>();
        newSet->keys = snap->keys;
        if (newSet->keys.empty()) return newSet;

        auto& reader = MftReader::instance();
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
                    else if (column == 1) {
                        
                        p.sVal = QString::fromStdWString(reader.getPathFast(static_cast<size_t>(reader.m_parent_frns[idx] >> 48), reader.m_frns[idx])).toStdString();
                    }
                    else if (column == 2) p.iVal = reader.m_sizes[idx];
                    else if (column == 3) p.iVal = reader.m_timestamps[idx];
                }
                proxies.push_back(std::move(p));
            }
        }

        std::sort(proxies.begin(), proxies.end(), [column, order](const SortProxy& a, const SortProxy& b) {
            bool less = false;
            if (column == 0 || column == 1) {
                less = _stricmp(a.sVal.c_str(), b.sVal.c_str()) < 0;
            } else {
                less = a.iVal < b.iVal;
            }
            return (order == 0) ? less : !less;
        });

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

    if (m_sortWatcher.isRunning()) {
        
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

        
        std::shared_ptr<ResultSet> newSet;
        bool changed = false;

        auto& reader = MftReader::instance();
        for (const auto& ev : events) {
            
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

        
        return std::make_shared<ResultSet>(*snap);
    });

    m_sortBaseSnap = snap;
    m_sortWatcher.setFuture(future);
}

} 
