#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MftReader.h"
#include "UsnWatcher.h"
#include "../ui/UiHelper.h"
#include "NtfsVolumeMftParser.h"
#include "MemoryQueryEngine.h"
#include "UsnJournalTreeSynchronizer.h"
#include "DiskIndexCacheCoordinator.h"
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
        std::unique_lock<std::shared_mutex> lock(m_pathCacheMutex);
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

    // 2026-07-28 极致修复：锁内仅执行增量映射补全
    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_parent_indices[i] != 0xFFFFFFFF) continue;
        uint64_t encodedPf = m_parent_frns[i];
        auto itP = m_frn_to_idx.find(encodedPf);
        if (itP != m_frn_to_idx.end()) m_parent_indices[i] = itP->second;
    }

    m_isInitialized = true;
    lock.unlock();

    compact(); 
    buildSortedIndices();
    for (auto* w : newWatchers) w->start();
}

bool MftReader::loadFromCache() {
    return DiskIndexCacheCoordinator::loadFromCache(this);
}

bool MftReader::loadDriveFromCache(const QString& drive) {
    return DiskIndexCacheCoordinator::loadDriveFromCache(this, drive);
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
            std::unique_lock<std::shared_mutex> pathLock(m_pathCacheMutex);
            auto itP = m_path_cache.begin();
            while (itP != m_path_cache.end()) {
                if ((itP->first >> 48) == dIdx) itP = m_path_cache.erase(itP);
                else ++itP;
            }
        }

        // 2026-07-28 核心修复：卸载数据时禁止在锁内遍历 SoA。
        // 策略：仅清空盘符名称并更新掩码。物理剔除任务交给随后的锁外 compact()。
        m_drive_list[dIdx] = L"";

        m_drive_ever_saved.erase(dIdx);
        m_is_compacting.erase(dIdx);
        m_compaction_buffer.erase(dIdx);

        uint32_t mask = m_drive_active_mask.load();
        mask &= ~(1 << dIdx);
        m_drive_active_mask.store(mask);
    }

    // 2026-07-28 修复：卸载后必须强制 compact 以物理回收该盘占用的所有内存。
    // compact 内部已实现去锁化。
    compact(true); 

    if (w) { w->stop(); delete w; }
}

bool MftReader::saveToCache() {
    return DiskIndexCacheCoordinator::saveToCache(this);
}

bool MftReader::saveDriveToCache(size_t driveIdx) {
    return DiskIndexCacheCoordinator::saveDriveToCache(this, driveIdx);
}

bool MftReader::saveDriveToCacheInternal(size_t driveIdx) {
    return DiskIndexCacheCoordinator::saveDriveToCacheInternal(this, driveIdx);
}

bool MftReader::saveDriveToCacheUnlocked(size_t driveIdx) {
    return DiskIndexCacheCoordinator::saveDriveToCacheUnlocked(this, driveIdx);
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
    // 代表内存中加载的所有条目总数（用于内存计算）
    return (int)m_frn_to_idx.size();
}

