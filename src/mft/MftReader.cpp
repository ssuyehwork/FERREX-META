#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MftReader.h"
#include "UsnWatcher.h"
#include <winioctl.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#include <algorithm>
#include <execution>
#include <mutex>
#include <numeric>
#include <filesystem>
#include <QDebug>
#include <QRegularExpression>
#include <QDir>
#include <QDateTime>
#include <QtConcurrent/QtConcurrent>
#include <QtConcurrent>
#include <QFuture>
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


namespace ArcMeta {

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
}

MftReader::~MftReader() {
    clear();
}

void MftReader::clearInternal() {
    // 极致工业级优化方案 A：物理内存“强制归还”
    // 使用 Swap 技巧强制 STL 释放 Capacity 并归还堆内存给操作系统
    std::vector<uint64_t>().swap(m_frns);
    std::vector<uint64_t>().swap(m_parent_frns);
    std::vector<int64_t>().swap(m_sizes);
    std::vector<int64_t>().swap(m_timestamps);
    std::vector<uint32_t>().swap(m_name_offsets);
    std::vector<uint32_t>().swap(m_attributes);
    std::vector<uint8_t>().swap(m_metadata_fetched);
    std::vector<uint8_t>().swap(m_string_pool);
    std::vector<uint32_t>().swap(m_sorted_indices);

    m_drive_list.clear();
    m_drive_active_mask = 0;
    m_frn_to_idx.clear();
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
    for (int i = 0; i < 32; ++i) {
        m_drive_dirty_counts[i] = 0;
        m_drive_entry_indices[i].clear();
    }
}

void MftReader::clear() {
    // 极致工业级重构：非阻塞异步清理链
    // 1. 立即标记状态失效，让 UI 线程在 request_lock 时能快速感知并退出，实现“秒关”体验
    {
        QWriteLocker lock(&m_dataLock);
        if (!m_isInitialized || m_is_clearing.load()) return;
        m_is_clearing.store(true);
        m_isInitialized = false; 
    }

    // 2. 将耗时的停止、存盘、释放逻辑转移至后台线程
    (void)QtConcurrent::run([this]() {
        // A. 停止所有监控线程 (防止产生新的脏数据)
        std::vector<UsnWatcher*> toStop;
        {
            QWriteLocker lock(&m_dataLock);
            toStop = std::move(m_watchers);
            m_watchers.clear();
        }
        for (auto* w : toStop) { if (w) { w->stop(); delete w; } }

        // B. 等待正在进行的异步写盘任务结束
        while (m_is_saving.load(std::memory_order_acquire)) {
            QThread::msleep(10);
        }

        // C. 执行最后一次强制存盘 (持久化 USN 游标)
        // 方案一：盘符级状态隔离。检查所有盘符的脏计数
        bool hasDirty = false;
        for (int i = 0; i < 32; ++i) {
            if (m_drive_dirty_counts[i].load() > 0) { hasDirty = true; break; }
        }
        if (hasDirty) {
            saveToCache();
        }

        // D. 物理释放内存 (Swap 技巧)
        {
            QWriteLocker lock(&m_dataLock);
            clearInternal();
            m_is_clearing.store(false);
        }
    });
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
    std::vector<int> scanIndices((int)toScan.size());
    std::iota(scanIndices.begin(), scanIndices.end(), 0);
    std::for_each((std::execution::par), scanIndices.begin(), scanIndices.end(), [&](int i) {
        scannedResults[i].volume = toScan[i];
        scannedResults[i].success = loadMftDirect(toScan[i], scannedResults[i].res);
    });

    QWriteLocker lock(&m_dataLock);
    std::vector<UsnWatcher*> newWatchers;
    for (auto& sr : scannedResults) {
        if (!sr.success || sr.res.entries.empty()) continue;
        
        size_t dIdx = m_drive_list.size();
        m_drive_list.push_back(sr.volume);
        if (dIdx < 32) m_drive_active_mask.fetch_or(1 << dIdx);
        m_next_usns[sr.volume] = sr.res.nextUsn;
        mergeDriveResult(sr.volume, sr.res, dIdx);
        saveDriveToCacheInternal(dIdx);
        
        auto* w = new UsnWatcher(sr.volume, sr.res.nextUsn, nullptr);
        m_watchers.push_back(w);
        newWatchers.push_back(w);
    }

    rebuildFrnToIndexMap();
    buildSortedIndices();
    m_isInitialized = true;

    lock.unlock();
    for (auto* w : newWatchers) w->start();
}

