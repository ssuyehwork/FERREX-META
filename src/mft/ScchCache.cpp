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
#include <unordered_map>
#include <cstddef>

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

    // 2026-06-xx 增强：计算记录级别 CRC
    // 覆盖范围：除 record_crc32 以外的所有字段 + 文件名内容
    std::vector<uint8_t> temp_for_crc;
    temp_for_crc.reserve(offsetof(ScchRecord, record_crc32) + nameLen);
    temp_for_crc.insert(temp_for_crc.end(), reinterpret_cast<const uint8_t*>(rec), reinterpret_cast<const uint8_t*>(rec) + offsetof(ScchRecord, record_crc32));
    if (nameLen > 0) {
        temp_for_crc.insert(temp_for_crc.end(), pkg.name.begin(), pkg.name.end());
    }
    rec->record_crc32 = ScchCache::computeCrc32(temp_for_crc.data(), temp_for_crc.size());

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

            // 2026-06-xx 性能优化：采用“方案 A”，废除增量追加时的全量索引读回重算 CRC。
            // 理由：单条记录已有 CRC 保护，全局 CRC 在增量追加场景下开销为 O(n)，严重拖慢实时更新。
            head.crc32 = 0xDEADDATA; // 使用魔数标记该版本索引不再受全局 CRC 保护

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
    std::vector<ScchIndexEntry> sorted_entries;

    if (std::filesystem::exists(idx_path)) {
        FILE* f_idx = fopen(idx_path.c_str(), "rb");
        if (f_idx) {
            ScchIdxHeader head;
            if (fread(&head, sizeof(head), 1, f_idx) == 1) {
                if (memcmp(head.magic, SCCH_MAGIC_IDX, 4) == 0 && head.version_major == SCCH_VERSION_MAJOR) {
                    size_t total_count = (size_t)(head.main_index_count + head.delta_index_count);
                    std::vector<ScchIndexEntry> raw_entries(total_count);
                    if (fread(raw_entries.data(), sizeof(ScchIndexEntry), total_count, f_idx) == total_count) {
                        // 2026-06-xx 增强：兼容全局 CRC 和 增量魔数标记
                        bool crc_pass = (head.crc32 == 0xDEADDATA) ||
                                       (computeCrc32(reinterpret_cast<uint8_t*>(raw_entries.data()), total_count * sizeof(ScchIndexEntry)) == head.crc32);

                        if (crc_pass) {
                            // 2026-06-xx 性能优化：使用 map 去重（保留最后的 offset）
                            std::unordered_map<uint64_t, uint64_t> frn_to_offset;
                            for (const auto& e : raw_entries) frn_to_offset[e.frn] = e.offset;

                            // 2026-06-xx 核心性能优化：按 offset 排序实现顺序读取，消除磁头随机寻址跳跃
                            sorted_entries.reserve(frn_to_offset.size());
                            for (auto const& [frn, offset] : frn_to_offset) sorted_entries.push_back({frn, offset});
                            std::sort(sorted_entries.begin(), sorted_entries.end(), [](const ScchIndexEntry& a, const ScchIndexEntry& b) {
                                return a.offset < b.offset;
                            });

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
        // 获取文件大小用于边界检查
        fseek(f_bin, 0, SEEK_END);
        long bin_size = ftell(f_bin);

        for (const auto& entry : sorted_entries) {
            fseek(f_bin, (long)entry.offset, SEEK_SET);
            ScchRecord rec;
            if (fread(&rec, sizeof(rec), 1, f_bin) == 1) {
                // 1. 基础检查：FRN 匹配
                if (rec.frn != entry.frn) { idx_ok = false; break; }
                if (rec.tombstone) continue;

                // 2. 边界检查：防止 name_len 异常导致 OOM 或截断读取
                long current_pos = ftell(f_bin);
                long remaining = bin_size - current_pos;
                if (rec.name_len > (uint32_t)remaining || rec.name_len > 1024 * 2) {
                    idx_ok = false; break;
                }

                // 3. 记录级别 CRC 校验
                std::string name;
                if (rec.name_len > 0) {
                    name.resize(rec.name_len);
                    if (fread(&name[0], 1, rec.name_len, f_bin) != rec.name_len) {
                        idx_ok = false; break;
                    }
                }

                std::vector<uint8_t> temp_for_crc;
                temp_for_crc.reserve(offsetof(ScchRecord, record_crc32) + rec.name_len);
                temp_for_crc.insert(temp_for_crc.end(), reinterpret_cast<const uint8_t*>(&rec), reinterpret_cast<const uint8_t*>(&rec) + offsetof(ScchRecord, record_crc32));
                if (rec.name_len > 0) {
                    temp_for_crc.insert(temp_for_crc.end(), name.begin(), name.end());
                }

                if (computeCrc32(temp_for_crc.data(), temp_for_crc.size()) != rec.record_crc32) {
                    idx_ok = false; break;
                }

                // 4. 校验通过，装载数据
                ScchDataPackage pkg;
                pkg.frn = rec.frn;
                pkg.parent_frn = rec.parent_frn;
                pkg.size = rec.size;
                pkg.timestamp = rec.timestamp;
                pkg.attributes = rec.attributes;
                pkg.metadata_fetched = rec.metadata_fetched;
                pkg.tombstone = 0;
                pkg.name = std::move(name);
                out_records.push_back(std::move(pkg));
            } else {
                idx_ok = false; break;
            }
        }

        if (!idx_ok) {
            out_records.clear();
            // 如果索引加载过程中失败，则后续会进入全量扫描降级路径
        }
    }

    if (!idx_ok) {
        // 全量扫描重建
        fseek(f_bin, 0, SEEK_END);
        long bin_size = ftell(f_bin);
        fseek(f_bin, sizeof(ScchBinHeader), SEEK_SET);

        std::map<uint64_t, ScchDataPackage> rebuild_map;
        while (ftell(f_bin) < bin_size) {
            ScchRecord rec;
            if (fread(&rec, sizeof(rec), 1, f_bin) != 1) break;

            // 健壮性检查：边界与阈值
            if (rec.name_len > 1024 * 2 || ftell(f_bin) + rec.name_len > bin_size) {
                break; // 停止扫描
            }

            std::string name;
            if (rec.name_len > 0) {
                name.resize(rec.name_len);
                if (fread(&name[0], 1, rec.name_len, f_bin) != rec.name_len) break;
            }

            // CRC 校验
            std::vector<uint8_t> temp_for_crc;
            temp_for_crc.reserve(offsetof(ScchRecord, record_crc32) + rec.name_len);
            temp_for_crc.insert(temp_for_crc.end(), reinterpret_cast<const uint8_t*>(&rec), reinterpret_cast<const uint8_t*>(&rec) + offsetof(ScchRecord, record_crc32));
            if (rec.name_len > 0) {
                temp_for_crc.insert(temp_for_crc.end(), name.begin(), name.end());
            }

            if (computeCrc32(temp_for_crc.data(), temp_for_crc.size()) == rec.record_crc32) {
                if (rec.tombstone) {
                    rebuild_map.erase(rec.frn);
                } else {
                    ScchDataPackage pkg;
                    pkg.frn = rec.frn;
                    pkg.parent_frn = rec.parent_frn;
                    pkg.size = rec.size;
                    pkg.timestamp = rec.timestamp;
                    pkg.attributes = rec.attributes;
                    pkg.metadata_fetched = rec.metadata_fetched;
                    pkg.tombstone = 0;
                    pkg.name = std::move(name);
                    rebuild_map[pkg.frn] = std::move(pkg);
                }
            }
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
