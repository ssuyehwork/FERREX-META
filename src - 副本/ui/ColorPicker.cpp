#include "ColorPicker.h"
#include <QPainter>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include "UiHelper.h"

namespace ArcMeta {

// --- SvPicker ---
SvPicker::SvPicker(QWidget* parent) : QWidget(parent) {
    setFixedSize(160, 160);
    setCursor(Qt::CrossCursor);
}

void SvPicker::setHue(int h) {
    if (m_h == h) return;
    m_h = h;
    m_dirty = true;
    update();
}

void SvPicker::setSv(int s, int v) {
    if (m_s == s && m_v == v) return;
    m_s = s;
    m_v = v;
    update();
}

void SvPicker::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (m_dirty || m_bgCache.isNull() || m_bgCache.size() != size()) {
        m_bgCache = QImage(width(), height(), QImage::Format_ARGB32_Premultiplied);
        QPainter cachePainter(&m_bgCache);
        
        QColor hueColor = QColor::fromHsv(m_h, 255, 255);
        QLinearGradient bgGrad(0, 0, width(), 0);
        bgGrad.setColorAt(0, Qt::white);
        bgGrad.setColorAt(1, hueColor);
        cachePainter.fillRect(rect(), bgGrad);

        QLinearGradient fgGrad(0, 0, 0, height());
        fgGrad.setColorAt(0, QColor(0, 0, 0, 0));
        fgGrad.setColorAt(1, QColor(0, 0, 0, 255));
        cachePainter.fillRect(rect(), fgGrad);
        
        m_dirty = false;
    }

    painter.drawImage(0, 0, m_bgCache);

    // 绘制准星
    int cx = (m_s * width()) / 255;
    int cy = height() - (m_v * height()) / 255;
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawEllipse(QPoint(cx, cy), 4, 4);
    painter.setPen(QPen(Qt::black, 1));
    painter.drawEllipse(QPoint(cx, cy), 5, 5);
}

void SvPicker::updateFromPos(const QPoint& pos) {
    int x = qBound(0, pos.x(), width());
    int y = qBound(0, pos.y(), height());
    m_s = (x * 255) / width();
    m_v = 255 - (y * 255) / height();
    update();
    emit svChanged(m_s, m_v);
}

void SvPicker::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) updateFromPos(event->pos());
}

void SvPicker::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) updateFromPos(event->pos());
}

// --- HueSlider ---
HueSlider::HueSlider(QWidget* parent) : QWidget(parent) {
    setFixedSize(16, 160);
    setCursor(Qt::PointingHandCursor);
}

void HueSlider::setHue(int h) {
    if (m_h == h) return;
    m_h = h;
    update();
}

void HueSlider::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QLinearGradient grad(0, 0, 0, height());
    grad.setColorAt(0.0/6.0, QColor::fromHsv(0, 255, 255));
    grad.setColorAt(1.0/6.0, QColor::fromHsv(60, 255, 255));
    grad.setColorAt(2.0/6.0, QColor::fromHsv(120, 255, 255));
    grad.setColorAt(3.0/6.0, QColor::fromHsv(180, 255, 255));
    grad.setColorAt(4.0/6.0, QColor::fromHsv(240, 255, 255));
    grad.setColorAt(5.0/6.0, QColor::fromHsv(300, 255, 255));
    grad.setColorAt(6.0/6.0, QColor::fromHsv(359, 255, 255));

    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawRoundedRect(rect(), 4, 4);

    int cy = (m_h * height()) / 359;
    painter.setPen(QPen(Qt::white, 2));
    painter.drawLine(0, cy, width(), cy);
    painter.setPen(QPen(Qt::black, 1));
    painter.drawLine(0, cy - 1, width(), cy - 1);
    painter.drawLine(0, cy + 1, width(), cy + 1);
}

void HueSlider::updateFromPos(int y) {
    y = qBound(0, y, height());
    m_h = (y * 359) / height();
    update();
    emit hueChanged(m_h);
}

void HueSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) updateFromPos(event->pos().y());
}

void HueSlider::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) updateFromPos(event->pos().y());
}

// --- ColorPicker ---
ColorPicker::ColorPicker(QWidget* parent) : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_DeleteOnClose);
    setFixedSize(220, 320); // 2026-05-17 按照用户要求：高度由 280 扩大到 320，容纳准确度滑条行
    setStyleSheet("ColorPicker { background: #181818; border: 1px solid #333333; border-radius: 6px; }");

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // 1. 顶部 SV与H选择区
    QHBoxLayout* topLayout = new QHBoxLayout();
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(10);
    
    m_svPicker = new SvPicker(this);
    m_hueSlider = new HueSlider(this);
    topLayout->addWidget(m_svPicker);
    topLayout->addWidget(m_hueSlider);
    mainLayout->addLayout(topLayout);

    connect(m_hueSlider, &HueSlider::hueChanged, this, [this](int h) {
        m_h = h;
        m_svPicker->setHue(h);
        updateColorFromHsv();
    });
    connect(m_svPicker, &SvPicker::svChanged, this, [this](int s, int v) {
        m_s = s; m_v = v;
        updateColorFromHsv();
    });

    // 2. 预设色块区
    QGridLayout* gridLayout = new QGridLayout();
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(6);
    
    QStringList presets = {
        "#FFFFFF", "#808080", "#000000", "#EAEAEA", "#A0A0A0", "#8B4513", "#FF69B4",
        "#E24B4A", "#EF9F27", "#FECF0E", "#639922", "#1D9E75", "#378ADD", "#7F77DD"
    };
    
    int row = 0, col = 0;
    for (const QString& hex : presets) {
        QPushButton* btn = new QPushButton(this);
        btn->setFixedSize(22, 22);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QString(
            "QPushButton { background: %1; border: 1px solid #444; border-radius: 4px; }"
            "QPushButton:hover { border: 1px solid #AAA; }"
        ).arg(hex));
        connect(btn, &QPushButton::clicked, this, [this, hex]() {
            setCurrentColor(QColor(hex));
            // 2026-05-17 选择预设后携带当前容差值发射信号并关闭
            emit colorSelected(m_color, m_toleranceSlider ? m_toleranceSlider->value() : 30);
            close();
        });
        gridLayout->addWidget(btn, row, col);
        col++;
        if (col >= 7) { col = 0; row++; }
    }
    mainLayout->addLayout(gridLayout);

    // 3. 底部预览及十六进制输入区
    QHBoxLayout* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(4, 4, 4, 4);
    bottomLayout->setSpacing(8);
    
    QWidget* bottomContainer = new QWidget(this);
    bottomContainer->setStyleSheet("QWidget { background: #252526; border: 1px solid #378ADD; border-radius: 4px; }");
    bottomContainer->setLayout(bottomLayout);
    mainLayout->addWidget(bottomContainer);

    m_previewBlock = new QWidget(bottomContainer);
    m_previewBlock->setFixedSize(16, 16);
    m_previewBlock->setStyleSheet("background: red; border-radius: 4px;");
    bottomLayout->addWidget(m_previewBlock);

    m_hexEdit = new QLineEdit(bottomContainer);
    m_hexEdit->setStyleSheet("QLineEdit { background: transparent; border: none; color: #EEEEEE; font-family: monospace; font-size: 12px; }");
    m_hexEdit->setText("#FF0000");
    connect(m_hexEdit, &QLineEdit::returnPressed, this, [this]() {
        QColor c(m_hexEdit->text());
        if (c.isValid()) {
            setCurrentColor(c);
            // 2026-05-17 按照用户要求：携带容差值发射
            emit colorSelected(m_color, m_toleranceSlider ? m_toleranceSlider->value() : 30);
            close();
        }
    });
    bottomLayout->addWidget(m_hexEdit);

    QPushButton* btnConfirm = new QPushButton(bottomContainer);
    btnConfirm->setFixedSize(20, 20);
    btnConfirm->setFlat(true);
    btnConfirm->setCursor(Qt::PointingHandCursor);
    btnConfirm->setIcon(UiHelper::getIcon("color_wheel", QColor("#EEEEEE"), 16));
    btnConfirm->setStyleSheet("QPushButton { border: none; background: transparent; } QPushButton:hover { background: rgba(255,255,255,0.1); border-radius: 2px; }");
    connect(btnConfirm, &QPushButton::clicked, this, [this]() {
        // 2026-05-17 按照用户要求：携带容差值发射
        emit colorSelected(m_color, m_toleranceSlider ? m_toleranceSlider->value() : 30);
        close();
    });
    bottomLayout->addWidget(btnConfirm);

    // 4. 2026-05-17 按照用户要求：准确度控制行（容差滑条）
    QWidget* toleranceRow = new QWidget(this);
    toleranceRow->setStyleSheet("QWidget { background: transparent; }");
    QHBoxLayout* toleranceLayout = new QHBoxLayout(toleranceRow);
    toleranceLayout->setContentsMargins(0, 0, 0, 0);
    toleranceLayout->setSpacing(8);

    QLabel* toleranceLabel = new QLabel("\u51c6\u786e\u5ea6:", toleranceRow); // "准确度:"
    toleranceLabel->setStyleSheet("QLabel { color: #AAAAAA; font-size: 11px; background: transparent; border: none; }");
    toleranceLabel->setFixedWidth(40);
    toleranceLayout->addWidget(toleranceLabel);

    m_toleranceSlider = new QSlider(Qt::Horizontal, toleranceRow);
    m_toleranceSlider->setRange(0, 100);
    m_toleranceSlider->setValue(30); // 默认容差 30
    m_toleranceSlider->setCursor(Qt::PointingHandCursor);
    m_toleranceSlider->setStyleSheet(
        "QSlider::groove:horizontal {"
        "  height: 3px; background: #444; border-radius: 2px;"
        "}"
        "QSlider::handle:horizontal {"
        "  width: 12px; height: 12px;"
        "  background: #EEEEEE; border-radius: 6px;"
        "  margin: -5px 0;"
        "}"
        "QSlider::sub-page:horizontal {"
        "  background: #378ADD; border-radius: 2px;"
        "}"
    );
    toleranceLayout->addWidget(m_toleranceSlider, 1);
    mainLayout->addWidget(toleranceRow);

    setCurrentColor(Qt::red);
    setFocusProxy(m_hexEdit);
    installEventFilter(this);
}

void ColorPicker::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor("#181818"));
    painter.setPen(QPen(QColor("#333333"), 1));
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);
}

bool ColorPicker::eventFilter(QObject* watched, QEvent* event) {
    if (watched == this && event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            close();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ColorPicker::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    deleteLater();
}

void ColorPicker::updateColorFromHsv() {
    m_color = QColor::fromHsv(m_h, m_s, m_v);
    updatePreview();
}

void ColorPicker::updatePreview() {
    m_previewBlock->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(m_color.name()));
    m_hexEdit->setText(m_color.name().toUpper());
}

void ColorPicker::syncHsvFromColor() {
    m_color.getHsv(&m_h, &m_s, &m_v);
    if (m_h < 0) m_h = 0;
    m_hueSlider->setHue(m_h);
    m_svPicker->setHue(m_h);
    m_svPicker->setSv(m_s, m_v);
}

void ColorPicker::setCurrentColor(const QColor& c) {
    if (!c.isValid()) return;
    m_color = c;
    syncHsvFromColor();
    updatePreview();
}

// 2026-05-17 按照用户要求：暴露准确度滑条当前值
int ColorPicker::currentTolerance() const {
    return m_toleranceSlider ? m_toleranceSlider->value() : 30;
}

} // namespace ArcMeta
