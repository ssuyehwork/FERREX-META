#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MftReader.h"
#include "UsnWatcher.h"
#include <winioctl.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#include <algorithm>
#include <mutex>
#include <thread>
#include <numeric>
#include <filesystem>
#include <QDebug>
#include <QRegularExpression>
#include <QDir>
#include <QDateTime>
#include <QtConcurrent/QtConcurrent>
#include <QtConcurrent>
#include <QFuture>
#include <QThreadPool>
#include <QFileIconProvider>
#include <QFileInfo>

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

// 2026-06-xx 性能优化：实现扩展名预拆分。
// 规则：取最后一个 "." 之后的字符串并转小写；若无 "." 或 "." 为首字符，则 ext 为空。
static void splitNameAndExt(const std::string& fullName, std::string& outExt) {
    outExt.clear();
    size_t lastDot = fullName.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        outExt = fullName.substr(lastDot + 1);
        std::transform(outExt.begin(), outExt.end(), outExt.begin(), ::tolower);
    }
}

static int64_t filetimeToUnixMs(int64_t filetime) {
    // 2026-05-14 物理对标 Windows FILETIME 标准 (1601 Epoch to 1970 Unix)
    // 116444736000000000LL 是 1601 到 1970 的 100纳秒数
    // 如果时间戳小于 1970 或等于 0，则返回 0 以便 UI 能够正确忽略或显示占位符
    if (filetime <= 116444736000000000LL) return 0;
    // 10000LL 将 100纳秒 转换为 毫秒 (1ms = 10,000 * 100ns)
    return (filetime - 116444736000000000LL) / 10000LL;
}

static bool enablePrivilege(LPCWSTR privilege) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return false;
    LUID luid;
    if (!LookupPrivilegeValue(NULL, privilege, &luid)) { CloseHandle(hToken); return false; }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) { CloseHandle(hToken); return false; }
    bool ok = (GetLastError() == ERROR_SUCCESS);
    CloseHandle(hToken);
    return ok;
}

MftReader& MftReader::instance() {
    static MftReader inst;
    static std::once_flag flag;
    std::call_once(flag, []() {
        enablePrivilege(SE_BACKUP_NAME);
        enablePrivilege(SE_RESTORE_NAME);
    });
    return inst;
}

MftReader::MftReader() {
    clearInternal();
    m_metadataPool = new QThreadPool(this);
    m_metadataPool->setMaxThreadCount(2); // 限制补全线程数，避免对磁盘造成地毯式寻址冲击
    m_notifyTimer = new QTimer(this);
    m_notifyTimer->setInterval(150); // 150ms 聚合通知
    connect(m_notifyTimer, &QTimer::timeout, this, [this]() {
        emit entriesChangedBatch();
    });
}

MftReader::~MftReader() {
    clear();
}

void MftReader::clearInternal() {
    m_frns.clear();
    m_parent_frns.clear();
    m_parent_indices.clear();
    m_sizes.clear();
    m_timestamps.clear();
    m_name_offsets.clear();
    m_ext_offsets.clear();
    m_attributes.clear();
    m_metadata_fetched.clear();
    m_string_pool.clear();
    m_drive_list.clear();
    m_drive_active_mask = 0;
    m_frn_to_idx.clear();
    m_sorted_indices.clear();
    {
        std::lock_guard<std::mutex> lock(m_pathCacheMutex);
        m_path_cache.clear();
    }
    {
        QWriteLocker lock(&m_iconCacheLock);
        m_icon_cache.clear();
    }
    m_next_usns.clear();
    m_isInitialized = false;
    m_dirty_count = 0;
    m_dead_count = 0;
    m_wasted_string_bytes = 0;
}

void MftReader::clear() {
    // 2026-06-xx 生命周期强化：立即设置停止位，解除所有长循环阻塞
    m_isStopping.store(true);

    if (m_metadataPool) {
        m_metadataPool->waitForDone();
    }

    std::vector<UsnWatcher*> toStop;
    {
        QWriteLocker lock(&m_dataLock);
        for (auto it = m_watcher_map.begin(); it != m_watcher_map.end(); ++it) {
            toStop.push_back(it->second);
        }
        m_watcher_map.clear();
    }
    for (auto* w : toStop) { if (w) { w->stop(); delete w; } }
    
    QWriteLocker lock(&m_dataLock);
    clearInternal();
    
    // 清理任务队列
    {
        std::lock_guard<std::mutex> journalLock(m_journalMutex);
        m_changeJournal.clear();
    }
    if (m_notifyTimer) m_notifyTimer->stop();

    m_isStopping.store(false); // 重置状态，准备下一次初始化
}

void MftReader::updateActiveDrives(const QStringList& activeDrives) {
    // 2026-05-14 核心修正：使用原子掩码替代 QWriteLocker，消除 UI 线程同步死锁风险
    uint32_t mask = 0;
    QReadLocker lock(&m_dataLock);
    for (const QString& d : activeDrives) {
        std::wstring vol = d.toStdWString();
        if (vol.size() > 1 && (vol.back() == L'\\' || vol.back() == L'/')) vol.pop_back();
        for (size_t i = 0; i < m_drive_list.size(); ++i) {
            if (_wcsicmp(m_drive_list[i].c_str(), vol.c_str()) == 0) {
                mask |= (1 << i);
                break;
            }
        }
    }
    m_drive_active_mask.store(mask, std::memory_order_relaxed);
}

bool MftReader::isDriveIndexed(const QString& drive) {
    std::wstring vol = drive.toStdWString();
    if (vol.size() > 1 && (vol.back() == L'\\' || vol.back() == L'/')) vol.pop_back();
    
    QReadLocker lock(&m_dataLock);
    for (const auto& indexedVol : m_drive_list) {
        if (_wcsicmp(indexedVol.c_str(), vol.c_str()) == 0) return true;
    }
    return false;
}

