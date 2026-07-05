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
#include <windows.h>
#include <winioctl.h>
#include <QIcon>
#include <QHash>
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


namespace ArcMeta {

class UsnWatcher;

/**
 * @brief 高性能 MFT 索引引擎 (SoA 架构)
 */
class MftReader : public QObject {
    Q_OBJECT
public:
    static MftReader& instance();

    // 全局图标缓存管理 (解决 UAF 风险)
    QIcon getCachedIcon(const QString& ext, bool isDir);

signals:
    void dataChanged(int index = -1);
    void entryAdded(uint32_t index);   // 2026-05-28 性能优化：改用索引直接定位
    void entryRemoved(uint64_t key);   // 2026-06-xx 新增：实时删除信号
    void entryUpdated(uint32_t index); // 2026-05-28 性能优化：改用索引直接定位
    void driveLoaded(const QString& drive, int count, int total); // 2026-05-14 新增：驱动器就绪信号

public:
    // 生命周期管理
    void buildIndex(const QStringList& drives = QStringList());
    bool loadFromCache();
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
    int64_t getSize(int index) const;
    int64_t getModifyTime(int index) const;
    uint32_t getAttributes(int index) const;
    uint64_t getFrn(int index) const;
    bool isDirectory(int index) const;
    int totalCount() const;
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
        std::string nameUtf8;
    };
    struct DriveResult {
        std::vector<RawEntry> entries;
        uint64_t nextUsn;
    };

    bool saveDriveToCacheInternal(size_t driveIdx); 
    void clearInternal(); 
    void rebuildFrnToIndexMap();
    void compact();
    void buildSortedIndices();
    
    bool loadMftDirect(const std::wstring& volume, DriveResult& result);
    void mergeDriveResult(const std::wstring& volume, const DriveResult& result, size_t driveIdx);

    // SoA 主数据
    std::vector<uint64_t>  m_frns;
    std::vector<uint64_t>  m_parent_frns; // 高 16 位存储盘符索引
    std::vector<int64_t>   m_sizes;
    std::vector<int64_t>   m_timestamps;   
    std::vector<uint32_t>  m_name_offsets;
    std::vector<uint32_t>  m_attributes;
    std::vector<uint8_t>   m_metadata_fetched; // 0: 未获取, 1: 获取中, 2: 已完成
    std::vector<uint8_t>   m_string_pool;

    std::vector<std::wstring> m_drive_list;
    std::atomic<uint32_t>     m_drive_active_mask{0}; // 驱动器过滤掩码 (位图)

    std::unordered_map<uint64_t, uint32_t>              m_frn_to_idx;

    mutable std::unordered_map<uint64_t, std::wstring>  m_path_cache;
    mutable std::mutex m_pathCacheMutex;

    std::unordered_map<std::wstring, uint64_t>          m_next_usns;
    std::vector<UsnWatcher*> m_watchers;

    mutable QReadWriteLock m_dataLock;
    mutable QReadWriteLock m_iconCacheLock;
    QHash<QString, QIcon>  m_icon_cache;

    bool m_isInitialized = false;
    uint32_t m_dirty_count = 0;
    size_t   m_dead_count = 0;
    size_t   m_wasted_string_bytes = 0;
    std::vector<uint32_t> m_sorted_indices;
};

} // namespace ArcMeta