bool MftReader::loadFromCache() {
    std::filesystem::path cacheDir = "ArcMeta/cache";
    if (!std::filesystem::exists(cacheDir)) return false;

    // 物理优化：加载前先停止现有监控，避免句柄冲突
    // 注意：这里手动执行清理逻辑，但不触发 saveToCache，防止覆盖磁盘缓存
    std::vector<UsnWatcher*> toStop;
    {
        QWriteLocker lock(&m_dataLock);
        toStop = std::move(m_watchers);
        m_watchers.clear();
    }
    for (auto* w : toStop) { if (w) { w->stop(); delete w; } }
    
    {
        QWriteLocker lock(&m_dataLock);
        clearInternal();
    }

    struct DriveIndices {
        std::vector<uint32_t> sorted;
        uint32_t baseIdx;
    };
    std::vector<DriveIndices> allSortedIndices;

    for (auto const& entry : std::filesystem::directory_iterator{cacheDir}) {
        if (entry.path().extension() == ".scch") {
            std::vector<uint64_t> f, pf;
            std::vector<int64_t> s, t;
            std::vector<uint32_t> no, attr, ds;
            std::vector<uint8_t> sp, mf;
            std::unordered_map<std::string, uint64_t> usnMap;

            if (ScchCache::load(entry.path().string().c_str(), f, pf, s, t, no, attr, mf, sp, ds, usnMap) == ScchResult::Ok) {
                size_t dIdx;
                size_t oldPoolSize;
                uint32_t baseIdx;
                size_t count = f.size();
                size_t currentTotal;
                std::wstring driveName;

                {
                    QWriteLocker lock(&m_dataLock);
                    dIdx = m_drive_list.size();
                    oldPoolSize = m_string_pool.size();
                    baseIdx = (uint32_t)m_frns.size();

                    m_frns.insert(m_frns.end(), f.begin(), f.end());
                    m_sizes.insert(m_sizes.end(), s.begin(), s.end());
                    m_timestamps.insert(m_timestamps.end(), t.begin(), t.end());
                    m_attributes.insert(m_attributes.end(), attr.begin(), attr.end());
                    m_metadata_fetched.insert(m_metadata_fetched.end(), mf.begin(), mf.end());
                    m_string_pool.insert(m_string_pool.end(), sp.begin(), sp.end());

                    for (size_t i = 0; i < count; ++i) {
                        m_parent_frns.push_back((static_cast<uint64_t>(dIdx) << 48) | (pf[i] & 0x0000FFFFFFFFFFFFull));
                        m_name_offsets.push_back(no[i] + (uint32_t)oldPoolSize);
                    }
                    
                    allSortedIndices.push_back({std::move(ds), baseIdx});

                    for (const auto& [drive, usn] : usnMap) {
                        driveName = QString::fromStdString(drive).toStdWString();
                        m_drive_list.push_back(driveName);
                        m_next_usns[driveName] = usn;
                    }
                    currentTotal = m_frns.size();
                }
                
                // 2026-05-14 启动流控优化：释放锁后发射信号，避免 UI 线程调用 totalCount() 时死锁
                emit driveLoaded(QString::fromStdWString(driveName), (int)count, (int)currentTotal);
            }
        }
    }

    QWriteLocker lock(&m_dataLock);
    if (m_frns.empty()) return false;
    rebuildFrnToIndexMap();

    // 2026-05-14 核心性能优化：执行 K 路归并合并排序索引 (Complexity: O(N log K))
    // 这取代了耗时的 O(N log N) 全量排序，是 C++ 找回极致性能的关键
    if (!allSortedIndices.empty()) {
        m_sorted_indices.clear();
        m_sorted_indices.reserve(m_frns.size());

        struct MergeNode {
            uint32_t globalIdx;
            size_t driveArrIdx;
            size_t innerIdx;
            bool operator>(const MergeNode& other) const {
                const char* s1 = reinterpret_cast<const char*>(MftReader::instance().m_string_pool.data() + MftReader::instance().m_name_offsets[globalIdx]);
                const char* s2 = reinterpret_cast<const char*>(MftReader::instance().m_string_pool.data() + MftReader::instance().m_name_offsets[other.globalIdx]);
                return _stricmp(s1, s2) > 0;
            }
        };
        std::priority_queue<MergeNode, std::vector<MergeNode>, std::greater<MergeNode>> pq;

        for (size_t i = 0; i < allSortedIndices.size(); ++i) {
            if (!allSortedIndices[i].sorted.empty()) {
                pq.push({allSortedIndices[i].sorted[0] + allSortedIndices[i].baseIdx, i, 0});
            }
        }

        while (!pq.empty()) {
            MergeNode top = pq.top();
            pq.pop();
            m_sorted_indices.push_back(top.globalIdx);
            if (top.innerIdx + 1 < allSortedIndices[top.driveArrIdx].sorted.size()) {
                size_t nextInner = top.innerIdx + 1;
                pq.push({allSortedIndices[top.driveArrIdx].sorted[nextInner] + allSortedIndices[top.driveArrIdx].baseIdx, top.driveArrIdx, nextInner});
            }
        }
    } else {
        buildSortedIndices();
    }

    m_isInitialized = true;

    // 方案一：补完缓存加载后的监控链 (接管变动)
    // 在缓存加载成功后，立即为所有已加载的驱动器启动 UsnWatcher
    // 2026-05-29 物理修复：移除此处冗余的 lock 声明（父作用域已持有 lock），消除 C4456 警告
    for (const auto& drive : m_drive_list) {
        uint64_t lastUsn = m_next_usns[drive];
        auto* w = new UsnWatcher(drive, lastUsn, nullptr);
        m_watchers.push_back(w);
        w->start();
    }

    return true;
}

