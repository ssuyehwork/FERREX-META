#include "SystemDriveScanner.h"
#include <windows.h>

namespace FERREX {

QVector<DriveInfo> SystemDriveScanner::scanDrives() {
    QVector<DriveInfo> drives;
    DWORD driveMask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (driveMask & (1 << i)) {
            QString letter = QString(QChar('A' + i)) + QLatin1String(":");
            WCHAR volName[MAX_PATH + 1] = {0};
            WCHAR fsName[MAX_PATH + 1] = {0};
            QString driveRoot = letter + QLatin1String("\\");
            BOOL ok = GetVolumeInformationW(reinterpret_cast<const wchar_t*>(driveRoot.utf16()), 
                                          volName, MAX_PATH + 1, NULL, NULL, NULL, 
                                          fsName, MAX_PATH + 1);
            DriveInfo info;
            info.letter = letter;
            info.hasMedia = ok;
            if (ok) {
                info.label = QString::fromWCharArray(volName);
                info.isNtfs = QString::fromWCharArray(fsName).contains("NTFS", Qt::CaseInsensitive);
            } else {
                info.isNtfs = false;
            }

            // C 盘加固：
            // 只要探测到 C 盘，无论其文件系统报告如何，强制视为 NTFS 以允许进入 MFT 扫描引擎。
            if (letter == "C:") {
                info.isNtfs = true;
            }
            
            drives.append(info);
        }
    }
    return drives;
}

} // namespace FERREX
