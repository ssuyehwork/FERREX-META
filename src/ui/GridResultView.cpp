#include "GridResultView.h"
#include "ThumbnailDelegate.h"
#include <QThreadPool>

namespace FERREX {

GridResultView::GridResultView(QWidget* parent)
    : IScanResultView(parent)
{
    m_justifiedView = new JustifiedView(parent);
    m_justifiedView->setLayoutMode(JustifiedView::GridMode);
    
    m_justifiedView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_justifiedView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_justifiedView->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_justifiedView->setDragEnabled(true);

    // 转发信号
    connect(m_justifiedView, &QListView::doubleClicked, this, &IScanResultView::itemDoubleClicked);
    connect(m_justifiedView, &QListView::customContextMenuRequested, this, &IScanResultView::customContextMenuRequested);
    
    // 监听选择改变信号
    connect(m_justifiedView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection& selected, const QItemSelection& deselected) {
        emit selectionChanged(selected, deselected);
    });
}

GridResultView::~GridResultView() {
    // 线程安全：切换或关闭窗口时，析构必须清理后台未处理的缩略图线程池任务
    auto* model = m_justifiedView ? m_justifiedView->model() : nullptr;
    if (model) {
        // 由于 ScanTableModel 的线程池在 model 里，可以通过调用清理方法等操作保护
        // 我们在 ScanTableModel 的析构中对 m_thumbPool 调用了 waitForDone 且 processThumbQueue 有安全机制，
        // 且主 ScanTableModel 析构时会正确处理所有正在运行的任务。
    }
}

QWidget* GridResultView::getWidget() {
    return m_justifiedView;
}

void GridResultView::setModel(QAbstractItemModel* model) {
    m_justifiedView->setModel(model);
    
    disconnect(m_justifiedView->selectionModel(), &QItemSelectionModel::selectionChanged, nullptr, nullptr);
    connect(m_justifiedView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this](const QItemSelection& selected, const QItemSelection& deselected) {
        emit selectionChanged(selected, deselected);
    });
}

void GridResultView::selectRows(const QItemSelection& selection) {
    if (m_justifiedView->selectionModel()) {
        m_justifiedView->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
}

void GridResultView::clearSelection() {
    if (m_justifiedView->selectionModel()) {
        m_justifiedView->selectionModel()->clearSelection();
    }
}

void GridResultView::refreshLayout() {
    m_justifiedView->doItemsLayout();
}

void GridResultView::forceFetchAllResults() {
    auto* model = m_justifiedView->model();
    if (model) {
        QMetaObject::invokeMethod(model, "forceFetchAll");
    }
}

} // namespace FERREX
