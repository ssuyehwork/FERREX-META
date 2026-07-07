#pragma once

#include <QFrame>
#include <QTreeView>
#include <QVBoxLayout>
#include <QPushButton>
#include <QSet>
#include <QModelIndex>
#include <QLineEdit>

namespace ArcMeta {

class CategoryModel;
class CategoryFilterProxyModel;
class DropTreeView;

/**
 * @brief 分类面板（面板一）
 * 还原旧版双树架构
 */
class CategoryPanel : public QFrame {
    Q_OBJECT

public:
    explicit CategoryPanel(QWidget* parent = nullptr);
    ~CategoryPanel() override = default;
    
    // 2026-04-12 关键修复：延迟初始化数据模型，避免主线程阻塞
    void deferredInit();

    /**
     * @brief 物理还原：设置 1px 翠绿高亮线的显隐状态
     */
    void setFocusHighlight(bool visible);

    /**
     * @brief 暴露模型以便外部（如 MainWindow）触发刷新
     */
    CategoryModel* model() const { return m_categoryModel; }

    /**
     * @brief 2026-06-xx 彻底重构：外部跳转接口，用于从内容容器反向选中侧边栏
     */
    void selectCategory(int id);

    /**
     * @brief 2026-07-xx 按照 Plan-56：通过类型选中系统项
     */
    void selectCategoryByType(const QString& type);

    /**
     * @brief 2026-06-xx 物理削峰：请求刷新侧边栏计数
     * 采用 500ms 防抖逻辑，合并高频信号风暴
     * @param fullRebuild 2026-07-xx: 是否强制重建树结构 (用于导入后即时显示新分类)
     */
    void requestRefresh(bool fullRebuild = false);

signals:
    void categorySelected(int id, const QString& name, const QString& type, const QString& path = "");
    void fileSelected(const QString& path);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onCreateCategory();
    void onCreateSubCategory();
    void onRenameCategory();
    void onDeleteCategory();
    void onClassifyToCategory();
    void onSetColor();
    void onRandomColor();
    void onSetPresetTags();
    void onTogglePin();
    void onSetPassword();
    void onClearPassword();
    void onSortByNameAsc();
    void onSortByNameDesc();
    void onSortAllByNameAsc();
    void onSortAllByNameDesc();

    // 2026-06-xx 按照用户要求：补全回收站专属操作
    void onEmptyTrash();
    void onRestoreAllFromTrash();

    // 2026-xx-xx 按照 Plan-98：搜索过滤
    void onSearchTextChanged(const QString& text);

private:
    void initUi();
    void setupContextMenu();
    
    /**
     * @brief 2026-03-xx 物理寻址辅助：递归向上查找有效的分类 ID
     */
    int getTargetCategoryId(const QModelIndex& index);

    /**
     * @brief 2026-03-xx 持久化记忆：保存与加载展开状态到 QSettings
     */
    void saveExpandedStateToSettings();
    void loadExpandedStateFromSettings();

    /**
     * @brief 递归保存 QTreeView 的展开状态
     */
    void saveExpandedState(const QModelIndex& parent, QSet<int>& expandedIds, QStringList& expandedNames);

    /**
     * @brief 递归恢复 QTreeView 的展开状态
     */
    void restoreExpandedState(const QModelIndex& parent, const QSet<int>& expandedIds, const QStringList& expandedNames);

    /**
     * @brief 2026-03-xx 安全逻辑：尝试解锁分类
     * @return 是否解锁成功
     */
    bool tryUnlockCategory(const QModelIndex& index);

    QVBoxLayout* m_mainLayout = nullptr;
    QWidget* m_focusLine = nullptr;
    
    DropTreeView* m_categoryTree = nullptr;
    CategoryModel* m_categoryModel = nullptr;
    CategoryFilterProxyModel* m_proxyModel = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QTimer* m_searchTimer = nullptr; // 2026-xx-xx 按照 Plan-106：搜索防抖计时器

    // 2026-03-xx 会话级解锁列表：存储当前已验证通过的加密分类 ID
    QSet<int> m_unlockedIds;

    // 2026-04-15 物理锁：恢复状态期间严禁反向触发保存，防止信号回流污染 Settings
    bool m_isRestoringState = false;
    bool m_isFirstLoad = true;
};

} // namespace ArcMeta
