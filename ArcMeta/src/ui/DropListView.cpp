#include "DropListView.h"
#include "ContentPanel.h"
#include <QDrag>
#include <QPixmap>
#include <QMimeData>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include "Logger.h"

namespace ArcMeta {

DropListView::DropListView(QWidget* parent) : QListView(parent) {
    setAcceptDrops(true);
}

void DropListView::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QListView::dragEnterEvent(event);
    }
}

void DropListView::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) {
        // 物理隔离：不调用 setCurrentIndex(idx)
        // 物理同步：显式调用基类逻辑以激活放置指示器 (Drop Indicator)
        QListView::dragMoveEvent(event);
        event->acceptProposedAction();
    } else {
        QListView::dragMoveEvent(event);
    }
}

void DropListView::dropEvent(QDropEvent* event) {
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
        QListView::dropEvent(event);
    }
}

void DropListView::startDrag(Qt::DropActions supportedActions) {
    QModelIndexList indexes = selectedIndexes();
    if (indexes.isEmpty()) return;

    Logger::log(QString("[列表视图] 开始拖拽 | 选中项数量: %1").arg(indexes.count()));

    // 核心增强：拦截并注入物理路径 QUrl，确保 CategoryPanel 接收校验通过
    QMimeData* mimeData = model()->mimeData(indexes);
    QList<QUrl> urls;
    for (const QModelIndex& idx : indexes) {
        // 2026-03-xx 物理对齐：使用标准 PathRole 枚举名，消除位移隐患
        QString path = idx.data(PathRole).toString(); 
        Logger::log(QString("[列表视图] 提取路径 (PathRole) 对于 %1 : %2").arg(idx.data().toString()).arg(path));
        
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            urls << QUrl::fromLocalFile(path);
        }
    }
    
    QStringList urlStrs;
    for(const QUrl& u : urls) urlStrs << u.toString();
    Logger::log(QString("[列表视图] 最终注入的物理路径列表: %1").arg(urlStrs.join(",")));

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
    
    drag->exec(supportedActions, Qt::MoveAction);
}

} // namespace ArcMeta
