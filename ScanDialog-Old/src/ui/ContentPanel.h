#pragma once

#include <QDateTime>
#include <QMap>
#include <deque>
#include <vector>
#include <QCache>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QListView>
#include <QTreeView>
#include <QStackedWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QVBoxLayout>
#include <QStyledItemDelegate>
#include <QPersistentModelIndex>
#include <QDebug>
#include <QIcon>
#include "FilterPanel.h"
#include "../meta/MetadataManager.h"
#include "../db/ItemRepo.h"
#include "../core/ModelContract.h"

namespace ArcMeta {

/**
 * @brief 内部代理类：专门处理高级筛选逻辑 (2026-05-25 物理化以修复 static_cast 编译报错)
 */
class FilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit FilterProxyModel(QObject* parent = nullptr);

    FilterState currentFilter;
    QString m_searchQuery;

    void updateFilter();
    void setSearchQuery(const QString& query);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const override;
};

/**
 * @brief 虚拟化数据库模型：支持百万级条目瞬时加载 (2026-06-xx 重构)
 */
class FerrexVirtualDbModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit FerrexVirtualDbModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    // 虚拟化加载
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    void setRecords(const std::vector<ArcMeta::ItemRepo::ItemRecord>& records);
    void clear();

    const std::vector<ArcMeta::ItemRepo::ItemRecord>& allRecords() const { return m_allRecords; }

private:
    std::vector<ArcMeta::ItemRepo::ItemRecord> m_allRecords;
    int m_displayCount = 0;

    mutable QCache<QString, QIcon> m_iconCache;
    mutable QSet<QString> m_requestedIcons;
    mutable QMap<QString, double> m_aspectRatios;
    mutable QCache<QString, ArcMeta::RuntimeMeta> m_metaCache;
};


/**
 * @brief 内容面板（面板四）：核心业务展示区
 * 支持网格视图（QListView）与列表视图（QTreeView）切换
 */
class ContentPanel : public QFrame {
    Q_OBJECT

public:
    /**
     * @brief 内部传输结构，用于异步扫描结果
     */
    struct ScanItemData {
        QString name;
        QString fullPath;
        bool isDir;
        QString suffix;
        qint64 size;
        QDateTime mtime;
        RuntimeMeta meta;
        bool isEmpty = false;
    };

    /**
     * @brief 统计结构
     */
    struct ScanStats {
        QMap<int, int> ratingCounts;
        QMap<QString, int> colorCounts;
        QMap<QString, int> tagCounts;
        QMap<QString, int> typeCounts;
        QMap<QString, int> createDateCounts;
        QMap<QString, int> modifyDateCounts;
        int noTagCount = 0;
    };

    enum ViewMode {
        GridView,
        ListView
    };

    /**
     * @brief 右键菜单动作枚举 (2026-06-01 按照用户要求：取代弱类型字符串匹配)
     */
    enum ContextAction {
        ActionOpen,
        ActionOpenDefault,
        ActionShowInExplorer,
        ActionNewFolder,
        ActionNewMd,
        ActionNewTxt,
        ActionCategorize,
        ActionPin,
        ActionUnpin,
        ActionColorTag,
        ActionEncrypt,
        ActionDecrypt,
        ActionChangePwd,
        ActionBatchRename,
        ActionRename,
        ActionCopy,
        ActionCut,
        ActionPaste,
        ActionDelete,
        ActionPermanentDelete,
        ActionSecureDelete,
        ActionRestore,
        ActionCopyPath,
        ActionProperties,
        ActionExtractColor
    };

    explicit ContentPanel(QWidget* parent = nullptr);
    ~ContentPanel() override = default;

    // 2026-04-12 关键修复：延迟初始化
    void deferredInit();


    /**
     * @brief 切换视图模式
     */
    void setViewMode(ViewMode mode);

    /**
     * @brief 拦截空格键（红线：物理拦截 QEvent::KeyPress 且为 Key_Space）
     */
    bool eventFilter(QObject* obj, QEvent* event) override;

    // --- 业务接口 ---
    QAbstractItemModel* model() const { return m_model; }
    QSortFilterProxyModel* getProxyModel() const { return m_proxyModel; }
    QModelIndexList getSelectedIndexes() const {
        return (m_viewStack->currentWidget() == m_gridView) ? 
                m_gridView->selectionModel()->selectedIndexes() : 
                m_treeView->selectionModel()->selectedIndexes();
    }

    /**
     * @brief 物理定位：在当前视图模型中寻找与 currentPath 相邻的文件路径
     * @param delta 偏移方向 (-1 为上一个, 1 为下一个)
     */
    QString getAdjacentFilePath(const QString& currentPath, int delta);

signals:
    /**
     * @brief 请求 QuickLook 预览信号
     * @param path 物理路径
     */
    void requestQuickLook(const QString& path);

