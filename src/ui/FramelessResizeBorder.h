#pragma once

#include <QObject>
#include <QEvent>
#include <QPoint>
#include <QRect>

namespace FERREX {

class ScanDialog;

class FramelessResizeBorder : public QObject {
    Q_OBJECT

public:
    explicit FramelessResizeBorder(ScanDialog* window);

    enum ResizeDirection {
        None = 0,
        Left, Right, Top, Bottom,
        TopLeft, TopRight, BottomLeft, BottomRight
    };

    ResizeDirection getResizeDirection(const QPoint& localPos) const;
    void updateCursorShape(ResizeDirection dir);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ScanDialog* m_window;

    ResizeDirection m_resizeDir = None;
    bool m_isResizing = false;
    bool m_isDragging = false;             
    QPoint m_resizeStartGlobal;
    QRect  m_resizeStartGeometry;
    QPoint m_dragPosition;                 

    static constexpr int kResizeMargin = 6; 
};

} // namespace FERREX
