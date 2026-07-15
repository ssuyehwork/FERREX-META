#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QReadWriteLock>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <windows.h>
#include <winioctl.h>
#include <QIcon>
#include <QHash>
#include <QThreadPool>
#include <QTimer>
#include "ScchCache.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef run
#undef run
#endif


namespace FERREX {

class UsnWatcher;

/**
 * @brief 高性能 MFT 索引引擎 (SoA 架构)
 */
class NtfsVolumeMftParser;
class UsnJournalTreeSynchronizer;
class DiskIndexCacheCoordinator;
class MemoryQueryEngine;

class MftReader : public QObject {
    Q_OBJECT
    friend class ScanController; // 2026-06-xx 极致架构：允许控制器直接访问 SoA，消除锁竞争开销
    friend class NtfsVolumeMftParser;
    friend class UsnJournalTreeSynchronizer;
    friend class DiskIndexCacheCoordinator;
    friend class MemoryQueryEngine;
public:
    static MftReader& instance();

    // 全局图标缓存管理 (解决 UAF 风险)
    QIcon getCachedIcon(const QString& ext, bool isDir);

signals:
    void dataChanged(int index = -1);
    void entriesChangedBatch();        // 2026-06-xx 新增：批量变动信号，解决信号风暴
    void driveLoaded(const QString& drive, int count, int total); // 2026-05-14 新增：驱动器就绪信号

public:
    // 生命周期管理
    void buildIndex(const QStringList& drives = QStringList());
    bool loadFromCache(); // 默认全量加载
    bool loadDriveFromCache(const QString& drive); // 按需加载单个盘符
    void unloadDrive(const QString& drive);        // 动态卸载单个盘符
    bool saveToCache(); 
    bool saveDriveToCache(size_t driveIdx); 
    void clear();

    // 驱动器隔离状态管理
    void updateActiveDrives(const QStringList& activeDrives);
    bool isDriveIndexed(const QString& drive);

    // 查询接口 (支持驱动器掩码隔离)
    // 2026-06-xx 物理重构：返回稳定的复合 FRN 主键而非数组下标，杜绝跨线程索引漂移
    std::vector<uint64_t> search(const QString& query, bool useRegex = false, bool caseSensitive = false, 
                                 const QStringList& extensionList = QStringList(), 
                                 bool includeHidden = true, bool includeSystem = true,
                                 bool includeDollar = true);
    
    // SoA 访问接口
    bool     matchEntry(int index, const QString& query, bool useRegex, bool caseSensitive, 
                        const QStringList& extensionList, bool includeHidden, bool includeSystem,
                        bool includeDollar = true) const;
    int      getIndexByKey(uint64_t compositeKey) const;
    uint64_t getKeyByIndex(int index) const;
    QString  getName(int index) const;
    const char* getExt(int index) const; // 2026-06-xx 新增：获取预拆分的扩展名 C-String
    QString     getExtQString(int index) const; // 2026-06-xx 新增：获取预拆分的扩展名 QString (线程安全)
    int64_t getSize(int index) const;
    int64_t getModifyTime(int index) const;
    uint32_t getAttributes(int index) const;
    uint64_t getFrn(int index) const;
    bool isDirectory(int index) const;
    int totalCount() const;            // 代表内存中加载的所有条目总数（用于内存计算）
    int activeCount() const;           // 新增：仅返回当前处于激活（勾选）状态的盘符的文件总数
    QString getFullPath(int index) const;
    void requestMetadata(int index);
    bool isMetadataFetched(int index) const;

    // 辅助工具：生成复合主键
    static inline uint64_t makeKey(size_t driveIdx, uint64_t frn) {
        return (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);
    }

    // USN 更新
    void updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume);
    void removeEntryByFrn(const std::wstring& volume, uint64_t frn);

    // 搜索原子取消管理
    inline bool isSearchCanceled() const { return m_searchCancelRequested.load(std::memory_order_relaxed); }
    inline void setSearchCanceled(bool canceled) { m_searchCancelRequested.store(canceled, std::memory_order_relaxed); }

    // 变动日志访问 (供 Controller 批量拉取)
    struct ChangeEvent {
        enum Type { Added, Removed, Updated } type;
        uint64_t key;
        uint32_t index;
    };
    std::vector<ChangeEvent> pullChangeJournal();
    std::wstring getPathFast(size_t driveIdx, uint64_t frn);

private:
    MftReader();
    ~MftReader();

