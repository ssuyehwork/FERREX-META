#ifndef TOOLTIPOVERLAY_H
#define TOOLTIPOVERLAY_H

#include <QWidget>
#include <QPainter>
#include <QElapsedTimer>
#include <QDebug>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QFontMetrics>
#include <QTextDocument>
#include <QPointer>
#include <QPainterPath>
#include <QColor>
#include <QPen>
#include <QBrush>
#include <QRectF>

namespace ArcMeta {

/**
 * @brief ToolTipOverlay: 全局统一的自定义 Tooltip
 * [CRITICAL] 本项目严禁使用任何形式的“Windows 系统默认 Tip 样式”！
 * [RULE] 1. 杜绝原生内容带来的系统阴影和不透明度。
 * [RULE] 2. 所有的 ToolTip 逻辑必须通过此 ToolTipOverlay 渲染。
 * [RULE] 3. 此组件必须保持扁平化 (Flat)，严禁添加任何阴影特效。
 */
class ToolTipOverlay : public QWidget {
    Q_OBJECT
public:
    static ToolTipOverlay* instance() {
        static QPointer<ToolTipOverlay> inst;
        if (!inst) {
            inst = new ToolTipOverlay();
        }
        return inst;
    }

    /**
     * @brief 显示提示文字（2026-03-xx 重构升级版）
     */
    void showText(const QPoint& globalPos, const QString& text, int timeout = 700, const QColor& borderColor = QColor("#B0B0B0"));

    // 兼容旧接口
    void showTip(const QString& text, const QPoint& pos, int timeout = 700) {
        showText(pos, text, timeout);
    }

    static void hideTip() {
        if (instance()) instance()->hide();
    }

protected:
    explicit ToolTipOverlay();
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_text;
    QTextDocument m_doc;
    QTimer m_hideTimer;
    QColor m_currentBorderColor = QColor("#B0B0B0");
};

} // namespace ArcMeta

#endif // TOOLTIPOVERLAY_H
