#pragma once

#include <QStandardItemModel>
#include <QSet>
#include "../core/ModelContract.h"

namespace ArcMeta {

class CategoryModel : public QStandardItemModel {
    Q_OBJECT
public:
    enum Type { System, User, Both };
    explicit CategoryModel(Type type, QObject* parent = nullptr);

    void setUnlockedIds(const QSet<int>& ids);

    // 2026-04-12 关键修复：延迟刷新接口声明
    void deferredRefresh();

    /**
     * @brief 2026-05-27 物理修复：展开时动态加载子项，防止启动卡死
     */
    void loadCategoryItems(const QModelIndex& parentIndex);

public slots:
    // 2026-05-27 物理修复：refresh 改为异步逻辑，不再直接执行数据库
    void refresh();

    /**
     * @brief 2026-06-xx 物理优化：执行局部统计更新，杜绝全量重置
     * @param sysCounts 系统项计数映射
     * @param catCounts 用户分类项计数映射
     */
    void updateStatistics(const QMap<QString, int>& sysCounts, const QMap<int, int>& catCounts);
    void updateSystemCounts();

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& val, int role = Qt::EditRole) override;

    Qt::DropActions supportedDropActions() const override;
    bool dropMimeData(const QMimeData* mimeData, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;

private:
    Type m_type;
    QSet<int> m_unlockedIds;
    bool m_isFirstLoad = true;
};

} // namespace ArcMeta
