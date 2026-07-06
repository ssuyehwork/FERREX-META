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
#include <atomic>
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
 * 已适配 Plan-137 双文件存储协议，并针对丝滑流畅进行了锁分离优化。
 */
class MftReader : public QObject {
    Q_OBJECT
public:
    static MftReader& instance();

    QIcon getCachedIcon(const QString& ext, bool isDir);

signals:
    void dataChanged(int index = -1);
    void entryAdded(uint32_t index);
    void entryRemoved(uint64_t key);
    void entryUpdated(uint32_t index);
    void driveLoaded(const QString& drive, int count, int total);

public:
    void buildIndex(const QStringList& drives = QStringList());
    bool loadFromCache();
    void clear();

    void updateActiveDrives(const QStringList& activeDrives);
    bool isDriveIndexed(const QString& drive);

    std::vector<uint64_t> search(const QString& query, bool useRegex = false, bool caseSensitive = false, 
                                 const QStringList& extensionList = QStringList(), 
                                 bool includeHidden = true, bool includeSystem = true,
                                 bool includeDollar = true);
    
    bool matchEntry(int index, const QString& query, bool useRegex, bool caseSensitive, 
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

    static inline uint64_t makeKey(size_t driveIdx, uint64_t frn) {
        return (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);
    }

    void updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume);
    void removeEntryByFrn(const std::wstring& volume, uint64_t frn);
    std::wstring getPathFast(size_t driveIdx, uint64_t frn);

private:
    MftReader();
    ~MftReader();

    struct DriveResult {
        std::vector<ScchCache::Record> entries;
        uint64_t nextUsn;
        uint64_t volumeSerial;
    };

    void clearInternal(); 
    void rebuildFrnToIndexMap();
    void compact();
    void buildSortedIndices();
    
    bool loadMftDirect(const std::wstring& volume, DriveResult& result);
    void mergeDriveResultInternal(const std::wstring& volume, const DriveResult& result, size_t driveIdx);

    void flushDirtyToDisk(size_t driveIdx);

    // SoA 主数据
    std::vector<uint64_t>  m_frns;
    std::vector<uint64_t>  m_parent_frns; 
    std::vector<int64_t>   m_sizes;
    std::vector<int64_t>   m_timestamps;   
    std::vector<uint32_t>  m_name_offsets;
    std::vector<uint32_t>  m_attributes;
    std::vector<uint8_t>   m_metadata_fetched; // 0: 未获取, 1: 获取中, 2: 已完成
    std::vector<uint8_t>   m_string_pool;

    std::vector<std::wstring> m_drive_list;
    std::vector<uint64_t>     m_drive_serials;
    std::atomic<uint32_t>     m_drive_active_mask{0};

    std::unordered_map<uint64_t, uint32_t> m_frn_to_idx;

    mutable std::unordered_map<uint64_t, std::wstring> m_path_cache;
    mutable std::mutex m_pathCacheMutex;

    std::unordered_map<std::wstring, uint64_t> m_next_usns;
    std::vector<UsnWatcher*> m_watchers;

    mutable QReadWriteLock m_dataLock;
    mutable QReadWriteLock m_iconCacheLock;
    QHash<QString, QIcon>  m_icon_cache;

    std::unordered_map<size_t, std::vector<ScchCache::Record>> m_dirty_buffers;
    std::mutex m_dirtyMutex;

    bool m_isInitialized = false;
    std::vector<uint32_t> m_sorted_indices;
};

} // namespace ArcMeta