    /**
     * @brief 选中项发生变化时通知元数据面板刷新
     * @param paths 选中条目的物理路径列表
     */
    void selectionChanged(const QStringList& paths);
    void directorySelected(const QString& path);

    /**
     * @brief 数据源变更信号，用于焦点线管理
     * @param source 数据源标识
     */
    void dataSourceChanged(const QString& source);

    /**
     * @brief 目录装载完成后发出，携带统计数据供 FilterPanel 填充
     */
    void directoryStatsReady(
        const QMap<int, int>&     ratingCounts,
        const QMap<QString, int>& colorCounts,
        const QMap<QString, int>& tagCounts,
        const QMap<QString, int>& typeCounts,
        const QMap<QString, int>& createDateCounts,
        const QMap<QString, int>& modifyDateCounts);

private:
    void initUi();
    void initGridView();
    void initListView();
    void setupContextMenu();
    void updateLayersButtonState();

    /**
     * @brief 内部业务辅助逻辑
     */
    void performCopy(bool cutMode);
    void performPaste();
    void performBatchRename();

    QVBoxLayout* m_mainLayout = nullptr;
    QStackedWidget* m_viewStack = nullptr;
    QPushButton* m_btnLayers = nullptr;
    QTextBrowser* m_textPreview = nullptr;
    QLabel* m_imagePreview = nullptr;

    // 视图组件
    QAbstractItemView* m_gridView = nullptr;
    QTreeView* m_treeView = nullptr;
    FerrexVirtualDbModel* m_model = nullptr;
    QSortFilterProxyModel* m_proxyModel = nullptr;


    FilterState m_currentFilter;

    int m_zoomLevel = 64;
    QString m_currentPath;
    QString m_currentCategoryType; // 用于驱动差异化右键菜单
    bool m_isRecursive = false;
    bool m_isLoading = false; // 2026-06-16 物理状态锁：防止加载数据时的布局抖动覆盖用户配置
    void updateGridSize();
    void updateStatusBarStats();
    void recalculateAndEmitStats();

    void addItemsFromDirectory(const QString& path, bool recursive,
                               QMap<int, int>& ratingCounts,
                               QMap<QString, int>& colorCounts,
                               QMap<QString, int>& tagCounts,
                               QMap<QString, int>& typeCounts,
                               QMap<QString, int>& createDateCounts,
                               QMap<QString, int>& modifyDateCounts,
                               int& noTagCount);

public slots:
    void onSelectionChanged();
    void onCustomContextMenuRequested(const QPoint& pos);
    void onDoubleClicked(const QModelIndex& index);

    /**
     * @brief 加载并显示目录内容
     */
    void loadDirectory(const QString& path, bool recursive = false);

    /**
     * @brief 全局/本地搜索
     */
    void search(const QString& query);

    /**
     * @brief 应用当前筛选器
     */
    void applyFilters(const FilterState& state);
    void applyFilters(); // 使用保存的状态重新应用

    /**
     * @brief 创建新条目（文件夹/Markdown/Txt）
     */
    void createNewItem(const QString& type);

    /**
     * @brief 预览文件内容 (支持文本、Markdown、图片等)
     */
    void previewFile(const QString& path);

    /**
     * @brief 加载指定路径列表 (分类联动使用)
     */
    void loadPaths(const QStringList& paths);

    /**
     * @brief 2026-06-xx 彻底重构：加载分类及其子项 (分类 ID 联动)
     */
    void loadCategory(int categoryId);

    /**
     * @brief 设置当前分类类型，用于驱动右键菜单差异化
     */
    void setCurrentCategoryType(const QString& type) { m_currentCategoryType = type; }

signals:
    /**
     * @brief 当在内容区点击子分类时触发，告知 MainWindow 切换侧边栏选中状态
     */
    void categoryClicked(int categoryId);

    /**
     * @brief 状态栏统计信息信号
     * @param fileCount 文件数量
     * @param folderCount 文件夹数量
     * @param totalCount 总项目数量
     */
    void statusBarStatsUpdated(int fileCount, int folderCount, int totalCount);


protected:
    void wheelEvent(QWheelEvent* event) override;
};

/**
 * @brief 自定义 Delegate：处理网格视图下的图标、星级、颜色圆点及角标叠加
 */
class GridItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    struct GridMetrics {
        QRect cardRect;      // 整个条目占用的总区域
        QRect squareRect;    // 正方形背景区域（包含图标、评级、角标）
        int iconDrawSize;
        int ratingH;
        int nameH;
        int gap1;            // 图标与评级之间的间距
        int gap2;            // 正方形区与名称之间的间距
        int totalH;
        int startY;
        QRect iconRect;
        int ratingY;
        int infoTotalW;
        int infoStartX;
        QRect banRect;
        int starsStartX;
        int starSize;
        int starSpacing;
        int nameY;
        QRect nameRect;
    };

    static GridMetrics calculateMetrics(const QStyleOptionViewItem& option);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

} // namespace ArcMeta
