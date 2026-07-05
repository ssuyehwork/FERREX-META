#pragma once

#include <QWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QVBoxLayout>
#include <QDir>

namespace ArcMeta {

/**
 * @brief 导航面板（面板二）
 * 使用 QTreeView + QFileSystemModel 实现文件夹树导航
 */
class NavPanel : public QFrame {
    Q_OBJECT

public:
    explicit NavPanel(QWidget* parent = nullptr);
    ~NavPanel() override = default;

    // 2026-04-12 关键修复：延迟初始化数据模型
    void deferredInit();

    /**
     * @brief 物理还原：设置 1px 翠绿高亮线的显隐状态
     */
    void setFocusHighlight(bool visible);

    /**
     * @brief 设置并跳转到指定目录
     * @param path 完整路径
     */
    void setRootPath(const QString& path);

    /**
     * @brief 在树中选中指定路径对应的项
     */
    void selectPath(const QString& path);

private slots:
    void onItemExpanded(const QModelIndex& index);

signals:
    /**
     * @brief 当用户点击目录时发出信号
     * @param path 目标目录完整路径
     */
    void directorySelected(const QString& path);

private:
    void initUi();
    void fetchChildDirs(QStandardItem* parent);
    
    QTreeView* m_treeView = nullptr;
    QStandardItemModel* m_model = nullptr;
    QVBoxLayout* m_mainLayout = nullptr;
    QWidget* m_focusLine = nullptr;

private slots:
    void onTreeClicked(const QModelIndex& index);
};

} // namespace ArcMeta
