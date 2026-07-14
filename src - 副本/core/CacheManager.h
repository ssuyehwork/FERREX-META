#pragma once

#include "IndexedEntry.h"
#include <QString>
#include <QVector>
#include <QHash>
#include <QDateTime>
#include <memory>
#include <atomic>

namespace FERREX {

struct CacheHeader {
    char magic[4];          
    uint32_t version;       
    uint64_t timestamp;     
    uint64_t entryCount;    
    uint64_t stringPoolSize; 
    uint64_t driveSerial;   
    uint64_t usnWatermark;  
    uint32_t checksum;     
    uint32_t reserved;     
};

#pragma pack(push, 1)
struct CacheEntry {
    uint64_t frn;          
    uint64_t parentFrn;    
    uint64_t size;         
    uint64_t modifyTime;   
    uint32_t nameOffset;   
    uint32_t flags;        
};
#pragma pack(pop)

enum class CacheValidationResult {
    Valid,              
    InvalidFormat,      
    Corrupted,          
    Outdated,           
    DriveMismatch,      
    NotExist           
};

struct CacheStats {
    bool isValid = false;
    QDateTime cacheTime;
    QDateTime lastScanTime;
    uint64_t entryCount = 0;
    uint64_t cacheSize = 0;
    uint32_t loadTimeMs = 0;
};

class CacheManager {
public:
    explicit CacheManager();
    ~CacheManager();

    CacheManager(const CacheManager&) = delete;
    CacheManager& operator=(const CacheManager&) = delete;

    CacheValidationResult validateCache(const QString& drivePath);

    bool loadFromCache(const QString& drivePath, QList<IndexedEntry>& outEntries);
    bool loadFromCache(const QStringList& drivePaths, QList<IndexedEntry>& outEntries);

    bool saveToCache(const QString& drivePath, const QList<IndexedEntry>& entries);
    bool saveToCache(const QString& drivePath, const std::vector<IndexedEntry>& entries);
    bool saveToCache(const QStringList& drivePaths, const QList<IndexedEntry>& entries);

    CacheStats getCacheStats(const QString& drivePath);

    bool clearCache(const QString& drivePath);

    void clearAllCache();

    QString getCacheDirectory() const;

private:
    
    QString getCacheFilePath(const QString& drivePath) const;
    uint64_t getDriveSerial(const QString& drivePath) const;
    uint32_t calculateChecksum(const QByteArray& data) const;
    bool createCacheDirectory() const;

    QByteArray buildStringPool(const QList<IndexedEntry>& entries, QVector<uint32_t>& nameOffsets) const;
    QString extractStringFromPool(const char* poolData, size_t poolSize, uint32_t offset) const;

    static void initializeCRCTable();
    static uint32_t crc32(const void* data, size_t size);

private:
    QString m_cacheDir;
    mutable QHash<QString, uint64_t> m_driveSerialCache; 

    static bool s_crcTableInitialized;
    static std::vector<uint32_t> s_crcTable;
};

} 
