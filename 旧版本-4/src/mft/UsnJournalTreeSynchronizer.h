#pragma once

#include <string>
#include <windows.h>
#include <winioctl.h>
#include "MftReader.h"

namespace FERREX {

class UsnJournalTreeSynchronizer {
public:
    static void updateEntryFromUsn(MftReader* reader, USN_RECORD_V2* record, const std::wstring& volume);
    static void removeEntryByFrn(MftReader* reader, const std::wstring& volume, uint64_t frn);
};

} // namespace FERREX
