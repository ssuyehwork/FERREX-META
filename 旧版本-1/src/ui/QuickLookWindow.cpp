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
#include <QTimer>
#include <QWheelEvent>
#include <QSet>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace FERREX {

// 静态文件分类后缀定义
static const QSet<QString> VIDEO_EXTS = {"mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "3gp"};
static const QSet<QString> AUDIO_EXTS = {"mp3", "wav", "wma", "flac", "aac", "ogg", "m4a", "ape"};
static const QSet<QString> UNPREVIEWABLE_EXTS = {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "exe", "dll", "msi", "sys", "iso", "dmg", "pkg", "bin", "ini", "lnk"};

// ==========================================
// QuickLookGraphicsView 实现
// ==========================================

QuickLookGraphicsView::QuickLookGraphicsView(QWidget* parent) : QGraphicsView(parent) {
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    
    m_pixmapItem = new QGraphicsPixmapItem();
    m_pixmapItem->setTransformationMode(Qt::SmoothTransformation);
    m_scene->addItem(m_pixmapItem);

    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorUnderMouse);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setStyleSheet("background: transparent; border: none;");
    
    // 美化滚动条
    horizontalScrollBar()->setStyleSheet(R"(
        QScrollBar:horizontal { height: 4px; background: transparent; }
        QScrollBar::handle:horizontal { background: #444; border-radius: 2px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { border: none; background: none; }
    )");
    verticalScrollBar()->setStyleSheet(R"(
        QScrollBar:vertical { width: 4px; background: transparent; }
        QScrollBar::handle:vertical { background: #444; border-radius: 2px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { border: none; background: none; }
    )");
}

void QuickLookGraphicsView::setPixmap(const QPixmap& pixmap) {
    m_pixmapItem->setPixmap(pixmap);
    m_scene->setSceneRect(m_pixmapItem->boundingRect());
    fitImage();
}

void QuickLookGraphicsView::clear() {
    m_pixmapItem->setPixmap(QPixmap());
    m_scene->setSceneRect(QRectF());
    resetTransform();
    m_currentScale = 1.0;
    m_isFitMode = true;
    updateCursor();
}

void QuickLookGraphicsView::fitImage() {
    if (!m_pixmapItem || m_pixmapItem->pixmap().isNull()) return;
    
    resetTransform();
    m_scene->setSceneRect(m_pixmapItem->boundingRect());
    fitInView(m_pixmapItem, Qt::KeepAspectRatio);
    
    m_currentScale = transform().m11();
    m_isFitMode = true;
    updateCursor();
}

void QuickLookGraphicsView::setZoomOriginal() {
    if (!m_pixmapItem || m_pixmapItem->pixmap().isNull()) return;
    
    resetTransform();
    m_scene->setSceneRect(m_pixmapItem->boundingRect());
    m_currentScale = 1.0;
    m_isFitMode = false;
    updateCursor();
}

void QuickLookGraphicsView::wheelEvent(QWheelEvent* event) {
    if (!m_pixmapItem || m_pixmapItem->pixmap().isNull()) {
        QGraphicsView::wheelEvent(event);
        return;
    }

    double factor = 1.15;
    if (event->angleDelta().y() < 0) {
        factor = 1.0 / factor;
    }

    double newScale = m_currentScale * factor;
    if (newScale < 0.1) {
        factor = 0.1 / m_currentScale;
        newScale = 0.1;
    } else if (newScale > 10.0) {
        factor = 10.0 / m_currentScale;
        newScale = 10.0;
    }

    if (qFuzzyCompare(newScale, m_currentScale)) {
        return;
    }

    m_isFitMode = false;
    scale(factor, factor);
    m_currentScale = newScale;
    updateCursor();
}

void QuickLookGraphicsView::mouseDoubleClickEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    if (!m_pixmapItem || m_pixmapItem->pixmap().isNull()) return;

    if (m_isFitMode) {
        setZoomOriginal();
    } else {
        fitImage();
    }
}

void QuickLookGraphicsView::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    if (m_isFitMode) {
        fitImage();
    }
}

