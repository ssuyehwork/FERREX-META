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
#include <queue>

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
    if (filetime <= 116444736000000000LL) return 0;
    return (filetime - 116444736000000000LL) / 10000LL;
}

static bool enablePrivilege(LPCWSTR privilege) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        qDebug() << "[MftReader] OpenProcessToken 失败，无法提升权限:" << QString::fromWCharArray(privilege);
        return false;
    }
    LUID luid;
    if (!LookupPrivilegeValue(NULL, privilege, &luid)) {
        qDebug() << "[MftReader] LookupPrivilegeValue 失败:" << QString::fromWCharArray(privilege);
        CloseHandle(hToken);
        return false;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) {
        qDebug() << "[MftReader] AdjustTokenPrivileges 失败:" << QString::fromWCharArray(privilege);
        CloseHandle(hToken);
        return false;
    }
    bool ok = (GetLastError() == ERROR_SUCCESS);
    if (!ok) {
        qDebug() << "[MftReader] 权限提升失败 (可能由于非管理员运行):" << QString::fromWCharArray(privilege);
    } else {
        qDebug() << "[MftReader] 权限提升成功:" << QString::fromWCharArray(privilege);
    }
    CloseHandle(hToken);
    return ok;
}

MftReader& MftReader::instance() {
    static MftReader inst;
    static std::once_flag flag;
    std::call_once(flag, []() {
        qDebug() << "[MftReader] 主线程单例预热启动，开始尝试提升系统备份与恢复权限...";
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
    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_metadata_queue.clear();
    }
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
    {
        QWriteLocker lock(&m_dataLock);
        if (!m_isInitialized || m_is_clearing.load()) return;
        m_is_clearing.store(true);
        m_abort_scan.store(true);
        m_isInitialized = false; 
    }

    qDebug() << "[MftReader] 正在清理底层资源，准备停止监控线程并保存游标...";

    std::vector<UsnWatcher*> toStop;
    {
        QWriteLocker lock(&m_dataLock);
        toStop = std::move(m_watchers);
        m_watchers.clear();
    }
    for (auto* w : toStop) { if (w) { w->stop(); delete w; } }

    (void)QtConcurrent::run([this]() {
        while (m_is_saving.load(std::memory_order_acquire)) {
            QThread::msleep(10);
        }

        bool hasDirty = false;
        for (int i = 0; i < 32; ++i) {
            if (m_drive_dirty_counts[i].load() > 0) { hasDirty = true; break; }
        }
        if (hasDirty) {
            saveToCache();
        }

        {
            QWriteLocker lock(&m_dataLock);
            clearInternal();
            m_is_clearing.store(false);
        }
        qDebug() << "[MftReader] 底层清理释放结束。";
    });
}

void MftReader::updateActiveDrives(const QStringList& activeDrives) {
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
    qDebug() << "[MftReader] 更新活跃驱动器掩码为:" << mask;
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
    qDebug() << "[MftReader] 收到构建索引指令！盘符列表:" << drives;
    
    // 【核心解耦修复】：
    // 将原本一个巨大的、同步连坐阻塞大列表扫描拆分为针对每一个单盘符的并行异步任务，实现“即扫即启”
    for (const QString& d : drives) {
        std::wstring vol = d.toStdWString();
        if (vol.size() > 1 && (vol.back() == L'\\' || vol.back() == L'/')) vol.pop_back();

        // 采用 C++11 Lambda 直接闭包捕获 MftReader 私有成员，完美避开修改 MftReader.h 头文件
        (void)QtConcurrent::run([this, vol]() {
            qDebug() << "[MftReader] [单盘异步] 启动针对盘符" << QString::fromStdWString(vol) << "的扫描与监控线程...";

            // 1. 判断是否已经在内存列表中
            {
                QReadLocker lock(&m_dataLock);
                for (const auto& indexedVol : m_drive_list) {
                    if (_wcsicmp(indexedVol.c_str(), vol.c_str()) == 0) {
                        qDebug() << "[MftReader] [单盘异步] 盘符" << QString::fromStdWString(vol) << "已在索引中，跳过。";
                        return;
                    }
                }
            }

            // 2. 扫盘
            MftReader::DriveResult res; 
            bool success = loadMftDirect(vol, res);

            if (!success || res.entries.empty()) {
                qDebug() << "[MftReader] [单盘异步] [警告] 盘符" << QString::fromStdWString(vol) << "扫盘失败或记录为空，该盘退出初始化。";
                return;
            }

            qDebug() << "[MftReader] [单盘异步] 盘符" << QString::fromStdWString(vol) << "物理扫描大获成功！解析出条目数:" << res.entries.size() << "。开始锁定写入内存 SoA 容器...";

            UsnWatcher* watcherToStart = nullptr;
            size_t dIdx = 0;

            // 3. 内存原子合并
            {
                QWriteLocker lock(&m_dataLock);
                
                // 并发双重安全防御（Double Check）
                bool found = false;
                for (size_t i = 0; i < m_drive_list.size(); ++i) {
                    if (_wcsicmp(m_drive_list[i].c_str(), vol.c_str()) == 0) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    qDebug() << "[MftReader] [单盘异步] 盘符" << QString::fromStdWString(vol) << "在扫盘期间已被其他并发线程合并，跳过内存。";
                    return;
                }

                dIdx = m_drive_list.size();
                m_drive_list.push_back(vol);
                if (dIdx < 32) {
                    m_drive_active_mask.fetch_or(1 << dIdx);
                }
                m_next_usns[vol] = res.nextUsn;
                mergeDriveResult(vol, res, dIdx);
                saveDriveToCacheInternal(dIdx);
                
                rebuildFrnToIndexMap();
                buildSortedIndices();
                m_isInitialized = true;

                qDebug() << "[MftReader] [单盘异步] 盘符" << QString::fromStdWString(vol) << "内存SoA合并、树型反查图建立、缓存SCCH落盘成功！正在实例化 USN 监控对象...";
                watcherToStart = new UsnWatcher(vol, res.nextUsn, nullptr);
                m_watchers.push_back(watcherToStart);
            }

            // 4. 立即激活实时 USN 监听，无需等待其它慢盘
            if (watcherToStart) {
                qDebug() << "[MftReader] [单盘异步] 抢跑机制生效！正在单独、立即激活实时监控线程:" << QString::fromStdWString(vol);
                watcherToStart->start();
            }
        });
    }
}

bool MftReader::loadFromCache() {
    std::filesystem::path cacheDir = "ArcMeta/cache";
    qDebug() << "[MftReader] 开始检查本地缓存..." << QString::fromStdWString(cacheDir.wstring());
    
    // 如果缓存目录完全不存在，我们直接创建它
    if (!std::filesystem::exists(cacheDir)) {
        std::filesystem::create_directories(cacheDir);
        qDebug() << "[MftReader] 本地缓存目录不存在，已自动创建目录:" << QString::fromStdWString(cacheDir.wstring());
    }

    // 检查缓存目录下是否有有效的 .scch 缓存文件
    bool hasScchFiles = false;
    for (auto const& entry : std::filesystem::directory_iterator{cacheDir}) {
        if (entry.path().extension() == ".scch") {
            hasScchFiles = true;
            break;
        }
    }

    // 【终极自愈自检】：如果未发现任何缓存文件，说明是首次运行。
    // 我们在此不返回 false 坐以待毙，而是直接自动搜寻所有磁盘中包含 ArcMeta.Library_* 的盘符，并在后台自动触发单盘解耦异步 buildIndex！
    if (!hasScchFiles) {
        qDebug() << "[MftReader] [自愈自检] 未发现任何有效缓存文件！我们将自动搜寻磁盘托管库并强制异步解耦重构索引...";
        
        QStringList drivesToScan;
        const auto drives = QDir::drives();
        for (const QFileInfo& d : drives) {
            QString drivePath = d.absolutePath();
            QDir rootDir(drivePath);
            QStringList entries = rootDir.entryList({"ArcMeta.Library_*"}, QDir::Dirs | QDir::Hidden);
            if (!entries.isEmpty()) {
                QString driveLetter = drivePath.left(2); // 例如 "Z:"
                if (driveLetter.endsWith('/') || driveLetter.endsWith('\\')) {
                    driveLetter.chop(1);
                }
                drivesToScan.push_back(driveLetter);
            }
        }

        qDebug() << "[MftReader] [自愈自检] 自动探测到存在托管库的快/慢盘符列表:" << drivesToScan;
        
        if (!drivesToScan.isEmpty()) {
            // 自愈：异步解耦扫盘
            buildIndex(drivesToScan);
            return true; // 返回 true，欺骗外层已成功加载，由我们内部进行异步盘符自愈
        } else {
            qDebug() << "[MftReader] [自愈自检] 未在任何磁盘发现托管库文件夹，无需构建索引。";
            return false;
        }
    }

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
    int successfullyLoadedCaches = 0;

    for (auto const& entry : std::filesystem::directory_iterator{cacheDir}) {
        if (entry.path().extension() == ".scch") {
            qDebug() << "[MftReader] 发现本地缓存文件，开始加载:" << QString::fromStdString(entry.path().string());
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
                        qDebug() << "[MftReader] 成功还原盘符:" << QString::fromStdWString(driveName) << "上次记录的 NextUsn 指针:" << usn;
                    }
                    currentTotal = m_frns.size();
                }
                
                successfullyLoadedCaches++;
                emit driveLoaded(QString::fromStdWString(driveName), (int)count, (int)currentTotal);
            } else {
                qDebug() << "[MftReader] 警告：加载缓存文件失败:" << QString::fromStdString(entry.path().string());
            }
        }
    }

    QWriteLocker lock(&m_dataLock);
    if (m_frns.empty() || successfullyLoadedCaches == 0) {
        qDebug() << "[MftReader] 本地没有可用的缓存记录，退回全量重扫。";
        return false;
    }
    
    rebuildFrnToIndexMap();

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

    qDebug() << "[MftReader] 缓存装载完毕。当前已载入的物理盘符数量为:" << m_drive_list.size();
    for (const auto& drive : m_drive_list) {
        uint64_t lastUsn = m_next_usns[drive];
        qDebug() << "[MftReader] 正在为已加载驱动器建立 USN 实时监控线程:" << QString::fromStdWString(drive) << "起始 USN 指针:" << lastUsn;
        auto* w = new UsnWatcher(drive, lastUsn, nullptr);
        m_watchers.push_back(w);
        w->start();
    }

    qDebug() << "[MftReader] 缓存加载自愈监控初始化全部就绪。当前监控线程总数:" << m_watchers.size();
    return true;
}

