#include "ResizeEventFilter.h"
#include "ScanDialog.h"
#include <QMouseEvent>

namespace FERREX {

ResizeEventFilter::ResizeEventFilter(ScanDialog* window) 
    : QObject(window), m_window(window) {}

bool ResizeEventFilter::eventFilter(QObject* watched, QEvent* event) {
    // 1. 若主对话框已被最大化，则退化关闭边缘热区判定
    if (m_window->isMaximized()) {
        return QObject::eventFilter(watched, event);
    }

    // 2. 捕捉未按下鼠标时的普通悬停移动 (QEvent::MouseMove)
    if (event->type() == QEvent::MouseMove) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        
        // 3. 将全局坐标映射回 ScanDialog 的局部坐标系中
        QPoint localPos = m_window->mapFromGlobal(me->globalPosition().toPoint());
        
        // 4. 计算边缘拉伸方向，并直接调用主窗体方法更新光标
        ScanDialog::ResizeDirection dir = m_window->getResizeDirection(localPos);
        m_window->updateCursorShape(dir);
    } 
    // 5. 捕捉鼠标离开主窗口的事件，强制复位光标
    else if (event->type() == QEvent::Leave && watched == m_window) {
        m_window->setCursor(Qt::ArrowCursor);
    }

    return QObject::eventFilter(watched, event);
}

} // namespace FERREX
