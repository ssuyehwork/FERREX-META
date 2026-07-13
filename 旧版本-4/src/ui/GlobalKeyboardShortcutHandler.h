#pragma once

#include <QObject>
#include <QKeyEvent>

namespace FERREX {

class ScanDialog;

class GlobalKeyboardShortcutHandler : public QObject {
    Q_OBJECT
public:
    explicit GlobalKeyboardShortcutHandler(ScanDialog* dialog);
    bool handleKeyPress(QKeyEvent* event);
};

} // namespace FERREX
