#include "UsnWatcher.h"
#include "MftReader.h"
#include <QDebug>
#include <winioctl.h>

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
        qDebug() << "[UsnWatcher] 严重错误：无法打开卷句柄" << QString::fromStdWString(devPath) << "错误码:" << GetLastError();
    } else {
        qDebug() << "[UsnWatcher] 成功打开卷句柄，准备监听:" << QString::fromStdWString(m_volume);
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
    if (m_hVolume == INVALID_HANDLE_VALUE) return;

    // 1. 获取 Journal ID
    USN_JOURNAL_DATA_V0 journalData;
    DWORD bytesReturned;
    if (!DeviceIoControl(m_hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &journalData, sizeof(journalData), &bytesReturned, NULL)) {
        qDebug() << "[UsnWatcher] 错误：无法查询 Journal 状态" << QString::fromStdWString(m_volume) << "错误码:" << GetLastError();
        return;
    }
    qDebug() << "[UsnWatcher] 正在启动实时监听循环，盘符:" << QString::fromStdWString(m_volume) << "起始 USN:" << m_lastUsn;

    // 2. 离线追平逻辑：若 m_lastUsn 为 0，从当前 NextUsn 开始
    if (m_lastUsn == 0) {
        m_lastUsn = journalData.NextUsn;
    }

    READ_USN_JOURNAL_DATA_V0 readData{};
    readData.StartUsn = m_lastUsn;
    readData.ReasonMask = 0xFFFFFFFF; // 监控所有原因
    readData.ReturnOnlyOnClose = 0;
    readData.Timeout = 0;
    readData.BytesToWaitFor = 0;
    readData.UsnJournalID = journalData.UsnJournalID;

    // 根据规范：使用 std::unique_ptr<uint8_t[]> 管理缓冲区
    const int bufferSize = 128 * 1024;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[bufferSize]);

    while (!m_stopRequested.load()) {
        if (!DeviceIoControl(m_hVolume, FSCTL_READ_USN_JOURNAL, &readData, sizeof(readData), buffer.get(), bufferSize, &bytesReturned, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_JOURNAL_DELETE_IN_PROGRESS || err == ERROR_JOURNAL_NOT_ACTIVE) {
                qDebug() << "[UsnWatcher] 警告：Journal 失效，线程退出" << QString::fromStdWString(m_volume);
                break;
            }
            // 出错时小步长等待，确保可及时退出
            for (int i = 0; i < 10 && !m_stopRequested.load(); ++i) msleep(50);
            continue;
        }

        if (bytesReturned <= sizeof(USN)) {
            // 无新数据，小步长等待
            for (int i = 0; i < 10 && !m_stopRequested.load(); ++i) msleep(50);
            continue;
        }

        uint8_t* pRecord = buffer.get() + sizeof(USN);
        uint8_t* pEnd = buffer.get() + bytesReturned;

        while (pRecord < pEnd) {
            USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(pRecord);
            if (header->RecordLength == 0) break; // 物理隔离：防止 RecordLength 为 0 导致的死循环

            // 处理 V2 和 V3 版本记录
            if (header->MajorVersion == 2 || header->MajorVersion == 3) {
                handleRecord(reinterpret_cast<USN_RECORD_V2*>(pRecord));
            }

            pRecord += header->RecordLength;
        }

        // 更新起始 USN 为本次读取后的 NextUsn
        readData.StartUsn = *reinterpret_cast<USN*>(buffer.get());
        m_lastUsn = readData.StartUsn;
    }
}

void UsnWatcher::handleRecord(USN_RECORD_V2* pRecord) {
    USN_RECORD_COMMON_HEADER* header = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(pRecord);
    uint32_t reason;
    uint64_t frn;

    std::wstring fileName;
    if (header->MajorVersion == 2) {
        reason = pRecord->Reason;
        frn = pRecord->FileReferenceNumber;
        fileName = std::wstring(reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(pRecord) + pRecord->FileNameOffset), pRecord->FileNameLength / 2);
    } else if (header->MajorVersion == 3) {
        USN_RECORD_V3* v3 = reinterpret_cast<USN_RECORD_V3*>(pRecord);
        reason = v3->Reason;
        frn = *reinterpret_cast<uint64_t*>(&v3->FileReferenceNumber);
        fileName = std::wstring(reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(v3) + v3->FileNameOffset), v3->FileNameLength / 2);
    } else return;

    if (reason & (USN_REASON_FILE_CREATE | USN_REASON_CLOSE | USN_REASON_RENAME_NEW_NAME)) {
        qDebug() << "[UsnWatcher] " << QString::fromStdWString(m_volume) << "记录:" << QString::fromStdWString(fileName) << "原因:" << QString::number(reason, 16);
    }

    // 2026-05-28 物理增强：监听 REASON_CLOSE。对于大文件或复制操作，CLOSE 是获取最终元数据的物理锚点
    if (reason & (USN_REASON_FILE_CREATE | USN_REASON_DATA_OVERWRITE | USN_REASON_BASIC_INFO_CHANGE | USN_REASON_CLOSE)) {
        MftReader::instance().updateEntryFromUsn(pRecord, m_volume);
    }
    else if (reason & USN_REASON_FILE_DELETE) {
        MftReader::instance().removeEntryByFrn(m_volume, frn);
    }
    else if (reason & (USN_REASON_RENAME_NEW_NAME | USN_REASON_RENAME_OLD_NAME)) {
        // 重命名时统一调用 update，MftReader 内部会根据 FRN 自动定位并更新名称
        MftReader::instance().updateEntryFromUsn(pRecord, m_volume);
    }
}

} // namespace ArcMeta
