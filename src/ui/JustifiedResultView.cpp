#include "JustifiedResultView.h"
#include "JustifiedView.h"
#include "ThumbnailDelegate.h"
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include <QThreadPool>

namespace FERREX {

JustifiedResultView::JustifiedResultView(QWidget* parent) : IScanResultView(parent) {
    m_justifiedView = new JustifiedView(parent);
    m_justifiedView->setLayoutMode(JustifiedView::JustifiedMode);
    
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

JustifiedResultView::~JustifiedResultView() {
    if (m_thumbPool) {
        m_thumbPool->clear();
    }
}

QWidget* JustifiedResultView::getWidget() {
    return m_justifiedView;
}

QAbstractItemView* JustifiedResultView::getBaseView() {
    return m_justifiedView;
}

void JustifiedResultView::setModel(QAbstractItemModel* model) {
    m_justifiedView->setModel(model);
    auto* tableModel = qobject_cast<ScanTableModel*>(model);
    if (tableModel) {
        m_thumbPool = tableModel->getThumbPool();
    }
}

void JustifiedResultView::setIconSize(int size) {
    m_justifiedView->setTargetRowHeight(size);
}

void JustifiedResultView::refreshLayout() {
    m_justifiedView->setLayoutMode(JustifiedView::JustifiedMode);
    m_justifiedView->doItemsLayout();
}

} // namespace FERREX
