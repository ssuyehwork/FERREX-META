#pragma once

#include <QObject>
#include <QTimer>
#include <QModelIndex>
#include <QPoint>

namespace FERREX {

class ScanDialog;

class ViewportTooltipController : public QObject {
    Q_OBJECT
public:
    explicit ViewportTooltipController(ScanDialog* dialog);
    bool handleEvent(QObject* watched, QEvent* event);

private slots:
    void onTooltipTimeout();

private:
    ScanDialog* m_dialog;
    QTimer* m_itemToolTipTimer = nullptr;
    QModelIndex m_hoveredIndex;
    QPoint m_hoveredGlobalPos;
};

} // namespace FERREX
