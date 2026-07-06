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
#include <fstream>
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

namespace ArcMeta {

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
}

MftReader::~MftReader() {
    clear();
}

void MftReader::clearInternal() {
    m_frns.clear();
    m_parent_frns.clear();
    m_sizes.clear();
    m_timestamps.clear();
    m_name_offsets.clear();
    m_attributes.clear();
    m_metadata_fetched.clear();
    m_string_pool.clear();
    m_drive_list.clear();
    m_drive_serials.clear();
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
    m_dirty_buffers.clear();
}

void MftReader::clear() {
    std::vector<UsnWatcher*> toStop;
    {
        QWriteLocker lock(&m_dataLock);
        toStop = std::move(m_watchers);
        m_watchers.clear();
    }
    for (auto* w : toStop) { if (w) { w->stop(); delete w; } }
    QWriteLocker lock(&m_dataLock);
    clearInternal();
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
                if (_wcsicmp(indexedVol.c_str(), vol.c_str()) == 0) { found = true; break; }
            }
            if (!found) toScan.push_back(vol);
        }
    }
    if (toScan.empty()) return;

    struct ScannedDrive { std::wstring volume; DriveResult res; bool success = false; };
    std::vector<ScannedDrive> results(toScan.size());
    std::vector<int> indices((int)toScan.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::for_each((std::execution::par), indices.begin(), indices.end(), [&](int i) {
        results[i].volume = toScan[i];
        results[i].success = loadMftDirect(toScan[i], results[i].res);
    });

    QWriteLocker lock(&m_dataLock);
    for (auto& sr : results) {
        if (!sr.success || sr.res.entries.empty()) continue;
        size_t dIdx = m_drive_list.size();
        m_drive_list.push_back(sr.volume);
        m_drive_serials.push_back(sr.res.volumeSerial);
        m_next_usns[sr.volume] = sr.res.nextUsn;
        mergeDriveResultInternal(sr.volume, sr.res, dIdx);
        
        flushDirtyToDisk(dIdx);

        auto* w = new UsnWatcher(sr.volume, sr.res.nextUsn, nullptr);
        m_watchers.push_back(w);
        w->start();
    }
    rebuildFrnToIndexMap();
    buildSortedIndices();
    m_isInitialized = true;
}

bool MftReader::loadFromCache() {
    std::filesystem::path cacheDir = "ArcMeta/cache";
    if (!std::filesystem::exists(cacheDir)) return false;

    clear();
    QWriteLocker lock(&m_dataLock);
    
    for (auto const& entry : std::filesystem::directory_iterator{cacheDir}) {
        if (entry.path().extension() == ".bin") {
            std::string binPath = entry.path().string();
            std::filesystem::path ip = entry.path();
            ip.replace_extension(".idx");
            std::string idxPath = ip.string();

            std::ifstream bf(binPath, std::ios::binary);
            if (!bf) continue;
            BinHeader bh; bf.read(reinterpret_cast<char*>(&bh), sizeof(BinHeader));
            if (bh.magic != BIN_MAGIC_VAL) continue;

            uint64_t nextUsn = 0;
            std::vector<IndexEntry> mainIndex, deltaLayer;
            if (!ScchCache::loadIndex(idxPath, bh.volume_serial, nextUsn, mainIndex, deltaLayer)) {
                if (!ScchCache::rebuildIndexFromBin(binPath, bh.volume_serial, mainIndex)) continue;
            }

            size_t dIdx = m_drive_list.size();
            std::wstring volName = entry.path().stem().wstring() + L":";
            m_drive_list.push_back(volName);
            m_drive_serials.push_back(bh.volume_serial);
            m_next_usns[volName] = nextUsn;

            std::vector<ScchCache::Record> allRecords;
            ScchCache::readRecords(binPath, mainIndex, allRecords);
            ScchCache::readRecords(binPath, deltaLayer, allRecords);

            for (const auto& r : allRecords) {
                uint64_t compositeKey = makeKey(dIdx, r.frn);
                auto it = m_frn_to_idx.find(compositeKey);
                if (it != m_frn_to_idx.end()) {
                    uint32_t idx = it->second;
                    m_timestamps[idx] = r.timestamp;
                    m_attributes[idx] = r.attributes;
                    QByteArray utf8 = QString::fromStdString(r.name).toUtf8();
                    m_name_offsets[idx] = (uint32_t)m_string_pool.size();
                    m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
                    m_string_pool.push_back('\0');
                } else {
                    uint32_t currentIdx = (uint32_t)m_frns.size();
                    m_frns.push_back(r.frn);
                    m_parent_frns.push_back((static_cast<uint64_t>(dIdx) << 48) | (r.parentFrn & 0x0000FFFFFFFFFFFFull));
                    m_sizes.push_back(0);
                    m_timestamps.push_back(r.timestamp);
                    m_attributes.push_back(r.attributes);
                    m_metadata_fetched.push_back(0);
                    QByteArray utf8 = QString::fromStdString(r.name).toUtf8();
                    m_name_offsets.push_back((uint32_t)m_string_pool.size());
                    m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
                    m_string_pool.push_back('\0');
                    m_frn_to_idx[compositeKey] = currentIdx;
                }
            }

            auto* w = new UsnWatcher(volName, nextUsn, nullptr);
            m_watchers.push_back(w);
            w->start();
        }
    }

    buildSortedIndices();
    m_isInitialized = true;
    return !m_frns.empty();
}