void MftReader::buildIndex(const QStringList& drives) {
    updateActiveDrives(drives);

    std::vector<std::wstring> toScan;
    {
        QReadLocker lock(&m_dataLock);
        for (const QString& d : drives) {
            std::wstring vol = d.toStdWString();
            if (vol.size() > 1 && (vol.back() == L'\\' || vol.back() == L'/')) vol.pop_back();
            
            bool found = false;
            for (const auto& indexedVol : m_drive_list) {
                if (_wcsicmp(indexedVol.c_str(), vol.c_str()) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                toScan.push_back(vol);
            }
        }
    }

    if (toScan.empty()) {
        // 如果没有新盘需要扫描，且已经初始化，则不需要重建索引
        QReadLocker lock(&m_dataLock);
        if (m_isInitialized) return;
    }

    struct ScannedDrive {
        std::wstring volume;
        MftReader::DriveResult res; 
        bool success = false;
    };
    std::vector<ScannedDrive> scannedResults(toScan.size());
    // 2026-06-xx 性能策略：使用 QtConcurrent 索引映射实现多驱动器并行扫描
    // 理由：避免 blockingMap 内部嵌套循环查找，将对齐复杂度由 O(N^2) 降至 O(N)
    std::vector<int> taskIndices((int)toScan.size());
    std::iota(taskIndices.begin(), taskIndices.end(), 0);
    
    QtConcurrent::blockingMap(taskIndices.begin(), taskIndices.end(), [&](int i) {
        if (m_isStopping.load()) return;
        scannedResults[i].volume = toScan[i];
        scannedResults[i].success = loadMftDirect(toScan[i], scannedResults[i].res);
    });

    if (m_isStopping.load()) return;

    QWriteLocker lock(&m_dataLock);
    std::vector<UsnWatcher*> newWatchers;
    for (auto& sr : scannedResults) {
        if (!sr.success || sr.res.entries.empty()) {
            qWarning() << "[MftReader] 跳过驱动器扫描 (结果为空或扫描失败):" << QString::fromStdWString(sr.volume) << " success:" << sr.success;
            continue;
        }
        
        // 2026-07-07 物理修复：优先复用空置槽位 (Analysis_Modification_Plan-154.md)
        size_t dIdx = (size_t)-1;
        for (size_t i = 0; i < m_drive_list.size(); ++i) {
            if (m_drive_list[i].empty()) { dIdx = i; m_drive_list[i] = sr.volume; break; }
        }
        if (dIdx == (size_t)-1) {
            dIdx = m_drive_list.size();
            m_drive_list.push_back(sr.volume);
        }

        if (dIdx < 32) m_drive_active_mask.fetch_or(1 << dIdx);
        m_next_usns[sr.volume] = sr.res.nextUsn;
        mergeDriveResult(sr.volume, sr.res, dIdx);
        // 2026-06-xx 物理修复：在持有写锁时调用 Unlocked 版本，解除递归锁自杀式死锁
        saveDriveToCacheUnlocked(dIdx);
        
        auto* w = new UsnWatcher(sr.volume, sr.res.nextUsn, nullptr);
        m_watcher_map[sr.volume] = w;
        newWatchers.push_back(w);
    }

    rebuildFrnToIndexMap();
    buildSortedIndices();
    m_isInitialized = true;

    lock.unlock();
    for (auto* w : newWatchers) w->start();
}

bool MftReader::loadFromCache() {
    std::filesystem::path cacheDir = "FERREX/cache";
    if (!std::filesystem::exists(cacheDir)) return false;

    // 2026-06-xx 物理修复：载入缓存前必须执行全量清理（含停止旧监听器），杜绝资源泄露与逻辑重叠
    clear(); 

    struct DriveIndices {
        std::vector<uint32_t> sorted;
        uint32_t baseIdx;
    };
    std::vector<DriveIndices> allSortedIndices;

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
                size_t dIdx;
                size_t count = records.size();
                size_t currentTotal;
                std::wstring driveName = QString::fromStdString(stem + ":").toStdWString(); 

                {
                    QWriteLocker lock(&m_dataLock);
                    dIdx = m_drive_list.size();
                    m_drive_list.push_back(driveName);
                    m_next_usns[driveName] = lastUsn;

                    for (const auto& pkg : records) {
                        uint32_t idx = (uint32_t)m_frns.size();
                        m_frn_to_idx[makeKey(dIdx, pkg.frn)] = idx;
                        
                        m_frns.push_back(pkg.frn);
                        m_parent_frns.push_back((static_cast<uint64_t>(dIdx) << 48) | (pkg.parent_frn & 0x0000FFFFFFFFFFFFull));
                        m_parent_indices.push_back(0xFFFFFFFF);
                        m_sizes.push_back(pkg.size);
                        m_timestamps.push_back(pkg.timestamp);
                        m_attributes.push_back(pkg.attributes);
                        m_metadata_fetched.push_back(pkg.metadata_fetched);
                        
                        m_name_offsets.push_back((uint32_t)m_string_pool.size());
                        m_string_pool.insert(m_string_pool.end(), pkg.name.begin(), pkg.name.end());
                        m_string_pool.push_back('\0');

                        // 2026-06-xx 物理对标：快照加载时同步预拆分扩展名
                        std::string extStr;
                        splitNameAndExt(pkg.name, extStr);
                        m_ext_offsets.push_back((uint32_t)m_string_pool.size());
                        m_string_pool.insert(m_string_pool.end(), extStr.begin(), extStr.end());
                        m_string_pool.push_back('\0');
                    }
                    currentTotal = m_frns.size();
                }
                emit driveLoaded(QString::fromStdWString(driveName), (int)count, (int)currentTotal);
            }
        }
    }

    QWriteLocker lock(&m_dataLock);
    if (m_frns.empty()) return false;
    
    // 2026-06-xx 物理补齐：缓存加载后必须执行重映射，以补全 m_parent_indices 链条
    rebuildFrnToIndexMap(); 

    // 2026-07-07 物理修复：物理回收重复项空间 (Analysis_Modification_Plan-154.md)
    compact();

    buildSortedIndices();

    m_isInitialized = true;

    // 2026-06-xx 核心修复：加载缓存后立即启动 USN 监听器
    // 理由：确保系统在从快照恢复后，能通过 USN 锚点自动追平离线期间的磁盘变动，并开始实时监听。
    std::vector<UsnWatcher*> newWatchers;
    for (const auto& drive : m_drive_list) {
        if (m_watcher_map.count(drive)) continue; // 2026-07-07 物理防御：防止重复启动监听
        uint64_t lastUsn = m_next_usns.count(drive) ? m_next_usns[drive] : 0;
        auto* w = new UsnWatcher(drive, lastUsn, nullptr);
        m_watcher_map[drive] = w;
        newWatchers.push_back(w);
        qDebug() << "[MftReader] 从快照恢复监听驱动器:" << QString::fromStdWString(drive) << "起始 USN:" << lastUsn;
    }
    
    // 释放数据锁后启动线程，防止死锁
    lock.unlock();
    for (auto* w : newWatchers) w->start();

    return true;
}

bool MftReader::loadDriveFromCache(const QString& drive) {
    std::wstring vol = drive.toStdWString();
    if (vol.size() > 1 && (vol.back() == L'\\' || vol.back() == L'/')) vol.pop_back();

    {
        QReadLocker lock(&m_dataLock);
        for (const auto& d : m_drive_list) if (_wcsicmp(d.c_str(), vol.c_str()) == 0) return true;
    }

    std::filesystem::path cacheDir = "FERREX/cache";
    std::string stem = drive.left(1).toUpper().toStdString();
    std::string path_base = (cacheDir / stem).string();

    std::vector<ScchDataPackage> records;
    uint64_t lastUsn = 0;

    if (ScchCache::load(path_base, records, lastUsn) != ScchResult::Ok) return false;

    QWriteLocker lock(&m_dataLock);
    // 2026-07-07 物理修复：优先复用空置槽位 (Analysis_Modification_Plan-154.md)
    size_t dIdx = (size_t)-1;
    for (size_t i = 0; i < m_drive_list.size(); ++i) {
        if (m_drive_list[i].empty()) { dIdx = i; m_drive_list[i] = vol; break; }
    }
    if (dIdx == (size_t)-1) {
        dIdx = m_drive_list.size();
        m_drive_list.push_back(vol);
    }

    if (dIdx < 32) m_drive_active_mask.fetch_or(1 << dIdx);
    m_next_usns[vol] = lastUsn;

    for (const auto& pkg : records) {
        uint32_t idx = (uint32_t)m_frns.size();
        m_frn_to_idx[makeKey(dIdx, pkg.frn)] = idx;
        
        m_frns.push_back(pkg.frn);
        m_parent_frns.push_back((static_cast<uint64_t>(dIdx) << 48) | (pkg.parent_frn & 0x0000FFFFFFFFFFFFull));
        m_parent_indices.push_back(0xFFFFFFFF);
        m_sizes.push_back(pkg.size);
        m_timestamps.push_back(pkg.timestamp);
        m_attributes.push_back(pkg.attributes);
        m_metadata_fetched.push_back(pkg.metadata_fetched);
        
        m_name_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), pkg.name.begin(), pkg.name.end());
        m_string_pool.push_back('\0');

        std::string extStr;
        splitNameAndExt(pkg.name, extStr);
        m_ext_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), extStr.begin(), extStr.end());
        m_string_pool.push_back('\0');
    }

    rebuildFrnToIndexMap();
    // 2026-07-07 物理修复：单盘加载后的强制去重收缩 (Analysis_Modification_Plan-154.md)
    compact();
    buildSortedIndices();
    m_isInitialized = true;

    auto* w = new UsnWatcher(vol, lastUsn, nullptr);
    m_watcher_map[vol] = w;
    lock.unlock();
    w->start();

    return true;
}

void MftReader::unloadDrive(const QString& drive) {
    std::wstring vol = drive.toStdWString();
    if (vol.size() > 1 && (vol.back() == L'\\' || vol.back() == L'/')) vol.pop_back();

    UsnWatcher* w = nullptr;
    size_t dIdx = (size_t)-1;

    {
        QWriteLocker lock(&m_dataLock);
        for (size_t i = 0; i < m_drive_list.size(); ++i) {
            if (_wcsicmp(m_drive_list[i].c_str(), vol.c_str()) == 0) {
                dIdx = i;
                break;
            }
        }
        if (dIdx == (size_t)-1) return;

        auto itW = m_watcher_map.find(vol);
        if (itW != m_watcher_map.end()) {
            w = itW->second;
            m_watcher_map.erase(itW);
        }

        // 2026-07-07 物理补齐：按盘符卸载时清理路径缓存与 USN 锚点
        m_next_usns.erase(vol);
        {
            std::lock_guard<std::mutex> pathLock(m_pathCacheMutex);
            auto itP = m_path_cache.begin();
            while (itP != m_path_cache.end()) {
                if ((itP->first >> 48) == dIdx) itP = m_path_cache.erase(itP);
                else ++itP;
            }
        }

        // 策略：将该盘符的所有条目标记为已删除
        for (size_t i = 0; i < m_frns.size(); ++i) {
            if ((m_parent_frns[i] >> 48) == dIdx) {
                m_frns[i] = 0;
                m_dead_count++;
            }
        }

        // 修正：保留占位，禁止平移索引以杜绝漂移 (Analysis_Modification_Plan-154.md)
        m_drive_list[dIdx] = L"";

        // 更新掩码与相关映射
        m_drive_ever_saved.erase(dIdx);
        m_is_compacting.erase(dIdx);
        m_compaction_buffer.erase(dIdx);

        uint32_t mask = m_drive_active_mask.load();
        mask &= ~(1 << dIdx);
        m_drive_active_mask.store(mask);

        compact(); 
    }

    if (w) { w->stop(); delete w; }
}

