#include "DropTreeView.h"
#include "CategoryModel.h"
#include "ContentPanel.h"
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QPixmap>
#include <QAbstractProxyModel>
#include <QMimeData>
#include <QUrl>
#include <QDir>
#include <QStringList>
#include <QFileInfo>
#include "Logger.h"

namespace ArcMeta {

DropTreeView::DropTreeView(QWidget* parent) : QTreeView(parent) {
    setAcceptDrops(true);
    setDropIndicatorShown(true);
}

void DropTreeView::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QTreeView::dragEnterEvent(event);
    }
}

void DropTreeView::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) {
        // 物理同步：显式调用基类逻辑以激活放置指示器 (Drop Indicator)
        QTreeView::dragMoveEvent(event);
        event->acceptProposedAction();
    } else {
        QTreeView::dragMoveEvent(event);
    }
}

void DropTreeView::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasUrls()) {
        QStringList paths;
        for (const QUrl& u : event->mimeData()->urls()) {
            if (u.isLocalFile()) {
                paths << QDir::toNativeSeparators(u.toLocalFile());
            }
        }
        QModelIndex idx = indexAt(event->position().toPoint());
        if (!paths.isEmpty()) {
            emit pathsDropped(paths, idx);
        }
        event->acceptProposedAction();
    } else {
        QTreeView::dropEvent(event);
    }
}

void DropTreeView::startDrag(Qt::DropActions supportedActions) {
    QModelIndexList indexes = selectedIndexes();
    if (indexes.isEmpty()) return;

    // 核心增强：拦截并注入物理路径 QUrl，确保 CategoryPanel 接收校验通过
    QMimeData* mimeData = model()->mimeData(indexes);
    QList<QUrl> urls;
    for (const QModelIndex& idx : indexes) {
        if (idx.column() != 0) continue;
        
        // 2026-06-xx 工业级增强：优先从 PathRole 提取以规避 ContentPanel 中的角色冲突 (UserRole+1 为 Rating)
        QString path;
        QVariant pathVar = idx.data(PathRole);
        if (pathVar.isValid()) {
            path = pathVar.toString();
        } else {
            path = idx.data(Qt::UserRole + 1).toString();
        }
        
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            urls << QUrl::fromLocalFile(path);
        }
    }
    
    if (!urls.isEmpty()) {
        mimeData->setUrls(urls);
    }

    QDrag* drag = new QDrag(this);
    drag->setMimeData(mimeData);
    
    // 物理还原：消除卡片快照干扰，使用 1x1 透明像素
    QPixmap pix(1, 1);
    pix.fill(Qt::transparent);
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(0, 0));
    
    drag->exec(supportedActions | Qt::CopyAction, Qt::MoveAction);
}

void DropTreeView::keyboardSearch(const QString& search) {
    Q_UNUSED(search);
}

} // namespace ArcMeta
