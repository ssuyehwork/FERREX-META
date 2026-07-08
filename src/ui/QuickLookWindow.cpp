#include "QuickLookWindow.h"
#include "UiHelper.h"
#include <QKeyEvent>
#include <QFileInfo>
#include <QScreen>
#include <QApplication>
#include <QPainter>
#include <QFile>
#include <QStringDecoder>
#include <QScrollBar>
#include <algorithm>
#include <QSvgRenderer>
#include <QtConcurrent>
#include <QPointer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace FERREX {

QuickLookWindow::QuickLookWindow(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);

    setupUi();
    installEventFilter(this);
}

QuickLookWindow::~QuickLookWindow() {}

void QuickLookWindow::setupUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 20);

    m_container = new QWidget();
    m_container->setObjectName("QLContainer");
    m_container->setStyleSheet(R"(
        #QLContainer {
            background-color: rgba(30, 30, 30, 230);
            border: 1px solid #444;
            border-radius: 12px;
        }
        QLabel { color: #CCC; font-size: 12px; }
        #QLTitle { color: #FF8C00; font-weight: bold; font-size: 14px; }
        QPlainTextEdit {
            background: transparent;
            border: none;
            color: #D4D4D4;
            font-family: 'Consolas', 'Monaco', 'PingFang SC', 'Microsoft YaHei';
            font-size: 13px;
        }
    )");

    auto* layout = new QVBoxLayout(m_container);
    layout->setContentsMargins(15, 15, 15, 15);
    layout->setSpacing(10);

    m_titleLabel = new QLabel();
    m_titleLabel->setObjectName("QLTitle");
    layout->addWidget(m_titleLabel);

    m_imageLabel = new QLabel();
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_imageLabel);

    m_textEdit = new QPlainTextEdit();
    m_textEdit->setReadOnly(true);
    m_textEdit->hide();
    // 美化滚动条
    m_textEdit->verticalScrollBar()->setStyleSheet(R"(
        QScrollBar:vertical { width: 4px; background: transparent; }
        QScrollBar::handle:vertical { background: #444; border-radius: 2px; }
    )");
    layout->addWidget(m_textEdit);

    m_infoLabel = new QLabel();
    m_infoLabel->setStyleSheet("color: #777;");
    layout->addWidget(m_infoLabel);

    rootLayout->addWidget(m_container);

    resize(800, 600);
}

