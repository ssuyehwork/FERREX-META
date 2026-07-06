#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QTimer>
#include <QFutureWatcher>
#include <vector>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

namespace FERREX-META {

struct ScanFilterState {
    QStringList extensionList; 
    bool useRegex = false;
    bool caseSensitive = false;
    bool includeHidden = true;
    bool includeSystem = true;
    bool includeDollar = true;
    bool autoDisplay = false;

    bool isEmpty() const { 
        return extensionList.isEmpty() && !useRegex && !caseSensitive && includeHidden && includeSystem && includeDollar && !autoDisplay; 
    }
};

/**
 * @brief 稳定的结果集封装 (支持 O(1) 定位)
 */
struct ResultSet {
    std::vector<uint64_t> keys;
    std::unordered_map<uint64_t, int> keyToPos;
};

class ScanController : public QObject {
    Q_OBJECT
public:
    explicit ScanController(QObject* parent = nullptr);
    ~ScanController() override;

    void setSearchText(const QString& text);
    void setFilterState(const ScanFilterState& state);
    
    void triggerSearch(bool immediate = false);
    void sort(int column, int order);

    std::shared_ptr<ResultSet> snapshot() const;
    int resultCount() const;

    static bool compareKeys(uint64_t a, uint64_t b, int column, int order);
    static bool compareKeysNoLock(uint64_t a, uint64_t b, int column, int order);

signals:
    void searchStarted();
    void searchFinished(int count, int64_t elapsedMs);
    
    void resultsSwapped(std::shared_ptr<ResultSet> newSet);
    void entryAdded(std::shared_ptr<ResultSet> newSet, uint64_t key, int row);
    void entryRemoved(std::shared_ptr<ResultSet> newSet, uint64_t key, int row);
    void entryUpdated(std::shared_ptr<ResultSet> newSet, uint64_t key, int row);

private slots:
    void processPendingIncrementalUpdates();
    void onMftChangesApplied(const std::vector<MftReader::Change>& changes);

private:
    void performSearch();
    void updateKeyToPosMapping(ResultSet& rs);

    QString m_searchText;
    ScanFilterState m_filterState;
    int m_currentSortColumn = 0;
    int m_currentSortOrder = 0;

    std::shared_ptr<ResultSet> m_resultSet;
    mutable std::mutex m_resultsMutex;
    
    QTimer* m_debounceTimer = nullptr;
    QTimer* m_incrementalTimer = nullptr;
    
    struct PendingUpdate {
        enum Type { Added, Removed, Updated } type;
        uint64_t key;
        uint32_t mftIndex;
    };
    std::vector<PendingUpdate> m_pendingUpdates;
    std::mutex m_pendingMutex;

    QFutureWatcher<std::vector<uint64_t>> m_watcher;
    QFutureWatcher<std::vector<uint64_t>> m_sortWatcher;

    struct IncrementalResult {
        std::shared_ptr<ResultSet> newSet;
        struct Diff { enum Type { Add, Rem, Upd } type; uint64_t key; int row; };
        std::vector<Diff> diffs;
    };
    QFutureWatcher<IncrementalResult> m_incrementalWatcher;
};

} // namespace FERREX-META
