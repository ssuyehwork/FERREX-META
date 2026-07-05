#include "AddressBar.h"
#include <QHBoxLayout>
#include <QDir>

namespace ArcMeta {

AddressBar::AddressBar(QWidget* parent) : QWidget(parent) {
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_pathStack = new QStackedWidget(this);
    m_pathStack->setFixedHeight(32); 
    m_pathStack->setMinimumWidth(300);
    m_pathStack->setStyleSheet("QStackedWidget { background: #1E1E1E; border: 1px solid #333333; border-radius: 6px; }");

    m_breadcrumbBar = new BreadcrumbBar(m_pathStack);
    m_pathStack->addWidget(m_breadcrumbBar);

    m_pathEdit = new QLineEdit(m_pathStack);
    m_pathEdit->setPlaceholderText("输入路径...");
    m_pathEdit->setFixedHeight(30); // 扣除上下各 1px 边框，确保背景不溢出圆角
    m_pathEdit->setStyleSheet("QLineEdit { background: transparent; border: none; color: #EEEEEE; padding-left: 8px; }");
    m_pathStack->addWidget(m_pathEdit);

    layout->addWidget(m_pathStack);

    connect(m_breadcrumbBar, &BreadcrumbBar::blankAreaClicked, this, &AddressBar::onBreadcrumbBlankClicked);
    connect(m_pathEdit, &QLineEdit::editingFinished, this, &AddressBar::onPathEditFinished);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        QString input = m_pathEdit->text();
        if (QDir(input).exists() || input == "computer://" || input == "此电脑") {
            emit pathChanged(input == "此电脑" ? "computer://" : input);
        } else {
            m_pathEdit->setText(QDir::toNativeSeparators(m_currentPath));
            m_pathStack->setCurrentWidget(m_breadcrumbBar);
        }
    });
    connect(m_breadcrumbBar, &BreadcrumbBar::pathClicked, this, &AddressBar::onBreadcrumbClicked);
}

void AddressBar::setPath(const QString& path) {
    m_currentPath = path;
    QString displayPath = (path == "computer://") ? "此电脑" : QDir::toNativeSeparators(path);
    m_pathEdit->setText(displayPath);
    m_breadcrumbBar->setPath(path);
    m_pathStack->setCurrentWidget(m_breadcrumbBar);
}

void AddressBar::onBreadcrumbBlankClicked() {
    m_pathEdit->setText(QDir::toNativeSeparators(m_currentPath));
    m_pathStack->setCurrentWidget(m_pathEdit);
    m_pathEdit->setFocus();
    m_pathEdit->selectAll();
}

void AddressBar::onPathEditFinished() {
    if (m_pathStack->currentWidget() == m_pathEdit) {
        m_pathStack->setCurrentWidget(m_breadcrumbBar);
    }
}

void AddressBar::onBreadcrumbClicked(const QString& path) {
    emit pathChanged(path);
}

} // namespace ArcMeta
