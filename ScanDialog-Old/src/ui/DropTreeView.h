#ifndef DROPTREEVIEW_H
#define DROPTREEVIEW_H

#include <QTreeView>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTimer>

namespace ArcMeta {

class DropTreeView : public QTreeView {
    Q_OBJECT
public:
    explicit DropTreeView(QWidget* parent = nullptr);

signals:
    void notesDropped(const QList<int>& noteIds, const QModelIndex& targetIndex);
    void pathsDropped(const QStringList& paths, const QModelIndex& targetIndex);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;

    void keyboardSearch(const QString& search) override;

private:
    // 2026-06-xx 物理辅助：拖拽悬停自动展开
    QTimer* m_autoExpandTimer = nullptr;
    QModelIndex m_hoverIndex;
};

} // namespace ArcMeta

#endif // DROPTREEVIEW_H
