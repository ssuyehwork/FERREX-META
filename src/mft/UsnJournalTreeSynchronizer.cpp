#include "UsnJournalTreeSynchronizer.h"
#include <QWriteLocker>
#include <QReadLocker>
#include <QDebug>
#include <QThreadPool>

namespace FERREX {

static int64_t filetimeToUnixMs(int64_t filetime) {
    return (filetime - 116444736000000000LL) / 10000;
}

static void splitNameAndExt(const std::string& fullName, std::string& outExt) {
    outExt.clear();
    size_t lastDot = fullName.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        outExt = fullName.substr(lastDot + 1);
        std::transform(outExt.begin(), outExt.end(), outExt.begin(), ::tolower);
    }
}

void UsnJournalTreeSynchronizer::updateEntryFromUsn(MftReader* reader, USN_RECORD_V2* record, const std::wstring& volume) {
    if (!reader) return;

    USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(record);
    uint64_t frn, parentFrn, usn;
    uint32_t attr;
    LARGE_INTEGER timestamp;
    WORD fileNameLength, fileNameOffset;

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
        frn = *reinterpret_cast<uint64_t*>(&v3->FileReferenceNumber);
        parentFrn = *reinterpret_cast<uint64_t*>(&v3->ParentFileReferenceNumber);
        usn = v3->Usn;
        attr = v3->FileAttributes;
        timestamp = v3->TimeStamp;
        fileNameLength = v3->FileNameLength;
        fileNameOffset = v3->FileNameOffset;
    } else return;

    uint64_t fileSize = 0; 
    int64_t finalModifyTime = filetimeToUnixMs(timestamp.QuadPart);
    uint32_t finalAttr = attr;

    // --- 1. 锁外重度计算 & 编解码 ---
    QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(record) + fileNameOffset), fileNameLength / 2);
    QByteArray utf8 = name.toUtf8();
    std::string extStr;
    splitNameAndExt(utf8.toStdString(), extStr);

    // --- 2. 盘符索引读锁化预查 ---
    int dIdx = -1;
    {
        QReadLocker readLock(&reader->m_dataLock);
        for (size_t i = 0; i < reader->m_drive_list.size(); ++i) { 
            if (_wcsicmp(reader->m_drive_list[i].c_str(), volume.c_str()) == 0) { 
                dIdx = (int)i; 
                break; 
            } 
        }
    }
    if (dIdx == -1) {
        qDebug() << "[MftReader] 警告：接收到未索引驱动器的 USN 记录:" << QString::fromStdWString(volume);
        return;
    }

    uint64_t encodedPf = MftReader::makeKey((size_t)dIdx, parentFrn);
    uint64_t compositeKey = MftReader::makeKey(dIdx, frn);
    uint32_t finalIdx = 0;
    bool isNew = false;
    bool shouldCompact = false;

    // --- 3. 临界区极简写锁更新 ---
    {
        QWriteLocker lock(&reader->m_dataLock);
        auto it = reader->m_frn_to_idx.find(compositeKey);

        if (it != reader->m_frn_to_idx.end()) {
            finalIdx = it->second;
            reader->m_parent_frns[finalIdx] = encodedPf;
            
            auto itParent = reader->m_frn_to_idx.find(encodedPf);
            reader->m_parent_indices[finalIdx] = (itParent != reader->m_frn_to_idx.end()) ? itParent->second : 0xFFFFFFFF;

            reader->m_attributes[finalIdx] = finalAttr;
            reader->m_metadata_fetched[finalIdx] = 0; 
            
            reader->m_sizes[finalIdx] = fileSize;
            reader->m_timestamps[finalIdx] = finalModifyTime;

            uint32_t oldOff = reader->m_name_offsets[finalIdx];
            const char* oldPtr = reinterpret_cast<const char*>(reader->m_string_pool.data() + oldOff);
            size_t oldLen = strlen(oldPtr);
            
            uint32_t oldExtOff = reader->m_ext_offsets[finalIdx];
            reader->m_wasted_string_bytes += (strlen(reinterpret_cast<const char*>(reader->m_string_pool.data() + oldExtOff)) + 1);

            if ((size_t)utf8.size() <= oldLen) {
                memcpy(reader->m_string_pool.data() + oldOff, utf8.constData(), utf8.size());
                reader->m_string_pool[oldOff + utf8.size()] = '\0';
                if ((size_t)utf8.size() < oldLen) reader->m_wasted_string_bytes += (oldLen - utf8.size());
            } else {
                reader->m_wasted_string_bytes += (oldLen + 1);
                reader->m_name_offsets[finalIdx] = (uint32_t)reader->m_string_pool.size();
                reader->m_string_pool.insert(reader->m_string_pool.end(), utf8.begin(), utf8.end());
                reader->m_string_pool.push_back('\0');
            }

            reader->m_ext_offsets[finalIdx] = (uint32_t)reader->m_string_pool.size();
            reader->m_string_pool.insert(reader->m_string_pool.end(), extStr.begin(), extStr.end());
            reader->m_string_pool.push_back('\0');
        } else {
            finalIdx = (uint32_t)reader->m_frns.size();
            isNew = true;
            reader->m_frns.push_back(frn);
            reader->m_parent_frns.push_back(encodedPf);
            
            auto itParent = reader->m_frn_to_idx.find(encodedPf);
            reader->m_parent_indices.push_back(itParent != reader->m_frn_to_idx.end() ? itParent->second : 0xFFFFFFFF);

            reader->m_sizes.push_back(fileSize);
            reader->m_timestamps.push_back(finalModifyTime);
            reader->m_attributes.push_back(finalAttr);
            reader->m_metadata_fetched.push_back(0); 
            
            reader->m_name_offsets.push_back((uint32_t)reader->m_string_pool.size());
            reader->m_string_pool.insert(reader->m_string_pool.end(), utf8.begin(), utf8.end());
            reader->m_string_pool.push_back('\0');

            reader->m_ext_offsets.push_back((uint32_t)reader->m_string_pool.size());
            reader->m_string_pool.insert(reader->m_string_pool.end(), extStr.begin(), extStr.end());
            reader->m_string_pool.push_back('\0');

            reader->m_frn_to_idx[compositeKey] = finalIdx;
        }
        { std::unique_lock<std::shared_mutex> l(reader->m_pathCacheMutex); reader->m_path_cache.erase(compositeKey); }
        reader->m_next_usns[volume] = usn;
        reader->m_dirty_count++;
        {
            std::lock_guard<std::mutex> dLock(reader->m_dirtyLock);
            reader->m_dirty_indices.insert(finalIdx);
        }
        
        if (reader->m_wasted_string_bytes > 20 * 1024 * 1024 || reader->m_dead_count > 100000) {
            shouldCompact = true;
        }
    }

    // --- 4. 完全移出写锁范围，通过全局线程池无锁并发紧凑化整理 ---
    if (shouldCompact) {
        QThreadPool::globalInstance()->start([reader]() {
            reader->compact();
            reader->buildSortedIndices();
        });
    }

    bool shouldSave = false;
    {
        QWriteLocker lock(&reader->m_dataLock);
        if (reader->m_dirty_count >= 1000) { 
            reader->m_dirty_count = 0; 
            shouldSave = true;
        }
    }

    if (shouldSave) {
        QThreadPool::globalInstance()->start([reader, dIdx]() {
            reader->saveDriveToCache(dIdx);
        });
    }

    reader->requestMetadata(finalIdx);

    {
        std::lock_guard<std::mutex> journalLock(reader->m_journalMutex);
        reader->m_changeJournal.push_back({isNew ? MftReader::ChangeEvent::Added : MftReader::ChangeEvent::Updated, compositeKey, finalIdx});
        if (!reader->m_notifyTimer->isActive()) {
            QMetaObject::invokeMethod(reader->m_notifyTimer, "start", Qt::QueuedConnection);
        }
    }
}

