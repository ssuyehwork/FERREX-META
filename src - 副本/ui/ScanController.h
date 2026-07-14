#pragma once

#include <QObject>
#include <QString>
#include <QColor>
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

namespace FERREX {

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

struct RenderMeta {
    QColor color;
    explicit RenderMeta(const QColor& c = QColor()) : color(c) {}
};

struct ResultSet {
    std::vector<uint64_t> keys;
    std::unordered_map<uint64_t, int> keyToPos;
    std::unordered_map<uint64_t, RenderMeta> metadata;
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

signals:
    void searchStarted();
    void searchFinished(int count, int64_t elapsedMs);

    void resultsSwapped(std::shared_ptr<ResultSet> newSet);

private slots:
    void onMftEntryAdded(uint32_t index);
    void onMftEntryRemoved(uint64_t key);
    void onMftEntryUpdated(uint32_t index);
    void processBatchUpdates();

private:
    void performSearch();
    void updateKeyToPosMapping(ResultSet& rs);

    QString m_searchText;
    ScanFilterState m_filterState;
    int m_currentSortColumn = 0;
    int m_currentSortOrder = 0;

    std::shared_ptr<ResultSet> m_resultSet;
    std::shared_ptr<ResultSet> m_sortBaseSnap; 
    mutable std::mutex m_resultsMutex;
    
    QTimer* m_debounceTimer = nullptr;
    QTimer* m_batchTimer = nullptr;
    
    struct PendingEvent {
        enum Type { Add, Remove, Update } type;
        uint64_t key;
        uint32_t index; 
    };
    std::vector<PendingEvent> m_pendingEvents;
    std::mutex m_pendingMutex;

    QFutureWatcher<std::shared_ptr<ResultSet>> m_watcher;
    QFutureWatcher<std::shared_ptr<ResultSet>> m_sortWatcher;
};

} 
