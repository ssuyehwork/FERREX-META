#include "CategoryModel.h"
#include "../db/CategoryRepo.h"
#include "../db/ItemRepo.h"
#include "../db/FavoritesRepo.h"
#include "UiHelper.h"
#include <QMimeData>
#include <QFileInfo>
#include <QFont>
#include <QTimer>
#include <QSet>
#include <QMap>
#include "../core/AppConfig.h"
#include <QApplication>

namespace ArcMeta {

CategoryModel::CategoryModel(Type type, QObject* parent) 
    : QStandardItemModel(parent), m_type(type) 
{
}

void CategoryModel::setUnlockedIds(const QSet<int>& ids) {
    m_unlockedIds = ids;
}

void CategoryModel::deferredRefresh() {
    refresh();
}

void CategoryModel::refresh() {
    // 2026-06-xx 物理修复：废除破坏性的 clear()，改用 beginResetModel 手动管理。
    // 理由：clear() 会提前发射重置信号，导致 UI 在数据还没填充时就尝试恢复展开状态，引发折叠。
    beginResetModel();
    
    // 清理旧项
    removeRows(0, rowCount());
    
    QStandardItem* root = invisibleRootItem();

    // 1. 系统模块 (同步构建 - 8项)
    if (m_type == System || m_type == Both) {
        auto counts = CategoryRepo::getSystemCounts();
        
        auto addSystemItem = [&](const QString& name, const QString& type, const QString& icon, const QString& color, int sysId) {
            int count = counts.value(type, 0);
            QStandardItem* item = new QStandardItem(QString("%1 (%2)").arg(name).arg(count));
            item->setData(type, TypeRole);
            item->setData(name, NameRole);
            item->setData(color, ColorRole); 
            // 2026-06-xx 物理修复：为系统项分配负数 ID，彻底消除与数据库 ID (0/正数) 的歧义冲突
            item->setData(sysId, IdRole);
            item->setEditable(false); 
            item->setIcon(UiHelper::getIcon(icon, QColor(color), 16));
            root->appendRow(item);
        };

        // [还原] 还原原始设计的语义化图标与配色
        // 物理分配负值 ID 空间
        addSystemItem("全部数据", "all", "all_data", "#3498db", -1);
        addSystemItem("未分类", "uncategorized", "uncategorized", "#95a5a6", -2);
        addSystemItem("未标签", "untagged", "untagged", "#7f8c8d", -3);
        addSystemItem("今日数据", "today", "today", "#2ecc71", -4);
        addSystemItem("昨日数据", "yesterday", "today", "#f39c12", -5);
        addSystemItem("最近访问", "recently_visited", "clock", "#9b59b6", -6);
        addSystemItem("标签管理", "tags", "tag", "#1abc9c", -7);
        addSystemItem("回收站", "trash", "trash", "#e74c3c", -8);
    }

    // 2. 快速访问模块
    QStandardItem* favGroup = nullptr;
    if (m_type == Both || m_type == User) {
        favGroup = new QStandardItem("快速访问");
        favGroup->setData("快速访问", NameRole);
        favGroup->setSelectable(false);
        favGroup->setEditable(false);
        favGroup->setIcon(UiHelper::getIcon("folder_filled", QColor("#FFFFFF"), 16));
        
        QFont font = favGroup->font();
        font.setBold(true);
        favGroup->setFont(font);
        favGroup->setForeground(QColor("#FFFFFF"));
        root->appendRow(favGroup);

        // A. 物理收藏路径 (FavoritesRepo)
        auto favorites = FavoritesRepo::getAll();
        for (const auto& fav : favorites) {
            QStandardItem* item = new QStandardItem(QString::fromStdWString(fav.name));
            item->setData("bookmark", TypeRole);
            item->setData(QString::fromStdWString(fav.path), PathRole);
            item->setData(QString::fromStdWString(fav.name), NameRole);
            item->setIcon(UiHelper::getIcon("folder_filled", QColor("#555555"), 16));
            favGroup->appendRow(item);
        }
    }

    // 3. 我的分类模块
    QStandardItem* userGroup = nullptr;
    if (m_type == User || m_type == Both) {
        userGroup = new QStandardItem("我的分类");
        userGroup->setData("我的分类", NameRole);
        userGroup->setSelectable(false);
        userGroup->setEditable(false);
        userGroup->setFlags(userGroup->flags() | Qt::ItemIsDropEnabled);
        userGroup->setIcon(UiHelper::getIcon("folder_filled", QColor("#FFFFFF"), 16));
        
        QFont font = userGroup->font();
        font.setBold(true);
        userGroup->setFont(font);
        userGroup->setForeground(QColor("#FFFFFF"));
        root->appendRow(userGroup);

        auto categories = CategoryRepo::getAll();
        auto countsVec = CategoryRepo::getCounts();
        QMap<int, int> counts;
        for (const auto& p : countsVec) counts[p.first] = p.second;

        QMap<int, QStandardItem*> itemMap;
        QMap<int, Category> catMap;

        // 先创建所有分类节点，但不挂载
        for (const auto& cat : categories) {
            catMap[cat.id] = cat;
            int id = cat.id;
            QString name = QString::fromStdWString(cat.name);
            QString color = QString::fromStdWString(cat.color).isEmpty() ? "#555555" : QString::fromStdWString(cat.color);
            int count = counts.value(id, 0);

            QStandardItem* item = new QStandardItem(QString("%1 (%2)").arg(name).arg(count));
            item->setData("category", TypeRole);
            item->setData(id, IdRole);
            item->setData(color, ColorRole);
            item->setData(name, NameRole);
            item->setData(cat.pinned, PinnedRole);
            item->setData(cat.encrypted, EncryptedRole);
            item->setData(QString::fromStdWString(cat.encryptHint), EncryptHintRole);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            
            if (cat.encrypted && !m_unlockedIds.contains(id)) {
                item->setIcon(UiHelper::getIcon("lock", QColor("#aaaaaa"), 16));
            } else {
                item->setIcon(UiHelper::getIcon("folder_filled", QColor(color), 16));
            }
            itemMap[id] = item;
        }

        // 2026-06-xx 按照用户要求回归“镜像模式”：实体保留，置顶生成快捷镜像
        // 逻辑：1. 在“我的分类”中构建完整树；2. 将置顶项镜像一份到“快速访问”
        
        // 1. 在“我的分类”构建完整原始树 (不收置顶状态位移干扰)
        for (const auto& cat : categories) {
            int id = cat.id;
            QStandardItem* item = itemMap[id];
            int parentId = cat.parentId;

            if (parentId > 0 && itemMap.contains(parentId)) {
                itemMap[parentId]->appendRow(item);
            } else if (userGroup) {
                userGroup->appendRow(item);
            }
        }

        // 2. 为置顶项在“快速访问”中创建虚拟镜像 (快捷入口)
        if (favGroup) {
            for (const auto& cat : categories) {
                if (cat.pinned) {
                    int id = cat.id;
                    QString name = QString::fromStdWString(cat.name);
                    QString color = QString::fromStdWString(cat.color).isEmpty() ? "#555555" : QString::fromStdWString(cat.color);
                    
                    QStandardItem* mirror = new QStandardItem(name);
                    mirror->setData("category", TypeRole);
                    mirror->setData(id, IdRole);
                    mirror->setData(color, ColorRole);
                    mirror->setData(name, NameRole);
                    mirror->setData(true, PinnedRole);
                    
                    if (cat.encrypted && !m_unlockedIds.contains(id)) {
                        mirror->setIcon(UiHelper::getIcon("lock", QColor("#aaaaaa"), 16));
                    } else {
                        mirror->setIcon(UiHelper::getIcon("folder_filled", QColor(color), 16));
                    }
                    favGroup->appendRow(mirror);
                }
            }
        }
    }
    
    endResetModel();
}

void CategoryModel::loadCategoryItems(const QModelIndex& parentIndex) {
    Q_UNUSED(parentIndex);
}

QVariant CategoryModel::data(const QModelIndex& index, int role) const {
    if (role == Qt::EditRole) {
        return QStandardItemModel::data(index, NameRole);
    }
    return QStandardItemModel::data(index, role);
}

bool CategoryModel::setData(const QModelIndex& index, const QVariant& val, int role) {
    if (role == Qt::EditRole) {
        QString newName = val.toString().trimmed();
        if (newName.isEmpty()) return false;

        QString type = index.data(TypeRole).toString();
        int id = index.data(IdRole).toInt();
        
        if (type == "category" && id > 0) {
            auto categories = CategoryRepo::getAll();
            for (auto& cat : categories) {
                if (cat.id == id) {
                    cat.name = newName.toStdWString();
                    CategoryRepo::update(cat);
                    break;
                }
            }
            refresh();
            return true;
        }
        return false;
    }
    return QStandardItemModel::setData(index, val, role);
}

Qt::DropActions CategoryModel::supportedDropActions() const {
    // 2026-06-xx 物理修复：扩展支持的动作。界外拖入通常被识别为 Copy 或 Link。
    // 只有在此处声明，Qt 视图才不会在拖入时显示“禁止图标”。
    return Qt::MoveAction | Qt::CopyAction | Qt::LinkAction;
}

bool CategoryModel::dropMimeData(const QMimeData* mimeData, Qt::DropAction action, int row, int column, const QModelIndex& parent) {
    // 2026-06-xx 物理修复：如果是外部 URL/路径拖入，放宽校验限制。
    // 允许在侧边栏任意位置释放，由 CategoryPanel 处理具体的分类归属逻辑。
    if (mimeData->hasUrls() || mimeData->hasFormat("text/plain")) {
        return true;
    }

    Q_UNUSED(action);
    Q_UNUSED(row);
    Q_UNUSED(column);
    
    QModelIndex actualParent = parent;
    if (actualParent.isValid()) {
        QStandardItem* parentItem = itemFromIndex(actualParent);
        if (!parentItem) return false;
        
        QString type = parentItem->data(TypeRole).toString();
        QString name = parentItem->data(NameRole).toString();
        
        // 内部拖拽（Move）依然保持严格校验，仅允许移动到分类、书签或根组
        if (type != "category" && type != "bookmark" && name != "我的分类") {
            return false; 
        }
    }
    return QStandardItemModel::dropMimeData(mimeData, action, row, column, actualParent);
}

} // namespace ArcMeta
