#pragma once
#include <QString>
#include <QStringList>
#include <QDir>
#include <QProcess>
#include <vector>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <ntddstor.h>
#endif
#include "../ui/UiHelper.h"

namespace FERREX {

/**
 * @brief 系统服务层工具类 (ShellHelper)
 * 封装 Windows 原生 Shell 调用与通用格式化逻辑。
 */
class ShellHelper {
public:
    /**
     * @brief 移入回收站
     */
    static bool moveToTrash(const QStringList& paths) {
#ifdef Q_OS_WIN
        static QThreadStorage<ScopedComInit> s_comInit;
        if (!s_comInit.hasLocalData()) s_comInit.setLocalData(ScopedComInit());

        if (paths.isEmpty()) return true;
        std::wstring from;
        for (const QString& p : paths) {
            from += QDir::toNativeSeparators(p).toStdWString() + L'\0';
        }
        from += L'\0';

        SHFILEOPSTRUCTW fileOp = { 0 };
        fileOp.wFunc = FO_DELETE;
        fileOp.pFrom = from.c_str();
        fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
        return SHFileOperationW(&fileOp) == 0;
#else
        return false;
#endif
    }

    /**
     * @brief 执行复制或移动
     */
    static bool copyOrMoveItems(const QStringList& sourcePaths, const QString& destDir, bool isMove) {
#ifdef Q_OS_WIN
        static QThreadStorage<ScopedComInit> s_comInit;
        if (!s_comInit.hasLocalData()) s_comInit.setLocalData(ScopedComInit());

        if (sourcePaths.isEmpty() || destDir.isEmpty()) return false;
        
        std::wstring from;
        for (const QString& p : sourcePaths) {
            from += QDir::toNativeSeparators(p).toStdWString() + L'\0';
        }
        from += L'\0';

        std::wstring to = QDir::toNativeSeparators(destDir).toStdWString() + L'\0' + L'\0';

        SHFILEOPSTRUCTW fileOp = { 0 };
        fileOp.wFunc = isMove ? FO_MOVE : FO_COPY;
        fileOp.pFrom = from.c_str();
        fileOp.pTo = to.c_str();
        fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR;
        return SHFileOperationW(&fileOp) == 0;
#else
        return false;
#endif
    }

    /**
     * @brief 显示文件属性对话框
     */
    static void showProperties(const QString& path) {
#ifdef Q_OS_WIN
        static QThreadStorage<ScopedComInit> s_comInit;
        if (!s_comInit.hasLocalData()) s_comInit.setLocalData(ScopedComInit());

        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_INVOKEIDLIST;
        sei.lpVerb = L"properties";
        std::wstring wpath = QDir::toNativeSeparators(path).toStdWString();
        sei.lpFile = wpath.c_str();
        sei.nShow = SW_SHOW;
        ShellExecuteExW(&sei);
#endif
    }

    /**
     * @brief 在资源管理器中定位
     */
    static void openInExplorer(const QString& path) {
#ifdef Q_OS_WIN
        QStringList args;
        args << "/select," << QDir::toNativeSeparators(path);
        QProcess::startDetached("explorer", args);
#endif
    }

    /**
     * @brief 探测是否为固态硬盘 (SSD)
     * 2026-06-xx 任务 4.1：物理寻道特征探测。
     * 理由：HDD 对多线程随机 I/O 极其敏感，磁头剧烈寻道会导致性能雪崩；SSD 则可维持较高并发。
     */
    static bool isSolidStateDrive(const QString& drivePath) {
#ifdef Q_OS_WIN
        QString path = drivePath;
        if (!path.endsWith("\\")) path += "\\";
        QString volumePath = "\\\\.\\" + path.left(2); // 格式如 \\.\C:

        HANDLE hDevice = CreateFileW(reinterpret_cast<const wchar_t*>(volumePath.utf16()),
                                     0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) return false;

        STORAGE_PROPERTY_QUERY query;
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;

        DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor = { 0 };
        DWORD bytesReturned;
        bool isSSD = false;

        if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                            &query, sizeof(query),
                            &descriptor, sizeof(descriptor),
                            &bytesReturned, NULL)) {
            isSSD = !descriptor.IncursSeekPenalty;
        }

        CloseHandle(hDevice);
        return isSSD;
#else
        return true; // 非 Windows 环境默认视为高速介质
#endif
    }

    /**
     * @brief 格式化字节大小
     */
    static QString formatSize(qint64 bytes) {
        if (bytes < 1024) return QString("%1 B").arg(bytes);
        if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 2);
        if (bytes < 1024LL * 1024 * 1024) return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
        return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }
};

} // namespace FERREX