bool MftReader::saveToCache() {
    QReadLocker lock(&m_dataLock);
    if (!m_isInitialized) return false;
    for (size_t i = 0; i < m_drive_list.size(); ++i) saveDriveToCacheInternal(i);
    return true;
}

bool MftReader::saveDriveToCache(size_t driveIdx) {
    // 2026-06-xx 物理修复：saveDriveToCache 不再持有全局锁，改为内部精细化锁管理
    return saveDriveToCacheInternal(driveIdx);
}

bool MftReader::saveDriveToCacheInternal(size_t driveIdx) {
    // 2026-06-xx 极致架构优化：通过延迟 I/O 彻底剥离锁内磁盘操作
    std::vector<ScchDataPackage> dirtyData;
    uint64_t lastUsn = 0;
    bool isFullSave = false;
    std::wstring volume;

    {
        QReadLocker lock(&m_dataLock);
        if (driveIdx >= m_drive_list.size()) return false;
        volume = m_drive_list[driveIdx];
        lastUsn = m_next_usns[volume];

        std::lock_guard<std::mutex> dLock(m_dirtyLock);
        // 2026-06-xx 任务一修复：isFullSave 判断必须按盘符独立维护，不再依赖全局 m_dirty_indices
        // 方案 A：查 m_drive_ever_saved 标记。
        isFullSave = !m_drive_ever_saved[driveIdx];
        
        // 执行内存拷贝 (O(delta))
        if (isFullSave) {
            for (size_t i = 0; i < m_frns.size(); ++i) {
                if (m_frns[i] != 0 && (m_parent_frns[i] >> 48) == driveIdx) {
                    ScchDataPackage pkg;
                    pkg.frn = m_frns[i];
                    pkg.parent_frn = m_parent_frns[i] & 0x0000FFFFFFFFFFFFull;
                    pkg.size = m_sizes[i];
                    pkg.timestamp = m_timestamps[i];
                    pkg.attributes = m_attributes[i];
                    pkg.metadata_fetched = m_metadata_fetched[i];
                    pkg.tombstone = 0;
                    pkg.name = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
                    dirtyData.push_back(std::move(pkg));
                }
            }
        } else {
            auto it = m_dirty_indices.begin();
            while (it != m_dirty_indices.end()) {
                uint32_t idx = *it;
                if (idx < m_frns.size() && (m_parent_frns[idx] >> 48) == driveIdx) {
                    ScchDataPackage pkg;
                    if (m_frns[idx] == 0) {
                        pkg.frn = m_dead_frns[idx];
                        pkg.tombstone = 1;
                        m_dead_frns.erase(idx);
                    } else {
                        pkg.frn = m_frns[idx];
                        pkg.tombstone = 0;
                    }
                    pkg.parent_frn = m_parent_frns[idx] & 0x0000FFFFFFFFFFFFull;
                    pkg.size = m_sizes[idx];
                    pkg.timestamp = m_timestamps[idx];
                    pkg.attributes = m_attributes[idx];
                    pkg.metadata_fetched = m_metadata_fetched[idx];
                    pkg.name = pkg.tombstone ? "" : reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[idx]);
                    dirtyData.push_back(std::move(pkg));
                    it = m_dirty_indices.erase(it);
                } else {
                    ++it;
                }
            }
        }
    } // [LOCK RELEASE POINT] - 锁已释放

    // [I/O START POINT] - 执行磁盘操作，杜绝假死
    std::string path_base = "FERREX/cache/" + QString::fromStdWString(volume).left(1).toStdString();
    
    // 2026-06-xx 任务一修复：追加模式下若文件不存在，强制转为全量保存
    if (!isFullSave) {
        if (!std::filesystem::exists(path_base + ".bin")) {
            isFullSave = true;
            // 重新获取全量数据 (此处已在锁外，存在微弱一致性风险，但优于永久无法落盘)
            dirtyData.clear();
            {
                QReadLocker lock(&m_dataLock);
                for (size_t i = 0; i < m_frns.size(); ++i) {
                    if (m_frns[i] != 0 && (m_parent_frns[i] >> 48) == driveIdx) {
                        ScchDataPackage pkg;
                        pkg.frn = m_frns[i];
                        pkg.parent_frn = m_parent_frns[i] & 0x0000FFFFFFFFFFFFull;
                        pkg.size = m_sizes[i];
                        pkg.timestamp = m_timestamps[i];
                        pkg.attributes = m_attributes[i];
                        pkg.metadata_fetched = m_metadata_fetched[i];
                        pkg.tombstone = 0;
                        pkg.name = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
                        dirtyData.push_back(std::move(pkg));
                    }
                }
            }
        }
    }

    if (isFullSave) {
        bool ok = ScchCache::saveAll(path_base, dirtyData, lastUsn);
        if (ok) {
            std::lock_guard<std::mutex> dLock(m_dirtyLock);
            m_drive_ever_saved[driveIdx] = true;
        }
        return ok;
    } else {
        // 增量模式下的 Compaction 触发逻辑
        bool ok = ScchCache::appendEntries(path_base, dirtyData, lastUsn);
        if (ScchCache::needsCompaction(path_base)) {
            {
                QWriteLocker lock(&m_dataLock);
                m_is_compacting[driveIdx] = true;
            }
            QThreadPool::globalInstance()->start([this, path_base, driveIdx]() {
                ScchCache::compact(path_base);
                std::vector<ScchDataPackage> buffered;
                uint64_t finalUsn = 0;
                {
                    QWriteLocker lock(&m_dataLock);
                    buffered = std::move(m_compaction_buffer[driveIdx]);
                    m_is_compacting[driveIdx] = false;
                    finalUsn = m_next_usns[m_drive_list[driveIdx]];
                }
                if (!buffered.empty()) ScchCache::appendEntries(path_base, buffered, finalUsn);
            });
        }
        return ok;
    }
}

bool MftReader::saveDriveToCacheUnlocked(size_t driveIdx) {
    // 2026-06-xx 物理修复：在持有写锁时执行的“无锁版”落盘辅助。
    // 为了符合“锁外 I/O”规范，我们将 I/O 逻辑异步化。
    // 理由：buildIndex 内部持有 QWriteLocker，此时若执行同步 I/O 会导致所有 UI 读线程挂起。
    QThreadPool::globalInstance()->start([this, driveIdx]() {
        if (!saveDriveToCacheInternal(driveIdx)) {
            QString letter = "?:";
            {
                QReadLocker lock(&m_dataLock);
                if (driveIdx < m_drive_list.size()) letter = QString::fromStdWString(m_drive_list[driveIdx]);
            }
            qWarning() << "[MftReader] 后台异步落盘失败! 盘符:" << letter << " driveIdx:" << driveIdx;
        }
    });
    return true;
}

QString MftReader::getName(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_name_offsets.size()) return QString();
    return QString::fromUtf8(reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[index]));
}

const char* MftReader::getExt(int index) const {
    // 物理加固：虽然裸指针存在风险，但考虑到 matchEntry 和 search 在并行循环中对 QByteArray/QString 的分配极其敏感，
    // 我们在此处维持裸指针返回，但在注释中明确警告：仅允许在持有数据读锁或 UI 线程快照安全期内使用。
    // 为了对标 getName 的安全性模式，若非性能热点建议改用 getExtQString()。
    if (index < 0 || index >= (int)m_ext_offsets.size()) return "";
    return reinterpret_cast<const char*>(m_string_pool.data() + m_ext_offsets[index]);
}

QString MftReader::getExtQString(int index) const {
    QReadLocker lock(&m_dataLock);
    return QString::fromUtf8(getExt(index));
}

