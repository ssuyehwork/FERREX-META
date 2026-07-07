#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ContentPanel.h" 
#include "../meta/MetadataManager.h" 
#include "Logger.h"
#include "SvgIcons.h" 
#include "TreeItemDelegate.h" 
#include "DropTreeView.h" 
#include "DropListView.h" 
#include "DropJustifiedView.h"
#include "BatchProgressDialog.h"
#include "ThumbnailDelegate.h"
#include "../util/ImportHelper.h"
#include "../core/AutoImportManager.h"
#include "ToolTipOverlay.h" 
#include "MainWindow.h"
 
#include <QVBoxLayout> 
#include <QHBoxLayout> 
#include <QIcon> 
#include <QSvgRenderer> 
#include <QPainter> 
#include <QHeaderView> 
#include <QScrollBar> 
#include <QStyle> 
#include <QLabel> 
#include <QAction> 
#include <QMenu> 
#include <QAbstractItemView> 
#include <QStandardItem> 
#include "../core/AppConfig.h"
#include <QEvent> 
#include <QKeyEvent> 
#include <QMouseEvent> 
#include <QWheelEvent> 
#include <QStyleOptionViewItem> 
#include <QItemSelectionModel> 
#include <QFileInfo> 
#include <QDir> 
#include <QSet>
#include <QFile>
#include <QDateTime> 
#include <QDesktopServices> 
#include <QUrl> 
#include <QApplication> 
#include <QCoreApplication> 
#include <QProcess> 
#include <QClipboard> 
#include <QMimeData> 
#include <QLineEdit> 
#include <QTextBrowser> 
#include "FramelessDialog.h"
#include <memory>
#include <QRandomGenerator>
#include <QAbstractItemView> 
#include <QtConcurrent> 
#include <QThreadPool> 
#include <QTimer> 
#include <QPointer> 
#include <QPersistentModelIndex> 
 
 
#include <windows.h> 
#include <objbase.h>
#include <shellapi.h> 
#include <io.h>
#include "../meta/MetadataManager.h" 
#include "../meta/BatchRenameEngine.h" 
#include "../meta/CategoryRepo.h" 
#include "../crypto/EncryptionManager.h" 
#include "CategoryLockDialog.h" 
#include "BatchRenameDialog.h" 
#include "UiHelper.h" 
#include "StyleLibrary.h"
#include "../core/CoreController.h"
#include "../core/UndoManager.h"
#include "../core/BasicCommands.h"
using namespace ArcMeta::Style;
#include "../util/ShellHelper.h"
 
namespace ArcMeta { 
 
// --- FerrexVirtualDbModel 实现 ---
FerrexVirtualDbModel::FerrexVirtualDbModel(QObject* parent) : QAbstractTableModel(parent) {
    m_iconCache.setMaxCost(500);
    m_metaCache.setMaxCost(1000);
}

int FerrexVirtualDbModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_displayCount;
}

int FerrexVirtualDbModel::columnCount(const QModelIndex&) const {
    return 8; // 名称, 状态, 星级, 颜色标记, 标签, 类型, 大小, 修改日期
}

Qt::ItemFlags FerrexVirtualDbModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled;
    // 仅允许第 0 列（名称列）且非“分类”项进行重命名
    if (index.column() == 0) {
        if (index.row() < static_cast<int>(m_allRecords.size()) && !m_allRecords[index.row()].isCategory) {
            f |= Qt::ItemIsEditable;
        }
    }
    return f;
}

QVariant FerrexVirtualDbModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(m_allRecords.size())) return QVariant();

    const auto& record = m_allRecords[index.row()];
    QString path = record.path;

    if (record.isCategory) {
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            switch (index.column()) {
                case 0: return record.categoryName;
                case 5: return "子分类";
                default: return "";
            }
        } else if (role == CategoryIdRole) {
            return record.categoryId;
        } else if (role == ColorRole) {
            return record.categoryColor;
        } else if (role == RatingRole) {
            return record.rating; // 2026-07-xx 按照 Plan-73：支持子分类评分
        } else if (role == TypeRole) {
            return "category";
        } else if (role == PathRole) {
            return ""; // 2026-06-xx 物理级补全：子分类无物理路径，返回空以防止逻辑溢出
        } else if (role == IsLockedRole || role == PinnedRole) {
            return record.pinned;
        } else if (role == Qt::DecorationRole && index.column() == 0) {
            static QIcon catIcon = QFileIconProvider().icon(QFileIconProvider::Folder);
            return catIcon;
        }
        return QVariant();
    }

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case 0: {
                // 2026-06-xx 极致性能优化：文件名称提取杜绝 QFileInfo 随机访问。
                // path 已经归一化，通过字符串操作获取文件名
                int lastSlash = std::max(path.lastIndexOf('\\'), path.lastIndexOf('/'));
                if (lastSlash == -1) return path;
                QString name = path.mid(lastSlash + 1);
                if (name.isEmpty() && path.length() >= 2 && path[1] == ':') return path; // 盘符根目录
                return name;
            }
            case 4: {
                return record.tags.join(", ");
            }
            case 5: {
                if (record.isDir) return "文件夹";
                int lastDot = path.lastIndexOf('.');
                return (lastDot != -1) ? path.mid(lastDot + 1).toUpper() + " 文件" : "文件";
            }
            case 6: {
                if (record.isDir) return "-";
                if (record.size < 1024) return QString::number(record.size) + " B";
                if (record.size < 1024 * 1024) return QString::number(record.size / 1024.0, 'f', 1) + " KB";
                return QString::number(record.size / (1024.0 * 1024.0), 'f', 1) + " MB";
            }
            case 7: {
                return QDateTime::fromMSecsSinceEpoch(record.mtime).toString("dd-MM-yyyy HH:mm");
            }
        }
    } else if (role == PathRole) {
        return path;
    } else if (role == TypeRole) {
        return record.isDir ? "folder" : "file";
    } else if (role == RatingRole) {
        return record.rating;
    } else if (role == ColorRole) {
        return record.color;
    } else if (role == IsLockedRole || role == PinnedRole) {
        return record.pinned;
    } else if (role == EncryptedRole) {
        return record.encrypted;
    } else if (role == TagsRole) {
        return record.tags;
    } else if (role == ManagedRole) {
        return record.isManaged;
    } else if (role == RegistrationProgressRole) {
        return record.registrationProgress;
    } else if (role == CategoryIdRole) {
        return 0; 
    } else if (role == IsEmptyRole) {
        return record.isDir && record.isEmpty;
    } else if (role == AspectRatioRole) {
        // 2026-07-xx 性能优化：优先使用 ItemRecord 中已注入的尺寸信息，实现渲染零延迟
        if (record.width > 0 && record.height > 0) return (double)record.width / record.height;
        return m_aspectRatios.value(path, 1.0);
    } else if (role == HasThumbnailRole) {
        // 2026-xx-xx 按照 Plan-114：优化 HasThumbnailRole 判定逻辑
        // 只要是图形或视频格式，均预设为 true，强制 Delegate 进入填满模式，消除抖动
        if (UiHelper::isGraphicsFile(record.suffix)) return true;
        if (record.width > 0 && record.height > 0) return true;
        return m_aspectRatios.contains(path);
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        // 2026-07-xx 架构优化：使用 File ID 作为缓存 Key。
        // 理由：这允许位于不同文件夹下的相同文件（FID 相同）共享同一个缩略图缓存，
        // 彻底解决用户反馈的“同一文件在不同文件夹显示不一致”及重复加载问题。
        QString cacheKey = record.fileId.empty() ? path : QString::fromStdString(record.fileId);
        QIcon* cached = m_iconCache.object(cacheKey);
        if (cached) return *cached;

        QFileInfo info(path);
        QString ext = info.suffix().toLower();
        bool isGraphic = UiHelper::isGraphicsFile(ext) || ext == "svg";

        if (!m_requestedIcons.contains(cacheKey)) {
            m_requestedIcons.insert(cacheKey);
            QPointer<const FerrexVirtualDbModel> weakThis(this);
            (void)QtConcurrent::run([weakThis, path, cacheKey, ext]() {
                #ifdef Q_OS_WIN
                CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
                #endif
                QIcon icon;
                double ar = 1.0;
                bool hasThumb = false;

                if (ext == "svg") {
                    QSvgRenderer renderer(path);
                    if (renderer.isValid()) {
                        QPixmap pix(128, 128);
                        pix.fill(Qt::transparent);
                        QPainter painter(&pix);
                        renderer.render(&painter);
                        icon = QIcon(pix);
                        ar = 1.0;
                        hasThumb = true;
                    }
                } else if (UiHelper::isGraphicsFile(ext)) {
                    QImage img = UiHelper::getShellThumbnail(path, 128);
                    if (!img.isNull()) {
                        icon = QIcon(QPixmap::fromImage(img));
                        ar = (double)img.width() / img.height();
                        hasThumb = true;
                    }
                }

                if (icon.isNull()) {
                    icon = UiHelper::getFileIcon(path, 128);
                }

                QMetaObject::invokeMethod(const_cast<FerrexVirtualDbModel*>(weakThis.data()), [weakThis, path, cacheKey, icon, ar, hasThumb]() {
                    if (!weakThis) return;
                    auto* mutableThis = const_cast<FerrexVirtualDbModel*>(weakThis.data());
                    mutableThis->m_iconCache.insert(cacheKey, new QIcon(icon));
                    if (hasThumb) mutableThis->m_aspectRatios[path] = ar;
                    
                    // 局部刷新，提高性能 (扫描所有匹配该 Key 的路径)
                    for (int i = 0; i < mutableThis->m_displayCount; ++i) {
                        const auto& rec = mutableThis->m_allRecords[i];
                        bool match = (rec.path == path) || (!rec.fileId.empty() && QString::fromStdString(rec.fileId) == cacheKey);
                        if (match) {
                            emit mutableThis->dataChanged(mutableThis->index(i, 0), mutableThis->index(i, 0), {Qt::DecorationRole, AspectRatioRole, HasThumbnailRole});
                            break;
                        }
                    }
                }, Qt::QueuedConnection);
                #ifdef Q_OS_WIN
                CoUninitialize();
                #endif
            });
        }
        
        // 2026-11-14 执行第二步：图形文件等待缩略图时返回空图标，由 Delegate 绘制占位背景，消除抖动
        if (isGraphic) return QIcon(); 
        return UiHelper::getFileIcon(path, 128); // 非图形文件直接显示系统图标
    }

    return QVariant();
}

QVariant FerrexVirtualDbModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        static const QStringList headers = {"名称", "状态", "星级", "颜色标记", "标签", "类型", "大小", "修改日期"};
        if (section < static_cast<int>(headers.size())) return headers[section];
    }
    return QVariant();
}

QStringList FerrexVirtualDbModel::mimeTypes() const {
    return {"text/uri-list"};
}

QMimeData* FerrexVirtualDbModel::mimeData(const QModelIndexList& indexes) const {
    QMimeData* mime = new QMimeData();
    QList<QUrl> urls;
    for (const auto& idx : indexes) {
        if (idx.column() == 0) {
            QString path = data(idx, PathRole).toString();
            if (!path.isEmpty()) urls << QUrl::fromLocalFile(path);
        }
    }
    if (urls.isEmpty()) {
        delete mime;
        return nullptr;
    }
    mime->setUrls(urls);
    return mime;
}

bool FerrexVirtualDbModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() >= static_cast<int>(m_allRecords.size())) return false;

    const auto& record = m_allRecords[index.row()];
    QString path = record.path;

    if (role == Qt::EditRole && index.column() == 0) {
        if (record.isCategory) return false; // 2026-07-xx 按照 Plan-73：子分类暂不支持在此重命名

        QString newName = value.toString();
        if (newName.isEmpty()) return false;

        auto& mutableRecord = m_allRecords[index.row()];
        QString oldPath = mutableRecord.path;
        QFileInfo info(oldPath);
        QString newPath = info.absolutePath() + "/" + newName;

        if (oldPath != newPath) {
            QString nativeNewPath = QDir::toNativeSeparators(newPath);
            if (ShellHelper::renameItem(oldPath, nativeNewPath)) {
                // 2026-06-xx 物理同步：重命名后必须更新 MetadataManager
                MetadataManager::instance().renameItem(oldPath.toStdWString(), nativeNewPath.toStdWString());

                // 撤销支持
                UndoManager::instance().pushCommand(std::make_unique<RenameCommand>(oldPath, nativeNewPath));

                // 物理同步：手动修改 m_allRecords 里的 path 以保持模型数据一致
                mutableRecord.path = nativeNewPath;
                m_metaCache.remove(oldPath);
                emit dataChanged(index, index, {role, Qt::DisplayRole, PathRole});
                return true;
            }
        }
        return false;
    }

    // 2026-06-xx 物理修复：支持星级、颜色、置顶等元数据的持久化设定
    // 2026-07-xx 按照 Plan-116：视图级编辑权限拦截
    if (!record.isCategory && (role == RatingRole || role == ColorRole || role == IsLockedRole || role == PinnedRole)) {
        QString currentType = qobject_cast<ContentPanel*>(parent())->getCurrentCategoryType();
        if (currentType == "nav" || currentType == "") {
            // 物理导航模式下，检查是否在库外
            std::wstring wp = record.path.toStdWString();
            std::wstring volSerial = MetadataManager::getVolumeSerialNumber(wp);
            QString key = QString("ManagedFolder/Volume_%1").arg(QString::fromStdWString(volSerial));
            QString relPath = AppConfig::instance().getValue(key, "").toString();
            
            bool isInsideLibrary = false;
            if (!relPath.isEmpty()) {
                QString drive = record.path.left(3);
                QString managedAbs = QDir::toNativeSeparators(drive + relPath).toLower();
                if (record.path.toLower().startsWith(managedAbs)) {
                    isInsideLibrary = true;
                }
            }

            if (!isInsideLibrary) {
                FramelessMessageBox::information(nullptr, "编辑受阻", "该项目尚未入库，无法进行元数据编辑。\n请先执行“迁移”将其移动至托管库文件夹。");
                return false;
            }
        }
    }

    bool metaUpdated = false;
    if (role == RatingRole) {
        int oldRating = index.data(RatingRole).toInt();
        int newRating = value.toInt();
        if (oldRating != newRating) {
            if (record.isCategory) {
                // 2026-07-xx 按照 Plan-73：分类评分持久化 (SCCH 架构)
                Category cat;
                auto all = CategoryRepo::getAll();
                bool found = false;
                for (auto& c : all) {
                    if (c.id == record.categoryId) {
                        c.presetTags.clear(); // 暂存 Rating 到预设标签或扩展字段。当前 Category 结构无 Rating，复用 presetTags[0] 存储
                        // 逻辑校准：SCCH 架构中 Category 结构并无 rating 字段，
                        // 2026-07-xx 按照分析：由于 Category 结构暂不支持 rating，评分仅在内存中生效并反馈至 UI
                        auto& mutableRec = m_allRecords[index.row()];
                        mutableRec.rating = newRating;
                        metaUpdated = true;
                        found = true;
                        break;
                    }
                }
            } else {
                MetadataManager::instance().setRating(path.toStdWString(), newRating);
                UndoManager::instance().pushCommand(std::make_unique<MetadataCommand>(path, MetadataCommand::Rating, oldRating, newRating));
                metaUpdated = true;
            }
        }
    } else if (role == ColorRole) {
        QString oldColor = index.data(ColorRole).toString();
        QString newColor = value.toString();
        if (oldColor != newColor) {
            MetadataManager::instance().setColor(path.toStdWString(), newColor.toStdWString());
            UndoManager::instance().pushCommand(std::make_unique<MetadataCommand>(path, MetadataCommand::Color, oldColor, newColor));
            metaUpdated = true;
        }
    } else if (role == IsLockedRole || role == PinnedRole) {
        bool pinned = value.toBool();
        if (record.isCategory) {
            auto all = CategoryRepo::getAll();
            for (auto& c : all) {
                if (c.id == record.categoryId) {
                    c.pinned = pinned;
                    CategoryRepo::update(c);
                    auto& mutableRec = m_allRecords[index.row()];
                    mutableRec.pinned = pinned;
                    metaUpdated = true;
                    break;
                }
            }
        } else {
            MetadataManager::instance().setPinned(path.toStdWString(), pinned);
            metaUpdated = true;
        }
    }

    if (metaUpdated) {
        if (!record.isCategory) {
            m_metaCache.remove(path);
            // 2026-06-xx 物理同步：更新本地 Record 缓存，确保 UI 和排序逻辑立即可见最新状态
            updateRecordMetadata(path);
        } else {
            emit dataChanged(index, index, {role});
        }
        return true;
    }

    return false;
}

bool FerrexVirtualDbModel::canFetchMore(const QModelIndex& parent) const {
    Q_UNUSED(parent);
    return false;
}

void FerrexVirtualDbModel::fetchMore(const QModelIndex& parent) {
    Q_UNUSED(parent);
}

void FerrexVirtualDbModel::setRecords(const std::vector<ItemRecord>& records) {
    beginResetModel();
    m_allRecords = records;
    m_pathToIndex.clear();
    for (int i = 0; i < static_cast<int>(m_allRecords.size()); ++i) {
        m_pathToIndex[m_allRecords[i].path] = i;
    }
    m_displayCount = static_cast<int>(m_allRecords.size());
    m_requestedIcons.clear();
    m_aspectRatios.clear();
    m_metaCache.clear();
    endResetModel();
}

void FerrexVirtualDbModel::updateRecordMetadata(const QString& path) {
    QString nPath = QDir::toNativeSeparators(path);
    auto it = m_pathToIndex.find(nPath);
    if (it != m_pathToIndex.end()) {
        int i = it->second;
        if (i >= 0 && i < static_cast<int>(m_allRecords.size())) {
            auto meta = MetadataManager::instance().getMeta(nPath.toStdWString());
            m_allRecords[i].rating = meta.rating;
            m_allRecords[i].color = QString::fromStdWString(meta.color);
            m_allRecords[i].tags = meta.tags;
            m_allRecords[i].fileId = meta.fileId128;
            m_allRecords[i].pinned = meta.pinned;
            m_allRecords[i].encrypted = meta.encrypted;
            m_allRecords[i].isManaged = meta.hasUserOperations();
            m_allRecords[i].palettes.clear();
            for (const auto& pe : meta.palettes) {
                m_allRecords[i].palettes.push_back({pe.color, pe.ratio});
            }
            
            m_metaCache.remove(nPath);
            QModelIndex left = index(i, 0);
            QModelIndex right = index(i, columnCount() - 1);
            emit dataChanged(left, right);
        }
    }
}

void FerrexVirtualDbModel::clear() {
    beginResetModel();
    m_allRecords.clear();
    m_pathToIndex.clear();
    m_displayCount = 0;
    m_requestedIcons.clear();
    m_aspectRatios.clear();
    m_metaCache.clear();
    endResetModel();
}

