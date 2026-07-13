#include "DiskIndexCacheCoordinator.h"
#include "MftReader.h"
#include "UsnWatcher.h"
#include "ScchCache.h"
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <QDebug>
#include <QWriteLocker>
#include <QReadLocker>
#include <QThreadPool>

namespace FERREX {

static void splitNameAndExt(const std::string& fullName, std::string& outExt) {
    outExt.clear();
    size_t lastDot = fullName.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        outExt = fullName.substr(lastDot + 1);
        std::transform(outExt.begin(), outExt.end(), outExt.begin(), ::tolower);
    }
}

bool DiskIndexCacheCoordinator::loadFromCache(MftReader* reader) {
    if (!reader) return false;

    std::filesystem::path cacheDir = "FERREX/cache";
    if (!std::filesystem::exists(cacheDir)) return false;

    // 2026-06-xx 物理修复：载入缓存前必须执行全量清理（含停止旧监听器），杜绝资源泄露与逻辑重叠
    reader->clear(); 

    std::unordered_set<std::string> loadedBases;
    for (auto const& entry : std::filesystem::directory_iterator{cacheDir}) {
        std::string baseExt = entry.path().extension().string();
        if (baseExt == ".bin" || baseExt == ".idx") {
            std::string stem = entry.path().stem().string();
            if (loadedBases.count(stem)) continue;
            loadedBases.insert(stem);

            std::string path_base = (cacheDir / stem).string();
            std::vector<ScchDataPackage> records;
            uint64_t lastUsn = 0;

            if (ScchCache::load(path_base, records, lastUsn) == ScchResult::Ok) {
                std::wstring driveName = QString::fromStdString(stem + ":").toStdWString();
                
                // 2026-07-28 优化：记录解析循环移出写锁范围
                struct PreparedData {
                    std::vector<uint64_t> frns;
                    std::vector<uint64_t> parent_frns;
                    std::vector<int64_t>  sizes;
                    std::vector<int64_t>  timestamps;
                    std::vector<uint32_t> attributes;
                    std::vector<uint8_t>  metadata_fetched;
                    std::vector<uint8_t>  string_pool;
                    std::vector<uint32_t> name_offsets;
                    std::vector<uint32_t> ext_offsets;
                } pd;

                pd.frns.reserve(records.size());
                pd.parent_frns.reserve(records.size());
                pd.sizes.reserve(records.size());
                pd.timestamps.reserve(records.size());
                pd.attributes.reserve(records.size());
                pd.metadata_fetched.reserve(records.size());
                pd.name_offsets.reserve(records.size());
                pd.ext_offsets.reserve(records.size());

                for (const auto& pkg : records) {
                    pd.frns.push_back(pkg.frn);
                    pd.parent_frns.push_back(pkg.parent_frn & 0x0000FFFFFFFFFFFFull);
                    pd.sizes.push_back(pkg.size);
                    pd.timestamps.push_back(pkg.timestamp);
                    pd.attributes.push_back(pkg.attributes);
                    
                    // 2026-07-11 极致性能重构：
                    // 彻底消除由于早期默认状态标记 0，导致列表模式渲染被迫抛弃缓存、频繁对数万个文件重新发起 Windows API 物理磁盘 IO 的架构缺陷。
                    if (pkg.size > 0 || pkg.timestamp > 0) {
                        pd.metadata_fetched.push_back(2);
                    } else {
                        pd.metadata_fetched.push_back(pkg.metadata_fetched);
                    }
                    
                    pd.name_offsets.push_back((uint32_t)pd.string_pool.size());
                    pd.string_pool.insert(pd.string_pool.end(), pkg.name.begin(), pkg.name.end());
                    pd.string_pool.push_back('\0');

                    std::string extStr;
                    splitNameAndExt(pkg.name, extStr);
                    pd.ext_offsets.push_back((uint32_t)pd.string_pool.size());
                    pd.string_pool.insert(pd.string_pool.end(), extStr.begin(), extStr.end());
                    pd.string_pool.push_back('\0');
                }

                size_t dIdx;
                size_t currentTotal;
                {
                    QWriteLocker lock(&reader->m_dataLock);
                    dIdx = reader->m_drive_list.size();
                    reader->m_drive_list.push_back(driveName);
                    reader->m_next_usns[driveName] = lastUsn;

                    uint32_t poolBase = (uint32_t)reader->m_string_pool.size();
                    size_t startIdx = reader->m_frns.size();
                    
                    reader->m_frns.insert(reader->m_frns.end(), pd.frns.begin(), pd.frns.end());
                    reader->m_sizes.insert(reader->m_sizes.end(), pd.sizes.begin(), pd.sizes.end());
                    reader->m_timestamps.insert(reader->m_timestamps.end(), pd.timestamps.begin(), pd.timestamps.end());
                    reader->m_attributes.insert(reader->m_attributes.end(), pd.attributes.begin(), pd.attributes.end());
                    reader->m_metadata_fetched.insert(reader->m_metadata_fetched.end(), pd.metadata_fetched.begin(), pd.metadata_fetched.end());
                    
                    for (size_t i = 0; i < pd.frns.size(); ++i) {
                        reader->m_parent_frns.push_back((static_cast<uint64_t>(dIdx) << 48) | pd.parent_frns[i]);
                        reader->m_name_offsets.push_back(poolBase + pd.name_offsets[i]);
                        reader->m_ext_offsets.push_back(poolBase + pd.ext_offsets[i]);
                        reader->m_parent_indices.push_back(0xFFFFFFFF);
                        
                        uint64_t key = MftReader::makeKey(dIdx, pd.frns[i]);
                        auto it = reader->m_frn_to_idx.find(key);
                        if (it != reader->m_frn_to_idx.end()) {
                            reader->m_frns[it->second] = 0;
                            reader->m_dead_count++;
                        }
                        reader->m_frn_to_idx[key] = (uint32_t)(startIdx + i);
                    }
                    reader->m_string_pool.insert(reader->m_string_pool.end(), pd.string_pool.begin(), pd.string_pool.end());
                    currentTotal = reader->m_frns.size();
                }
                emit reader->driveLoaded(QString::fromStdWString(driveName), (int)records.size(), (int)currentTotal);
            }
        }
    }

    QWriteLocker lock(&reader->m_dataLock);
    if (reader->m_frns.empty()) return false;
    
    // 2026-07-28 极致修复：锁内仅执行增量映射补全
    for (size_t i = 0; i < reader->m_frns.size(); ++i) {
        if (reader->m_parent_indices[i] != 0xFFFFFFFF) continue;
        uint64_t encodedPf = reader->m_parent_frns[i];
        auto itP = reader->m_frn_to_idx.find(encodedPf);
        if (itP != reader->m_frn_to_idx.end()) reader->m_parent_indices[i] = itP->second;
    }

    reader->m_isInitialized = true;
    lock.unlock();

    reader->compact(); 
    reader->buildSortedIndices();

    // 2026-06-xx 核心修复：加载缓存后立即启动 USN 监听器
    std::vector<UsnWatcher*> newWatchers;
    for (const auto& drive : reader->m_drive_list) {
        if (reader->m_watcher_map.count(drive)) continue; 
        uint64_t lastUsn = reader->m_next_usns.count(drive) ? reader->m_next_usns[drive] : 0;
        auto* w = new UsnWatcher(drive, lastUsn, nullptr);
        reader->m_watcher_map[drive] = w;
        newWatchers.push_back(w);
        qDebug() << "[MftReader] 从快照恢复监听驱动器:" << QString::fromStdWString(drive) << "起始 USN:" << lastUsn;
    }
    
    for (auto* w : newWatchers) w->start();

    return true;
}

