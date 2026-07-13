#include "HoverEventFilter.h"
#include "ToolTipOverlay.h"
#include <QCursor>
#include <QVariant>

namespace FERREX {

HoverEventFilter::HoverEventFilter(QObject* parent) : QObject(parent) {}

bool HoverEventFilter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::Enter) {
        QString text = watched->property("tooltipText").toString();
        if (!text.isEmpty()) {
            // 悬停触发的提示设置 timeout 为 0（代表不自动隐藏，伴随鼠标移动并由 Leave 事件销毁）
            ToolTipOverlay::instance()->showText(QCursor::pos(), text, 0);
        }
    } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
        ToolTipOverlay::hideTip();
    }
    return QObject::eventFilter(watched, event);
}

} // namespace FERREX
