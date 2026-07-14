#include "ResultTableColumnWidthPolicy.h"
#include <QHeaderView>
#include <QScrollBar>

namespace FERREX {

int ResultTableColumnWidthPolicy::calculateNameColumnWidthLimit(QTableView* tableView) {
    if (!tableView) return 260;

    int viewportWidth = tableView->viewport()->width();
    if (viewportWidth <= 0) {
        viewportWidth = tableView->width();
    }

    if (viewportWidth <= 0) return 260;

    auto* header = tableView->horizontalHeader();
    if (!header) return 260;

    int reservedWidth = 0;
    
    for (int i = 1; i < header->count(); ++i) {
        if (header->sectionResizeMode(i) == QHeaderView::Stretch) {

            reservedWidth += header->minimumSectionSize();
        } else {

            reservedWidth += tableView->columnWidth(i);
        }
    }

    if (tableView->verticalScrollBar() && tableView->verticalScrollBar()->isVisible()) {
        reservedWidth += tableView->verticalScrollBar()->width();
    }

    int maxAllowedWidth = viewportWidth - reservedWidth;
    if (maxAllowedWidth < 200) {
        maxAllowedWidth = 200; 
    }

    return maxAllowedWidth;
}

} 
