#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CacheManager.h"
#include "IndexedEntry.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDataStream>
#include <QCryptographicHash>
#include <QDebug>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif

namespace FERREX {

std::vector<uint32_t> CacheManager::s_crcTable;
bool CacheManager::s_crcTableInitialized = false;

CacheManager::CacheManager() {
    
    QString appDir = QCoreApplication::applicationDirPath();
    m_cacheDir = appDir + "/Cache";

    if (!s_crcTableInitialized) {
        initializeCRCTable();
        s_crcTableInitialized = true;
    }
}

CacheManager::~CacheManager() {
    
}

CacheValidationResult CacheManager::validateCache(const QString& drivePath) {
    QString cacheFile = getCacheFilePath(drivePath);
    QFile file(cacheFile);
    if (!file.exists()) return CacheValidationResult::NotExist;
    if (!file.open(QIODevice::ReadOnly)) return CacheValidationResult::NotExist;
    
    qint64 fileSize = file.size();
    if (fileSize < (qint64)sizeof(CacheHeader)) return CacheValidationResult::InvalidFormat;

    uchar* data = file.map(0, fileSize);
    if (!data) return CacheValidationResult::InvalidFormat;

    const CacheHeader* header = reinterpret_cast<const CacheHeader*>(data);

    if (memcmp(header->magic, "SCCH", 4) != 0 || header->version != 2) {
        file.unmap(data);
        return CacheValidationResult::InvalidFormat;
    }

    uint64_t currentSerial = getDriveSerial(drivePath);
    if (currentSerial != 0 && header->driveSerial != currentSerial) {
        file.unmap(data);
        return CacheValidationResult::DriveMismatch;
    }

    uint32_t storedChecksum = header->checksum;

    

    

    

    
    CacheHeader headerCopy = *header;
    headerCopy.checksum = 0;
    uint32_t check = crc32(&headerCopy, sizeof(CacheHeader));
    check = crc32(data + sizeof(CacheHeader), fileSize - sizeof(CacheHeader)); 

    auto crc_func = [&](const void* d, size_t s, uint32_t initial) {
        uint32_t c = initial;
        const uint8_t* p = static_cast<const uint8_t*>(d);
        for (size_t i = 0; i < s; ++i) {
            c = s_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        }
        return c;
    };

    uint32_t finalCrc = 0xFFFFFFFF;
    finalCrc = crc_func(data, 0x30, finalCrc); 
    uint32_t z = 0;
    finalCrc = crc_func(&z, 4, finalCrc);      
    finalCrc = crc_func(data + 0x34, fileSize - 0x34, finalCrc); 
    finalCrc ^= 0xFFFFFFFF;

    if (finalCrc != storedChecksum) {
        file.unmap(data);
        return CacheValidationResult::Corrupted;
    }
    
    file.unmap(data);

    if (QDateTime::fromMSecsSinceEpoch(header->timestamp).addDays(1) < QDateTime::currentDateTime()) {
        return CacheValidationResult::Outdated;
    }
    
    return CacheValidationResult::Valid;
}

bool CacheManager::loadFromCache(const QStringList& drivePaths, QList<IndexedEntry>& outEntries) {
    bool anySuccess = false;
    for (const QString& drive : drivePaths) {
        if (loadFromCache(drive, outEntries)) anySuccess = true;
    }
    return anySuccess;
}

bool CacheManager::saveToCache(const QStringList& drivePaths, const QList<IndexedEntry>& entries) {
    Q_UNUSED(drivePaths);
    Q_UNUSED(entries);
    
    return false;
}

bool CacheManager::loadFromCache(const QString& drivePath, QList<IndexedEntry>& outEntries) {
    QString cacheFile = getCacheFilePath(drivePath);
    QFile file(cacheFile);
    if (!file.exists()) return false;

    CacheValidationResult validation = validateCache(drivePath);
    if (validation != CacheValidationResult::Valid) return false;
    
    if (!file.open(QIODevice::ReadOnly)) return false;
    qint64 fileSize = file.size();

    uchar* data = file.map(0, fileSize);
    if (!data) return false;

    const CacheHeader* header = reinterpret_cast<const CacheHeader*>(data);
    uint64_t entryCount = header->entryCount;
    
    const CacheEntry* cacheEntries = reinterpret_cast<const CacheEntry*>(data + sizeof(CacheHeader));
    const char* stringPoolData = reinterpret_cast<const char*>(data + sizeof(CacheHeader) + entryCount * sizeof(CacheEntry));
    size_t stringPoolSize = static_cast<size_t>(header->stringPoolSize);

    outEntries.clear();
    outEntries.reserve(entryCount);
    
    for (uint64_t i = 0; i < entryCount; ++i) {
        const CacheEntry& ce = cacheEntries[i];
        IndexedEntry entry;
        entry.frn = ce.frn;
        entry.parentFrn = ce.parentFrn;
        entry.size = ce.size;
        entry.modifyTime = ce.modifyTime;
        entry.parentIndex = 0; 
        entry.attributes = ce.flags;
        entry.isDir = (ce.flags & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.name = extractStringFromPool(stringPoolData, stringPoolSize, ce.nameOffset);
        outEntries.append(entry);
    }
    
    file.unmap(data);
    qDebug() << "Mmap loaded" << entryCount << "entries from" << drivePath;
    return true;
}

bool CacheManager::saveToCache(const QString& drivePath, const QList<IndexedEntry>& entries) {
    if (entries.isEmpty()) return false;
    std::vector<IndexedEntry> vec;
    vec.reserve(entries.size());
    for(const auto& e : entries) vec.push_back(e);
    return saveToCache(drivePath, vec);
}

bool CacheManager::saveToCache(const QString& drivePath, const std::vector<IndexedEntry>& entries) {
    if (entries.empty()) {
        return false;
    }

    if (!createCacheDirectory()) {
        return false;
    }
    
    QString cacheFile = getCacheFilePath(drivePath);

    QVector<uint32_t> nameOffsets;
    
    QByteArray stringPool;
    {
        nameOffsets.reserve(static_cast<int>(entries.size()));
        for (const auto& entry : entries) {
            nameOffsets.append(static_cast<uint32_t>(stringPool.size()));
            stringPool.append(entry.name.toUtf8());
            stringPool.append('\0');
        }
    }

    CacheHeader header;
    memcpy(header.magic, "SCCH", 4);
    header.version = 2;
    header.timestamp = QDateTime::currentMSecsSinceEpoch();
    header.entryCount = entries.size();
    header.stringPoolSize = stringPool.size();
    header.driveSerial = getDriveSerial(drivePath);
    header.usnWatermark = 0; 
    header.checksum = 0; 
    header.reserved = 0;

    QVector<CacheEntry> cacheEntries;
    cacheEntries.reserve(static_cast<int>(entries.size()));
    
    for (size_t i = 0; i < entries.size(); ++i) {
        const IndexedEntry& srcEntry = entries[i];
        CacheEntry cacheEntry;
        
        cacheEntry.frn = srcEntry.frn;
        cacheEntry.parentFrn = srcEntry.parentFrn;
        cacheEntry.size = srcEntry.size;
        cacheEntry.modifyTime = srcEntry.modifyTime;
        cacheEntry.nameOffset = nameOffsets[static_cast<int>(i)];
        cacheEntry.flags = srcEntry.attributes;
        
        cacheEntries.append(cacheEntry);
    }

    QByteArray fullData;
    fullData.reserve(sizeof(CacheHeader) + 
                     cacheEntries.size() * sizeof(CacheEntry) + 
                     stringPool.size());

    fullData.append(reinterpret_cast<const char*>(&header), sizeof(CacheHeader));

    fullData.append(reinterpret_cast<const char*>(cacheEntries.constData()), 
                   cacheEntries.size() * sizeof(CacheEntry));

    fullData.append(stringPool);

    uint32_t checksum = crc32(fullData.constData(), static_cast<size_t>(fullData.size()));
    
    reinterpret_cast<CacheHeader*>(fullData.data())->checksum = checksum;

    QFile file(cacheFile);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open cache file for writing:" << cacheFile << "Error:" << file.errorString();
        return false;
    }
    
    qint64 bytesWritten = file.write(fullData);
    bool success = (bytesWritten == fullData.size());
    if (success) {
        file.flush();
        qDebug() << "Successfully saved" << entries.size() << "entries to cache for" << drivePath;
    } else {
        qDebug() << "Failed to write cache file for" << drivePath 
                 << "Expected:" << fullData.size() << "bytes"
                 << "Written:" << bytesWritten << "bytes"
                 << "Error:" << file.errorString();
    }
    
    return success;
}

CacheStats CacheManager::getCacheStats(const QString& drivePath) {
    CacheStats stats;
    QString cacheFile = getCacheFilePath(drivePath);
    QFileInfo fileInfo(cacheFile);
    
    if (!fileInfo.exists()) {
        return stats;
    }
    
    QFile file(cacheFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return stats;
    }
    
    QByteArray headerData = file.read(sizeof(CacheHeader));
    if (headerData.size() != sizeof(CacheHeader)) {
        return stats;
    }
    
    const CacheHeader* header = reinterpret_cast<const CacheHeader*>(headerData.constData());

    if (memcmp(header->magic, "SCCH", 4) != 0 || header->version != 2) {
        return stats;
    }
    
    stats.isValid = true;
    stats.cacheTime = QDateTime::fromMSecsSinceEpoch(header->timestamp);
    stats.entryCount = header->entryCount;
    stats.cacheSize = fileInfo.size();
    
    return stats;
}

bool CacheManager::clearCache(const QString& drivePath) {
    QString cacheFile = getCacheFilePath(drivePath);
    QFile file(cacheFile);
    
    if (file.exists()) {
        return file.remove();
    }
    
    return true; 
}

void CacheManager::clearAllCache() {
    QDir cacheDir(m_cacheDir);
    if (!cacheDir.exists()) {
        return;
    }
    
    QStringList filters;
    filters << "*.scch";
    
    QFileInfoList files = cacheDir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo& fileInfo : files) {
        QFile::remove(fileInfo.absoluteFilePath());
    }
    
