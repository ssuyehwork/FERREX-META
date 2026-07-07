#pragma once

#include "JustifiedView.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>

namespace ArcMeta {

class DropJustifiedView : public JustifiedView {
    Q_OBJECT
public:
    explicit DropJustifiedView(QWidget* parent = nullptr);

signals:
    void pathsDropped(const QStringList& paths, const QModelIndex& targetIndex);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;
};

} // namespace ArcMeta
