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
    // 遍历所有非第 0 列的其它列，将它们的宽度进行扣减累加
    for (int i = 1; i < header->count(); ++i) {
        if (header->sectionResizeMode(i) == QHeaderView::Stretch) {
            // 被 Stretch 的弹性列：
            // 根据表头 minimumSectionSize() 的配置来动态查询其作为弹性的最低极限缓冲物理下限，杜绝任何瞎猜魔法硬编码。
            reservedWidth += header->minimumSectionSize();
        } else {
            // 其他可交互列：
            // 实时测量测量它们被用户拉伸/默认配置下的当前 columnWidth()
            reservedWidth += tableView->columnWidth(i);
        }
    }

    // 考虑垂直滚动条的宽度占位差（如果垂直滚动条是可见的，占去它的宽度）
    if (tableView->verticalScrollBar() && tableView->verticalScrollBar()->isVisible()) {
        reservedWidth += tableView->verticalScrollBar()->width();
    }

    int maxAllowedWidth = viewportWidth - reservedWidth;
    if (maxAllowedWidth < 200) {
        maxAllowedWidth = 200; // 维持名称列最低 200 像素的基本显示
    }

    return maxAllowedWidth;
}

} // namespace FERREX
