#include "ResizeEventFilter.h"
#include "ScanDialog.h"
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>
#include <QScreen>

namespace FERREX {

ResizeEventFilter::ResizeEventFilter(ScanDialog* window) 
    : QObject(window), m_window(window) {}

ResizeEventFilter::ResizeDirection ResizeEventFilter::getResizeDirection(const QPoint& pos) const {
    int m = kResizeMargin;
    if (m_window->windowHandle()) {
        m = qRound(m_window->windowHandle()->screen()->logicalDotsPerInch() / 96.0 * (double)kResizeMargin);
    }
    const int w = m_window->width(), h = m_window->height();
    bool left   = pos.x() < m;
    bool right  = pos.x() > w - m;
    bool top    = pos.y() < m;
    bool bottom = pos.y() > h - m;

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

bool ResizeEventFilter::eventFilter(QObject* watched, QEvent* event) {
    // 仅拦截和过滤 ScanDialog 窗口自身的事件，对子控件（如输入框、按钮等）完全放行，保障标题栏控件交互正常
    if (watched != m_window) {
        return QObject::eventFilter(watched, event);
    }

    if (m_window->isMaximized()) {
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            QPoint localPos = m_window->mapFromGlobal(me->globalPosition().toPoint());
            ResizeDirection dir = getResizeDirection(localPos);

            if (dir != None) {
                m_isResizing = true;
                m_isDragging = false;
                m_resizeDir = dir;
                m_resizeStartGlobal   = me->globalPosition().toPoint();
                m_resizeStartGeometry = m_window->geometry();
                me->accept();
                return true;
            }

            if (localPos.y() <= 34) {
                m_isDragging = true;
                m_dragPosition = me->globalPosition().toPoint() - m_window->frameGeometry().topLeft();
                me->accept();
                return true;
            }
        }
    }
    else if (event->type() == QEvent::MouseMove) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        
        if (m_isResizing) {
            const QPoint delta = me->globalPosition().toPoint() - m_resizeStartGlobal;
            QRect r = m_resizeStartGeometry;

            if (m_resizeDir == Left || m_resizeDir == TopLeft || m_resizeDir == BottomLeft)
                r.setLeft(r.left() + delta.x());
            if (m_resizeDir == Right || m_resizeDir == TopRight || m_resizeDir == BottomRight)
                r.setRight(r.right() + delta.x());
            if (m_resizeDir == Top || m_resizeDir == TopLeft || m_resizeDir == TopRight)
                r.setTop(r.top() + delta.y());
            if (m_resizeDir == Bottom || m_resizeDir == BottomLeft || m_resizeDir == BottomRight)
                r.setBottom(r.bottom() + delta.y());

            if (r.width() >= m_window->minimumWidth() && r.height() >= m_window->minimumHeight()) {
                m_window->setGeometry(r);
            }
            me->accept();
            return true;
        }

        if (m_isDragging && (me->buttons() & Qt::LeftButton)) {
            m_window->move(me->globalPosition().toPoint() - m_dragPosition);
            me->accept();
            return true;
        }

        if (!m_isDragging) {
            QPoint localPos = m_window->mapFromGlobal(me->globalPosition().toPoint());
            updateCursorShape(getResizeDirection(localPos));
        }
    }
    else if (event->type() == QEvent::MouseButtonRelease) {
        m_isDragging  = false;
        m_isResizing  = false;
        m_resizeDir   = None;
        m_window->setCursor(Qt::ArrowCursor);
    }
    else if (event->type() == QEvent::Leave && watched == m_window) {
        m_window->setCursor(Qt::ArrowCursor);
    }

    return QObject::eventFilter(watched, event);
}

} // namespace FERREX
