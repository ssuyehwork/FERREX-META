#include "UsnWatcher.h"
#include "MftReader.h"
#include <QDebug>
#include <winioctl.h>
#include <cstring> // 引入 std::memcpy

namespace ArcMeta {

UsnWatcher::UsnWatcher(const std::wstring& volume, uint64_t startUsn, QObject* parent)
    : QThread(parent), m_volume(volume), m_lastUsn(startUsn), m_stopRequested(false) {
    
    std::wstring devPath = L"\\\\.\\" + m_volume;
    if (devPath.back() == L'\\') devPath.pop_back();

    m_hVolume = CreateFileW(devPath.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS,
                           NULL);
    
    if (m_hVolume == INVALID_HANDLE_VALUE) {
        qDebug() << "[UsnWatcher] [严重错误]：无法打开卷句柄" << QString::fromStdWString(devPath) 
                 << "错误码:" << GetLastError() << "（请确保使用管理员权限运行！）";
    } else {
        qDebug() << "[UsnWatcher] 成功打开卷句柄:" << QString::fromStdWString(devPath);
    }
}

UsnWatcher::~UsnWatcher() {
    stop();
    if (m_hVolume != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hVolume);
        m_hVolume = INVALID_HANDLE_VALUE;
    }
}

void UsnWatcher::stop() {
    m_stopRequested.store(true);
    if (isRunning()) {
        wait();
    }
}