    qDebug() << "Cleared all cache files";
}

QString CacheManager::getCacheDirectory() const {
    return m_cacheDir;
}

QString CacheManager::getCacheFilePath(const QString& drivePath) const {
    QString normalizedDrive = drivePath.toUpper();
    if (normalizedDrive.length() >= 2 && normalizedDrive[1] == ':') {
        normalizedDrive = normalizedDrive.left(1); 
    } else if (normalizedDrive.endsWith("/")) {
        normalizedDrive.remove("/");
    }
    
    return m_cacheDir + QString("/Drive_%1.scch").arg(normalizedDrive);
}

uint64_t CacheManager::getDriveSerial(const QString& drivePath) const {
    QString normalizedDrive = drivePath.toUpper();
    if (normalizedDrive.length() == 1) {
        normalizedDrive += ":";
    }

    if (m_driveSerialCache.contains(normalizedDrive)) {
        return m_driveSerialCache[normalizedDrive];
    }
    
#ifdef Q_OS_WIN
    
    QString volumePath = normalizedDrive + "\\";
    DWORD volumeSerialNumber;
    DWORD maximumComponentLength;
    DWORD fileSystemFlags;
    
    BOOL result = GetVolumeInformationW(
        reinterpret_cast<const wchar_t*>(volumePath.utf16()),
        NULL,
        0,
        &volumeSerialNumber,
        &maximumComponentLength,
        &fileSystemFlags,
        NULL,
        0
    );
    
    uint64_t serial = 0;
    if (result) {
        serial = static_cast<uint64_t>(volumeSerialNumber);
    }

    m_driveSerialCache[normalizedDrive] = serial;
    return serial;
#else
    Q_UNUSED(drivePath)
    return 0;
#endif
}

bool CacheManager::createCacheDirectory() const {
    QDir dir;
    return dir.mkpath(m_cacheDir);
}

QByteArray CacheManager::buildStringPool(const QList<IndexedEntry>& entries, 
                                        QVector<uint32_t>& nameOffsets) const {
    QByteArray pool;
    nameOffsets.clear();
    nameOffsets.reserve(entries.size());
    
    for (const IndexedEntry& entry : entries) {
        nameOffsets.append(pool.size());
        
        QByteArray nameData = entry.name.toUtf8();
        pool.append(nameData);
        pool.append('\0'); 
    }
    
    return pool;
}

QString CacheManager::extractStringFromPool(const char* poolData, size_t poolSize, uint32_t offset) const {
    if (offset >= poolSize) return QString();
    const char* strData = poolData + offset;
    return QString::fromUtf8(strData);
}

void CacheManager::initializeCRCTable() {
    s_crcTable.resize(256);
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
        s_crcTable[i] = crc;
    }
}

uint32_t CacheManager::crc32(const void* data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        crc = s_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

} 