int64_t MftReader::getSize(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_sizes.size()) return 0;
    return m_sizes[index];
}

int64_t MftReader::getModifyTime(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_timestamps.size()) return 0;
    return m_timestamps[index];
}

uint32_t MftReader::getAttributes(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_attributes.size()) return 0;
    return m_attributes[index];
}

uint64_t MftReader::getFrn(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_frns.size()) return 0;
    return m_frns[index];
}

bool MftReader::isDirectory(int index) const {
    return (getAttributes(index) & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool MftReader::isMetadataFetched(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_metadata_fetched.size()) return true;
    return m_metadata_fetched[index] == 2;
}

int MftReader::totalCount() const {
    QReadLocker lock(&m_dataLock);

    uint32_t activeMask = m_drive_active_mask.load(std::memory_order_relaxed);
    int count = 0;

    // 2026-07-07 物理修正：仅累加处于激活掩码中的盘符文件 (Analysis_Modification_Plan-154.md)
    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] == 0) continue; // 忽略已删除条目

        size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
        if (dIdx < 32 && (activeMask & (1 << dIdx))) {
            count++;
        }
    }
    return count;
}

int MftReader::getIndexByKey(uint64_t compositeKey) const {
    QReadLocker lock(&m_dataLock);
    auto it = m_frn_to_idx.find(compositeKey);
    return (it != m_frn_to_idx.end()) ? (int)it->second : -1;
}

bool MftReader::matchEntry(int i, const QString& query, bool useRegex, bool caseSensitive, 
                          const QStringList& extensionList, bool includeHidden, bool includeSystem,
                          bool includeDollar) const {
    QReadLocker lock(&m_dataLock);
    if (i < 0 || i >= (int)m_frns.size() || m_frns[i] == 0) return false;

    // 驱动器过滤
    size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
    if (dIdx >= 32 || !(m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) return false;

    // 属性过滤
    uint32_t at = m_attributes[i];
    if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) return false;
    if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) return false;

    const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);

    // $ 过滤逻辑：如果不包含 $，且文件名以 $ 开头，则过滤掉
    if (!includeDollar && p[0] == '$') return false;

    if (query.isEmpty() && extensionList.isEmpty()) return true;

    // 2026-06-xx 极致性能重构：基于 SoA 预拆分字段的零解析后缀比较
    if (!extensionList.isEmpty()) {
        bool extMatch = false;
        const char* ext = reinterpret_cast<const char*>(m_string_pool.data() + m_ext_offsets[i]);
        for (const QString& ex : extensionList) {
            // 此处通常由 UI 层调用，ex 可能未预处理。
            // 物理优化：仅针对最简单的情况进行优化。
            QByteArray exUtf8 = ex.toUtf8();
            const char* exPtr = exUtf8.constData();
            if (exPtr[0] == '.') exPtr++;
            if (_stricmp(ext, exPtr) == 0) {
                extMatch = true;
                break;
            }
        }
        if (!extMatch) return false;
    }

    if (query.isEmpty()) return true;

    // 内容过滤 (2026-06-xx 极致性能重构：去分配化/低频分配路径)
    if (useRegex) {
        // 正则表达式暂时无法避免 QString 构造，但在 matchEntry 中通常用于二次细分过滤，频次受控
        return QRegularExpression(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption)
               .match(QString::fromUtf8(p)).hasMatch();
    } else {
        // 极致性能：直接对原始 UTF-8 内存块执行子串查找，彻底消除对 Qt 类型转换的依赖
        QByteArray queryUtf8 = query.toUtf8();
        if (caseSensitive) {
            return (strstr(p, queryUtf8.constData()) != nullptr);
        } else {
            // StrStrIA 是 Windows Shlwapi.h 提供的原生 ANSI 子串查找，性能优于 QString::contains
            return (StrStrIA(p, queryUtf8.constData()) != nullptr);
        }
    }
}

uint64_t MftReader::getKeyByIndex(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_frns.size()) return 0;
    size_t dIdx = static_cast<size_t>(m_parent_frns[index] >> 48);
    return makeKey(dIdx, m_frns[index]);
}

QString MftReader::getFullPath(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_frns.size()) return QString();
    uint64_t frn = m_frns[index];
    size_t dIdx = static_cast<size_t>(m_parent_frns[index] >> 48);
    return QString::fromStdWString(const_cast<MftReader*>(this)->getPathFast(dIdx, frn));
}

std::wstring MftReader::getPathFast(size_t driveIdx, uint64_t frn) {
    // 2026-05-16 核心修正：使用复合 Key (driveIdx << 48 | 48位FRN) 解决多盘符冲突与序列号匹配失效
    uint64_t compositeKey = (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);

    {
        std::lock_guard<std::mutex> lock(m_pathCacheMutex);
        auto it = m_path_cache.find(compositeKey);
        if (it != m_path_cache.end()) return it->second;
    }

    std::vector<std::wstring> segments;
    
    // 2026-06-xx 极致架构优化：采用 SoA 直连下标进行路径回溯。
    // 理由：getPathFast 常在 UI 渲染的热点路径被调用，消除 Map 查找是实现百万级数据“瞬间回溯”的关键。
    auto idxIt = m_frn_to_idx.find(compositeKey);
    if (idxIt == m_frn_to_idx.end()) return L"";

    uint32_t curIdx = idxIt->second;
    std::unordered_set<uint32_t> vis;

    while (curIdx != 0xFFFFFFFF) {
        if (vis.count(curIdx)) break;
        vis.insert(curIdx);

        const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[curIdx]);
        segments.push_back(QString::fromUtf8(p).toStdWString());

        uint64_t parentFrn = m_parent_frns[curIdx] & 0x0000FFFFFFFFFFFFull;
        if (parentFrn == 5 || parentFrn == 0) break;

        curIdx = m_parent_indices[curIdx];
    }

    if (segments.empty()) return L"";

    std::wstring volume = (driveIdx < m_drive_list.size()) ? m_drive_list[driveIdx] : L"C:";
    std::wstring path = volume;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) path += L"\\" + *it;

    {
        std::lock_guard<std::mutex> lock(m_pathCacheMutex);
        if (m_path_cache.size() > 200000) { // 2026-05-16 扩容路径缓存以提升深度目录渲染性能
            auto it_clear = m_path_cache.begin();
            for (int i = 0; i < 2000; ++i) it_clear = m_path_cache.erase(it_clear);
        }
        m_path_cache[compositeKey] = path;
    }
    return path;
}

