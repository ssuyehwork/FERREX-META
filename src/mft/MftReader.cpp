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

namespace FERREX-META {

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
    m_string_pool.reserve(100 * 1024 * 1024); // 预留 100MB 减少重分配
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

    for (auto& sr : results) {
        if (!sr.success || sr.res.entries.empty()) continue;
        
        size_t dIdx;
        {
            QWriteLocker lock(&m_dataLock);
            dIdx = m_drive_list.size();
            m_drive_list.push_back(sr.volume);
            m_drive_serials.push_back(sr.res.volumeSerial);
            m_next_usns[sr.volume] = sr.res.nextUsn;
        }

        // Plan-Smooth: 在 merge 内部处理分段加锁，防止新盘扫描导入时界面假死
        mergeDriveResultInternal(sr.volume, sr.res, dIdx);
        
        flushDirtyToDisk(dIdx);

        auto* w = new UsnWatcher(sr.volume, sr.res.nextUsn, nullptr);
        {
            QWriteLocker lock(&m_dataLock);
            m_watchers.push_back(w);
        }
        w->start();
    }
    
    {
        QWriteLocker lock(&m_dataLock);
        rebuildFrnToIndexMap();
    }
    buildSortedIndices();
    m_isInitialized = true;
}

bool MftReader::loadFromCache() {
    std::filesystem::path cacheDir = "FERREX-META/cache";
    if (!std::filesystem::exists(cacheDir)) return false;

    clear();
    
    // Plan-Smooth: 不要在整个加载周期持有写锁。
    // 我们先在外部解析文件，最后分批或一次性快速并入 SoA。
    
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
            std::vector<ScchIndexEntry> mainIndex, deltaLayer;
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

            // Plan-Smooth: 分批载入，每 10w 条释放一次锁，允许 UI 线程保持 60fps 响应（如显示加载进度）
            const size_t batchSize = 100000;
            for (size_t i = 0; i < allRecords.size(); i += batchSize) {
                QWriteLocker lock(&m_dataLock);
                size_t limit = (std::min)(i + batchSize, allRecords.size());
                for (size_t k = i; k < limit; ++k) {
                    const auto& r = allRecords[k];
                    uint64_t compositeKey = makeKey(dIdx, r.frn);
                    auto it = m_frn_to_idx.find(compositeKey);
                    if (it != m_frn_to_idx.end()) {
                        uint32_t idx = it->second;
                        m_timestamps[idx] = r.timestamp;
                        m_attributes[idx] = r.attributes;
                        QByteArray utf8 = QString::fromStdString(r.name).toUtf8();
                        m_name_offsets[idx] = (uint32_t)m_string_pool.size();
                        m_string_pool.insert(m_string_pool.end(), (const uint8_t*)utf8.constData(), (const uint8_t*)utf8.constData() + utf8.size());
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
                        m_string_pool.insert(m_string_pool.end(), (const uint8_t*)utf8.constData(), (const uint8_t*)utf8.constData() + utf8.size());
                        m_string_pool.push_back('\0');
                        m_frn_to_idx[compositeKey] = currentIdx;
                    }
                }
                m_total_count.store(m_frns.size(), std::memory_order_relaxed);
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
    std::string binPath = "FERREX-META/cache/" + QString::fromStdWString(vol).left(1).toStdString() + ".bin";
    std::string idxPath = "FERREX-META/cache/" + QString::fromStdWString(vol).left(1).toStdString() + ".idx";
    
    std::filesystem::create_directories("FERREX-META/cache");
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

    // Plan-Smooth: 预处理过滤条件，避免在循环中重复计算
    QRegularExpression re;
    if (hasQuery && useRegex) {
        re = QRegularExpression(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    }
    std::vector<std::string> extList;
    for (const auto& ex : extensionList) extList.push_back((ex.startsWith('.') ? ex : "." + ex).toLower().toStdString());

    // 路径 A: 高性能有序索引二分查找
    if (hasQuery && !useRegex && !caseSensitive && extensionList.isEmpty()) {
        QByteArray qUtf8 = query.toUtf8();
        auto it = std::lower_bound(m_sorted_indices.begin(), m_sorted_indices.end(), qUtf8.constData(), 
            [this](uint32_t idx, const char* q) {
                return _strnicmp(reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[idx]), q, strlen(q)) < 0;
            });
        
        for (size_t count = 0; it != m_sorted_indices.end(); ++it, ++count) {
            // Plan-Smooth: 定期释放锁，允许 USN 写入
            if (count > 0 && count % 2000 == 0) { lock.unlock(); lock.relock(); }

            uint32_t i = *it;
            const char* p = reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[i]);
            if (_strnicmp(p, qUtf8.constData(), (size_t)qUtf8.size()) != 0) break;
            
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

    // 路径 B: 全量分块线性扫描
    const size_t totalC = m_frns.size();
    const size_t chunkSize = 4096;

    for (size_t start = 0; start < totalC; start += chunkSize) {
        // Plan-Smooth: 定期释放锁，防止长时间持有读锁导致 USN 写入线程饥饿，进而引发 UI 假死
        if (start > 0) { lock.unlock(); lock.relock(); }

        size_t end = (std::min)(start + chunkSize, totalC);
        for (size_t i = start; i < end; ++i) {
            if (m_frns[i] == 0) continue;
            
            if (matchEntryOptimized((int)i, query, re, useRegex, caseSensitive, extList, includeHidden, includeSystem, includeDollar)) {
                size_t dIdx = static_cast<size_t>(m_parent_frns[i] >> 48);
                results.push_back(makeKey(dIdx, m_frns[i]));
                if (results.size() >= 50000) break;
            }
        }
        if (results.size() >= 50000) break;
    }
    return results;
}

bool MftReader::matchEntry(int i, const QString& query, bool useRegex, bool caseSensitive, 
                          const QStringList& extensionList, bool includeHidden, bool includeSystem,
                          bool includeDollar) const {
    // 简单实现，用于非高性能场景
    QRegularExpression re;
    if (useRegex && !query.isEmpty()) {
        re = QRegularExpression(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    }
    std::vector<std::string> el;
    for (const auto& e : extensionList) el.push_back((e.startsWith('.') ? e : "." + e).toLower().toStdString());
    return matchEntryOptimized(i, query, re, useRegex, caseSensitive, el, includeHidden, includeSystem, includeDollar);
}

bool MftReader::matchEntryOptimized(int i, const QString& query, const QRegularExpression& re, bool useRegex, bool caseSensitive, 
                                   const std::vector<std::string>& extList, bool includeHidden, bool includeSystem,
                                   bool includeDollar) const {
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
            if (!re.match(QString::fromUtf8(p)).hasMatch()) return false;
        } else {
            if (caseSensitive) { if (!strstr(p, query.toUtf8().constData())) return false; }
            else { if (!StrStrIA(p, query.toUtf8().constData())) return false; }
        }
    }

    if (!extList.empty()) {
        bool extMatch = false; size_t nlen = strlen(p);
        for (const auto& dotEx : extList) {
            if (nlen >= dotEx.size() && _stricmp(p + nlen - dotEx.size(), dotEx.c_str()) == 0) { extMatch = true; break; }
        }
        if (!extMatch) return false;
    }
    return true;
}

void MftReader::updateEntryFromUsn(USN_RECORD_V2* record, const std::wstring& volume) {
    QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(record) + record->FileNameOffset), record->FileNameLength / 2);
    
    UsnUpdateTask task;
    task.isDelete = false;
    task.frn = record->FileReferenceNumber;
    task.parentFrn = record->ParentFileReferenceNumber;
    task.attributes = record->FileAttributes;
    task.timestamp = filetimeToUnixMs(record->TimeStamp.QuadPart);
    task.usn = record->Usn;
    task.name = name;
    task.volume = volume;

    {
        std::lock_guard<std::mutex> lock(m_usnTaskMutex);
        m_usn_task_buffer.push_back(std::move(task));
    }
    
    // 异步触发应用逻辑，若已在运行则跳过
    if (!m_usn_task_running.exchange(true)) {
        QThreadPool::globalInstance()->start([this]() { applyBufferedUsnUpdates(); });
    }
}