bool MftReader::saveToCache() {
    // 极致工业级优化：一次 O(N) 扫描完成全盘符数据采样，杜绝多次扫描带来的读锁积压
    struct DriveSnapshot {
        std::wstring volume;
        std::vector<uint64_t> f, pf;
        std::vector<int64_t> s, t;
        std::vector<uint32_t> no, attr, ds;
        std::vector<uint8_t> sp, mf;
        uint64_t usn;
    };
    std::vector<DriveSnapshot> snapshots;

    {
        QReadLocker lock(&m_dataLock);
        if (!m_isInitialized && !m_is_clearing.load()) return false;
        
        size_t dCount = m_drive_list.size();
        snapshots.resize(dCount);
        std::vector<std::unordered_map<uint32_t, uint32_t>> offsetMaps(dCount);
        std::vector<std::unordered_map<size_t, uint32_t>> g2lMaps(dCount);

        for (size_t i = 0; i < dCount; ++i) {
            snapshots[i].volume = m_drive_list[i];
            snapshots[i].usn = m_next_usns[m_drive_list[i]];
        }

        // 方案三：精准写入优化。不再执行全量 $O(N)$ 遍历，改为按驱动器索引遍历
        for (size_t dIdx = 0; dIdx < dCount; ++dIdx) {
            if (dIdx >= 32) continue;
            auto& snap = snapshots[dIdx];
            const auto& indices = m_drive_entry_indices[dIdx];
            snap.f.reserve(indices.size());
            snap.pf.reserve(indices.size());
            snap.s.reserve(indices.size());
            snap.t.reserve(indices.size());
            snap.attr.reserve(indices.size());
            snap.mf.reserve(indices.size());
            snap.no.reserve(indices.size());

            for (uint32_t i : indices) {
                if (m_frns[i] == 0) continue;
                
                uint32_t localIdx = (uint32_t)snap.f.size();
                g2lMaps[dIdx][i] = localIdx;

                snap.f.push_back(m_frns[i]);
                snap.pf.push_back(m_parent_frns[i] & 0x0000FFFFFFFFFFFFull);
                snap.s.push_back(m_sizes[i]);
                snap.t.push_back(m_timestamps[i]);
                snap.attr.push_back(m_attributes[i]);
                snap.mf.push_back(m_metadata_fetched[i]);

                uint32_t oldOff = m_name_offsets[i];
                auto it = offsetMaps[dIdx].find(oldOff);
                if (it == offsetMaps[dIdx].end()) {
                    uint32_t newOff = (uint32_t)snap.sp.size();
                    const char* ptr = reinterpret_cast<const char*>(m_string_pool.data() + oldOff);
                    size_t len = strlen(ptr) + 1;
                    snap.sp.insert(snap.sp.end(), ptr, ptr + len);
                    offsetMaps[dIdx][oldOff] = newOff;
                    snap.no.push_back(newOff);
                } else {
                    snap.no.push_back(it->second);
                }
            }
        }

        for (uint32_t gIdx : m_sorted_indices) {
            size_t dIdx = static_cast<size_t>(m_parent_frns[gIdx] >> 48);
            if (dIdx < dCount) {
                auto it = g2lMaps[dIdx].find(gIdx);
                if (it != g2lMaps[dIdx].end()) snapshots[dIdx].ds.push_back(it->second);
            }
        }
    }

    // 锁外并行存盘 (QtConcurrent)
    for (const auto& snap : snapshots) {
        if (snap.f.empty()) continue;
        std::unordered_map<std::string, uint64_t> usnMap;
        usnMap[QString::fromStdWString(snap.volume).toStdString()] = snap.usn;
        QString path = QString("ArcMeta/cache/%1.scch").arg(QString::fromStdWString(snap.volume).left(1));
        ScchCache::save(path.toStdString().c_str(), snap.f, snap.pf, snap.s, snap.t, snap.no, snap.attr, snap.mf, snap.sp, snap.ds, usnMap);
    }
    return true;
}

bool MftReader::saveDriveToCache(size_t driveIdx) {
    // 工业级重构：此函数现在是非阻塞的读锁持有者
    return saveDriveToCacheInternal(driveIdx);
}

bool MftReader::saveDriveToCacheInternal(size_t driveIdx) {
    // 工业级锁分离架构：[阶段 1] 锁内采样（Snapshotting）
    // 仅在内存拷贝阶段持有锁，将耗时 I/O 移出锁外，彻底解决 MainWindow 读者挂起问题。
    std::wstring volume;
    std::vector<uint64_t> f, pf;
    std::vector<int64_t> s, t;
    std::vector<uint32_t> no, attr, ds;
    std::vector<uint8_t> sp, mf;
    uint64_t nextUsnVal = 0;

    {
        // 2026-05-29 物理加固：在采样阶段显式获取读锁，确保 SoA 容器在拷贝期间不被写线程重分配
        QReadLocker lock(&m_dataLock);
        if (!m_isInitialized) return false;
        
        if (driveIdx >= m_drive_list.size()) return false;
        volume = m_drive_list[driveIdx];
        nextUsnVal = m_next_usns[volume];

        std::unordered_map<uint32_t, uint32_t> offsetMap;
        std::unordered_map<size_t, uint32_t> globalToLocal;

        if (driveIdx < 32) {
            const auto& indices = m_drive_entry_indices[driveIdx];
            f.reserve(indices.size());
            pf.reserve(indices.size());
            s.reserve(indices.size());
            t.reserve(indices.size());
            attr.reserve(indices.size());
            mf.reserve(indices.size());
            no.reserve(indices.size());

            for (uint32_t i : indices) {
                if (m_frns[i] == 0) continue;
                
                uint32_t localIdx = (uint32_t)f.size();
                globalToLocal[i] = localIdx;

                f.push_back(m_frns[i]);
                pf.push_back(m_parent_frns[i] & 0x0000FFFFFFFFFFFFull);
                s.push_back(m_sizes[i]);
                t.push_back(m_timestamps[i]);
                attr.push_back(m_attributes[i]);
                mf.push_back(m_metadata_fetched[i]);
                uint32_t oldOff = m_name_offsets[i];
                if (offsetMap.find(oldOff) == offsetMap.end()) {
                    uint32_t newOff = (uint32_t)sp.size();
                    const char* ptr = reinterpret_cast<const char*>(m_string_pool.data() + oldOff);
                    size_t len = strlen(ptr) + 1;
                    sp.insert(sp.end(), ptr, ptr + len);
                    offsetMap[oldOff] = newOff;
                    no.push_back(newOff);
                } else {
                    no.push_back(offsetMap[oldOff]);
                }
            }

            for (uint32_t gIdx : m_sorted_indices) {
                auto it = globalToLocal.find(gIdx);
                if (it != globalToLocal.end()) {
                    ds.push_back(it->second);
                }
            }
        }
    }

    // [阶段 2] 锁外 I/O（Unlocked Persistence）
    // 此时已释放 m_dataLock，UI 线程可以自由执行搜索、渲染等操作。
    std::unordered_map<std::string, uint64_t> usnMap;
    usnMap[QString::fromStdWString(volume).toStdString()] = nextUsnVal;
    QString path = QString("ArcMeta/cache/%1.scch").arg(QString::fromStdWString(volume).left(1));
    
    // 释放调用者的锁以允许并发（由于 saveToCache 等函数持有锁，这里需要特殊的逻辑处理）
    // 为了不破坏现有调用链，我们将 saveToCache 逻辑也进行重构。
    return ScchCache::save(path.toStdString().c_str(), f, pf, s, t, no, attr, mf, sp, ds, usnMap);
}

