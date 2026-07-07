#pragma once

#include <QSortFilterProxyModel>
#include "../core/ModelContract.h"

namespace ArcMeta {

/**
 * @brief 分类递归过滤代理模型
 * 2026-xx-xx 按照 Plan-98：实现“我的分类”专项递归过滤
 */
class CategoryFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit CategoryFilterProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {
        setRecursiveFilteringEnabled(false); 
    }

    void setFilterText(const QString& text) {
        m_filterText = text;
        beginFilterChange();
        endFilterChange();
    }

    QString filterText() const { return m_filterText; }

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override {
        if (m_filterText.isEmpty()) return true;

        QModelIndex index = sourceModel()->index(source_row, 0, source_parent);
        int id = index.data(IdRole).toInt();
        QString name = index.data(NameRole).toString();
        
        // 1. 系统项（ID < 0）始终可见，不参与过滤
        if (id < 0) return true;

        // 2. 根容器处理
        if (name == "快速访问" || name == "我的分类") {
            return hasMatchingChild(index);
        }

        // 3. “我的分类”子树逻辑
        if (name.contains(m_filterText, Qt::CaseInsensitive)) return true;

        // 4. 递归检查子项
        return hasMatchingChild(index);
    }

private:
    bool hasMatchingChild(const QModelIndex& parent) const {
        int rowCount = sourceModel()->rowCount(parent);
        for (int i = 0; i < rowCount; ++i) {
            QModelIndex child = sourceModel()->index(i, 0, parent);
            QString name = child.data(NameRole).toString();
            if (name.contains(m_filterText, Qt::CaseInsensitive)) return true;
            if (hasMatchingChild(child)) return true;
        }
        return false;
    }

    QString m_filterText;
};

} // namespace ArcMeta