void QuickLookGraphicsView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        bool exceeds = (m_pixmapItem->boundingRect().width() * m_currentScale > viewport()->width()) ||
                       (m_pixmapItem->boundingRect().height() * m_currentScale > viewport()->height());
        if (exceeds) {
            setCursor(Qt::ClosedHandCursor);
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void QuickLookGraphicsView::mouseReleaseEvent(QMouseEvent* event) {
    QGraphicsView::mouseReleaseEvent(event);
    updateCursor();
}

void QuickLookGraphicsView::updateCursor() {
    if (!m_pixmapItem || m_pixmapItem->pixmap().isNull()) {
        setCursor(Qt::ArrowCursor);
        return;
    }
    
    bool exceedsHorizontal = m_pixmapItem->boundingRect().width() * m_currentScale > viewport()->width();
    bool exceedsVertical = m_pixmapItem->boundingRect().height() * m_currentScale > viewport()->height();
    
    if (exceedsHorizontal || exceedsVertical) {
        setCursor(Qt::OpenHandCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}


// ==========================================
// QuickLookWindow 实现
// ==========================================

QuickLookWindow::QuickLookWindow(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::Tool);
    
    setupUi();
    installEventFilter(this);
}

QuickLookWindow::~QuickLookWindow() {}

void QuickLookWindow::setupUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    m_container = new QWidget();
    m_container->setObjectName("QLContainer");
    m_container->setStyleSheet(R"(
        #QLContainer {
            background-color: #1E1E1E;
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
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_titleLabel = new QLabel(m_container);
    m_titleLabel->setObjectName("QLTitle");
    m_titleLabel->hide();
    // 2026-07-xx 按用户要求：标题栏不再占用布局空间，仅保留对象供其他函数调用
    // setText()，避免调用处出现空指针；实际不会显示在界面上。

    // 图片渲染控件 (QGraphicsView 替代原本的 QLabel)
    m_graphicsView = new QuickLookGraphicsView();
    m_graphicsView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_graphicsView);

    // 文本渲染控件
    m_textEdit = new QPlainTextEdit();
    m_textEdit->setReadOnly(true);
    m_textEdit->hide();
    m_textEdit->verticalScrollBar()->setStyleSheet(R"(
        QScrollBar:vertical { width: 4px; background: transparent; }
        QScrollBar::handle:vertical { background: #444; border-radius: 2px; }
    )");
    m_textEdit->installEventFilter(this); // 2026-07-xx 交互优化：安装事件过滤器拦截空格键以防吞噬
    layout->addWidget(m_textEdit);

    // 媒体播放器控件容器
    m_mediaContainer = new QWidget();
    m_mediaContainer->hide();
    auto* mediaLayout = new QVBoxLayout(m_mediaContainer);
    mediaLayout->setContentsMargins(0, 0, 0, 0);
    mediaLayout->setSpacing(10);

#ifdef FERREX_HAS_MULTIMEDIA
    m_videoWidget = new QVideoWidget();
    m_videoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mediaLayout->addWidget(m_videoWidget);
#endif

    m_audioPlaceholder = new QLabel();
    m_audioPlaceholder->setAlignment(Qt::AlignCenter);
    m_audioPlaceholder->setStyleSheet("color: #FF8C00; font-size: 20px; font-weight: bold; background: #121212; border-radius: 8px;");
    m_audioPlaceholder->hide();
    mediaLayout->addWidget(m_audioPlaceholder);

    auto* ctrlLayout = new QHBoxLayout();
    ctrlLayout->setContentsMargins(0, 0, 0, 0);
    ctrlLayout->setSpacing(10);

    m_playBtn = new QPushButton("播放");
    m_playBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #333;
            color: #EEE;
            border: 1px solid #555;
            padding: 5px 15px;
            border-radius: 4px;
        }
        QPushButton:hover {
            background-color: #444;
        }
    )");
    ctrlLayout->addWidget(m_playBtn);

    m_timeSlider = new QSlider(Qt::Horizontal);
    m_timeSlider->setStyleSheet(R"(
        QSlider::groove:horizontal {
            height: 6px;
            background: #333;
            border-radius: 3px;
        }
        QSlider::sub-page:horizontal {
            background: #FF8C00;
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #EEE;
            width: 12px;
            margin-top: -3px;
            margin-bottom: -3px;
            border-radius: 6px;
        }
    )");
    ctrlLayout->addWidget(m_timeSlider);

    m_timeLabel = new QLabel("00:00 / 00:00");
    m_timeLabel->setStyleSheet("color: #AAA; font-family: 'Consolas';");
    ctrlLayout->addWidget(m_timeLabel);

    mediaLayout->addLayout(ctrlLayout);
    layout->addWidget(m_mediaContainer);

    // 状态与信息标签
    m_infoLabel = new QLabel(m_container);
    m_infoLabel->setStyleSheet("color: #777;");
    m_infoLabel->hide();
    // 2026-07-xx 按用户要求：信息栏不再占用布局空间，仅保留对象供其他函数调用
    // setText()，避免调用处出现空指针；实际不会显示在界面上。

    rootLayout->addWidget(m_container);

#ifdef FERREX_HAS_MULTIMEDIA
    // 初始化媒体播放器
    m_mediaPlayer = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_mediaPlayer->setAudioOutput(m_audioOutput);
    m_mediaPlayer->setVideoOutput(m_videoWidget);

    connect(m_mediaPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (!m_timeSlider->isSliderDown()) {
            m_timeSlider->setValue(static_cast<int>(position));
        }
        m_timeLabel->setText(QString("%1 / %2")
            .arg(formatTime(position))
            .arg(formatTime(m_mediaPlayer->duration())));
    });

    connect(m_mediaPlayer, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        m_timeSlider->setRange(0, static_cast<int>(duration));
        m_timeLabel->setText(QString("%1 / %2")
            .arg(formatTime(m_mediaPlayer->position()))
            .arg(formatTime(duration)));
    });

    connect(m_playBtn, &QPushButton::clicked, this, [this]() {
        if (m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) {
            m_mediaPlayer->pause();
            m_playBtn->setText("播放");
        } else {
            m_mediaPlayer->play();
            m_playBtn->setText("暂停");
        }
    });

    connect(m_timeSlider, &QSlider::sliderMoved, this, [this](int position) {
        m_mediaPlayer->setPosition(position);
    });

    connect(m_mediaPlayer, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error, const QString &errorString) {
        Q_UNUSED(error);
        m_infoLabel->setText(QString("播放错误: %1 (可能缺失系统编解码器)").arg(errorString));
        m_infoLabel->setStyleSheet("color: #E24B4A; font-weight: bold;");
    });
