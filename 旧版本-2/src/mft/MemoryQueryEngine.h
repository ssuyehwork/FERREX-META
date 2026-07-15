#pragma once

#include <QString>
#include <QStringList>
#include <vector>
#include "MftReader.h"

namespace FERREX {

class MemoryQueryEngine {
public:
    static std::vector<uint64_t> search(MftReader* reader, const QString& query, bool useRegex, bool caseSensitive, 
                                        const QStringList& extensionList, bool includeHidden, bool includeSystem,
                                        bool includeDollar);
};

} // namespace FERREX