void MftReader::flushDirtyToDisk(size_t driveIdx) {
    if (driveIdx >= m_drive_list.size()) return;

    std::vector<ScchCache::Record> records;
    uint64_t currentUsn = 0;
    {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        records = std::move(m_dirty_buffers[driveIdx]);
        m_dirty_buffers[driveIdx].clear();
        currentUsn = m_next_usns[m_drive_list[driveIdx]];
    }
    if (records.empty()) return;

    std::wstring vol = m_drive_list[driveIdx];
    uint64_t serial = m_drive_serials[driveIdx];
    std::string binPath = "ArcMeta/cache/" + QString::fromStdWString(vol).left(1).toStdString() + ".bin";
    std::string idxPath = "ArcMeta/cache/" + QString::fromStdWString(vol).left(1).toStdString() + ".idx";

    std::filesystem::create_directories("ArcMeta/cache");
    ScchCache::appendBatch(binPath, idxPath, serial, currentUsn, records);
}

std::vector<uint64_t> MftReader::search(const QString& query, bool useRegex, bool caseSensitive, 
                                       const QStringList& extensionList, bool includeHidden, bool includeSystem,
                                       bool includeDollar) {
    QReadLocker lock(&m_dataLock);
    if (!m_isInitialized) return std::vector<uint64_t>();

    bool hasQuery = !query.isEmpty();
    std::vector<uint64_t> results;
    results.reserve(1000);

    if (hasQuery && !useRegex && !caseSensitive && extensionList.isEmpty()) {
        QByteArray qUtf8 = query.toUtf8();
        auto it = std::lower_bound(m_sorted_indices.begin(), m_sorted_indices.end(), qUtf8.constData(),
            [this](uint32_t idx, const char* q) {
                return _strnicmp(reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[idx]), q, strlen(q)) < 0;
            });
        
        for (; it != m_sorted_indices.end(); ++it) {
            uint32_t i = *it;
            const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
            if (_strnicmp(p, qUtf8.constData(), qUtf8.size()) != 0) break;
            
            size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
            if (!(m_drive_active_mask.load() & (1 << dIdx))) continue;
            if (!includeDollar && p[0] == '$') continue;
            uint32_t at = m_attributes[i];
            if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;

            results.push_back(makeKey(dIdx, m_frns[i]));
            if (results.size() >= 50000) break;
        }
        return results;
    }

    const size_t totalC = m_frns.size();
    const size_t chunkSize = 4096;

    for (size_t start = 0; start < totalC; start += chunkSize) {
        size_t end = (std::min)(start + chunkSize, totalC);
        for (size_t i = start; i < end; ++i) {
            if (m_frns[i] == 0) continue;

            size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
            if (!(m_drive_active_mask.load() & (1 << dIdx))) continue;

            const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
            if (!includeDollar && p[0] == '$') continue;

            uint32_t at = m_attributes[i];
            if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;

            bool match = true;
            if (hasQuery) {
                if (useRegex) {
                    QRegularExpression re(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
                    match = re.match(QString::fromUtf8(p)).hasMatch();
                } else {
                    if (caseSensitive) match = (strstr(p, query.toUtf8().constData()) != nullptr);
                    else match = (StrStrIA(p, query.toUtf8().constData()) != nullptr);
                }
            }
            if (match && !extensionList.isEmpty()) {
                bool extMatch = false; size_t nlen = strlen(p);
                for (const auto& ex : extensionList) {
                    std::string dotEx = (ex.startsWith('.') ? ex : "." + ex).toLower().toStdString();
                    if (nlen >= dotEx.size() && _stricmp(p + nlen - dotEx.size(), dotEx.c_str()) == 0) { extMatch = true; break; }
                }
                match = extMatch;
            }
            if (match) { results.push_back(makeKey(dIdx, m_frns[i])); if (results.size() >= 50000) break; }
        }
        if (results.size() >= 50000) break;
    }
    return results;
}