bool MftReader::saveToCache() {
    qDebug() << "[MftReader] 触发全局持久化存盘...";
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

    for (const auto& snap : snapshots) {
        if (snap.f.empty()) continue;
        std::unordered_map<std::string, uint64_t> usnMap;
        usnMap[QString::fromStdWString(snap.volume).toStdString()] = snap.usn;
        QString path = QString("ArcMeta/cache/%1.scch").arg(QString::fromStdWString(snap.volume).left(1));
        ScchCache::save(path.toStdString().c_str(), snap.f, snap.pf, snap.s, snap.t, snap.no, snap.attr, snap.mf, snap.sp, snap.ds, usnMap);
    }
    qDebug() << "[MftReader] 物理缓存存盘写入结束。";
    return true;
}

bool MftReader::saveDriveToCache(size_t driveIdx) {
    return saveDriveToCacheInternal(driveIdx);
}

bool MftReader::saveDriveToCacheInternal(size_t driveIdx) {
    std::wstring volume;
    std::vector<uint64_t> f, pf;
    std::vector<int64_t> s, t;
    std::vector<uint32_t> no, attr, ds;
    std::vector<uint8_t> sp, mf;
    uint64_t nextUsnVal = 0;

    {
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

    std::unordered_map<std::string, uint64_t> usnMap;
    usnMap[QString::fromStdWString(volume).toStdString()] = nextUsnVal;
    QString path = QString("ArcMeta/cache/%1.scch").arg(QString::fromStdWString(volume).left(1));
    
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

uint64_t MftReader::getParentFrnByFrn(uint64_t frn, int driveIdx) const {
    QReadLocker lock(&m_dataLock);
    uint64_t compositeKey = (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);
    auto it = m_frn_to_idx.find(compositeKey);
    if (it != m_frn_to_idx.end()) {
        return m_parent_frns[it->second] & 0x0000FFFFFFFFFFFFull;
    }
    return 0;
}

bool MftReader::matchEntry(int i, const QString& query, bool useRegex, bool caseSensitive, 
                          const QStringList& extensionList, bool includeHidden, bool includeSystem,
                          bool includeDollar) const {
    QReadLocker lock(&m_dataLock);
    if (i < 0 || i >= (int)m_frns.size() || m_frns[i] == 0) return false;

    size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
    if (dIdx >= 32 || !(m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) return false;

    uint32_t at = m_attributes[i];
    if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) return false;
    if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) return false;

    const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);

    if (!includeDollar && p[0] == '$') return false;

    if (query.isEmpty() && extensionList.isEmpty()) return true;

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
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_frns.size()) return QString();
    uint64_t frn = m_frns[index];
    size_t dIdx = static_cast<size_t>(m_parent_frns[index] >> 48);
    return QString::fromStdWString(const_cast<MftReader*>(this)->getPathFastInternal(dIdx, frn));
}

