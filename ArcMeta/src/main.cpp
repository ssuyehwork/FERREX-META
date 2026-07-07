#ifndef NOMINMAX
#define NOMINMAX
#endif
//2813583 main 禁止删除此行
#include <QApplication>
#include "FramelessDialog.h"
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QSvgRenderer>
#include <QPainter>
#include <QLockFile>
#include <QDir>
#include <QMutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif
#include "ui/UiHelper.h"
#include "ui/Logger.h"
#include "ui/MainWindow.h"


#include "meta/MetadataManager.h"
#include "meta/CategoryRepo.h"
#include "mft/MftReader.h"
#include "core/CoreController.h"
#include "core/AutoImportManager.h"

/**
 * @brief 自定义日志处理程序，将 qDebug 消息重定向至本地 .log 文件
 * 2026-03-xx 按照用户要求：在手动运行 .exe 时，通过日志文件排查初始化挂起或信号丢失问题。
 */
void customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    Q_UNUSED(context); 
    static QMutex s_logMutex; // 2026-07-xx 物理加固：增加互斥锁，确保多线程扫描时日志不交织且不丢失
    QMutexLocker locker(&s_logMutex);

    static int writeCount = 0;
    QString fileName = "arcmeta_debug.log";

    // 2026-xx-xx 按照 Plan-97：同步容量哨兵逻辑
    if (++writeCount >= 100) {
        ArcMeta::Logger::rotateLogFiles(fileName);
        writeCount = 0;
    }

    QFile logFile(fileName);
    // 2026-07-xx 按照用户要求 (1.18)：开启强力落盘模式
    // 即使发生系统级闪退，也要确保最后一条日志已写入物理磁盘
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream textStream(&logFile);
        QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
        QString level;
        switch (type) {
            case QtDebugMsg:    level = "DEBUG";    break;
            case QtInfoMsg:     level = "INFO ";    break;
            case QtWarningMsg:  level = "WARN ";    break;
            case QtCriticalMsg: level = "CRIT ";    break;
            case QtFatalMsg:    level = "FATAL";    break;
        }
        textStream << QString("[%1][%2] %3").arg(timeStr, level, msg) << Qt::endl;
        textStream.flush();
        logFile.flush(); // 强制物理落盘
        logFile.close();
    }
}

int main(int argc, char *argv[]) {
    // 2026-xx-xx 按照 Plan-97：启动自检，容量哨兵立即执行
    ArcMeta::Logger::rotateLogFiles("arcmeta_debug.log");

    // 2026-07-xx 按照用户要求 (1.20)：主程序限制单实例运行，防止无限打开
#ifdef Q_OS_WIN
    // Windows: 使用 Mutex 实现，确保程序异常退出后资源能被 OS 自动回收
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "ArcMeta_SingleInstance_Mutex");
    if (hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        // 单实例检测失败，由于此时 QApplication 尚未启动，直接退出
        return 0;
    }
#else
    // 非 Windows (Linux/macOS): 使用 QLockFile 确保单实例运行
    QString lockPath = QDir::tempPath() + "/ArcMeta_SingleInstance.lock";
    static QLockFile lockFile(lockPath); // 必须静态或在 main 作用域持续存在
    if (!lockFile.tryLock(100)) { // 尝试等待 100ms
        return 0;
    }
#endif

    qint64 mainStartTime = QDateTime::currentMSecsSinceEpoch();

    // 初始化 COM 环境 (多媒体缩略图提取需要)
#ifdef Q_OS_WIN
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
#endif

    // 1. 安装自定义日志处理器：确保从程序启动的第一秒开始就能捕获所有调试信息
    qInstallMessageHandler(customMessageHandler);
    qDebug() << "================ ArcMeta 启动加载 ================";
    qDebug() << "[PERF] 程序入口点计时开始";

    // 设置高 DPI 支持：Qt 6 默认行为，此处显式设置 PassThrough 以防旧设备缩放模糊
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication a(argc, argv);

    // ==========================================
    // 【核心修复一：强制锁定工作路径】
    // 强制将当前工作路径（Working Directory）重置为当前可执行文件（.exe）所在位置。
    // 这能彻底杜绝 Windows 强制管理员启动时，工作目录无脑漂移到 C:/Windows/System32 的致命 Bug。
    // ==========================================
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    qDebug() << "[WORK-DIR] 工作目录已强制锁死为:" << QDir::currentPath();

    // 2026-06-xx 按照用户要求：全局统一设置蓝色透明框选样式
    QPalette p = a.palette();
    p.setColor(QPalette::Highlight, QColor(52, 152, 219));      // #3498db (蓝色)
    p.setColor(QPalette::HighlightedText, Qt::white);
    a.setPalette(p);

    a.setQuitOnLastWindowClosed(false);
    
    // 2026-04-14 按照用户要求：物理加固图标加载逻辑
    // 杜绝相对路径幻觉，强制使用 Qt 资源系统 (:/) 加载 app_icon.ico，确保托盘显示不失效
    a.setWindowIcon(QIcon(":/app_icon.ico"));

    a.setApplicationName("ArcMeta");
    a.setOrganizationName("ArcMetaTeam");

    // ==========================================
    // 【核心修复二：主线程预热 MftReader 单例】
    // 强制在主 GUI 线程实例化 MftReader 单例。
    // 确保 MftReader 内部的信号发射机制、事件循环的线程所属权（Affinity）完全绑定在 GUI 主线程。
    // 这样能 100% 确保后台 USN 变动产生的批量信号能够跨线程（QueuedConnection）投递给 AutoImportManager 槽函数，不发生静默丢失。
    // ==========================================
    qint64 metaInitStart = QDateTime::currentMSecsSinceEpoch();
    ArcMeta::MetadataManager::instance();
    ArcMeta::MftReader::instance(); // 强制在主线程初始化并锁定线程所属权
    ArcMeta::CategoryRepo::initialize();
    qDebug() << "[PERF] MetadataManager/MftReader/CategoryRepo 单例预热总耗时:" << (QDateTime::currentMSecsSinceEpoch() - metaInitStart) << "ms";

    // 3. 简化启动：直接显示主窗口
    // 2026-04-13 按用户要求移除 LoadingWindow 和 initializeHotIcons()
    qint64 windowCreateStart = QDateTime::currentMSecsSinceEpoch();
    ArcMeta::MainWindow* w = new ArcMeta::MainWindow();
    qDebug() << "[PERF] MainWindow 构造耗时:" << (QDateTime::currentMSecsSinceEpoch() - windowCreateStart) << "ms";
    
    w->show();
    qDebug() << "[PERF] MainWindow->show() 调用耗时（至首帧渲染前）:" << (QDateTime::currentMSecsSinceEpoch() - windowCreateStart) << "ms";

    // 5. 启动异步系统扫描（后台初始化，UI 可响应）
    ArcMeta::CoreController::instance().startSystem();
    
    // 2026-07-xx 按照 Plan-124：最终诊断，验证此处代码是否被实际执行
    qDebug() << "[DIAG-MAIN] 即将调用 startListening";
    auto& aim = ArcMeta::AutoImportManager::instance();
    qDebug() << "[DIAG-MAIN] AutoImportManager 实例地址:" << &aim;
    aim.startListening();
    qDebug() << "[DIAG-MAIN] startListening 调用已返回";

    qDebug() << "[PERF] main 函数逻辑执行完毕，进入事件循环。总耗时:" << (QDateTime::currentMSecsSinceEpoch() - mainStartTime) << "ms";

    int ret = a.exec();

    // 程序退出前释放单实例锁
#ifdef Q_OS_WIN
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
#endif

    return ret;
}