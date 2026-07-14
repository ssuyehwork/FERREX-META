#ifndef TOOLTIPOVERLAY_H
#define TOOLTIPOVERLAY_H

#include <QWidget>
#include <QPainter>
#include <QElapsedTimer>
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

namespace FERREX {

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

    void showText(const QPoint& globalPos, const QString& text, int timeout = 700, const QColor& borderColor = QColor("#B0B0B0"));

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

} 

#endif 
