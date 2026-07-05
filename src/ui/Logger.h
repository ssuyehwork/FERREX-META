#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>
#include <QTextStream>
#include <QDateTime>

namespace ArcMeta {

/**
 * @brief 独立日志工具类，绕过 qDebug 直接写入本地文件
 * 2026-04-02 按照用户铁律：严禁使用 qDebug()，必须直接操作文件输出。
 */
class Logger {
public:
    static void log(const QString& msg) {
        QFile file("arcmeta_debug.log"); // 统一写入主日志文件
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&file);
            QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
            out << QString("[%1][DEBUG] %2").arg(timeStr, msg) << Qt::endl;
            file.flush();
            file.close();
        }
    }
};

} // namespace ArcMeta

#endif // LOGGER_H
