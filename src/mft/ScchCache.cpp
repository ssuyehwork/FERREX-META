#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ScchCache.h"
#include <windows.h>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <iostream>
#include <array>
#include <algorithm>

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

const char* scchResultString(ScchResult r) {
    switch (r) {
        case ScchResult::Ok:               return "Ok";
        case ScchResult::FileNotFound:     return "文件不存在";
        case ScchResult::BadMagic:         return "魔数不匹配（非 .scch 文件）";
        case ScchResult::VersionMismatch:  return "版本不兼容，需要重新扫描";
        case ScchResult::CrcMismatch:      return "CRC 校验失败，文件已损坏";
        case ScchResult::Truncated:        return "文件不完整（意外截断）";
        case ScchResult::IoError:          return "I/O 读写错误";
    }
    return "未知错误";
}

// ── CRC32（标准多项式 0xEDB88320）────────────────────────────────

static const std::array<uint32_t, 256> CRC32_TABLE = []() {
    std::array<uint32_t, 256> table;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}();

uint32_t ScchCache::computeCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = CRC32_TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ── 保存 (Mmap 优化) ───────────────────────────────────────────

bool ScchCache::save(
    const char*                                  path,
    const std::vector<uint64_t>&                 frns,
    const std::vector<uint64_t>&                 parent_frns,
    const std::vector<int64_t>&                  sizes,
    const std::vector<int64_t>&                  timestamps,
    const std::vector<uint32_t>&                 name_offsets,
    const std::vector<uint32_t>&                 attributes,
    const std::vector<uint8_t>&                  metadata_fetched,
    const std::vector<uint8_t>&                  string_pool,
    const std::vector<uint32_t>&                 sorted_indices,
    const std::unordered_map<std::string, uint64_t>& usn_map
) {
    try {
        std::filesystem::path p(path);
        std::filesystem::create_directories(p.parent_path());
        
        std::string tmpPath = p.string() + ".tmp";
        std::wstring wTmpPath = std::filesystem::path(tmpPath).wstring();

        // 1. 计算总大小
        size_t bodySize = 0;
        bodySize += 8 + frns.size() * 8;
        bodySize += 8 + parent_frns.size() * 8;
        bodySize += 8 + sizes.size() * 8;
        bodySize += 8 + timestamps.size() * 8;
        bodySize += 8 + name_offsets.size() * 4;
        bodySize += 8 + attributes.size() * 4;
        bodySize += 8 + metadata_fetched.size();
        bodySize += 8 + string_pool.size();
        bodySize += 8 + sorted_indices.size() * 4;
        bodySize += 8 + usn_map.size() * sizeof(ScchUsnEntry);

        size_t totalSize = sizeof(ScchHeader) + bodySize;

        // 2. 创建文件并映射
        HANDLE hFile = CreateFileW(wTmpPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER li; li.QuadPart = totalSize;
        if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) || !SetEndOfFile(hFile)) {
            CloseHandle(hFile); return false;
        }

        HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
        if (!hMap) { CloseHandle(hFile); return false; }

        uint8_t* base = static_cast<uint8_t*>(MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0));
        if (!base) { CloseHandle(hMap); CloseHandle(hFile); return false; }

        // 3. 顺序写入数据
        uint8_t* ptr = base + sizeof(ScchHeader);
        auto writeRaw = [&](const void* data, size_t len) {
            memcpy(ptr, data, len);
            ptr += len;
        };
        auto writeU64 = [&](uint64_t v) { writeRaw(&v, 8); };
        auto writeVec64u = [&](const std::vector<uint64_t>& v) { writeU64(v.size()); writeRaw(v.data(), v.size() * 8); };
        auto writeVec64i = [&](const std::vector<int64_t>& v) { writeU64(v.size()); writeRaw(v.data(), v.size() * 8); };
        auto writeVec32 = [&](const std::vector<uint32_t>& v) { writeU64(v.size()); writeRaw(v.data(), v.size() * 4); };

        writeVec64u(frns);
        writeVec64u(parent_frns);
        writeVec64i(sizes);
        writeVec64i(timestamps);
        writeVec32(name_offsets);
        writeVec32(attributes);

        writeU64(metadata_fetched.size());
        writeRaw(metadata_fetched.data(), metadata_fetched.size());

        writeU64(string_pool.size());
        writeRaw(string_pool.data(), string_pool.size());

        writeVec32(sorted_indices);

        writeU64(usn_map.size());
        for (const auto& [drive, usn] : usn_map) {
            ScchUsnEntry entry{};
            size_t copyLen = (std::min)(drive.size(), sizeof(entry.drive) - 1);
            memcpy(entry.drive, drive.data(), copyLen);
            entry.next_usn = usn;
            writeRaw(&entry, sizeof(entry));
        }

        // 4. 计算 CRC 并填充头部
        uint32_t crc = computeCrc32(base + sizeof(ScchHeader), bodySize);
        ScchHeader* header = reinterpret_cast<ScchHeader*>(base);
        memcpy(header->magic, SCCH_MAGIC, 4);
        header->version_major = SCCH_VERSION_MAJOR;
        header->version_minor = SCCH_VERSION_MINOR;
        header->created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        header->record_count = frns.size();
        header->pool_size = string_pool.size();
        header->usn_map_count = usn_map.size();
        header->sorted_indices_count = sorted_indices.size();
        header->crc32 = crc;
        header->flags = 0;

        // 5. 解除映射
        UnmapViewOfFile(base);
        CloseHandle(hMap);
        CloseHandle(hFile);

        // 6. 原子替换
        std::filesystem::rename(tmpPath, path);
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[ScchCache] save failed: " << e.what() << "\n";
        return false;
    }
}