std::vector<uint64_t> MftReader::search(const QString& query, bool useRegex, bool caseSensitive, 
                                       const QStringList& extensionList, bool includeHidden, bool includeSystem,
                                       bool includeDollar) {
    {
        QReadLocker lock(&m_dataLock);
        if (!m_isInitialized) return {};
    }

    bool hasQuery = !query.isEmpty();
    bool hasExt = !extensionList.isEmpty();
    
    QRegularExpression re;
    QByteArray queryUtf8;
    if (hasQuery) {
        if (useRegex) {
            re = QRegularExpression(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        } else {
            queryUtf8 = query.toUtf8();
        }
    }

    std::vector<QByteArray> processedExtBytes;
    if (hasExt) {
        for (const QString& ex : extensionList) {
            QString normalized = ex.toLower();
            if (normalized.startsWith('.')) normalized = normalized.mid(1);
            processedExtBytes.push_back(normalized.toUtf8());
        }
    }

    std::mutex mtx;
    std::vector<uint64_t> finalRes;
    finalRes.reserve(m_frns.size() / 16);

    // 2026-06-xx 性能优化：动态任务平衡。
    // 将分块粒度改为 (总数 / 核心数 / 4)，消除由于数据分布不均（如 C 盘文件密度极高）产生的单核长尾阻塞。
    // 2026-06-xx 物理加固：处理 hardware_concurrency 返回 0 的极端情况。
    unsigned int nThreads = std::thread::hardware_concurrency();
    if (nThreads == 0) nThreads = 2; 
    const size_t idealGrain = (m_frns.size() / (nThreads * 4));
    const size_t grainSize = (std::max)(static_cast<size_t>(10000), idealGrain); 

    // 2026-06-xx 极致算法重构：去锁化/大跨度锁搜索
    if (hasQuery && !useRegex && !caseSensitive && !hasExt) {
        // 1. 前缀搜索分支：采用单次大跨度读锁保护，彻底消除二分查找中的锁震荡
        QReadLocker lock(&m_dataLock);
        
        auto it_start = std::lower_bound(m_sorted_indices.begin(), m_sorted_indices.end(), queryUtf8.constData(), 
            [this](uint32_t idx, const char* q) {
                const char* name = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[idx]);
                return _strnicmp(name, q, strlen(q)) < 0;
            });
        
        for (auto it = it_start; it != m_sorted_indices.end(); ++it) {
            uint32_t i = *it;
            if (i >= m_frns.size() || m_frns[i] == 0) continue;
            
            const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
            if (_strnicmp(p, queryUtf8.constData(), queryUtf8.size()) != 0) break; 

            if (!includeDollar && p[0] == '$') continue;

            size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
            if (dIdx >= 32 || !(m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) continue;
            
            uint32_t at = m_attributes[i];
            if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;

            finalRes.push_back(makeKey(dIdx, m_frns[i]));
            if (finalRes.size() > 200000) break; 
        }
    } else {
        // 2. 全量/复杂搜索分支：维持分块并行，但大幅降低加锁频率
        size_t currentTotal = 0;
        { QReadLocker lock(&m_dataLock); currentTotal = m_frns.size(); }

        size_t numChunks = (currentTotal + grainSize - 1) / grainSize;
        std::vector<size_t> chunks(numChunks);
        std::iota(chunks.begin(), chunks.end(), 0);

        // 2026-06-xx 性能策略：使用 QtConcurrent 实现分块并行搜索，杜绝 std::execution 导致的编译失败
        QtConcurrent::blockingMap(chunks.begin(), chunks.end(), [&](size_t chunkIdx) {
            std::vector<uint64_t> localRes;
            size_t startPos = chunkIdx * grainSize;
            
            {
                QReadLocker lock(&m_dataLock);
                size_t endPos = (std::min)(startPos + grainSize, m_frns.size());

                for (size_t i = startPos; i < endPos; ++i) {
                    if (m_frns[i] == 0) continue;
                    
                    size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
                    if (dIdx >= 32 || !(m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) continue;

                    uint32_t at = m_attributes[i];
                    if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
                    if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;

                    const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
                    if (!includeDollar && p[0] == '$') continue;

                    if (!hasQuery && !hasExt) {
                        localRes.push_back(makeKey(dIdx, m_frns[i]));
                        continue;
                    }

                    if (hasExt) {
                        bool extMatch = false;
                        const char* ext = reinterpret_cast<const char*>(m_string_pool.data() + m_ext_offsets[i]);
                        for (const auto& ex : processedExtBytes) {
                            if (_stricmp(ext, ex.constData()) == 0) {
                                extMatch = true; break;
                            }
                        }
                        if (!extMatch) continue;
                    }

                    if (!hasQuery) {
                        localRes.push_back(makeKey(dIdx, m_frns[i]));
                    } else {
                        bool match = false;
                        if (useRegex) match = re.match(QString::fromUtf8(p)).hasMatch();
                        else {
                            if (caseSensitive) match = (strstr(p, queryUtf8.constData()) != nullptr);
                            else match = (StrStrIA(p, queryUtf8.constData()) != nullptr);
                        }
                        if (match) localRes.push_back(makeKey(dIdx, m_frns[i]));
                    }
                }
            }
            if (!localRes.empty()) { std::lock_guard<std::mutex> l(mtx); finalRes.insert(finalRes.end(), localRes.begin(), localRes.end()); }
        });
    }
    return finalRes;
}

void MftReader::updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume) {
    USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(record);
    uint64_t frn, parentFrn, usn;
    uint32_t attr;
    LARGE_INTEGER timestamp;
    WORD fileNameLength, fileNameOffset;

    // 2026-05-28 物理修复：针对 USN V3 (ReFS/最新Win11) 进行原子化布局匹配
    // V3 采用 128位 FRN，且 USN 字段偏移量 (40) 与 V2 (24) 截然不同。
    if (header->MajorVersion == 2) {
        frn = record->FileReferenceNumber;
        parentFrn = record->ParentFileReferenceNumber;
        usn = record->Usn;
        attr = record->FileAttributes;
        timestamp = record->TimeStamp;
        fileNameLength = record->FileNameLength;
        fileNameOffset = record->FileNameOffset;
    } else if (header->MajorVersion == 3) {
        USN_RECORD_V3* v3 = reinterpret_cast<USN_RECORD_V3*>(record);
        // 取低 64 位 FRN 兼容现有 SoA 架构 (Memories.md 物理铁律)
        frn = *reinterpret_cast<uint64_t*>(&v3->FileReferenceNumber);
        parentFrn = *reinterpret_cast<uint64_t*>(&v3->ParentFileReferenceNumber);
        usn = v3->Usn;
        attr = v3->FileAttributes;
        timestamp = v3->TimeStamp;
        fileNameLength = v3->FileNameLength;
        fileNameOffset = v3->FileNameOffset;
    } else return;

    // 2026-06-xx 极致架构优化：彻底剥离 USN 处理路径中的同步磁盘 I/O。
    // 理由：OpenFileById 在高频 USN 冲击下会导致监听线程严重阻塞，引发 Journal 溢出风险。
    uint64_t fileSize = 0; // 物理大小暂时设为 0，由 requestMetadata 异步补全
    int64_t finalModifyTime = filetimeToUnixMs(timestamp.QuadPart);
    uint32_t finalAttr = attr;

    QWriteLocker lock(&m_dataLock);
    QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(record) + fileNameOffset), fileNameLength / 2);
    
    // 2026-05-28 物理修复：采用不区分大小写的盘符匹配，确保多盘符环境下 dIdx 绝对对齐
    int dIdx = -1;
    for (size_t i = 0; i < m_drive_list.size(); ++i) { 
        if (_wcsicmp(m_drive_list[i].c_str(), volume.c_str()) == 0) { 
            dIdx = (int)i; 
            break; 
        } 
    }
    if (dIdx == -1) {
        qDebug() << "[MftReader] 警告：接收到未索引驱动器的 USN 记录:" << QString::fromStdWString(volume);
        return;
    }

    uint64_t encodedPf = makeKey((size_t)dIdx, parentFrn);
    uint64_t compositeKey = makeKey(dIdx, frn);
    auto it = m_frn_to_idx.find(compositeKey);
    uint32_t finalIdx = 0;
    bool isNew = false;

    if (it != m_frn_to_idx.end()) {
        finalIdx = it->second;
        m_parent_frns[finalIdx] = encodedPf;
        
        // 2026-06-xx 物理补齐：在更新路径同步维护父节点下标，确保路径回溯不漂移
        auto itParent = m_frn_to_idx.find(encodedPf);
        m_parent_indices[finalIdx] = (itParent != m_frn_to_idx.end()) ? itParent->second : 0xFFFFFFFF;

        m_attributes[finalIdx] = finalAttr;
        m_metadata_fetched[finalIdx] = 0; // 标记为未补全
        
        m_sizes[finalIdx] = fileSize;
        m_timestamps[finalIdx] = finalModifyTime;

        QByteArray utf8 = name.toUtf8();
        uint32_t oldOff = m_name_offsets[finalIdx];
        const char* oldPtr = reinterpret_cast<const char*>(m_string_pool.data() + oldOff);
        size_t oldLen = strlen(oldPtr);
        
        // 增量更新时，原有的 ext 也视作浪费
        uint32_t oldExtOff = m_ext_offsets[finalIdx];
        m_wasted_string_bytes += (strlen(reinterpret_cast<const char*>(m_string_pool.data() + oldExtOff)) + 1);

        if ((size_t)utf8.size() <= oldLen) {
            memcpy(m_string_pool.data() + oldOff, utf8.constData(), utf8.size());
            m_string_pool[oldOff + utf8.size()] = '\0';
            if ((size_t)utf8.size() < oldLen) m_wasted_string_bytes += (oldLen - utf8.size());
        } else {
            m_wasted_string_bytes += (oldLen + 1);
            m_name_offsets[finalIdx] = (uint32_t)m_string_pool.size();
            m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
            m_string_pool.push_back('\0');
        }

        // 重新追加 ext
        std::string extStr;
        splitNameAndExt(utf8.toStdString(), extStr);
        m_ext_offsets[finalIdx] = (uint32_t)m_string_pool.size();
        m_string_pool.insert(m_string_pool.end(), extStr.begin(), extStr.end());
        m_string_pool.push_back('\0');
    } else {
        finalIdx = (uint32_t)m_frns.size();
        isNew = true;
        m_frns.push_back(frn);
        m_parent_frns.push_back(encodedPf);
        
        // 2026-06-xx 物理补齐：实时更新父节点索引，确保路径回溯流水线不中断
        auto itParent = m_frn_to_idx.find(encodedPf);
        m_parent_indices.push_back(itParent != m_frn_to_idx.end() ? itParent->second : 0xFFFFFFFF);

        m_sizes.push_back(fileSize);
        m_timestamps.push_back(finalModifyTime);
        m_attributes.push_back(finalAttr);
        m_metadata_fetched.push_back(0); // 标记为未补全
        
        QByteArray utf8 = name.toUtf8();
        m_name_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
        m_string_pool.push_back('\0');

        std::string extStr;
        splitNameAndExt(utf8.toStdString(), extStr);
        m_ext_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), extStr.begin(), extStr.end());
        m_string_pool.push_back('\0');

        m_frn_to_idx[compositeKey] = finalIdx;
    }
    { std::lock_guard<std::mutex> l(m_pathCacheMutex); m_path_cache.erase(compositeKey); }
    m_next_usns[volume] = usn;
    m_dirty_count++;
    {
        std::lock_guard<std::mutex> dLock(m_dirtyLock);
        m_dirty_indices.insert(finalIdx);
    }
    
    // 2026-05-14 工业级内存加固：实时监控内存碎片率
    // 当浪费的字符串空间超过 20MB 或死亡条目过多时，强制执行 compact 碎片整理
    if (m_wasted_string_bytes > 20 * 1024 * 1024 || m_dead_count > 100000) {
        compact();
        // 碎片整理后索引可能发生变化，需要重新从 map 获取
        finalIdx = m_frn_to_idx[compositeKey];
    }

    bool shouldSave = false;
    if (m_dirty_count >= 1000) { 
        m_dirty_count = 0; 
        shouldSave = true;
    }
    
    lock.unlock(); 

    if (shouldSave) {
        // 2026-06-xx 物理分离：将耗时 I/O 移出写锁范围，杜绝 UI 挂起
        QThreadPool::globalInstance()->start([this, dIdx]() {
            saveDriveToCache(dIdx);
        });
    }

    // [流水线补齐点]：在完成内存登记后，立即触发异步物理属性获取
    requestMetadata(finalIdx);

    {
        std::lock_guard<std::mutex> journalLock(m_journalMutex);
        m_changeJournal.push_back({isNew ? ChangeEvent::Added : ChangeEvent::Updated, compositeKey, finalIdx});
        if (!m_notifyTimer->isActive()) {
            QMetaObject::invokeMethod(m_notifyTimer, "start", Qt::QueuedConnection);
        }
    }
}

