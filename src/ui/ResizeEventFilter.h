#pragma once

#include <QObject>
#include <QEvent>
#include <QMainWindow>

namespace ArcMeta {

/**
 * @brief 边缘缩放事件过滤器
 * 专门处理无边框窗口的边缘缩放光标更新与感应逻辑
 */
class ResizeEventFilter : public QObject {
    Q_OBJECT

public:
    explicit ResizeEventFilter(QMainWindow* window);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum ResizeDirection {
        None = 0,
        Left, Right, Top, Bottom,
        TopLeft, TopRight, BottomLeft, BottomRight
    };

    QMainWindow* m_window;
    
    ResizeDirection getResizeDirection(const QPoint& pos) const;
    void updateCursorShape(ResizeDirection dir);
};

} // namespace ArcMeta
