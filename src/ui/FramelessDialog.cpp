#include "FramelessDialog.h"
#include "UiHelper.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QApplication>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace ArcMeta {

// ============================================================================
// FramelessDialog 基类实现
// ============================================================================
FramelessDialog::FramelessDialog(const QString& title, QWidget* parent) 
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window) 
{
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);

    setWindowTitle(title);

    // 2026-05-10 对标规范：通过 DwmSetWindowAttribute 开启 Windows 11 原生圆角
    DWORD attribute = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()), 33, &attribute, sizeof(attribute));

    m_outerLayout = new QVBoxLayout(this);
    m_outerLayout->setContentsMargins(0, 0, 0, 0);

    m_container = new QWidget(this);
    m_container->setObjectName("DialogContainer");
    m_container->setAttribute(Qt::WA_StyledBackground);
    m_container->setStyleSheet(
        "#DialogContainer {"
        "  background-color: #1E1E1E;"
        "  border: 1px solid #333333;"
        "  border-radius: 6px;"
        "}"
    );
    m_outerLayout->addWidget(m_container);

    m_mainLayout = new QVBoxLayout(m_container);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    // --- 标题栏 ---
    auto* titleBar = new QWidget();
    titleBar->setObjectName("TitleBar");
    titleBar->setFixedHeight(34);
    // 移除 border-bottom 以防止穿透组件，改为使用独立的物理分割线
    titleBar->setStyleSheet("background-color: transparent; border: none;");
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(12, 0, 5, 0); // 右侧对齐 5px 物理边距
    titleLayout->setSpacing(4);

    m_titleLabel = new QLabel(title);
    m_titleLabel->setStyleSheet("color: #AAAAAA; font-size: 12px; font-weight: bold; border: none;");
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addStretch();

    auto createTitleBtn = [this](const QString& iconName, const QString& tooltip, const QString& hoverColor) {
        QPushButton* btn = new QPushButton();
        // 2026-05-16 对标 MainWindow 规范：尺寸固定为 24x24 -> 20x20，图标 18x18 -> 16x16
        btn->setFixedSize(20, 20);
        btn->setIcon(UiHelper::getIcon(iconName, QColor("#CCCCCC"), 16));
        btn->setIconSize(QSize(16, 16));
        btn->setAutoDefault(false);
        btn->setProperty("tooltipText", tooltip);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QString(
            "QPushButton { background-color: transparent; border: none; border-radius: 4px; padding: 0; } "
            "QPushButton:hover { background-color: %1; } "
            "QPushButton:pressed { background-color: #555555; }"
        ).arg(hoverColor));
        btn->installEventFilter(this);
        return btn;
    };

    m_pinBtn = createTitleBtn("pin_tilted", "置顶", "#333333");
    // 2026-05-16 置顶按钮逻辑规范：移除内边距，改由全局 spacing 控制
    m_pinBtn->setCheckable(true);
    m_pinBtn->setStyleSheet(
        "QPushButton { background-color: transparent; border: none; border-radius: 4px; } "
        "QPushButton:hover { background-color: #333333; } "
        "QPushButton:checked { background-color: rgba(255, 85, 28, 0.2); }" // 2026-05-16 品牌橙高亮
    );
    connect(m_pinBtn, &QPushButton::toggled, this, [this](bool checked) {
        m_pinBtn->setIcon(UiHelper::getIcon(checked ? "pin_vertical" : "pin_tilted", 
                                            checked ? QColor("#FF551C") : QColor("#CCCCCC"), 18));
        // 设置/取消置顶
        setWindowFlag(Qt::WindowStaysOnTopHint, checked);
        show();
    });

    m_minBtn = createTitleBtn("minimize", "最小化", "#333333");
    connect(m_minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);

    m_maxBtn = createTitleBtn("maximize", "最大化", "#333333");
    connect(m_maxBtn, &QPushButton::clicked, this, [this]() {
        if (isMaximized()) {
            showNormal();
            m_maxBtn->setIcon(UiHelper::getIcon("maximize", QColor("#CCCCCC"), 18));
        } else {
            showMaximized();
            m_maxBtn->setIcon(UiHelper::getIcon("restore_window", QColor("#CCCCCC"), 18));
        }
    });

    m_closeBtn = new QPushButton();
    m_closeBtn->setFixedSize(20, 20); // 2026-05-16 同步为 20x20
    m_closeBtn->setIcon(UiHelper::getIcon("close", QColor("#FFFFFF"), 16));
    m_closeBtn->setIconSize(QSize(16, 16));
    m_closeBtn->setAutoDefault(false);
    m_closeBtn->setProperty("tooltipText", "关闭");
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setStyleSheet(
        "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
        "QPushButton:hover { background-color: #F1707A; } "
        "QPushButton:pressed { background-color: #A50000; }"
    );
    m_closeBtn->installEventFilter(this);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    // 2026-05-10 对标规范：从右到左排列 (布局顺序: pin -> min -> max -> close)
    titleLayout->addWidget(m_pinBtn);
    titleLayout->addWidget(m_minBtn);
    titleLayout->addWidget(m_maxBtn);
    titleLayout->addWidget(m_closeBtn);

    m_mainLayout->addWidget(titleBar);

    // 按照用户要求：将标题栏下方的切割线向下偏移 2 像素 -> 增加到 4 像素以实现明显的向下偏移
    m_mainLayout->addSpacing(4);

    // 添加独立的 1px 分割线，彻底杜绝 UI 穿透问题
    auto* line = new QFrame();
    line->setFixedHeight(1);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setStyleSheet("background-color: #333333; border: none;");
    m_mainLayout->addWidget(line);

    m_contentArea = new QWidget();
    m_contentArea->setObjectName("DialogContentArea");
    m_contentArea->setStyleSheet("QWidget#DialogContentArea { background: transparent; border: none; }");
    m_mainLayout->addWidget(m_contentArea, 1);
}

void FramelessDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
}

void FramelessDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // 判定是否在标题栏区域拖拽
        QWidget* child = childAt(event->pos());
        if (child) {
            bool inTitleBar = false;
            QWidget* p = child;
            while (p && p != m_container) {
                if (p->objectName() == "TitleBar") {
                    inTitleBar = true;
                    break;
                }
                p = p->parentWidget();
            }
            
            // 排除掉标题栏上的按钮
            if (inTitleBar && !qobject_cast<QPushButton*>(child)) {
                m_isDragging = true;
                m_dragPos = event->globalPosition().toPoint() - frameGeometry().topLeft();
                event->accept();
                return;
            }
        }
    }
    QDialog::mousePressEvent(event);
}

void FramelessDialog::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void FramelessDialog::mouseReleaseEvent(QMouseEvent* event) {
    m_isDragging = false;
    QDialog::mouseReleaseEvent(event);
}


void FramelessDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        // 物理还原两段式 UX：若有非空输入框则先清空，否则关闭
        QLineEdit* edit = findChild<QLineEdit*>();
        if (edit && edit->isVisible() && !edit->text().isEmpty()) {
            edit->clear();
            event->accept();
            return;
        }
        reject();
    } else {
        QDialog::keyPressEvent(event);
    }
}

bool FramelessDialog::eventFilter(QObject* watched, QEvent* event) {
    return QDialog::eventFilter(watched, event);
}

// ============================================================================
// FramelessInputDialog 实现
// ============================================================================
FramelessInputDialog::FramelessInputDialog(const QString& title, const QString& label, 
                                           const QString& initial, QWidget* parent)
    : FramelessDialog(title, parent) 
{
    // 按照用户最新要求：高度减去 50 像素 (260 -> 210)
    resize(500, 210);
    setMinimumSize(400, 190);
    
    auto* layout = new QVBoxLayout(m_contentArea);
    layout->setContentsMargins(20, 15, 20, 20);
    layout->setSpacing(7);

    auto* lbl = new QLabel(label);
    lbl->setStyleSheet("color: #EEEEEE; font-size: 13px;");
    layout->addWidget(lbl);

    m_edit = new QLineEdit(initial);
    m_edit->setMinimumHeight(38);
    // 严格对齐 RapidNotes 输入框风格：深色背景 + 蓝色聚焦边框，应用 6px 圆角
    m_edit->setStyleSheet(
        "QLineEdit {"
        "  background-color: #2D2D2D; border: 1px solid #444; border-radius: 6px;"
        "  padding: 0px 10px; color: white; selection-background-color: #3498db;"
        "  font-size: 14px;"
        "}"
        "QLineEdit:focus { border: 1px solid #3498db; }"
    );
    layout->addWidget(m_edit);

    connect(m_edit, &QLineEdit::returnPressed, this, &QDialog::accept);

    layout->addStretch();

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    
    auto* btnCancel = new QPushButton("取消");
    btnCancel->setFixedSize(80, 32);
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnCancel->setStyleSheet(
        "QPushButton { background-color: transparent; color: #888; border: 1px solid #444; border-radius: 4px; } "
        "QPushButton:hover { color: #EEE; background-color: #333; }"
    );
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(btnCancel);

    auto* btnOk = new QPushButton("确定");
    btnOk->setFixedSize(80, 32);
    btnOk->setCursor(Qt::PointingHandCursor);
    btnOk->setStyleSheet(
        "QPushButton { background-color: #3498db; color: white; border: none; border-radius: 4px; font-weight: bold; } "
        "QPushButton:hover { background-color: #3E3E42; }" // 统一悬停色
    );
    connect(btnOk, &QPushButton::clicked, this, &QDialog::accept);
    btnLayout->addWidget(btnOk);

    layout->addLayout(btnLayout);

    m_edit->setFocus();
    m_edit->selectAll();
}

void FramelessInputDialog::showEvent(QShowEvent* event) {
    FramelessDialog::showEvent(event);
    QTimer::singleShot(50, m_edit, qOverload<>(&QWidget::setFocus));
}

} // namespace ArcMeta