std::vector<MftReader::ChangeEvent> MftReader::pullChangeJournal() {
    std::lock_guard<std::mutex> lock(m_journalMutex);
    return std::move(m_changeJournal);
}

void MftReader::removeEntryByFrn(const std::wstring& volume, uint64_t frn) {
    QWriteLocker lock(&m_dataLock);
    
    // 2026-05-28 物理修复：采用不区分大小写的盘符匹配
    int dIdx = -1;
    for (size_t i = 0; i < m_drive_list.size(); ++i) { 
        if (_wcsicmp(m_drive_list[i].c_str(), volume.c_str()) == 0) { 
            dIdx = (int)i; 
            break; 
        } 
    }
    if (dIdx == -1) return;

    uint64_t compositeKey = makeKey((size_t)dIdx, frn);

    auto it = m_frn_to_idx.find(compositeKey);
    if (it != m_frn_to_idx.end()) {
        uint32_t idx = it->second;

        // 2026-06-xx 物理修复：在磁盘层面追加 tombstone 记录
        {
            std::lock_guard<std::mutex> dLock(m_dirtyLock);
            m_dirty_indices.insert(idx); 
            m_dead_frns[idx] = m_frns[idx];
        }

        m_frns[idx] = 0; // 标记为死亡
        m_frn_to_idx.erase(it);
        m_dead_count++;
        m_dirty_count++;

        // 2026-05-28 物理修复：立即从排序索引中移除已删除项的引用
        // 理由：如果不移除，二分搜索可能会撞上这个 frn=0 的死亡条目，导致提前 break 从而漏掉有效结果。
        auto itSorted = std::find(m_sorted_indices.begin(), m_sorted_indices.end(), idx);
        if (itSorted != m_sorted_indices.end()) {
            m_sorted_indices.erase(itSorted);
        }

        const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[idx]);
        m_wasted_string_bytes += (strlen(p) + 1);
        
        { std::lock_guard<std::mutex> lockCache(m_pathCacheMutex); m_path_cache.erase(compositeKey); }
        
        bool shouldCompact = (m_dead_count > 50000 || m_wasted_string_bytes > 10 * 1024 * 1024);
        
        lock.unlock(); 

        if (shouldCompact) {
            // Compact 必须在持有写锁时执行，但我们可以选择在空闲时段触发
            // 或者暂时维持现状，但将其移出 removeEntryByFrn 的紧凑循环
            QWriteLocker compactLock(&m_dataLock);
            compact();
        }
        {
            std::lock_guard<std::mutex> journalLock(m_journalMutex);
            m_changeJournal.push_back({ChangeEvent::Removed, compositeKey, 0});
            if (!m_notifyTimer->isActive()) {
                QMetaObject::invokeMethod(m_notifyTimer, "start", Qt::QueuedConnection);
            }
        }
    }
}

void MftReader::compact() {
    m_generation.fetch_add(1, std::memory_order_relaxed);
    // 2026-05-14 内存管理优化：执行碎片整理，回收无效条目和字符串池空间
    std::vector<uint64_t>  new_frns;
    std::vector<uint64_t>  new_parent_frns;
    std::vector<int64_t>   new_sizes;
    std::vector<int64_t>   new_timestamps;
    std::vector<uint32_t>  new_name_offsets;
    std::vector<uint32_t>  new_ext_offsets;
    std::vector<uint32_t>  new_attributes;
    std::vector<uint8_t>   new_metadata_fetched;
    std::vector<uint8_t>   new_string_pool;

    size_t count = m_frns.size();
    new_frns.reserve(count - m_dead_count);
    new_parent_frns.reserve(count - m_dead_count);
    new_sizes.reserve(count - m_dead_count);
    new_timestamps.reserve(count - m_dead_count);
    new_name_offsets.reserve(count - m_dead_count);
    new_attributes.reserve(count - m_dead_count);
    new_metadata_fetched.reserve(count - m_dead_count);
    new_string_pool.reserve(m_string_pool.size() - m_wasted_string_bytes);

    m_frn_to_idx.clear();
    for (size_t i = 0; i < count; ++i) {
        if (m_frns[i] == 0) continue;
        
        uint32_t newIdx = (uint32_t)new_frns.size();
        // 2026-05-28 物理修复：在碎片整理重构索引时，必须维持驱动器复合 Key 映射，杜绝多盘符冲突
        size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
        m_frn_to_idx[makeKey(dIdx, m_frns[i])] = newIdx;
        
        new_frns.push_back(m_frns[i]);
        new_parent_frns.push_back(m_parent_frns[i]);
        new_sizes.push_back(m_sizes[i]);
        new_timestamps.push_back(m_timestamps[i]);
        new_attributes.push_back(m_attributes[i]);
        new_metadata_fetched.push_back(m_metadata_fetched[i]);
        
        const char* name = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
        size_t len = strlen(name) + 1;
        new_name_offsets.push_back((uint32_t)new_string_pool.size());
        new_string_pool.insert(new_string_pool.end(), name, name + len);

        const char* ext = reinterpret_cast<const char*>(m_string_pool.data() + m_ext_offsets[i]);
        size_t extLen = strlen(ext) + 1;
        new_ext_offsets.push_back((uint32_t)new_string_pool.size());
        new_string_pool.insert(new_string_pool.end(), ext, ext + extLen);
    }

    m_frns = std::move(new_frns);
    m_parent_frns = std::move(new_parent_frns);
    // m_parent_indices 在 rebuildFrnToIndexMap 中重建，此处无需处理
    m_sizes = std::move(new_sizes);
    m_timestamps = std::move(new_timestamps);
    m_name_offsets = std::move(new_name_offsets);
    m_ext_offsets = std::move(new_ext_offsets);
    m_attributes = std::move(new_attributes);
    m_metadata_fetched = std::move(new_metadata_fetched);
    m_string_pool = std::move(new_string_pool);

    m_dead_count = 0;
    m_wasted_string_bytes = 0;
    rebuildFrnToIndexMap();
    buildSortedIndices();
}