int MftReader::activeCount() const {
    QReadLocker lock(&m_dataLock);
    
    uint32_t activeMask = m_drive_active_mask.load(std::memory_order_relaxed);
    int count = 0;
    
    // SoA 内存连续，在 400 万量级下此遍历通常在 2ms 内完成，效率极高 (Analysis_Modification_Plan-154.md)
    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] == 0) continue; // 排除标记为已删除的死亡条目
        
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
        std::shared_lock<std::shared_mutex> lock(m_pathCacheMutex);
        auto it = m_path_cache.find(compositeKey);
        if (it != m_path_cache.end()) return it->second;
    }

    std::vector<std::wstring> segments;
    
    // 2026-06-xx 极致架构优化：采用 SoA 直连下标进行路径回溯。
    // 理由：getPathFast 常在 UI 渲染的热点路径被调用，消除 Map 查找是实现百万级数据“瞬间回溯”的关键。
    auto idxIt = m_frn_to_idx.find(compositeKey);
    if (idxIt == m_frn_to_idx.end()) return L"";

    uint32_t curIdx = idxIt->second;

    // 2026-06-xx 性能优化：限制回溯深度，并使用更轻量的循环检测。
    // 避免在大规模目录树中因循环引用导致的死循环，同时减少内存分配开销。
    int depth = 0;
    while (curIdx != 0xFFFFFFFF && depth < 64) {
        const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[curIdx]);
        
        // 2026-06-xx 性能优化：直接使用 QString::fromUtf8 获取路径片段，避免重复转换损耗
        segments.push_back(QString::fromUtf8(p).toStdWString());

        uint64_t parentFrn = m_parent_frns[curIdx] & 0x0000FFFFFFFFFFFFull;
        if (parentFrn == 5 || parentFrn == 0) break;

        curIdx = m_parent_indices[curIdx];
        depth++;
    }

    if (segments.empty()) return L"";

    std::wstring volume = (driveIdx < m_drive_list.size()) ? m_drive_list[driveIdx] : L"C:";
    std::wstring path = volume;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) path += L"\\" + *it;

    {
        std::unique_lock<std::shared_mutex> lock(m_pathCacheMutex);
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
    return MemoryQueryEngine::search(this, query, useRegex, caseSensitive, extensionList, includeHidden, includeSystem, includeDollar);
}

void MftReader::updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume) {
    UsnJournalTreeSynchronizer::updateEntryFromUsn(this, record, volume);
}

std::vector<MftReader::ChangeEvent> MftReader::pullChangeJournal() {
    std::lock_guard<std::mutex> lock(m_journalMutex);
    return std::move(m_changeJournal);
}

void MftReader::removeEntryByFrn(const std::wstring& volume, uint64_t frn) {
    UsnJournalTreeSynchronizer::removeEntryByFrn(this, volume, frn);
}