bool DiskIndexCacheCoordinator::loadDriveFromCache(MftReader* reader, const QString& drive) {
    if (!reader) return false;

    std::wstring vol = drive.toStdWString();
    if (vol.size() > 1 && (vol.back() == L'\\' || vol.back() == L'/')) vol.pop_back();

    {
        QReadLocker lock(&reader->m_dataLock);
        for (const auto& d : reader->m_drive_list) if (_wcsicmp(d.c_str(), vol.c_str()) == 0) return true;
    }

    std::filesystem::path cacheDir = "FERREX/cache";
    std::string stem = drive.left(1).toUpper().toStdString();
    std::string path_base = (cacheDir / stem).string();

    std::vector<ScchDataPackage> records;
    uint64_t lastUsn = 0;

    if (ScchCache::load(path_base, records, lastUsn) != ScchResult::Ok) return false;

    struct PreparedData {
        std::vector<uint64_t> frns;
        std::vector<uint64_t> parent_frns;
        std::vector<int64_t>  sizes;
        std::vector<int64_t>  timestamps;
        std::vector<uint32_t> attributes;
        std::vector<uint8_t>  metadata_fetched;
        std::vector<uint8_t>  string_pool;
        std::vector<uint32_t> name_offsets;
        std::vector<uint32_t> ext_offsets;
    } pd;

    pd.frns.reserve(records.size());
    pd.parent_frns.reserve(records.size());
    pd.sizes.reserve(records.size());
    pd.timestamps.reserve(records.size());
    pd.attributes.reserve(records.size());
    pd.metadata_fetched.reserve(records.size());
    pd.name_offsets.reserve(records.size());
    pd.ext_offsets.reserve(records.size());

    for (const auto& pkg : records) {
        pd.frns.push_back(pkg.frn);
        pd.parent_frns.push_back(pkg.parent_frn & 0x0000FFFFFFFFFFFFull);
        pd.sizes.push_back(pkg.size);
        pd.timestamps.push_back(pkg.timestamp);
        pd.attributes.push_back(pkg.attributes);
        
        if (pkg.size > 0 || pkg.timestamp > 0) {
            pd.metadata_fetched.push_back(2);
        } else {
            pd.metadata_fetched.push_back(pkg.metadata_fetched);
        }

        pd.name_offsets.push_back((uint32_t)pd.string_pool.size());
        pd.string_pool.insert(pd.string_pool.end(), pkg.name.begin(), pkg.name.end());
        pd.string_pool.push_back('\0');

        std::string extStr;
        splitNameAndExt(pkg.name, extStr);
        pd.ext_offsets.push_back((uint32_t)pd.string_pool.size());
        pd.string_pool.insert(pd.string_pool.end(), extStr.begin(), extStr.end());
        pd.string_pool.push_back('\0');
    }

    QWriteLocker lock(&reader->m_dataLock);
    size_t dIdx = reader->m_drive_list.size();
    reader->m_drive_list.push_back(vol);
    if (dIdx < 32) reader->m_drive_active_mask.fetch_or(1 << dIdx);
    reader->m_next_usns[vol] = lastUsn;

    uint32_t poolBase = (uint32_t)reader->m_string_pool.size();
    size_t startIdx = reader->m_frns.size();
    
    reader->m_frns.insert(reader->m_frns.end(), pd.frns.begin(), pd.frns.end());
    reader->m_sizes.insert(reader->m_sizes.end(), pd.sizes.begin(), pd.sizes.end());
    reader->m_timestamps.insert(reader->m_timestamps.end(), pd.timestamps.begin(), pd.timestamps.end());
    reader->m_attributes.insert(reader->m_attributes.end(), pd.attributes.begin(), pd.attributes.end());
    reader->m_metadata_fetched.insert(reader->m_metadata_fetched.end(), pd.metadata_fetched.begin(), pd.metadata_fetched.end());
    
    for (size_t i = 0; i < pd.frns.size(); ++i) {
        reader->m_parent_frns.push_back((static_cast<uint64_t>(dIdx) << 48) | pd.parent_frns[i]);
        reader->m_name_offsets.push_back(poolBase + pd.name_offsets[i]);
        reader->m_ext_offsets.push_back(poolBase + pd.ext_offsets[i]);
        reader->m_parent_indices.push_back(0xFFFFFFFF);
        
        uint64_t key = MftReader::makeKey(dIdx, pd.frns[i]);
        auto it = reader->m_frn_to_idx.find(key);
        if (it != reader->m_frn_to_idx.end()) {
            reader->m_frns[it->second] = 0;
            reader->m_dead_count++;
        }
        reader->m_frn_to_idx[key] = (uint32_t)(startIdx + i);
    }
    reader->m_string_pool.insert(reader->m_string_pool.end(), pd.string_pool.begin(), pd.string_pool.end());

    // 2026-07-28 极致修复：锁内仅执行增量映射补全
    for (size_t i = startIdx; i < reader->m_frns.size(); ++i) {
        uint64_t encodedPf = reader->m_parent_frns[i];
        auto itP = reader->m_frn_to_idx.find(encodedPf);
        if (itP != reader->m_frn_to_idx.end()) reader->m_parent_indices[i] = itP->second;
    }

    reader->m_isInitialized = true;
    auto* w = new UsnWatcher(vol, lastUsn, nullptr);
    reader->m_watcher_map[vol] = w;
    lock.unlock();

    reader->compact(); 
    reader->buildSortedIndices();
    w->start();

    return true;
}