#else
    m_playBtn->setEnabled(false);
    m_timeSlider->setEnabled(false);
#endif
}

void QuickLookWindow::preview(const QString& filePath) {
    m_currentPath = filePath;
    QFileInfo fi(filePath);
    m_titleLabel->setText(fi.fileName());
    m_infoLabel->setStyleSheet("color: #777;");
    
    QString ext = fi.suffix().toLower();
    
    // 停止并重置之前的媒体播放
    resetMedia();

    if (VIDEO_EXTS.contains(ext) || AUDIO_EXTS.contains(ext)) {
        renderMedia(filePath);
    } else if (UiHelper::isGraphicsFile(ext)) {
        renderImage(filePath);
    } else if (UNPREVIEWABLE_EXTS.contains(ext)) {
        // 直接提示不支持，显示其系统图标
        m_graphicsView->hide();
        m_textEdit->hide();
        m_mediaContainer->hide();
        
        m_graphicsView->clear();
        m_textEdit->clear();
        
        QIcon fileIcon = UiHelper::getFileIcon(filePath, 256);
        QPixmap pix = fileIcon.pixmap(256, 256);
        m_graphicsView->setPixmap(pix);
        m_graphicsView->show();
        
        m_infoLabel->setText("该文件类型暂不支持预览");
        m_infoLabel->setStyleSheet("color: #FF8C00; font-weight: bold; font-size: 14px;");
    } else {
        renderText(filePath);
    }

    // 全屏显示：改用 Qt 原生 showFullScreen()，由 Qt/操作系统正确处理当前显示器的
    // DPI 缩放、多屏坐标等细节，避免手动计算 screen geometry 导致的尺寸偏差。
    showFullScreen();
    
    raise();
    activateWindow();

#ifdef Q_OS_WIN
    // 强制置顶保护
    m_ignoreDeactivate = true;
    QTimer::singleShot(150, this, [this]() {
        m_ignoreDeactivate = false;
    });
    SetWindowPos((HWND)winId(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
#endif
}

void QuickLookWindow::closePreview() {
    resetMedia();
    hide();
}

void QuickLookWindow::renderImage(const QString& path) {
    m_textEdit->hide();
    m_mediaContainer->hide();
    m_graphicsView->show();
    m_graphicsView->clear();
    m_infoLabel->setText("正在加载预览...");

    QFileInfo fi(path);
    QString ext = fi.suffix().toLower();

    // 优先读取原始像素的本地格式
    static const QSet<QString> QT_NATIVE_FORMATS = {"png", "jpg", "jpeg", "bmp", "gif", "webp"};

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
        } else if (QT_NATIVE_FORMATS.contains(ext)) {
            // Qt 原生支持的格式无损加载
            img.load(path);
        } else {
            // 非原生格式兜底
            img = UiHelper::getShellThumbnail(path, 4096);
            if (img.isNull()) {
                img.load(path);
            }
        }

        if (!weakThis) return;
        QMetaObject::invokeMethod(weakThis.data(), [weakThis, img, path]() {
            if (!weakThis || weakThis->m_currentPath != path) return;
            if (!img.isNull()) {
                qint64 totalPixels = static_cast<qint64>(img.width()) * img.height();
                bool isHuge = totalPixels > 50000000LL; // 超过 5000 万像素安全降采样
                
                QPixmap pix;
                if (isHuge) {
                    pix = QPixmap::fromImage(img.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    weakThis->m_infoLabel->setText(QString("超大图像（已应用安全限制）: %1x%2 | %3")
                        .arg(img.width()).arg(img.height()).arg(path));
                } else {
                    pix = QPixmap::fromImage(img);
                    weakThis->m_infoLabel->setText(QString("%1x%2 | %3")
                        .arg(img.width()).arg(img.height()).arg(path));
                }
                pix.setDevicePixelRatio(weakThis->devicePixelRatioF());
                weakThis->m_graphicsView->setPixmap(pix);
            } else {
                weakThis->renderText(path); // 图片加载失败尝试文本模式
            }
        });
    });
}

