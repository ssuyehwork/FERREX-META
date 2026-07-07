#ifndef ARCMETA_TAG_MANAGER_VIEW_H
#define ARCMETA_TAG_MANAGER_VIEW_H

#include <QWidget>
#include <QMap>
#include <QStringList>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMenu>

namespace ArcMeta {

/**
 * @brief 标签管理专属视图
 * 按照 3 栏布局要求实现，逻辑独立于 NavPanel 和 ContentPanel
 */
class TagManagerView : public QWidget {
    Q_OBJECT
public:
    explicit TagManagerView(QWidget* parent = nullptr);

    /**
     * @brief 刷新标签数据并重建 UI
     */
    void refresh();

    /**
     * @brief 对标签进行实时搜索/筛选
     */
    void search(const QString& keyword);

    /**
     * @brief [TODO实现] 创建新的标签组
     */
    void createNewGroup();

    void addTagToGroup(const QString& tagName, int groupId);
    void removeTagFromGroup(const QString& tagName, int groupId = -1);
    void renameGroup(int groupId, const QString& newName);
    void deleteGroup(int groupId);

    /**
     * @brief 动态调整流式布局容器高度
     */
    void adjustFlowHeights();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    /**
     * @brief 请求搜索含此标签的项目
     */
    void requestSearchTag(const QString& tag);

private:
    void initUi();
    void setupSidebar();
    void setupContentArea();
    
    // 侧边栏组件
    class QFrame* m_sidebar = nullptr;
    QVBoxLayout* m_sidebarLayout = nullptr;
    QWidget* m_groupContainer = nullptr;
    QLabel* m_allTagsCountLabel = nullptr;
    QLabel* m_uncategorizedTagsCountLabel = nullptr;
    QLabel* m_frequentTagsCountLabel = nullptr;
    
    // 内容区组件
    class QFrame* m_contentContainer = nullptr;
    QLabel* m_contentTitleLabel = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_contentWidget = nullptr;
    
    QSplitter* m_splitter = nullptr;
    
    // 数据
    QMap<QString, int> m_tagCounts;
    
    struct TagGroup {
        int id;
        QString name;
        QString color;
        QStringList tags;
    };
    QList<TagGroup> m_tagGroups;

    QWidget* createSidebarItem(const QString& icon, const QString& name, const QString& countText, QLabel** outCountLabel = nullptr);
};

} // namespace ArcMeta

#endif // ARCMETA_TAG_MANAGER_VIEW_H
