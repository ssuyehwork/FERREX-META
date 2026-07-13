#include "NtfsVolumeMftParser.h"
#include <QDebug>
#include <QtConcurrent/QtConcurrent>
#include "ScchCache.h"

namespace FERREX {

static int64_t filetimeToUnixMs(int64_t filetime) {
    return (filetime - 116444736000000000LL) / 10000;
}

bool NtfsVolumeMftParser::loadMftDirect(const std::wstring& volume, MftReader::DriveResult& result) {
    std::wstring dev = L"\\\\.\\" + volume;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        qWarning() << "[MftReader] 无法打开卷句柄" << QString::fromStdWString(volume) << "错误码:" << GetLastError();
        return false;
    }

    std::wstring rootPath = volume + L"\\";
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

    auto& readerInstance = MftReader::instance();

    while (true) {
        BOOL ok = DeviceIoControl(h, FSCTL_ENUM_USN_DATA, &ed, sizeof(ed), buf.data(), (DWORD)buf.size(), &cb, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            
            qDebug() << "[MFT] Enumeration encountered non-fatal error:" << err << "on volume" << QString::fromStdWString(volume);
            
            if (++consecutiveErrors > 10) break;

            if (err == ERROR_ACCESS_DENIED || err == ERROR_INVALID_PARAMETER) {
                ed.StartFileReferenceNumber++; 
                continue;
            }
            break; 
        }
        consecutiveErrors = 0;

        if (readerInstance.m_isStopping.load()) break;
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

            if (recordCount - lastSavedCount >= 100000) {
                std::vector<ScchDataPackage> delta;
                delta.reserve(recordCount - lastSavedCount);
                
                const uint8_t* poolBase = result.string_pool.data();
                
                for (int i = lastSavedCount; i < recordCount; ++i) {
                    const auto& re = result.entries[i];
                    ScchDataPackage pkg;
                    pkg.frn = re.frn;
                    pkg.parent_frn = re.parentFrn;
                    pkg.size = re.size;
                    pkg.timestamp = re.modifyTime;
                    pkg.attributes = re.attributes;
                    
                    pkg.name = std::string(reinterpret_cast<const char*>(poolBase + re.nameOffset));
                    
                    delta.push_back(std::move(pkg));
                }

                std::string path_base = "FERREX/cache/" + QString::fromStdWString(volume).left(1).toStdString();
                uint64_t currentUsn = ed.StartFileReferenceNumber;

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

} // namespace FERREX
