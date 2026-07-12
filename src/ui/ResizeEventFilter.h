#pragma once

#include <QObject>
#include <QEvent>
#include <QPoint>
#include <QRect>

namespace FERREX {

class ScanDialog;

/**
 * @brief 边缘缩放与拖拽事件过滤器 (完整接管无边框窗口的所有鼠标行为)
 * 完美托管，解决子控件遮挡导致的光标状态机异常，彻底使 ScanDialog 减负
 */
class ResizeEventFilter : public QObject {
    Q_OBJECT

public:
    explicit ResizeEventFilter(ScanDialog* window);

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
