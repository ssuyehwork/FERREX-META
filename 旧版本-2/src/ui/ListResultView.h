#pragma once
#include "IScanResultView.h"

class QTableView;

namespace FERREX {

class ListResultView : public IScanResultView {
    Q_OBJECT
public:
    explicit ListResultView(QWidget* parent = nullptr);
    ~ListResultView() override;

    QWidget* getWidget() override;
    QAbstractItemView* getBaseView() override;
    void setModel(QAbstractItemModel* model) override;
    void setIconSize(int size) override;
    void refreshLayout() override;

private:
    QTableView* m_tableView = nullptr;
};

} // namespace FERREX
