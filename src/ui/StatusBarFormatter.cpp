#include "StatusBarFormatter.h"
#include <QLocale>
#include <QStringList>

namespace FERREX {

QString StatusBarFormatter::formatNumber(int64_t n) {
    return QLocale(QLocale::English).toString(n);
}

QString StatusBarFormatter::formatSize(int64_t bytes) {
    if (bytes == 0) return "0 B";
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < units.size() - 1) {
        size /= 1024.0;
        unit++;
    }
    return QString("%1 %2").arg(size, 0, 'f', 2).arg(units[unit]);
}

} // namespace FERREX
