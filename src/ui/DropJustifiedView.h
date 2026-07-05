#pragma once

#include "JustifiedView.h"

namespace ArcMeta {

class DropJustifiedView : public JustifiedView {
    Q_OBJECT
public:
    explicit DropJustifiedView(QWidget* parent = nullptr);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

} // namespace ArcMeta