bool MftReader::matchEntry(int i, const QString& query, bool useRegex, bool caseSensitive,
                          const QStringList& extensionList, bool includeHidden, bool includeSystem,
                          bool includeDollar) const {
    QReadLocker lock(&m_dataLock);
    if (i < 0 || i >= (int)m_frns.size() || m_frns[i] == 0) return false;

    size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
    if (!(m_drive_active_mask.load() & (1 << dIdx))) return false;

    const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
    if (!includeDollar && p[0] == '$') return false;

    uint32_t at = m_attributes[i];
    if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) return false;
    if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) return false;

    if (!query.isEmpty()) {
        if (useRegex) {
            QRegularExpression re(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
            if (!re.match(QString::fromUtf8(p)).hasMatch()) return false;
        } else {
            if (caseSensitive) { if (!strstr(p, query.toUtf8().constData())) return false; }
            else { if (!StrStrIA(p, query.toUtf8().constData())) return false; }
        }
    }

    if (!extensionList.isEmpty()) {
        bool extMatch = false; size_t nlen = strlen(p);
        for (const auto& ex : extensionList) {
            std::string dotEx = (ex.startsWith('.') ? ex : "." + ex).toLower().toStdString();
            if (nlen >= dotEx.size() && _stricmp(p + nlen - dotEx.size(), dotEx.c_str()) == 0) { extMatch = true; break; }
        }
        if (!extMatch) return false;
    }
    return true;
}

void MftReader::updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume) {
    QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(record) + record->FileNameOffset), record->FileNameLength / 2);

    QWriteLocker lock(&m_dataLock);
    size_t dIdx = 0;
    for (; dIdx < m_drive_list.size(); ++dIdx) { if (_wcsicmp(m_drive_list[dIdx].c_str(), volume.c_str()) == 0) break; }
    if (dIdx >= m_drive_list.size()) return;

    uint64_t key = makeKey(dIdx, record->FileReferenceNumber);
    uint32_t targetIdx;
    auto it = m_frn_to_idx.find(key);
    if (it != m_frn_to_idx.end()) {
        targetIdx = it->second;
        m_timestamps[targetIdx] = filetimeToUnixMs(record->TimeStamp.QuadPart);
        m_attributes[targetIdx] = record->FileAttributes;
        QByteArray utf8 = name.toUtf8();
        m_name_offsets[targetIdx] = (uint32_t)m_string_pool.size();
        m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
        m_string_pool.push_back('\0');
    } else {
        targetIdx = (uint32_t)m_frns.size();
        m_frns.push_back(record->FileReferenceNumber);
        m_parent_frns.push_back((static_cast<uint64_t>(dIdx) << 48) | (record->ParentFileReferenceNumber & 0x0000FFFFFFFFFFFFull));
        m_sizes.push_back(0);
        m_timestamps.push_back(filetimeToUnixMs(record->TimeStamp.QuadPart));
        m_attributes.push_back(record->FileAttributes);
        m_metadata_fetched.push_back(0);
        QByteArray utf8 = name.toUtf8();
        m_name_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
        m_string_pool.push_back('\0');
        m_frn_to_idx[key] = targetIdx;
    }
    m_next_usns[volume] = record->Usn;

    ScchCache::Record r;
    r.frn = record->FileReferenceNumber;
    r.parentFrn = record->ParentFileReferenceNumber;
    r.name = name.toStdString();
    r.attributes = record->FileAttributes;
    r.timestamp = filetimeToUnixMs(record->TimeStamp.QuadPart);

    {
        std::lock_guard<std::mutex> dLock(m_dirtyMutex);
        m_dirty_buffers[dIdx].push_back(r);
        if (m_dirty_buffers[dIdx].size() >= 100) {
            size_t idx = dIdx;
            QtConcurrent::run([this, idx]() { flushDirtyToDisk(idx); });
        }
    }

    lock.unlock();
    emit entryUpdated(targetIdx);
}

