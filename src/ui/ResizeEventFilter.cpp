#include "ResizeEventFilter.h"
#include "ScanDialog.h"
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>
#include <QScreen>

namespace FERREX {

ResizeEventFilter::ResizeEventFilter(ScanDialog* window) 
    : QObject(window), m_window(window) {}

bool ResizeEventFilter::eventFilter(QObject* watched, QEvent* event) {
    QWidget* watchedWidget = qobject_cast<QWidget*>(watched);
    if (!watchedWidget || (watchedWidget != m_window && !m_window->isAncestorOf(watchedWidget))) {
        return QObject::eventFilter(watched, event);
    }

    if (m_window->isMaximized()) {
        return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseMove) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        
        if (m_isResizing) {
            const QPoint delta = me->globalPosition().toPoint() - m_resizeStartGlobal;
            QRect r = m_resizeStartGeometry;
            int dir = m_resizeDir;

            if (dir == ScanDialog::Left || dir == ScanDialog::TopLeft || dir == ScanDialog::BottomLeft)
                r.setLeft(r.left() + delta.x());
            if (dir == ScanDialog::Right || dir == ScanDialog::TopRight || dir == ScanDialog::BottomRight)
                r.setRight(r.right() + delta.x());
            if (dir == ScanDialog::Top || dir == ScanDialog::TopLeft || dir == ScanDialog::TopRight)
                r.setTop(r.top() + delta.y());
            if (dir == ScanDialog::Bottom || dir == ScanDialog::BottomLeft || dir == ScanDialog::BottomRight)
                r.setBottom(r.bottom() + delta.y());

            if (r.width() >= m_window->minimumWidth() && r.height() >= m_window->minimumHeight()) {
                m_window->setGeometry(r);
            }
            return true;
        }

        if (m_isDragging && (me->buttons() & Qt::LeftButton)) {
            m_window->move(me->globalPosition().toPoint() - m_dragPosition);
            return true;
        }

        QPoint localPos = m_window->mapFromGlobal(me->globalPosition().toPoint());
        ScanDialog::ResizeDirection dir = m_window->getResizeDirection(localPos);
        m_window->updateCursorShape(dir);
    } 
    else if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            QPoint localPos = m_window->mapFromGlobal(me->globalPosition().toPoint());
            ScanDialog::ResizeDirection dir = m_window->getResizeDirection(localPos);

            if (dir != ScanDialog::None) {
                m_isResizing = true;
                m_isDragging = false;
                m_resizeDir = static_cast<int>(dir);
                m_resizeStartGlobal = me->globalPosition().toPoint();
                m_resizeStartGeometry = m_window->geometry();
                return true;
            }

            if (localPos.y() <= 34) {
                m_isDragging = true;
                m_dragPosition = me->globalPosition().toPoint() - m_window->frameGeometry().topLeft();
                return true;
            }
        }
    }
    else if (event->type() == QEvent::MouseButtonRelease) {
        m_isDragging = false;
        m_isResizing = false;
        m_resizeDir = 0;
        m_window->setCursor(Qt::ArrowCursor);
    }
    else if (event->type() == QEvent::Leave && watched == m_window) {
        m_window->setCursor(Qt::ArrowCursor);
    }

    return QObject::eventFilter(watched, event);
}

} // namespace FERREX
