#pragma once 
#include <QWidget> 
#include <QAbstractItemView> 
#include <QItemSelection> 
#include <QAbstractItemModel> 
 
namespace FERREX { 
 
class IScanResultView : public QObject { 
    Q_OBJECT 
public: 
    explicit IScanResultView(QWidget* parent = nullptr) : QObject(parent) {}
    virtual ~IScanResultView() = default; 
 
    // 获取宿主物理外层 QWidget 容器控件（用于加载至 QStackedWidget） 
    virtual QWidget* getWidget() = 0; 
 
    // 获取关联的 Qt 抽象视图基类指针 
    // 这允许外界直接将其作为 QAbstractItemView* 挂接事件过滤、FontMetrics 等，免除脑补 
    virtual QAbstractItemView* getBaseView() = 0; 
 
    // 绑定数据源模型 
    virtual void setModel(QAbstractItemModel* model) = 0; 
 
    // 刷新排版与动态图标大小 
    virtual void setIconSize(int size) = 0; 
    virtual void refreshLayout() = 0; 
}; 
 
} // namespace FERREX 