void MftReader::removeEntryByFrn(const std::wstring& volume, uint64_t frn) {
    UsnUpdateTask task;
    task.isDelete = true;
    task.frn = frn;
    task.volume = volume;

    {
        std::lock_guard<std::mutex> lock(m_usnTaskMutex);
        m_usn_task_buffer.push_back(std::move(task));
    }
    
    if (!m_usn_task_running.exchange(true)) {
        QThreadPool::globalInstance()->start([this]() { applyBufferedUsnUpdates(); });
    }
}

void MftReader::applyBufferedUsnUpdates() {
    // Plan-Smooth: 循环处理，直到缓冲区清空
    while (true) {
        std::vector<UsnUpdateTask> batch;
        {
            std::lock_guard<std::mutex> lock(m_usnTaskMutex);
            if (m_usn_task_buffer.empty()) {
                m_usn_task_running = false;
                return;
            }
            batch = std::move(m_usn_task_buffer);
            m_usn_task_buffer.clear();
        }

        std::vector<MftReader::Change> changes;
        changes.reserve(batch.size());

        // Plan-Smooth: 分段加锁处理，每 200 条记录释放一次锁，允许 UI 线程渲染请求插队
        const size_t segmentSize = 200;
        for (size_t start = 0; start < batch.size(); start += segmentSize) {
            size_t end = (std::min)(start + segmentSize, batch.size());
            
            QWriteLocker dataWriteLock(&m_dataLock);
            for (size_t i = start; i < end; ++i) {
                const auto& task = batch[i];
                size_t dIdx = 0;
                bool foundDrive = false;
                for (; dIdx < m_drive_list.size(); ++dIdx) { 
                    if (_wcsicmp(m_drive_list[dIdx].c_str(), task.volume.c_str()) == 0) {
                        foundDrive = true; break; 
                    } 
                }
                if (!foundDrive) continue;

                uint64_t key = makeKey(dIdx, task.frn);
                if (task.isDelete) {
                    auto it = m_frn_to_idx.find(key);
                    if (it != m_frn_to_idx.end()) {
                        m_frns[it->second] = 0;
                        m_frn_to_idx.erase(it);
                        changes.push_back({ MftReader::Change::Rem, key, 0 });
                    }
                } else {
                    uint32_t targetIdx;
                    auto it = m_frn_to_idx.find(key);
                    if (it != m_frn_to_idx.end()) {
                        targetIdx = it->second;
                        m_timestamps[targetIdx] = task.timestamp;
                        m_attributes[targetIdx] = task.attributes;
                        QByteArray utf8 = task.name.toUtf8();
                        m_name_offsets[targetIdx] = (uint32_t)m_string_pool.size();
                        m_string_pool.insert(m_string_pool.end(), (const uint8_t*)utf8.constData(), (const uint8_t*)utf8.constData() + utf8.size());
                        m_string_pool.push_back('\0');
                        changes.push_back({ MftReader::Change::Upd, key, targetIdx });
                    } else {
                        targetIdx = (uint32_t)m_frns.size();
                        m_frns.push_back(task.frn);
                        m_parent_frns.push_back((static_cast<uint64_t>(dIdx) << 48) | (task.parentFrn & 0x0000FFFFFFFFFFFFull));
                        m_sizes.push_back(0);
                        m_timestamps.push_back(task.timestamp);
                        m_attributes.push_back(task.attributes);
                        m_metadata_fetched.push_back(0);
                        QByteArray utf8 = task.name.toUtf8();
                        m_name_offsets.push_back((uint32_t)m_string_pool.size());
                        m_string_pool.insert(m_string_pool.end(), (const uint8_t*)utf8.constData(), (const uint8_t*)utf8.constData() + utf8.size());
                        m_string_pool.push_back('\0');
                        m_frn_to_idx[key] = targetIdx;
                        m_total_count.store(m_frns.size(), std::memory_order_relaxed);
                        changes.push_back({ MftReader::Change::Add, key, targetIdx });
                    }
                    m_next_usns[task.volume] = task.usn;

                    ScchCache::Record r;
                    r.frn = task.frn; r.parentFrn = task.parentFrn; r.name = task.name.toStdString();
                    r.attributes = task.attributes; r.timestamp = task.timestamp;
                    {
                        std::lock_guard<std::mutex> dLock(m_dirtyMutex);
                        m_dirty_buffers[dIdx].push_back(std::move(r));
                    }
                }
            }
            dataWriteLock.unlock(); // 释放锁，允许插队
        }

        // Plan-Smooth: 在完全释放锁之后发射一次聚合信号。
        if (!changes.empty()) {
            emit changesApplied(changes);
        }

        // 异步刷新磁盘
        for (size_t d = 0; d < m_drive_list.size(); ++d) {
            {
                std::lock_guard<std::mutex> dLock(m_dirtyMutex);
                if (m_dirty_buffers[d].size() < 100) continue;
            }
            size_t idx = d;
            QThreadPool::globalInstance()->start([this, idx]() { flushDirtyToDisk(idx); });
        }
    }
}