void UsnJournalTreeSynchronizer::removeEntryByFrn(MftReader* reader, const std::wstring& volume, uint64_t frn) {
    if (!reader) return;

    QWriteLocker lock(&reader->m_dataLock);
    
    int dIdx = -1;
    for (size_t i = 0; i < reader->m_drive_list.size(); ++i) { 
        if (_wcsicmp(reader->m_drive_list[i].c_str(), volume.c_str()) == 0) { 
            dIdx = (int)i; 
            break; 
        } 
    }
    if (dIdx == -1) return;

    uint64_t compositeKey = MftReader::makeKey((size_t)dIdx, frn);

    auto it = reader->m_frn_to_idx.find(compositeKey);
    if (it != reader->m_frn_to_idx.end()) {
        uint32_t idx = it->second;

        {
            std::lock_guard<std::mutex> dLock(reader->m_dirtyLock);
            reader->m_dirty_indices.insert(idx); 
            reader->m_dead_frns[idx] = reader->m_frns[idx];
        }

        reader->m_frns[idx] = 0; 
        reader->m_frn_to_idx.erase(it);
        reader->m_dead_count++;
        reader->m_dirty_count++;

        auto itSorted = std::find(reader->m_sorted_indices.begin(), reader->m_sorted_indices.end(), idx);
        if (itSorted != reader->m_sorted_indices.end()) {
            reader->m_sorted_indices.erase(itSorted);
        }

        const char* p = reinterpret_cast<const char*>(reader->m_string_pool.data() + reader->m_name_offsets[idx]);
        reader->m_wasted_string_bytes += (strlen(p) + 1);
        
        { std::unique_lock<std::shared_mutex> lockCache(reader->m_pathCacheMutex); reader->m_path_cache.erase(compositeKey); }
        
        bool shouldCompact = (reader->m_dead_count > 50000 || reader->m_wasted_string_bytes > 10 * 1024 * 1024);
        
        lock.unlock(); 

        if (shouldCompact) {
            reader->compact();
            reader->buildSortedIndices();
        }
        {
            std::lock_guard<std::mutex> journalLock(reader->m_journalMutex);
            reader->m_changeJournal.push_back({MftReader::ChangeEvent::Removed, compositeKey, 0});
            if (!reader->m_notifyTimer->isActive()) {
                QMetaObject::invokeMethod(reader->m_notifyTimer, "start", Qt::QueuedConnection);
            }
        }
    }
}

} // namespace FERREX