void MftReader::compact(bool force) {
    // 2026-07-28 修复：恢复条件触发，禁止无条件全量重建
    if (!force && m_dead_count == 0 && m_wasted_string_bytes < 1024 * 1024) return;

    m_generation.fetch_add(1, std::memory_order_relaxed);
    
    // 2026-07-28 优化：将 O(total) 的数据搬运过程移出写锁
    // 1. 投影准备 (持有读锁拷贝必要状态)
    struct CompactSnapshot {
        std::vector<uint64_t> frns;
        std::vector<uint64_t> parent_frns;
        std::vector<int64_t>  sizes;
        std::vector<int64_t>  timestamps;
        std::vector<uint32_t> name_offsets;
        std::vector<uint32_t> ext_offsets;
        std::vector<uint32_t> attributes;
        std::vector<uint8_t>  metadata_fetched;
        std::vector<uint8_t>  string_pool;
        std::vector<std::wstring> drive_list;
        size_t dead_count;
        size_t wasted_string_bytes;
    } snap;

    {
        QReadLocker lock(&m_dataLock);
        snap.frns = m_frns;
        snap.parent_frns = m_parent_frns;
        snap.sizes = m_sizes;
        snap.timestamps = m_timestamps;
        snap.name_offsets = m_name_offsets;
        snap.ext_offsets = m_ext_offsets;
        snap.attributes = m_attributes;
        snap.metadata_fetched = m_metadata_fetched;
        snap.string_pool = m_string_pool;
        snap.drive_list = m_drive_list;
        snap.dead_count = m_dead_count;
        snap.wasted_string_bytes = m_wasted_string_bytes;
    }

    // 2. 锁外重建 (O(total) 计算完全处于锁外)
    std::vector<uint64_t>  new_frns;
    std::vector<uint64_t>  new_parent_frns;
    std::vector<int64_t>   new_sizes;
    std::vector<int64_t>   new_timestamps;
    std::vector<uint32_t>  new_name_offsets;
    std::vector<uint32_t>  new_ext_offsets;
    std::vector<uint32_t>  new_attributes;
    std::vector<uint8_t>   new_metadata_fetched;
    std::vector<uint8_t>   new_string_pool;
    std::unordered_map<uint64_t, uint32_t> new_frn_to_idx;

    size_t count = snap.frns.size();
    new_frns.reserve(count - snap.dead_count);
    new_parent_frns.reserve(count - snap.dead_count);
    new_sizes.reserve(count - snap.dead_count);
    new_timestamps.reserve(count - snap.dead_count);
    new_name_offsets.reserve(count - snap.dead_count);
    new_attributes.reserve(count - snap.dead_count);
    new_metadata_fetched.reserve(count - snap.dead_count);
    new_string_pool.reserve(snap.string_pool.size() - snap.wasted_string_bytes);

    for (size_t i = 0; i < count; ++i) {
        if (snap.frns[i] == 0) continue;
        
        size_t dIdx = static_cast<size_t>(snap.parent_frns[i] >> 48);
        // 2026-07-28 极致优化：如果盘符已在 unloadDrive 中被置空，则该记录物理剔除
        if (dIdx >= snap.drive_list.size() || snap.drive_list[dIdx].empty()) continue;

        uint32_t newIdx = (uint32_t)new_frns.size();
        new_frn_to_idx[makeKey(dIdx, snap.frns[i])] = newIdx;
        
        new_frns.push_back(snap.frns[i]);
        new_parent_frns.push_back(snap.parent_frns[i]);
        new_sizes.push_back(snap.sizes[i]);
        new_timestamps.push_back(snap.timestamps[i]);
        new_attributes.push_back(snap.attributes[i]);
        new_metadata_fetched.push_back(snap.metadata_fetched[i]);
        
        const char* name = reinterpret_cast<const char*>(snap.string_pool.data() + snap.name_offsets[i]);
        size_t len = strlen(name) + 1;
        new_name_offsets.push_back((uint32_t)new_string_pool.size());
        new_string_pool.insert(new_string_pool.end(), name, name + len);

        const char* ext = reinterpret_cast<const char*>(snap.string_pool.data() + snap.ext_offsets[i]);
        size_t extLen = strlen(ext) + 1;
        new_ext_offsets.push_back((uint32_t)new_string_pool.size());
        new_string_pool.insert(new_string_pool.end(), ext, ext + extLen);
    }

    // 3. 结果回写 (极短写锁窗口进行原子替换)
    {
        QWriteLocker lock(&m_dataLock);
        m_frns = std::move(new_frns);
        m_parent_frns = std::move(new_parent_frns);
        m_sizes = std::move(new_sizes);
        m_timestamps = std::move(new_timestamps);
        m_name_offsets = std::move(new_name_offsets);
        m_ext_offsets = std::move(new_ext_offsets);
        m_attributes = std::move(new_attributes);
        m_metadata_fetched = std::move(new_metadata_fetched);
        m_string_pool = std::move(new_string_pool);
        m_frn_to_idx = std::move(new_frn_to_idx);

        m_dead_count = 0;
        m_wasted_string_bytes = 0;
        
        // 在锁内快速完成映射补全，避免状态不一致
        m_parent_indices.assign(m_frns.size(), 0xFFFFFFFF);
        for (size_t i = 0; i < m_frns.size(); ++i) {
            uint64_t encodedPf = m_parent_frns[i];
            auto itP = m_frn_to_idx.find(encodedPf);
            if (itP != m_frn_to_idx.end()) m_parent_indices[i] = itP->second;
        }
    }
    
    // 2026-07-28 修复：不再在 compact 内部自动触发排序，该职责移交至调用者以确保锁一致性
    // buildSortedIndices();
}

