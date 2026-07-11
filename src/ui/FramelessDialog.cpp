#include "FramelessDialog.h"
#include "UiHelper.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QApplication>
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace FERREX {

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
    m_container->setMouseTracking(true);
    m_container->installEventFilter(this);
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
        // 严格遵循 AGENTS.md 规范：
        // 5.3 窗口置顶 唯一标准：一律使用 Win32 原生 SetWindowPos（HWND_TOPMOST / HWND_NOTOPMOST）
        // 严禁使用 setWindowFlag(Qt::WindowStaysOnTopHint) 或任何导致窗口重建的操作
        // 调用时必须配合 SWP_NOSENDCHANGING 标志
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (checked) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOSENDCHANGING);
        } else {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOSENDCHANGING);
        }
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
            m_maxBtn->setIcon(UiHelper::getIcon("restore_line", QColor("#CCCCCC"), 18));
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
        "QPushButton:hover { background-color: #E81123; } "
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

FramelessDialog::ResizeDir FramelessDialog::getResizeDir(const QPoint& pos) const {
    int x = pos.x();
    int y = pos.y();
    int w = this->width();
    int h = this->height();

    bool left = (x >= 0 && x <= PADDING);
    bool right = (x >= w - PADDING && x <= w);
    bool top = (y >= 0 && y <= PADDING);
    bool bottom = (y >= h - PADDING && y <= h);

    if (left && top) return DIR_TOPLEFT;
    if (right && top) return DIR_TOPRIGHT;
    if (left && bottom) return DIR_BOTTOMLEFT;
    if (right && bottom) return DIR_BOTTOMRIGHT;
    if (left) return DIR_LEFT;
    if (right) return DIR_RIGHT;
    if (top) return DIR_TOP;
    if (bottom) return DIR_BOTTOM;

    return DIR_NONE;
}

void FramelessDialog::updateCursorShape(ResizeDir dir) {
    switch (dir) {
        case DIR_LEFT:
        case DIR_RIGHT:
            setCursor(Qt::SizeHorCursor);
            break;
        case DIR_TOP:
        case DIR_BOTTOM:
            setCursor(Qt::SizeVerCursor);
            break;
        case DIR_TOPLEFT:
        case DIR_BOTTOMRIGHT:
            setCursor(Qt::SizeFDiagCursor);
            break;
        case DIR_TOPRIGHT:
        case DIR_BOTTOMLEFT:
            setCursor(Qt::SizeBDiagCursor);
            break;
        default:
            setCursor(Qt::ArrowCursor);
            break;
    }
}

void FramelessDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QPoint localPos = event->pos();
        ResizeDir dir = getResizeDir(localPos);
        if (dir != DIR_NONE) {
            m_resizeDir = dir;
            m_startGlobalPos = event->globalPosition().toPoint();
            m_startGeometry = geometry();
            event->accept();
            return;
        }

        // 判定是否在标题栏区域拖拽
        QWidget* child = childAt(localPos);
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
    if (m_resizeDir != DIR_NONE) {
        QPoint currentGlobalPos = event->globalPosition().toPoint();
        int dx = currentGlobalPos.x() - m_startGlobalPos.x();
        int dy = currentGlobalPos.y() - m_startGlobalPos.y();

        int minWidth = minimumSize().width();
        int minHeight = minimumSize().height();

        QRect newGeom = m_startGeometry;

        switch (m_resizeDir) {
            case DIR_RIGHT: {
                int newWidth = qMax(minWidth, m_startGeometry.width() + dx);
                newGeom.setWidth(newWidth);
                break;
            }
            case DIR_BOTTOM: {
                int newHeight = qMax(minHeight, m_startGeometry.height() + dy);
                newGeom.setHeight(newHeight);
                break;
            }
            case DIR_LEFT: {
                int newWidth = qMax(minWidth, m_startGeometry.width() - dx);
                int newX = m_startGeometry.right() - newWidth + 1;
                newGeom.setLeft(newX);
                newGeom.setWidth(newWidth);
                break;
            }
            case DIR_TOP: {
                int newHeight = qMax(minHeight, m_startGeometry.height() - dy);
                int newY = m_startGeometry.bottom() - newHeight + 1;
                newGeom.setTop(newY);
                newGeom.setHeight(newHeight);
                break;
            }
            case DIR_TOPLEFT: {
                int newWidth = qMax(minWidth, m_startGeometry.width() - dx);
                int newX = m_startGeometry.right() - newWidth + 1;
                int newHeight = qMax(minHeight, m_startGeometry.height() - dy);
                int newY = m_startGeometry.bottom() - newHeight + 1;
                newGeom.setLeft(newX);
                newGeom.setWidth(newWidth);
                newGeom.setTop(newY);
                newGeom.setHeight(newHeight);
                break;
            }
            case DIR_TOPRIGHT: {
                int newWidth = qMax(minWidth, m_startGeometry.width() + dx);
                int newHeight = qMax(minHeight, m_startGeometry.height() - dy);
                int newY = m_startGeometry.bottom() - newHeight + 1;
                newGeom.setWidth(newWidth);
                newGeom.setTop(newY);
                newGeom.setHeight(newHeight);
                break;
            }
            case DIR_BOTTOMLEFT: {
                int newWidth = qMax(minWidth, m_startGeometry.width() - dx);
                int newX = m_startGeometry.right() - newWidth + 1;
                int newHeight = qMax(minHeight, m_startGeometry.height() + dy);
                newGeom.setLeft(newX);
                newGeom.setWidth(newWidth);
                newGeom.setHeight(newHeight);
                break;
            }
            case DIR_BOTTOMRIGHT: {
                int newWidth = qMax(minWidth, m_startGeometry.width() + dx);
                int newHeight = qMax(minHeight, m_startGeometry.height() + dy);
                newGeom.setWidth(newWidth);
                newGeom.setHeight(newHeight);
                break;
            }
            default:
                break;
        }

        setGeometry(newGeom);
        event->accept();
        return;
    }

    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        // 2026-07-10 核心重构：支持最大化拖拽向下自适应还原并跟随移动（对应用户原话：“拖动标题栏也无法恢复窗口”）
        if (isMaximized()) {
            // A. 保存最大化时鼠标在标题栏的横向位置
            QPoint globalPos = event->globalPosition().toPoint();
            double relativeRatio = (double)event->pos().x() / (double)width(); // 获取相对宽度的百分比
            
            // B. 触发还原
            showNormal();
            
            // C. 重新计算还原后窗口由于变小，m_dragPos 相对标题栏横向应处于的新等比例坐标
            int newDragX = qRound(relativeRatio * width());
            
            // 纵向通常高度保持固定（17px 即标题栏中线上），横向按比例定位
            m_dragPos = QPoint(newDragX, 17); 
            
            // D. 根据新偏移重算窗口还原后的物理起始左上角
            move(globalPos - m_dragPos);
            event->accept();
            return;
        }

        // 常规状态下的拖动
        move(event->globalPosition().toPoint() - m_dragPos);
        event->accept();
        return;
    }

    // 处于正常悬停阶段，更新光标样式
    ResizeDir dir = getResizeDir(event->pos());
    updateCursorShape(dir);

    QDialog::mouseMoveEvent(event);
}

