#include "BreadcrumbBar.h"
#include <QMouseEvent>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include "UiHelper.h"

namespace ArcMeta {

BreadcrumbBar::BreadcrumbBar(QWidget* parent) : QWidget(parent) {
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(8, 0, 8, 0);
    m_layout->setSpacing(2);
    
    setCursor(Qt::PointingHandCursor);
    // 基础样式：作为地址栏背景
    setStyleSheet("QWidget { background: transparent; border: none; }");
}

void BreadcrumbBar::setPath(const QString& path) {
    m_currentPath = path;
    clearButtons();

    if (path == "computer://") {
        addLevel("此电脑", "computer://");
        m_layout->addStretch();
        return;
    }

    QString normPath = QDir::toNativeSeparators(path);
    QStringList parts = normPath.split(QDir::separator(), Qt::SkipEmptyParts);

    // 处理 Windows 盘符根目录 (如 C:\)
    QString currentBuildPath;
    if (normPath.contains(":") && normPath.indexOf(":") == 1) {
        QString drive = normPath.left(3); // "C:\"
        addLevel(drive, drive);
        currentBuildPath = drive;
        
        // 如果路径只是根目录，parts 可能只有盘符或为空
        if (parts.size() > 0 && parts[0].contains(":")) {
             parts.removeFirst(); 
        }
    }

    for (qsizetype i = 0; i < parts.size(); ++i) {
        // 添加箭头/分隔符
        QLabel* sep = new QLabel(">", this);
        sep->setStyleSheet("color: #555; font-size: 10px; padding: 0 2px;");
        m_layout->addWidget(sep);

        if (!currentBuildPath.endsWith(QDir::separator())) {
            currentBuildPath += QDir::separator();
        }
        currentBuildPath += parts[i];
        
        addLevel(parts[i], currentBuildPath);
    }

    m_layout->addStretch();
}

void BreadcrumbBar::clearButtons() {
    QLayoutItem* item;
    while ((item = m_layout->takeAt(0))) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
}

void BreadcrumbBar::addLevel(const QString& name, const QString& fullPath) {
    QPushButton* btn = new QPushButton(name, this);
    btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    btn->setFixedHeight(24);
    
    // 面包屑按钮样式：扁平化，仅悬停可见背景
    btn->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 6px; "
        "              color: #EEEEEE; font-size: 12px; padding: 0 6px; }"
        "QPushButton:hover { background: rgba(255, 255, 255, 0.1); }"
        "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
    );

    connect(btn, &QPushButton::clicked, [this, fullPath]() {
        emit pathClicked(fullPath);
    });

    m_layout->addWidget(btn);
}

void BreadcrumbBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // 改进：点击按钮以外的任何地方（包括分隔符和空白区）都触发编辑模式
        QWidget* child = childAt(event->pos());
        if (!qobject_cast<QPushButton*>(child)) {
            emit blankAreaClicked();
        }
    }
    QWidget::mousePressEvent(event);
}

} // namespace ArcMeta