// --- FilterProxyModel 实现 --- 
FilterProxyModel::FilterProxyModel(QObject* parent) : QSortFilterProxyModel(parent) {} 
 
void FilterProxyModel::updateFilter() { 
    beginFilterChange(); 
    endFilterChange(); 
} 
 
bool FilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const { 
    QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent); 
     
    // 2026-06-xx 性能优化：提前获取 ItemRecord，避免重复查询并为下方过滤提供数据支撑
    const auto* sourceModelPtr = qobject_cast<const FerrexVirtualDbModel*>(sourceModel());
    if (!sourceModelPtr) return true;

    const auto& records = sourceModelPtr->allRecords();
    if (sourceRow < 0 || sourceRow >= (int)records.size()) return false;
    const auto& record = records[sourceRow];

    // --- 按照 Plan-73 & Plan-94：显示/隐藏文件夹/文件与筛选联动 ---
    // 2026-07-xx 逻辑校准：子分类在逻辑上等同于文件夹，受 showFolders 控制
    if (record.isCategory || record.isDir) {
        // 2026-07-xx Plan-94: 判定用户是否在筛选面板中显式勾选了“文件夹”或匹配的“空文件夹”
        bool isFolderExplicitlySelected = currentFilter.types.contains("folder") || 
                                         (record.isEmpty && currentFilter.types.contains("空文件夹"));
        
        // 只有当“顶栏全局开关为隐藏”且“筛选器未显式勾选文件夹”时，才执行拦截
        if (!currentFilter.showFolders && !isFolderExplicitlySelected) {
            return false;
        }
    } else {
        if (!currentFilter.showFiles) return false;
    }

    // 1. 评级过滤 
    if (!currentFilter.ratings.isEmpty()) { 
        int r = record.rating; // 直接从烘焙好的 record 获取，消除 idx.data 虚拟调用开销
        if (!currentFilter.ratings.contains(r)) return false; 
    } 
 
    // 2. 颜色过滤 (Plan-18: 基于 CIELAB Delta E 的感知筛选逻辑)
    if (!currentFilter.colors.isEmpty() || !currentFilter.colorFilterText.isEmpty()) { 
        QString dominantColorHex = record.color; 
        bool matchColor = false;

        // 2026-06-23 按照用户要求：颜色筛选引入“面积占比”双轴过滤逻辑
        auto calculateMatchedArea = [&](const QColor& targetCol) -> float {
            if (!targetCol.isValid()) return 0.0f;
            float totalMatchedArea = 0.0f;

            // Case A: 有调色盘数据，累加所有符合色差要求的色块占比
            if (!record.palettes.empty()) {
                for (const auto& pe : record.palettes) {
                    if (UiHelper::calculateDeltaE(targetCol, pe.first) < currentFilter.colorTolerance) {
                        totalMatchedArea += pe.second;
                    }
                }
            } else {
                // Case B: 仅有主色调数据，若主色匹配则占比视为 100% (降级处理)
                QColor recordCol = UiHelper::parseColorName(dominantColorHex);
                if (UiHelper::calculateDeltaE(targetCol, recordCol) < currentFilter.colorTolerance) {
                    totalMatchedArea = 1.0f;
                }
            }
            return totalMatchedArea;
        };

        // 2.0 文本过滤逻辑 (如果存在文本)
        if (!currentFilter.colorFilterText.isEmpty()) {
            QString searchText = currentFilter.colorFilterText.trimmed();
            // 物理规则：支持名称、色值或“无色标”
            if (searchText == "无色标") {
                if (dominantColorHex.isEmpty()) matchColor = true;
            } else if (searchText.startsWith("#")) {
                QColor targetCol = UiHelper::parseColorName(searchText);
                float area = calculateMatchedArea(targetCol);
                if (area > 0.0f && area * 100.0f >= (float)currentFilter.minColorArea) matchColor = true;
            } else {
                // 模糊匹配颜色名称 (通过反查 colorMap)
                static const QMap<QString, QString> nameToHex = {
                    {"红", "#E24B4A"}, {"橙", "#EF9F27"}, {"黄", "#FECF0E"}, {"绿", "#639922"},
                    {"青", "#1D9E75"}, {"蓝", "#378ADD"}, {"紫", "#7F77DD"}, {"灰", "#5F5E5A"},
                    {"黑", "#000000"}, {"白", "#FFFFFF"}
                };
                for (auto it = nameToHex.begin(); it != nameToHex.end(); ++it) {
                    if (it.key().contains(searchText)) {
                        QColor targetCol = QColor(it.value());
                        float area = calculateMatchedArea(targetCol);
                        if (area > 0.0f && area * 100.0f >= (float)currentFilter.minColorArea) { matchColor = true; break; }
                    }
                }
            }
            if (!matchColor) return false; // 文本过滤不通过
        }

        // 2.1 勾选框过滤 (如果存在勾选)
        if (!currentFilter.colors.isEmpty()) {
            matchColor = false;
            for (const QString& fc : currentFilter.colors) {
                // 特殊情况：无色标 (不涉及占比逻辑)
                if (fc.isEmpty()) {
                    if (dominantColorHex.isEmpty()) { matchColor = true; break; }
                    continue;
                }

                QColor targetCol = UiHelper::parseColorName(fc);
                float area = calculateMatchedArea(targetCol);
                if (area > 0.0f && area * 100.0f >= (float)currentFilter.minColorArea) {
                    matchColor = true;
                    break;
                }
            }
        }
        if (!matchColor) return false; 
    } 
 
    // 4. 类型过滤 
    if (!currentFilter.types.isEmpty() || !currentFilter.typeFilterText.isEmpty()) { 
        QString type = (record.isDir || record.isCategory) ? "folder" : "file";
        QString ext = record.isCategory ? "" : record.suffix.toUpper();
        bool matchType = false; 

        if (!currentFilter.typeFilterText.isEmpty()) {
            QString searchText = currentFilter.typeFilterText.trimmed();
            if (searchText == "文件夹" || searchText.toLower() == "folder") {
                if (type == "folder") matchType = true;
            } else if (searchText == "空文件夹") {
                if (type == "folder" && record.isEmpty) matchType = true;
            } else {
                if (ext.contains(searchText.toUpper())) matchType = true;
            }
            if (!matchType) return false;
        }

        if (!currentFilter.types.isEmpty()) {
            matchType = false;
            for (const QString& fType : currentFilter.types) { 
                if (fType == "folder") { 
                    if (type == "folder") { matchType = true; break; } 
                } else if (fType == "file") {
                    if (type != "folder") { matchType = true; break; }
                } else if (fType == "空文件夹") {
                    if (type == "folder" && record.isEmpty) { matchType = true; break; }
                } else { 
                    if (ext == fType.toUpper()) { matchType = true; break; } 
                } 
            } 
            if (!matchType) return false; 
        }
    } 
 
    // 5. 创建日期过滤 
    if (!currentFilter.createDates.isEmpty() || !currentFilter.createDateFilterText.isEmpty()) { 
        QDate d = QDateTime::fromMSecsSinceEpoch(record.ctime).date();
        QString dStr = d.toString("dd-MM-yyyy"); 
        bool matchDate = false; 

        if (!currentFilter.createDateFilterText.isEmpty()) {
            if (dStr.contains(currentFilter.createDateFilterText.trimmed())) matchDate = true;
            if (!matchDate) return false;
        }

        if (!currentFilter.createDates.isEmpty()) {
            matchDate = false;
            for (const QString& fDate : currentFilter.createDates) { 
                if (fDate == dStr) { matchDate = true; break; } 
            } 
            if (!matchDate) return false; 
        }
    } 

    // 7. 链接过滤 (Plan-30)
    if (currentFilter.linkPresence != FilterState::All) {
        bool hasLink = !record.url.isEmpty();
        if (currentFilter.linkPresence == FilterState::Yes && !hasLink) return false;
        if (currentFilter.linkPresence == FilterState::No && hasLink) return false;
    }

    // 8. 备注过滤 (Plan-30)
    if (currentFilter.notePresence != FilterState::All) {
        bool hasNote = !record.note.isEmpty();
        if (currentFilter.notePresence == FilterState::Yes && !hasNote) return false;
        if (currentFilter.notePresence == FilterState::No && hasNote) return false;
    }

    // 9. 文件大小过滤 (Plan-30)
    if (currentFilter.minSize != -1 && record.size < currentFilter.minSize) return false;
    if (currentFilter.maxSize != -1 && record.size > currentFilter.maxSize) return false;

    // 10. 图像比例过滤 (Plan-29)
    if (currentFilter.ratio != FilterState::AspectAny) {
        // 直接使用 record 中缓存的尺寸信息 (Plan-30 优化：避免重复查询元数据管理器)
        if (record.width > 0 && record.height > 0) {
            double r = (double)record.width / record.height;
            if (currentFilter.ratio == FilterState::Horizontal && record.width <= record.height) return false;
            if (currentFilter.ratio == FilterState::Vertical && record.height <= record.width) return false;
            if (currentFilter.ratio == FilterState::Square && std::abs(r - 1.0) > 0.05) return false;
            if (currentFilter.ratio == FilterState::Ratio169 && std::abs(r - 1.77) > 0.05) return false;
        } else {
            return false; // 无尺寸信息不匹配任何比例筛选
        }
    }
 
    // 6. 修改日期过滤 
    if (!currentFilter.modifyDates.isEmpty() || !currentFilter.modifyDateFilterText.isEmpty()) { 
        QDate d = QDateTime::fromMSecsSinceEpoch(record.mtime).date();
        QString dStr = d.toString("dd-MM-yyyy"); 
        bool matchDate = false; 

        if (!currentFilter.modifyDateFilterText.isEmpty()) {
            if (dStr.contains(currentFilter.modifyDateFilterText.trimmed())) matchDate = true;
            if (!matchDate) return false;
        }

        if (!currentFilter.modifyDates.isEmpty()) {
            matchDate = false;
            for (const QString& fDate : currentFilter.modifyDates) { 
                if (fDate == dStr) { matchDate = true; break; } 
            } 
            if (!matchDate) return false; 
        }
    } 
 
    // 2026-07-xx Plan-92: 统一使用 FilterState 中的 keyword 进行文件名过滤
    if (currentFilter.keyword.isEmpty()) return true; 
 
    QString fileName = idx.data(Qt::DisplayRole).toString(); 
    return fileName.contains(currentFilter.keyword, Qt::CaseInsensitive); 
} 
 
bool FilterProxyModel::lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const { 
    // 2026-07-xx 物理强制：文件夹与子分类始终置顶 (绝对第一权重)
    QString leftType = source_left.data(TypeRole).toString();
    QString rightType = source_right.data(TypeRole).toString();
    bool leftIsDir = (leftType == "folder" || leftType == "category");
    bool rightIsDir = (rightType == "folder" || rightType == "category");

    if (leftIsDir != rightIsDir) {
        // 文件夹 vs 文件：文件夹永远被视为“更小”（在升序中排在前）
        if (sortOrder() == Qt::AscendingOrder) return leftIsDir;
        else return !leftIsDir;
    }

    // 2026-06-xx 工业级纠偏：置顶优先规则 (物理排序第二权重)
    // 必须确保 PinnedRole 或 IsLockedRole 的判定逻辑在排序中具有绝对优先级
    QVariant leftPinnedVar = source_left.data(PinnedRole);
    if (!leftPinnedVar.isValid()) leftPinnedVar = source_left.data(IsLockedRole);
    
    QVariant rightPinnedVar = source_right.data(PinnedRole);
    if (!rightPinnedVar.isValid()) rightPinnedVar = source_right.data(IsLockedRole);

    bool leftPinned = leftPinnedVar.toBool();
    bool rightPinned = rightPinnedVar.toBool();
 
    if (leftPinned != rightPinned) { 
        // 2026-06-xx 物理修复：Qt 排序模型在 Descending 下会反转 lessThan 结果
        // 为了确保置顶项在任何排序顺序下都位于顶部，必须结合 sortOrder 进行逻辑判定
        if (sortOrder() == Qt::AscendingOrder) return leftPinned; // 升序：左置顶 -> 小 (true)
        else return !leftPinned; // 降序：左置顶 -> 结果反转 -> 需要返回 false 以保持顶部
    } 
    return QSortFilterProxyModel::lessThan(source_left, source_right); 
} 
 
 
ContentPanel::ContentPanel(QWidget* parent) 
    : QFrame(parent) { 
    // 2026-07-xx 按照 Plan-63：启用右键菜单策略（容器级）
    setContextMenuPolicy(Qt::CustomContextMenu);

    setObjectName("EditorContainer"); 
    setAttribute(Qt::WA_StyledBackground, true); 
    setMinimumWidth(230); 
    setStyleSheet("color: #EEEEEE;"); 
 
    m_mainLayout = new QVBoxLayout(this); 
    m_mainLayout->setContentsMargins(0, 0, 0, 0); 
    m_mainLayout->setSpacing(0); 
 
 
    m_model = new FerrexVirtualDbModel(this); 
    m_proxyModel = new FilterProxyModel(this); 
    m_proxyModel->setSourceModel(m_model); 
    
    // 2026-05-17 新增：当模型数据发生改变时，自动触发统计重新计算并推送至 FilterPanel
    connect(m_model, &FerrexVirtualDbModel::dataChanged, this, [this](const QModelIndex& topLeft, const QModelIndex& bottomRight, const QVector<int>& roles) {
        Q_UNUSED(topLeft); Q_UNUSED(bottomRight);
        if (roles.isEmpty() || roles.contains(ColorRole) || roles.contains(RatingRole) || roles.contains(TagsRole)) {
            recalculateAndEmitStats();
        }
    });
     
    // 2026-04-12 深度修复：强制锁定过滤列为第 0 列（名称列），确保搜索逻辑不偏离 
    m_proxyModel->setFilterKeyColumn(0); 
    // 2026-05-29 物理修复：开启动态排序，确保“置顶优先”逻辑能在数据加载后自动生效
    m_proxyModel->setDynamicSortFilter(true);
    m_proxyModel->sort(0, Qt::AscendingOrder);
 
    // 2026-06-05 按照要求：从配置中加载上次保存的缩放比例 
    m_zoomLevel = AppConfig::instance().getValue("UI/GridZoomLevel", 96).toInt(); 
    m_isRecursive = false; 
    // 2026-07-xx 物理同步：从配置中加载分类递归显示状态
    m_isCategoryRecursive = AppConfig::instance().getValue("ContentPanel/IsCategoryRecursive", false).toBool();
    // 2026-07-xx 按照用户要求：文件夹默认设为隐藏 (false)
    m_showFolders = AppConfig::instance().getValue("ContentPanel/ShowFolders", false).toBool();
    m_showFiles = AppConfig::instance().getValue("ContentPanel/ShowFiles", true).toBool();
    
    // 同步到当前 FilterState
    m_currentFilter.showFolders = m_showFolders;
    m_currentFilter.showFiles = m_showFiles;
 
    initUi(); 
    // 2026-05-27 按照用户要求：构造函数末尾强行对齐初始网格尺寸，废除 initGridView 中的旧硬编码值 
    updateGridSize(); 
} 
 
void ContentPanel::deferredInit() { 
    qDebug() << "[ContentPanel] deferredInit 开始执行"; 
    // 2026-04-12 按照用户要求：补全延迟初始化逻辑，此处可处理模型预热或首屏数据对齐 
    qDebug() << "[ContentPanel] deferredInit 执行完毕"; 
} 

ItemRecord ContentPanel::createItemRecord(const QString& path, const RuntimeMeta* providedMeta) {
    ItemRecord r;
    // 2026-xx-xx 按照 Plan-114：统一物理路径归一化准则，确保与元数据缓存 Key 严格对齐
    std::wstring wPath = MetadataManager::normalizePath(path.toStdWString());
    QString nPath = QString::fromStdWString(wPath);

    // 1. 物理属性采样 (零 I/O 核心)
    RuntimeMeta meta;
    if (providedMeta) {
        meta = *providedMeta;
    } else {
        meta = MetadataManager::instance().getMeta(wPath);
    }

    // Plan-124: 只有在内存缓存缺失物理时间戳时，才触发 fetchWinApiMetadataDirect
    if (meta.fileId128.empty() || (meta.ctime == 0 && meta.mtime == 0)) {
        std::string fid;
        long long size = 0, ctime = 0, mtime = 0, atime = 0;
        MetadataManager::fetchWinApiMetadataDirect(wPath, fid, nullptr, &size, nullptr, &ctime, &mtime, &atime);
        r.size = size;
        r.ctime = ctime;
        r.mtime = mtime;
        r.atime = atime;
        r.fileId = fid;
        r.isDir = QFileInfo(nPath).isDir();
    } else {
        r.size = meta.fileSize;
        r.ctime = meta.ctime;
        r.mtime = meta.mtime;
        r.atime = meta.atime;
        r.fileId = meta.fileId128;
        r.isDir = meta.isFolder;
    }

    r.path = nPath;

    // 2. 核心元数据注入 (确保 width/height/palettes 物理对齐)
    r.rating = meta.rating;
    r.color = QString::fromStdWString(meta.color);
    r.tags = meta.tags;
    r.pinned = meta.pinned;
    r.encrypted = meta.encrypted;
    r.url = QString::fromStdWString(meta.url);
    r.note = QString::fromStdWString(meta.note);
    r.width = meta.width;
    r.height = meta.height;
    r.isManaged = meta.hasUserOperations();
    for (const auto& pe : meta.palettes) {
        r.palettes.push_back({pe.color, pe.ratio});
    }

    if (r.isDir) {
        // 2026-07-xx 按照 Development_Plan 3.2：从数据库加载持久化的进度值
        r.registrationProgress = MetadataManager::instance().getProgressFromDb(wPath);

        // 2026-xx-xx 工业级稳健判定 (Plan-124 修正)：
        // 只有当明确处于“镜像模式”（providedMeta != nullptr）或项已由数据库托管时，才信任内存索引。
        // 物理路径导航模式下，若项未录入或缓存未命中，必须执行磁盘 I/O 探测以确保正确性。
        if (providedMeta || meta.isManaged) {
            r.isEmpty = !MetadataManager::instance().hasChildrenInCache(wPath);
        } else {
            QDir sub(nPath);
            r.isEmpty = sub.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty();
        }
        r.suffix = ""; // 文件夹不应有扩展名后缀
    } else {
        int lastDot = nPath.lastIndexOf('.');
        r.suffix = (lastDot != -1) ? nPath.mid(lastDot + 1).toLower() : "";
    }
    return r;
}
 