void QuickLookWindow::renderText(const QString& path) {
    m_graphicsView->hide();
    m_mediaContainer->hide();
    m_textEdit->show();
    m_textEdit->setPlainText("正在读取文件...");

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_textEdit->setPlainText("无法打开文件进行预览。");
        return;
    }

    QByteArray fileData = file.read(128 * 1024);
    file.close();

    bool potentialUtf16 = fileData.startsWith("\xFF\xFE") || fileData.startsWith("\xFE\xFF");
    if (!potentialUtf16 && isBinary(fileData)) {
        m_textEdit->hide();
        m_mediaContainer->hide();
        m_graphicsView->show();
        m_graphicsView->clear();
        
        QIcon fileIcon = UiHelper::getFileIcon(path, 256);
        QPixmap pix = fileIcon.pixmap(256, 256);
        m_graphicsView->setPixmap(pix);
        
        m_infoLabel->setText("二进制文件，无法直接预览文本");
        m_infoLabel->setStyleSheet("color: #FF8C00; font-weight: bold; font-size: 14px;");
        return;
    }

    QString encodingName = detectEncoding(fileData);
    QString text;

    if (encodingName == "UTF-8") {
        text = QString::fromUtf8(fileData);
    } else if (encodingName == "UTF-16LE") {
        text = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(fileData.constData()), fileData.size() / 2);
    } else if (encodingName == "UTF-16BE") {
        auto decoder = QStringDecoder(QStringDecoder::Utf16BE);
        text = decoder(fileData);
    } else {
        text = QString::fromLocal8Bit(fileData);
    }

    m_textEdit->setPlainText(text);
    m_textEdit->verticalScrollBar()->setValue(0);
    m_infoLabel->setText(QString("编码: %1 | 大小: %2 KB | %3").arg(encodingName).arg(QFileInfo(path).size() / 1024.0, 0, 'f', 1).arg(path));
}

