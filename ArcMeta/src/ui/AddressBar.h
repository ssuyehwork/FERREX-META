#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QStackedWidget>
#include <QEvent>
#include "BreadcrumbBar.h"
#include "AddressHistoryPanel.h"

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

    /**
     * @brief 2026-06-xx 按照用户要求：持久化路径历史
     */
    void saveToHistory(const QString& path);

signals:
    void pathChanged(const QString& path);
    void refreshRequested();

private slots:
    void onBreadcrumbBlankClicked();
    void onPathEditFinished();
    void onBreadcrumbClicked(const QString& path);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QWidget*        m_addressContainer = nullptr;
    QStackedWidget* m_pathStack = nullptr;
    BreadcrumbBar*  m_breadcrumbBar = nullptr;
    QLineEdit*      m_pathEdit = nullptr;
    QPushButton*    m_btnRefresh = nullptr;
    AddressHistoryPanel* m_historyPanel = nullptr;
    QString         m_currentPath;
};

} // namespace ArcMeta