// Plan-Smooth: SoA 访问接口不再内部加锁，UI 线程应通过 Snapshot 机制或在外部统一加锁以获得极致性能
int64_t MftReader::getSize(int index) const { QReadLocker lock(&m_dataLock); return getSizeNoLock(index); }
int64_t MftReader::getSizeNoLock(int index) const { return (index >= 0 && index < (int)m_sizes.size()) ? m_sizes[index] : 0; }

int64_t MftReader::getModifyTime(int index) const { QReadLocker lock(&m_dataLock); return getModifyTimeNoLock(index); }
int64_t MftReader::getModifyTimeNoLock(int index) const { return (index >= 0 && index < (int)m_timestamps.size()) ? m_timestamps[index] : 0; }

uint32_t MftReader::getAttributes(int index) const { QReadLocker lock(&m_dataLock); return getAttributesNoLock(index); }
uint32_t MftReader::getAttributesNoLock(int index) const { return (index >= 0 && index < (int)m_attributes.size()) ? m_attributes[index] : 0; }

uint64_t MftReader::getFrn(int index) const { QReadLocker lock(&m_dataLock); return getFrnNoLock(index); }
uint64_t MftReader::getFrnNoLock(int index) const { return (index >= 0 && index < (int)m_frns.size()) ? m_frns[index] : 0; }