void QuickLookWindow::renderMedia(const QString& path) {
    m_graphicsView->hide();
    m_textEdit->hide();
    m_mediaContainer->show();

    QFileInfo fi(path);
    QString ext = fi.suffix().toLower();

#ifdef FERREX_HAS_MULTIMEDIA
    if (AUDIO_EXTS.contains(ext)) {
        m_videoWidget->hide();
        m_audioPlaceholder->setText(QString("音频播放中...\n%1").arg(fi.fileName()));
        m_audioPlaceholder->show();
    } else {
        m_audioPlaceholder->hide();
        m_videoWidget->show();
    }

    m_mediaPlayer->setSource(QUrl::fromLocalFile(path));
    m_mediaPlayer->play();
    m_playBtn->setText("暂停");
    
    m_infoLabel->setText(path);
#else
    m_audioPlaceholder->setText(QString("音视频预览未启用\n%1").arg(fi.fileName()));
    m_audioPlaceholder->show();
    m_infoLabel->setText("当前系统未启用多媒体播放模块 (构建时缺少 Qt Multimedia 组件)");
    m_infoLabel->setStyleSheet("color: #FF8C00; font-weight: bold;");
#endif
}

void QuickLookWindow::resetMedia() {
#ifdef FERREX_HAS_MULTIMEDIA
    if (m_mediaPlayer) {
        m_mediaPlayer->stop();
        m_mediaPlayer->setSource(QUrl());
    }
#endif
    m_playBtn->setText("播放");
    m_timeSlider->setValue(0);
    m_timeLabel->setText("00:00 / 00:00");
}

QString QuickLookWindow::formatTime(qint64 ms) {
    qint64 secs = ms / 1000;
    qint64 mins = secs / 60;
    secs = secs % 60;
    return QString("%1:%2")
        .arg(mins, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

bool QuickLookWindow::isBinary(const QByteArray& fileData) {
    if (fileData.isEmpty()) return false;
    int checkLen = std::min<int>(fileData.size(), 1024);
    int continuousNull = 0;
    for (int i = 0; i < checkLen; ++i) {
        if (fileData[i] == '\0') {
            continuousNull++;
            if (continuousNull > 2) return true;
        } else {
            continuousNull = 0;
        }
    }
    return false;
}

QString QuickLookWindow::detectEncoding(const QByteArray& fileData) {
    if (fileData.startsWith("\xEF\xBB\xBF")) return "UTF-8";
    if (fileData.startsWith("\xFF\xFE")) return "UTF-16LE";
    if (fileData.startsWith("\xFE\xFF")) return "UTF-16BE";

    int utf8Count = 0;
    for (int i = 0; i < fileData.size() - 2; ++i) {
        unsigned char c = (unsigned char)fileData[i];
        if (c >= 0xC0 && c <= 0xDF) {
            if ((unsigned char)fileData[i+1] >= 0x80 && (unsigned char)fileData[i+1] <= 0xBF) { utf8Count++; i++; }
        } else if (c >= 0xE0 && c <= 0xEF) {
            if ((unsigned char)fileData[i+1] >= 0x80 && (unsigned char)fileData[i+1] <= 0xBF &&
                (unsigned char)fileData[i+2] >= 0x80 && (unsigned char)fileData[i+2] <= 0xBF) { utf8Count += 2; i += 2; }
        }
    }

    return (utf8Count > 0) ? "UTF-8" : "GBK";
}

void QuickLookWindow::keyPressEvent(QKeyEvent* event) {
    // 2026-07-10 新增：支持 Ctrl+W 关闭空格文件预览窗口（对应用户原话：“我期望整个应用的任何界面都必须支持Ctrl+W关闭窗口”）
    if (event->key() == Qt::Key_W && (event->modifiers() & Qt::ControlModifier)) {
        closePreview();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Escape) {
        closePreview();
        return;
    }
    if (event->key() == Qt::Key_P) {
#ifdef FERREX_HAS_MULTIMEDIA
        if (m_mediaContainer && m_mediaContainer->isVisible() && m_mediaPlayer) {
            if (m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) {
                m_mediaPlayer->pause();
                m_playBtn->setText("播放");
            } else {
                m_mediaPlayer->play();
                m_playBtn->setText("暂停");
            }
        }
#endif
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
    // 2026-07-xx 核心改进：当文本框组件获得焦点并按下空格键时，将其拦截，改为执行关闭预览逻辑
    if (watched == m_textEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space) {
            closePreview();
            return true; // 100% 拦截，阻止 QPlainTextEdit 响应并向下翻页
        }
    }

    if (event->type() == QEvent::WindowDeactivate) {
        if (m_ignoreDeactivate) {
            return true;
        }
        closePreview();
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace FERREX