std::wstring MftReader::getPathFast(size_t driveIdx, uint64_t frn) {
    QReadLocker readLock(&m_dataLock);
    return getPathFastInternal(driveIdx, frn);
}

std::wstring MftReader::getPathFastInternal(size_t driveIdx, uint64_t frn) {
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

void MftReader::updateEntryFromUsn(uint8_t* recordPtr, const std::wstring& volume) {
    USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(recordPtr);
    uint64_t frn, parentFrn;
    uint32_t attr;
    LARGE_INTEGER timestamp;
    WORD fileNameLength, fileNameOffset;
    USN usn;

    if (header->MajorVersion == 2) {
        USN_RECORD_V2* record = reinterpret_cast<USN_RECORD_V2*>(recordPtr);
        frn = record->FileReferenceNumber;
        parentFrn = record->ParentFileReferenceNumber;
        attr = record->FileAttributes;
        timestamp = record->TimeStamp;
        fileNameLength = record->FileNameLength;
        fileNameOffset = record->FileNameOffset;
        usn = record->Usn;
    } else if (header->MajorVersion == 3) {
        struct V3_LAYOUT {
            DWORD RecordLength; WORD MajorVersion; WORD MinorVersion;
            BYTE FileReferenceNumber[16]; BYTE ParentFileReferenceNumber[16];
            USN Usn; LARGE_INTEGER TimeStamp; DWORD Reason; DWORD SourceInfo;
            DWORD SecurityId; DWORD FileAttributes; WORD FileNameLength; WORD FileNameOffset;
        } *v3 = reinterpret_cast<V3_LAYOUT*>(recordPtr);
        frn = *reinterpret_cast<uint64_t*>(v3->FileReferenceNumber);
        parentFrn = *reinterpret_cast<uint64_t*>(v3->ParentFileReferenceNumber);
        usn = v3->Usn;
        attr = v3->FileAttributes;
        timestamp = v3->TimeStamp;
        fileNameLength = v3->FileNameLength;
        fileNameOffset = v3->FileNameOffset;
        usn = v3->Usn;
    } else return;

    uint64_t fileSize = 0;
    int64_t finalModifyTime = filetimeToUnixMs(timestamp.QuadPart);
    uint32_t finalAttr = attr;
    bool fetchedSuccess = false;

    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring rootPath = volume + L"\\";
        HANDLE hHint = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hHint != INVALID_HANDLE_VALUE) {
            FILE_ID_DESCRIPTOR id = {0};
            id.dwSize = sizeof(FILE_ID_DESCRIPTOR);
            id.Type = FileIdType;
            id.FileId.QuadPart = frn;
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
    QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(recordPtr + fileNameOffset), fileNameLength / 2);
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
    m_next_usns[volume] = usn;
    if (dIdx < 32) m_drive_dirty_counts[dIdx]++;
    
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

    lock.unlock();

    if (shouldSave) {
        bool expected = false;
        if (m_is_saving.compare_exchange_strong(expected, true)) {
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

void MftReader::updateEntriesFromUsnBatch(const std::vector<uint8_t*>& records, const std::wstring& volume) {
    if (records.empty()) return;
    
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

    for (uint8_t* recordPtr : records) {
        USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(recordPtr);
        uint64_t frn, parentFrn;
        uint32_t attr;
        LARGE_INTEGER timestamp;
        WORD fileNameLength, fileNameOffset;
        USN usn;

        if (header->MajorVersion == 2) {
            USN_RECORD_V2* record = reinterpret_cast<USN_RECORD_V2*>(recordPtr);
            frn = record->FileReferenceNumber;
            parentFrn = record->ParentFileReferenceNumber;
            attr = record->FileAttributes;
            timestamp = record->TimeStamp;
            fileNameLength = record->FileNameLength;
            fileNameOffset = record->FileNameOffset;
            usn = record->Usn;
        } else if (header->MajorVersion == 3) {
            struct V3_LAYOUT {
                DWORD RecordLength; WORD MajorVersion; WORD MinorVersion;
                BYTE FileReferenceNumber[16]; BYTE ParentFileReferenceNumber[16];
                USN Usn; LARGE_INTEGER TimeStamp; DWORD Reason; DWORD SourceInfo;
                DWORD SecurityId; DWORD FileAttributes; WORD FileNameLength; WORD FileNameOffset;
            } *v3 = reinterpret_cast<V3_LAYOUT*>(recordPtr);
            frn = *reinterpret_cast<uint64_t*>(v3->FileReferenceNumber);
            parentFrn = *reinterpret_cast<uint64_t*>(v3->ParentFileReferenceNumber);
            attr = v3->FileAttributes;
            timestamp = v3->TimeStamp;
            fileNameLength = v3->FileNameLength;
            fileNameOffset = v3->FileNameOffset;
            usn = v3->Usn;
        } else continue;

        int64_t finalModifyTime = filetimeToUnixMs(timestamp.QuadPart);
        uint32_t finalAttr = attr;

        QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(recordPtr + fileNameOffset), fileNameLength / 2);
        uint64_t encodedPf = makeKey(dIdx, parentFrn);
        uint64_t compositeKey = makeKey(dIdx, frn);
        
        auto it = m_frn_to_idx.find(compositeKey);
        if (it != m_frn_to_idx.end()) {
            uint32_t idx = it->second;
            m_parent_frns[idx] = encodedPf;
            m_attributes[idx] = finalAttr;
            m_metadata_fetched[idx] = 0; 
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
        m_next_usns[volume] = usn;
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

    if (addedKeys.size() + updatedKeys.size() < 50) {
        for (uint64_t key : addedKeys) emit entryAdded(key);
        for (uint64_t key : updatedKeys) emit entryUpdated(key);
    } else {
        emit dataChanged(-1); 
    }
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
        
        lock.unlock(); 
        emit entryRemoved(compositeKey);
        emit dataChanged(-1);
    }
}

void MftReader::compact() {
    m_is_compacting.store(true);
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
    m_is_compacting.store(false);
}

bool MftReader::loadMftDirect(const std::wstring& volume, MftReader::DriveResult& result) {
    qDebug() << "[MftReader] 开始从物理卷全量读取 MFT。目标卷:" << QString::fromStdWString(volume);
    
    std::wstring dev = L"\\\\.\\" + volume;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        qDebug() << "[MftReader] [错误] 无法打开卷句柄:" << QString::fromStdWString(dev) << "GetLastError:" << GetLastError();
        return false;
    }

    std::wstring rootPath = volume + L"\\";
    HANDLE hHint = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

    USN_JOURNAL_DATA_V0 j; DWORD cb;
    if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &j, sizeof(j), &cb, NULL)) { 
        qDebug() << "[MftReader] [错误] FSCTL_QUERY_USN_JOURNAL 失败！卷名:" << QString::fromStdWString(dev) << "GetLastError:" << GetLastError();
        if (hHint != INVALID_HANDLE_VALUE) CloseHandle(hHint);
        CloseHandle(h); return false; 
    }
    result.nextUsn = j.NextUsn;
    qDebug() << "[MftReader] 成功获取卷" << QString::fromStdWString(volume) << "USN 日志信息。最新 NextUsn:" << j.NextUsn;

    size_t estimatedCount = static_cast<size_t>(j.NextUsn / 150);
    if (estimatedCount > 100000) result.entries.reserve(estimatedCount);

    MFT_ENUM_DATA_V0 ed = {0}; ed.HighUsn = j.NextUsn;
    std::vector<uint8_t> buf(1024 * 1024);
    int stepCount = 0;
    
    while (DeviceIoControl(h, FSCTL_ENUM_USN_DATA, &ed, sizeof(ed), buf.data(), (DWORD)buf.size(), &cb, NULL)) {
        if (m_abort_scan.load()) {
            qDebug() << "[MftReader] 检测到强制终止信号，停止 MFT 扫描！";
            break; 
        }
        if (cb < 8) break;
        stepCount++;
        if (stepCount % 50 == 0) {
            qDebug() << "[MftReader] 正在并行枚举" << QString::fromStdWString(volume) << "MFT 数据块 #" << stepCount;
        }

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
    
    qDebug() << "[MftReader] 卷" << QString::fromStdWString(volume) << "全量 MFT 扫描结束。解析出条目数:" << result.entries.size();
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
    
    if (driveIdx < 32) {
        m_drive_entry_indices[driveIdx].reserve(m_drive_entry_indices[driveIdx].size() + count);
    }

    for (const auto& e : result.entries) {
        uint32_t newIdx = (uint32_t)m_frns.size();
        m_frns.push_back(e.frn);
        m_parent_frns.push_back((static_cast<uint64_t>(driveIdx) << 48) | (e.parentFrn & 0x0000FFFFFFFFFFFFull));
        m_sizes.push_back(e.size); 
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
    m_sorted_indices.resize(m_frns.size());
    std::iota(m_sorted_indices.begin(), m_sorted_indices.end(), 0);
    std::sort((std::execution::par), m_sorted_indices.begin(), m_sorted_indices.end(), [this](uint32_t a, uint32_t b) {
        const char* s1 = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[a]);
        const char* s2 = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[b]);
        return _stricmp(s1, s2) < 0;
    });
}

