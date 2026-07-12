#pragma once
#include <QObject>
#include <QWidget>
#include <QModelIndex>
#include <QPoint>
#include <QItemSelection>
#include <QAbstractItemModel>

namespace FERREX {

class IScanResultView : public QObject {
    Q_OBJECT
public:
    explicit IScanResultView(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~IScanResultView() = default;

    // 核心接口：获取该视图的宿主物理控件（用于在 QStackedWidget 中加载）
    virtual QWidget* getWidget() = 0;

    // 绑定数据源模型
    virtual void setModel(QAbstractItemModel* model) = 0;

    // 将外部的选择事件、元数据同步、视图局部重绘完全委派给具体子类
    virtual void selectRows(const QItemSelection& selection) = 0;
    virtual void clearSelection() = 0;
    virtual void refreshLayout() = 0;
    virtual void forceFetchAllResults() = 0;

signals:
    // 转发共享的用户双击打开事件、自定义上下文菜单请求、选中项变化事件
    void itemDoubleClicked(const QModelIndex& index);
    void customContextMenuRequested(const QPoint& pos);
    void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected);
};

} // namespace FERREX
