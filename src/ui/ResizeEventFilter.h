#pragma once

#include <QObject>
#include <QEvent>

namespace FERREX {

class ScanDialog;

/**
 * @brief 边缘缩放与拖拽事件过滤器
 * 通过全局安装拦截器，完全接管 ScanDialog 的窗口无边框行为，减轻 ScanDialog 自身的代码负担
 */
class ResizeEventFilter : public QObject {
    Q_OBJECT

public:
    explicit ResizeEventFilter(ScanDialog* window);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ScanDialog* m_window;
    bool m_isResizing = false;
    bool m_isDragging = false;
    int m_resizeDir = 0; // 0=None
    QPoint m_resizeStartGlobal;
    QRect m_resizeStartGeometry;
    QPoint m_dragPosition;
};

} // namespace FERREX