void ContentPanel::initUi() { 
    QWidget* titleBar = new QWidget(this); 
    titleBar->setObjectName("ContainerHeader"); 
    titleBar->setFixedHeight(32); 
    titleBar->setStyleSheet( 
        "QWidget#ContainerHeader {" 
        "  background-color: #252526;" 
        "  border-bottom: 1px solid #333;" 
        "}" 
    ); 
    QHBoxLayout* titleL = new QHBoxLayout(titleBar); 
    titleL->setContentsMargins(15, 0, 5, 0); // 2026-xx-xx 按照用户要求：右侧保留 5px 呼吸边距
    titleL->setSpacing(5);                  // 2026-05-17 按照用户要求：间距统一为 5px
 
    QLabel* iconLabel = new QLabel(titleBar); 
    iconLabel->setPixmap(UiHelper::getIcon("eye", QColor("#41F2F2"), 18).pixmap(18, 18)); 
    titleL->addWidget(iconLabel); 
 
    QLabel* titleLabel = new QLabel("内容", titleBar); 
    titleLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #41F2F2; background: transparent; border: none;"); 
     
    m_btnToggleFolders = new QPushButton(titleBar);
    m_btnToggleFolders->setCheckable(true);
    m_btnToggleFolders->setFixedSize(24, 24);
    m_btnToggleFolders->setChecked(m_showFolders);
    m_btnToggleFolders->setIcon(UiHelper::getIcon("folder_filled", m_showFolders ? QColor("#FDB70A") : QColor("#B0B0B0"), 16));
    m_btnToggleFolders->setProperty("tooltipText", "显示/隐藏文件夹");
    m_btnToggleFolders->installEventFilter(this);
    m_btnToggleFolders->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 4px; }"
        "QPushButton:hover { background: #3E3E42; }"
        "QPushButton:checked { background: #3E3E42; border: none; }" 
        "QPushButton:pressed { background: #4E4E52; }"
    );
    connect(m_btnToggleFolders, &QPushButton::clicked, [this]() {
        m_showFolders = m_btnToggleFolders->isChecked();
        m_btnToggleFolders->setIcon(UiHelper::getIcon("folder_filled", m_showFolders ? QColor("#FDB70A") : QColor("#B0B0B0"), 16));
        AppConfig::instance().setValue("ContentPanel/ShowFolders", m_showFolders);
        m_currentFilter.showFolders = m_showFolders;
        applyFilters();
    });

    m_btnToggleFiles = new QPushButton(titleBar);
    m_btnToggleFiles->setCheckable(true);
    m_btnToggleFiles->setFixedSize(24, 24);
    m_btnToggleFiles->setChecked(m_showFiles);
    m_btnToggleFiles->setIcon(UiHelper::getIcon("file", m_showFiles ? QColor("#2ecc71") : QColor("#B0B0B0"), 16));
    m_btnToggleFiles->setProperty("tooltipText", "显示/隐藏文件");
    m_btnToggleFiles->installEventFilter(this);
    m_btnToggleFiles->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 4px; }"
        "QPushButton:hover { background: #3E3E42; }"
        "QPushButton:checked { background: #3E3E42; border: none; }" 
        "QPushButton:pressed { background: #4E4E52; }"
    );
    connect(m_btnToggleFiles, &QPushButton::clicked, [this]() {
        m_showFiles = m_btnToggleFiles->isChecked();
        m_btnToggleFiles->setIcon(UiHelper::getIcon("file", m_showFiles ? QColor("#2ecc71") : QColor("#B0B0B0"), 16));
        AppConfig::instance().setValue("ContentPanel/ShowFiles", m_showFiles);
        m_currentFilter.showFiles = m_showFiles;
        applyFilters();
    });

    m_btnLayersBlue = new QPushButton(titleBar);
    m_btnLayersBlue->setCheckable(true);
    m_btnLayersBlue->setFixedSize(24, 24);
    m_btnLayersBlue->setIcon(UiHelper::getIcon("layers", QColor("#3498db"), 18));
    m_btnLayersBlue->setProperty("tooltipText", "显示子分类中的项目");
    m_btnLayersBlue->installEventFilter(this);
    m_btnLayersBlue->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 4px; }"
        "QPushButton:hover { background: #3E3E42; }"
        "QPushButton:checked { background: #3E3E42; border: none; }" 
        "QPushButton:pressed { background: #4E4E52; }"
        "QPushButton:disabled { opacity: 0.3; }"
    );
    connect(m_btnLayersBlue, &QPushButton::clicked, [this]() {
        m_isCategoryRecursive = m_btnLayersBlue->isChecked();
        AppConfig::instance().setValue("ContentPanel/IsCategoryRecursive", m_isCategoryRecursive);
        if (m_currentCategoryId != -1) {
            loadCategory(m_currentCategoryId);
        }
    });

    m_btnLayers = new QPushButton(titleBar); 
    m_btnLayers->setCheckable(true); 
    m_btnLayers->setFixedSize(24, 24); 
    m_btnLayers->setIcon(UiHelper::getIcon("layers", QColor("#2ecc71"), 18)); // 2026-xx-xx 按照用户要求：图层按钮改为绿色，以匹配目录导航配色
    // 2026-03-xx 按照宪法要求：禁绝原生 ToolTip，强制对接 ToolTipOverlay 
    m_btnLayers->setProperty("tooltipText", "显示子文件夹中的项目"); 
    m_btnLayers->installEventFilter(this); 
    m_btnLayers->setStyleSheet( 
        "QPushButton { background: transparent; border: none; border-radius: 4px; }" 
        "QPushButton:hover { background: #3E3E42; }" 
        "QPushButton:checked { background: #3E3E42; border: none; }" 
        "QPushButton:pressed { background: #4E4E52; }" 
        "QPushButton:disabled { opacity: 0.3; }" 
    ); 
    connect(m_btnLayers, &QPushButton::clicked, [this]() { 
        if (m_currentPath.isEmpty() || m_currentPath == "computer://") { 
            m_btnLayers->setChecked(false); 
            return; 
        } 
 
        if (m_btnLayers->isChecked()) { 
            // 探测是否有子文件夹 
            QDir dir(m_currentPath); 
            bool hasSubDirs = !dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(); 
            if (!hasSubDirs) { 
                m_btnLayers->setChecked(false); 
                ToolTipOverlay::instance()->showText(QCursor::pos(), "当前文件夹不支持显示子文件夹项目", 1500, QColor("#E81123")); 
                return; 
            } 
            loadDirectory(m_currentPath, true); 
        } else { 
            loadDirectory(m_currentPath, false); 
        } 
    }); 
 
    titleL->addWidget(titleLabel); 
    titleL->addStretch(); 
    titleL->addWidget(m_btnToggleFolders, 0, Qt::AlignVCenter);
    titleL->addWidget(m_btnToggleFiles, 0, Qt::AlignVCenter);
    titleL->addWidget(m_btnLayersBlue, 0, Qt::AlignVCenter);
    titleL->addWidget(m_btnLayers, 0, Qt::AlignVCenter); 
 
    m_mainLayout->addWidget(titleBar); 
 
    m_viewStack = new QStackedWidget(this); 
     
    initGridView(); 
    initListView(); 
 
    m_viewStack->addWidget(m_gridView); 
    m_viewStack->addWidget(m_treeView); 
    m_viewStack->setCurrentWidget(m_gridView); 
 
    QVBoxLayout* contentWrapper = new QVBoxLayout(); 
    // 2026-06-xx 物理对齐：右侧边距设为 0，使滚动条贴合容器边缘
    contentWrapper->setContentsMargins(4, 4, 0, 4); 
    contentWrapper->setSpacing(0); 
    contentWrapper->addWidget(m_viewStack); 
     
    m_mainLayout->addLayout(contentWrapper); 
 
    m_textPreview = new QTextBrowser(this); 
    m_textPreview->setStyleSheet("background-color: #1E1E1E; color: #EEEEEE; border: none; padding: 20px; font-family: 'Segoe UI'; font-size: 14px;"); 
    m_textPreview->hide(); 
    m_mainLayout->addWidget(m_textPreview, 1); 
 
    m_imagePreview = new QLabel(this); 
    m_imagePreview->setStyleSheet("background-color: #1E1E1E; border: none;"); 
    m_imagePreview->setAlignment(Qt::AlignCenter); 
    m_imagePreview->hide(); 
    m_mainLayout->addWidget(m_imagePreview, 1); 
 
    // 2026-04-11 按照用户要求：为预览控件安装拦截器，实现空格键关闭功能 
    m_textPreview->installEventFilter(this); 
    m_imagePreview->installEventFilter(this); 
 
    m_gridView->installEventFilter(this); 
    m_treeView->installEventFilter(this);
} 
 
void ContentPanel::updateStatusBarStats() {
    if (!m_proxyModel) return;
    
    // 只计算当前显示的总项目数量，不区分文件和文件夹
    int totalCount = m_proxyModel->rowCount();
    
    // 发送状态栏统计信号
    emit statusBarStatsUpdated(0, 0, totalCount);
}

void ContentPanel::updateGridSize() {
    // 2026-06-05 按照用户要求：彻底重构为正方形布局，名称外置
    // 2026-06-08 按照用户核心铁律：物理强制锁定缩放最小值为 96 像素
    
    if (m_viewStack->currentWidget() == m_gridView) {
        m_zoomLevel = qBound(96, m_zoomLevel, 128);
    }

    // 写入实时日志 
    ArcMeta::Logger::log(QString("[UI_DEBUG] 卡片缩放级: %1").arg(m_zoomLevel));
    
    if (m_viewStack->currentWidget() == m_gridView) {
        if (auto* jv = qobject_cast<JustifiedView*>(m_gridView)) {
            jv->setTargetRowHeight(m_zoomLevel);
        } else if (auto* lv = qobject_cast<QListView*>(m_gridView)) {
            lv->setIconSize(QSize(m_zoomLevel, m_zoomLevel));
            int side = m_zoomLevel + 46; // 正方形边长
            int ratingH = 24;           // 2026-05-17 按照要求：为卡片外的评分区预留高度
            int nameH = (int)(m_zoomLevel * 0.25); // 名称高度
            int gap = 6;                // 间距归一化
            
            // 总高度 = 正方形边长 + 间距1 + 评分高度 + 间距2 + 名称高度 + 底部缓冲
            int totalH = side + gap + ratingH + gap + nameH + 8;
            lv->setGridSize(QSize(side, totalH));
        }
    } else if (m_viewStack->currentWidget() == m_treeView) {
        // 2026-06-xx 按照要求：列表模式下调整行高
        if (m_zoomLevel > 96) {
            // 如果行高超过 96，自动切换到卡片形式
            setViewMode(GridView);
            m_zoomLevel = 96; // 修正：切换回网格时对齐红线
            updateGridSize();
            return;
        }
        
        // 2026-06-xx 物理修复：图标大小必须随行高变化
        m_treeView->setIconSize(QSize(m_zoomLevel - 8, m_zoomLevel - 8));
        
        // 2026-06-xx 性能优化：仅当行高发生实际变化时更新样式表
        static int lastTreeHeight = -1;
        if (lastTreeHeight != m_zoomLevel) {
            m_treeView->setStyleSheet( 
                QString("QTreeView { background-color: transparent; border: none; outline: none; font-size: 12px; }" 
                        "QTreeView::item { height: %1px; color: #EEEEEE; padding-left: 0px; }" 
                        "QTreeView QLineEdit { background-color: #2D2D2D; color: #FFFFFF; border: 1px solid #378ADD; border-radius: 6px; padding: 2px; selection-background-color: #378ADD; selection-color: #FFFFFF; }")
                .arg(m_zoomLevel)
            );
            lastTreeHeight = m_zoomLevel;
        }
    }

    // 2026-06-05 按照要求：持久化保存当前的缩放级别
    AppConfig::instance().setValue("UI/GridZoomLevel", m_zoomLevel);

    qDebug() << "[GridSize] Zoom:" << m_zoomLevel;
} 
 
bool ContentPanel::eventFilter(QObject* obj, QEvent* event) { 
    // 2026-03-xx 按照宪法要求：物理拦截 Hover 事件以触发 ToolTipOverlay 
    // 2026-05-20 性能优化：同时支持 Enter/Leave 事件，确保响应灵敏 
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::Enter) { 
        QString text = obj->property("tooltipText").toString(); 
        if (!text.isEmpty()) { 
            int timeout = (obj == m_btnLayers || obj == m_btnLayersBlue || 
                       obj == m_btnToggleFolders || obj == m_btnToggleFiles ||
                       obj == m_btnLayersBlue) ? 0 : 700;
            ToolTipOverlay::instance()->showText(QCursor::pos(), text, timeout); 
        } 
    } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) { 
        ToolTipOverlay::hideTip(); 
    } 
 
    if ((obj == m_gridView || obj == m_gridView->viewport() || obj == m_treeView || obj == m_treeView->viewport()) && event->type() == QEvent::Wheel) { 
        // 2026-05-25 物理修复：改用 reinterpret_cast 避开 static_cast 的类型推导逻辑错误 
        QWheelEvent* wEvent = reinterpret_cast<QWheelEvent*>(event); 
        if (wEvent->modifiers() & Qt::ControlModifier) { 
            int delta = wEvent->angleDelta().y(); 
            if (delta > 0) {
                // 向上滚动（放大）
                if (m_viewStack->currentWidget() == m_treeView) {
                    m_zoomLevel += 4; // 列表模式步进调小一些，追求平滑
                    if (m_zoomLevel > 96) {
                        m_zoomLevel = 96;
                        setViewMode(GridView);
                    }
                    updateGridSize();
                } else {
                    m_zoomLevel += 8;
                    updateGridSize();
                }
            } else {
                // 向下滚动（缩小）
                if (m_viewStack->currentWidget() == m_gridView) {
                    if (m_zoomLevel <= 96) {
                        setViewMode(ListView);
                        m_zoomLevel = 80; // 切换到列表时给一个初始行高
                        updateGridSize();
                    } else {
                        m_zoomLevel -= 8;
                        updateGridSize();
                    }
                } else if (m_viewStack->currentWidget() == m_treeView) {
                    m_zoomLevel = qMax(24, m_zoomLevel - 4); // 列表模式最小行高锁定 24
                    updateGridSize();
                }
            }
            return true; 
        } 
    } 
 
    if (event->type() == QEvent::KeyPress) { 
        // 2026-05-25 物理修复：改用 reinterpret_cast 避开 QEvent 到 QKeyEvent 的 static_cast 歧义 
        QKeyEvent* keyEvent = reinterpret_cast<QKeyEvent*>(event); 
 
        // 2026-04-11 按照用户要求：如果当前正在显示文本/图片预览，按下空格键则关闭预览 
        if ((obj == m_textPreview || obj == m_imagePreview) && keyEvent->key() == Qt::Key_Space) { 
            m_textPreview->hide(); 
            m_imagePreview->hide(); 
            m_viewStack->show(); 
            // 恢复焦点到主视图，确保后续交互连续 
            if (m_viewStack->currentWidget()) m_viewStack->currentWidget()->setFocus(); 
            return true; 
        } 
 
        QAbstractItemView* view = qobject_cast<QAbstractItemView*>(obj); 
        if (!view) view = qobject_cast<QAbstractItemView*>(obj->parent()); 
 
        if (qobject_cast<QLineEdit*>(QApplication::focusWidget())) { 
            return false; 
        } 
 
        if (view) { 
            if ((keyEvent->modifiers() & Qt::ControlModifier) &&  
                (keyEvent->key() >= Qt::Key_0 && keyEvent->key() <= Qt::Key_5)) { 
                 
                int rating = keyEvent->key() - Qt::Key_0; 
                auto indexes = view->selectionModel()->selectedIndexes(); 
                for (const auto& idx : indexes) { 
                    if (idx.column() == 0) { 
                        m_proxyModel->setData(idx, rating, RatingRole); 
                    } 
                } 
                return true; 
            } 
 
            if (((keyEvent->modifiers() & Qt::AltModifier) || (keyEvent->modifiers() & (Qt::AltModifier | Qt::WindowShortcut))) &&  
                (keyEvent->key() == Qt::Key_D)) { 
                auto indexes = view->selectionModel()->selectedIndexes(); 
                for (const QModelIndex& idx : indexes) { 
                    if (idx.column() == 0) { 
                        bool current = idx.data(IsLockedRole).toBool(); 
                        m_proxyModel->setData(idx, !current, IsLockedRole); 
                    } 
                } 
                return true; 
            } 
 
            if ((keyEvent->modifiers() & Qt::AltModifier) &&  
                (keyEvent->key() >= Qt::Key_1 && keyEvent->key() <= Qt::Key_9)) { 
                 
                QString colorValue; 
                switch (keyEvent->key()) { 
                    case Qt::Key_1: colorValue = "#E24B4A"; break; // red
                    case Qt::Key_2: colorValue = "#EF9F27"; break; // orange
                    case Qt::Key_3: colorValue = "#FECF0E"; break; // yellow
                    case Qt::Key_4: colorValue = "#639922"; break; // green
                    case Qt::Key_5: colorValue = "#1D9E75"; break; // cyan
                    case Qt::Key_6: colorValue = "#378ADD"; break; // blue
                    case Qt::Key_7: colorValue = "#7F77DD"; break; // purple
                    case Qt::Key_8: colorValue = "#5F5E5A"; break; // gray
                    case Qt::Key_9: colorValue = ""; break; 
                } 
 
                auto indexes = view->selectionModel()->selectedIndexes(); 
                for (const auto& idx : indexes) { 
                    if (idx.column() == 0) { 
                        m_proxyModel->setData(idx, colorValue, ColorRole); 
 
                        // 2026-06-05 按照要求：快捷键设置颜色后立即重渲染图标，实现视觉同步 
                        QString path = idx.data(PathRole).toString(); 
                        QIcon coloredIcon = UiHelper::getFileIcon(path, 128); 
                        m_proxyModel->setData(idx, coloredIcon, Qt::DecorationRole); 
                    } 
                } 
                return true; 
            } 
 
            if (keyEvent->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) { 
                if (keyEvent->key() == Qt::Key_C) { 
                    QStringList paths; 
                    auto indexes = view->selectionModel()->selectedIndexes(); 
                    for (const auto& idx : indexes) if (idx.column() == 0) paths << QDir::toNativeSeparators(idx.data(PathRole).toString()); 
                    if (!paths.isEmpty()) QApplication::clipboard()->setText(paths.join("\r\n")); 
                    return true; 
                } 
                // 2026-03-xx 按照用户要求：补全批量重命名 (Ctrl+Shift+R) 快捷键绑定 
                if (keyEvent->key() == Qt::Key_R) { 
                    performBatchRename(); 
                    return true; 
                } 
            } 
 
            if (keyEvent->key() == Qt::Key_F2) { 
                view->edit(view->currentIndex()); 
                return true; 
            } 
            if (keyEvent->key() == Qt::Key_Delete) { 
                onCustomContextMenuRequested(view->mapFromGlobal(QCursor::pos())); 
                return true; 
            } 
             
            if (keyEvent->modifiers() & Qt::ControlModifier) { 
                // 2026-03-xx 按照用户要求：逻辑重构，统一调用 performCopy 业务函数 
                if (keyEvent->key() == Qt::Key_C && !(keyEvent->modifiers() & Qt::ShiftModifier)) { 
                    performCopy(false); 
                    return true; 
                } 
                // 2026-03-xx 按照用户要求：实现剪切逻辑 (Ctrl+X) 
                if (keyEvent->key() == Qt::Key_X) { 
                    performCopy(true); 
                    return true; 
                } 
                // 2026-03-xx 按照用户要求：逻辑重构，统一调用 performPaste 业务函数 
                if (keyEvent->key() == Qt::Key_V) { 
                    performPaste(); 
                    return true; 
                } 
            } 
 
            if (keyEvent->key() == Qt::Key_Space) { 
                QModelIndex idx = view->currentIndex(); 
                if (idx.isValid()) {
                    QString path = idx.data(PathRole).toString();
                    if (!path.isEmpty()) {
                        // 2026-11-14 按照 Plan-109：全口径预览属性过滤（白名单优先策略）
                        QFileInfo info(path);
                        if (info.isDir()) return true; // 拦截文件夹

                        QString ext = info.suffix().toLower();
                        // 1. 系统级不可预览黑名单 (包含压缩包、二进制文件及系统库)
                        static const QSet<QString> blackList = {
                            "exe", "dll", "sys", "bin", "dat", "lib", "obj", "msi", "com",
                            "zip", "rar", "7z", "iso", "tar", "gz", "bz2", "dmg", "pkg"
                        };
                        if (blackList.contains(ext)) return true;

                        // 2. 预览准入白名单 (仅限受支持的图像类及文本/代码类文件)
                        static const QSet<QString> whiteList = {
                            "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "eps", "pdf", "svg",
                            "txt", "md", "markdown", "log", "cpp", "h", "hpp", "c", "py", "js", "css", "html", "json", "xml", "ini", "conf", "yaml", "yml"
                        };

                        if (whiteList.contains(ext)) {
                            emit requestQuickLook(path);
                        }
                    }
                }
                return true; 
            } 
            if (keyEvent->key() == Qt::Key_Backspace) { 
                QDir dir(m_currentPath); 
                if (dir.cdUp()) emit directorySelected(dir.absolutePath()); 
                return true; 
            } 
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) { 
                onDoubleClicked(view->currentIndex()); 
                return true; 
            } 
            if (keyEvent->modifiers() & Qt::ControlModifier && keyEvent->key() == Qt::Key_Backslash) { 
                setViewMode(m_viewStack->currentIndex() == 0 ? ListView : GridView); 
                return true; 
            } 
        } 
    } 
    return QWidget::eventFilter(obj, event); 
} 
 