    // 内部结构体
    struct RawEntry {
        uint64_t frn;
        uint64_t parentFrn;
        uint64_t size; // 2026-05-14 补全：文件大小字段
        uint32_t attributes;
        int64_t  modifyTime;
        uint32_t nameOffset; // 2026-06-xx 极致优化：使用偏移量替代 std::string，实现零分配扫描
    };
    struct DriveResult {
        std::vector<RawEntry> entries;
        std::vector<uint8_t>  string_pool; // 2026-06-xx 极致优化：本地字符串池
        uint64_t nextUsn;
    };

    bool saveDriveToCacheInternal(size_t driveIdx); 
    bool saveDriveToCacheUnlocked(size_t driveIdx); // 2026-06-xx 新增：不带锁的落盘辅助函数，用于 buildIndex
    void clearInternal(); 
    void rebuildFrnToIndexMap();
    void compact(bool force = false);
    void buildSortedIndices();
    
    bool loadMftDirect(const std::wstring& volume, DriveResult& result);
    void mergeDriveResult(const std::wstring& volume, const DriveResult& result, size_t driveIdx);

    // SoA 主数据
    std::vector<uint64_t>  m_frns;
    std::vector<uint64_t>  m_parent_frns;  // 高 16 位存储盘符索引
    std::vector<uint32_t>  m_parent_indices; // 2026-06-xx 新增：父节点在 SoA 中的下标，加速路径回溯
    std::vector<int64_t>   m_sizes;
    std::vector<int64_t>   m_timestamps;   
    std::vector<uint32_t>  m_name_offsets;
    std::vector<uint32_t>  m_ext_offsets;    // 2026-06-xx 新增：扩展名在字符串池中的偏移，实现零解析搜索
    std::vector<uint32_t>  m_attributes;
    std::vector<uint8_t>   m_metadata_fetched; // 0: 未获取, 1: 获取中, 2: 已完成
    std::vector<uint8_t>   m_string_pool;

    std::vector<std::wstring> m_drive_list;
    std::atomic<uint32_t>     m_drive_active_mask{0}; // 驱动器过滤掩码 (位图)
    std::atomic<uint64_t>     m_generation{0};        // 数据代数，用于检测 compact

    std::unordered_map<uint64_t, uint32_t>              m_frn_to_idx;
    std::unordered_map<size_t, bool>                    m_drive_ever_saved; // 2026-06-xx 按盘符独立维护全量保存状态

    mutable std::unordered_map<uint64_t, std::wstring>  m_path_cache;
    mutable std::shared_mutex m_pathCacheMutex;

    std::unordered_map<std::wstring, uint64_t>          m_next_usns;
    std::unordered_map<std::wstring, UsnWatcher*>      m_watcher_map; // 2026-07-07 物理重构：使用 Map 管理监听器以支持按盘符卸载

    mutable QReadWriteLock m_dataLock;
    QThreadPool*           m_metadataPool = nullptr; // 2026-06-xx 新增：物理属性补全专用线程池
    mutable QReadWriteLock m_iconCacheLock;
    QHash<QString, QIcon>  m_icon_cache;

    bool m_isInitialized = false;
    std::atomic<bool> m_isStopping{false}; // 2026-06-xx 新增：全局退出/中断令牌
    std::atomic<bool> m_searchCancelRequested{false}; // 2026-07-xx 新增：高频搜索取消令牌
    uint32_t m_dirty_count = 0;
    std::unordered_set<uint32_t> m_dirty_indices; // 2026-06-xx 新增：记录变动的 SoA 下标
    std::unordered_map<uint32_t, uint64_t> m_dead_frns; // 2026-06-xx 新增：记录已删除项的原始 FRN 用于落盘
    std::mutex   m_dirtyLock; // 2026-06-xx 新增：保护脏数据追踪容器
    
    // 2026-06-xx 新增：合并期间的缓冲机制
    std::unordered_map<size_t, bool> m_is_compacting; 
    std::unordered_map<size_t, std::vector<ScchDataPackage>> m_compaction_buffer;

    std::vector<ChangeEvent> m_changeJournal;
    std::mutex               m_journalMutex;
    QTimer*                  m_notifyTimer = nullptr;
    size_t   m_dead_count = 0;
    size_t   m_wasted_string_bytes = 0;
    std::vector<uint32_t> m_sorted_indices;
};

} // namespace FERREX
