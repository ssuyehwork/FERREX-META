#pragma once

#include <QString>

namespace FERREX {

class StatusBarFormatter {
public:
    static QString formatNumber(int64_t n);
    static QString formatSize(int64_t bytes);
};

} // namespace FERREX