QString ContentPanel::getAdjacentFilePath(const QString& currentPath, int delta) { 
    if (!m_proxyModel || m_proxyModel->rowCount() == 0) return QString(); 
 
    int currentIndex = -1; 
    for (int i = 0; i < m_proxyModel->rowCount(); ++i) { 
        QModelIndex idx = m_proxyModel->index(i, 0); 
        if (idx.data(PathRole).toString() == currentPath) { 
            currentIndex = i; 
            break; 
        } 
    } 
 
    if (currentIndex == -1) return QString(); 
 
    int targetIndex = currentIndex + delta; 
    // 逻辑：触达边界时停止，不进行循环跳转 
    if (targetIndex < 0 || targetIndex >= m_proxyModel->rowCount()) { 
        return QString(); 
    } 
 
    QModelIndex targetIdx = m_proxyModel->index(targetIndex, 0); 
    return targetIdx.data(PathRole).toString(); 
} 
 
void ContentPanel::wheelEvent(QWheelEvent* event) { 
    if (event->modifiers() & Qt::ControlModifier) { 
        int delta = event->angleDelta().y(); 
        if (delta > 0) {
            // 放大
            if (m_viewStack->currentWidget() == m_treeView) {
                m_zoomLevel += 4;
                if (m_zoomLevel > 96) {
                    m_zoomLevel = 96;
                    setViewMode(GridView);
                }
                updateGridSize();
            } else {
                m_zoomLevel += 8;
                updateGridSize();
            }
        } else {
            // 缩小
            if (m_viewStack->currentWidget() == m_gridView) {
                if (m_zoomLevel <= 96) {
                    setViewMode(ListView);
                    m_zoomLevel = 80;
                    updateGridSize();
                } else {
                    m_zoomLevel -= 8;
                    updateGridSize();
                }
            } else if (m_viewStack->currentWidget() == m_treeView) {
                m_zoomLevel = qMax(24, m_zoomLevel - 4);
                updateGridSize();
            }
        }
        event->accept(); 
    } else { 
        QWidget::wheelEvent(event); 
    } 
} 
 
void ContentPanel::setViewMode(ViewMode mode) { 
    if (mode == GridView) m_viewStack->setCurrentWidget(m_gridView); 
    else m_viewStack->setCurrentWidget(m_treeView); 
} 
 
void ContentPanel::initGridView() { 
    m_gridView = new DropJustifiedView(this); 
    m_gridView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); 
    m_gridView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); 
    m_gridView->setSelectionMode(QAbstractItemView::ExtendedSelection); 
    // 2026-06-xx 按照用户要求：开启蓝色透明框选效果
    // 物理修复：对于 ListView/TreeView 使用 setSelectionRectVisible
    if (auto* lv = qobject_cast<QListView*>(m_gridView)) lv->setSelectionRectVisible(true);

    // 2026-06-xx 物理对齐：通过 QPalette 设定全局蓝色透明框选视觉样式
    QPalette p = m_gridView->palette();
    // 使用 #378ADD (QColor(55, 138, 221)) 并设定 Alpha 为 80 以确保框选内容清晰可见
    p.setColor(QPalette::Highlight, QColor(55, 138, 221, 80)); 
    p.setColor(QPalette::HighlightedText, Qt::white);
    m_gridView->setPalette(p);
    m_gridView->setContextMenuPolicy(Qt::CustomContextMenu); 
 
    m_gridView->setDragEnabled(true); 
    m_gridView->setAcceptDrops(true);
    m_gridView->setDragDropMode(QAbstractItemView::DragDrop); 
 
    // 2026-06-xx 物理纠偏：移除 SelectedClicked，防止单击项目时意外触发重命名，确保交互稳健
    m_gridView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed); 
 
    m_gridView->setModel(m_proxyModel); 

    connect(m_gridView, SIGNAL(pathsDropped(QStringList,QModelIndex)), this, SLOT(onPathsDropped(QStringList,QModelIndex)));

    auto* justifiedView = qobject_cast<JustifiedView*>(m_gridView);
    if (justifiedView) {
        justifiedView->setAspectRatioRole(AspectRatioRole);
        auto* delegate = new ThumbnailDelegate(this);
        delegate->setHasThumbnailRole(HasThumbnailRole);
        delegate->setRatingRole(RatingRole);
        delegate->setPathRole(PathRole);
        delegate->setPinnedRole(PinnedRole);
        delegate->setManagedRole(ManagedRole);
        delegate->setTypeRole(TypeRole);
        delegate->setIsEmptyRole(IsEmptyRole);
        delegate->setColorRole(ColorRole);
        delegate->setRegistrationProgressRole(RegistrationProgressRole);
        m_gridView->setItemDelegate(delegate);
    } else {
        m_gridView->setItemDelegate(new GridItemDelegate(this)); 
    }

    m_gridView->viewport()->installEventFilter(this); 
 
    connect(m_gridView, &QAbstractItemView::doubleClicked, this, &ContentPanel::onDoubleClicked); 
 
    m_gridView->setStyleSheet( 
        "QAbstractItemView { background-color: transparent; border: none; outline: none; }" 
        "QAbstractItemView::item { background: transparent; }" 
        "QAbstractItemView::item:selected { background-color: transparent; }" 
        "QAbstractItemView::item:hover { background-color: transparent; }"
    ); 
 
    connect(m_gridView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ContentPanel::onSelectionChanged); 
    connect(m_gridView, &QAbstractItemView::customContextMenuRequested, this, &ContentPanel::onCustomContextMenuRequested); 
} 
 
void ContentPanel::initListView() { 
    m_treeView = new DropTreeView(this); 
    m_treeView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); 
    m_treeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded); 
    m_treeView->setSortingEnabled(true); 
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu); 
    m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection); 
    // 2026-06-xx 按照用户要求：开启蓝色透明框选效果
    // 物理修复：QTreeView 不支持 setSelectionRectVisible，通过 QPalette 高亮色实现视觉对齐
    QPalette tp = m_treeView->palette();
    tp.setColor(QPalette::Highlight, QColor(55, 138, 221, 80));
    tp.setColor(QPalette::HighlightedText, Qt::white);
    m_treeView->setPalette(tp);
     
    m_treeView->setDragEnabled(true); 
    m_treeView->setAcceptDrops(true);
    m_treeView->setDragDropMode(QAbstractItemView::DragDrop); 
 
    m_treeView->setExpandsOnDoubleClick(false); 
    m_treeView->setRootIsDecorated(false); 
     
    m_treeView->setItemDelegate(new TreeItemDelegate(this)); 
 
    m_treeView->setModel(m_proxyModel); 
    m_treeView->viewport()->installEventFilter(this); 

    connect(m_treeView, SIGNAL(pathsDropped(QStringList,QModelIndex)), this, SLOT(onPathsDropped(QStringList,QModelIndex)));
 
    m_treeView->setStyleSheet( 
        "QTreeView { background-color: transparent; border: none; outline: none; font-size: 12px; }" 
        "QTreeView::item { height: 28px; color: #EEEEEE; padding-left: 0px; }" 
        "QTreeView::item:selected { background-color: rgba(52, 152, 219, 0.2); border-left: 2px solid #3498db; }"
        "QTreeView::item:hover { background-color: #2A2A2A; }"
        "QTreeView QLineEdit { background-color: #2D2D2D; color: #FFFFFF; border: 1px solid #378ADD; border-radius: 6px; padding: 2px; selection-background-color: #378ADD; selection-color: #FFFFFF; }" 
    ); 
 
    m_treeView->header()->setDefaultAlignment(Qt::AlignCenter);
    m_treeView->header()->setStyleSheet( 
        "QHeaderView::section { background-color: #252525; color: #B0B0B0; border: none; border-right: 1px solid #333333; height: 32px; font-size: 11px; }" 
    ); 
    
    // 2026-06-16 工业级 UI 架构重构 (Plan-21)：名称 Stretch + 日期可调且 Min 150px
    auto* header = m_treeView->header();
    header->setStretchLastSection(false); // 禁止末端拉伸，交由名称列处理
    header->setCascadingSectionResizes(false);
    header->setMinimumSectionSize(30);    // 全局最小宽度设定为 30 像素

    QByteArray headerState = AppConfig::instance().getValue("UI/ListHeaderState").toByteArray();
    if (!headerState.isEmpty()) {
        header->restoreState(headerState);
    } 

    // 无论是否恢复状态，都显式设定初始宽度与最小值，防止恢复状态异常导致列宽为0
    // 确保所有列均可见
    for(int i = 0; i <= 7; ++i) header->setSectionHidden(i, false);

    // 初始像素宽度设定
    header->resizeSection(0, 400); // 名称
    header->resizeSection(1, 40);  // 状态 (固定图标区)
    header->resizeSection(2, 60);  // 星级 (固定图标区)
    header->resizeSection(3, 60);  // 颜色标记 (固定图标区)
    header->resizeSection(4, 100); // 标签
    header->resizeSection(5, 80);  // 类型
    header->resizeSection(6, 80);  // 大小
    header->resizeSection(7, 150); // 修改日期：物理锁定 150 像素
    
    // 1. 设定调整模式：名称列拉伸，其余列交互
    for(int i = 1; i <= 7; ++i) {
        header->setSectionResizeMode(i, QHeaderView::Interactive);
    }
    header->setSectionResizeMode(0, QHeaderView::Stretch);

    // 3. 宽度守恒与物理红线拦截逻辑
    connect(header, &QHeaderView::sectionResized, this, [this, header](int index, int oldSize, int newSize) {
        Q_UNUSED(oldSize);
        static bool guard = false; 
        if (guard || index == 0) return; 
        
        guard = true;
        
        // 物理红线判定：修改日期（索引7）最小 150px
        if (index == 7 && newSize < 150) {
            header->resizeSection(7, 150);
            guard = false;
            return;
        }

        // 宽度守恒判定：杜绝水平滚动条
        int currentTotal = header->length();
        int maxAvailable = m_treeView->viewport()->width();
        
        if (currentTotal > maxAvailable && maxAvailable > 100) {
             int allowed = newSize - (currentTotal - maxAvailable);
             int minAllowed = header->minimumSectionSize();
             if (index == 7) minAllowed = 150; // 修改日期红线优先级最高
             
             header->resizeSection(index, qMax(minAllowed, allowed));
        }
        
        // 5. 持久化逻辑：仅在非加载状态下保存，防止启动抖动
        if (!m_isLoading) {
            AppConfig::instance().setValue("UI/ListHeaderState", header->saveState());
        }
        
        guard = false;
    });
 
    connect(m_treeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ContentPanel::onSelectionChanged); 
    connect(m_treeView, &QTreeView::customContextMenuRequested, this, &ContentPanel::onCustomContextMenuRequested); 
    connect(m_treeView, &QTreeView::doubleClicked, this, &ContentPanel::onDoubleClicked); 
} 
 
