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
    void driveLoaded(const QString& drive, int count, int total);

    // Plan-Smooth: 聚合信号，一次性传递所有变动，彻底杜绝 UI 信号风暴导致的 ANR
    struct Change { enum Type { Add, Rem, Upd } type; uint64_t key; uint32_t index; };
    void changesApplied(const std::vector<MftReader::Change>& changes);

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
    
    // Plan-Smooth: matchEntry 不再内部加锁，由调用者统一管理锁生命周期以减少竞争
    bool matchEntry(int index, const QString& query, bool useRegex, bool caseSensitive,
                    const QStringList& extensionList, bool includeHidden, bool includeSystem,
                    bool includeDollar = true) const;

    // 针对高性能循环的匹配接口
    bool matchEntryOptimized(int index, const QString& query, const QRegularExpression& re, bool useRegex, bool caseSensitive,
                            const std::vector<std::string>& extList, bool includeHidden, bool includeSystem,
                            bool includeDollar = true) const;

    // Plan-Smooth: 提供无锁版 SoA 访问接口，用于高性能批量操作 (调用者必须确保持有 m_dataLock)
    int      getIndexByKey(uint64_t compositeKey) const;
    int      getIndexByKeyNoLock(uint64_t compositeKey) const;
    uint64_t getKeyByIndex(int index) const;
    uint64_t getKeyByIndexNoLock(int index) const;
    QString  getName(int index) const;
    QString  getNameNoLock(int index) const;
    int64_t  getSize(int index) const;
    int64_t  getSizeNoLock(int index) const;
    int64_t  getModifyTime(int index) const;
    int64_t  getModifyTimeNoLock(int index) const;
    uint32_t getAttributes(int index) const;
    uint32_t getAttributesNoLock(int index) const;
    uint64_t getFrn(int index) const;
    uint64_t getFrnNoLock(int index) const;
    bool     isDirectory(int index) const;
    bool     isDirectoryNoLock(int index) const;
    int totalCount() const;
    QString getFullPath(int index) const;
    void requestMetadata(int index);
    bool isMetadataFetched(int index) const;

    static inline uint64_t makeKey(size_t driveIdx, uint64_t frn) {
        return (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);
    }

    void updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume);
    void removeEntryByFrn(const std::wstring& volume, uint64_t frn);

    // Plan-Smooth: 异步批量应用增量记录，减少对 m_dataLock 写锁的竞争频率
    void applyBufferedUsnUpdates();
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

public:
    mutable QReadWriteLock m_dataLock;
    std::atomic<size_t> m_total_count{0};
private:
    mutable QReadWriteLock m_iconCacheLock;
    QHash<QString, QIcon>  m_icon_cache;

    std::unordered_map<size_t, std::vector<ScchCache::Record>> m_dirty_buffers;
    std::mutex m_dirtyMutex;

    struct UsnUpdateTask {
        bool isDelete;
        uint64_t frn;
        uint64_t parentFrn;
        uint32_t attributes;
        int64_t timestamp;
        uint64_t usn;
        QString name;
        std::wstring volume;
    };
    std::vector<UsnUpdateTask> m_usn_task_buffer;
    std::mutex m_usnTaskMutex;
    std::atomic<bool> m_usn_task_running{false};

    bool m_isInitialized = false;
    std::vector<uint32_t> m_sorted_indices;
};

} // namespace ArcMeta
