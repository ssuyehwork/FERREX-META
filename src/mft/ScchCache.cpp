#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
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
#include <map>

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
        case ScchResult::BadMagic:         return "魔数不匹配";
        case ScchResult::VersionMismatch:  return "版本不兼容";
        case ScchResult::CrcMismatch:      return "CRC 校验失败";
        case ScchResult::Truncated:        return "文件不完整";
        case ScchResult::IoError:          return "I/O 读写错误";
    }
    return "未知错误";
}

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

// 内部辅助：将记录写入缓冲区
static size_t serializeRecord(const ScchDataPackage& pkg, std::vector<uint8_t>& buf) {
    size_t nameLen = pkg.name.size();
    size_t totalLen = sizeof(ScchRecord) + nameLen;
    size_t start = buf.size();
    buf.resize(start + totalLen);
    
    ScchRecord* rec = reinterpret_cast<ScchRecord*>(buf.data() + start);
    rec->frn = pkg.frn;
    rec->parent_frn = pkg.parent_frn;
    rec->size = pkg.size;
    rec->timestamp = pkg.timestamp;
    rec->attributes = pkg.attributes;
    rec->metadata_fetched = pkg.metadata_fetched;
    rec->tombstone = pkg.tombstone;
    rec->name_len = (uint32_t)nameLen;
    
    if (nameLen > 0) {
        memcpy(buf.data() + start + sizeof(ScchRecord), pkg.name.data(), nameLen);
    }
    return totalLen;
}

bool ScchCache::saveAll(const std::string& path_base, const std::vector<ScchDataPackage>& records, uint64_t last_usn) {
    try {
        std::filesystem::path p_bin = path_base + ".bin";
        std::filesystem::path p_idx = path_base + ".idx";
        std::filesystem::path p_bin_tmp = path_base + ".bin.tmp";
        std::filesystem::path p_idx_tmp = path_base + ".idx.tmp";
        std::filesystem::create_directories(p_bin.parent_path());

        // 1. 准备 .bin 数据
        std::vector<uint8_t> bin_data;
        ScchBinHeader bin_header{};
        memcpy(bin_header.magic, SCCH_MAGIC_BIN, 4);
        bin_header.version_major = SCCH_VERSION_MAJOR;
        bin_header.version_minor = SCCH_VERSION_MINOR;
        bin_header.created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        bin_header.total_records = records.size();

        std::vector<ScchIndexEntry> indices;
        indices.reserve(records.size());

        size_t current_offset = sizeof(ScchBinHeader);
        for (const auto& pkg : records) {
            indices.push_back({pkg.frn, current_offset});
            std::vector<uint8_t> rec_buf;
            current_offset += serializeRecord(pkg, rec_buf);
            bin_data.insert(bin_data.end(), rec_buf.begin(), rec_buf.end());
        }

        // 写入 .bin (临时文件)
        FILE* f_bin = fopen(p_bin_tmp.string().c_str(), "wb");
        if (!f_bin) return false;
        fwrite(&bin_header, sizeof(bin_header), 1, f_bin);
        fwrite(bin_data.data(), 1, bin_data.size(), f_bin);
        fclose(f_bin);

        // 2. 准备 .idx 数据
        // 主索引需要按 FRN 排序以便快速加载或二分查找
        std::sort(indices.begin(), indices.end(), [](const ScchIndexEntry& a, const ScchIndexEntry& b) {
            return a.frn < b.frn;
        });

        ScchIdxHeader idx_header{};
        memcpy(idx_header.magic, SCCH_MAGIC_IDX, 4);
        idx_header.version_major = SCCH_VERSION_MAJOR;
        idx_header.version_minor = SCCH_VERSION_MINOR;
        idx_header.main_index_count = indices.size();
        idx_header.delta_index_count = 0;
        idx_header.tombstone_count = 0;
        idx_header.last_usn = last_usn;
        
        idx_header.crc32 = computeCrc32(reinterpret_cast<uint8_t*>(indices.data()), indices.size() * sizeof(ScchIndexEntry));

        FILE* f_idx = fopen(p_idx_tmp.string().c_str(), "wb");
        if (!f_idx) return false;
        fwrite(&idx_header, sizeof(idx_header), 1, f_idx);
        fwrite(indices.data(), sizeof(ScchIndexEntry), indices.size(), f_idx);
        fclose(f_idx);

        // 原子替换
        std::filesystem::rename(p_bin_tmp, p_bin);
        std::filesystem::rename(p_idx_tmp, p_idx);

        return true;
    } catch (...) {
        return false;
    }
}