void MftReader::removeEntryByFrn(const std::wstring& volume, uint64_t frn) {
    QWriteLocker lock(&m_dataLock);
    size_t dIdx = 0;
    for (; dIdx < m_drive_list.size(); ++dIdx) { if (_wcsicmp(m_drive_list[dIdx].c_str(), volume.c_str()) == 0) break; }
    if (dIdx >= m_drive_list.size()) return;

    uint64_t key = makeKey(dIdx, frn);
    auto it = m_frn_to_idx.find(key);
    if (it != m_frn_to_idx.end()) {
        m_frns[it->second] = 0;
        m_frn_to_idx.erase(it);
        lock.unlock();
        emit entryRemoved(key);
    }
}

int64_t MftReader::getSize(int index) const { QReadLocker l(&m_dataLock); return (index >= 0 && index < (int)m_sizes.size()) ? m_sizes[index] : 0; }
int64_t MftReader::getModifyTime(int index) const { QReadLocker l(&m_dataLock); return (index >= 0 && index < (int)m_timestamps.size()) ? m_timestamps[index] : 0; }
uint32_t MftReader::getAttributes(int index) const { QReadLocker l(&m_dataLock); return (index >= 0 && index < (int)m_attributes.size()) ? m_attributes[index] : 0; }
uint64_t MftReader::getFrn(int index) const { QReadLocker l(&m_dataLock); return (index >= 0 && index < (int)m_frns.size()) ? m_frns[index] : 0; }
bool MftReader::isDirectory(int index) const { return (getAttributes(index) & FILE_ATTRIBUTE_DIRECTORY) != 0; }
int MftReader::totalCount() const { QReadLocker l(&m_dataLock); return (int)m_frns.size(); }
QString MftReader::getName(int index) const { QReadLocker l(&m_dataLock); if (index < 0 || index >= (int)m_name_offsets.size()) return ""; return QString::fromUtf8(reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[index])); }
int MftReader::getIndexByKey(uint64_t key) const { QReadLocker l(&m_dataLock); auto it = m_frn_to_idx.find(key); return (it != m_frn_to_idx.end()) ? (int)it->second : -1; }
uint64_t MftReader::getKeyByIndex(int idx) const { QReadLocker l(&m_dataLock); if (idx < 0 || idx >= (int)m_frns.size()) return 0; size_t d = static_cast<size_t>(m_parent_frns[idx] >> 48); return makeKey(d, m_frns[idx]); }
bool MftReader::isMetadataFetched(int idx) const { QReadLocker l(&m_dataLock); return (idx >= 0 && idx < (int)m_metadata_fetched.size()) ? m_metadata_fetched[idx] == 2 : true; }
void MftReader::requestMetadata(int idx) { Q_UNUSED(idx); }

QString MftReader::getFullPath(int index) const {
    QReadLocker lock(&m_dataLock);
    if (index < 0 || index >= (int)m_frns.size()) return "";
    uint64_t frn = m_frns[index];
    size_t dIdx = static_cast<size_t>(m_parent_frns[index] >> 48);
    return QString::fromStdWString(const_cast<MftReader*>(this)->getPathFast(dIdx, frn));
}

std::wstring MftReader::getPathFast(size_t driveIdx, uint64_t frn) {
    uint64_t key = makeKey(driveIdx, frn);
    { std::lock_guard<std::mutex> l(m_pathCacheMutex); auto it = m_path_cache.find(key); if (it != m_path_cache.end()) return it->second; }
    std::vector<std::wstring> segments;
    uint64_t cur = frn;
    while (true) {
        uint64_t curKey = makeKey(driveIdx, cur);
        auto it = m_frn_to_idx.find(curKey);
        if (it == m_frn_to_idx.end()) break;
        uint32_t idx = it->second;
        segments.push_back(getName(idx).toStdWString());
        uint64_t pf = m_parent_frns[idx] & 0x0000FFFFFFFFFFFFull;
        if (pf == 5 || pf == 0 || pf == cur) break;
        cur = pf;
    }
    std::wstring path = (driveIdx < m_drive_list.size()) ? m_drive_list[driveIdx] : L"C:";
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) path += L"\\" + *it;
    { std::lock_guard<std::mutex> l(m_pathCacheMutex); m_path_cache[key] = path; }
    return path;
}