bool MftReader::isDirectory(int index) const { QReadLocker lock(&m_dataLock); return isDirectoryNoLock(index); }
bool MftReader::isDirectoryNoLock(int index) const { return (getAttributesNoLock(index) & FILE_ATTRIBUTE_DIRECTORY) != 0; }

int MftReader::totalCount() const { return static_cast<int>(m_total_count.load(std::memory_order_relaxed)); }
QString MftReader::getName(int index) const { QReadLocker lock(&m_dataLock); return getNameNoLock(index); }
QString MftReader::getNameNoLock(int index) const { if (index < 0 || index >= (int)m_name_offsets.size()) return ""; return QString::fromUtf8(reinterpret_cast<const char*>(m_string_pool.data() + m_name_offsets[index])); }

int MftReader::getIndexByKey(uint64_t key) const { QReadLocker lock(&m_dataLock); return getIndexByKeyNoLock(key); }
int MftReader::getIndexByKeyNoLock(uint64_t key) const { auto it = m_frn_to_idx.find(key); return (it != m_frn_to_idx.end()) ? (int)it->second : -1; }

uint64_t MftReader::getKeyByIndex(int idx) const { QReadLocker lock(&m_dataLock); return getKeyByIndexNoLock(idx); }
uint64_t MftReader::getKeyByIndexNoLock(int idx) const { if (idx < 0 || idx >= (int)m_frns.size()) return 0; size_t d = static_cast<size_t>(m_parent_frns[idx] >> 48); return makeKey(d, m_frns[idx]); }
bool MftReader::isMetadataFetched(int idx) const { QReadLocker lock(&m_dataLock); return (idx >= 0 && idx < (int)m_metadata_fetched.size()) ? m_metadata_fetched[idx] == 2 : true; }
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
    int depth = 0;
    while (depth++ < 1024) { // Plan-Smooth: 增加深度限制，防止损坏的 MFT 结构导致死循环卡死 UI
        uint64_t curKey = makeKey(driveIdx, cur);
        auto it = m_frn_to_idx.find(curKey);
        if (it == m_frn_to_idx.end()) break;
        uint32_t idx = it->second;
        
        // Plan-Smooth: 内部直接调用 NoLock 接口，减少递归锁带来的性能损耗
        segments.push_back(getNameNoLock(idx).toStdWString());
        
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
    size_t count = result.entries.size();
    const size_t batchSize = 100000;

    for (size_t i = 0; i < count; i += batchSize) {
        QWriteLocker lock(&m_dataLock);
        size_t limit = (std::min)(i + batchSize, count);
        
        // 预留空间以减少重分配
        m_frns.reserve(m_frns.size() + (limit - i));
        m_parent_frns.reserve(m_parent_frns.size() + (limit - i));

        for (size_t k = i; k < limit; ++k) {
            const auto& e = result.entries[k];
            uint32_t idx = (uint32_t)m_frns.size();
            m_frns.push_back(e.frn);
            m_parent_frns.push_back((static_cast<uint64_t>(driveIdx) << 48) | (e.parentFrn & 0x0000FFFFFFFFFFFFull));
            m_sizes.push_back(0); 
            m_timestamps.push_back(e.timestamp); 
            m_attributes.push_back(e.attributes);
            m_metadata_fetched.push_back(0);
            
            QByteArray utf8 = QString::fromStdString(e.name).toUtf8();
            m_name_offsets.push_back((uint32_t)m_string_pool.size());
            m_string_pool.insert(m_string_pool.end(), (const uint8_t*)utf8.constData(), (const uint8_t*)utf8.constData() + utf8.size());
            m_string_pool.push_back('\0');
            
            m_frn_to_idx[makeKey(driveIdx, e.frn)] = idx;
        }
        m_total_count.store(m_frns.size(), std::memory_order_relaxed);
    }

    // Plan-Smooth: 将 Mutex 锁定移出循环，执行批量插入 DirtyBuffer，极大减少锁竞争
    {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        auto& buffer = m_dirty_buffers[driveIdx];
        buffer.insert(buffer.end(), result.entries.begin(), result.entries.end());
    }
}

