#include "ToolTipOverlay.h"

namespace ArcMeta {

ToolTipOverlay::ToolTipOverlay() : QWidget(nullptr) {
    // [CRITICAL] 彻底弃用 Qt::ToolTip，防止 OS 动画残留
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | 
                  Qt::WindowTransparentForInput | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
    setObjectName("ToolTipOverlay");

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    
    m_doc.setUndoRedoEnabled(false);
    // [ULTIMATE FIX] 强制锁定调色板颜色
    QPalette pal = palette();
    pal.setColor(QPalette::WindowText, QColor("#EEEEEE"));
    pal.setColor(QPalette::Text, QColor("#EEEEEE"));
    pal.setColor(QPalette::ButtonText, QColor("#EEEEEE"));
    setPalette(pal);

    m_doc.setDefaultStyleSheet("body, div, p, span, b, i { color: #EEEEEE !important; font-family: 'Microsoft YaHei', 'Segoe UI'; }"); 
    setStyleSheet("QWidget { color: #EEEEEE !important; background: transparent; }");

    QFont f = font();
    f.setPointSize(9);
    m_doc.setDefaultFont(f);

    m_hideTimer.setSingleShot(true);
    connect(&m_hideTimer, &QTimer::timeout, this, &QWidget::hide);

    // 初始静默隐藏，等待 MainWindow 的 showEvent 触发真正有效的 GPU 预热
    hide();
}

void ToolTipOverlay::showText(const QPoint& globalPos, const QString& text, int timeout, const QColor& borderColor) {
    // [THREAD SAFE] 强制确保在主线程执行
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, [this, globalPos, text, timeout, borderColor]() { 
            showText(globalPos, text, timeout, borderColor); 
        });
        return;
    }

    if (text.isEmpty()) { hide(); return; }

    // 2026-05-20 性能优化：内容脏检查，防止鼠标在按钮内部微动导致的重复渲染卡顿
    if (isVisible() && m_text == text && m_currentBorderColor == borderColor) {
        move(globalPos + QPoint(15, 15));
        return;
    }
    
    if (timeout > 0) {
        timeout = qBound(500, timeout, 60000); 
    }

    m_currentBorderColor = borderColor;

    QString htmlBody;
    if (text.contains("<") && text.contains(">")) {
        htmlBody = text;
    } else {
        htmlBody = text.toHtmlEscaped().replace("\n", "<br>");
    }

    m_text = QString(
        "<html><head><style>div, p, span, body { color: #EEEEEE !important; }</style></head>"
        "<body style='margin:0; padding:0; color:#EEEEEE; font-family:\"Microsoft YaHei\",\"Segoe UI\",sans-serif;'>"
        "<div style='color:#EEEEEE !important;'>%1</div>"
        "</body></html>"
    ).arg(htmlBody);
    
    m_doc.setHtml(m_text);
    m_doc.setDocumentMargin(0); 
    
    m_doc.setTextWidth(-1); 
    qreal idealW = m_doc.idealWidth();
    
    if (idealW > 450) {
        m_doc.setTextWidth(450); 
    } else {
        m_doc.setTextWidth(idealW); 
    }
    
    QSize textSize = m_doc.size().toSize();
    
    int padX = 12; 
    int padY = 8;
    
    int w = textSize.width() + padX * 2;
    int h = textSize.height() + padY * 2;
    
    w = qMax(w, 40);
    h = qMax(h, 24);
    
    resize(w, h);
    
    QPoint pos = globalPos + QPoint(15, 15);
    
    QScreen* screen = QGuiApplication::screenAt(globalPos);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect screenGeom = screen->geometry();
        if (pos.x() + width() > screenGeom.right()) {
            pos.setX(globalPos.x() - width() - 15);
        }
        if (pos.y() + height() > screenGeom.bottom()) {
            pos.setY(globalPos.y() - height() - 15);
        }
    }
    
    move(pos);
    show();
    update();

    if (timeout > 0) {
        m_hideTimer.start(timeout);
    } else {
        m_hideTimer.stop();
    }
}

void ToolTipOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    
    QRectF rectF(0.5, 0.5, width() - 1, height() - 1);
    
    p.setPen(QPen(m_currentBorderColor, 1));
    p.setBrush(QColor("#2B2B2B"));
    // 2026-03-xx 按照用户硬性要求：ToolTip 圆角必须锁定为 2px
    p.drawRoundedRect(rectF, 2, 2);
    
    p.save();
    p.translate(12, 8); 
    m_doc.drawContents(&p);
    p.restore();
}

} // namespace ArcMeta
