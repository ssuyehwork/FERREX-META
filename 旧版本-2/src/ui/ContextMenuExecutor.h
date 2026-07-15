#pragma once

#include <QObject>
#include <QPoint>

namespace FERREX {

class ScanDialog;

class ContextMenuExecutor : public QObject {
    Q_OBJECT
public:
    explicit ContextMenuExecutor(ScanDialog* dialog);
    void executeContextMenu(const QPoint& pos);
};

} // namespace FERREX
