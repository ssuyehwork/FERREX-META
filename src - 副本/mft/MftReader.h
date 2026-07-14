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

class NtfsVolumeMftParser;
class UsnJournalTreeSynchronizer;
class DiskIndexCacheCoordinator;
class MemoryQueryEngine;

class MftReader : public QObject {
    Q_OBJECT
    friend class ScanController; 
    friend class NtfsVolumeMftParser;
    friend class UsnJournalTreeSynchronizer;
    friend class DiskIndexCacheCoordinator;
    friend class MemoryQueryEngine;
public:
    static MftReader& instance();

    QIcon getCachedIcon(const QString& ext, bool isDir);

signals:
    void dataChanged(int index = -1);
    void entriesChangedBatch();        
    void driveLoaded(const QString& drive, int count, int total); 

public:
    
    void buildIndex(const QStringList& drives = QStringList());
    bool loadFromCache(); 
    bool loadDriveFromCache(const QString& drive); 
    void unloadDrive(const QString& drive);        
    bool saveToCache(); 
    bool saveDriveToCache(size_t driveIdx); 
    void clear();

    void updateActiveDrives(const QStringList& activeDrives);
    bool isDriveIndexed(const QString& drive);

    
    std::vector<uint64_t> search(const QString& query, bool useRegex = false, bool caseSensitive = false, 
                                 const QStringList& extensionList = QStringList(), 
                                 bool includeHidden = true, bool includeSystem = true,
                                 bool includeDollar = true);

    bool     matchEntry(int index, const QString& query, bool useRegex, bool caseSensitive, 
                        const QStringList& extensionList, bool includeHidden, bool includeSystem,
                        bool includeDollar = true) const;
    int      getIndexByKey(uint64_t compositeKey) const;
    uint64_t getKeyByIndex(int index) const;
    QString  getName(int index) const;
    const char* getExt(int index) const; 
    QString     getExtQString(int index) const; 
    int64_t getSize(int index) const;
    int64_t getModifyTime(int index) const;
    uint32_t getAttributes(int index) const;
    uint64_t getFrn(int index) const;
    bool isDirectory(int index) const;
    int totalCount() const;            
    int activeCount() const;           
    QString getFullPath(int index) const;
    void requestMetadata(int index);
    bool isMetadataFetched(int index) const;

    static inline uint64_t makeKey(size_t driveIdx, uint64_t frn) {
        return (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);
    }

    void updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume);
    void removeEntryByFrn(const std::wstring& volume, uint64_t frn);

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

    struct RawEntry {
        uint64_t frn;
        uint64_t parentFrn;
        uint64_t size; 
        uint32_t attributes;
        int64_t  modifyTime;
        uint32_t nameOffset; 
    };
    struct DriveResult {
        std::vector<RawEntry> entries;
        std::vector<uint8_t>  string_pool; 
        uint64_t nextUsn;
    };

    bool saveDriveToCacheInternal(size_t driveIdx); 
    bool saveDriveToCacheUnlocked(size_t driveIdx); 
    void clearInternal(); 
    void rebuildFrnToIndexMap();
    void compact(bool force = false);
    void buildSortedIndices();
    
    bool loadMftDirect(const std::wstring& volume, DriveResult& result);
    void mergeDriveResult(const std::wstring& volume, const DriveResult& result, size_t driveIdx);

    std::vector<uint64_t>  m_frns;
    std::vector<uint64_t>  m_parent_frns;  
    std::vector<uint32_t>  m_parent_indices; 
    std::vector<int64_t>   m_sizes;
    std::vector<int64_t>   m_timestamps;   
    std::vector<uint32_t>  m_name_offsets;
    std::vector<uint32_t>  m_ext_offsets;    
    std::vector<uint32_t>  m_attributes;
    std::vector<uint8_t>   m_metadata_fetched; 
    std::vector<uint8_t>   m_string_pool;

    std::vector<std::wstring> m_drive_list;
    std::atomic<uint32_t>     m_drive_active_mask{0}; 
    std::atomic<uint64_t>     m_generation{0};        

    std::unordered_map<uint64_t, uint32_t>              m_frn_to_idx;
    std::unordered_map<size_t, bool>                    m_drive_ever_saved; 

    mutable std::unordered_map<uint64_t, std::wstring>  m_path_cache;
    mutable std::mutex m_pathCacheMutex;

    std::unordered_map<std::wstring, uint64_t>          m_next_usns;
    std::unordered_map<std::wstring, UsnWatcher*>      m_watcher_map; 

    mutable QReadWriteLock m_dataLock;
    QThreadPool*           m_metadataPool = nullptr; 
    mutable QReadWriteLock m_iconCacheLock;
    QHash<QString, QIcon>  m_icon_cache;

    bool m_isInitialized = false;
    std::atomic<bool> m_isStopping{false}; 
    uint32_t m_dirty_count = 0;
    std::unordered_set<uint32_t> m_dirty_indices; 
    std::unordered_map<uint32_t, uint64_t> m_dead_frns; 
    std::mutex   m_dirtyLock; 

    std::unordered_map<size_t, bool> m_is_compacting; 
    std::unordered_map<size_t, std::vector<ScchDataPackage>> m_compaction_buffer;

    std::vector<ChangeEvent> m_changeJournal;
    std::mutex               m_journalMutex;
    QTimer*                  m_notifyTimer = nullptr;
    size_t   m_dead_count = 0;
    size_t   m_wasted_string_bytes = 0;
    std::vector<uint32_t> m_sorted_indices;
};

} 
