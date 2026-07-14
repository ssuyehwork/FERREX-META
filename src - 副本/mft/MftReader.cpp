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

static void splitNameAndExt(const std::string& fullName, std::string& outExt) {
    outExt.clear();
    size_t lastDot = fullName.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        outExt = fullName.substr(lastDot + 1);
        std::transform(outExt.begin(), outExt.end(), outExt.begin(), ::tolower);
    }
}

static int64_t filetimeToUnixMs(int64_t filetime) {

    
    if (filetime <= 116444736000000000LL) return 0;
    
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
    m_metadataPool->setMaxThreadCount(2); 
    m_notifyTimer = new QTimer(this);
    m_notifyTimer->setInterval(150); 
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

    {
        std::lock_guard<std::mutex> journalLock(m_journalMutex);
        m_changeJournal.clear();
    }
    if (m_notifyTimer) m_notifyTimer->stop();

    m_isStopping.store(false); 
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
        
        QReadLocker lock(&m_dataLock);
        if (m_isInitialized) return;
    }

    struct ScannedDrive {
        std::wstring volume;
        MftReader::DriveResult res; 
        bool success = false;
    };
    std::vector<ScannedDrive> scannedResults(toScan.size());

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
        
        saveDriveToCacheUnlocked(dIdx);
        
        auto* w = new UsnWatcher(sr.volume, sr.res.nextUsn, nullptr);
        m_watcher_map[sr.volume] = w;
        newWatchers.push_back(w);
    }

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

        m_next_usns.erase(vol);
        {
            std::lock_guard<std::mutex> pathLock(m_pathCacheMutex);
            auto itP = m_path_cache.begin();
            while (itP != m_path_cache.end()) {
                if ((itP->first >> 48) == dIdx) itP = m_path_cache.erase(itP);
                else ++itP;
            }
        }

        
        m_drive_list[dIdx] = L"";

        m_drive_ever_saved.erase(dIdx);
        m_is_compacting.erase(dIdx);
        m_compaction_buffer.erase(dIdx);

        uint32_t mask = m_drive_active_mask.load();
        mask &= ~(1 << dIdx);
        m_drive_active_mask.store(mask);
    }

    
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
    
    return (int)m_frn_to_idx.size();
}

int MftReader::activeCount() const {
    QReadLocker lock(&m_dataLock);
    
    uint32_t activeMask = m_drive_active_mask.load(std::memory_order_relaxed);
    int count = 0;

    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] == 0) continue; 
        
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
        const char* ext = reinterpret_cast<const char*>(m_string_pool.data() + m_ext_offsets[i]);
        for (const QString& ex : extensionList) {

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

    if (useRegex) {
        
        return QRegularExpression(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption)
               .match(QString::fromUtf8(p)).hasMatch();
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
    return QString::fromStdWString(const_cast<MftReader*>(this)->getPathFast(dIdx, frn));
}

std::wstring MftReader::getPathFast(size_t driveIdx, uint64_t frn) {
    
    uint64_t compositeKey = (static_cast<uint64_t>(driveIdx) << 48) | (frn & 0x0000FFFFFFFFFFFFull);

    {
        std::lock_guard<std::mutex> lock(m_pathCacheMutex);
        auto it = m_path_cache.find(compositeKey);
        if (it != m_path_cache.end()) return it->second;
    }

    std::vector<std::wstring> segments;

    
    auto idxIt = m_frn_to_idx.find(compositeKey);
    if (idxIt == m_frn_to_idx.end()) return L"";

    uint32_t curIdx = idxIt->second;

    
    int depth = 0;
    while (curIdx != 0xFFFFFFFF && depth < 64) {
        const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[curIdx]);

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
    
    if (!force && m_dead_count == 0 && m_wasted_string_bytes < 1024 * 1024) return;

    m_generation.fetch_add(1, std::memory_order_relaxed);

    
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

        m_parent_indices.assign(m_frns.size(), 0xFFFFFFFF);
        for (size_t i = 0; i < m_frns.size(); ++i) {
            uint64_t encodedPf = m_parent_frns[i];
            auto itP = m_frn_to_idx.find(encodedPf);
            if (itP != m_frn_to_idx.end()) m_parent_indices[i] = itP->second;
        }
    }

    
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
        m_parent_indices.push_back(0xFFFFFFFF); 
        m_sizes.push_back(e.size); 
        m_timestamps.push_back(e.modifyTime); m_attributes.push_back(e.attributes);
        m_metadata_fetched.push_back(0);
        
        const char* namePtr = reinterpret_cast<const char*>(result.string_pool.data() + e.nameOffset);
        m_name_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), namePtr, namePtr + strlen(namePtr) + 1);

        std::string extStr;
        splitNameAndExt(namePtr, extStr);
        m_ext_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), extStr.begin(), extStr.end());
        m_string_pool.push_back('\0');
    }
}

void MftReader::rebuildFrnToIndexMap() {
    
    m_frn_to_idx.clear();

    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] != 0) {
            size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
            m_frn_to_idx[makeKey(dIdx, m_frns[i])] = (uint32_t)i;
        }
    }

    for (size_t i = 0; i < m_frns.size(); ++i) {
        if (m_frns[i] == 0) continue;
        size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
        uint64_t key = makeKey(dIdx, m_frns[i]);
        if (m_frn_to_idx[key] != (uint32_t)i) {
            m_frns[i] = 0;
            m_dead_count++;
        }
    }

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

    
    
    struct NameProjection {
        uint32_t idx;
        std::string name; 
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

    std::sort(projections.begin(), projections.end(), [](const NameProjection& a, const NameProjection& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    std::vector<uint32_t> new_sorted;
    new_sorted.reserve(projections.size());
    for (const auto& p : projections) new_sorted.push_back(p.idx);

    {
        QWriteLocker lock(&m_dataLock);
        m_sorted_indices = std::move(new_sorted);
    }
}

void MftReader::requestMetadata(int index) {
    
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

    (void)QtConcurrent::run(m_metadataPool, [this, index, frn, volume]() {

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

} 