void MftReader::requestMetadata(int index) {
    {
        QReadLocker readLock(&m_dataLock);
        if (index < 0 || index >= (int)m_frns.size() || m_frns[index] == 0) return;
        if (m_metadata_fetched[index] != 0) return; 
    }

    QWriteLocker writeLock(&m_dataLock);
    if (index < 0 || index >= (int)m_frns.size() || m_frns[index] == 0) return;
    
    if (m_metadata_fetched[index] != 0) return; 
    m_metadata_fetched[index] = 1; 

    uint64_t frn = m_frns[index];
    size_t dIdx = static_cast<size_t>(m_parent_frns[index] >> 48);
    if (dIdx >= m_drive_list.size()) {
        m_metadata_fetched[index] = 0;
        return;
    }
    std::wstring volume = m_drive_list[dIdx];
    writeLock.unlock();

    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_metadata_queue.push_back({index, frn, volume});
    }
    processMetadataQueue();
}

void MftReader::processMetadataQueue() {
    if (m_active_metadata_tasks.load() >= 4) return;

    MetadataTask task;
    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        if (m_metadata_queue.empty()) return;
        task = m_metadata_queue.back(); 
        m_metadata_queue.pop_back();
    }

    m_active_metadata_tasks.fetch_add(1);
    (void)QtConcurrent::run([this, task]() {
        int index = task.index;
        uint64_t frn = task.frn;
        std::wstring volume = task.volume;

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

        m_active_metadata_tasks.fetch_sub(1);
        processMetadataQueue(); 
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