QString MftReader::getName(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_name_offsets.size()) return QString();
    return QString::fromUtf8(reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[index]));
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
    return (int)m_frns.size();
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

    // 后缀过滤
    if (!extensionList.isEmpty()) {
        bool extMatch = false;
        size_t nameLen = strlen(p);
        for (const QString& ex : extensionList) {
            QByteArray exUtf8 = (ex.startsWith('.') ? ex : "." + ex).toUtf8();
            if (nameLen >= (size_t)exUtf8.size()) {
                if (_stricmp(p + nameLen - exUtf8.size(), exUtf8.constData()) == 0) {
                    extMatch = true;
                    break;
                }
            }
        }
        if (!extMatch) return false;
    }

    if (query.isEmpty()) return true;

    // 内容过滤
    if (useRegex) {
        QRegularExpression re(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        return re.match(QString::fromUtf8(p)).hasMatch();
    } else {
        QByteArray queryUtf8 = query.toUtf8();
        if (caseSensitive) {
            return (strstr(p, queryUtf8.constData()) != nullptr);
        } else {
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
    // 2026-05-29 物理修复：重构锁顺序，先持有 m_dataLock，内部调用无锁版本的 getPathFastInternal
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_frns.size()) return QString();
    uint64_t frn = m_frns[index];
    size_t dIdx = static_cast<size_t>(m_parent_frns[index] >> 48);
    return QString::fromStdWString(const_cast<MftReader*>(this)->getPathFastInternal(dIdx, frn));
}

std::wstring MftReader::getPathFast(size_t driveIdx, uint64_t frn) {
    // 2026-05-29 物理修复：公开接口持有读锁，内部调用私有无锁逻辑，解决递归死锁。
    QReadLocker readLock(&m_dataLock);
    return getPathFastInternal(driveIdx, frn);
}

std::wstring MftReader::getPathFastInternal(size_t driveIdx, uint64_t frn) {
    // 2026-05-29 物理修复：内部无锁实现。调用方必须已持有 m_dataLock。
    uint64_t compositeKey = (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);

    {
        std::lock_guard<std::mutex> lock(m_pathCacheMutex);
        auto it = m_path_cache.find(compositeKey);
        if (it != m_path_cache.end()) return it->second;
    }

    if (!m_isInitialized) return L"";

    std::vector<std::wstring> segments;
    uint64_t cur = frn;
    std::unordered_set<uint64_t> vis;
    while (true) {
        uint64_t curKey = (static_cast<uint64_t>(driveIdx) << 48) | (cur & 0x0000FFFFFFFFFFFFull);
        auto idxIt = m_frn_to_idx.find(curKey);
        if (idxIt == m_frn_to_idx.end() || vis.count(cur)) break;
        vis.insert(cur);

        uint32_t idx = idxIt->second;
        const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[idx]);
        segments.push_back(QString::fromUtf8(p).toStdWString());

        uint64_t parent = m_parent_frns[idx] & 0x0000FFFFFFFFFFFFull;
        if (parent == 5 || parent == cur || parent == 0) break;
        cur = parent;
    }

    if (segments.empty()) return L"";

    std::wstring volume = (driveIdx < m_drive_list.size()) ? m_drive_list[driveIdx] : L"C:";
    std::wstring path = volume;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) path += L"\\" + *it;

    {
        std::lock_guard<std::mutex> lock(m_pathCacheMutex);
        if (m_path_cache.size() > 200000) { 
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
    QReadLocker lock(&m_dataLock);
    if (!m_isInitialized) return {};

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

    std::vector<QByteArray> extUtf8;
    if (hasExt) {
        for (const QString& ex : extensionList) {
            QString normalized = (ex.startsWith('.') ? ex : "." + ex);
            extUtf8.push_back(normalized.toUtf8());
        }
    }

    std::mutex mtx;
    std::vector<uint64_t> finalRes;
    finalRes.reserve(m_frns.size() / 16);

    size_t total = m_frns.size();
    const size_t grainSize = 4096;
    size_t numChunks = (total + grainSize - 1) / grainSize;
    std::vector<size_t> chunkIndices(numChunks);
    std::iota(chunkIndices.begin(), chunkIndices.end(), 0);

    // 2026-05-29 极致算法重构：返回稳定的复合 FRN 主键
    if (hasQuery && !useRegex && !caseSensitive && !hasExt) {
        auto it_start = std::lower_bound(m_sorted_indices.begin(), m_sorted_indices.end(), queryUtf8.constData(), 
            [this](uint32_t idx, const char* q) {
                const char* name = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[idx]);
                return _strnicmp(name, q, strlen(q)) < 0;
            });
        
        for (auto it = it_start; it != m_sorted_indices.end(); ++it) {
            uint32_t i = *it;
            if (m_frns[i] == 0) continue;
            
            const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
            if (_strnicmp(p, queryUtf8.constData(), queryUtf8.size()) != 0) break; 

            // $ 过滤
            if (!includeDollar && p[0] == '$') continue;

            size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
            if (dIdx >= 32 || !(m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) continue;
            
            uint32_t at = m_attributes[i];
            if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;

            finalRes.push_back(makeKey(dIdx, m_frns[i]));
            if (finalRes.size() > 200000) break; 
        }

        for (size_t i = m_sorted_indices.size(); i < m_frns.size(); ++i) {
            if (m_frns[i] == 0) continue;
            const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
            if (_strnicmp(p, queryUtf8.constData(), queryUtf8.size()) == 0) {
                // $ 过滤
                if (!includeDollar && p[0] == '$') continue;

                size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
                if (dIdx >= 32 || !(m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) continue;
                uint32_t at = m_attributes[i];
                if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
                if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;
                finalRes.push_back(makeKey(dIdx, m_frns[i]));
            }
        }
    } else {
        std::for_each((std::execution::par), chunkIndices.begin(), chunkIndices.end(), [&](size_t chunkIdx) {
            std::vector<uint64_t> localRes;
            localRes.reserve(grainSize / 16);
            size_t startPos = chunkIdx * grainSize;
            size_t endPos = (std::min)(startPos + grainSize, total);

            for (size_t i = startPos; i < endPos; ++i) {
                if (m_frns[i] == 0) continue;
                
                size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
                if (dIdx >= 32 || !(m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) continue;

                uint32_t at = m_attributes[i];
                if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
                if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;

                const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
                
                // $ 过滤
                if (!includeDollar && p[0] == '$') continue;

                if (!hasQuery && !hasExt) {
                    localRes.push_back(makeKey(dIdx, m_frns[i]));
                    continue;
                }

                if (hasExt) {
                    bool extMatch = false;
                    size_t nameLen = strlen(p);
                    for (const auto& ex : extUtf8) {
                        if (nameLen >= (size_t)ex.size()) {
                            if (_stricmp(p + nameLen - ex.size(), ex.constData()) == 0) {
                                extMatch = true;
                                break;
                            }
                        }
                    }
                    if (!extMatch) continue;
                }

                if (!hasQuery) {
                    localRes.push_back(makeKey(dIdx, m_frns[i]));
                } else {
                    bool match = false;
                    if (useRegex) {
                        match = re.match(QString::fromUtf8(p)).hasMatch();
                    } else {
                        if (caseSensitive) {
                            match = (strstr(p, queryUtf8.constData()) != nullptr);
                        } else {
                            match = (StrStrIA(p, queryUtf8.constData()) != nullptr);
                        }
                    }
                    if (match) localRes.push_back(makeKey(dIdx, m_frns[i]));
                }
            }
            if (!localRes.empty()) { std::lock_guard<std::mutex> l(mtx); finalRes.insert(finalRes.end(), localRes.begin(), localRes.end()); }
        });
    }
    return finalRes;
}

void MftReader::updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume) {
    USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(record);
    uint64_t frn, parentFrn;
    uint32_t attr;
    LARGE_INTEGER timestamp;
    WORD fileNameLength, fileNameOffset;

    // 2026-05-14 核心排查：针对 V2 (64bit FRN) 和 V3 (128bit FRN) 进行严格的偏移匹配
    if (header->MajorVersion == 2) {
        frn = record->FileReferenceNumber;
        parentFrn = record->ParentFileReferenceNumber;
        attr = record->FileAttributes;
        timestamp = record->TimeStamp;
        fileNameLength = record->FileNameLength;
        fileNameOffset = record->FileNameOffset;
    } else if (header->MajorVersion == 3) {
        // 手动映射 V3 布局，避免 SDK 定义缺失导致的读取错误
        struct V3_LAYOUT {
            DWORD RecordLength; WORD MajorVersion; WORD MinorVersion;
            BYTE FileReferenceNumber[16]; BYTE ParentFileReferenceNumber[16];
            USN Usn; LARGE_INTEGER TimeStamp; DWORD Reason; DWORD SourceInfo;
            DWORD SecurityId; DWORD FileAttributes; WORD FileNameLength; WORD FileNameOffset;
        } *v3 = reinterpret_cast<V3_LAYOUT*>(record);
        frn = *reinterpret_cast<uint64_t*>(v3->FileReferenceNumber);
        parentFrn = *reinterpret_cast<uint64_t*>(v3->ParentFileReferenceNumber);
        attr = v3->FileAttributes;
        timestamp = v3->TimeStamp;
        fileNameLength = v3->FileNameLength;
        fileNameOffset = v3->FileNameOffset;
    } else return;

    uint64_t fileSize = 0;
    int64_t finalModifyTime = filetimeToUnixMs(timestamp.QuadPart);
    uint32_t finalAttr = attr;
    bool fetchedSuccess = false;

    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring rootPath = volume + L"\\";
        // 2026-05-14 修正：hHint 需要 FILE_READ_ATTRIBUTES 权限来辅助 OpenFileById
        HANDLE hHint = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hHint != INVALID_HANDLE_VALUE) {
            FILE_ID_DESCRIPTOR id = {0};
            id.dwSize = sizeof(FILE_ID_DESCRIPTOR);
            id.Type = FileIdType;
            id.FileId.QuadPart = frn;
            // 2026-05-14 核心修正：OpenFileById 的 DesiredAccess 不能为 0，必须至少为 FILE_READ_ATTRIBUTES 才能获取文件大小
            HANDLE hFile = OpenFileById(hHint, &id, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, FILE_FLAG_BACKUP_SEMANTICS);
            if (hFile != INVALID_HANDLE_VALUE) {
                BY_HANDLE_FILE_INFORMATION bhfi;
                if (GetFileInformationByHandle(hFile, &bhfi)) {
                    fileSize = (static_cast<uint64_t>(bhfi.nFileSizeHigh) << 32) | bhfi.nFileSizeLow;
                    finalAttr = bhfi.dwFileAttributes;
                    finalModifyTime = filetimeToUnixMs((static_cast<int64_t>(bhfi.ftLastWriteTime.dwHighDateTime) << 32) | bhfi.ftLastWriteTime.dwLowDateTime);
                    fetchedSuccess = true;
                }
                CloseHandle(hFile);
            }
            CloseHandle(hHint);
        }
    }

    QWriteLocker lock(&m_dataLock);
    QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(record) + fileNameOffset), fileNameLength / 2);
    size_t dIdx = 0;
    for (size_t i = 0; i < m_drive_list.size(); ++i) { if (m_drive_list[i] == volume) { dIdx = i; break; } }
    uint64_t encodedPf = makeKey(dIdx, parentFrn);
    uint64_t compositeKey = makeKey(dIdx, frn);
    auto it = m_frn_to_idx.find(compositeKey);
    if (it != m_frn_to_idx.end()) {
        uint32_t idx = it->second;
        m_parent_frns[idx] = encodedPf;
        m_attributes[idx] = finalAttr;
        m_metadata_fetched[idx] = fetchedSuccess ? 2 : 0;
        
        // 2026-05-14 逻辑加固：优先使用 API 获取的属性，若失败则回退至 USN 提供的时间戳
        m_sizes[idx] = fileSize;
        m_timestamps[idx] = (finalModifyTime > 0) ? finalModifyTime : filetimeToUnixMs(timestamp.QuadPart);

        QByteArray utf8 = name.toUtf8();
        uint32_t oldOff = m_name_offsets[idx];
        const char* oldPtr = reinterpret_cast<const char*>(m_string_pool.data() + oldOff);
        size_t oldLen = strlen(oldPtr);
        if ((size_t)utf8.size() <= oldLen) {
            memcpy(m_string_pool.data() + oldOff, utf8.constData(), utf8.size());
            m_string_pool[oldOff + utf8.size()] = '\0';
            if ((size_t)utf8.size() < oldLen) m_wasted_string_bytes += (oldLen - utf8.size());
        } else {
            m_wasted_string_bytes += (oldLen + 1);
            m_name_offsets[idx] = (uint32_t)m_string_pool.size();
            m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
            m_string_pool.push_back('\0');
        }
    } else {
        uint32_t newIdx = (uint32_t)m_frns.size();
        m_frns.push_back(frn);
        m_parent_frns.push_back(encodedPf);
        m_sizes.push_back(fileSize);
        m_timestamps.push_back(finalModifyTime);
        m_attributes.push_back(finalAttr);
        m_metadata_fetched.push_back(fetchedSuccess ? 2 : 0);
        QByteArray utf8 = name.toUtf8();
        m_name_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
        m_string_pool.push_back('\0');
        m_frn_to_idx[compositeKey] = newIdx;
        if (dIdx < 32) m_drive_entry_indices[dIdx].push_back(newIdx);
    }
    { std::lock_guard<std::mutex> l(m_pathCacheMutex); m_path_cache.erase(compositeKey); }
    m_next_usns[volume] = record->Usn;
    if (dIdx < 32) m_drive_dirty_counts[dIdx]++;
    
    // 2026-05-14 工业级内存加固：实时监控内存碎片率
    // 当浪费的字符串空间超过 20MB 或死亡条目过多时，强制执行 compact 碎片整理
    if (m_wasted_string_bytes > 20 * 1024 * 1024 || m_dead_count > 100000) {
        compact();
    }

    bool shouldSave = false;
    if (dIdx < 32 && m_drive_dirty_counts[dIdx].load() >= 1000) { 
        m_drive_dirty_counts[dIdx] = 0; 
        shouldSave = true;
    }
    
    int finalIdx = -1;
    bool isNew = (it == m_frn_to_idx.end());
    if (!isNew) finalIdx = (int)it->second;
    else finalIdx = (int)m_frns.size() - 1;

    lock.unlock(); // 2026-05-29 物理安全：先解锁再发射信号，杜绝 DirectConnection 导致的跨模块死锁

    if (shouldSave) {
        // 工业级架构优化：将耗时 I/O 持久化逻辑异步执行，杜绝阻塞 USN 监控主循环与 UI 响应
        // 增加 CAS 原子操作防止并发写盘竞争，确保单盘持久化原子性
        bool expected = false;
        if (m_is_saving.compare_exchange_strong(expected, true)) {
            // 2026-05-29 工业级警告消除：明确丢弃 QFuture 返回值，满足 MSVC C4858 规范
            (void)QtConcurrent::run([this, dIdx]() {
                try {
                    saveDriveToCache(dIdx); 
                } catch (...) {
                    qWarning() << "[MftReader] 后台存盘发生未知异常";
                }
                m_is_saving.store(false);
            });
        }
    }

    if (isNew) {
        emit entryAdded(compositeKey);
    } else {
        emit entryUpdated(compositeKey);
    }
    emit dataChanged(finalIdx);
}

