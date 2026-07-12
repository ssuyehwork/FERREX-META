#pragma once

#include <QObject>
#include <QEvent>

namespace FERREX {

class ScanDialog;

/**
 * @brief 边缘缩放事件过滤器 (完美复刻自 ArcMeta 工业级事件层)
 * 通过全局安装拦截器，解决子控件遮挡导致无法更新双向光标的问题
 */
class ResizeEventFilter : public QObject {
    Q_OBJECT

public:
    explicit ResizeEventFilter(ScanDialog* window);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ScanDialog* m_window;
};

} // namespace FERREX
