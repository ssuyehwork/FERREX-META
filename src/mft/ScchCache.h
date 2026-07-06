#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace FERREX-META {

// Plan-137: 双文件格式规范
constexpr uint32_t BIN_MAGIC_VAL = 0x4E49422E; // ".BIN"
constexpr uint32_t IDX_MAGIC_VAL = 0x5844492E; // ".IDX"
constexpr uint16_t STORAGE_VERSION = 1;

#pragma pack(push, 1)
struct BinHeader {
    uint32_t magic;
    uint16_t version;
    uint64_t volume_serial;
    uint64_t record_count;
};

struct IdxHeader {
    uint32_t magic;
    uint16_t version;
    uint64_t volume_serial;
    uint64_t next_usn;
    uint64_t main_count;
    uint64_t delta_count;
};

struct IndexEntry {
    uint64_t frn;
    uint64_t offset;
};
#pragma pack(pop)

class ScchCache {
public:
    struct Record {
        uint64_t frn;
        uint64_t parentFrn;
        std::string name;
        uint32_t attributes;
        int64_t timestamp;
    };

    /**
     * @brief 追加一批记录到 .bin，并更新 .idx 的 delta layer
     */
    static bool appendBatch(const std::string& binPath, const std::string& idxPath, 
                            uint64_t volumeSerial, uint64_t nextUsn, const std::vector<Record>& records);

    /**
     * @brief 从 .idx 加载索引项
     */
    static bool loadIndex(const std::string& idxPath, uint64_t volumeSerial, uint64_t& nextUsn,
                         std::vector<IndexEntry>& mainIndex, std::vector<IndexEntry>& deltaLayer);

    /**
     * @brief 批量从 .bin 读取记录 (优化性能)
     */
    static bool readRecords(const std::string& binPath, const std::vector<IndexEntry>& entries, std::vector<Record>& records);

    /**
     * @brief 如果 .idx 缺失或损坏，从 .bin 全量重建
     */
    static bool rebuildIndexFromBin(const std::string& binPath, uint64_t volumeSerial,
                                    std::vector<IndexEntry>& mainIndex);

    /**
     * @brief 执行合并：读取 bin，应用 delta，写出全新的 bin+idx
     */
    static bool performCompaction(const std::string& binPath, const std::string& idxPath, 
                                  uint64_t volumeSerial, uint64_t nextUsn);

    static uint32_t computeCrc32(const uint8_t* data, size_t len);
};

} // namespace FERREX-META