// ── 加载 (Mmap 优化 - 零拷贝思路) ───────────────────────────────────

ScchResult ScchCache::load(
    const char*                                  path,
    std::vector<uint64_t>&                       frns,
    std::vector<uint64_t>&                       parent_frns,
    std::vector<int64_t>&                        sizes,
    std::vector<int64_t>&                        timestamps,
    std::vector<uint32_t>&                       name_offsets,
    std::vector<uint32_t>&                       attributes,
    std::vector<uint8_t>&                        metadata_fetched,
    std::vector<uint8_t>&                        string_pool,
    std::vector<uint32_t>&                       sorted_indices,
    std::unordered_map<std::string, uint64_t>&   usn_map
) {
    try {
        std::wstring wpath = std::filesystem::path(path).wstring();
        HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return ScchResult::FileNotFound;

        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li)) { CloseHandle(hFile); return ScchResult::IoError; }
        size_t fileSize = static_cast<size_t>(li.QuadPart);
        if (fileSize < sizeof(ScchHeader)) { CloseHandle(hFile); return ScchResult::Truncated; }

        HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMap) { CloseHandle(hFile); return ScchResult::IoError; }

        const uint8_t* base = static_cast<const uint8_t*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
        if (!base) { CloseHandle(hMap); CloseHandle(hFile); return ScchResult::IoError; }

        const ScchHeader* header = reinterpret_cast<const ScchHeader*>(base);
        if (memcmp(header->magic, SCCH_MAGIC, 4) != 0) {
            UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
            return ScchResult::BadMagic;
        }
        if (header->version_major != SCCH_VERSION_MAJOR) {
            UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
            return ScchResult::VersionMismatch;
        }

        size_t bodySize = fileSize - sizeof(ScchHeader);
        if (computeCrc32(base + sizeof(ScchHeader), bodySize) != header->crc32) {
            UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
            return ScchResult::CrcMismatch;
        }

        const uint8_t* ptr = base + sizeof(ScchHeader);
        const uint8_t* end = base + fileSize;

        auto readU64 = [&](uint64_t& v) -> bool {
            if (ptr + 8 > end) return false;
            memcpy(&v, ptr, 8); ptr += 8; return true;
        };

        auto readVec64u = [&](std::vector<uint64_t>& v, uint64_t expected) -> bool {
            uint64_t count = 0;
            if (!readU64(count) || count != expected || ptr + count * 8 > end) return false;
            v.insert(v.end(), reinterpret_cast<const uint64_t*>(ptr), reinterpret_cast<const uint64_t*>(ptr) + count);
            ptr += count * 8; return true;
        };

        auto readVec64i = [&](std::vector<int64_t>& v, uint64_t expected) -> bool {
            uint64_t count = 0;
            if (!readU64(count) || count != expected || ptr + count * 8 > end) return false;
            v.insert(v.end(), reinterpret_cast<const int64_t*>(ptr), reinterpret_cast<const int64_t*>(ptr) + count);
            ptr += count * 8; return true;
        };

        auto readVec32 = [&](std::vector<uint32_t>& v, uint64_t expected) -> bool {
            uint64_t count = 0;
            if (!readU64(count) || count != expected || ptr + count * 4 > end) return false;
            v.insert(v.end(), reinterpret_cast<const uint32_t*>(ptr), reinterpret_cast<const uint32_t*>(ptr) + count);
            ptr += count * 4; return true;
        };

        uint64_t rc = header->record_count;
        if (!readVec64u(frns, rc) || !readVec64u(parent_frns, rc) || !readVec64i(sizes, rc) ||
            !readVec64i(timestamps, rc) || !readVec32(name_offsets, rc) || !readVec32(attributes, rc)) {
            UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
            return ScchResult::Truncated;
        }

        uint64_t fetchedSize = 0;
        if (!readU64(fetchedSize) || fetchedSize != rc || ptr + fetchedSize > end) {
            UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
            return ScchResult::Truncated;
        }
        metadata_fetched.insert(metadata_fetched.end(), ptr, ptr + fetchedSize);
        ptr += fetchedSize;

        uint64_t poolSize = 0;
        if (!readU64(poolSize) || poolSize != header->pool_size || ptr + poolSize > end) {
            UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
            return ScchResult::Truncated;
        }
        string_pool.insert(string_pool.end(), ptr, ptr + poolSize);
        ptr += poolSize;

        if (!readVec32(sorted_indices, header->sorted_indices_count)) {
            UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
            return ScchResult::Truncated;
        }

        uint64_t usnCount = 0;
        if (!readU64(usnCount) || usnCount != header->usn_map_count) {
            UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
            return ScchResult::Truncated;
        }
        usn_map.clear();
        for (uint64_t i = 0; i < usnCount; ++i) {
            if (ptr + sizeof(ScchUsnEntry) > end) {
                UnmapViewOfFile(base); CloseHandle(hMap); CloseHandle(hFile);
                return ScchResult::Truncated;
            }
            ScchUsnEntry entry{};
            memcpy(&entry, ptr, sizeof(entry));
            ptr += sizeof(entry);
            usn_map[std::string(entry.drive)] = entry.next_usn;
        }

        UnmapViewOfFile(base);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return ScchResult::Ok;

    } catch (const std::exception& e) {
        std::cerr << "[ScchCache] load failed: " << e.what() << "\n";
        return ScchResult::IoError;
    }
}

} // namespace FERREX