bool DiskIndexCacheCoordinator::saveToCache(MftReader* reader) {
    if (!reader) return false;

    QReadLocker lock(&reader->m_dataLock);
    if (!reader->m_isInitialized) return false;
    for (size_t i = 0; i < reader->m_drive_list.size(); ++i) {
        saveDriveToCacheInternal(reader, i);
    }
    return true;
}

bool DiskIndexCacheCoordinator::saveDriveToCache(MftReader* reader, size_t driveIdx) {
    if (!reader) return false;
    return saveDriveToCacheInternal(reader, driveIdx);
}

bool DiskIndexCacheCoordinator::saveDriveToCacheInternal(MftReader* reader, size_t driveIdx) {
    if (!reader) return false;

    std::vector<ScchDataPackage> dirtyData;
    uint64_t lastUsn = 0;
    bool isFullSave = false;
    std::wstring volume;

    {
        QReadLocker lock(&reader->m_dataLock);
        if (driveIdx >= reader->m_drive_list.size()) return false;
        volume = reader->m_drive_list[driveIdx];
        lastUsn = reader->m_next_usns[volume];

        std::lock_guard<std::mutex> dLock(reader->m_dirtyLock);
        isFullSave = !reader->m_drive_ever_saved[driveIdx];
        
        if (isFullSave) {
            for (size_t i = 0; i < reader->m_frns.size(); ++i) {
                if (reader->m_frns[i] != 0 && (reader->m_parent_frns[i] >> 48) == driveIdx) {
                    ScchDataPackage pkg;
                    pkg.frn = reader->m_frns[i];
                    pkg.parent_frn = reader->m_parent_frns[i] & 0x0000FFFFFFFFFFFFull;
                    pkg.size = reader->m_sizes[i];
                    pkg.timestamp = reader->m_timestamps[i];
                    pkg.attributes = reader->m_attributes[i];
                    pkg.metadata_fetched = reader->m_metadata_fetched[i];
                    pkg.tombstone = 0;
                    pkg.name = reinterpret_cast<const char*>(reader->m_string_pool.data() + reader->m_name_offsets[i]);
                    dirtyData.push_back(std::move(pkg));
                }
            }
        } else {
            auto it = reader->m_dirty_indices.begin();
            while (it != reader->m_dirty_indices.end()) {
                uint32_t idx = *it;
                if (idx < reader->m_frns.size() && (reader->m_parent_frns[idx] >> 48) == driveIdx) {
                    ScchDataPackage pkg;
                    if (reader->m_frns[idx] == 0) {
                        pkg.frn = reader->m_dead_frns[idx];
                        pkg.tombstone = 1;
                        reader->m_dead_frns.erase(idx);
                    } else {
                        pkg.frn = reader->m_frns[idx];
                        pkg.tombstone = 0;
                    }
                    pkg.parent_frn = reader->m_parent_frns[idx] & 0x0000FFFFFFFFFFFFull;
                    pkg.size = reader->m_sizes[idx];
                    pkg.timestamp = reader->m_timestamps[idx];
                    pkg.attributes = reader->m_attributes[idx];
                    pkg.metadata_fetched = reader->m_metadata_fetched[idx];
                    pkg.name = pkg.tombstone ? "" : reinterpret_cast<const char*>(reader->m_string_pool.data() + reader->m_name_offsets[idx]);
                    dirtyData.push_back(std::move(pkg));
                    it = reader->m_dirty_indices.erase(it);
                } else {
                    ++it;
                }
            }
        }
    } 

    std::string path_base = "FERREX/cache/" + QString::fromStdWString(volume).left(1).toStdString();
    
    if (!isFullSave) {
        if (!std::filesystem::exists(path_base + ".bin")) {
            isFullSave = true;
            dirtyData.clear();
            {
                QReadLocker lock(&reader->m_dataLock);
                for (size_t i = 0; i < reader->m_frns.size(); ++i) {
                    if (reader->m_frns[i] != 0 && (reader->m_parent_frns[i] >> 48) == driveIdx) {
                        ScchDataPackage pkg;
                        pkg.frn = reader->m_frns[i];
                        pkg.parent_frn = reader->m_parent_frns[i] & 0x0000FFFFFFFFFFFFull;
                        pkg.size = reader->m_sizes[i];
                        pkg.timestamp = reader->m_timestamps[i];
                        pkg.attributes = reader->m_attributes[i];
                        pkg.metadata_fetched = reader->m_metadata_fetched[i];
                        pkg.tombstone = 0;
                        pkg.name = reinterpret_cast<const char*>(reader->m_string_pool.data() + reader->m_name_offsets[i]);
                        dirtyData.push_back(std::move(pkg));
                    }
                }
            }
        }
    }

    if (isFullSave) {
        bool ok = ScchCache::saveAll(path_base, dirtyData, lastUsn);
        if (ok) {
            std::lock_guard<std::mutex> dLock(reader->m_dirtyLock);
            reader->m_drive_ever_saved[driveIdx] = true;
        }
        return ok;
    } else {
        bool ok = ScchCache::appendEntries(path_base, dirtyData, lastUsn);
        if (ScchCache::needsCompaction(path_base)) {
            {
                QWriteLocker lock(&reader->m_dataLock);
                reader->m_is_compacting[driveIdx] = true;
            }
            QThreadPool::globalInstance()->start([reader, path_base, driveIdx]() {
                ScchCache::compact(path_base);
                std::vector<ScchDataPackage> buffered;
                uint64_t finalUsn = 0;
                {
                    QWriteLocker lock(&reader->m_dataLock);
                    buffered = std::move(reader->m_compaction_buffer[driveIdx]);
                    reader->m_is_compacting[driveIdx] = false;
                    finalUsn = reader->m_next_usns[reader->m_drive_list[driveIdx]];
                }
                if (!buffered.empty()) ScchCache::appendEntries(path_base, buffered, finalUsn);
            });
        }
        return ok;
    }
}

bool DiskIndexCacheCoordinator::saveDriveToCacheUnlocked(MftReader* reader, size_t driveIdx) {
    if (!reader) return false;

    QThreadPool::globalInstance()->start([reader, driveIdx]() {
        if (!saveDriveToCacheInternal(reader, driveIdx)) {
            QString letter = "?:";
            {
                QReadLocker lock(&reader->m_dataLock);
                if (driveIdx < reader->m_drive_list.size()) {
                    letter = QString::fromStdWString(reader->m_drive_list[driveIdx]);
                }
            }
            qWarning() << "[MftReader] 后台异步落盘失败! 盘符:" << letter << " driveIdx:" << driveIdx;
        }
    });
    return true;
}

} // namespace FERREX
