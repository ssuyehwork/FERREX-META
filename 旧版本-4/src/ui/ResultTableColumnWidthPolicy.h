#pragma once

#include <QTableView>

namespace FERREX {

class ResultTableColumnWidthPolicy {
public:
    static int calculateNameColumnWidthLimit(QTableView* tableView);
};

} // namespace FERREX