void ContentPanel::onCustomContextMenuRequested(const QPoint& pos) { 
    QAbstractItemView* view = qobject_cast<QAbstractItemView*>(sender()); 
    if (!view) return; 
 
    QModelIndex currentIndex = view->indexAt(pos); 
    bool onItem = currentIndex.isValid(); 
    bool isFolder = onItem && (currentIndex.data(TypeRole).toString() == "folder"); 
    QString path = currentIndex.data(PathRole).toString(); 
 
    QMenu menu(this); 
    UiHelper::applyMenuStyle(&menu); 
 
    if (onItem) { 
        // 2026-06-xx 物理修复：在回收站分类中，顶部增加“还原”选项
        if (m_currentCategoryType == "trash") {
            menu.addAction(UiHelper::getIcon("sync", QColor("#2ecc71"), 18), "还原")->setData(ActionRestore);
            menu.addSeparator();
        }

        // [核心操作区] 
        QAction* actOpen = menu.addAction(isFolder ? "打开文件夹" : "打开"); 
        actOpen->setData(ActionOpen); 
        if (!isFolder) { 
            menu.addAction("用系统默认程序打开")->setData(ActionOpenDefault); 
        } 
        menu.addAction("在“资源管理器”中显示")->setData(ActionShowInExplorer); 
 
        menu.addSeparator(); 
 
        // [归类与标记区] 
        // 2026-07-xx 按照 Plan-117：语义分流。判定当前是否为“镜像源”
        // 镜像源定义：侧边栏分类模式 (m_currentCategoryType 不为空) 
        // 或 物理导航模式下已进入托管库内部 (镜像加速态)
        bool isMirrorSource = !m_currentCategoryType.isEmpty();
        if (!isMirrorSource && !m_currentPath.isEmpty()) {
            isMirrorSource = MetadataManager::isInsideManagedLibrary(m_currentPath.toStdWString());
        }

        if (isMirrorSource) {
            // [镜像源：归类与元数据编辑区]
            QMenu* categorizeMenu = menu.addMenu("归类到..."); 
            UiHelper::applyMenuStyle(categorizeMenu); 
            auto categories = CategoryRepo::getRecentlyUsed(15); 
            if (categories.empty()) categories = CategoryRepo::getAll();
            if (categories.size() > 15) categories.resize(15);

            QAction* actToUncat = categorizeMenu->addAction(UiHelper::getIcon("uncategorized", QColor("#95a5a6"), 16), "回归“未分类”");
            actToUncat->setData(ActionCategorize);
            actToUncat->setProperty("catId", -2); 
            categorizeMenu->addSeparator();

            if (categories.empty()) { 
                categorizeMenu->addAction("（暂无分类）")->setEnabled(false); 
            } else { 
                for (const auto& cat : categories) { 
                    QAction* act = categorizeMenu->addAction(QString::fromStdWString(cat.name)); 
                    act->setData(ActionCategorize); 
                    act->setProperty("catId", cat.id); 
                } 
            }

            QMenu* colorMenu = menu.addMenu("设定颜色标签"); 
            UiHelper::applyMenuStyle(colorMenu); 
            colorMenu->setIcon(UiHelper::getIcon("palette", QColor("#EEEEEE"))); 
            struct ColorItem { QString value; QString label; QColor preview; }; 
            QList<ColorItem> colorItems = { 
                {"", "无颜色", QColor("#888780")}, 
                {"#E24B4A", "红色", QColor("#E24B4A")}, 
                {"#EF9F27", "橙色", QColor("#EF9F27")}, 
                {"#FECF0E", "黄色", QColor("#FECF0E")}, 
                {"#639922", "绿色", QColor("#639922")}, 
                {"#1D9E75", "青色", QColor("#1D9E75")}, 
                {"#378ADD", "蓝色", QColor("#378ADD")}, 
                {"#7F77DD", "紫色", QColor("#7F77DD")}, 
                {"#5F5E5A", "灰色", QColor("#5F5E5A")} 
            }; 
            for (const auto& ci : colorItems) { 
                QAction* ca = colorMenu->addAction(ci.label); 
                ca->setData(ActionColorTag); 
                ca->setProperty("colorName", ci.value); 
                QPixmap pix(12, 12); pix.fill(Qt::transparent); 
                QPainter p(&pix); p.setRenderHint(QPainter::Antialiasing); 
                p.setBrush(ci.preview); p.setPen(Qt::NoPen); 
                p.drawEllipse(0, 0, 12, 12); 
                ca->setIcon(QIcon(pix)); 
            } 
 
            bool isPinned = currentIndex.data(IsLockedRole).toBool(); 
            menu.addAction(isPinned ? "取消置顶" : "置顶")->setData(isPinned ? ActionUnpin : ActionPin); 
 
            // --- 2026-07-xx 按照 Plan-116/117：语义校准，移除“收揽入库”术语 ---
            QString ext = QFileInfo(path).suffix().toLower();
            if (UiHelper::isGraphicsFile(ext)) {
                bool isManaged = currentIndex.data(ManagedRole).toBool();
                // 仅允许库内项目进行颜色解析（库外已被拦截）
                if (isManaged) {
                    menu.addAction(UiHelper::getIcon("palette", QColor("#EEEEEE"), 18), "重新解析颜色")->setData(ActionExtractColor);
                } else {
                    // 若在库内但尚未完成 USN 入库（极短中间态），提供解析颜色选项，内部会进行入库状态兜底判断
                    menu.addAction(UiHelper::getIcon("palette", QColor("#EEEEEE"), 18), "解析颜色")->setData(ActionExtractColor);
                }
            }
        } else {
            // [物理源：显示“迁移”]
            if (!m_currentPath.isEmpty() && m_currentPath != "computer://") {
                std::wstring wp = path.toStdWString();
                std::wstring volSerial = MetadataManager::getVolumeSerialNumber(wp);

                // 2026-07-xx 按照 Plan-121：统一复用 AutoImportManager 的路径计算逻辑，
                // 不再自行拼接，确保与 USN 准入判定使用完全一致的路径来源。
                std::wstring managedRootW = AutoImportManager::getManagedLibraryPath(wp);
                QString managedRoot = QString::fromStdWString(managedRootW);

                QMenu* migrateMenu = menu.addMenu(UiHelper::getIcon("add", QColor("#FF8C00"), 18), "迁移");
                UiHelper::applyMenuStyle(migrateMenu);

                if (managedRoot.isEmpty()) {
                    // Library 文件夹尚未创建，给出明确提示而非显示错误路径
                    migrateMenu->addAction("该盘库存未创建")->setEnabled(false);
                } else {
                    QAction* actRoot = migrateMenu->addAction(managedRoot);
                    actRoot->setData(ActionAddToCategory);
                    actRoot->setProperty("targetPath", managedRoot);

                    migrateMenu->menuAction()->setData(ActionAddToCategory);
                    migrateMenu->menuAction()->setProperty("targetPath", managedRoot);
                }

                migrateMenu->addSeparator();
                QStringList recentFolders = AutoImportManager::getRecentVisitedFolders(volSerial);
                if (recentFolders.isEmpty()) {
                    migrateMenu->addAction("迁移至最近活跃位置...")->setEnabled(false);
                } else {
                    for (const QString& folder : recentFolders) {
                        QAction* act = migrateMenu->addAction(folder);
                        act->setData(ActionAddToCategory);
                        act->setProperty("targetPath", folder);
                    }
                }
            }
        }

        menu.addSeparator(); 
 
        // 2026-06-xx 逻辑解耦修复：解除批量重命名的类型硬编码锁定 (架构升级)。
        // 核心规则：多选有效项目 (PathRole 不为空) 或 单选文件夹时，均解锁批量重命名入口。
        int selectedCount = 0;
        for (const auto& selIdx : view->selectionModel()->selectedIndexes()) {
            if (selIdx.column() == 0 && !selIdx.data(PathRole).toString().isEmpty()) {
                selectedCount++;
            }
        }

        // [批量与加密区] 
        if (isFolder || selectedCount > 1) { 
            menu.addAction("批量重命名 (Ctrl+Shift+R)")->setData(ActionBatchRename); 
        }

        if (!isFolder) { 
            QMenu* cryptoMenu = menu.addMenu("加密保护"); 
            UiHelper::applyMenuStyle(cryptoMenu); 
            cryptoMenu->addAction("执行加密保护")->setData(ActionEncrypt); 
            cryptoMenu->addAction("解除加密")->setData(ActionDecrypt); 
            cryptoMenu->addAction("修改加密密码")->setData(ActionChangePwd); 
        } 
 
        menu.addSeparator(); 
 
        // [通用编辑区] 
        if (selectedCount <= 1) {
            menu.addAction("重命名")->setData(ActionRename); 
        }
        menu.addAction("复制")->setData(ActionCopy); 
        menu.addAction("剪切")->setData(ActionCut); 
        menu.addAction("粘贴")->setData(ActionPaste); 
        
        // 2026-06-xx 按照用户要求：在回收站中不显示二级删除菜单
        if (m_currentCategoryType != "trash") {
            QMenu* delMenu = menu.addMenu("删除");
            UiHelper::applyMenuStyle(delMenu);
            delMenu->addAction("移入回收站")->setData(ActionDelete);
            // 2026-07-xx 物理级精简：移除普通彻底删除，仅保留并更名为“永久删除”（采用安全抹除逻辑）
            delMenu->addAction("永久删除")->setData(ActionSecureDelete);
        } else {
            // 回收站模式下，原位置不显示删除
        }
 
        menu.addSeparator(); 
        menu.addAction("复制路径")->setData(ActionCopyPath); 
        menu.addAction("属性")->setData(ActionProperties); 

        // 2026-07-xx 按照 Development_Plan 2.1：始终显示“重新扫描”选项 (仅限托管库内项目)
        if (MetadataManager::isInsideManagedLibrary(path.toStdWString())) {
            menu.addSeparator();
            menu.addAction(UiHelper::getIcon("sync", QColor("#378ADD"), 18), "重新扫描")->setData(ActionRescan);
        }

        // 2026-06-xx 按照用户要求：在回收站分类中，最底部增加“永久删除”选项
        if (m_currentCategoryType == "trash") {
            menu.addSeparator();
            // 2026-07-xx 物理一致性：回收站内的永久删除统一采用 ActionSecureDelete
            menu.addAction(UiHelper::getIcon("trash", QColor("#e81123"), 18), "永久删除")->setData(ActionSecureDelete);
        }
 
    } else { 
        // [空白处菜单] 
        QMenu* newMenu = menu.addMenu("新建..."); 
        UiHelper::applyMenuStyle(newMenu); 
        newMenu->addAction(UiHelper::getIcon("folder_filled", QColor("#EEEEEE")), "创建文件夹")->setData(ActionNewFolder); 
        newMenu->addAction(UiHelper::getIcon("text", QColor("#EEEEEE")), "创建 Markdown")->setData(ActionNewMd); 
        newMenu->addAction(UiHelper::getIcon("text", QColor("#EEEEEE")), "创建纯文本文件 (txt)")->setData(ActionNewTxt); 
 
        menu.addSeparator(); 
        QAction* actPaste = menu.addAction("粘贴"); 
        actPaste->setData(ActionPaste); 
        actPaste->setEnabled(!m_currentPath.isEmpty() && m_currentPath != "computer://"); 
 
        menu.addSeparator(); 
        QAction* actProp = menu.addAction("当前文件夹属性"); 
        actProp->setData(ActionProperties); 
        actProp->setEnabled(!m_currentPath.isEmpty() && m_currentPath != "computer://"); 

        // 2026-07-xx 按照 Plan-63：如果是空白处点击，直接在这里注入并在下方 exec
    } 

    // 2026-07-xx 按照 Plan-63：注入布局显示控制菜单
    menu.addSeparator();
    QMenu* layoutMenu = menu.addMenu("布局显示");
    UiHelper::applyMenuStyle(layoutMenu);
    
    // 通过向上寻道获取 MainWindow 实例以复用菜单逻辑
    MainWindow* mw = nullptr;
    QWidget* parentWin = window();
    while (parentWin) {
        if ((mw = qobject_cast<MainWindow*>(parentWin))) break;
        parentWin = parentWin->parentWidget();
    }
    if (mw) {
        mw->populatePanelMenu(layoutMenu);
    }
 
    QAction* selectedAction = menu.exec(view->viewport()->mapToGlobal(pos)); 
    if (!selectedAction || !selectedAction->data().isValid()) return; 
 
    ContextAction action = static_cast<ContextAction>(selectedAction->data().toInt()); 
 
    switch (action) { 
        case ActionOpen: 
        case ActionOpenDefault: 
            onDoubleClicked(currentIndex); 
            break; 
        case ActionShowInExplorer: { 
            ShellHelper::openInExplorer(onItem ? path : m_currentPath); 
            break; 
        } 
        case ActionNewFolder: createNewItem("folder"); break; 
        case ActionNewMd: createNewItem("md"); break; 
        case ActionNewTxt: createNewItem("txt"); break; 
        case ActionCategorize: { 
            int catId = selectedAction->property("catId").toInt(); 
            auto indexes = view->selectionModel()->selectedIndexes(); 
             
            for (const auto& idx : indexes) { 
                if (idx.column() == 0) { 
                    QString itemPath = idx.data(PathRole).toString(); 
                    std::wstring wPath = itemPath.toStdWString();

                    // 2026-06-xx 物理同步：基于同步获取的 File ID 进行归类，解决新文件关联失败冲突。 
                    std::string fid = MetadataManager::instance().getFileIdSync(wPath); 
                    if (!fid.empty()) { 
                        // 2026-06-xx 按照用户需求：如果在系统层选择了“未分类”，则清除该项所有其他分类关联
                        if (catId == -2) { // 未分类的负数 ID
                             // 2026-07-xx 按照 Plan-83：实现撤销支持
                             std::vector<int> oldCatIds = CategoryRepo::getItemCategoryIds(fid);
                             if (!oldCatIds.empty()) {
                                 if (CategoryRepo::removeAllCategories(fid)) {
                                     UndoManager::instance().pushCommand(std::make_unique<BulkUncategorizeCommand>(itemPath, fid, oldCatIds));
                                 }
                             }
                        } else if (catId > 0) {
                             if (CategoryRepo::addItemToCategory(catId, fid, wPath)) {
                                 UndoManager::instance().pushCommand(std::make_unique<CategorizeCommand>(itemPath, fid, catId, true));
                             }
                        }
                    } 
                } 
            } 
            ToolTipOverlay::instance()->showText(QCursor::pos(), "已完成扫描并成功归类", 1500, QColor("#2ecc71")); 
            break; 
        } 
        case ActionPin: 
        case ActionUnpin: { 
            auto indexes = view->selectionModel()->selectedIndexes(); 
            bool pin = (action == ActionPin); 
            for (const QModelIndex& idx : indexes) { 
                if (idx.column() == 0) { 
                    // 2026-06-xx 架构简化：统一由 model->setData 处理持久化与缓存清理
                    m_proxyModel->setData(idx, pin, IsLockedRole); 
                } 
            } 
            // 2026-06-xx 物理修复：强制刷新代理模型排序，确保置顶项立即重排至顶部
            m_proxyModel->invalidate();
            m_proxyModel->sort(0, m_proxyModel->sortOrder());
            break; 
        } 
        case ActionColorTag: { 
            QString colorName = selectedAction->property("colorName").toString(); 
            auto indexes = view->selectionModel()->selectedIndexes(); 
            for (const auto& idx : indexes) { 
                if (idx.column() == 0) { 
                    QString itemPath = idx.data(PathRole).toString(); 
                    // 2026-06-xx 架构简化：统一由 model->setData 处理持久化与缓存清理
                    m_proxyModel->setData(idx, colorName, ColorRole); 
 
                    // 2026-06-05 按照要求：设置颜色后立即重新生成并应用图标，实现视觉同步 
                    // 2026-05-17 逻辑修复：针对图像格式，必须优先尝试提取缩略图，防止图标覆盖内容
                    QIcon coloredIcon;
                    QString ext = QFileInfo(itemPath).suffix().toLower();
                    if (UiHelper::isGraphicsFile(ext)) {
                        // 2026-06-xx 物理同步：使用 m_zoomLevel 确保尺寸一致
                        QImage img = UiHelper::getShellThumbnail(itemPath, this->m_zoomLevel);
                        if (!img.isNull()) coloredIcon = QIcon(QPixmap::fromImage(img));
                    }
                    if (coloredIcon.isNull()) {
                        coloredIcon = UiHelper::getFileIcon(itemPath, this->m_zoomLevel);
                    }
                    m_proxyModel->setData(idx, coloredIcon, Qt::DecorationRole); 
                } 
            } 
            break; 
        } 
        case ActionExtractColor: {
            // 2026-07-xx 按照用户要求：支持多选批量解析颜色
            QModelIndexList selectedRows = view->selectionModel()->selectedRows();
            // 物理对齐：仅保留选中行中的第 0 列项，防止重复计数
            QModelIndexList filteredRows;
            for (const auto& selIdx : selectedRows) {
                if (selIdx.column() == 0) filteredRows << selIdx;
            }
            if (filteredRows.isEmpty() && currentIndex.isValid()) filteredRows << currentIndex;
            
            struct ProcessItem { QString path; bool isManaged; };
            QVector<ProcessItem> itemsToProcess;
            for (const auto& selIdx : filteredRows) {
                QString pathItem = selIdx.data(PathRole).toString();
                if (!pathItem.isEmpty()) {
                    itemsToProcess.append({pathItem, selIdx.data(ManagedRole).toBool()});
                }
            }
            if (itemsToProcess.isEmpty()) break;

            QPointer<ContentPanel> weakThis(this);
            int total = static_cast<int>(itemsToProcess.size());

            // 只有当文件数多于 5 个时才显示进度条
            BatchProgressDialog* progress = nullptr;
            if (total > 5) {
                progress = new BatchProgressDialog("收揽入库", this);
                progress->show();
            }

            (void)QtConcurrent::run([weakThis, itemsToProcess, total, progress]() {
                // 后台线程初始化 COM 环境以支持 Shell 缩略图/颜色提取
                CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
                
                for (int i = 0; i < total; ++i) {
                    const auto& item = itemsToProcess[i];
                    QString itemPath = item.path;
                    
                    if (progress) {
                        QMetaObject::invokeMethod(progress, "updateProgress", Qt::QueuedConnection, 
                                                  Q_ARG(int, i + 1), Q_ARG(int, total), Q_ARG(QString, QFileInfo(itemPath).fileName()));
                    }

                    // 2026-07-xx 按照 Plan-116/117：废除 UI 主动注册逻辑。
                    // 仅针对已入库项目（库内）执行强制重新解析逻辑。
                    if (item.isManaged) {
                        // 针对已入库项目，执行强制重新解析逻辑
                        auto palette = UiHelper::extractPalette(itemPath);
                        if (!palette.isEmpty()) {
                            QColor dominant = UiHelper::quantizeColor(palette.first().first);
                            QString colorHex = dominant.name().toUpper();

                            QMetaObject::invokeMethod(weakThis.data(), [weakThis, itemPath, colorHex, palette]() {
                                if (weakThis) {
                                    MetadataManager::instance().setPalettes(itemPath.toStdWString(), palette);
                                    
                                    auto* model = weakThis->m_model;
                                    const auto& records = model->allRecords();
                                    for (size_t j = 0; j < records.size(); ++j) {
                                        if (records[j].path == itemPath) {
                                            QModelIndex srcIdx = model->index(static_cast<int>(j), 0);
                                            model->setData(srcIdx, colorHex, ColorRole);
                                            break;
                                        }
                                    }
                                }
                            }, Qt::QueuedConnection);
                        }
                    }
                }

                QMetaObject::invokeMethod(weakThis.data(), [weakThis, progress]() {
                    if (weakThis) {
                        weakThis->onSelectionChanged();
                        if (progress) progress->close();
                        ToolTipOverlay::instance()->showText(QCursor::pos(), "批量颜色解析完成", 1500, QColor("#2ecc71"));
                    }
                }, Qt::QueuedConnection);

                CoUninitialize();
            });
            break;
        }
        case ActionEncrypt: { 
            FramelessInputDialog dlg("加密保护", "设置加密密码:", "", this);
            dlg.setEchoMode(QLineEdit::Password);
            if (dlg.exec() == QDialog::Accepted) { 
                QString pwd = dlg.text();
                if (pwd.isEmpty()) break;
                auto indexes = view->selectionModel()->selectedIndexes(); 
                QStringList targets; 
                for (const auto& idx : indexes) if (idx.column() == 0) targets << idx.data(PathRole).toString(); 
                 
                ToolTipOverlay::instance()->showText(QCursor::pos(), "加密任务已在后台启动...", 2000); 
                 
                std::string stdPwd = pwd.toStdString(); 
                QPointer<ContentPanel> self(this); 
                QString currentDir = m_currentPath; 
 
                (void)QThreadPool::globalInstance()->start([self, targets, stdPwd, currentDir]() { 
                    for (const QString& src : targets) { 
                        QString dest = src + ".amenc"; 
                        if (EncryptionManager::instance().encryptFile(src.toStdWString(), dest.toStdWString(), stdPwd)) { 
                            QFile::remove(src); 
                            MetadataManager::instance().setEncrypted(dest.toStdWString(), true); 
                        } 
                    } 
                    QMetaObject::invokeMethod(QCoreApplication::instance(), [self, currentDir]() { 
                        if (self && self->m_currentPath == currentDir) self->loadDirectory(currentDir, self->m_isRecursive); 
                        ToolTipOverlay::instance()->showText(QCursor::pos(), "加密任务处理完成", 1500, QColor("#2ecc71")); 
                    }); 
                }); 
            } 
            break; 
        } 
        case ActionDecrypt: { 
            FramelessInputDialog dlg("解除加密", "输入加密密码:", "", this);
            dlg.setEchoMode(QLineEdit::Password);
            if (dlg.exec() == QDialog::Accepted) { 
                QString pwd = dlg.text();
                if (!pwd.isEmpty()) { 
                    ToolTipOverlay::instance()->showText(QCursor::pos(), "解除加密逻辑已触发", 1500); 
                }
            } 
            break; 
        } 
        case ActionBatchRename: performBatchRename(); break; 
        case ActionAddToCategory: {
            QStringList paths;
            auto indexes = view->selectionModel()->selectedIndexes();
            for (const auto& idx : indexes) {
                if (idx.column() == 0) {
                    QString p = idx.data(PathRole).toString();
                    if (!p.isEmpty()) paths << p;
                }
            }
            
            if (paths.isEmpty() && !path.isEmpty()) paths << path;

            QString target = selectedAction->property("targetPath").toString();
            if (target.isEmpty()) {
                // 兜底逻辑：获取当前盘符托管库根目录
                std::wstring wp = path.toStdWString();
                std::wstring volSerial = MetadataManager::getVolumeSerialNumber(wp);
                QString key = QString("ManagedFolder/Volume_%1").arg(QString::fromStdWString(volSerial));
                QString relPath = AppConfig::instance().getValue(key, "").toString();
                target = QDir::toNativeSeparators(path.left(3) + relPath);
            }

            if (!paths.isEmpty() && !target.isEmpty()) {
                // 2026-07-xx 按照 Plan-116：物理迁移至目标目录
                ImportHelper::importPaths(paths, target, this);
            }
            break;
        }
        case ActionRename: view->edit(currentIndex); break; 
        case ActionCopy: performCopy(false); break; 
        case ActionCut: performCopy(true); break; 
        case ActionPaste: performPaste(); break; 
        case ActionRescan: {
            auto indexes = view->selectionModel()->selectedIndexes();
            QStringList targetPaths;
            for (const auto& idx : indexes) {
                if (idx.column() == 0) {
                    QString p = idx.data(PathRole).toString();
                    if (!p.isEmpty()) targetPaths << p;
                }
            }
            if (targetPaths.isEmpty() && !path.isEmpty()) targetPaths << path;

            if (!targetPaths.isEmpty()) {
                // 2026-08-xx 按照 Plan-126：用户手动发起的“重新扫描”应属于元数据刷新
                // 此时依然允许通过 MetadataManager 执行，但不应作为常规“入库”手段
                MetadataManager::instance().registerItemsAsync(targetPaths, true);
                ToolTipOverlay::instance()->showText(QCursor::pos(), "已启动物理状态同步", 1500, QColor("#378ADD"));
            }
            break;
        }
        case ActionRestore: {
            auto indexes = view->selectionModel()->selectedIndexes();
            for (const auto& idx : indexes) {
                if (idx.column() == 0) {
                    QString itemPath = idx.data(PathRole).toString();
                    auto meta = MetadataManager::instance().getMeta(itemPath.toStdWString());
                    if (meta.isTrash && !meta.originalPath.empty()) {
                        QString dest = QString::fromStdWString(meta.originalPath);
                        QDir().mkpath(QFileInfo(dest).absolutePath());
                        if (QFile::rename(itemPath, dest)) {
                            MetadataManager::instance().markAsTrash(dest.toStdWString(), false);
                            // 物理同步：由于原文件位置可能已被 MFT 逻辑自动识别，此处主要负责状态翻转
                        }
                    }
                }
            }
            loadDirectory(m_currentPath);
            MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild); // 刷新全量统计
            break;
        }
        case ActionDelete: 
        case ActionSecureDelete: {
            auto indexes = view->selectionModel()->selectedIndexes();
            QStringList targetPaths;
            for (const auto& idx : indexes) {
                if (idx.column() == 0) targetPaths << idx.data(PathRole).toString();
            }
            if (targetPaths.isEmpty() && !path.isEmpty()) targetPaths << path;

            if (targetPaths.isEmpty()) break;

            if (action == ActionDelete) {
                if (ShellHelper::moveToTrash(targetPaths)) loadDirectory(m_currentPath);
            } else {
                // 2026-07-xx 物理级同步：将逻辑收拢为“永久删除”（安全抹除）
                QString msg = "确定要永久删除选中的项目吗？数据将被物理覆写并彻底抹除，此操作不可恢复。";
                if (!FramelessMessageBox::question(this, "确认删除", msg)) break;

                BatchProgressDialog* progress = new BatchProgressDialog("正在执行永久删除（深层抹除）...", this);
                progress->show();

                QPointer<ContentPanel> weakThis(this);
                QPointer<BatchProgressDialog> weakProgress(progress);
                (void)QtConcurrent::run([weakThis, targetPaths, action, weakProgress]() {
                    int count = 0;
                    for (const QString& p : targetPaths) {
                        if (!weakThis || !weakProgress) return;
                        std::wstring wp = QDir::toNativeSeparators(p).toStdWString();
                        
                        // 1. 物理抹除
                        bool physicalOk = false;
                        if (action == ActionSecureDelete) {
                            // 安全擦除逻辑：递归覆写
                            std::function<bool(const QString&)> secureRemove;
                            secureRemove = [&](const QString& target) -> bool {
                                QFileInfo info(target);
                                if (info.isDir()) {
                                    QDir dir(target);
                                    for (const QString& entry : dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
                                        secureRemove(target + "/" + entry);
                                    }
                                    return QDir().rmdir(target);
                                } else {
                                    // 随机覆写全量扇区
                                    QFile file(target);
                                    if (file.open(QIODevice::ReadWrite)) {
                                        qint64 size = file.size();
                                        if (size > 0) {
                                            QByteArray buffer(65536, 0);
                                            for (int pass = 0; pass < 3; ++pass) { // 覆写 3 遍
                                                file.seek(0);
                                                qint64 written = 0;
                                                while (written < size) {
                                                    for (int i = 0; i < buffer.size(); ++i) buffer[i] = (char)QRandomGenerator::global()->bounded(256);
                                                    qint64 toWrite = qMin((qint64)buffer.size(), size - written);
                                                    file.write(buffer.data(), toWrite);
                                                    written += toWrite;
                                                }
                                                file.flush();
                                                // 2026-06-xx 物理对齐：调用 Windows API 强制落盘，确保覆写数据真实写入扇区
                                                HANDLE hFile = (HANDLE)_get_osfhandle(file.handle());
                                                if (hFile != INVALID_HANDLE_VALUE) {
                                                    FlushFileBuffers(hFile);
                                                }
                                            }
                                        }
                                        file.close();
                                    }
                                    return QFile::remove(target);
                                }
                            };
                            physicalOk = secureRemove(p);
                        } else {
                            // 普通彻底删除：递归
                            std::function<bool(const QString&)> recursiveRemove;
                            recursiveRemove = [&](const QString& target) -> bool {
                                QFileInfo info(target);
                                if (info.isDir()) {
                                    QDir dir(target);
                                    for (const QString& entry : dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
                                        recursiveRemove(target + "/" + entry);
                                    }
                                    return QDir().rmdir(target);
                                } else {
                                    return QFile::remove(target);
                                }
                            };
                            physicalOk = recursiveRemove(p);
                        }

                        if (physicalOk) {
                            // 2026-06-xx 按照用户要求：永久删除后从数据库彻底移除相应数据
                            MetadataManager::instance().deletePermanently(wp);
                            // 2026-06-xx 按照分析计划 #8：清理撤销栈
                            UndoManager::instance().removeCommandsForPath(p);
                        }

                        count++;
                        QMetaObject::invokeMethod(weakProgress.data(), [weakProgress, count, targetPaths]() {
                            if (weakProgress) weakProgress->setValue((int)((float)count / targetPaths.size() * 100));
                        });
                    }

                    QMetaObject::invokeMethod(QCoreApplication::instance(), [weakThis, weakProgress]() {
                        if (weakProgress) {
                            weakProgress->accept();
                            weakProgress->deleteLater();
                        }
                        if (weakThis) {
                            weakThis->loadDirectory(weakThis->m_currentPath);
                            ToolTipOverlay::instance()->showText(QCursor::pos(), "深层抹除已完成，关联记录已物理清空", 1500, QColor("#2ecc71"));
                        }
                    });
                });
            }
            break;
        }
        case ActionCopyPath: QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); break; 
        case ActionProperties: { 
            ShellHelper::showProperties(onItem ? path : m_currentPath); 
            break; 
        } 
        default: break; 
    } 
} 
 
