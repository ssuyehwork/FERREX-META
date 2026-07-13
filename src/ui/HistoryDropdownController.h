#pragma once

#include <QObject>
#include <QLineEdit>

namespace FERREX {

class ScanDialog;

class HistoryDropdownController : public QObject {
    Q_OBJECT
public:
    explicit HistoryDropdownController(ScanDialog* dialog);
    bool showDropdown(QLineEdit* edit, bool isQuery);
};

} // namespace FERREX
