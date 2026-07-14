#pragma once

#include <QObject>
#include <QEvent>

namespace FERREX {

class HoverEventFilter : public QObject {
    Q_OBJECT

public:
    explicit HoverEventFilter(QObject* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} 