bool MftReader::loadMftDirect(const std::wstring& volume, MftReader::DriveResult& result) {
    return NtfsVolumeMftParser::loadMftDirect(volume, result);
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

        // 2026-07-29 物理修复 (Analysis_Modification_Plan-202.md)：
        // 新写入的记录必须同步登记进 m_frn_to_idx，不能只依赖事后的 compact()。
        // compact() 在 !force 且 m_dead_count == 0（全新扫描必然如此）时会提前
        // return，导致刚合并进来的这批数据永远进不了映射表——搜索/getFullPath
        // 全部走 m_frn_to_idx.find()，找不到就直接失败或返回空路径，这正是
        // "立即扫描并索引"后搜不到数据、缩略图卡片全灰的根因。
        // 这里在写锁范围内（调用方 buildIndex 持有 QWriteLocker）直接同步维护，
        // 代价是 O(本次新增条目数)，不需要再依赖 compact(true) 对全部已加载
        // 条目做一次代价高得多的 O(全量) 重建。
        m_frn_to_idx[makeKey(driveIdx, e.frn)] = (uint32_t)(m_frns.size() - 1);

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
    // 2026-07-28 极致修复：真正的去锁化双缓冲排序
    // 理由：std::sort 是 O(N log N) 操作，必须完全移出写锁范围。
    
    // 1. 投影准备 (持有读锁，快速拷贝文件名指针)
    struct NameProjection {
        uint32_t idx;
        std::string name; // 必须拷贝字符串以实现完全脱离 m_string_pool 的生命周期，确保锁外安全
    };
    std::vector<NameProjection> projections;
    
    {
        QReadLocker lock(&m_dataLock);
        size_t count = m_frns.size();
        projections.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (m_frns[i] == 0) continue;
            const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
            projections.push_back({(uint32_t)i, p});
        }
    }

    // 2. 排序阶段 (代码行号证据：1560-1562，此处完全不持有 m_dataLock)
    std::sort(projections.begin(), projections.end(), [](const NameProjection& a, const NameProjection& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    // 3. 回写结果 (短写锁进行交换)
    std::vector<uint32_t> new_sorted;
    new_sorted.reserve(projections.size());
    for (const auto& p : projections) new_sorted.push_back(p.idx);

    {
        QWriteLocker lock(&m_dataLock);
        m_sorted_indices = std::move(new_sorted);
    }
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
                size_t dIdxForSave = static_cast<size_t>(m_parent_frns[index] >> 48);
                lock.unlock();

                // 2026-07-xx 物理修复：元数据补全后必须标记脏数据并触发落盘，
                // 否则重启程序后已补全的大小/时间会丢失，下次还要重新查询。
                bool shouldSave = false;
                {
                    std::lock_guard<std::mutex> dLock(m_dirtyLock);
                    m_dirty_indices.insert(index);
                    m_dirty_count++;
                    if (m_dirty_count >= 1000) {
                        m_dirty_count = 0;
                        shouldSave = true;
                    }
                }
                if (shouldSave && dIdxForSave < m_drive_list.size()) {
                    QThreadPool::globalInstance()->start([this, dIdxForSave]() {
                        saveDriveToCache(dIdxForSave);
                    });
                }

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

                        size_t dIdxForSave = static_cast<size_t>(m_parent_frns[index] >> 48);
                        writeLock.unlock();

                        // 2026-07-xx 物理修复：同上，这条退化路径成功时也必须标记脏数据并触发落盘。
                        bool shouldSave = false;
                        {
                            std::lock_guard<std::mutex> dLock(m_dirtyLock);
                            m_dirty_indices.insert(index);
                            m_dirty_count++;
                            if (m_dirty_count >= 1000) {
                                m_dirty_count = 0;
                                shouldSave = true;
                            }
                        }
                        if (shouldSave && dIdxForSave < m_drive_list.size()) {
                            QThreadPool::globalInstance()->start([this, dIdxForSave]() {
                                saveDriveToCache(dIdxForSave);
                            });
                        }
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
    return UiHelper::getCachedIcon(ext, isDir);
}

} // namespace FERREX