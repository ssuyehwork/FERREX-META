#include "DriveButton.h"
#include <QPainter>
#include <QMouseEvent>
#include "UiHelper.h"

namespace ArcMeta {

DriveButton::DriveButton(const QString& driveLetter, QWidget* parent)
    : QPushButton(parent), m_driveLetter(driveLetter) {
    setFixedSize(60, 28);
    setCursor(Qt::PointingHandCursor);
    
    m_animationTimer = new QTimer(this);
    connect(m_animationTimer, &QTimer::timeout, this, &DriveButton::updateAnimation);
}

void DriveButton::setState(State state) {
    if (m_state == state) return;
    m_state = state;
    
    if (m_state == Running) {
        startRotation();
    } else {
        stopRotation();
    }
    update();
}

void DriveButton::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QRect rect = this->rect().adjusted(1, 1, -1, -1);
    
    // 1. 背景绘制 (严格对齐 Plan-108 视觉规范)
    QColor bgColor;
    QColor borderColor;
    QColor textColor = Style::TextMain;

    switch (m_state) {
        case Inactive:
            bgColor = QColor("#333333");
            borderColor = QColor("#444444");
            textColor = Style::TextDim;
            break;
        case Active:
        case Running:
            bgColor = Style::PrimaryBlue;
            borderColor = Style::PrimaryBlue.lighter(110);
            break;
        case Paused:
            bgColor = QColor("#555555");
            borderColor = QColor("#666666");
            break;
    }

    painter.setPen(QPen(borderColor, 1));
    painter.setBrush(bgColor);
    painter.drawRoundedRect(rect, 4, 4);

    // 2. 盘符文字
    painter.setPen(textColor);
    QFont font = painter.font();
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(rect.adjusted(5, 0, -20, 0), Qt::AlignCenter, m_driveLetter);

    // 3. 状态图标绘制
    QRect iconRect(rect.right() - 22, rect.top() + (rect.height() - 16) / 2, 16, 16);
    
    if (m_state == Running) {
        // 旋转绘制 refresh 图标
        painter.save();
        painter.translate(iconRect.center());
        painter.rotate(m_rotationAngle);
        painter.translate(-iconRect.center());
        QPixmap pix = UiHelper::getPixmap("refresh", QSize(16, 16), Qt::white);
        painter.drawPixmap(iconRect, pix);
        painter.restore();
    } else if (m_state == Paused) {
        // 绘制暂停图标
        QIcon pauseIcon = UiHelper::getIcon("pause", Qt::white, 16);
        pauseIcon.paint(&painter, iconRect);
    } else if (m_state == Active) {
        // 激活态待机显示打勾 (或空，暂定打勾以示激活)
        QIcon checkIcon = UiHelper::getIcon("check", Qt::white, 14);
        checkIcon.paint(&painter, iconRect);
    }
}

void DriveButton::mousePressEvent(QMouseEvent* event) {
    QPushButton::mousePressEvent(event);
}

void DriveButton::mouseReleaseEvent(QMouseEvent* event) {
    QPushButton::mouseReleaseEvent(event);
}

void DriveButton::updateAnimation() {
    m_rotationAngle = (m_rotationAngle + 10) % 360;
    update();
}

void DriveButton::startRotation() {
    if (!m_animationTimer->isActive()) {
        m_animationTimer->start(30);
    }
}

void DriveButton::stopRotation() {
    m_animationTimer->stop();
    m_rotationAngle = 0;
}

} // namespace ArcMeta