void MftReader::updateEntriesFromUsnBatch(const std::vector<USN_RECORD_V2*>& records, const std::wstring& volume) {
    if (records.empty()) return;
    
    // 工业级重构：批量更新模式，大幅降低 QWriteLocker 的竞争频率
    QWriteLocker lock(&m_dataLock);
    
    size_t dIdx = 0;
    bool driveFound = false;
    for (size_t i = 0; i < m_drive_list.size(); ++i) { 
        if (m_drive_list[i] == volume) { 
            dIdx = i; 
            driveFound = true;
            break; 
        } 
    }
    if (!driveFound) return;

    std::vector<uint64_t> addedKeys;
    std::vector<uint64_t> updatedKeys;

    for (USN_RECORD_V2* record : records) {
        USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(record);
        uint64_t frn, parentFrn;
        uint32_t attr;
        LARGE_INTEGER timestamp;
        WORD fileNameLength, fileNameOffset;

        if (header->MajorVersion == 2) {
            frn = record->FileReferenceNumber;
            parentFrn = record->ParentFileReferenceNumber;
            attr = record->FileAttributes;
            timestamp = record->TimeStamp;
            fileNameLength = record->FileNameLength;
            fileNameOffset = record->FileNameOffset;
        } else if (header->MajorVersion == 3) {
            struct V3_LAYOUT {
                DWORD RecordLength; WORD MajorVersion; WORD MinorVersion;
                BYTE FileReferenceNumber[16]; BYTE ParentFileReferenceNumber[16];
                USN Usn; LARGE_INTEGER TimeStamp; DWORD Reason; DWORD SourceInfo;
                DWORD SecurityId; DWORD FileAttributes; WORD FileNameLength; WORD FileNameOffset;
            } *v3 = reinterpret_cast<V3_LAYOUT*>(record);
            frn = *reinterpret_cast<uint64_t*>(v3->FileReferenceNumber);
            parentFrn = *reinterpret_cast<uint64_t*>(v3->ParentFileReferenceNumber);
            attr = v3->FileAttributes;
            timestamp = v3->TimeStamp;
            fileNameLength = v3->FileNameLength;
            fileNameOffset = v3->FileNameOffset;
        } else continue;

        int64_t finalModifyTime = filetimeToUnixMs(timestamp.QuadPart);
        uint32_t finalAttr = attr;

        // 批量模式下暂不执行耗时的 OpenFileById 同步拉取，交由异步 Metadata 队列处理
        // 这样可以确保 USN 监控线程以最高速吞噬日志
        
        QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(record) + fileNameOffset), fileNameLength / 2);
        uint64_t encodedPf = makeKey(dIdx, parentFrn);
        uint64_t compositeKey = makeKey(dIdx, frn);
        
        auto it = m_frn_to_idx.find(compositeKey);
        if (it != m_frn_to_idx.end()) {
            uint32_t idx = it->second;
            m_parent_frns[idx] = encodedPf;
            m_attributes[idx] = finalAttr;
            m_metadata_fetched[idx] = 0; // 标记为未获取，待后续异步补全
            m_sizes[idx] = 0;
            m_timestamps[idx] = finalModifyTime;

            QByteArray utf8 = name.toUtf8();
            uint32_t oldOff = m_name_offsets[idx];
            const char* oldPtr = reinterpret_cast<const char*>(m_string_pool.data() + oldOff);
            size_t oldLen = strlen(oldPtr);
            if ((size_t)utf8.size() <= oldLen) {
                memcpy(m_string_pool.data() + oldOff, utf8.constData(), utf8.size());
                m_string_pool[oldOff + utf8.size()] = '\0';
                if ((size_t)utf8.size() < oldLen) m_wasted_string_bytes += (oldLen - utf8.size());
            } else {
                m_wasted_string_bytes += (oldLen + 1);
                m_name_offsets[idx] = (uint32_t)m_string_pool.size();
                m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
                m_string_pool.push_back('\0');
            }
            updatedKeys.push_back(compositeKey);
        } else {
            uint32_t newIdx = (uint32_t)m_frns.size();
            m_frns.push_back(frn);
            m_parent_frns.push_back(encodedPf);
            m_sizes.push_back(0);
            m_timestamps.push_back(finalModifyTime);
            m_attributes.push_back(finalAttr);
            m_metadata_fetched.push_back(0);
            QByteArray utf8 = name.toUtf8();
            m_name_offsets.push_back((uint32_t)m_string_pool.size());
            m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
            m_string_pool.push_back('\0');
            m_frn_to_idx[compositeKey] = newIdx;
            if (dIdx < 32) m_drive_entry_indices[dIdx].push_back(newIdx);
            addedKeys.push_back(compositeKey);
        }
        { std::lock_guard<std::mutex> l(m_pathCacheMutex); m_path_cache.erase(compositeKey); }
        m_next_usns[volume] = record->Usn;
        if (dIdx < 32) m_drive_dirty_counts[dIdx]++;
    }

    if (m_wasted_string_bytes > 20 * 1024 * 1024 || m_dead_count > 100000) {
        compact();
    }

    bool shouldSave = false;
    if (dIdx < 32 && m_drive_dirty_counts[dIdx].load() >= 1000) { 
        m_drive_dirty_counts[dIdx] = 0; 
        shouldSave = true;
    }

    lock.unlock();

    if (shouldSave) {
        bool expected = false;
        if (m_is_saving.compare_exchange_strong(expected, true)) {
            (void)QtConcurrent::run([this, dIdx]() {
                try {
                    saveDriveToCache(dIdx); 
                } catch (...) {
                    qWarning() << "[MftReader] 批量更新后台存盘发生未知异常";
                }
                m_is_saving.store(false);
            });
        }
    }

    // 发射批量信号 (UI 侧可以根据需要合并处理)
    for (uint64_t key : addedKeys) emit entryAdded(key);
    for (uint64_t key : updatedKeys) emit entryUpdated(key);
    emit dataChanged(-1);
}

