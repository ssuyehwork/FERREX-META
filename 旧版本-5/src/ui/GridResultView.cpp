#include "GridResultView.h"
#include "JustifiedView.h"
#include "ThumbnailDelegate.h"
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include <QThreadPool>

namespace FERREX {

GridResultView::GridResultView(QWidget* parent) : IScanResultView(parent) {
    m_justifiedView = new JustifiedView(parent);
    m_justifiedView->setLayoutMode(JustifiedView::GridMode);
    
    auto* delegate = new ThumbnailDelegate(m_justifiedView);
    delegate->setHasThumbnailRole(Qt::UserRole + 1);
    delegate->setPathRole(Qt::UserRole + 3);
    m_justifiedView->setItemDelegate(delegate);
    
    m_justifiedView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_justifiedView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_justifiedView->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_justifiedView->setDragEnabled(true);
    m_justifiedView->setStyleSheet(
        "background-color: #1E1E1E; border: 1px solid #333; color: #D4D4D4; outline: none;"
    );
}

GridResultView::~GridResultView() {
    if (m_thumbPool) {
        m_thumbPool->clear();
    }
}

QWidget* GridResultView::getWidget() {
    return m_justifiedView;
}

QAbstractItemView* GridResultView::getBaseView() {
    return m_justifiedView;
}

void GridResultView::setModel(QAbstractItemModel* model) {
    m_justifiedView->setModel(model);
    auto* tableModel = qobject_cast<ScanTableModel*>(model);
    if (tableModel) {
        m_thumbPool = tableModel->getThumbPool();
    }
}

void GridResultView::setIconSize(int size) {
    m_justifiedView->setTargetRowHeight(size);
}

void GridResultView::refreshLayout() {
    m_justifiedView->setLayoutMode(JustifiedView::GridMode);
    m_justifiedView->doItemsLayout();
}

} // namespace FERREX