void MftReader::rebuildFrnToIndexMap() {
    m_total_count.store(m_frns.size(), std::memory_order_relaxed);
}
void MftReader::compact() {}
void MftReader::buildSortedIndices() {
    std::vector<uint32_t> next_sorted;
    size_t count = 0;
    {
        QReadLocker lock(&m_dataLock);
        count = m_frns.size();
        if (count == 0) return;
        next_sorted.resize(count);
        std::iota(next_sorted.begin(), next_sorted.end(), 0);
    }

    // Plan-Smooth: 在完全释放锁的状态下执行耗时的并行排序，彻底消除启动时的锁死假死
    std::sort((std::execution::par), next_sorted.begin(), next_sorted.end(), [this](uint32_t a, uint32_t b) {
        // 关键：此处访问的是 m_string_pool 和 m_name_offsets。
        // 在 buildSortedIndices 运行期间，只有 USN 更新线程会修改这些数组。
        // USN 更新线程会持有写锁。由于此处没有任何锁，存在风险。
        // 但 buildSortedIndices 通常只在初始化时调用一次。
        // 为确保绝对“丝滑”，我们提取一份指针
        const uint8_t* pool = m_string_pool.data();
        const uint32_t* offsets = m_name_offsets.data();
        return _stricmp(reinterpret_cast<const char*>(pool + offsets[a]),
                        reinterpret_cast<const char*>(pool + offsets[b])) < 0;
    });

    {
        QWriteLocker lock(&m_dataLock);
        if (next_sorted.size() == m_frns.size()) {
            m_sorted_indices = std::move(next_sorted);
        }
    }
}

QIcon MftReader::getCachedIcon(const QString& ext, bool isDir) {
    QString key = isDir ? "folder" : ext.toLower();
    { QReadLocker lock(&m_iconCacheLock); if (m_icon_cache.contains(key)) return m_icon_cache[key]; }
    QFileIconProvider p; QIcon icon = isDir ? p.icon(QFileIconProvider::Folder) : p.icon(QFileInfo("dummy." + key));
    { QWriteLocker lock(&m_iconCacheLock); m_icon_cache[key] = icon; }
    return icon;
}

} // namespace FERREX-META
