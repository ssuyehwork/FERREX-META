#include "QuickLookWindow.h"
#include <QKeyEvent>
#include <QFileInfo>
#include <QFile>
#include <QGraphicsPixmapItem>
#include <QLabel>
#include <QShortcut>
#include "UiHelper.h"

namespace ArcMeta {

QuickLookWindow& QuickLookWindow::instance() {
    static QuickLookWindow inst;
    return inst;
}

QuickLookWindow::QuickLookWindow() : QWidget(nullptr) {
    // 强制赋予全屏及最高层级，禁绝系统装饰
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setStyleSheet("QWidget { background-color: rgba(30, 30, 30, 0.95); border: 1px solid #444; border-radius: 12px; }");
    
    resize(800, 600);
    initUi();

    // 2026-04-11 按照用户要求：使用 QShortcut 物理级拦截窗口快捷键
    // 这是解决 QAbstractScrollArea/Viewport 内部键盘事件分发拦截失败的最优解
    new QShortcut(QKeySequence(Qt::Key_Space), this, SLOT(hide()), nullptr, Qt::WindowShortcut);
    new QShortcut(QKeySequence(Qt::Key_Escape), this, SLOT(hide()), nullptr, Qt::WindowShortcut);
}

void QuickLookWindow::initUi() {
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(10, 10, 10, 10);

    // 图片渲染层
    m_graphicsView = new QGraphicsView(this);
    m_graphicsView->setRenderHint(QPainter::Antialiasing);
    m_graphicsView->setRenderHint(QPainter::SmoothPixmapTransform);
    m_graphicsView->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    m_graphicsView->setStyleSheet("background: transparent; border: none;");
    m_scene = new QGraphicsScene(this);
    m_graphicsView->setScene(m_scene);
    
    // 文本渲染层
    m_textPreview = new QPlainTextEdit(this);
    m_textPreview->setReadOnly(true);
    // 2026-04-11 按照用户要求：彻底修正原生样式污染问题
    // transparent 会穿透至系统合成层（白色）而非深色背景，必须显式指定 #1E1E1E
    // 同时完整覆盖 viewport、滚动条以及边框，确保与全局风格一致
    m_textPreview->setStyleSheet(
        "QPlainTextEdit {"
        "  background-color: #1E1E1E;"
        "  color: #DDDDDD;"
        "  border: none;"
        "  font-family: 'Consolas', 'Microsoft YaHei';"
        "  font-size: 13px;"
        "  padding: 16px;"
        "}"
        "QPlainTextEdit QScrollBar:vertical {"
        "  border: none; background: transparent; width: 6px; margin: 0px;"
        "}"
        "QPlainTextEdit QScrollBar::handle:vertical {"
        "  background: #444444; min-height: 20px; border-radius: 3px;"
        "}"
        "QPlainTextEdit QScrollBar::handle:vertical:hover { background: #555555; }"
        "QPlainTextEdit QScrollBar::add-line:vertical, QPlainTextEdit QScrollBar::sub-line:vertical { height: 0px; }"
        "QPlainTextEdit QScrollBar:horizontal { height: 6px; background: transparent; border: none; margin: 0px; }"
        "QPlainTextEdit QScrollBar::handle:horizontal { background: #444444; border-radius: 3px; min-width: 20px; }"
        "QPlainTextEdit QScrollBar::handle:horizontal:hover { background: #555555; }"
        "QPlainTextEdit QScrollBar::add-line:horizontal, QPlainTextEdit QScrollBar::sub-line:horizontal { width: 0px; }"
        "QPlainTextEdit QScrollBar::add-page:vertical, QPlainTextEdit QScrollBar::sub-page:vertical, "
        "QPlainTextEdit QScrollBar::add-page:horizontal, QPlainTextEdit QScrollBar::sub-page:horizontal { background: none; }"
    );
    
    m_mainLayout->addWidget(m_graphicsView);
    m_mainLayout->addWidget(m_textPreview);

    m_graphicsView->hide();
    m_textPreview->hide();
}

void QuickLookWindow::previewFile(const QString& path) {
    // 2026-04-11 按照用户要求：逻辑重构，先进入全屏锁定几何尺寸，再执行图片加载计算
    showFullScreen();
    raise();
    activateWindow();

    QFileInfo info(path);
    QString ext = info.suffix().toLower();
    if (UiHelper::isGraphicsFile(ext)) {
        renderProfessionalImage(path);
    } else {
        renderText(path);
    }
}

void QuickLookWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // 2026-04-11 按照用户要求：物理解决“首次打开比例报错”问题
    // 强制在窗口尺寸变更（如全屏化）后重新执行 fitInView，确保缩放比绝对精确
    if (m_graphicsView && m_graphicsView->isVisible() && m_scene) {
        auto items = m_scene->items();
        if (!items.isEmpty()) {
            // 重新适配当前视图内的图片比例
            m_graphicsView->fitInView(items.first(), Qt::KeepAspectRatio);
        }
    }
}

