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
#include <shared_mutex>
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

/**
 * @brief 稳定的结果集封装 (支持 O(1) 定位)
 */
struct RenderMeta {
    QColor color;
    explicit RenderMeta(const QColor& c = QColor()) : color(c) {}
};

struct ResultSet {
    std::vector<uint64_t> keys;
    std::unordered_map<uint64_t, int> keyToPos;
    std::unordered_map<uint64_t, RenderMeta> metadata;
    
    // 工业级 SoA 数据投影：将路径、大小等在后台线程利用引擎短暂读锁一次性装配完毕
    // 彻底切断主线程 TableModel::data() 运行时对 MftReader 的高开销锁竞争与递归寻址
    std::vector<QString> cachedNames;
    std::vector<QString> cachedPaths;
    std::vector<int64_t> cachedSizes;
    std::vector<int64_t> cachedMtimes;
    std::vector<bool> isDirFlags;
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
    std::shared_ptr<ResultSet> m_sortBaseSnap; // 2026-06-xx 新增：记录重排序任务的基准快照，防止数据过期覆盖
    mutable std::shared_mutex m_resultsMutex;
    
    QTimer* m_debounceTimer = nullptr;
    QTimer* m_batchTimer = nullptr;
    
    struct PendingEvent {
        enum Type { Add, Remove, Update } type;
        uint64_t key;
        uint32_t index; // Only for Add/Update
    };
    std::vector<PendingEvent> m_pendingEvents;
    std::mutex m_pendingMutex;

    std::atomic<uint32_t> m_currentSortId{0}; // 2026-07-xx 新增：排序任务唯一递增版本号

    QFutureWatcher<std::shared_ptr<ResultSet>> m_watcher;
    QFutureWatcher<std::shared_ptr<ResultSet>> m_sortWatcher;
};

} // namespace FERREX
