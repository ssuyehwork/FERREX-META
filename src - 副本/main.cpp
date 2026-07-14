#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <QApplication>
#include <QMessageBox>
#include <QDebug>
#include <QSharedMemory>
#include <QMutex>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QStandardPaths>
#include <QDir>
#include <QSvgRenderer>
#include <QPainter>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include "ui/UiHelper.h"
#include "ui/ScanDialog.h"
#include "ui/TrayController.h"
#include "meta/MetadataManager.h"
#include "mft/MftReader.h"
#include "core/CoreController.h"

static qint64 g_currentLogSize = -1; 
static QMutex g_logMutex;            

void customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    Q_UNUSED(context); 
    QMutexLocker locker(&g_logMutex);

    static QString logPath;
    if (logPath.isEmpty()) {
        
        logPath = QDir(QCoreApplication::applicationDirPath()).filePath("FERREX_debug.log");
    }

    const QString logFileName = logPath;
    const qint64 maxLogSize = 10 * 1024 * 1024; 
    const int maxHistoryFiles = 3;           

    if (g_currentLogSize < 0) {
        QFile f(logFileName);
        g_currentLogSize = f.exists() ? f.size() : 0;
    }

    QFile logFile(logFileName);
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
        QString line = QString("[%1][%2] %3\n").arg(timeStr, level, msg);
        textStream << line;
        textStream.flush();

        fprintf(stderr, "%s", line.toLocal8Bit().constData());
        fflush(stderr);

        g_currentLogSize += line.toUtf8().size(); 
        logFile.close();

        if (g_currentLogSize > maxLogSize) {
            
            for (int i = maxHistoryFiles; i >= 1; --i) {
                QString oldName = logFileName + QString(".%1").arg(i);
                QString newName = logFileName + QString(".%1").arg(i + 1);
                if (i == maxHistoryFiles) {
                    QFile::remove(oldName);
                } else {
                    if (QFile::exists(oldName)) QFile::rename(oldName, newName);
                }
            }
            
            if (QFile::rename(logFileName, logFileName + ".1")) {
                g_currentLogSize = 0;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    qInstallMessageHandler(customMessageHandler);
    qDebug() << "================ FERREX 启动加载 ================";

    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication a(argc, argv);

    QSharedMemory sharedMem("FERREX_SINGLE_INSTANCE_SHARED_MEMORY_KEY_UNIQUE_2026");
    if (sharedMem.attach()) {
        qWarning() << "[SINGLE_INSTANCE] 检测到已有实例正在运行，当前进程准备安全退出。";
        return 0; 
    }
    
    if (!sharedMem.create(1)) {
        qWarning() << "[SINGLE_INSTANCE] 共享内存块创建失败，错误代码:" << sharedMem.errorString();
        
        if (sharedMem.error() == QSharedMemory::AlreadyExists) {
            qWarning() << "[SINGLE_INSTANCE] 共享内存已存在。重复运行，当前进程准备安全退出。";
            return 0;
        }
    }

    a.setQuitOnLastWindowClosed(false);

    
    a.setWindowIcon(QIcon(":/app_icon.ico"));

    a.setApplicationName("FERREX");
    a.setOrganizationName("FERREXTeam");

    FERREX::MetadataManager::instance();

    
    FERREX::ScanDialog* w = new FERREX::ScanDialog();
    w->show();

    FERREX::TrayController* tray = new FERREX::TrayController(w);
    tray->show();

    FERREX::CoreController::instance().startSystem();

    int ret = a.exec();

    FERREX::MftReader::instance().clear();
    CoUninitialize();

    return ret;
}