bool MftReader::loadMftDirect(const std::wstring& volume, MftReader::DriveResult& result) {
    std::wstring dev = L"\\\\.\\" + volume;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        qWarning() << "[MftReader] 无法打开卷句柄" << QString::fromStdWString(volume) << "错误码:" << GetLastError();
        return false;
    }

    // 2026-05-14 获取根目录句柄作为 Hint，这对于 OpenFileById 的稳定性至关重要
    std::wstring rootPath = volume + L"\\";
    // 修正：赋予 FILE_READ_ATTRIBUTES 权限
    HANDLE hHint = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

    USN_JOURNAL_DATA_V0 j; DWORD cb;
    if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &j, sizeof(j), &cb, NULL)) { 
        qWarning() << "[MftReader] FSCTL_QUERY_USN_JOURNAL 失败" << QString::fromStdWString(volume) << "错误码:" << GetLastError();
        if (hHint != INVALID_HANDLE_VALUE) CloseHandle(hHint);
        CloseHandle(h); return false; 
    }
    result.nextUsn = j.NextUsn;
    MFT_ENUM_DATA_V0 ed = {0}; ed.HighUsn = j.NextUsn;
    std::vector<uint8_t> buf(1024 * 1024);
    int recordCount = 0;
    int lastSavedCount = 0;
    int consecutiveErrors = 0;

    // 2026-06-xx 极致容错与零分配重构
    while (true) {
        BOOL ok = DeviceIoControl(h, FSCTL_ENUM_USN_DATA, &ed, sizeof(ed), buf.data(), (DWORD)buf.size(), &cb, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            
            // 2026-06-xx 物理加固：遇到受限条目（如 System Volume Information 内部 I/O 冲突）
            // 采用“死不回头”策略，记录并尝试继续。
            qDebug() << "[MFT] Enumeration encountered non-fatal error:" << err << "on volume" << QString::fromStdWString(volume);
            
            // 风险控制：如果连续出现 10 次错误且锚点未推进，则判定卷不可访问，退出以防死循环。
            if (++consecutiveErrors > 10) break;

            if (err == ERROR_ACCESS_DENIED || err == ERROR_INVALID_PARAMETER) {
                // 尝试跳过当前锚点
                ed.StartFileReferenceNumber++; 
                continue;
            }
            break; 
        }
        consecutiveErrors = 0;

        if (m_isStopping.load()) break;
        if (cb < 8) break;
        uint8_t* p = buf.data() + 8; uint8_t* end = buf.data() + cb;
        while (p < end) {
            USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(p);
            uint64_t frn, parentFrn;
            LARGE_INTEGER timestamp;
            uint32_t attr;
            WORD fileNameLength, fileNameOffset;

            if (header->MajorVersion == 2) {
                USN_RECORD_V2* rec = reinterpret_cast<USN_RECORD_V2*>(p);
                frn = rec->FileReferenceNumber;
                parentFrn = rec->ParentFileReferenceNumber;
                timestamp = rec->TimeStamp;
                attr = rec->FileAttributes;
                fileNameLength = rec->FileNameLength;
                fileNameOffset = rec->FileNameOffset;
            } else if (header->MajorVersion == 3) {
                struct V3_LAYOUT {
                    DWORD RecordLength; WORD MajorVersion; WORD MinorVersion;
                    BYTE FileReferenceNumber[16]; BYTE ParentFileReferenceNumber[16];
                    USN Usn; LARGE_INTEGER TimeStamp; DWORD Reason; DWORD SourceInfo;
                    DWORD SecurityId; DWORD FileAttributes; WORD FileNameLength; WORD FileNameOffset;
                } *rec = reinterpret_cast<V3_LAYOUT*>(p);
                frn = *reinterpret_cast<uint64_t*>(rec->FileReferenceNumber);
                parentFrn = *reinterpret_cast<uint64_t*>(rec->ParentFileReferenceNumber);
                timestamp = rec->TimeStamp;
                attr = rec->FileAttributes;
                fileNameLength = rec->FileNameLength;
                fileNameOffset = rec->FileNameOffset;
            } else {
                p += header->RecordLength; continue;
            }

            // 2026-06-xx 全链路“零分配”扫描逻辑：
            // 直接将文件名从 MFT 缓冲区通过 WideCharToMultiByte 泵入本地字符串池。
            // 理由：彻底杜绝 QString 和 std::string 构造产生的 O(N) 内存碎块。
            MftReader::RawEntry e; 
            e.frn = frn; 
            e.parentFrn = parentFrn;
            e.size = 0; 
            e.attributes = attr;
            e.modifyTime = filetimeToUnixMs(timestamp.QuadPart);
            
            e.nameOffset = (uint32_t)result.string_pool.size();
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWCH>(p + fileNameOffset), fileNameLength / 2, NULL, 0, NULL, NULL);
            if (utf8Len > 0) {
                size_t oldSize = result.string_pool.size();
                result.string_pool.resize(oldSize + utf8Len + 1);
                WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWCH>(p + fileNameOffset), fileNameLength / 2, reinterpret_cast<LPSTR>(&result.string_pool[oldSize]), utf8Len, NULL, NULL);
                result.string_pool[oldSize + utf8Len] = '\0';
            } else {
                result.string_pool.push_back('\0');
            }

            result.entries.push_back(e);
            recordCount++;

            // 2026-06-xx 中途强制落盘机制 (Checkpointing)
            // 理由：C 盘扫描时间长，引入 10 万记录级别的中途检查点。
            // 2026-06-xx 物理安全性修复：通过构造深拷贝的增量 DataPackage 序列实现线程安全的异步落盘。
            if (recordCount - lastSavedCount >= 100000) {
                // 2026-06-xx 物理安全性加固：显式执行深拷贝，杜绝 UAF 风险。
                std::vector<ScchDataPackage> delta;
                delta.reserve(recordCount - lastSavedCount);
                
                // 获取当前字符串池基地址，由于此循环内不修改 result.string_pool，指针是稳定的
                const uint8_t* poolBase = result.string_pool.data();
                
                for (int i = lastSavedCount; i < recordCount; ++i) {
                    const auto& re = result.entries[i];
                    ScchDataPackage pkg;
                    pkg.frn = re.frn;
                    pkg.parent_frn = re.parentFrn;
                    pkg.size = re.size;
                    pkg.timestamp = re.modifyTime;
                    pkg.attributes = re.attributes;
                    
                    // 核心修复：显式构造 std::string 以确保数据被物理拷贝到 pkg 内部缓冲区。
                    // 理由：虽然赋值操作符也是深拷贝，但显式构造更符合安全审计要求。
                    pkg.name = std::string(reinterpret_cast<const char*>(poolBase + re.nameOffset));
                    
                    delta.push_back(std::move(pkg));
                }

                std::string path_base = "FERREX/cache/" + QString::fromStdWString(volume).left(1).toStdString();
                uint64_t currentUsn = ed.StartFileReferenceNumber;

                // 2026-06-xx 性能策略：异步追加。
                // 理由：delta 包含完全独立所有权的 std::string 副本，后台线程访问是绝对安全的。
                (void)QtConcurrent::run([path_base, delta, currentUsn]() {
                    ScchCache::appendEntries(path_base, delta, currentUsn);
                });

                lastSavedCount = recordCount;
                qDebug() << "[MFT] Incremental checkpoint saved:" << recordCount << "entries for" << QString::fromStdWString(volume);
            }

            p += header->RecordLength;
        }
        ed.StartFileReferenceNumber = *reinterpret_cast<DWORDLONG*>(buf.data());
    }
    if (hHint != INVALID_HANDLE_VALUE) CloseHandle(hHint);
    CloseHandle(h);
    return !result.entries.empty();
}

