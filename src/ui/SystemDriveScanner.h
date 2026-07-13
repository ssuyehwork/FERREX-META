#pragma once

#include <QString>
#include <QVector>

namespace FERREX {

struct DriveInfo {
    QString letter;
    QString label;
    bool isNtfs;
    bool hasMedia;
};

class SystemDriveScanner {
public:
    static QVector<DriveInfo> scanDrives();
};

} // namespace FERREX
