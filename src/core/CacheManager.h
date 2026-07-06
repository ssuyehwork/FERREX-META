#pragma once

#include "IndexedEntry.h"
#include <QString>
#include <QVector>
#include <QHash>
#include <QDateTime>
#include <memory>
#include <atomic>

namespace ArcMeta {

// 2026-05-09 按照用户要求：实现高效缓存机制，参考 FERREX 的 FIDX 格式
struct CacheHeader {
    char magic[4];          // "SCCH" (ScanCache)
    uint32_t version;       // 版本号 (当前为 2)
    uint64_t timestamp;     // 缓存创建时间 (Unix 毫秒)
    uint64_t entryCount;    // 条目总数
    uint64_t stringPoolSize; // 字符串池大小
    uint64_t driveSerial;   // 驱动器序列号
    uint64_t usnWatermark;  // 新增：USN 水位
    uint32_t checksum;     // CRC32 校验
    uint32_t reserved;     // 保留字段
};

// 紧凑型缓存条目 (40字节，对标 Rust 的 FileRecord)
#pragma pack(push, 1)
struct CacheEntry {
    uint64_t frn;          // 新增：文件参考号
    uint64_t parentFrn;    // 新增：父文件参考号
    uint64_t size;         // 文件大小
    uint64_t modifyTime;   // 修改时间 (Unix 毫秒)
    uint32_t nameOffset;   // 名称在字符串池中的偏移
    uint32_t flags;        // 标志位 (复用为文件属性 FILE_ATTRIBUTE_XXX)
};
#pragma pack(pop)

// 缓存验证结果
enum class CacheValidationResult {
    Valid,              // 缓存有效
    InvalidFormat,      // 文件格式无效
    Corrupted,          // CRC 校验失败
    Outdated,           // 缓存过期
    DriveMismatch,      // 驱动器不匹配
    NotExist           // 缓存文件不存在
};

// 缓存统计信息
struct CacheStats {
    bool isValid = false;
    QDateTime cacheTime;
    QDateTime lastScanTime;
    uint64_t entryCount = 0;
    uint64_t cacheSize = 0;
    uint32_t loadTimeMs = 0;
};

/**
 * @brief 高效扫描缓存管理器
 * 
 * 该类负责 ScanDialog 的缓存读写、验证和管理。
 * 使用二进制紧凑格式和内存映射实现极速加载。
 */
class CacheManager {
public:
    explicit CacheManager();
    ~CacheManager();

    // 禁用拷贝构造和赋值
    CacheManager(const CacheManager&) = delete;
    CacheManager& operator=(const CacheManager&) = delete;

    /**
     * @brief 验证缓存文件有效性
     * @param drivePath 驱动器路径 (如 "C:")
     * @return 验证结果
     */
    CacheValidationResult validateCache(const QString& drivePath);

    /**
     * @brief 从缓存加载扫描结果
     * @param drivePath 驱动器路径
     * @param outEntries 输出的条目列表
     * @return 是否加载成功
     */
    bool loadFromCache(const QString& drivePath, QList<IndexedEntry>& outEntries);
    bool loadFromCache(const QStringList& drivePaths, QList<IndexedEntry>& outEntries);

    /**
     * @brief 保存扫描结果到缓存
     * @param drivePath 驱动器路径
     * @param entries 要保存的条目列表
     * @return 是否保存成功
     */
    bool saveToCache(const QString& drivePath, const QList<IndexedEntry>& entries);
    bool saveToCache(const QString& drivePath, const std::vector<IndexedEntry>& entries);
    bool saveToCache(const QStringList& drivePaths, const QList<IndexedEntry>& entries);

    /**
     * @brief 获取缓存统计信息
     * @param drivePath 驱动器路径
     * @return 缓存统计
     */
    CacheStats getCacheStats(const QString& drivePath);

    /**
     * @brief 清除指定驱动的缓存
     * @param drivePath 驱动器路径
     * @return 是否清除成功
     */
    bool clearCache(const QString& drivePath);

    /**
     * @brief 清除所有缓存
     */
    void clearAllCache();

    /**
     * @brief 获取缓存目录路径
     * @return 缓存目录路径
     */
    QString getCacheDirectory() const;

private:
    // 内部实现方法
    QString getCacheFilePath(const QString& drivePath) const;
    uint64_t getDriveSerial(const QString& drivePath) const;
    uint32_t calculateChecksum(const QByteArray& data) const;
    bool createCacheDirectory() const;
    
    // 字符串池处理
    QByteArray buildStringPool(const QList<IndexedEntry>& entries, QVector<uint32_t>& nameOffsets) const;
    QString extractStringFromPool(const char* poolData, size_t poolSize, uint32_t offset) const;

    // CRC32 相关
    static void initializeCRCTable();
    static uint32_t crc32(const void* data, size_t size);

private:
    QString m_cacheDir;
    mutable QHash<QString, uint64_t> m_driveSerialCache; // 驱动器序列号缓存
    
    // CRC32 相关静态成员
    static bool s_crcTableInitialized;
    static std::vector<uint32_t> s_crcTable;
};

} // namespace ArcMeta
