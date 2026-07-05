#ifndef DROPLISTVIEW_H
#define DROPLISTVIEW_H

#include <QListView>

namespace ArcMeta {

class DropListView : public QListView {
    Q_OBJECT
public:
    explicit DropListView(QWidget* parent = nullptr);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

} // namespace ArcMeta

#endif // DROPLISTVIEW_H
