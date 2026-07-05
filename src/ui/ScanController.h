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

namespace ArcMeta {

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
    
    // 触发搜索（带防抖）
    void triggerSearch(bool immediate = false);

    // 排序接口（异步）
    void sort(int column, int order);

    // 结果访问 (线程安全快照)
    std::shared_ptr<ResultSet> snapshot() const;
    int resultCount() const;

    // 内部比较逻辑 (复用于二分插入与全局排序)
    static bool compareKeys(uint64_t a, uint64_t b, int column, int order);

signals:
    void searchStarted();
    void searchFinished(int count, int64_t elapsedMs);
    
    // 2026-06-xx 响应式信号 (携带原子快照，确保 Model 同步绝对安全)
    void resultsSwapped(std::shared_ptr<ResultSet> newSet);
    void entryAdded(std::shared_ptr<ResultSet> newSet, uint64_t key, int row);
    void entryRemoved(std::shared_ptr<ResultSet> newSet, uint64_t key, int row);
    void entryUpdated(std::shared_ptr<ResultSet> newSet, uint64_t key, int row);

private slots:
    void onMftEntryAdded(uint32_t index);
    void onMftEntryRemoved(uint64_t key);
    void onMftEntryUpdated(uint32_t index);

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
    QFutureWatcher<std::vector<uint64_t>> m_watcher;
    QFutureWatcher<std::vector<uint64_t>> m_sortWatcher;
};

} // namespace ArcMeta
