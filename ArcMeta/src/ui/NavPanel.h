#pragma once

#include <QWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QVBoxLayout>
#include <QDir>
#include <QSplitter>

namespace ArcMeta {

class DropTreeView;

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

signals:
    /**
     * @brief 当用户点击目录时发出信号
     * @param path 目标目录完整路径
     */
    void directorySelected(const QString& path);

    /**
     * @brief 请求在内容面板中定位并选中某个文件
     * @param path 文件完整路径
     */
    void requestLocateFile(const QString& path);

private slots:
    void onItemExpanded(const QModelIndex& index);
    void onTreeClicked(const QModelIndex& index);
    void onFavoriteClicked(const QModelIndex& index);
    void onFavoriteContextMenu(const QPoint& pos);
    void onPathsDroppedToFavorite(const QStringList& paths, const QModelIndex& target);
    void updateTreeHeight();
    void updateFavoriteHeight();

private:
    void initUi();
    void fetchChildDirs(QStandardItem* parent);
    
    // 收藏夹持久化
    void loadFavorites();
    void saveFavorites();
    void addFavoriteItem(const QString& path);

    QWidget* buildGroup(const QString& title, const QIcon& icon, const QColor& color, QVBoxLayout*& outContentLayout);

    QSplitter* m_splitter = nullptr;
    
    // 上方：磁盘树
    QTreeView* m_treeView = nullptr;
    QStandardItemModel* m_model = nullptr;

    // 下方：收藏夹
    DropTreeView* m_favoriteView = nullptr;
    QStandardItemModel* m_favoriteModel = nullptr;

    QVBoxLayout* m_mainLayout = nullptr;
    QWidget* m_focusLine = nullptr;
};

} // namespace ArcMeta