void MftReader::mergeDriveResult(const std::wstring& volume, const MftReader::DriveResult& result, size_t driveIdx) {
    Q_UNUSED(volume);
    size_t count = result.entries.size();
    m_frns.reserve(m_frns.size() + count);
    m_parent_frns.reserve(m_parent_frns.size() + count);
    m_sizes.reserve(m_sizes.size() + count);
    m_timestamps.reserve(m_timestamps.size() + count);
    m_name_offsets.reserve(m_name_offsets.size() + count);
    m_ext_offsets.reserve(m_ext_offsets.size() + count);
    m_attributes.reserve(m_attributes.size() + count);
    m_metadata_fetched.reserve(m_metadata_fetched.size() + count);
    for (const auto& e : result.entries) {
        m_frns.push_back(e.frn);
        m_parent_frns.push_back((static_cast<uint64_t>(driveIdx) << 48) | (e.parentFrn & 0x0000FFFFFFFFFFFFull));
        m_parent_indices.push_back(0xFFFFFFFF); // 初始为无效下标，待 rebuild 补齐
        m_sizes.push_back(e.size); // 2026-05-14 修正：将扫描到的大小压入 SoA
        m_timestamps.push_back(e.modifyTime); m_attributes.push_back(e.attributes);
        m_metadata_fetched.push_back(0);
        
        const char* namePtr = reinterpret_cast<const char*>(result.string_pool.data() + e.nameOffset);
        m_name_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), namePtr, namePtr + strlen(namePtr) + 1);

        // 2026-06-xx 物理对标：全量扫描时同步预拆分扩展名
        std::string extStr;
        splitNameAndExt(namePtr, extStr);
        m_ext_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), extStr.begin(), extStr.end());
        m_string_pool.push_back('\0');
    }
}

void MftReader::rebuildFrnToIndexMap() {
    // 2026-07-07 极致去重重构：利用 Map 覆盖特性实现物理级去重 (Analysis_Modification_Plan-154.md)
    m_frn_to_idx.clear();

    // 第一遍：构建索引地图（增量记录会自动覆盖旧的下标，保留最后出现的即最新的记录）
    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] != 0) {
            size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
            m_frn_to_idx[makeKey(dIdx, m_frns[i])] = (uint32_t)i;
        }
    }

    // 第二遍：反向标记物理冗余（物理剔除被增量覆盖的旧条目）
    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] == 0) continue;
        size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
        uint64_t key = makeKey(dIdx, m_frns[i]);
        if (m_frn_to_idx[key] != (uint32_t)i) {
            m_frns[i] = 0;
            m_dead_count++;
        }
    }

    // 第三遍：父节点下标预映射（提升路径回溯性能）
    m_parent_indices.assign(m_frns.size(), 0xFFFFFFFF);
    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] == 0) continue;
        uint64_t encodedPf = m_parent_frns[i];
        auto itP = m_frn_to_idx.find(encodedPf);
        if (itP != m_frn_to_idx.end()) {
            m_parent_indices[i] = itP->second;
        }
    }
}

void MftReader::buildSortedIndices() {
    // 2026-06-xx 极致架构优化：去锁化双缓冲排序。
    // 理由：buildSortedIndices 常在 buildIndex 或 compact 期间被调用，
    // 在持有排他写锁的情况下执行 O(N log N) 的字符串排序会物理阻塞 UI 线程数秒之久。
    
    // 1. 投影准备 (此时应持有读锁，但由于 buildIndex 内部逻辑，调用者已处理锁)
    std::vector<uint32_t> new_sorted;
    new_sorted.resize(m_frns.size());
    std::iota(new_sorted.begin(), new_sorted.end(), 0);

    struct NameProjection {
        uint32_t idx;
        const char* name;
    };
    std::vector<NameProjection> projections;
    projections.reserve(new_sorted.size());
    for (uint32_t i : new_sorted) {
        projections.push_back({i, reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i])});
    }

    // 2. 排序阶段 (实际上此处仍在 buildIndex 流程中，但未来可改为异步)
    std::sort(projections.begin(), projections.end(), [](const NameProjection& a, const NameProjection& b) {
        return _stricmp(a.name, b.name) < 0;
    });

    // 3. 回写索引
    for (size_t i = 0; i < projections.size(); ++i) {
        new_sorted[i] = projections[i].idx;
    }
    
    m_sorted_indices = std::move(new_sorted);
}

void MftReader::requestMetadata(int index) {
    // 2026-05-14 工业级异步补全架构：仅在 UI 可见区域按需拉取物理属性
    QWriteLocker writeLock(&m_dataLock);
    if (index < 0 || index >= (int)m_frns.size() || m_frns[index] == 0) return;
    
    // 状态机：0-未获取, 1-拉取中, 2-已完成
    if (m_metadata_fetched[index] != 0) return; 
    m_metadata_fetched[index] = 1; // 标记为拉取中，防止重复触发并发任务

    uint64_t frn = m_frns[index];
    size_t dIdx = static_cast<size_t>(m_parent_frns[index] >> 48);
    if (dIdx >= m_drive_list.size()) {
        m_metadata_fetched[index] = 0;
        return;
    }
    std::wstring volume = m_drive_list[dIdx];
    writeLock.unlock();

    (void)QtConcurrent::run(m_metadataPool, [this, index, frn, volume]() {
        // 2026-05-14 极致性能重构：对标 Rust 原版，采用 API 分级拉取策略
        // 1. 优先使用 GetFileAttributesExW (不涉及文件句柄，非侵入式，性能极高)
        QString fullPath = getFullPath(index);
        WIN32_FILE_ATTRIBUTE_DATA attrData;
        if (GetFileAttributesExW(reinterpret_cast<const wchar_t*>(fullPath.utf16()), GetFileExInfoStandard, &attrData)) {
            QWriteLocker lock(&m_dataLock);
            if (index < (int)m_frns.size() && m_frns[index] == frn) {
                m_sizes[index] = (static_cast<uint64_t>(attrData.nFileSizeHigh) << 32) | attrData.nFileSizeLow;
                m_timestamps[index] = filetimeToUnixMs((static_cast<int64_t>(attrData.ftLastWriteTime.dwHighDateTime) << 32) | attrData.ftLastWriteTime.dwLowDateTime);
                m_attributes[index] = attrData.dwFileAttributes;
                m_metadata_fetched[index] = 2;
                lock.unlock();
                emit dataChanged(index);
                return;
            }
        }

        // 2. 退化方案：对于特殊文件（如被独占锁定但允许属性读取的文件），使用 OpenFileById
        std::wstring rootPath = volume + L"\\";
        HANDLE hHint = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hHint != INVALID_HANDLE_VALUE) {
            FILE_ID_DESCRIPTOR id = { sizeof(FILE_ID_DESCRIPTOR), FileIdType };
            id.FileId.QuadPart = frn;
            HANDLE hFile = OpenFileById(hHint, &id, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, FILE_FLAG_BACKUP_SEMANTICS);
            if (hFile != INVALID_HANDLE_VALUE) {
                BY_HANDLE_FILE_INFORMATION bhfi;
                if (GetFileInformationByHandle(hFile, &bhfi)) {
                    QWriteLocker writeLock(&m_dataLock);
                    if (index < (int)m_frns.size() && m_frns[index] == frn) {
                        m_sizes[index] = (static_cast<uint64_t>(bhfi.nFileSizeHigh) << 32) | bhfi.nFileSizeLow;
                        m_timestamps[index] = filetimeToUnixMs((static_cast<int64_t>(bhfi.ftLastWriteTime.dwHighDateTime) << 32) | bhfi.ftLastWriteTime.dwLowDateTime);
                        m_attributes[index] = bhfi.dwFileAttributes;
                        m_metadata_fetched[index] = 2;
                    }
                }
                CloseHandle(hFile);
            }
            CloseHandle(hHint);
        }

        QWriteLocker lock(&m_dataLock);
        if (index < (int)m_metadata_fetched.size() && m_metadata_fetched[index] == 1) {
            if (m_metadata_fetched[index] != 2) m_metadata_fetched[index] = 0; 
        }
        lock.unlock();
        emit dataChanged(index); 
    });
}

QIcon MftReader::getCachedIcon(const QString& ext, bool isDir) {
    QString key = isDir ? "folder" : ext.toLower();
    {
        QReadLocker lock(&m_iconCacheLock);
        auto it = m_icon_cache.find(key);
        if (it != m_icon_cache.end()) return *it;
    }

    QFileIconProvider provider;
    QIcon icon;
    if (isDir) {
        icon = provider.icon(QFileIconProvider::Folder);
    } else {
        if (key.length() > 12) key = "unknown";
        icon = provider.icon(QFileInfo("dummy." + key));
        if (icon.isNull()) icon = provider.icon(QFileIconProvider::File);
    }

    {
        QWriteLocker lock(&m_iconCacheLock);
        m_icon_cache[key] = icon;
    }
    return icon;
}

} // namespace FERREX
