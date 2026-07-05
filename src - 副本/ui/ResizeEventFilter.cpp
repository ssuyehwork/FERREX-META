#include "ResizeEventFilter.h"
#include <QMouseEvent>
#include <QScreen>
#include <QWindow>
#include <QApplication>

namespace ArcMeta {

ResizeEventFilter::ResizeEventFilter(QMainWindow* window) 
    : QObject(window), m_window(window) {}

bool ResizeEventFilter::eventFilter(QObject* watched, QEvent* event) {
    if (m_window->isMaximized()) return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::MouseMove) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        // 如果正在调整大小，不在此处处理光标，交由 MainWindow 自己的逻辑或继续透传
        // 但根据需求，这里只负责“光标更新”
        QPoint localPos = m_window->mapFromGlobal(me->globalPosition().toPoint());
        ResizeDirection dir = getResizeDirection(localPos);
        updateCursorShape(dir);
    } else if (event->type() == QEvent::Leave && watched == m_window) {
        m_window->setCursor(Qt::ArrowCursor);
    }
    return QObject::eventFilter(watched, event);
}

ResizeEventFilter::ResizeDirection ResizeEventFilter::getResizeDirection(const QPoint& pos) const {
    // 2026-05-08 按照用户要求：根据 DPI 动态计算感应宽度
    int margin = 6;
    if (m_window->windowHandle()) {
        margin = qRound(m_window->screen()->logicalDotsPerInch() / 96.0 * 6.0);
    }
    
    const int w = m_window->width(), h = m_window->height();
    bool left   = pos.x() < margin;
    bool right  = pos.x() > w - margin;
    bool top    = pos.y() < margin;
    bool bottom = pos.y() > h - margin;

    if (top    && left)  return TopLeft;
    if (top    && right) return TopRight;
    if (bottom && left)  return BottomLeft;
    if (bottom && right) return BottomRight;
    if (left)   return Left;
    if (right)  return Right;
    if (top)    return Top;
    if (bottom) return Bottom;
    return None;
}

void ResizeEventFilter::updateCursorShape(ResizeDirection dir) {
    switch (dir) {
        case Left:        case Right:       m_window->setCursor(Qt::SizeHorCursor);  break;
        case Top:         case Bottom:      m_window->setCursor(Qt::SizeVerCursor);  break;
        case TopLeft:     case BottomRight: m_window->setCursor(Qt::SizeFDiagCursor); break;
        case TopRight:    case BottomLeft:  m_window->setCursor(Qt::SizeBDiagCursor); break;
        default:                            m_window->setCursor(Qt::ArrowCursor);    break;
    }
}

} // namespace ArcMeta
