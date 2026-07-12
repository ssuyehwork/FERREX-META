#include "ListResultView.h"
#include <QScrollBar>

namespace FERREX {

ListResultView::ListResultView(QWidget* parent)
    : IScanResultView(parent)
{
    m_tableView = new QTableView(parent);
    
    // 设置一些基本属性
    m_tableView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tableView->setShowGrid(false);
    m_tableView->setAlternatingRowColors(true);
    m_tableView->verticalHeader()->setVisible(false);

    // 转发信号
    connect(m_tableView, &QTableView::doubleClicked, this, &IScanResultView::itemDoubleClicked);
    connect(m_tableView, &QTableView::customContextMenuRequested, this, &IScanResultView::customContextMenuRequested);
    
    // 监听选择改变信号
    connect(m_tableView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection& selected, const QItemSelection& deselected) {
        emit selectionChanged(selected, deselected);
    });
}

QWidget* ListResultView::getWidget() {
    return m_tableView;
}

void ListResultView::setModel(QAbstractItemModel* model) {
    m_tableView->setModel(model);
    
    // 每次重新绑定 Model，QItemSelectionModel 会被重新创建，我们需要重新绑定 selectionChanged 信号
    disconnect(m_tableView->selectionModel(), &QItemSelectionModel::selectionChanged, nullptr, nullptr);
    connect(m_tableView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection& selected, const QItemSelection& deselected) {
        emit selectionChanged(selected, deselected);
    });
}

void ListResultView::selectRows(const QItemSelection& selection) {
    if (m_tableView->selectionModel()) {
        m_tableView->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
}

void ListResultView::clearSelection() {
    if (m_tableView->selectionModel()) {
        m_tableView->selectionModel()->clearSelection();
    }
}

void ListResultView::refreshLayout() {
    m_tableView->viewport()->update();
}

void ListResultView::forceFetchAllResults() {
    // 调用模型的 forceFetchAll
    auto* model = m_tableView->model();
    if (model) {
        QMetaObject::invokeMethod(model, "forceFetchAll");
    }
}

} // namespace FERREX
