#include "DropJustifiedView.h"
#include "ContentPanel.h"
#include <QDrag>
#include <QPixmap>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>

namespace ArcMeta {

DropJustifiedView::DropJustifiedView(QWidget* parent) : JustifiedView(parent) {
    setDragEnabled(true);
}

void DropJustifiedView::startDrag(Qt::DropActions supportedActions) {
    QModelIndexList indexes = selectedIndexes();
    if (indexes.isEmpty()) return;

    QMimeData* mimeData = model()->mimeData(indexes);
    QList<QUrl> urls;
    for (const QModelIndex& idx : indexes) {
        if (idx.column() == 0) {
            QString path = idx.data(PathRole).toString(); 
            if (!path.isEmpty() && QFileInfo::exists(path)) {
                urls << QUrl::fromLocalFile(path);
            }
        }
    }
    
    if (!urls.isEmpty()) {
        mimeData->setUrls(urls);
    }

    QDrag* drag = new QDrag(this);
    drag->setMimeData(mimeData);
    
    QPixmap pix(1, 1);
    pix.fill(Qt::transparent);
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(0, 0));
    
    drag->exec(supportedActions, Qt::MoveAction);
}

} // namespace ArcMeta
