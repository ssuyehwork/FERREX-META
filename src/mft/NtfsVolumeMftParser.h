#pragma once

#include <string>
#include "MftReader.h"

namespace FERREX {

class NtfsVolumeMftParser {
public:
    static bool loadMftDirect(const std::wstring& volume, MftReader::DriveResult& result);
};

} // namespace FERREX
