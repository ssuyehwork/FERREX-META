#include "LoadingWindow.h"
#include "SvgIcons.h"
#include <QVBoxLayout>
#include <QPainter>
#include <QScreen>
#include <QGuiApplication>
#include <QDebug>
#include <QLabel>
#include <QFrame>
#include <QTimer>
#include <QSvgRenderer>
#include <QPalette>
#include <QColor>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ArcMeta {

LoadingWindow::LoadingWindow(QWidget* parent)
    : QWidget(parent), m_rotationAngle(0) {
    qDebug() << "[LoadingWindow] 开始构造";
    
    // 设置窗口属性
    setWindowTitle("ArcMeta - 初始化中...");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);

    // 2026-06-xx 物理修复：通过原生 API 实现置顶，避免标志位导致的重建问题
#ifdef Q_OS_WIN
    QTimer::singleShot(0, this, [this]() {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    });
#else
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
#endif
    
    // 固定大小
    setFixedSize(350, 250);
    
    // 设置背景色
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor("#1E1E1E"));
    setPalette(pal);
    
    // 中央容器布局
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(15);
    layout->setAlignment(Qt::AlignCenter);
    
    // 初始化 SVG 渲染器
    QString refreshSvg = SvgIcons::icons.value("refresh");
    if (!refreshSvg.isEmpty()) {
        m_svgRenderer = std::make_unique<QSvgRenderer>();
        if (m_svgRenderer->load(refreshSvg.toLatin1())) {
            qDebug() << "[LoadingWindow] SVG 加载成功";
        } else {
            qWarning() << "[LoadingWindow] SVG 加载失败";
        }
    } else {
        qWarning() << "[LoadingWindow] refresh 图标不存在";
    }
    
    // 标题标签
    QLabel* titleLabel = new QLabel("ArcMeta", this);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("color: #4FACFE; font-size: 18px; font-weight: bold; background-color: transparent;");
    layout->addWidget(titleLabel);
    
    // 分隔线
    QFrame* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("color: #333333;");
    layout->addWidget(line);
    
    // 状态标签
    m_statusLabel = new QLabel("正在初始化...", this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: #B0B0B0; font-size: 13px; background-color: transparent;");
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    
    // 额外提示
    QLabel* tipLabel = new QLabel("请稍候", this);
    tipLabel->setAlignment(Qt::AlignCenter);
    tipLabel->setStyleSheet("color: #777777; font-size: 11px; background-color: transparent;");
    layout->addWidget(tipLabel);
    
    layout->addStretch();
    
    // 启动动画计时器
    m_animationTimer = new QTimer(this);
    connect(m_animationTimer, &QTimer::timeout, this, &LoadingWindow::onAnimationTimeout);
    m_animationTimer->start(30);
    
    qDebug() << "[LoadingWindow] 构造完成";
}

void LoadingWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    
    // 强制窗口居中显示
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect screenGeometry = screen->geometry();
        int x = (screenGeometry.width() - width()) / 2;
        int y = (screenGeometry.height() - height()) / 2;
        move(x, y);
        qDebug() << "[LoadingWindow] 窗口已移动到位置:" << x << y;
    }
    
    // 获取焦点并置顶
    raise();
    activateWindow();
    setFocus();
    qDebug() << "[LoadingWindow] 已激活窗口并获得焦点";
}

void LoadingWindow::updateStatus(const QString& text) {
    if (m_statusLabel) {
        m_statusLabel->setText(text);
        qDebug() << "[LoadingWindow] 进度更新:" << text;
    }
}

void LoadingWindow::onInitializationFinished() {
    qDebug() << "[LoadingWindow] 收到初始化完成信号";
    if (m_animationTimer) {
        m_animationTimer->stop();
    }
    
    // 简单延迟后关闭
    QTimer::singleShot(300, this, [this]() {
        qDebug() << "[LoadingWindow] 正在关闭窗口";
        close();
        emit finished();
    });
}

void LoadingWindow::onAnimationTimeout() {
    m_rotationAngle = (m_rotationAngle + 6) % 360;
    update();
}

void LoadingWindow::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);

    if (!m_svgRenderer || !m_svgRenderer->isValid()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 绘制旋转的 SVG 图标
    int iconSize = 60;
    int x = (width() - iconSize) / 2;
    int y = 100;

    painter.save();
    painter.translate(x + iconSize / 2, y + iconSize / 2);
    painter.rotate(m_rotationAngle);
    painter.translate(-(x + iconSize / 2), -(y + iconSize / 2));

    m_svgRenderer->render(&painter, QRect(x, y, iconSize, iconSize));

    painter.restore();
}

} // namespace ArcMeta