void MftReader::removeEntryByFrn(const std::wstring& volume, uint64_t frn) {
    QWriteLocker lock(&m_dataLock);
    size_t dIdx = 0;
    for (size_t i = 0; i < m_drive_list.size(); ++i) { if (m_drive_list[i] == volume) { dIdx = i; break; } }
    uint64_t compositeKey = makeKey(dIdx, frn);

    auto it = m_frn_to_idx.find(compositeKey);
    if (it != m_frn_to_idx.end()) {
        uint32_t idx = it->second;
        m_frns[idx] = 0;
        m_frn_to_idx.erase(it);
        m_dead_count++;
        const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[idx]);
        m_wasted_string_bytes += (strlen(p) + 1);
        
        { std::lock_guard<std::mutex> l(m_pathCacheMutex); m_path_cache.erase(compositeKey); }
        
        if (m_dead_count > 50000 || m_wasted_string_bytes > 10 * 1024 * 1024) {
            compact();
        }
        
        lock.unlock(); // 物理安全：解锁后再发射信号
        emit entryRemoved(compositeKey);
        emit dataChanged(-1);
    }
}

void MftReader::compact() {
    // 2026-05-14 内存管理优化：执行碎片整理，回收无效条目和字符串池空间
    std::vector<uint64_t>  new_frns;
    std::vector<uint64_t>  new_parent_frns;
    std::vector<int64_t>   new_sizes;
    std::vector<int64_t>   new_timestamps;
    std::vector<uint32_t>  new_name_offsets;
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
    for (int i = 0; i < 32; ++i) m_drive_entry_indices[i].clear();

    for (size_t i = 0; i < count; ++i) {
        if (m_frns[i] == 0) continue;
        
        uint32_t newIdx = (uint32_t)new_frns.size();
        size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
        uint64_t key = makeKey(dIdx, m_frns[i]);
        m_frn_to_idx[key] = newIdx;
        if (dIdx < 32) m_drive_entry_indices[dIdx].push_back(newIdx);
        
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
    }

    m_frns = std::move(new_frns);
    m_parent_frns = std::move(new_parent_frns);
    m_sizes = std::move(new_sizes);
    m_timestamps = std::move(new_timestamps);
    m_name_offsets = std::move(new_name_offsets);
    m_attributes = std::move(new_attributes);
    m_metadata_fetched = std::move(new_metadata_fetched);
    m_string_pool = std::move(new_string_pool);

    m_dead_count = 0;
    m_wasted_string_bytes = 0;
    rebuildFrnToIndexMap();
    buildSortedIndices();
}

