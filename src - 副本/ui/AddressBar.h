#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QStackedWidget>
#include "BreadcrumbBar.h"

namespace ArcMeta {

/**
 * @brief 复合地址栏控件
 * 内部封装面包屑导航与路径输入框，并自动处理状态切换
 */
class AddressBar : public QWidget {
    Q_OBJECT

public:
    explicit AddressBar(QWidget* parent = nullptr);
    ~AddressBar() override = default;

    void setPath(const QString& path);
    QString currentPath() const { return m_currentPath; }

signals:
    void pathChanged(const QString& path);

private slots:
    void onBreadcrumbBlankClicked();
    void onPathEditFinished();
    void onBreadcrumbClicked(const QString& path);

private:
    QStackedWidget* m_pathStack = nullptr;
    BreadcrumbBar*  m_breadcrumbBar = nullptr;
    QLineEdit*      m_pathEdit = nullptr;
    QString         m_currentPath;
};

} // namespace ArcMeta
