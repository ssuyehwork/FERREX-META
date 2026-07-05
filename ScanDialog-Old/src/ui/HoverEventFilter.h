#pragma once

#include <QObject>
#include <QEvent>

namespace ArcMeta {

/**
 * @brief 悬停事件过滤器
 * 专门处理鼠标进入/离开控件时，显示/隐藏自定义 ToolTipOverlay
 */
class HoverEventFilter : public QObject {
    Q_OBJECT

public:
    explicit HoverEventFilter(QObject* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace ArcMeta
