#pragma once

#include <string>
#include <QString>

namespace FERREX {

class MftReader;

class DiskIndexCacheCoordinator {
public:
    static bool loadFromCache(MftReader* reader);
    static bool loadDriveFromCache(MftReader* reader, const QString& drive);
    static bool saveToCache(MftReader* reader);
    static bool saveDriveToCache(MftReader* reader, size_t driveIdx);
    static bool saveDriveToCacheInternal(MftReader* reader, size_t driveIdx);
    static bool saveDriveToCacheUnlocked(MftReader* reader, size_t driveIdx);
};

} // namespace FERREX
