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

struct RenderMeta {
    QColor color;
    explicit RenderMeta(const QColor& c = QColor()) : color(c) {}
};

/**
 * @brief 高性能 SoA 视口滑动投影数据容器
 */
struct ResultSet {
    // 基础索引，全量仅存储 Keys 8-byte 整数
    std::vector<uint64_t> keys;
    std::unordered_map<uint64_t, int> keyToPos;
    std::unordered_map<uint64_t, RenderMeta> metadata;

    // 滑动视口按需 SoA 缓存槽：它的尺寸与 keys 的全量大小一致，
    // 但只有当前处于可见滑动窗口 [VisibleTop - 500, VisibleBottom + 500] 范围内的索引处才拥有有效数据。
    // 其余非视口区域保持默认空状态，零内存动态分配。
    std::vector<QString> cachedNames;  // 只读投影第 0 列名称
    std::vector<QString> cachedPaths;  // 只读投影第 1 列路径
    std::vector<int64_t> cachedSizes;  // 只读投影第 2 列物理大小 (-1 表示未装填)
    std::vector<int64_t> cachedMtimes; // 只读投影第 3 列修改时间 (-1 表示未装填)
    std::vector<bool> isDirFlags;      // 文件夹标识

    mutable std::shared_mutex soaMutex; // 保护 SoA 缓存槽跨线程装配与 UI 同步读写

    void initialize(size_t totalCount) {
        std::unique_lock<std::shared_mutex> lock(soaMutex);
        keys.clear();
        keyToPos.clear();
        metadata.clear();

        // 预分配数组骨架，但此时由于 QString 默认为空，并不发生堆内存实体分配，开销极小
        cachedNames.assign(totalCount, QString());
        cachedPaths.assign(totalCount, QString());
        cachedSizes.assign(totalCount, -1);
        cachedMtimes.assign(totalCount, -1);
        isDirFlags.assign(totalCount, false);
    }

    void initializeSoASparse(size_t totalCount) {
        std::unique_lock<std::shared_mutex> lock(soaMutex);
        cachedNames.assign(totalCount, QString());
        cachedPaths.assign(totalCount, QString());
        cachedSizes.assign(totalCount, -1);
        cachedMtimes.assign(totalCount, -1);
        isDirFlags.assign(totalCount, false);
    }
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