void QuickLookWindow::preview(const QString& filePath) {
    m_currentPath = filePath;
    QFileInfo fi(filePath);
    m_titleLabel->setText(fi.fileName());

    QString ext = fi.suffix().toLower();

    if (UiHelper::isGraphicsFile(ext)) {
        renderImage(filePath);
    } else {
        renderText(filePath);
    }

    // 居中显示
    if (!isVisible()) {
        QScreen* screen = QGuiApplication::primaryScreen();
        if (parentWidget() && parentWidget()->window()) {
            screen = parentWidget()->window()->screen();
        }
        QRect screenGeom = screen->geometry();
        move(screenGeom.center() - rect().center());
        show();
    }

    raise();
    activateWindow();

#ifdef Q_OS_WIN
    // 强制置顶
    SetWindowPos((HWND)winId(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
#endif
}

void QuickLookWindow::closePreview() {
    hide();
}

void QuickLookWindow::renderImage(const QString& path) {
    m_textEdit->hide();
    m_imageLabel->show();
    m_imageLabel->setText("正在加载预览...");

    QFileInfo fi(path);
    QString ext = fi.suffix().toLower();

    // 异步加载图片防止 UI 阻塞
    QPointer<QuickLookWindow> weakThis(this);
    (void)QtConcurrent::run([weakThis, path, ext]() {
        if (!weakThis) return;

        QImage img;
        if (ext == "svg") {
            QSvgRenderer renderer(path);
            if (renderer.isValid()) {
                img = QImage(1024, 1024, QImage::Format_ARGB32);
                img.fill(Qt::transparent);
                QPainter painter(&img);
                renderer.render(&painter);
            }
        } else {
            // 优先尝试缩略图引擎（处理 PSD/RAW 等）
            img = UiHelper::getShellThumbnail(path, 1024);
            if (img.isNull()) {
                img.load(path);
            }
        }

        if (!weakThis) return;
        QMetaObject::invokeMethod(weakThis.data(), [weakThis, img, path]() {
            if (!weakThis || weakThis->m_currentPath != path) return;
            if (!img.isNull()) {
                QPixmap pix = QPixmap::fromImage(img);
                if (pix.width() > weakThis->m_imageLabel->width() || pix.height() > weakThis->m_imageLabel->height()) {
                    pix = pix.scaled(weakThis->m_imageLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
                weakThis->m_imageLabel->setPixmap(pix);
                weakThis->m_infoLabel->setText(QString("%1x%2 | %3").arg(img.width()).arg(img.height()).arg(path));
            } else {
                weakThis->renderText(path); // 图片加载失败尝试文本模式
            }
        });
    });
}

void QuickLookWindow::renderText(const QString& path) {
    m_imageLabel->hide();
    m_textEdit->show();
    m_textEdit->setPlainText("正在读取文件...");

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_textEdit->setPlainText("无法打开文件进行预览。");
        return;
    }

    // 限制读取 128KB
    QByteArray data = file.read(128 * 1024);
    file.close();

    // 检查是否为二进制文件。如果是 UTF-16，虽然含有 null，但不应视为二进制
    bool potentialUtf16 = data.startsWith("\xFF\xFE") || data.startsWith("\xFE\xFF");
    if (!potentialUtf16 && isBinary(data)) {
        m_textEdit->hide();
        m_imageLabel->show();
        m_imageLabel->setText("二进制文件，无法预览。");
        m_infoLabel->setText(path);
        return;
    }

    QString encodingName = detectEncoding(data);
    QString text;

    if (encodingName == "UTF-8") {
        text = QString::fromUtf8(data);
    } else if (encodingName == "UTF-16LE") {
        text = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(data.constData()), data.size() / 2);
    } else if (encodingName == "UTF-16BE") {
        auto decoder = QStringDecoder(QStringDecoder::Utf16BE);
        text = decoder(data);
    } else {
        // GBK / Local8Bit
        text = QString::fromLocal8Bit(data);
    }

    m_textEdit->setPlainText(text);
    m_textEdit->verticalScrollBar()->setValue(0);
    m_infoLabel->setText(QString("编码: %1 | 大小: %2 KB | %3").arg(encodingName).arg(QFileInfo(path).size() / 1024.0, 0, 'f', 1).arg(path));
}

bool QuickLookWindow::isBinary(const QByteArray& data) {
    if (data.isEmpty()) return false;
    // 检查前 1KB
    int checkLen = std::min<int>(data.size(), 1024);
    int continuousNull = 0;
    for (int i = 0; i < checkLen; ++i) {
        if (data[i] == '\0') {
            continuousNull++;
            if (continuousNull > 2) return true; // 连续 3 个 null 基本确定是二进制
        } else {
            continuousNull = 0;
        }
    }
    return false;
}

QString QuickLookWindow::detectEncoding(const QByteArray& data) {
    if (data.startsWith("\xEF\xBB\xBF")) return "UTF-8";
    if (data.startsWith("\xFF\xFE")) return "UTF-16LE";
    if (data.startsWith("\xFE\xFF")) return "UTF-16BE";

    // 简单检测 UTF-8 特征
    int utf8Count = 0;
    for (int i = 0; i < data.size() - 2; ++i) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 0xC0 && c <= 0xDF) {
            if ((unsigned char)data[i+1] >= 0x80 && (unsigned char)data[i+1] <= 0xBF) { utf8Count++; i++; }
        } else if (c >= 0xE0 && c <= 0xEF) {
            if ((unsigned char)data[i+1] >= 0x80 && (unsigned char)data[i+1] <= 0xBF &&
                (unsigned char)data[i+2] >= 0x80 && (unsigned char)data[i+2] <= 0xBF) { utf8Count += 2; i += 2; }
        }
    }

    return (utf8Count > 0) ? "UTF-8" : "GBK";
}

void QuickLookWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Escape) {
        closePreview();
        return;
    }
    if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Left) {
        emit prevRequested();
        return;
    }
    if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Right) {
        emit nextRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

void QuickLookWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
}

bool QuickLookWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::WindowDeactivate) {
        closePreview();
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace FERREX
