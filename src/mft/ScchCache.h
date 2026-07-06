#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace FERREX {

// 当前格式版本
constexpr uint16_t SCCH_VERSION_MAJOR = 2; // 升级版本号以区分旧格式
constexpr uint16_t SCCH_VERSION_MINOR = 1;
constexpr char     SCCH_MAGIC_BIN[4]  = {'S','C','B','N'};
constexpr char     SCCH_MAGIC_IDX[4]  = {'S','C','I','X'};

#pragma pack(push, 1)

// .bin 文件头
struct ScchBinHeader {
    char     magic[4];           // "SCBN"
    uint16_t version_major;
    uint16_t version_minor;
    int64_t  created_at;
    uint64_t total_records;      // 包含增量和 tombstone
    uint32_t flags;
    uint32_t reserved;
};

// .idx 文件头
struct ScchIdxHeader {
    char     magic[4];           // "SCIX"
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t main_index_count;   // 已排序的主索引条目数
    uint64_t delta_index_count;  // 增量层条目数
    uint64_t tombstone_count;    // 标记删除的条目数
    uint64_t last_usn;           // 该磁盘记录的最新 USN
    uint32_t crc32;              // 头部之后所有字节的 CRC32
};

// 磁盘上的单条记录（用于 .bin）
struct ScchRecord {
    uint64_t frn;
    uint64_t parent_frn;
    int64_t  size;
    int64_t  timestamp;
    uint32_t attributes;
    uint8_t  metadata_fetched;
    uint8_t  tombstone;          // 1 表示已删除
    uint32_t name_len;           // 文件名长度（字节）
    uint32_t record_crc32;       // 记录完整性校验
    // 紧跟 UTF-8 文件名数据
};

// 索引条目（用于 .idx）
struct ScchIndexEntry {
    uint64_t frn;
    uint64_t offset;             // 在 .bin 文件中的偏移
};

#pragma pack(pop)

// 内存中用于传递数据的结构
struct ScchDataPackage {
    uint64_t frn;
    uint64_t parent_frn;
    int64_t  size;
    int64_t  timestamp;
    uint32_t attributes;
    uint8_t  metadata_fetched;
    uint8_t  tombstone;
    std::string name;
};

enum class ScchResult {
    Ok,
    FileNotFound,
    BadMagic,
    VersionMismatch,
    CrcMismatch,
    Truncated,
    IoError,
};

const char* scchResultString(ScchResult r);

class ScchCache {
public:
    // 全量保存（用于首次同步或 Compaction）
    static bool saveAll(
        const std::string& path_base,
        const std::vector<ScchDataPackage>& records,
        uint64_t last_usn
    );

    // 增量追加
    static bool appendEntries(
        const std::string& path_base,
        const std::vector<ScchDataPackage>& records,
        uint64_t last_usn
    );

    // 加载
    static ScchResult load(
        const std::string& path_base,
        std::vector<ScchDataPackage>& out_records,
        uint64_t& out_last_usn
    );

    // 合并机制
    static bool compact(const std::string& path_base);

    // 检查是否需要合并
    static bool needsCompaction(const std::string& path_base, uint32_t delta_threshold = 5000, float tombstone_ratio = 0.3f);

    static uint32_t computeCrc32(const uint8_t* data, size_t len);
};

} // namespace FERREX
