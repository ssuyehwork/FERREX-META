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

    virtual QWidget* getWidget() = 0; 

    
    virtual QAbstractItemView* getBaseView() = 0; 

    virtual void setModel(QAbstractItemModel* model) = 0; 

    virtual void setIconSize(int size) = 0; 
    virtual void refreshLayout() = 0; 
}; 
 
} 
