#include "DropTreeView.h"
#include "CategoryModel.h"
#include "ContentPanel.h"
#include <QDrag>
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
        // 2026-06-xx 按照用户要求：实现拖拽过程中的目标项实时高亮
        QModelIndex idx = indexAt(event->position().toPoint());
        if (idx.isValid()) setCurrentIndex(idx);
        event->acceptProposedAction();
    } else {
        QTreeView::dragMoveEvent(event);
    }
}

void DropTreeView::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasUrls()) {
        QStringList paths;
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                paths << QDir::toNativeSeparators(url.toLocalFile());
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

    Logger::log(QString("[树形视图] 开始拖拽 | 选中项数量: %1").arg(indexes.count()));

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
            Logger::log(QString("[树形视图] 优先从 PathRole 提取路径: %1").arg(path));
        } else {
            path = idx.data(Qt::UserRole + 1).toString();
            Logger::log(QString("[树形视图] 从 UserRole+1 (导航面板兼容) 提取路径: %1").arg(path));
        }
        
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            urls << QUrl::fromLocalFile(path);
        }
    }
    
    QStringList urlStrs;
    for(const QUrl& u : urls) urlStrs << u.toString();
    Logger::log(QString("[树形视图] 最终注入的物理路径列表: %1").arg(urlStrs.join(",")));

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
    
    Logger::log("[树形视图] 执行拖拽操作...");
    drag->exec(supportedActions | Qt::CopyAction, Qt::MoveAction);
}

void DropTreeView::keyboardSearch(const QString& search) {
    Q_UNUSED(search);
}

} // namespace ArcMeta
