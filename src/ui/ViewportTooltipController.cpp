#include "ViewportTooltipController.h"
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include "ToolTipOverlay.h"
#include "IScanResultView.h"
#include <QEvent>
#include <QMouseEvent>
#include <QAbstractItemView>

namespace FERREX {

ViewportTooltipController::ViewportTooltipController(ScanDialog* dialog)
    : QObject(dialog), m_dialog(dialog)
{
    m_itemToolTipTimer = new QTimer(this);
    m_itemToolTipTimer->setSingleShot(true);
    m_itemToolTipTimer->setInterval(2000); // 2000ms delay
    connect(m_itemToolTipTimer, &QTimer::timeout, this, &ViewportTooltipController::onTooltipTimeout);
}

void ViewportTooltipController::onTooltipTimeout() {
    if (m_hoveredIndex.isValid() && m_dialog && m_dialog->m_tableModel) {
        QString tipText = m_dialog->m_tableModel->data(m_hoveredIndex, Qt::ToolTipRole).toString();
        if (!tipText.isEmpty()) {
            ToolTipOverlay::instance()->showText(m_hoveredGlobalPos, tipText, 0);
        }
    }
}

bool ViewportTooltipController::handleEvent(QObject* watched, QEvent* event) {
    if (!m_dialog || !m_dialog->m_tableModel) return false;

    // Viewport-based tooltip logic
    bool isViewOrViewport = false;
    QAbstractItemView* view = nullptr;

    for (auto* resView : {m_dialog->m_listResultView, m_dialog->m_justifiedResultView, m_dialog->m_gridResultView}) {
        if (!resView) continue;
        QAbstractItemView* base = resView->getBaseView();
        if (watched == base || watched == base->viewport()) {
            isViewOrViewport = true;
            view = base;
            break;
        }
    }

    if (isViewOrViewport && view) {
        if (event->type() == QEvent::ToolTip) {
            return true; // Suppress native tooltip bubble
        }

        if (event->type() == QEvent::MouseMove) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            QPoint viewportPos = view->viewport()->mapFromGlobal(me->globalPosition().toPoint());
            QModelIndex idx = view->indexAt(viewportPos);

            if (idx.isValid()) {
                QModelIndex col0Idx = m_dialog->m_tableModel->index(idx.row(), 0);

                m_itemToolTipTimer->stop();
                ToolTipOverlay::hideTip();

                m_hoveredIndex = col0Idx;
                m_hoveredGlobalPos = me->globalPosition().toPoint();
                m_itemToolTipTimer->start();
            } else {
                m_itemToolTipTimer->stop();
                ToolTipOverlay::hideTip();
                m_hoveredIndex = QModelIndex();
            }
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave ||
                   event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusOut) {
            m_itemToolTipTimer->stop();
            ToolTipOverlay::hideTip();
            m_hoveredIndex = QModelIndex();
        }
    }

    return false;
}

} // namespace FERREX
