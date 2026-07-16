#include "ViewportTooltipController.h"
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include "ToolTipOverlay.h"
#include "IScanResultView.h"
#include <QEvent>
#include <QCoreApplication>
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
            QMetaObject::invokeMethod(QCoreApplication::instance(), [globalPos = m_hoveredGlobalPos, tipText]() {
                ToolTipOverlay::instance()->showText(globalPos, tipText, 0);
            }, Qt::QueuedConnection);
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
            QPoint currentGlobalPos = me->globalPosition().toPoint();

            // 坐标防抖：光标位移变化累计不超过 5 像素时不进行重复事件处理与提示框高频显隐，消灭闪烁
            int dx = std::abs(currentGlobalPos.x() - m_hoveredGlobalPos.x());
            int dy = std::abs(currentGlobalPos.y() - m_hoveredGlobalPos.y());

            QPoint viewportPos = view->viewport()->mapFromGlobal(currentGlobalPos);
            QModelIndex idx = view->indexAt(viewportPos);

            if (idx.isValid()) {
                QModelIndex col0Idx = m_dialog->m_tableModel->index(idx.row(), 0);
                
                if (col0Idx == m_hoveredIndex && dx < 5 && dy < 5) {
                    return false; // 防抖直接拦截返回，消灭频繁触发 hideTip() 的闪烁现象
                }

                m_itemToolTipTimer->stop();
                QMetaObject::invokeMethod(QCoreApplication::instance(), []() {
                    ToolTipOverlay::hideTip();
                }, Qt::QueuedConnection);

                m_hoveredIndex = col0Idx;
                m_hoveredGlobalPos = currentGlobalPos;
                m_itemToolTipTimer->start();
            } else {
                m_itemToolTipTimer->stop();
                QMetaObject::invokeMethod(QCoreApplication::instance(), []() {
                    ToolTipOverlay::hideTip();
                }, Qt::QueuedConnection);
                m_hoveredIndex = QModelIndex();
            }
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave ||
                   event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusOut) {
            m_itemToolTipTimer->stop();
            QMetaObject::invokeMethod(QCoreApplication::instance(), []() {
                ToolTipOverlay::hideTip();
            }, Qt::QueuedConnection);
            m_hoveredIndex = QModelIndex();
        }
    }

    return false;
}

} // namespace FERREX