void FramelessDialog::mouseReleaseEvent(QMouseEvent* event) {
    m_isDragging = false;
    m_resizeDir = DIR_NONE;
    updateCursorShape(getResizeDir(event->pos()));
    QDialog::mouseReleaseEvent(event);
}

// 2026-07-10 新增：物理支持双击自定义标题栏最大化/常规还原（对应用户原话：“双击标题栏也恢复不了窗口”）
void FramelessDialog::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
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
            
            // 排除标题栏中的按钮，确保不会在双击置顶/最小化/最大化按钮时引起误判
            if (inTitleBar && !qobject_cast<QPushButton*>(child)) {
                if (isMaximized()) {
                    showNormal();
                } else {
                    showMaximized();
                }
                event->accept();
                return;
            }
        }
    }
    QDialog::mouseDoubleClickEvent(event);
}

// 2026-07-10 新增：全面监听系统级窗口状态改变事件，确保按钮形态完美对齐（对应用户原话：“点击恢复按钮有时无法恢复”）
void FramelessDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) {
        if (m_maxBtn) {
            if (isMaximized()) {
                // 如果当前为最大化，按钮应渲染为“常规/恢复（restore_line）”图标，其 tooltip 变更为“向下还原”
                m_maxBtn->setIcon(UiHelper::getIcon("restore_line", QColor("#CCCCCC"), 16));
                m_maxBtn->setProperty("tooltipText", "向下还原");
            } else {
                // 如果当前恢复为常规，按钮应渲染为“最大化（maximize）”图标，其 tooltip 变更为“最大化”
                m_maxBtn->setIcon(UiHelper::getIcon("maximize", QColor("#CCCCCC"), 16));
                m_maxBtn->setProperty("tooltipText", "最大化");
            }
        }
    }
    QDialog::changeEvent(event);
}


void FramelessDialog::keyPressEvent(QKeyEvent* event) {
    // 2026-07-10 新增：整个应用的任何无边框对话框界面，皆支持 Ctrl+W 关闭窗口（对应用户原话：“我期望整个应用的任何界面都必须支持Ctrl+W关闭窗口”）
    if (event->key() == Qt::Key_W && (event->modifiers() & Qt::ControlModifier)) {
        reject();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        // 物理还原普通对话框两段式 UX：若有非空输入框则先清空，否则关闭
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
    if (watched == m_container) {
        if (event->type() == QEvent::MouseMove) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            // 将 m_container 的局部 mouseMove 映射为本窗口坐标，然后转发给 mouseMoveEvent
            QPoint dialogPos = mapFromGlobal(mouseEvent->globalPosition().toPoint());
            QMouseEvent mappedEvent(mouseEvent->type(), dialogPos, mouseEvent->globalPosition(), 
                                    mouseEvent->button(), mouseEvent->buttons(), mouseEvent->modifiers());
            mouseMoveEvent(&mappedEvent);
            // 必须不拦截，允许 container 内的控件继续接收鼠标移动事件
            return false;
        } else if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            QPoint dialogPos = mapFromGlobal(mouseEvent->globalPosition().toPoint());
            ResizeDir dir = getResizeDir(dialogPos);
            if (dir != DIR_NONE) {
                // 如果是在边缘上按下的，重定向到主窗口的鼠标按下事件
                QMouseEvent mappedEvent(mouseEvent->type(), dialogPos, mouseEvent->globalPosition(),
                                        mouseEvent->button(), mouseEvent->buttons(), mouseEvent->modifiers());
                mousePressEvent(&mappedEvent);
                return true; // 拦截此事件，不向子部件派发，避免边缘拖拽导致子控件获焦或误触
            }
        }
    }
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

} // namespace FERREX
