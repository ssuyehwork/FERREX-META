#include "HoverEventFilter.h"
#include "ToolTipOverlay.h"
#include <QCursor>
#include <QVariant>

namespace ArcMeta {

HoverEventFilter::HoverEventFilter(QObject* parent) : QObject(parent) {}

bool HoverEventFilter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::Enter) {
        QString text = watched->property("tooltipText").toString();
        if (!text.isEmpty()) {
            // 2026-07-xx 按照 Plan-65：悬停触发的提示设置 timeout 为 0（不自动隐藏）
            ToolTipOverlay::instance()->showText(QCursor::pos(), text, 0);
        }
    } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
        ToolTipOverlay::hideTip();
    }
    return QObject::eventFilter(watched, event);
}

} // namespace ArcMeta