/**
 * @brief 硬件加速图片渲染
 */
void QuickLookWindow::renderImage(const QString& path) {
    m_textPreview->hide();
    m_graphicsView->show();
    m_scene->clear();
    // 2026-04-11 按照用户要求：切图时强制重置缩放矩阵，确保初始为 1:1 或满屏适配态
    m_graphicsView->resetTransform();

    QPixmap pix(path);
    if (!pix.isNull()) {
        // 2026-04-11 按照用户要求：回归标准图片加载逻辑，Qt 自动处理正向扫描线
        auto item = m_scene->addPixmap(pix);
        m_graphicsView->fitInView(item, Qt::KeepAspectRatio);
    }
}

/**
 * @brief 使用 Shell 引擎渲染高清专业预览图 (PSD/AI/EPS/PDF)
 */
void QuickLookWindow::renderProfessionalImage(const QString& path) {
    m_textPreview->hide();
    m_graphicsView->show();
    m_scene->clear();
    // 2026-04-11 按照用户要求：切图时强制重置缩放矩阵
    m_graphicsView->resetTransform();

    // 2026-04-11 按照用户要求：请求 1024 级高清缩略图以支持 PSD/AI 快速预览
    QImage img = UiHelper::getShellThumbnail(path, 1024);
    if (!img.isNull()) {
        QPixmap pix = QPixmap::fromImage(img);
        auto item = m_scene->addPixmap(pix);
        m_graphicsView->fitInView(item, Qt::KeepAspectRatio);
    }
}

/**
 * @brief 极速文本加载（红线：支持内存映射思想）
 */
void QuickLookWindow::renderText(const QString& path) {
    m_graphicsView->hide();
    m_textPreview->show();
    // 2026-04-11 按照用户要求：文字模式下强制将焦点设置到 viewport，
    // 确保方向键、Page 等导航类按键能正常工作
    m_textPreview->setFocus();
    
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        // 对于大文件，仅加载前 128KB (文档红线要求)
        QByteArray previewBytes = file.read(128 * 1024); 
        m_textPreview->setPlainText(QString::fromUtf8(previewBytes));
        file.close();
    }
}

void QuickLookWindow::wheelEvent(QWheelEvent* event) {
    // 2026-04-11 按照用户要求：增加滚轮物理交互支持
    if (m_graphicsView && m_graphicsView->isVisible()) {
        // 图片模式：以鼠标为中心进行硬件加速缩放
        double factor = (event->angleDelta().y() > 0) ? 1.15 : 0.85;
        
        m_graphicsView->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        m_graphicsView->scale(factor, factor);
        event->accept();
    } else if (m_textPreview && m_textPreview->isVisible()) {
        // 文本模式：调整字号大小
        if (event->angleDelta().y() > 0) {
            m_textPreview->zoomIn(1);
        } else {
            m_textPreview->zoomOut(1);
        }
        event->accept();
    }
}

/**
 * @brief 按键交互：ESC 或 Space 退出预览，1-5 快速打标预览点位
 */
void QuickLookWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Space) {
        hide();
    } else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Up) {
        // 2026-04-11 按照用户要求：支持通过方向键显示上一个文件
        emit prevRequested();
    } else if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Down) {
        // 2026-04-11 按照用户要求：支持通过方向键显示下一个文件
        emit nextRequested();
    } else if (event->modifiers() & Qt::AltModifier && event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
        // 2026-04-11 按照用户要求：补全预览窗内的颜色标签快捷键映射 (Alt + 1-9)
        QString color;
        switch (event->key()) {
            case Qt::Key_1: color = "red"; break;
            case Qt::Key_2: color = "orange"; break;
            case Qt::Key_3: color = "yellow"; break;
            case Qt::Key_4: color = "green"; break;
            case Qt::Key_5: color = "cyan"; break;
            case Qt::Key_6: color = "blue"; break;
            case Qt::Key_7: color = "purple"; break;
            case Qt::Key_8: color = "gray"; break;
            case Qt::Key_9: color = ""; break;
        }
        emit colorRequested(color);
    } else if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_5 && !(event->modifiers() & Qt::AltModifier)) {
        int rating = event->key() - Qt::Key_0;
        emit ratingRequested(rating);
    }
}

} // namespace ArcMeta
