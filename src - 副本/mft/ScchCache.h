#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace FERREX {

constexpr uint16_t SCCH_VERSION_MAJOR = 2; 
constexpr uint16_t SCCH_VERSION_MINOR = 1;
constexpr char     SCCH_MAGIC_BIN[4]  = {'S','C','B','N'};
constexpr char     SCCH_MAGIC_IDX[4]  = {'S','C','I','X'};

#pragma pack(push, 1)

struct ScchBinHeader {
    char     magic[4];           
    uint16_t version_major;
    uint16_t version_minor;
    int64_t  created_at;
    uint64_t total_records;      
    uint32_t flags;
    uint32_t reserved;
};

struct ScchIdxHeader {
    char     magic[4];           
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t main_index_count;   
    uint64_t delta_index_count;  
    uint64_t tombstone_count;    
    uint64_t last_usn;           
    uint32_t crc32;              
};

struct ScchRecord {
    uint64_t frn;
    uint64_t parent_frn;
    int64_t  size;
    int64_t  timestamp;
    uint32_t attributes;
    uint8_t  metadata_fetched;
    uint8_t  tombstone;          
    uint32_t name_len;           
    uint32_t record_crc32;       
    
};

struct ScchIndexEntry {
    uint64_t frn;
    uint64_t offset;             
};

#pragma pack(pop)

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
    
    static bool saveAll(
        const std::string& path_base,
        const std::vector<ScchDataPackage>& records,
        uint64_t last_usn
    );

    static bool appendEntries(
        const std::string& path_base,
        const std::vector<ScchDataPackage>& records,
        uint64_t last_usn
    );

    static ScchResult load(
        const std::string& path_base,
        std::vector<ScchDataPackage>& out_records,
        uint64_t& out_last_usn
    );

    static bool compact(const std::string& path_base);

    static bool needsCompaction(const std::string& path_base, uint32_t delta_threshold = 5000, float tombstone_ratio = 0.3f);

    static uint32_t computeCrc32(const uint8_t* data, size_t len);
};

} 