bool MftReader::loadMftDirect(const std::wstring& volume, MftReader::DriveResult& result) {
    // 极致工业级优化方案 C：SoA 结构内存预分配优化
    // 预估 NTFS 卷的文件数量，提前 reserve 以减少动态扩容带来的内存碎片与拷贝开销
    std::wstring dev = L"\\\\.\\" + volume;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    // 2026-05-14 获取根目录句柄作为 Hint，这对于 OpenFileById 的稳定性至关重要
    std::wstring rootPath = volume + L"\\";
    // 修正：赋予 FILE_READ_ATTRIBUTES 权限
    HANDLE hHint = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

    USN_JOURNAL_DATA_V0 j; DWORD cb;
    if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &j, sizeof(j), &cb, NULL)) { 
        if (hHint != INVALID_HANDLE_VALUE) CloseHandle(hHint);
        CloseHandle(h); return false; 
    }
    result.nextUsn = j.NextUsn;

    // 工业级启发式预分配：根据日记账条目数预估文件总数，平均 150 字节一个条目
    size_t estimatedCount = static_cast<size_t>(j.NextUsn / 150);
    if (estimatedCount > 100000) result.entries.reserve(estimatedCount);

    MFT_ENUM_DATA_V0 ed = {0}; ed.HighUsn = j.NextUsn;
    std::vector<uint8_t> buf(1024 * 1024);
    while (DeviceIoControl(h, FSCTL_ENUM_USN_DATA, &ed, sizeof(ed), buf.data(), (DWORD)buf.size(), &cb, NULL)) {
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

            // 2026-05-14 极致性能优化：全量扫描阶段仅获取核心字段，将重量级 I/O 转移至延迟补全队列
            MftReader::RawEntry e; 
            e.frn = frn; 
            e.parentFrn = parentFrn;
            e.size = 0; 
            e.attributes = attr;
            e.modifyTime = filetimeToUnixMs(timestamp.QuadPart);
            
            QString n = QString::fromUtf16(reinterpret_cast<const char16_t*>(p + fileNameOffset), fileNameLength / 2);
            e.nameUtf8 = n.toUtf8().toStdString();
            result.entries.push_back(std::move(e));
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
    m_attributes.reserve(m_attributes.size() + count);
    m_metadata_fetched.reserve(m_metadata_fetched.size() + count);
    
    // 方案三：精准写入优化
    if (driveIdx < 32) {
        m_drive_entry_indices[driveIdx].reserve(m_drive_entry_indices[driveIdx].size() + count);
    }

    for (const auto& e : result.entries) {
        uint32_t newIdx = (uint32_t)m_frns.size();
        m_frns.push_back(e.frn);
        m_parent_frns.push_back((static_cast<uint64_t>(driveIdx) << 48) | (e.parentFrn & 0x0000FFFFFFFFFFFFull));
        m_sizes.push_back(e.size); // 2026-05-14 修正：将扫描到的大小压入 SoA
        m_timestamps.push_back(e.modifyTime); m_attributes.push_back(e.attributes);
        m_metadata_fetched.push_back(0);
        m_name_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), e.nameUtf8.begin(), e.nameUtf8.end());
        m_string_pool.push_back('\0');

        if (driveIdx < 32) {
            m_drive_entry_indices[driveIdx].push_back(newIdx);
        }
    }
}

void MftReader::rebuildFrnToIndexMap() {
    m_frn_to_idx.clear();
    for (int i = 0; i < 32; ++i) m_drive_entry_indices[i].clear();

    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] != 0) {
            size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
            uint64_t key = makeKey(dIdx, m_frns[i]);
            m_frn_to_idx[key] = (uint32_t)i;
            if (dIdx < 32) m_drive_entry_indices[dIdx].push_back((uint32_t)i);
        }
    }
}

void MftReader::buildSortedIndices() {
    // 2026-05-14 性能增强：构建预排序索引，支持二分查找 O(log N)
    m_sorted_indices.resize(m_frns.size());
    std::iota(m_sorted_indices.begin(), m_sorted_indices.end(), 0);
    std::sort((std::execution::par), m_sorted_indices.begin(), m_sorted_indices.end(), [this](uint32_t a, uint32_t b) {
        const char* s1 = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[a]);
        const char* s2 = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[b]);
        return _stricmp(s1, s2) < 0;
    });
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

    (void)QtConcurrent::run([this, index, frn, volume]() {
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

} // namespace ArcMeta