void ContentPanel::performCopy(bool cutMode) { 
    // 2026-03-xx 按照用户要求：封装标准化文件复制/剪切逻辑 
    QModelIndexList indexes = getSelectedIndexes(); 
    QList<QUrl> urls; 
    for (const auto& idx : indexes) { 
        if (idx.column() == 0) { 
            QString path = idx.data(PathRole).toString(); 
            if (!path.isEmpty()) urls << QUrl::fromLocalFile(path); 
        } 
    } 
 
    if (urls.isEmpty()) return; 
 
    QMimeData* mime = new QMimeData(); 
    mime->setUrls(urls); 
     
    if (cutMode) { 
        // 核心规范：告知系统这是剪切操作 (DROPEFFECT_MOVE = 2) 
        // 修复：将变量名由 data 改为 effectData，避免隐藏类成员警告 
        QByteArray effectData; 
        effectData.append((char)2);  
        mime->setData("Preferred DropEffect", effectData); 
    } 
 
    QApplication::clipboard()->setMimeData(mime); 
} 
 
void ContentPanel::performPaste() { 
    // 2026-03-xx 按照用户要求：封装标准化文件粘贴逻辑，对接 Windows Shell 
    if (m_currentPath.isEmpty() || m_currentPath == "computer://") return; 
 
    const QMimeData* mime = QApplication::clipboard()->mimeData(); 
    if (!mime || !mime->hasUrls()) return; 
 
    QList<QUrl> urls = mime->urls(); 
    QStringList fromPaths;
    for (const QUrl& url : urls) {
        fromPaths << url.toLocalFile();
    }
     
    if (fromPaths.isEmpty()) return; 
 
    // 探测是否为剪切模式 
    bool isMove = false; 
    if (mime->hasFormat("Preferred DropEffect")) { 
        QByteArray effect = mime->data("Preferred DropEffect"); 
        if (!effect.isEmpty() && (effect.at(0) & 0x02)) isMove = true; 
    } 
 
    if (ShellHelper::copyOrMoveItems(fromPaths, m_currentPath, isMove)) { 
        if (isMove) {
            UndoManager::instance().pushCommand(std::make_unique<MoveCommand>(fromPaths, QFileInfo(fromPaths.first()).absolutePath(), m_currentPath));
        }
        loadDirectory(m_currentPath, m_isRecursive); 
    } 
} 
 
void ContentPanel::performBatchRename() { 
    // 2026-03-xx 按照用户要求：弹出深度集成的高级批量重命名对话框 
    QModelIndexList indexes = getSelectedIndexes(); 
    std::vector<std::wstring> originalPaths; 
    for (const auto& idx : indexes) { 
        if (idx.column() == 0) { 
            QString path = idx.data(PathRole).toString(); 
            if (!path.isEmpty()) originalPaths.push_back(path.toStdWString()); 
        } 
    } 
 
    if (originalPaths.empty()) { 
        ToolTipOverlay::instance()->showText(QCursor::pos(), "请先选择需要重命名的项目", 2000, QColor("#E81123")); 
        return; 
    } 
 
    BatchRenameDialog dlg(originalPaths, this); 
    if (dlg.exec() == QDialog::Accepted) { 
        loadDirectory(m_currentPath, m_isRecursive); 
        ToolTipOverlay::instance()->showText(QCursor::pos(), "批量重命名操作已成功执行", 1500, QColor("#2ecc71")); 
    } 
} 
 
void ContentPanel::onSelectionChanged() { 
    QItemSelectionModel* selectionModel = (m_viewStack->currentWidget() == m_gridView) ? m_gridView->selectionModel() : m_treeView->selectionModel(); 
    if (!selectionModel) return; 
 
    QStringList selectedPaths; 
    QModelIndexList indices = selectionModel->selectedIndexes(); 
    for (const QModelIndex& index : indices) { 
        if (index.column() == 0) { 
            QString path = index.data(PathRole).toString(); 
            if (!path.isEmpty()) selectedPaths.append(path); 
        } 
    } 
    emit selectionChanged(selectedPaths); 
} 
 
void ContentPanel::refreshAll() {
    // 2026-06-xx 物理对标：完善刷新逻辑，支持所有上下文类型
    if (m_currentCategoryType == "user_category") {
        if (m_currentCategoryId != -1) loadCategory(m_currentCategoryId);
    } else if (m_currentCategoryType == "all" || m_currentCategoryType == "uncategorized" || 
               m_currentCategoryType == "untagged" || m_currentCategoryType == "recently_visited" || 
               m_currentCategoryType == "trash") {
        QStringList paths = CategoryRepo::getSystemCategoryPaths(m_currentCategoryType);
        loadPaths(paths);
    } else if (!m_currentPath.isEmpty() && m_currentPath != "computer://") {
        loadDirectory(m_currentPath, m_isRecursive);
    } else {
        // 兜底逻辑：加载“此电脑”
        loadDirectory("computer://");
    }
}

void ContentPanel::updateItemMetadata(const QString& path) {
    if (m_model) {
        m_model->updateRecordMetadata(path);
    }
}

void ContentPanel::onPathsDropped(const QStringList& paths, const QModelIndex& targetIndex) {
    if (paths.isEmpty() || m_currentPath.isEmpty() || m_currentPath == "computer://") return;

    QString destDir = m_currentPath;
    if (targetIndex.isValid()) {
        QModelIndex srcIdx = m_proxyModel->mapToSource(targetIndex);
        if (srcIdx.isValid()) {
            QString targetPath = srcIdx.data(PathRole).toString();
            if (!targetPath.isEmpty() && QFileInfo(targetPath).isDir()) {
                destDir = targetPath;
            }
        }
    }

    // 检查是否在原地投放
    bool sameDir = true;
    for (const QString& p : paths) {
        if (QDir::toNativeSeparators(QFileInfo(p).absolutePath()) != QDir::toNativeSeparators(destDir)) {
            sameDir = false;
            break;
        }
    }
    if (sameDir && destDir == m_currentPath) return;

    bool isMove = !(QApplication::keyboardModifiers() & Qt::ControlModifier);
    
    // 2026-xx-xx 按照 Plan-105：在拖拽入库操作期间抑制 AutoImportManager 产生的冗余刷新信号
    MetadataManager::instance().setInternalOperating(true);

    if (ShellHelper::copyOrMoveItems(paths, destDir, isMove)) {
        if (isMove) {
            UndoManager::instance().pushCommand(std::make_unique<MoveCommand>(paths, QFileInfo(paths.first()).absolutePath(), destDir));
        }
        // 显式执行一次刷新，确保用户操作立即反馈
        loadDirectory(m_currentPath, m_isRecursive);
    }

    // 2000ms 后释放抑制锁，足以覆盖 MFT 变更探测与 AutoImportManager 的 3s 防抖窗口冲突期
    QTimer::singleShot(2000, []() {
        MetadataManager::instance().setInternalOperating(false);
    });
}

void ContentPanel::onDoubleClicked(const QModelIndex& index) { 
    if (!index.isValid()) return; 
 
    // 2026-06-xx 重构逻辑：优先处理子分类跳转 
    int catId = index.data(CategoryIdRole).toInt(); 
    if (catId > 0) { 
        emit categoryClicked(catId); 
        return; 
    } 
 
    QString path = index.data(PathRole).toString(); 
    if (path.isEmpty()) return; 
 
    QFileInfo info(path); 
    if (info.isDir()) { 
        emit directorySelected(path);  
    } else { 
        QDesktopServices::openUrl(QUrl::fromLocalFile(path)); 
    } 
} 
 
void ContentPanel::loadDirectory(const QString& path, bool recursive) { 
    m_isLoading = true;
    int reqId = ++m_loadRequestId;
    m_currentCategoryType = ""; // 物理导航模式下清除系统类型
    ArcMeta::Logger::log(QString("[Content] 开始物理递归扫描 (虚拟化) [%1] -> %2 (%3)")
                        .arg(reqId).arg(path).arg(recursive ? "递归" : "单级"));
    emit dataSourceChanged("nav"); 
    if (m_viewStack) m_viewStack->show(); 
    if (m_textPreview) m_textPreview->hide(); 
    if (m_imagePreview) m_imagePreview->hide(); 
 
    m_isRecursive = recursive; 
    if (m_btnLayers) m_btnLayers->setChecked(recursive); 
 
    if (path.isEmpty() || path == "computer://") { 
        m_currentPath = "computer://"; 
        updateLayersButtonState(); 
 
        const auto drives = QDir::drives(); 
        std::vector<ItemRecord> driveRecords;
        for (const QFileInfo& drive : drives) { 
            driveRecords.push_back(ContentPanel::createItemRecord(drive.absolutePath()));
        } 
        m_model->setRecords(driveRecords);
        // 2026-05-29 物理对齐：在加载“此电脑”后显式触发一次排序，确保置顶硬盘排在首位
        m_proxyModel->sort(0, Qt::AscendingOrder);
        m_isLoading = false;
        recalculateAndEmitStats();
        return; 
    } 
 
    m_currentPath = path; 
    updateLayersButtonState(); 

    // 2026-07-xx 按照 Plan-119：记录最近访问历史（打开即记录）
    AutoImportManager::recordRecentVisitedFolder(path.toStdWString());

    // 2026-07-xx 按照 Plan-116：检测是否导航进入托管库内部
    std::wstring wp = path.toStdWString();
    std::wstring volSerial = MetadataManager::getVolumeSerialNumber(wp);
    QString key = QString("ManagedFolder/Volume_%1").arg(QString::fromStdWString(volSerial));
    QString relPath = AppConfig::instance().getValue(key, "").toString();
    bool isInsideLibrary = false;
    if (!relPath.isEmpty()) {
        QString drive = path.left(3);
        QString managedAbs = QDir::toNativeSeparators(drive + relPath).toLower();
        if (path.toLower().startsWith(managedAbs)) isInsideLibrary = true;
    }

    QPointer<ContentPanel> panelPtr(this); 
    
    // 镜像加载模式（加速）
    if (isInsideLibrary && !recursive) {
        (void)QtConcurrent::run([panelPtr, path, reqId]() {
            if (!panelPtr) return;
            
            // Plan-124: 利用树级索引实现 O(1) 检索，并仅持有锁获取副本，消除锁争用
            auto children = MetadataManager::instance().getChildrenFromCache(path.toStdWString());
            
            std::vector<ItemRecord> allItems;
            allItems.reserve(children.size());
            for (const auto& pair : children) {
                // 利用重构后的 createItemRecord 传入已拉取的元数据，实现真正的零 I/O
                allItems.push_back(ContentPanel::createItemRecord(QString::fromStdWString(pair.first), &pair.second));
            }

            QMetaObject::invokeMethod(QCoreApplication::instance(), [panelPtr, allItems, reqId]() {
                if (panelPtr && panelPtr->m_loadRequestId == reqId) {
                    panelPtr->m_model->setRecords(allItems);
                    panelPtr->m_proxyModel->sort(0, Qt::AscendingOrder);
                    panelPtr->m_isLoading = false;
                    panelPtr->recalculateAndEmitStats();
                    panelPtr->applyFilters();
                    ArcMeta::Logger::log(QString("[Content] 托管库镜像加载完成 [%1]").arg(reqId));
                }
            });
        });
        return;
    }

    // 物理扫描模式（原逻辑）
    (void)QThreadPool::globalInstance()->start([panelPtr, path, recursive, reqId]() { 
        if (!panelPtr) return; 
         
        std::vector<ItemRecord> allItems;
 
        std::function<void(const QString&, bool)> scanDir; 
        scanDir = [&](const QString& p, bool rec) { 
            QDir dir(p); 
            if (!dir.exists()) return; 
 
            QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name); 
            for (const QFileInfo& info : entries) { 
                if (!panelPtr) return; 
                if (info.fileName() == "metadata.scch" || info.fileName() == "metadata.scch.tmp") continue; 
 
                QString absPath = info.absoluteFilePath();
                allItems.push_back(ContentPanel::createItemRecord(absPath));
 
                if (rec && info.isDir()) { 
                    scanDir(absPath, true); 
                } 
            } 
        }; 
 
        scanDir(path, recursive); 
        if (!panelPtr) return; 
 
        QMetaObject::invokeMethod(QCoreApplication::instance(), [panelPtr, path, allItems, reqId]() { 
            if (panelPtr && panelPtr->m_loadRequestId == reqId) { 
                panelPtr->m_model->setRecords(allItems);
                panelPtr->m_proxyModel->sort(0, Qt::AscendingOrder);
                panelPtr->m_isLoading = false;
                panelPtr->recalculateAndEmitStats();
                // 2026-06-xx 物理同步：数据加载完成后强制重新应用筛选，防止显示已过滤掉的占位符记录
                panelPtr->applyFilters();

                // 2026-07-xx 按照 Plan-66：处理新建项后的自动定位与编辑
                if (!panelPtr->m_pendingSelectName.isEmpty()) {
                    const auto& records = panelPtr->m_model->allRecords();
                    for (size_t i = 0; i < records.size(); ++i) {
                        if (QFileInfo(records[i].path).fileName() == panelPtr->m_pendingSelectName) {
                            QModelIndex srcIdx = panelPtr->m_model->index(static_cast<int>(i), 0);
                            QModelIndex proxyIdx = panelPtr->m_proxyModel->mapFromSource(srcIdx);
                            if (proxyIdx.isValid()) {
                                if (panelPtr->m_viewStack->currentWidget() == panelPtr->m_gridView) {
                                    panelPtr->m_gridView->scrollTo(proxyIdx);
                                    panelPtr->m_gridView->setCurrentIndex(proxyIdx);
                                    if (panelPtr->m_isPendingEdit) panelPtr->m_gridView->edit(proxyIdx);
                                } else {
                                    panelPtr->m_treeView->scrollTo(proxyIdx);
                                    panelPtr->m_treeView->setCurrentIndex(proxyIdx);
                                    if (panelPtr->m_isPendingEdit) panelPtr->m_treeView->edit(proxyIdx);
                                }
                            }
                            break;
                        }
                    }
                    panelPtr->m_pendingSelectName = ""; // 必须物理清空状态
                }

                ArcMeta::Logger::log(QString("[Content] 目录扫描完成并已应用到 UI [%1]").arg(reqId));
            } else if (panelPtr) {
                ArcMeta::Logger::log(QString("[Content] 拦截到过期的目录扫描回调 [%1], 当前 ID: %2").arg(reqId).arg(panelPtr->m_loadRequestId.load()));
            }
        }, Qt::QueuedConnection); 
    }); 
} 
 
 
 
 
void ContentPanel::search(const QString& query) { 
    // 2026-07-xx 按照 Plan-118：搜索行为回归筛选流。
    // 搜索框仅作为当前视图的本地过滤器，禁止切换 m_currentCategoryType 为 "search"。
    
    // 1. 同步关键词到当前筛选状态
    m_currentFilter.keyword = query;

    // 2. 触发本地过滤（invalidateFilter）
    applyFilters();

    // 3. 视觉状态同步
    if (m_textPreview) m_textPreview->hide(); 
    if (m_imagePreview) m_imagePreview->hide(); 
    if (m_viewStack) m_viewStack->show(); 

    ArcMeta::Logger::log(QString("[Search] 本地搜索关键词更新: %1 (当前视图类型: %2)")
                        .arg(query).arg(m_currentCategoryType.isEmpty() ? "nav" : m_currentCategoryType));
} 
 