bool MftReader::loadMftDirect(const std::wstring& volume, DriveResult& result) {
    std::wstring dev = L"\\\\.\\" + volume;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    USN_JOURNAL_DATA_V0 j; DWORD cb;
    if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &j, sizeof(j), &cb, NULL)) { CloseHandle(h); return false; }
    result.nextUsn = j.NextUsn;
    result.volumeSerial = j.UsnJournalID;
    MFT_ENUM_DATA_V0 ed = {0}; ed.HighUsn = j.NextUsn;
    std::vector<uint8_t> buf(1024 * 1024);
    while (DeviceIoControl(h, FSCTL_ENUM_USN_DATA, &ed, sizeof(ed), buf.data(), (DWORD)buf.size(), &cb, NULL)) {
        if (cb < 8) break;
        uint8_t* p = buf.data() + 8; uint8_t* end = buf.data() + cb;
        while (p < end) {
            USN_RECORD_V2* rec = reinterpret_cast<USN_RECORD_V2*>(p);
            ScchCache::Record r; r.frn = rec->FileReferenceNumber; r.parentFrn = rec->ParentFileReferenceNumber;
            r.attributes = rec->FileAttributes; r.timestamp = filetimeToUnixMs(rec->TimeStamp.QuadPart);
            r.name = QString::fromUtf16(reinterpret_cast<const char16_t*>(p + rec->FileNameOffset), rec->FileNameLength / 2).toStdString();
            result.entries.push_back(std::move(r)); p += rec->RecordLength;
        }
        ed.StartFileReferenceNumber = *reinterpret_cast<DWORDLONG*>(buf.data());
    }
    CloseHandle(h); return true;
}

void MftReader::mergeDriveResultInternal(const std::wstring& volume, const DriveResult& result, size_t driveIdx) {
    Q_UNUSED(volume);
    for (const auto& e : result.entries) {
        uint32_t idx = (uint32_t)m_frns.size();
        m_frns.push_back(e.frn);
        m_parent_frns.push_back((static_cast<uint64_t>(driveIdx) << 48) | (e.parentFrn & 0x0000FFFFFFFFFFFFull));
        m_sizes.push_back(0); m_timestamps.push_back(e.timestamp); m_attributes.push_back(e.attributes);
        m_metadata_fetched.push_back(0);
        QByteArray utf8 = QString::fromStdString(e.name).toUtf8();
        m_name_offsets.push_back((uint32_t)m_string_pool.size());
        m_string_pool.insert(m_string_pool.end(), utf8.begin(), utf8.end());
        m_string_pool.push_back('\0');
        m_frn_to_idx[makeKey(driveIdx, e.frn)] = idx;
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        m_dirty_buffers[driveIdx].push_back(e);
    }
}

void MftReader::rebuildFrnToIndexMap() {}
void MftReader::compact() {}
void MftReader::buildSortedIndices() {
    m_sorted_indices.resize(m_frns.size());
    std::iota(m_sorted_indices.begin(), m_sorted_indices.end(), 0);
    std::sort((std::execution::par), m_sorted_indices.begin(), m_sorted_indices.end(), [this](uint32_t a, uint32_t b) {
        return _stricmp(reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[a]),
                        reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[b])) < 0;
    });
}

QIcon MftReader::getCachedIcon(const QString& ext, bool isDir) {
    QString key = isDir ? "folder" : ext.toLower();
    { QReadLocker lock(&m_iconCacheLock); if (m_icon_cache.contains(key)) return m_icon_cache[key]; }
    QFileIconProvider p; QIcon icon = isDir ? p.icon(QFileIconProvider::Folder) : p.icon(QFileInfo("dummy." + key));
    { QWriteLocker lock(&m_iconCacheLock); m_icon_cache[key] = icon; }
    return icon;
}

} // namespace ArcMeta
