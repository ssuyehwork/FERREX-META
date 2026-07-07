#include "CategoryModel.h"
#include "../meta/CategoryRepo.h"

#include "UiHelper.h"
#include <QMimeData>
#include <QFileInfo>
#include <QFile>
#include <QDir>
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
    m_isFirstLoad = false;

    beginResetModel();
    
    removeRows(0, rowCount());
    
    QStandardItem* root = invisibleRootItem();

    if (m_type == System || m_type == Both) {
        auto addSystemItem = [&](const QString& name, const QString& type, const QString& icon, const QString& color, int sysId) {
            QStandardItem* item = new QStandardItem(QString("%1 (0)").arg(name));
            item->setData(type, TypeRole);
            item->setData(name, NameRole);
            item->setData(color, ColorRole); 
            item->setData(sysId, IdRole);
            item->setEditable(false); 
            item->setIcon(UiHelper::getIcon(icon, QColor(color), 16));
            root->appendRow(item);
        };

        addSystemItem("全部数据", "all", "all_data", "#3498db", -1);
        addSystemItem("未分类", "uncategorized", "uncategorized", "#95a5a6", -2);
        addSystemItem("未标签", "untagged", "untagged", "#7f8c8d", -3);
        addSystemItem("最近访问", "recently_visited", "clock", "#9b59b6", -6);
        addSystemItem("失效数据", "invalid_data", "invalid_data", "#f1c40f", -9);
        addSystemItem("标签管理", "tags", "tag", "#1abc9c", -7);
        addSystemItem("回收站", "trash", "trash", "#e74c3c", -8);
    }

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
    }

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
        QMap<int, QStandardItem*> itemMap;
        QMap<int, Category> catMap;

        for (const auto& cat : categories) {
            catMap[cat.id] = cat;
            int id = cat.id;
            QString name = QString::fromStdWString(cat.name);
            QString color = QString::fromStdWString(cat.color).isEmpty() ? "#555555" : QString::fromStdWString(cat.color);

            QStandardItem* item = new QStandardItem(QString("%1 (0)").arg(name));
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

        for (const auto& cat : categories) {
            int id = cat.id;
            QStandardItem* item = itemMap[id];
            int parentId = cat.parentId;

            if (parentId > 0 && itemMap.contains(parentId)) {
                itemMap[parentId]->appendRow(item);
            } else {
                // 2026-08-xx 物理同步：ArcMeta.Library_* 强制置顶作为顶级分类 (Peer to "我的分类")
                if (QString::fromStdWString(cat.name).startsWith("ArcMeta.Library_", Qt::CaseInsensitive)) {
                    root->appendRow(item);
                } else if (userGroup) {
                    userGroup->appendRow(item);
                }
            }
        }

        if (favGroup) {
            for (const auto& cat : categories) {
                if (cat.pinned) {
                    int id = cat.id;
                    QString name = QString::fromStdWString(cat.name);
                    QString color = QString::fromStdWString(cat.color).isEmpty() ? "#555555" : QString::fromStdWString(cat.color);
                    
                    QStandardItem* mirror = new QStandardItem(QString("%1 (0)").arg(name));
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

void CategoryModel::updateSystemCounts() {
    auto counts = CategoryRepo::getSystemCounts();
    for (int i = 0; i < invisibleRootItem()->rowCount(); ++i) {
        QStandardItem* item = invisibleRootItem()->child(i);
        QString type = item->data(TypeRole).toString();
        if (counts.contains(type)) {
            QString name = item->data(NameRole).toString();
            item->setText(QString("%1 (%2)").arg(name).arg(counts[type]));
        }
    }
}

void CategoryModel::updateStatistics(const QMap<QString, int>& sysCounts, const QMap<int, int>& catCounts) {
    std::function<void(QStandardItem*)> updateItem;
    updateItem = [&](QStandardItem* parent) {
        for (int i = 0; i < parent->rowCount(); ++i) {
            QStandardItem* item = parent->child(i);
            QString type = item->data(TypeRole).toString();
            QString name = item->data(NameRole).toString();
            int id = item->data(IdRole).toInt();

            if (id < 0) { 
                int count = sysCounts.value(type, 0);
                QString newText = QString("%1 (%2)").arg(name).arg(count);
                if (item->text() != newText) {
                    item->setText(newText);
                }
            } else if (type == "category" && id > 0) { 
                int count = catCounts.value(id, 0);
                QString newText = QString("%1 (%2)").arg(name).arg(count);
                if (item->text() != newText) {
                    item->setText(newText);
                }
            }

            if (item->hasChildren()) {
                updateItem(item);
            }
        }
    };

    updateItem(invisibleRootItem());
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
                    if (!cat.physicalPath.empty()) {
                        QString oldPath = QString::fromStdWString(cat.physicalPath);
                        QFileInfo oldInfo(oldPath);
                        
                        if (oldInfo.fileName().startsWith("ArcMeta.Library_", Qt::CaseInsensitive) && cat.parentId == 0) {
                            return false; 
                        }

                        QString newPath = QDir::toNativeSeparators(oldInfo.absoluteDir().absoluteFilePath(newName));
                        if (oldPath != newPath) {
                            if (!QFile::rename(oldPath, newPath)) {
                                return false; 
                            }
                            cat.physicalPath = newPath.toStdWString();
                        }
                    }

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
    return Qt::MoveAction | Qt::CopyAction | Qt::LinkAction;
}

bool CategoryModel::dropMimeData(const QMimeData* mimeData, Qt::DropAction action, int row, int column, const QModelIndex& parent) {
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
        
        if (type != "category" && type != "bookmark" && name != "我的分类") {
            return false; 
        }
    }
    return QStandardItemModel::dropMimeData(mimeData, action, row, column, actualParent);
}

} // namespace ArcMeta