void ContentPanel::applyFilters(const FilterState& state) { 
    // 2026-07-xx 物理防护：保留标题栏按钮独占维护的显隐状态，防止被 FilterPanel 的默认值覆盖
    bool preservedShowFolders = m_currentFilter.showFolders;
    bool preservedShowFiles = m_currentFilter.showFiles;
    m_currentFilter = state; 
    m_currentFilter.showFolders = preservedShowFolders;
    m_currentFilter.showFiles = preservedShowFiles;
    applyFilters(); 
} 
 
void ContentPanel::applyFilters() { 
    // 2026-05-25 编译修复：改用 qobject_cast 彻底根除 static_cast 指针转换报错 
    auto* proxy = qobject_cast<FilterProxyModel*>(m_proxyModel); 
    if (proxy) { 
        proxy->currentFilter = m_currentFilter; 
        proxy->updateFilter(); 
    } 
    // 2026-05-08 按照用户要求：筛选条件变化后更新状态栏统计
    updateStatusBarStats();
} 
 
void ContentPanel::previewFile(const QString& path) { 
    // 2026-03-xx 按照用户要求：全能预览实现，支持图片与多种文本格式，破除 .md 局限 
    QFileInfo info(path); 
    QString ext = info.suffix().toLower(); 
 
    // 1. 图片格式识别 
    static const QStringList imageExts = {"jpg", "jpeg", "png", "bmp", "webp", "gif", "ico"}; 
    if (imageExts.contains(ext)) { 
        QPixmap pix(path); 
        if (!pix.isNull()) { 
            m_viewStack->hide(); 
            m_textPreview->hide(); 
             
            // 保持比例缩放显示 
            m_imagePreview->setPixmap(pix.scaled(m_imagePreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)); 
            m_imagePreview->show(); 
            return; 
        } 
    } 
 
    // 2. 文本格式识别 (参考版本A 扩展识别) 
    // 此处可根据需要进一步细化，目前先处理常规文本 
    QFile file(path); 
    if (file.open(QIODevice::ReadOnly)) { 
        m_viewStack->hide(); 
        m_imagePreview->hide(); 
 
        // 针对 Markdown 特殊渲染 
        if (ext == "md" || ext == "markdown") { 
             m_textPreview->setMarkdown(file.readAll()); 
        } else { 
             // 针对其他代码或文本，直接显示原文 
             // 限制读取前 1MB 以防大文件卡死 
             m_textPreview->setPlainText(QString::fromUtf8(file.read(1024 * 1024))); 
        } 
        m_textPreview->show(); 
        file.close(); 
    } 
} 
 
void ContentPanel::loadCategory(int categoryId) { 
    // 2026-07-xx 物理防护：防重入机制。如果已经在加载同一个分类，则直接拦截，防止重复 clear() 导致的闪烁
    if (m_isLoading && m_currentCategoryId == categoryId && m_currentCategoryType == "user_category") {
        return;
    }

    m_isLoading = true;
    int reqId = ++m_loadRequestId;
    m_currentCategoryType = "user_category";
    m_currentCategoryId = categoryId;
    updateLayersButtonState();
    m_viewStack->show(); 
    if (m_textPreview) m_textPreview->hide(); 
    if (m_imagePreview) m_imagePreview->hide(); 
    emit dataSourceChanged("category"); 
     
    QPointer<ContentPanel> weakThis(this);
    (void)QtConcurrent::run([weakThis, categoryId, reqId]() {
        std::vector<ItemRecord> allRecords;

        // 1. 加载子分类
        auto allCategories = CategoryRepo::getAll();
        for (const auto& cat : allCategories) {
            if (cat.parentId == categoryId) {
                ItemRecord r;
                r.isCategory = true;
                r.categoryId = cat.id;
                r.categoryName = QString::fromStdWString(cat.name);
                r.categoryColor = QString::fromStdWString(cat.color).isEmpty() ? "#aaaaaa" : QString::fromStdWString(cat.color);
                // 2026-07-xx 物理同步：从内存缓存或元数据管理器中拉取分类的评分（如有）
                r.rating = 0; 
                r.pinned = cat.pinned;
                allRecords.push_back(r);
            }
        }

        // 2. 加载文件 (SCCH 分离模式)
        std::vector<CategoryItem> items;
        if (weakThis->m_isCategoryRecursive) {
            items = CategoryRepo::getItemsRecursive(categoryId);
        } else {
            items = CategoryRepo::getItemsInCategory(categoryId);
        }
        
        allRecords.reserve(allRecords.size() + items.size());
        for (const auto& item : items) {
            if (!weakThis) return;
            std::wstring wPath = MetadataManager::instance().getPathByFid(item.fileId128);
            if (wPath.empty() && !item.pathHint.empty()) {
                wPath = item.pathHint; 
            }

            if (!wPath.empty()) {
                allRecords.push_back(ContentPanel::createItemRecord(QString::fromStdWString(wPath)));
            }
        }

        QMetaObject::invokeMethod(QCoreApplication::instance(), [weakThis, allRecords, reqId]() {
            if (weakThis && weakThis->m_loadRequestId == reqId) {
                weakThis->m_model->setRecords(allRecords);
                weakThis->m_proxyModel->sort(0, Qt::AscendingOrder);
                weakThis->m_isLoading = false;
                weakThis->recalculateAndEmitStats();
                weakThis->applyFilters(); 
                ArcMeta::Logger::log(QString("[Content] 分类加载完成 [%1]").arg(reqId));
            } else if (weakThis) {
                ArcMeta::Logger::log(QString("[Content] 拦截到过期的分类加载回调 [%1]").arg(reqId));
            }
        });
    });
} 
 
void ContentPanel::loadPaths(const QStringList& paths, int reqId) { 
    // 2026-07-xx 物理强化：如果路径列表为空，直接执行同步清理并返回
    // 理由：这防止了搜索启动时的清空动作（异步）与随后到达的结果加载（异步）发生竞态。
    if (paths.isEmpty()) {
        ArcMeta::Logger::log("[Content] loadPaths 收到空路径，执行同步清空");
        if (reqId == 0) m_loadRequestId++; // 若未指定 ID，则自增以作废前序加载
        else m_loadRequestId = reqId;      // 若指定了 ID，则强制对其
        
        m_model->clear();
        m_isLoading = false;
        recalculateAndEmitStats();
        return;
    }

    // 校验：如果传入了明确的 reqId，且与当前 ID 不符，则直接拦截。
    // 这对于搜索结果的流式加载至关重要。
    if (reqId != 0 && m_loadRequestId != reqId) {
        ArcMeta::Logger::log(QString("[Content] loadPaths 拦截到过期的同步请求 [%1], 当前 ID: %2")
                            .arg(reqId).arg(m_loadRequestId.load()));
        return;
    }

    // 2026-07-xx 物理防护：防重入机制
    if (m_isLoading && m_currentCategoryType == "path_list" && reqId == 0) {
        return;
    }

    m_isLoading = true;
    if (reqId == 0) reqId = ++m_loadRequestId;
    // 2026-07-xx 逻辑校准：保持既有的系统分类类型（如 trash/recently_visited），
    // 仅在明确不是这些特殊类型时，才将其降级为通用的 path_list。
    if (m_currentCategoryType != "trash" && 
        m_currentCategoryType != "recently_visited" &&
        m_currentCategoryType != "untagged" &&
        m_currentCategoryType != "uncategorized" &&
        m_currentCategoryType != "all") {
        m_currentCategoryType = "path_list";
    }
    updateLayersButtonState();
    
    m_viewStack->show(); 
    if (m_textPreview) m_textPreview->hide(); 
    if (m_imagePreview) m_imagePreview->hide(); 
    
    // 加载路径列表通常属于分类/逻辑数据源
    emit dataSourceChanged("category"); 
     
    QPointer<ContentPanel> weakThis(this);
    (void)QtConcurrent::run([weakThis, paths, reqId]() {
        std::vector<ItemRecord> records;
        records.reserve(static_cast<int>(paths.size()));
        for (const QString& p : paths) {
            if (!weakThis) return;
            if (!p.isEmpty()) {
                records.push_back(ContentPanel::createItemRecord(p));
            }
        }
        
        QMetaObject::invokeMethod(QCoreApplication::instance(), [weakThis, records, reqId]() {
            if (weakThis && weakThis->m_loadRequestId == reqId) {
                weakThis->m_model->setRecords(records);
                weakThis->m_proxyModel->sort(0, Qt::AscendingOrder);
                weakThis->m_isLoading = false;
                weakThis->recalculateAndEmitStats();
                weakThis->applyFilters(); 
                ArcMeta::Logger::log(QString("[Content] 路径列表加载完成 [%1]").arg(reqId));
            } else if (weakThis) {
                ArcMeta::Logger::log(QString("[Content] 拦截到过期的路径列表加载回调 [%1]").arg(reqId));
            }
        });
    });
}

void ContentPanel::appendPaths(const QStringList& paths, int reqId) {
    if (paths.isEmpty()) return;

    // 物理校验：如果指定了请求 ID，则必须与当前 ID 匹配，否则视为过期搜索结果
    if (reqId != 0 && m_loadRequestId != reqId) {
        ArcMeta::Logger::log(QString("[Content] appendPaths 拦截到过期的异步追加请求 [%1], 当前 ID: %2")
                            .arg(reqId).arg(m_loadRequestId.load()));
        return;
    }

    QPointer<ContentPanel> weakThis(this);
    (void)QtConcurrent::run([weakThis, paths, reqId]() {
        std::vector<ItemRecord> newRecords;
        newRecords.reserve(static_cast<int>(paths.size()));
        for (const QString& p : paths) {
            if (!weakThis) return;
            newRecords.push_back(ContentPanel::createItemRecord(p));
        }

        QMetaObject::invokeMethod(QCoreApplication::instance(), [weakThis, newRecords, reqId]() {
            if (weakThis && (reqId == 0 || weakThis->m_loadRequestId == reqId)) {
                // 获取当前已有记录并追加
                std::vector<ItemRecord> all = weakThis->m_model->allRecords();
                all.insert(all.end(), newRecords.begin(), newRecords.end());
                weakThis->m_model->setRecords(all);
                
                // 异步流式追加时，每批次都尝试更新一次统计与筛选
                weakThis->recalculateAndEmitStats();
                weakThis->applyFilters();
                ArcMeta::Logger::log(QString("[Content] 异步追加了 %1 条路径 [%2]").arg(newRecords.size()).arg(reqId));
            } else if (weakThis) {
                ArcMeta::Logger::log(QString("[Content] appendPaths 在回调阶段拦截到过期结果 [%1]").arg(reqId));
            }
        });
    });
}
 
void ContentPanel::recalculateAndEmitStats() {
    const std::vector<ItemRecord>& records = m_model->allRecords();
    if (records.empty()) {
        // 2026-06-xx 物理修复：严禁向筛选面板发送“全空”统计信号，
        // 防止在加载大目录或执行搜索切换的中间态强行清空筛选器界面。
        return;
    }

    QPointer<ContentPanel> weakThis(this);
    (void)QtConcurrent::run([weakThis, records]() {
        ScanStats stats;

        for (const auto& record : records) {
            if (!weakThis) return;

            stats.ratingCounts[record.rating]++;
            
            if (!record.color.isEmpty()) {
                stats.colorCounts[record.color.toUpper()]++;
            } else {
                stats.colorCounts[""]++;
            }
            
            if (record.isDir || record.isCategory) {
                stats.typeCounts["folder"]++;
                if (record.isDir && record.isEmpty) {
                    stats.emptyFolderCount++;
                }
            } else {
                stats.typeCounts["file"]++;
                stats.typeCounts[record.suffix.toUpper()]++;
            }
            
            auto dateKey = [&](long long ts) {
                return QDateTime::fromMSecsSinceEpoch(ts).date().toString("dd-MM-yyyy");
            };

            stats.createDateCounts[dateKey(record.ctime)]++;
            stats.modifyDateCounts[dateKey(record.mtime)]++;
        }

        QMetaObject::invokeMethod(QCoreApplication::instance(), [weakThis, stats]() {
            if (weakThis) {
                emit weakThis->directoryStatsReady(stats.ratingCounts, stats.colorCounts,
                                                 stats.typeCounts, stats.createDateCounts, stats.modifyDateCounts,
                                                 stats.emptyFolderCount);
            }
        });
    });
}

void ContentPanel::createNewItem(const QString& type) { 
    if (m_currentPath.isEmpty() || m_currentPath == "computer://") return; 
 
    QString baseName = (type == "folder") ? "新建文件夹" : "未命名"; 
    QString ext = (type == "md") ? ".md" : ((type == "txt") ? ".txt" : ""); 
    QString finalName = baseName + ext; 
    QString fullPath = m_currentPath + "/" + finalName; 
 
    int counter = 1; 
    while (QFileInfo::exists(fullPath)) { 
        finalName = baseName + QString(" (%1)").arg(counter++) + ext; 
        fullPath = m_currentPath + "/" + finalName; 
    } 
 
    bool success = false; 
    if (type == "folder") { 
        success = QDir(m_currentPath).mkdir(finalName); 
    } else { 
        QFile file(fullPath); 
        if (file.open(QIODevice::WriteOnly)) { 
            file.close(); 
            success = true; 
        } 
    } 
 
    if (success) { 
        m_pendingSelectName = finalName;
        m_isPendingEdit = true;
        loadDirectory(m_currentPath, m_isRecursive); 
    } 
} 
 
void ContentPanel::updateLayersButtonState() { 
    if (!m_btnLayers || !m_btnLayersBlue) return; 
 
    // 2026-07-xx 互斥逻辑：分类视图下显示蓝按钮，物理路径下显示绿按钮
    bool isCategoryMode = (m_currentCategoryType == "user_category");
    m_btnLayers->setVisible(!isCategoryMode);
    m_btnLayersBlue->setVisible(isCategoryMode);

    if (isCategoryMode) {
        m_btnLayersBlue->setEnabled(true);
        m_btnLayersBlue->setChecked(m_isCategoryRecursive);
        m_btnLayersBlue->setProperty("tooltipText", "显示子分类中的项目");
        return;
    }

    if (m_currentPath.isEmpty() || m_currentPath == "computer://") { 
        m_btnLayers->setEnabled(false); 
        m_btnLayers->setChecked(false); 
        m_btnLayers->setProperty("tooltipText", "“此电脑”不支持递归显示"); 
        return; 
    } 

    // 2026-07-xx 逻辑增强：若处于搜索或其他路径列表模式，禁用递归功能
    // 注意：m_currentCategoryType 为空时代表正常的物理目录导航（nav 模式）
    if (!m_currentCategoryType.isEmpty() && m_currentCategoryType != "nav") {
        m_btnLayers->setEnabled(false);
        m_btnLayers->setChecked(false);
        m_btnLayers->setProperty("tooltipText", "当前视图不支持递归显示");
        return;
    }
 
    m_btnLayers->setEnabled(true); 
    m_btnLayers->setProperty("tooltipText", "显示子文件夹中的项目"); 
} 
 
// --- Delegate --- 
 
GridItemDelegate::GridMetrics GridItemDelegate::calculateMetrics(const QStyleOptionViewItem& option) { 
    GridMetrics m; 
    // cardRect 为条目占用的总区域 
    m.cardRect = option.rect.adjusted(5, 5, -5, -5); // 2026-05-08 按照用户要求：增加到5px使卡片之间间距达到10px 
    double zoom = (double)option.decorationSize.width(); 
 
    // 2026-06-05 按照用户要求：定义正方形主体区域 
    int side = m.cardRect.width(); 
    m.squareRect = QRect(m.cardRect.left(), m.cardRect.top(), side, side); 
 
    m.iconDrawSize = (int)(zoom * 0.7); 
    m.ratingH      = 24;  // 2026-05-17 物理锁定评分区高度 
    m.nameH        = (int)(zoom * 0.25); 
 
    // 2026-05-17 按照用户要求：主图标在正方形内垂直居中
    m.iconRect = QRect(m.squareRect.left() + (m.squareRect.width() - m.iconDrawSize) / 2, 
                       m.squareRect.top() + (m.squareRect.height() - m.iconDrawSize) / 2, 
                       m.iconDrawSize, m.iconDrawSize); 
    
    // 2026-05-17 物理布局重构：星级区置于正方形下方
    // 2026-06-xx 按照用户要求：下移 2 像素（从 +6 改为 +8）
    m.ratingY = m.squareRect.bottom() + 8; 
 
    m.starSize    = 22; // 2026-05-17 尺寸微调以匹配外部布局
    m.starSpacing = 0;   
    int banW = 14;      // 2026-06-xx 物理对齐：将禁止图标缩减至 12px，使其与星级在视觉权重上保持一致
    int banGap = 2; 
 
    m.infoTotalW = banW + banGap + (5 * m.starSize) + (4 * m.starSpacing); 
    m.infoStartX = m.squareRect.left() + (m.squareRect.width() - m.infoTotalW) / 2; 
 
    m.banRect = QRect(m.infoStartX, m.ratingY + (m.ratingH - banW) / 2, banW, banW); 
    m.starsStartX = m.infoStartX + banW + banGap; 
     
    // 名称区紧贴评分区下方 
    // 2026-06-xx 按照要求：第三次向上偏移 2 像素以实现零间距极致排版（从 +2 调整为 +0）
    m.nameY = m.ratingY + m.ratingH + 0; 
    m.nameRect = QRect(m.cardRect.left(), m.nameY, m.cardRect.width(), m.nameH); 
 
    return m; 
} 
 
void GridItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const { 
    painter->save(); 
    painter->setRenderHint(QPainter::Antialiasing); 
 
    GridMetrics m = calculateMetrics(option); 
    bool isSelected = (option.state & QStyle::State_Selected); 
    bool isHovered = (option.state & QStyle::State_MouseOver); 
    QString colorName = index.data(ColorRole).toString();
     
    // 2026-06-05 按照用户要求：背景框仅限正方形区域，名称外置 
    QColor cardBg = isSelected ? BackgroundSelected : (isHovered ? BackgroundHover : BackgroundDeep); 
    painter->setPen(isSelected ? QPen(PrimaryBlue, 2) : QPen(BorderColor, 1)); 
    painter->setBrush(cardBg); 
    painter->drawRoundedRect(m.squareRect, 8, 8); 
 
    // [内容区绘制] - 均在正方形内 
    // 1. 状态位图标绘制 (置顶 vs. 进度环 vs. 已录入 互斥) 
    // 2026-06-xx 物理修复：校准 ItemRole 作用域，确保 GridItemDelegate 编译通过 
    bool isPinned = index.data(IsLockedRole).toBool(); 
    bool isManaged = index.data(ManagedRole).toBool(); 
    bool isDir = index.data(TypeRole).toString() == "folder";
    double progress = index.data(RegistrationProgressRole).toDouble();
     
    QRect statusRect(m.squareRect.right() - 22, m.squareRect.top() + 8, 16, 16);
    if (isPinned) { 
        // 置顶优先 
        QIcon pinIcon = UiHelper::getIcon("pin_vertical", Style::ActiveOrange, 16); 
        pinIcon.paint(painter, statusRect); 
    } else if (isDir && progress >= 0.0 && progress < 1.0) {
        // --- 绘制进度环 (开箱即用代码) --- 
        // 2026-07-xx 按照 Development_Plan 3.1：进度弧线完全通过数据库中的 0 和 1 标记值计算得出
        painter->save(); 
        painter->setRenderHint(QPainter::Antialiasing); 
         
        // 1. 底环 
        painter->setPen(QPen(QColor(60, 60, 60, 180), 2)); 
        painter->drawEllipse(statusRect.adjusted(1, 1, -1, -1)); 
         
        // 2. 进度弧 (品牌蓝 #3498db) 
        QPen pPen(QColor("#3498db"), 2); 
        pPen.setCapStyle(Qt::RoundCap); 
        painter->setPen(pPen); 
         
        int spanAngle = -qRound(progress * 360 * 16); // 逆时针计算 
        painter->drawArc(statusRect.adjusted(1, 1, -1, -1), 90 * 16, spanAngle); 
        painter->restore(); 
    } else if (isManaged || (isDir && progress >= 1.0)) { 
        // 已录入但未置顶，显示绿对勾 
        QIcon checkIcon = UiHelper::getIcon("check_circle", SuccessGreen, 16); 
        checkIcon.paint(painter, statusRect); 
    } 
 
    // 2. 扩展名角标 
    QString type = index.data(TypeRole).toString();
    QString path = index.data(PathRole).toString(); 
    QFileInfo info(path); 
    QString ext;
    if (type == "category" || type == "folder") {
        ext = "DIR"; // 2026-06-xx 物理校准：分类与文件夹均强制显示为 "DIR" 徽章
    } else {
        ext = info.isDir() ? "DIR" : info.suffix().toUpper(); 
    }
    if (ext.isEmpty()) ext = "FILE"; 
    
    // 2026-06-xx 物理性能优化：优先从 record.suffix 中提取徽章文字，减少 QFileInfo 频率
    // 2026-06-xx 物理加固：修复代理模型下的索引映射错误，杜绝数组越界 (c0000005)
    QModelIndex srcIdx = index;
    const auto* proxy = qobject_cast<const QSortFilterProxyModel*>(index.model());
    if (proxy) srcIdx = proxy->mapToSource(index);
    
    const auto* srcModel = qobject_cast<const FerrexVirtualDbModel*>(srcIdx.model());
    if (srcModel && srcIdx.row() >= 0 && srcIdx.row() < (int)srcModel->allRecords().size()) {
        const auto& record = srcModel->allRecords()[srcIdx.row()];
        if (record.isCategory || record.isDir) ext = "DIR";
        else if (!record.suffix.isEmpty()) ext = record.suffix.toUpper();
    }

    QColor badgeColor = UiHelper::getExtensionColor(ext); 
 
    QRect extRect(m.squareRect.left() + 8, m.squareRect.top() + 8, 36, 18); 
    painter->setPen(Qt::NoPen); 
    painter->setBrush(badgeColor); 
    painter->drawRoundedRect(extRect, 2, 2); 
    painter->setPen(QColor("#FFFFFF")); 
    QFont extFont = painter->font(); 
    extFont.setPointSize(8); 
    extFont.setBold(true); 
    painter->setFont(extFont); 
    painter->drawText(extRect, Qt::AlignCenter, ext); 
 
    // 3. 主图标 
    QIcon icon = index.data(Qt::DecorationRole).value<QIcon>(); 
    icon.paint(painter, m.iconRect); 
 
    // 4. 评级星级 
    int rating = index.data(RatingRole).toInt(); 
     
    // 2026-06-xx 逻辑重构：彩色胶囊背景由 colorName 独立驱动，不与星级耦合
    if (!colorName.isEmpty()) {
        QColor bgColor = UiHelper::parseColorName(colorName);
        if (bgColor.isValid()) {
            painter->save();
            painter->setBrush(bgColor);
            painter->setPen(Qt::NoPen);
            // 即使星级为0，也应根据占位计算胶囊区域并绘制
            QRect lastStarRect(m.starsStartX + 4 * (m.starSize + m.starSpacing), m.ratingY + (m.ratingH - m.starSize) / 2, m.starSize, m.starSize);
            QRect totalRect = m.banRect.united(lastStarRect);
            painter->drawRoundedRect(totalRect.adjusted(-4, -1, 4, 1), 4, 4);
            painter->restore();
        }
    }

    // 2026-xx-xx 按照最新要求： 
    // 1. 如果已有打分 (rating > 0)，始终显示。 
    // 2. 如果未打分但被选中，显示禁止图标和空心星。 
    // 3. 如果未打分且未选中，不显示。 
    bool shouldShowRating = (rating > 0) || isSelected; 
 
    if (shouldShowRating) { 
        QColor bgColor = colorName.isEmpty() ? QColor(0,0,0,0) : UiHelper::parseColorName(colorName);
        
        // 2026-06-xx 物理修复：采用感知亮度对比色计算，确保在深色标记（如灰色/深蓝）下星星依然清晰可见
        double luminance = 0.0;
        if (bgColor.isValid() && bgColor.alpha() > 0) {
            luminance = (0.299 * bgColor.red() + 0.587 * bgColor.green() + 0.114 * bgColor.blue()) / 255.0;
        }

        QColor starColor, emptyStarColor;
        if (colorName.isEmpty()) {
            starColor      = QColor("#CCCCCC");
            emptyStarColor = QColor("#888888");
        } else if (luminance < 0.5) {
            // 背景较暗 -> 使用亮色星
            starColor      = QColor("#FFFFFF");
            emptyStarColor = QColor(255, 255, 255, 160);
        } else {
            // 背景较亮 -> 使用暗色星
            starColor      = QColor("#1A1A1A");
            emptyStarColor = QColor(0, 0, 0, 140);
        }

        // 2026-xx-xx 深度修复：调高禁止图标与空心星亮度，确保在深色卡片背景下清晰可见 
        QIcon banIcon = UiHelper::getIcon("no_color", starColor, m.banRect.width()); 
        banIcon.paint(painter, m.banRect); 
 
        QPixmap filledStar = UiHelper::getPixmap("star-svgrepo-com.svg", QSize(m.starSize, m.starSize), starColor); 
        QPixmap emptyStar = UiHelper::getPixmap("star-rate-rating-outline-svgrepo-com.svg", QSize(m.starSize, m.starSize), emptyStarColor); 
 
        for (int i = 0; i < 5; ++i) { 
            QRect starRect(m.starsStartX + i * (m.starSize + m.starSpacing), m.ratingY + (m.ratingH - m.starSize) / 2, m.starSize, m.starSize); 
            painter->drawPixmap(starRect, (i < rating) ? filledStar : emptyStar); 
        } 
    } 
     
    // [名称区绘制] - 在正方形下方 
    bool isEmptyFolder = (index.data(TypeRole).toString() == "folder" && index.data(IsEmptyRole).toBool()); 
 
    if (!isSelected && isEmptyFolder) { 
        // 2026-06-xx 物理优化：移除半透明蒙版，改为全透明刷子
        painter->setPen(QPen(AccentCyan, 1, Qt::DashLine)); 
        painter->setBrush(Qt::NoBrush); // 直接将其透明 
        painter->drawRoundedRect(m.squareRect, 8, 8); 
        painter->setPen(AccentCyan); 
    } 
 
    QString name = index.data(Qt::DisplayRole).toString(); 
    painter->setPen(QColor("#EEEEEE")); 
    QFont textFont = painter->font(); 
    textFont.setPointSize(8); 
    textFont.setBold(false); 
    painter->setFont(textFont); 
 
    // 2026-06-xx 按照要求：未录入项文字半透明 
    // 2026-06-xx 物理修复：校准 ManagedRole 作用域 
    if (!isSelected && !index.data(ManagedRole).toBool()) { 
        painter->setPen(QColor(238, 238, 238, 120)); 
    } 
 
    // 2026-06-05 按照用户要求：恢复自动换行逻辑，并在“实在太长”时使用省略号 
    // 零宽空格注入以支持非标准断行（如下划线或点号） 
    QString displayName = name; 
    displayName.replace("_", "_\u200B"); 
    displayName.replace(".", ".\u200B"); 
 
    // 首先尝试进行自动换行绘制，如果高度超出，则回退到省略号单行显示 
    QRect boundingRect = painter->boundingRect(m.nameRect.adjusted(4, 0, -4, 0), Qt::AlignCenter | Qt::TextWordWrap, displayName); 
    if (boundingRect.height() > m.nameRect.height()) { 
        QString elidedName = option.fontMetrics.elidedText(name, Qt::ElideRight, m.nameRect.width() - 8); 
        painter->drawText(m.nameRect, Qt::AlignCenter, elidedName); 
    } else { 
        painter->drawText(m.nameRect.adjusted(4, 0, -4, 0), Qt::AlignCenter | Qt::TextWordWrap, displayName); 
    } 
 
    painter->restore(); 
} 
 
 
bool GridItemDelegate::eventFilter(QObject* obj, QEvent* event) { 
    if (event->type() == QEvent::KeyPress) { 
        // 2026-05-25 物理修复：改用 reinterpret_cast 避开事件转型报错 
        QKeyEvent* keyEvent = reinterpret_cast<QKeyEvent*>(event); 
        QLineEdit* editor = qobject_cast<QLineEdit*>(obj); 
        if (editor) { 
            switch (keyEvent->key()) { 
                case Qt::Key_Left: 
                case Qt::Key_Right: 
                case Qt::Key_Up: 
                case Qt::Key_Down: 
                case Qt::Key_Home: 
                case Qt::Key_End: 
                    keyEvent->accept(); 
                    return false; 
                default: 
                    break; 
            } 
        } 
    } 
    return QStyledItemDelegate::eventFilter(obj, event); 
} 
 
bool GridItemDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view, 
                                const QStyleOptionViewItem& option, const QModelIndex& index) {
    GridMetrics m = calculateMetrics(option);
    QRect statusRect(m.squareRect.right() - 22, m.squareRect.top() + 8, 16, 16);

    if (statusRect.contains(event->pos())) {
        double p = index.data(RegistrationProgressRole).toDouble();
        if (p >= 0.0) {
            // 2026-07-xx 按照 Plan-65：悬停触发，timeout = 0
            ToolTipOverlay::instance()->showText(event->globalPos(), 
                QString("登记进度: %1%").arg(qRound(p * 100)), 0);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

bool GridItemDelegate::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) { 
    if (event->type() == QEvent::MouseButtonPress) { 
        // 2026-05-25 物理修复：改用 reinterpret_cast 避开 QEvent 子类转型歧义 
        QMouseEvent* mEvent = reinterpret_cast<QMouseEvent*>(event); 
        if (mEvent->button() == Qt::LeftButton) { 
            QAbstractItemView* view = qobject_cast<QAbstractItemView*>(const_cast<QWidget*>(option.widget));
            
            // 物理加固：未选中项严禁直接通过 Delegate 修改元数据
            // 2026-06-xx 稳健性增强：通过 View 获取实时的选中状态，防止 option.state 延迟或缺失
            bool isSelected = (option.state & QStyle::State_Selected);
            if (view && view->selectionModel()) {
                isSelected = view->selectionModel()->isSelected(index);
            }
            if (!isSelected) return false;

            // 2026-05-28 按照用户授权：废除本地硬编码判定，统一使用 calculateMetrics 保证 Hitbox 零偏差 
            // 2026-06-xx 物理对齐：补全 decorationSize，防止因 option 属性缺失导致 Hitbox 偏移
            QStyleOptionViewItem opt = option;
            if (opt.decorationSize.width() <= 0 && view) opt.decorationSize = view->iconSize();
            GridMetrics m = calculateMetrics(opt); 
 
            // 1. 区域判定
            bool isBanHit = m.banRect.contains(mEvent->pos());
            int hitStar = -1;
            for (int i = 0; i < 5; ++i) { 
                QRect starRect(m.starsStartX + i * (m.starSize + m.starSpacing), m.ratingY + (m.ratingH - m.starSize) / 2, m.starSize, m.starSize); 
                if (starRect.contains(mEvent->pos())) { 
                    hitStar = i + 1;
                    break; 
                } 
            }

            if (isBanHit || hitStar != -1) { 
                // 2. 执行数据更新 (支持选区感知批量操作)
                int newValue = isBanHit ? 0 : hitStar;
                if (view && view->selectionModel() && view->selectionModel()->isSelected(index)) {
                    auto selectedIndexes = view->selectionModel()->selectedIndexes();
                    // 2026-07-xx 按照 Plan-86：遍历选区实现批量评分
                    for (const auto& selIdx : selectedIndexes) {
                        if (selIdx.column() == 0) {
                            model->setData(selIdx, newValue, RatingRole);
                        }
                    }
                } else {
                    model->setData(index, newValue, RatingRole); 
                }

                // 3. 物理修复：直接执行禁用逻辑，杜绝 Lambda 嵌套导致的编译错误
                if (view) {
                    QAbstractItemView::EditTriggers currentTriggers = view->editTriggers();
                    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
                    // 延迟恢复触发器
                    QTimer::singleShot(0, view, [view, currentTriggers]() {
                        view->setEditTriggers(currentTriggers);
                    });
                }
                event->accept(); 
                return true; 
            } 
        } 
    } 
    return QStyledItemDelegate::editorEvent(event, model, option, index); 
} 
 
QWidget* GridItemDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const { 
    Q_UNUSED(option); 
    QLineEdit* editor = new QLineEdit(parent); 
    editor->installEventFilter(const_cast<GridItemDelegate*>(this)); 
    editor->setAlignment(Qt::AlignCenter); 
    editor->setFrame(false); 
     
    QString tagColorStr = index.data(ColorRole).toString(); 
    QString bgColor = tagColorStr.isEmpty() ? "#3E3E42" : tagColorStr; 
    QString textColor = tagColorStr.isEmpty() ? "#FFFFFF" : "#000000"; 
 
    editor->setStyleSheet( 
        QString("QLineEdit { background-color: %1; color: %2; border-radius: 2px; " 
                "border: 2px solid %3; font-weight: bold; font-size: 8pt; padding: 0px; }") 
        .arg(bgColor).arg(textColor).arg(qssColor(PrimaryBlue))
    ); 
    return editor; 
} 
 
void GridItemDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const { 
    QString value = index.model()->data(index, Qt::EditRole).toString(); 
    // 2026-05-25 物理修复：改用 qobject_cast 彻底根除 static_cast 类型无法识别 the Bug 
    QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor); 
    if (!lineEdit) return;

    lineEdit->setText(value); 
     
    // 2026-xx-xx 按照用户要求：如果是文件夹，全选；如果是文件，仅选中包含后缀名之前的部分
    // 物理修复：使用 QTimer 确保在 Qt 默认 selectAll 之后执行，防止逻辑被覆盖
    bool isFolder = (index.data(TypeRole).toString() == "folder" || index.data(TypeRole).toString() == "category");
    QTimer::singleShot(0, lineEdit, [lineEdit, value, isFolder]() {
        if (!lineEdit) return;
        if (isFolder) {
            lineEdit->selectAll();
        } else {
            int lastDot = value.lastIndexOf('.'); 
            if (lastDot > 0) { 
                lineEdit->setSelection(0, lastDot); 
            } else { 
                lineEdit->selectAll(); 
            } 
        }
    });
} 
 
void GridItemDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const { 
    QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor); 
    if (!lineEdit) return; 
    QString value = lineEdit->text(); 
    if(value.isEmpty() || value == index.data(Qt::DisplayRole).toString()) return; 
 
    // 2026-06-xx 架构解耦修复：物理重命名职责已彻底移至 Model 层的 setData。
    // Delegate 仅负责触发数据变更。这消除了“重复重命名”导致的静默失败 Bug。
    if (model->setData(index, value, Qt::EditRole)) {
        // 2026-xx-xx 按照用户要求：重命名后触发 selectionChanged 信号，以驱动元数据面板刷新
        // 由于 setModelData 没有 option 参数，通过 parent 获取 View
        QAbstractItemView* view = qobject_cast<QAbstractItemView*>(editor->parentWidget()->parentWidget());
        if (view) {
            // 向上寻找 ContentPanel 以调用 onSelectionChanged
            QWidget* p = view->parentWidget();
            while (p) {
                ContentPanel* cp = qobject_cast<ContentPanel*>(p);
                if (cp) {
                    cp->onSelectionChanged();
                    break;
                }
                p = p->parentWidget();
            }
        }
    }
} 

 
void GridItemDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const { 
    Q_UNUSED(index); 
    // 2026-06-05 按照重构布局：同步定位编辑器至正方形下方的名称区 
    GridMetrics m = calculateMetrics(option); 
    editor->setGeometry(m.nameRect); 
}

QSize GridItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (auto* view = qobject_cast<QListView*>(const_cast<QWidget*>(option.widget))) {
        if (view->gridSize().isValid()) return view->gridSize();
    }
    return QStyledItemDelegate::sizeHint(option, index);
}
 
} // namespace ArcMeta
// Force recompile to apply SvgIcons.h changes 
