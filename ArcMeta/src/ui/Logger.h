#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <QFileInfo>
#include <QDir>

namespace ArcMeta {

/**
 * @brief 独立日志工具类，绕过 qDebug 直接写入本地文件
 * 2026-04-02 按照用户铁律：严禁使用 qDebug()，必须直接操作文件输出。
 */
class Logger {
public:
    /**
     * @brief 物理容量哨兵：单文件 4MB 滚动机制
     * 2026-xx-xx 按照 Plan-97 实现双文件滚动治理
     */
    static void rotateLogFiles(const QString& fileName) {
        QFileInfo info(fileName);
        if (info.exists() && info.size() > 4 * 1024 * 1024) { // 4MB 阈值
            QString oldFile = fileName + ".old";
            QFile::remove(oldFile);
            if (!QFile::rename(fileName, oldFile)) {
                // 若被占用导致重命名失败，则物理清空
                QFile file(fileName);
                (void)file.open(QIODevice::WriteOnly | QIODevice::Truncate);
                file.close();
            }
        }
    }

    static void log(const QString& msg) {
        static QMutex mutex;
        QMutexLocker locker(&mutex);
        
        static int writeCount = 0;
        QString fileName = "arcmeta_debug.log";

        // 每 100 次写入检测一次容量，降低系统调用开销
        if (++writeCount >= 100) {
            rotateLogFiles(fileName);
            writeCount = 0;
        }

        QFile file(fileName); // 统一写入主日志文件
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&file);
            QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
            out << QString("[%1][DEBUG] %2").arg(timeStr, msg) << Qt::endl;
            out.flush();
            file.flush();
            file.close();
        }
    }
};

} // namespace ArcMeta

#endif // LOGGER_H