void UsnWatcher::run() {
    qDebug() << "[UsnWatcher] 监控线程已激活，目标卷:" << QString::fromStdWString(m_volume) 
             << "配置的起始 USN 指针:" << m_lastUsn;

    if (m_hVolume == INVALID_HANDLE_VALUE) {
        qDebug() << "[UsnWatcher] [退出] 卷句柄无效，监控线程直接退出！";
        return;
    }

    // 1. 获取 Journal ID
    USN_JOURNAL_DATA_V0 journalData;
    DWORD bytesReturned;
    if (!DeviceIoControl(m_hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &journalData, sizeof(journalData), &bytesReturned, NULL)) {
        qDebug() << "[UsnWatcher] [退出] FSCTL_QUERY_USN_JOURNAL 失败，错误码:" << GetLastError() << "线程退出！";
        return;
    }

    qDebug() << "[UsnWatcher] 成功关联 USN 日志。JournalID:" << journalData.UsnJournalID 
             << "LowestValidUsn:" << journalData.LowestValidUsn 
             << "NextUsn (当前磁盘最新位置):" << journalData.NextUsn;

    // 2. 离线追平逻辑：若 m_lastUsn 为 0，从当前 NextUsn 开始
    if (m_lastUsn == 0) {
        m_lastUsn = journalData.NextUsn;
        qDebug() << "[UsnWatcher] m_lastUsn 为 0，已自动重置监控起始点为 NextUsn:" << m_lastUsn;
    }

    READ_USN_JOURNAL_DATA_V0 readData{};
    readData.StartUsn = m_lastUsn;
    readData.ReasonMask = 0xFFFFFFFF; // 监控所有原因
    readData.ReturnOnlyOnClose = 0;
    readData.Timeout = 0;
    readData.BytesToWaitFor = 0;
    readData.UsnJournalID = journalData.UsnJournalID;

    const int bufferSize = 128 * 1024;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[bufferSize]);

    // 用于 10 秒心跳自检的时间戳
    ULONGLONG lastHeartbeatTime = GetTickCount64();

    while (!m_stopRequested.load()) {
        ULONGLONG currentTime = GetTickCount64();
        // [心跳监控] 每 10 秒打印一次运行状态，用于判断线程是否卡死或静默退出
        if (currentTime - lastHeartbeatTime >= 10000) {
            qDebug() << "[UsnWatcher] [心跳监控] 线程存活且轮询中... 目标卷:" << QString::fromStdWString(m_volume) 
                     << "等待读取的 StartUsn 指针:" << readData.StartUsn;
            lastHeartbeatTime = currentTime;
        }

        bytesReturned = 0;
        if (!DeviceIoControl(m_hVolume, FSCTL_READ_USN_JOURNAL, &readData, sizeof(readData), buffer.get(), bufferSize, &bytesReturned, NULL)) {
            DWORD err = GetLastError();
            qDebug() << "[UsnWatcher] [API警告] DeviceIoControl 失败，错误码:" << err;

            // 引入自愈探测
            if (err == ERROR_JOURNAL_DELETE_IN_PROGRESS || 
                err == ERROR_JOURNAL_NOT_ACTIVE || 
                err == ERROR_INVALID_PARAMETER ||
                err == ERROR_JOURNAL_ENTRY_DELETED) {
                
                qDebug() << "[UsnWatcher] 触发自愈重置机制... 错误码:" << err << QString::fromStdWString(m_volume);
                
                USN_JOURNAL_DATA_V0 newJournalData;
                DWORD queryBytes;
                if (DeviceIoControl(m_hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &newJournalData, sizeof(newJournalData), &queryBytes, NULL)) {
                    readData.UsnJournalID = newJournalData.UsnJournalID;
                    readData.StartUsn = newJournalData.NextUsn;
                    m_lastUsn = readData.StartUsn;
                    qDebug() << "[UsnWatcher] 自愈成功，新 JournalID:" << newJournalData.UsnJournalID << "新 StartUsn:" << readData.StartUsn;
                } else {
                    qDebug() << "[UsnWatcher] 自愈再次查询失败，退回 0";
                    readData.StartUsn = 0;
                    m_lastUsn = 0;
                }
            }
            
            // 出错时小步长等待，确保可及时退出
            for (int i = 0; i < 10 && !m_stopRequested.load(); ++i) msleep(50);
            continue;
        }

        if (bytesReturned <= sizeof(USN)) {
            // 无新数据，休眠 500ms 后继续下一次轮询
            for (int i = 0; i < 10 && !m_stopRequested.load(); ++i) msleep(50);
            continue;
        }

        uint8_t* pRecord = buffer.get() + sizeof(USN);
        uint8_t* pEnd = buffer.get() + bytesReturned;

        qDebug() << "[UsnWatcher] [捕获数据] 成功获取原始日志包！大小 (bytesReturned):" << bytesReturned;

        std::vector<uint8_t*> updateBatch; 
        int parsedRecordCount = 0;

        while (pRecord < pEnd && !m_stopRequested.load()) {
            USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(pRecord);
            
            if (header->RecordLength == 0) {
                qDebug() << "[UsnWatcher] [严重警告] 捕获到无效 RecordLength (0)，强行跳出以防止死锁！";
                break;
            }

            parsedRecordCount++;

            if (header->MajorVersion == 2 || header->MajorVersion == 3) {
                uint32_t reason = 0;
                uint64_t frn = 0;
                QString name;

                if (header->MajorVersion == 2) {
                    USN_RECORD_V2* v2 = reinterpret_cast<USN_RECORD_V2*>(pRecord);
                    reason = v2->Reason;
                    frn = v2->FileReferenceNumber;
                    name = QString::fromUtf16(reinterpret_cast<const char16_t*>(pRecord + v2->FileNameOffset), v2->FileNameLength / 2);
                } else {
                    USN_RECORD_V3* v3 = reinterpret_cast<USN_RECORD_V3*>(pRecord);
                    reason = v3->Reason;
                    std::memcpy(&frn, &v3->FileReferenceNumber, sizeof(uint64_t));
                    name = QString::fromUtf16(reinterpret_cast<const char16_t*>(pRecord + v3->FileNameOffset), v3->FileNameLength / 2);
                }
                
                // 打印每一条由底层驱动传递过来的实时变更记录
                qDebug() << QString("[UsnWatcher] 逐条分析 -> 序号: #%1 | 名称: %2 | FRN: %3 | 原因码 (Reason): 0x%4")
                            .arg(parsedRecordCount)
                            .arg(name)
                            .arg(frn)
                            .arg(reason, 8, 16, QChar('0'));

                if (reason & (USN_REASON_FILE_CREATE | USN_REASON_DATA_OVERWRITE | USN_REASON_BASIC_INFO_CHANGE | USN_REASON_RENAME_NEW_NAME)) {
                    updateBatch.push_back(pRecord);
                } else if (reason & USN_REASON_FILE_DELETE) {
                    qDebug() << "[UsnWatcher] 检测到文件删除，FRN:" << frn;
                    MftReader::instance().removeEntryByFrn(m_volume, frn);
                }
            }
            pRecord += header->RecordLength;
        }

        if (!updateBatch.empty()) {
            qDebug() << "[UsnWatcher] 正在将有效变动数据（数量:" << updateBatch.size() << "）投递给 MftReader 批量缓冲区";
            const size_t chunkSize = 1000;
            for (size_t i = 0; i < updateBatch.size(); i += chunkSize) {
                if (m_stopRequested.load()) break;
                size_t end = (std::min)(i + chunkSize, updateBatch.size());
                std::vector<uint8_t*> chunk(updateBatch.begin() + i, updateBatch.begin() + end);
                MftReader::instance().updateEntriesFromUsnBatch(chunk, m_volume);
                
                QThread::msleep(5); 
            }
        }

        // 更新起始 USN 为本次读取后的 NextUsn
        readData.StartUsn = *reinterpret_cast<USN*>(buffer.get());
        m_lastUsn = readData.StartUsn;
        qDebug() << "[UsnWatcher] 起始 USN 已推移推至最新 NextUsn:" << readData.StartUsn;
    }

    qDebug() << "[UsnWatcher] 监控线程已正常退出。";
}

void UsnWatcher::handleRecord(USN_RECORD_V2* pRecord) {
    if (!pRecord) return;
    
    uint8_t* pRaw = reinterpret_cast<uint8_t*>(pRecord);
    USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(pRaw);
    uint32_t reason = 0;
    uint64_t frn = 0;

    if (header->MajorVersion == 2) {
        reason = pRecord->Reason;
        frn = pRecord->FileReferenceNumber;
    } else if (header->MajorVersion == 3) {
        USN_RECORD_V3* v3 = reinterpret_cast<USN_RECORD_V3*>(pRaw);
        reason = v3->Reason;
        std::memcpy(&frn, &v3->FileReferenceNumber, sizeof(uint64_t));
    } else {
        return;
    }

    if (reason & (USN_REASON_FILE_CREATE | USN_REASON_DATA_OVERWRITE | USN_REASON_BASIC_INFO_CHANGE | USN_REASON_RENAME_NEW_NAME)) {
        MftReader::instance().updateEntryFromUsn(pRaw, m_volume);
    }
    else if (reason & USN_REASON_FILE_DELETE) {
        MftReader::instance().removeEntryByFrn(m_volume, frn);
    }
}

} // namespace ArcMeta