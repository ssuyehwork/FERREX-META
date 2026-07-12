#pragma once
#include "IScanResultView.h"
#include <QTableView>
#include <QHeaderView>

namespace FERREX {

class ListResultView : public IScanResultView {
    Q_OBJECT
public:
    explicit ListResultView(QWidget* parent = nullptr);
    ~ListResultView() override = default;

    QWidget* getWidget() override;
    void setModel(QAbstractItemModel* model) override;
    void selectRows(const QItemSelection& selection) override;
    void clearSelection() override;
    void refreshLayout() override;
    void forceFetchAllResults() override;

    QTableView* getTableView() const { return m_tableView; }

private:
    QTableView* m_tableView = nullptr;
};

} // namespace FERREX