bool ScchCache::appendEntries(const std::string& path_base, const std::vector<ScchDataPackage>& records, uint64_t last_usn) {
    if (records.empty()) return true;

    std::string bin_path = path_base + ".bin";
    std::string idx_path = path_base + ".idx";

    HANDLE hBin = CreateFileA(bin_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hBin == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER current_bin_size;
    GetFileSizeEx(hBin, &current_bin_size);
    uint64_t offset = current_bin_size.QuadPart;

    std::vector<ScchIndexEntry> delta_entries;
    uint64_t tombstone_inc = 0;

    for (const auto& pkg : records) {
        std::vector<uint8_t> rec_buf;
        size_t len = serializeRecord(pkg, rec_buf);
        DWORD written;
        if (!WriteFile(hBin, rec_buf.data(), (DWORD)len, &written, NULL)) {
            CloseHandle(hBin);
            return false;
        }
        delta_entries.push_back({pkg.frn, offset});
        offset += len;
        if (pkg.tombstone) tombstone_inc++;
    }
    CloseHandle(hBin);

    // 更新 .bin 头部的 total_records
    HANDLE hBinHead = CreateFileA(bin_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hBinHead != INVALID_HANDLE_VALUE) {
        ScchBinHeader head;
        DWORD read;
        if (ReadFile(hBinHead, &head, sizeof(head), &read, NULL) && read == sizeof(head)) {
            head.total_records += records.size();
            SetFilePointer(hBinHead, 0, NULL, FILE_BEGIN);
            DWORD written;
            WriteFile(hBinHead, &head, sizeof(head), &written, NULL);
        }
        CloseHandle(hBinHead);
    }

    // 更新 .idx
    // 增量层直接追加到文件末尾
    HANDLE hIdx = CreateFileA(idx_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hIdx == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(hIdx, delta_entries.data(), (DWORD)(delta_entries.size() * sizeof(ScchIndexEntry)), &written, NULL);
    CloseHandle(hIdx);

    // 更新 .idx 头部
    HANDLE hIdxHead = CreateFileA(idx_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hIdxHead != INVALID_HANDLE_VALUE) {
        ScchIdxHeader head;
        DWORD read;
        if (ReadFile(hIdxHead, &head, sizeof(head), &read, NULL) && read == sizeof(head)) {
            head.delta_index_count += delta_entries.size();
            head.tombstone_count += tombstone_inc;
            head.last_usn = last_usn;
            
            // 重新计算整体 CRC 比较复杂，因为 delta layer 被追加了。
            // 这里简单处理，先不更新 CRC 或只对主索引做 CRC？
            // 按照要求，.idx 分为主索引和 delta layer。
            // 重新读取整个索引部分计算 CRC
            SetFilePointer(hIdxHead, sizeof(ScchIdxHeader), NULL, FILE_BEGIN);
            size_t total_idx_size = (size_t)(head.main_index_count + head.delta_index_count) * sizeof(ScchIndexEntry);
            std::vector<uint8_t> all_indices(total_idx_size);
            if (ReadFile(hIdxHead, all_indices.data(), (DWORD)total_idx_size, &read, NULL) && read == total_idx_size) {
                head.crc32 = computeCrc32(all_indices.data(), total_idx_size);
            }

            SetFilePointer(hIdxHead, 0, NULL, FILE_BEGIN);
            WriteFile(hIdxHead, &head, sizeof(head), &written, NULL);
        }
        CloseHandle(hIdxHead);
    }

    return true;
}

ScchResult ScchCache::load(const std::string& path_base, std::vector<ScchDataPackage>& out_records, uint64_t& out_last_usn) {
    std::string bin_path = path_base + ".bin";
    std::string idx_path = path_base + ".idx";

    if (!std::filesystem::exists(bin_path)) return ScchResult::FileNotFound;

    // 尝试从索引加载
    bool idx_ok = false;
    std::map<uint64_t, uint64_t> frn_to_offset;

    if (std::filesystem::exists(idx_path)) {
        FILE* f_idx = fopen(idx_path.c_str(), "rb");
        if (f_idx) {
            ScchIdxHeader head;
            if (fread(&head, sizeof(head), 1, f_idx) == 1) {
                if (memcmp(head.magic, SCCH_MAGIC_IDX, 4) == 0 && head.version_major == SCCH_VERSION_MAJOR) {
                    size_t total_count = (size_t)(head.main_index_count + head.delta_index_count);
                    std::vector<ScchIndexEntry> entries(total_count);
                    if (fread(entries.data(), sizeof(ScchIndexEntry), total_count, f_idx) == total_count) {
                        if (computeCrc32(reinterpret_cast<uint8_t*>(entries.data()), total_count * sizeof(ScchIndexEntry)) == head.crc32) {
                            for (const auto& e : entries) {
                                frn_to_offset[e.frn] = e.offset;
                            }
                            out_last_usn = head.last_usn;
                            idx_ok = true;
                        }
                    }
                }
            }
            fclose(f_idx);
        }
    }

    FILE* f_bin = fopen(bin_path.c_str(), "rb");
    if (!f_bin) return ScchResult::IoError;

    ScchBinHeader bin_head;
    if (fread(&bin_head, sizeof(bin_head), 1, f_bin) != 1 || memcmp(bin_head.magic, SCCH_MAGIC_BIN, 4) != 0) {
        fclose(f_bin);
        return ScchResult::BadMagic;
    }

    if (idx_ok) {
        for (auto const& [frn, offset] : frn_to_offset) {
            fseek(f_bin, (long)offset, SEEK_SET);
            ScchRecord rec;
            if (fread(&rec, sizeof(rec), 1, f_bin) == 1) {
                if (rec.tombstone) continue;
                ScchDataPackage pkg;
                pkg.frn = rec.frn;
                pkg.parent_frn = rec.parent_frn;
                pkg.size = rec.size;
                pkg.timestamp = rec.timestamp;
                pkg.attributes = rec.attributes;
                pkg.metadata_fetched = rec.metadata_fetched;
                pkg.tombstone = 0;
                if (rec.name_len > 0) {
                    pkg.name.resize(rec.name_len);
                    fread(&pkg.name[0], 1, rec.name_len, f_bin);
                }
                out_records.push_back(std::move(pkg));
            }
        }
    } else {
        // 全量扫描重建
        fseek(f_bin, sizeof(ScchBinHeader), SEEK_SET);
        std::map<uint64_t, ScchDataPackage> rebuild_map;
        while (!feof(f_bin)) {
            ScchRecord rec;
            if (fread(&rec, sizeof(rec), 1, f_bin) != 1) break;
            if (rec.tombstone) {
                rebuild_map.erase(rec.frn);
                if (rec.name_len > 0) fseek(f_bin, rec.name_len, SEEK_CUR);
                continue;
            }
            ScchDataPackage pkg;
            pkg.frn = rec.frn;
            pkg.parent_frn = rec.parent_frn;
            pkg.size = rec.size;
            pkg.timestamp = rec.timestamp;
            pkg.attributes = rec.attributes;
            pkg.metadata_fetched = rec.metadata_fetched;
            pkg.tombstone = 0;
            if (rec.name_len > 0) {
                pkg.name.resize(rec.name_len);
                fread(&pkg.name[0], 1, rec.name_len, f_bin);
            }
            rebuild_map[pkg.frn] = std::move(pkg);
        }
        for (auto& pair : rebuild_map) {
            out_records.push_back(std::move(pair.second));
        }
        out_last_usn = 0; // 扫描重建可能丢失 USN，返回 0 触发全量追平
    }

    fclose(f_bin);
    return ScchResult::Ok;
}

bool ScchCache::needsCompaction(const std::string& path_base, uint32_t delta_threshold, float tombstone_ratio) {
    std::string idx_path = path_base + ".idx";
    if (!std::filesystem::exists(idx_path)) return false;

    FILE* f = fopen(idx_path.c_str(), "rb");
    if (!f) return false;
    ScchIdxHeader head;
    bool needs = false;
    if (fread(&head, sizeof(head), 1, f) == 1) {
        if (head.delta_index_count > delta_threshold) needs = true;
        else if (head.main_index_count > 0 && (float)head.tombstone_count / (float)head.main_index_count > tombstone_ratio) needs = true;
    }
    fclose(f);
    return needs;
}

bool ScchCache::compact(const std::string& path_base) {
    std::vector<ScchDataPackage> records;
    uint64_t last_usn = 0;
    // 2026-06-xx 物理安全性：Compaction 必须读取当前全量状态并写回新文件
    if (load(path_base, records, last_usn) == ScchResult::Ok) {
        // load 逻辑内部已通过 frn_to_offset map 实现了自动去重和 tombstone 过滤
        return saveAll(path_base, records, last_usn);
    }
    return false;
}

} // namespace FERREX
