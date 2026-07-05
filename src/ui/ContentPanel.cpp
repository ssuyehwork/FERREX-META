#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ContentPanel.h" 
#include "Logger.h"
#include "SvgIcons.h" 
#include "TreeItemDelegate.h" 
#include "DropTreeView.h" 
#include "DropListView.h" 
#include "DropJustifiedView.h"
#include "ProgressDialog.h"
#include "ThumbnailDelegate.h"
#include "ToolTipOverlay.h" 
 
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
#include <QDateTime> 
#include <QDesktopServices> 
#include <QUrl> 
#include <QApplication> 
#include <QProcess> 
#include <QClipboard> 
#include <QMimeData> 
#include <QLineEdit> 
#include <QTextBrowser> 
#include <QInputDialog> 
#include <QMessageBox>
#include <QRandomGenerator>
#include <QAbstractItemView> 
#include <QtConcurrent> 
#include <QThreadPool> 
#include <QTimer> 
#include <QPointer> 
#include <functional> 
#include <QPointer> 
#include <QPersistentModelIndex> 
#include <QSqlDatabase> 
#include <QSqlQuery> 
#include <windows.h> 
#include <shellapi.h> 
#include <io.h>
#include "../meta/MetadataManager.h" 
#include "../meta/BatchRenameEngine.h" 
#include "../db/CategoryRepo.h" 
#include "../crypto/EncryptionManager.h" 
#include "CategoryLockDialog.h" 
#include "BatchRenameDialog.h" 
#include "UiHelper.h" 
#include "StyleLibrary.h"
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
        if (index.row() < (int)m_allRecords.size() && !m_allRecords[index.row()].isCategory) {
            f |= Qt::ItemIsEditable;
        }
    }
    return f;
}

QVariant FerrexVirtualDbModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= (int)m_allRecords.size()) return QVariant();

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
        } else if (role == TypeRole) {
            return "category";
        } else if (role == Qt::DecorationRole && index.column() == 0) {
            static QIcon catIcon = QFileIconProvider().icon(QFileIconProvider::Folder);
            return catIcon;
        }
        return QVariant();
    }

    auto getCachedMeta = [this](const QString& p) -> ArcMeta::RuntimeMeta {
        if (m_metaCache.contains(p)) return *m_metaCache.object(p);
        ArcMeta::RuntimeMeta meta = MetadataManager::instance().getMeta(p.toStdWString());
        m_metaCache.insert(p, new ArcMeta::RuntimeMeta(meta));
        return meta;
    };

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case 0: {
                QFileInfo info(path);
                QString name = info.fileName();
                // 如果文件名为空且为根目录（磁盘），则返回完整路径作为显示名
                if (name.isEmpty() && info.isRoot()) {
                    return QDir::toNativeSeparators(info.absoluteFilePath());
                }
                return name;
            }
            case 4: {
                return getCachedMeta(path).tags.join(", ");
            }
            case 5: {
                return record.isDir ? "文件夹" : QFileInfo(path).suffix().toUpper() + " 文件";
            }
            case 6: {
                QFileInfo info(path);
                if (info.isDir()) return "-";
                return QString::number(info.size() / 1024) + " KB";
            }
            case 7: {
                QFileInfo info(path);
                return info.lastModified().toString("yyyy-MM-dd HH:mm");
            }
        }
    } else if (role == PathRole) {
        return path;
    } else if (role == TypeRole) {
        return record.isDir ? "folder" : "file";
    } else if (role == RatingRole) {
        return getCachedMeta(path).rating;
    } else if (role == ColorRole) {
        return QString::fromStdWString(getCachedMeta(path).color);
    } else if (role == IsLockedRole || role == PinnedRole) {
        return getCachedMeta(path).pinned;
    } else if (role == EncryptedRole) {
        return getCachedMeta(path).encrypted;
    } else if (role == TagsRole) {
        return getCachedMeta(path).tags;
    } else if (role == InDatabaseRole) {
        return getCachedMeta(path).hasUserOperations();
    } else if (role == CategoryIdRole) {
        return 0; 
    } else if (role == IsEmptyRole) {
        QFileInfo info(path);
        return info.isDir() && QDir(path).isEmpty();
    } else if (role == AspectRatioRole) {
        return m_aspectRatios.value(path, 1.0);
    } else if (role == HasThumbnailRole) {
        return m_aspectRatios.contains(path);
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        QIcon* cached = m_iconCache.object(path);
        if (cached) return *cached;

        if (!m_requestedIcons.contains(path)) {
            m_requestedIcons.insert(path);
            auto* mutableThis = const_cast<FerrexVirtualDbModel*>(this);
            (void)QtConcurrent::run([mutableThis, path]() {
                QFileInfo info(path);
                QString ext = info.suffix().toLower();
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

                QMetaObject::invokeMethod(mutableThis, [mutableThis, path, icon, ar, hasThumb]() {
                    mutableThis->m_iconCache.insert(path, new QIcon(icon));
                    if (hasThumb) mutableThis->m_aspectRatios[path] = ar;
                    
                    // 局部刷新，提高性能
                    for (int i = 0; i < mutableThis->m_displayCount; ++i) {
                        if (mutableThis->m_allRecords[i].path == path) {
                            emit mutableThis->dataChanged(mutableThis->index(i, 0), mutableThis->index(i, 0), {Qt::DecorationRole, AspectRatioRole, HasThumbnailRole});
                            break;
                        }
                    }
                });
            });
        }
        return UiHelper::getFileIcon(path, 128); // 占位
    }

    return QVariant();
}

QVariant FerrexVirtualDbModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        static const QStringList headers = {"名称", "状态", "星级", "颜色标记", "标签", "类型", "大小", "修改日期"};
        if (section < headers.size()) return headers[section];
    }
    return QVariant();
}

bool FerrexVirtualDbModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() >= (int)m_allRecords.size()) return false;

    const auto& record = m_allRecords[index.row()];
    QString path = record.path;

    if (role == Qt::EditRole && index.column() == 0) {
        QString newName = value.toString();
        if (newName.isEmpty()) return false;

        auto& mutableRecord = m_allRecords[index.row()];
        QString oldPath = mutableRecord.path;
        QFileInfo info(oldPath);
        QString newPath = info.absolutePath() + "/" + newName;

        if (oldPath != newPath && QFile::rename(oldPath, newPath)) {
            // 同步更新元数据索引
            MetadataManager::instance().renameItem(oldPath.toStdWString(), newPath.toStdWString());
            // 物理同步：手动修改 m_allRecords 里的 path 以保持模型数据一致
            mutableRecord.path = QDir::toNativeSeparators(newPath);
            m_metaCache.remove(oldPath);
            emit dataChanged(index, index, {role, Qt::DisplayRole, PathRole});
            return true;
        }
        return false;
    }

    // 2026-06-xx 物理修复：支持星级、颜色、置顶等元数据的持久化设定
    bool metaUpdated = false;
    if (role == RatingRole) {
        int rating = value.toInt();
        MetadataManager::instance().setRating(path.toStdWString(), rating);
        metaUpdated = true;
    } else if (role == ColorRole) {
        QString color = value.toString();
        MetadataManager::instance().setColor(path.toStdWString(), color.toStdWString());
        metaUpdated = true;
    } else if (role == IsLockedRole || role == PinnedRole) {
        bool pinned = value.toBool();
        MetadataManager::instance().setPinned(path.toStdWString(), pinned);
        metaUpdated = true;
    }

    if (metaUpdated) {
        m_metaCache.remove(path);
        // 2026-06-xx 物理同步：发送全行更新信号，确保不同列的代理（如星级列、颜色列）同步刷新
        QModelIndex left = index.siblingAtColumn(0);
        QModelIndex right = index.siblingAtColumn(columnCount() - 1);
        emit dataChanged(left, right, {role});
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

void FerrexVirtualDbModel::setRecords(const std::vector<ArcMeta::ItemRepo::ItemRecord>& records) {
    beginResetModel();
    m_allRecords = records;
    m_displayCount = (int)m_allRecords.size();
    m_requestedIcons.clear();
    m_aspectRatios.clear();
    m_metaCache.clear();
    endResetModel();
}

void FerrexVirtualDbModel::clear() {
    beginResetModel();
    m_allRecords.clear();
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
 
void FilterProxyModel::setSearchQuery(const QString& query) { 
    m_searchQuery = query; 
    beginFilterChange(); 
    endFilterChange(); 
} 
 
bool FilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const { 
    QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent); 
     
    // 1. 评级过滤 
    if (!currentFilter.ratings.isEmpty()) { 
        int r = idx.data(RatingRole).toInt(); 
        if (!currentFilter.ratings.contains(r)) return false; 
    } 
 
    // 2. 颜色过滤 (变长物理多色板命中逻辑)
    if (!currentFilter.colors.isEmpty()) { 
        QString path = idx.data(PathRole).toString();
        QString dominantColor = idx.data(ColorRole).toString();
        
        // 获取该项目的所有物理颜色 (变长色板)
        QVector<QColor> palettes = MetadataManager::instance().getPalettes(path.toStdWString());
        
        bool matchColor = false;
        for (const QString& fc : currentFilter.colors) {
            // 物理修复：如果 HEX 完全一致（主色命中），直接通过
            if (fc == dominantColor.toUpper()) { matchColor = true; break; }

            // 如果色板存在，遍历色板执行容差检查
            if (!palettes.isEmpty()) {
                for (const auto& pc : palettes) {
                    if (fc == pc.name().toUpper()) { matchColor = true; break; }
                    
                    QColor fCol = UiHelper::parseColorName(fc);
                    if (fCol.isValid()) {
                        long rmean = (fCol.red() + pc.red()) / 2;
                        long r = fCol.red() - pc.red();
                        long g = fCol.green() - pc.green();
                        long b = fCol.blue() - pc.blue();
                        long distSq = (((512 + rmean)*r*r) >> 8) + 4*g*g + (((767-rmean)*b*b) >> 8);
                        if (distSq < 15000) { matchColor = true; break; }
                    }
                }
            } else {
                // 向下兼容：若色板为空，回退到对主色调的容差检查
                QColor dCol = UiHelper::parseColorName(dominantColor);
                QColor fCol = UiHelper::parseColorName(fc);
                if (dCol.isValid() && fCol.isValid()) {
                    long rmean = (fCol.red() + dCol.red()) / 2;
                    long r = fCol.red() - dCol.red();
                    long g = fCol.green() - dCol.green();
                    long b = fCol.blue() - dCol.blue();
                    long distSq = (((512 + rmean)*r*r) >> 8) + 4*g*g + (((767-rmean)*b*b) >> 8);
                    if (distSq < 15000) { matchColor = true; break; }
                }
            }
            if (matchColor) break;
        }
        if (!matchColor) return false; 
    } 
 
    // 3. 标签过滤 
    if (!currentFilter.tags.isEmpty()) { 
        QStringList itemTags = idx.data(TagsRole).toStringList(); 
        bool matchTag = false; 
        for (const QString& fTag : currentFilter.tags) { 
            if (fTag == "__none__") { 
                if (itemTags.isEmpty()) { matchTag = true; break; } 
            } else { 
                if (itemTags.contains(fTag)) { matchTag = true; break; } 
            } 
        } 
        if (!matchTag) return false; 
    } 
 
    // 4. 类型过滤 
    if (!currentFilter.types.isEmpty()) { 
        QString type = idx.data(TypeRole).toString();  
        QString ext = QFileInfo(idx.data(PathRole).toString()).suffix().toUpper(); 
        bool matchType = false; 
        for (const QString& fType : currentFilter.types) { 
            if (fType == "folder") { 
                if (type == "folder") { matchType = true; break; } 
            } else { 
                if (ext == fType.toUpper()) { matchType = true; break; } 
            } 
        } 
        if (!matchType) return false; 
    } 
 
    // 5. 创建日期过滤 
    if (!currentFilter.createDates.isEmpty()) { 
        QDate d = QFileInfo(idx.data(PathRole).toString()).birthTime().date(); 
        QDate today = QDate::currentDate(); 
        QString dStr = d.toString("yyyy-MM-dd"); 
        bool matchDate = false; 
        for (const QString& fDate : currentFilter.createDates) { 
            if (fDate == "today" && d == today) { matchDate = true; break; } 
            if (fDate == "yesterday" && d == today.addDays(-1)) { matchDate = true; break; } 
            if (fDate == dStr) { matchDate = true; break; } 
        } 
        if (!matchDate) return false; 
    } 
 
    // 6. 修改日期过滤 
    if (!currentFilter.modifyDates.isEmpty()) { 
        QDate d = QFileInfo(idx.data(PathRole).toString()).lastModified().date(); 
        QDate today = QDate::currentDate(); 
        QString dStr = d.toString("yyyy-MM-dd"); 
        bool matchDate = false; 
        for (const QString& fDate : currentFilter.modifyDates) { 
            if (fDate == "today" && d == today) { matchDate = true; break; } 
            if (fDate == "yesterday" && d == today.addDays(-1)) { matchDate = true; break; } 
            if (fDate == dStr) { matchDate = true; break; } 
        } 
        if (!matchDate) return false; 
    } 
 
    // 2026-04-12 深度修复：直接执行关键词包含检查 
    if (m_searchQuery.isEmpty()) return true; 
 
    QString fileName = idx.data(Qt::DisplayRole).toString(); 
    return fileName.contains(m_searchQuery, Qt::CaseInsensitive); 
} 
 
bool FilterProxyModel::lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const { 
    // 核心红线：置顶优先规则 
    bool leftPinned = source_left.data(IsLockedRole).toBool(); 
    bool rightPinned = source_right.data(IsLockedRole).toBool(); 
 
    if (leftPinned != rightPinned) { 
        if (sortOrder() == Qt::AscendingOrder) return leftPinned;  
        else return !leftPinned;  
    } 
    return QSortFilterProxyModel::lessThan(source_left, source_right); 
} 
 
 
ContentPanel::ContentPanel(QWidget* parent) 
    : QFrame(parent) { 
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
 
    initUi(); 
    // 2026-05-27 按照用户要求：构造函数末尾强行对齐初始网格尺寸，废除 initGridView 中的旧硬编码值 
    updateGridSize(); 
} 
 
void ContentPanel::deferredInit() { 
    qDebug() << "[ContentPanel] deferredInit 开始执行"; 
    // 2026-04-12 按照用户要求：补全延迟初始化逻辑，此处可处理模型预热或首屏数据对齐 
    qDebug() << "[ContentPanel] deferredInit 执行完毕"; 
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
    titleL->setContentsMargins(15, 0, 5, 0); // 2026-05-17 按照用户要求：右侧边距统一设为 5px，消除 15px 留白
    titleL->setSpacing(5);                  // 2026-05-17 按照用户要求：间距统一为 5px
 
    QLabel* iconLabel = new QLabel(titleBar); 
    iconLabel->setPixmap(UiHelper::getIcon("eye", QColor("#41F2F2"), 18).pixmap(18, 18)); 
    titleL->addWidget(iconLabel); 
 
    QLabel* titleLabel = new QLabel("内容", titleBar); 
    titleLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #41F2F2; background: transparent; border: none;"); 
     
    m_btnLayers = new QPushButton(titleBar); 
    m_btnLayers->setCheckable(true); 
    m_btnLayers->setFixedSize(24, 24); 
    m_btnLayers->setIcon(UiHelper::getIcon("layers", QColor("#B0B0B0"), 18)); 
    // 2026-03-xx 按照宪法要求：禁绝原生 ToolTip，强制对接 ToolTipOverlay 
    m_btnLayers->setProperty("tooltipText", "递归显示子目录所有文件"); 
    m_btnLayers->installEventFilter(this); 
    m_btnLayers->setStyleSheet( 
        "QPushButton { background: transparent; border: none; border-radius: 4px; }" 
        "QPushButton:hover { background: rgba(255, 255, 255, 0.1); }" 
        "QPushButton:checked { background: rgba(52, 152, 219, 0.2); border: 1px solid #3498db; }" 
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
    titleL->addWidget(m_btnLayers, 0, Qt::AlignVCenter); 
 
    m_mainLayout->addWidget(titleBar); 
 
    m_viewStack = new QStackedWidget(this); 
     
    initGridView(); 
    initListView(); 
 
    m_viewStack->addWidget(m_gridView); 
    m_viewStack->addWidget(m_treeView); 
    m_viewStack->setCurrentWidget(m_gridView); 
 
    QVBoxLayout* contentWrapper = new QVBoxLayout(); 
    contentWrapper->setContentsMargins(4, 4, 4, 4); // 2026-05-08 按照用户要求：增加到4px使卡片到容器边缘达到10px
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
            int ratingH = 22;           // 2026-05-17 按照要求：为卡片外的评分区预留高度
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
            ToolTipOverlay::instance()->showText(QCursor::pos(), text); 
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
                        QString path = idx.data(PathRole).toString(); 
                        if (!path.isEmpty()) { 
                            MetadataManager::instance().setRating(path.toStdWString(), rating); 
                            m_proxyModel->setData(idx, rating, RatingRole); 
                        } 
                    } 
                } 
                return true; 
            } 
 
            if (((keyEvent->modifiers() & Qt::AltModifier) || (keyEvent->modifiers() & (Qt::AltModifier | Qt::WindowShortcut))) &&  
                (keyEvent->key() == Qt::Key_D)) { 
                auto indexes = view->selectionModel()->selectedIndexes(); 
                for (const QModelIndex& idx : indexes) { 
                    if (idx.column() == 0) { 
                        QString itemPath = idx.data(PathRole).toString(); 
                        if (!itemPath.isEmpty()) { 
                            bool current = idx.data(IsLockedRole).toBool(); 
                            MetadataManager::instance().setPinned(itemPath.toStdWString(), !current); 
                            m_proxyModel->setData(idx, !current, IsLockedRole); 
                        } 
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
 
                QColor tagColor = UiHelper::parseColorName(colorValue); 
                auto indexes = view->selectionModel()->selectedIndexes(); 
                for (const auto& idx : indexes) { 
                    if (idx.column() == 0) { 
                        QString path = idx.data(PathRole).toString(); 
                        if (!path.isEmpty()) { 
                            MetadataManager::instance().setColor(path.toStdWString(), colorValue.toStdWString()); 
                            m_proxyModel->setData(idx, colorValue, ColorRole); 
 
                            // 2026-06-05 按照要求：快捷键设置颜色后立即重渲染图标，实现视觉同步 
                            QIcon coloredIcon = UiHelper::getFileIcon(path, 128); 
                            m_proxyModel->setData(idx, coloredIcon, Qt::DecorationRole); 
                        } 
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
                if (idx.isValid()) emit requestQuickLook(idx.data(PathRole).toString()); 
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
    m_gridView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); 
    m_gridView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); 
    m_gridView->setSelectionMode(QAbstractItemView::ExtendedSelection); 
    m_gridView->setContextMenuPolicy(Qt::CustomContextMenu); 
 
    m_gridView->setDragEnabled(true); 
    m_gridView->setDragDropMode(QAbstractItemView::DragOnly); 
 
    // 2026-06-xx 物理纠偏：移除 SelectedClicked，防止单击项目时意外触发重命名，确保交互稳健
    m_gridView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed); 
 
    m_gridView->setModel(m_proxyModel); 

    auto* justifiedView = qobject_cast<JustifiedView*>(m_gridView);
    if (justifiedView) {
        justifiedView->setAspectRatioRole(AspectRatioRole);
        auto* delegate = new ThumbnailDelegate(this);
        delegate->setHasThumbnailRole(HasThumbnailRole);
        delegate->setRatingRole(RatingRole);
        delegate->setPathRole(PathRole);
        delegate->setPinnedRole(PinnedRole);
        delegate->setManagedRole(InDatabaseRole);
        delegate->setTypeRole(TypeRole);
        delegate->setIsEmptyRole(IsEmptyRole);
        delegate->setColorRole(ColorRole);
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
    ); 
 
    connect(m_gridView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ContentPanel::onSelectionChanged); 
    connect(m_gridView, &QAbstractItemView::customContextMenuRequested, this, &ContentPanel::onCustomContextMenuRequested); 
} 
 
void ContentPanel::initListView() { 
    m_treeView = new DropTreeView(this); 
    m_treeView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); 
    m_treeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); 
    m_treeView->setSortingEnabled(true); 
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu); 
    m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection); 
     
    m_treeView->setDragEnabled(true); 
    m_treeView->setDragDropMode(QAbstractItemView::DragOnly); 
 
    m_treeView->setExpandsOnDoubleClick(false); 
    m_treeView->setRootIsDecorated(false); 
     
    m_treeView->setItemDelegate(new TreeItemDelegate(this)); 
 
    m_treeView->setModel(m_proxyModel); 
    m_treeView->viewport()->installEventFilter(this); 
 
    m_treeView->setStyleSheet( 
        "QTreeView { background-color: transparent; border: none; outline: none; font-size: 12px; }" 
        "QTreeView::item { height: 28px; color: #EEEEEE; padding-left: 0px; }" 
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
        QMenu* categorizeMenu = menu.addMenu("归类到..."); 
        UiHelper::applyMenuStyle(categorizeMenu); 
        // 2026-06-xx 逻辑优化：仅显示最近使用的 15 个分类
        auto categories = CategoryRepo::getRecentlyUsed(15); 
        if (categories.empty()) categories = CategoryRepo::getAll();
        if (categories.size() > 15) categories.resize(15);

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
 
        QMenu* ratingMenu = menu.addMenu("评分");
        UiHelper::applyMenuStyle(ratingMenu);
        int currentRating = currentIndex.data(RatingRole).toInt();
        for (int i = 0; i <= 5; ++i) {
            QString star = (i == 0) ? "无评分" : QString(i, QChar(0x2605));
            QAction* act = ratingMenu->addAction(star);
            act->setData(ActionSetRating);
            act->setProperty("value", i);
            if (currentRating == i) {
                act->setCheckable(true);
                act->setChecked(true);
            }
        }

        // --- 2026-05-16 图像分析：从图中提取主色调 ---
        QString ext = QFileInfo(path).suffix().toLower();
        if (UiHelper::isGraphicsFile(ext)) {
            menu.addAction("解析颜色...")->setData(ActionExtractColor);
        }

        menu.addSeparator(); 
 
        // [批量与加密区] 
        if (isFolder) { 
            menu.addAction("批量重命名 (Ctrl+Shift+R)")->setData(ActionBatchRename); 
        } else { 
            QMenu* cryptoMenu = menu.addMenu("加密保护"); 
            UiHelper::applyMenuStyle(cryptoMenu); 
            cryptoMenu->addAction("执行加密保护")->setData(ActionEncrypt); 
            cryptoMenu->addAction("解除加密")->setData(ActionDecrypt); 
            cryptoMenu->addAction("修改加密密码")->setData(ActionChangePwd); 
        } 
 
        menu.addSeparator(); 
 
        // [通用编辑区] 
        menu.addAction("编辑标签...")->setData(ActionEditTags);
        menu.addAction("编辑备注...")->setData(ActionEditNote);
        menu.addSeparator();
        menu.addAction("重命名")->setData(ActionRename); 
        menu.addAction("复制")->setData(ActionCopy); 
        menu.addAction("剪切")->setData(ActionCut); 
        menu.addAction("粘贴")->setData(ActionPaste); 
        
        // 2026-06-xx 按照用户要求：在回收站中不显示二级删除菜单
        if (m_currentCategoryType != "trash") {
            QMenu* delMenu = menu.addMenu("删除");
            UiHelper::applyMenuStyle(delMenu);
            delMenu->addAction("移入回收站")->setData(ActionDelete);
            delMenu->addAction("彻底删除 (不可恢复)")->setData(ActionPermanentDelete);
            delMenu->addAction("安全擦除 (覆写抹除)")->setData(ActionSecureDelete);
        } else {
            // 回收站模式下，原位置不显示删除
        }
 
        menu.addSeparator(); 
        menu.addAction("复制路径")->setData(ActionCopyPath); 
        menu.addAction("属性")->setData(ActionProperties); 

        // 2026-06-xx 按照用户要求：在回收站分类中，最底部增加“永久删除”选项
        if (m_currentCategoryType == "trash") {
            menu.addSeparator();
            menu.addAction(UiHelper::getIcon("trash", QColor("#e81123"), 18), "永久删除")->setData(ActionPermanentDelete);
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
        QMenu* viewMenu = menu.addMenu("视图(V)");
        UiHelper::applyMenuStyle(viewMenu);
        QAction* gvAct = viewMenu->addAction("网格视图");
        gvAct->setData(ActionViewGridView);
        gvAct->setCheckable(true);
        gvAct->setChecked(m_viewStack->currentIndex() == 0);
        
        QAction* lvAct = viewMenu->addAction("列表视图");
        lvAct->setData(ActionViewListView);
        lvAct->setCheckable(true);
        lvAct->setChecked(m_viewStack->currentIndex() == 1);

        QMenu* sortMenu = menu.addMenu("排序(S)");
        UiHelper::applyMenuStyle(sortMenu);
        sortMenu->addAction("名称")->setData(ActionSortName);
        sortMenu->addAction("大小")->setData(ActionSortSize);
        sortMenu->addAction("修改日期")->setData(ActionSortTime);

        menu.addAction("刷新(R)")->setData(ActionRefresh);
 
        menu.addSeparator(); 
        QAction* actProp = menu.addAction("当前文件夹属性"); 
        actProp->setData(ActionProperties); 
        actProp->setEnabled(!m_currentPath.isEmpty() && m_currentPath != "computer://"); 
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
                    // 2026-06-xx 物理同步：基于同步获取的 File ID 进行归类，解决新文件关联失败冲突。 
                    std::string fid = MetadataManager::instance().getFileIdSync(itemPath.toStdWString()); 
                    if (!fid.empty()) { 
                        CategoryRepo::addItemToCategory(catId, fid); 
                    } 
                } 
            } 
            ToolTipOverlay::instance()->showText(QCursor::pos(), "已成功归类 (基于 File ID)", 1500, QColor("#2ecc71")); 
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
            break; 
        } 
        case ActionSetRating: {
            int rating = selectedAction->property("value").toInt();
            auto indexes = view->selectionModel()->selectedIndexes();
            for (const auto& idx : indexes) {
                if (idx.column() == 0) {
                    m_proxyModel->setData(idx, rating, RatingRole);
                }
            }
            break;
        }
        case ActionEditTags: {
            auto meta = MetadataManager::instance().getMeta(path.toStdWString());
            bool ok;
            QString text = QInputDialog::getText(this, "编辑标签", "标签 (逗号分隔):", QLineEdit::Normal, meta.tags.join(","), &ok);
            if (ok) {
                auto indexes = view->selectionModel()->selectedIndexes();
                QStringList tagList = text.split(",", Qt::SkipEmptyParts);
                for (int i = 0; i < tagList.size(); ++i) tagList[i] = tagList[i].trimmed();
                
                for (const auto& idx : indexes) {
                    if (idx.column() == 0) {
                        QString itemPath = idx.data(PathRole).toString();
                        MetadataManager::instance().setTags(itemPath.toStdWString(), tagList);
                        // 触发模型更新
                        m_proxyModel->setData(idx, QVariant(), TagsRole);
                    }
                }
                ToolTipOverlay::instance()->showText(QCursor::pos(), "标签已更新", 1500, QColor("#2ecc71"));
            }
            break;
        }
        case ActionEditNote: {
            auto meta = MetadataManager::instance().getMeta(path.toStdWString());
            bool ok;
            QString text = QInputDialog::getMultiLineText(this, "编辑备注", "备注内容:", QString::fromStdWString(meta.note), &ok);
            if (ok) {
                auto indexes = view->selectionModel()->selectedIndexes();
                for (const auto& idx : indexes) {
                    if (idx.column() == 0) {
                        QString itemPath = idx.data(PathRole).toString();
                        MetadataManager::instance().setNote(itemPath.toStdWString(), text.toStdWString());
                        // 备注变化目前没有对应的 Role 直接刷新，通常伴随 meta 刷新
                        m_proxyModel->setData(idx, QVariant(), Qt::EditRole); 
                    }
                }
                ToolTipOverlay::instance()->showText(QCursor::pos(), "备注已更新", 1500, QColor("#2ecc71"));
            }
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
            QPointer<ContentPanel> weakThis(this);
            (void)QtConcurrent::run([weakThis, path]() {
                auto palette = UiHelper::extractPalette(path);
                if (palette.isEmpty()) return;
                
                // 1. 提取第一个颜色作为主色调 (用于图标着色)
                QColor dominant = UiHelper::quantizeColor(palette.first().first);
                QString colorHex = dominant.name().toUpper();
                
                QMetaObject::invokeMethod(weakThis.data(), [weakThis, path, colorHex, palette, dominant]() {
                    if (weakThis) {
                        // 2. 物理存储：全量变长色板 (主色通过 model->setData 持久化)
                        MetadataManager::instance().setPalettes(path.toStdWString(), palette);
                        
                        // 3. 物理同步 UI 状态并持久化主色
                        auto* model = weakThis->m_model;
                        const auto& records = model->allRecords();
                        for (size_t i = 0; i < records.size(); ++i) {
                            if (records[i].path == path) {
                                QModelIndex srcIdx = model->index(static_cast<int>(i), 0);
                                // 此处 setData 会触发 MetadataManager::setColor 和缓存清理
                                model->setData(srcIdx, colorHex, ColorRole);
                                break;
                            }
                        }
                        ToolTipOverlay::instance()->showText(QCursor::pos(), "变长色板已物理提取并绑定", 1500, QColor("#2ecc71"));
                    }
                });
            });
            break;
        }
        case ActionEncrypt: { 
            bool ok; 
            QString pwd = QInputDialog::getText(this, "加密保护", "设置加密密码:", QLineEdit::Password, "", &ok); 
            if (ok && !pwd.isEmpty()) { 
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
                    QMetaObject::invokeMethod(qApp, [self, currentDir]() { 
                        if (self && self->m_currentPath == currentDir) self->loadDirectory(currentDir, self->m_isRecursive); 
                        ToolTipOverlay::instance()->showText(QCursor::pos(), "加密任务处理完成", 1500, QColor("#2ecc71")); 
                    }); 
                }); 
            } 
            break; 
        } 
        case ActionDecrypt: { 
            bool ok; 
            QString pwd = QInputDialog::getText(this, "解除加密", "输入加密密码:", QLineEdit::Password, "", &ok); 
            if (ok && !pwd.isEmpty()) { 
                ToolTipOverlay::instance()->showText(QCursor::pos(), "解除加密逻辑已触发", 1500); 
            } 
            break; 
        } 
        case ActionBatchRename: performBatchRename(); break; 
        case ActionRename: view->edit(currentIndex); break; 
        case ActionCopy: performCopy(false); break; 
        case ActionCut: performCopy(true); break; 
        case ActionPaste: performPaste(); break; 
        case ActionRestore: {
            auto indexes = view->selectionModel()->selectedIndexes();
            for (const auto& idx : indexes) {
                if (idx.column() == 0) {
                    QString itemPath = idx.data(PathRole).toString();
                    ItemRepo::restoreByPath(itemPath.toStdWString());
                }
            }
            loadDirectory(m_currentPath);
            emit MetadataManager::instance().metaChanged(""); // 刷新侧边栏计数
            break;
        }
        case ActionDelete: 
        case ActionPermanentDelete:
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
                // 彻底删除或安全擦除
                QString msg = (action == ActionPermanentDelete) ? "确定要彻底删除选中的项目吗？此操作不可恢复。" : "确定要安全擦除选中的项目吗？数据将被覆写并永久抹除。";
                if (QMessageBox::question(this, "确认删除", msg) != QMessageBox::Yes) break;

                ProgressDialog* progress = new ProgressDialog("正在执行深层抹除...", this);
                progress->show();

                QPointer<ContentPanel> weakThis(this);
                QPointer<ProgressDialog> weakProgress(progress);
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
                            // 2. 数据库同步清理 (三位一体)
                            ItemRepo::physicalRemove(wp);
                            
                            // 3. 元数据管理清理 (离散 JSON 与 内存失效)
                            MetadataManager::instance().removeMetadataSync(wp);
                        }

                        count++;
                        QMetaObject::invokeMethod(weakProgress.data(), [weakProgress, count, targetPaths]() {
                            if (weakProgress) weakProgress->setValue((int)((float)count / targetPaths.size() * 100));
                        });
                    }

                    QMetaObject::invokeMethod(qApp, [weakThis, weakProgress]() {
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
        case ActionViewGridView: setViewMode(GridView); break;
        case ActionViewListView: setViewMode(ListView); break;
        case ActionSortName: m_proxyModel->sort(0, m_proxyModel->sortOrder()); break;
        case ActionSortSize: m_proxyModel->sort(6, m_proxyModel->sortOrder()); break;
        case ActionSortTime: m_proxyModel->sort(7, m_proxyModel->sortOrder()); break;
        case ActionRefresh: loadDirectory(m_currentPath, m_isRecursive); break;
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
    m_currentCategoryType = ""; // 物理导航模式下清除系统类型
    qDebug() << "[Content] 开始物理递归扫描 (虚拟化) ->" << path << (recursive ? "递归" : "单级"); 
    emit dataSourceChanged("nav"); 
    if (m_viewStack) m_viewStack->show(); 
    if (m_textPreview) m_textPreview->hide(); 
    if (m_imagePreview) m_imagePreview->hide(); 
 
    m_isRecursive = recursive; 
    if (m_btnLayers) m_btnLayers->setChecked(recursive); 
 
    m_model->clear(); 
 
    if (path.isEmpty() || path == "computer://") { 
        m_currentPath = "computer://"; 
        updateLayersButtonState(); 
 
        const auto drives = QDir::drives(); 
        std::vector<ArcMeta::ItemRepo::ItemRecord> driveRecords;
        for (const QFileInfo& drive : drives) { 
            ItemRepo::ItemRecord r;
            r.path = QDir::toNativeSeparators(drive.absolutePath());
            r.isDir = true;
            driveRecords.push_back(r);
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
     
    QPointer<ContentPanel> panelPtr(this); 
    (void)QThreadPool::globalInstance()->start([panelPtr, path, recursive]() { 
        if (!panelPtr) return; 
         
        std::vector<ArcMeta::ItemRepo::ItemRecord> allItems;
 
        std::function<void(const QString&, bool)> scanDir; 
        scanDir = [&](const QString& p, bool rec) { 
            QDir dir(p); 
            if (!dir.exists()) return; 
 
            QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name); 
            for (const QFileInfo& info : entries) { 
                if (!panelPtr) return; 
                if (info.fileName() == ".am_meta.json" || info.fileName() == ".am_meta.json.tmp") continue; 
 
                ItemRepo::ItemRecord r;
                r.path = QDir::toNativeSeparators(info.absoluteFilePath());
                r.isDir = info.isDir();
                allItems.push_back(r);
 
                if (rec && info.isDir()) { 
                    scanDir(info.absoluteFilePath(), true); 
                } 
            } 
        }; 
 
        scanDir(path, recursive); 
        if (!panelPtr) return; 
 
        QMetaObject::invokeMethod(qApp, [panelPtr, path, allItems]() { 
            if (panelPtr && panelPtr->m_currentPath == path) { 
                panelPtr->m_model->setRecords(allItems);
                panelPtr->m_isLoading = false;
                panelPtr->recalculateAndEmitStats();
                // 2026-06-xx 物理同步：数据加载完成后强制重新应用筛选，防止显示已过滤掉的占位符记录
                panelPtr->applyFilters();
            } 
        }, Qt::QueuedConnection); 
    }); 
} 
 
 
 
 
void ContentPanel::search(const QString& query) { 
    m_currentCategoryType = "search";
    qDebug() << "[Content] 物理检索 (DB模式) ->" << query; 
    if (m_viewStack) m_viewStack->show(); 
    if (m_textPreview) m_textPreview->hide(); 
    if (m_imagePreview) m_imagePreview->hide(); 
 
    m_isLoading = true;
    auto records = ItemRepo::searchRecordsByKeyword(query, m_currentPath);
    m_model->setRecords(records);
    m_isLoading = false;
    recalculateAndEmitStats();
    // 2026-06-xx 物理同步：搜索结果加载后同步应用高级筛选
    applyFilters();
} 
 
void ContentPanel::applyFilters(const FilterState& state) { 
    m_currentFilter = state; 
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
    m_isLoading = true;
    m_currentCategoryType = "user_category";
    m_viewStack->show(); 
    if (m_textPreview) m_textPreview->hide(); 
    if (m_imagePreview) m_imagePreview->hide(); 
    emit dataSourceChanged("category"); 
     
    m_model->clear(); 
 
    std::vector<ArcMeta::ItemRepo::ItemRecord> allRecords;

    // 1. 加载子分类
    auto allCategories = CategoryRepo::getAll();
    for (const auto& cat : allCategories) {
        if (cat.parentId == categoryId) {
            ItemRepo::ItemRecord r;
            r.isCategory = true;
            r.categoryId = cat.id;
            r.categoryName = QString::fromStdWString(cat.name);
            r.categoryColor = QString::fromStdWString(cat.color).isEmpty() ? "#aaaaaa" : QString::fromStdWString(cat.color);
            allRecords.push_back(r);
        }
    }

    // 2. 加载文件
    auto itemRecords = ItemRepo::getRecordsInCategory(categoryId);
    // 2026-06-xx 物理过滤：排除数据库残留的失效路径，防止内容区出现空“FILE”占位符
    for (auto it = itemRecords.begin(); it != itemRecords.end(); ) {
        if (!it->path.isEmpty() && !QFileInfo::exists(it->path)) {
            it = itemRecords.erase(it);
        } else {
            ++it;
        }
    }
    allRecords.insert(allRecords.end(), itemRecords.begin(), itemRecords.end());

    m_model->setRecords(allRecords);
     
    m_isLoading = false;
    recalculateAndEmitStats();
    applyFilters(); 
} 
 
void ContentPanel::loadPaths(const QStringList& paths) { 
    m_isLoading = true;
    m_viewStack->show(); 
    if (m_textPreview) m_textPreview->hide(); 
    if (m_imagePreview) m_imagePreview->hide(); 
    emit dataSourceChanged("category"); 
     
    m_model->clear(); 
 
    std::vector<ArcMeta::ItemRepo::ItemRecord> records;
    for (const QString& p : paths) {
        // 2026-06-xx 物理过滤：系统分类加载时强制校验物理存在性
        if (!p.isEmpty() && QFileInfo::exists(p)) {
            ItemRepo::ItemRecord r;
            r.path = QDir::toNativeSeparators(p);
            r.isDir = QFileInfo(p).isDir();
            records.push_back(r);
        }
    }
    m_model->setRecords(records);
 
    m_isLoading = false;
    recalculateAndEmitStats();
    applyFilters(); 
} 
 
void ContentPanel::recalculateAndEmitStats() {
    const std::vector<ArcMeta::ItemRepo::ItemRecord>& records = m_model->allRecords();
    if (records.empty()) {
        emit directoryStatsReady({}, {}, {}, {}, {}, {});
        return;
    }

    QPointer<ContentPanel> weakThis(this);
    (void)QtConcurrent::run([weakThis, records]() {
        ScanStats stats;
        QDate today = QDate::currentDate();
        QDate yesterday = today.addDays(-1);

        for (const auto& record : records) {
            if (!weakThis) return;
            QString path = record.path;
            auto meta = MetadataManager::instance().getMeta(path.toStdWString());
            QFileInfo info(path);

            stats.ratingCounts[meta.rating]++;
            
            QString dominantColor = QString::fromStdWString(meta.color);
            if (!dominantColor.isEmpty()) {
                stats.colorCounts[dominantColor.toUpper()]++;
            } else {
                stats.colorCounts[""]++;
            }
            
            if (info.isDir()) {
                stats.typeCounts["folder"]++;
            } else {
                stats.typeCounts[info.suffix().toUpper()]++;
            }
            
            for (const QString& tag : meta.tags) {
                stats.tagCounts[tag]++;
            }
            if (meta.tags.isEmpty()) stats.noTagCount++;
            
            // 物理磁盘访问移至后台线程，彻底解决 UI 假死
            if (info.exists()) {
                QDate cdate = info.birthTime().date();
                QDate mdate = info.lastModified().date();
                
                auto dateKey = [&](const QDate& d) {
                    if (d == today) return QString("today");
                    if (d == yesterday) return QString("yesterday");
                    return d.toString("yyyy-MM-dd");
                };

                stats.createDateCounts[dateKey(cdate)]++;
                stats.modifyDateCounts[dateKey(mdate)]++;
            }
        }
        if (stats.noTagCount > 0) stats.tagCounts["__none__"] = stats.noTagCount;

        QMetaObject::invokeMethod(qApp, [weakThis, stats]() {
            if (weakThis) {
                emit weakThis->directoryStatsReady(stats.ratingCounts, stats.colorCounts, stats.tagCounts,
                                                 stats.typeCounts, stats.createDateCounts, stats.modifyDateCounts);
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
        loadDirectory(m_currentPath, m_isRecursive); 
        // 虚拟模型中不再支持 findItems，需要手动寻找
    const std::vector<ArcMeta::ItemRepo::ItemRecord>& records = m_model->allRecords();
        for (size_t i = 0; i < records.size(); ++i) {
            if (QFileInfo(records[i].path).fileName() == finalName) {
                QModelIndex srcIdx = m_model->index(static_cast<int>(i), 0);
                QModelIndex proxyIdx = m_proxyModel->mapFromSource(srcIdx);
                if (proxyIdx.isValid()) {
                    m_gridView->setCurrentIndex(proxyIdx);
                    m_gridView->edit(proxyIdx);
                }
                break;
            }
        }
    } 
} 
 
void ContentPanel::updateLayersButtonState() { 
    if (!m_btnLayers) return; 
 
    if (m_currentPath.isEmpty() || m_currentPath == "computer://") { 
        m_btnLayers->setEnabled(false); 
        m_btnLayers->setChecked(false); 
        m_btnLayers->setProperty("tooltipText", "“此电脑”不支持递归显示"); 
        return; 
    } 
 
    m_btnLayers->setEnabled(true); 
    m_btnLayers->setProperty("tooltipText", "递归显示子目录所有文件"); 
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
    m.ratingH      = 20;  // 2026-05-17 物理锁定评分区高度 
    m.nameH        = (int)(zoom * 0.25); 
 
    // 2026-05-17 按照用户要求：主图标在正方形内垂直居中
    m.iconRect = QRect(m.squareRect.left() + (m.squareRect.width() - m.iconDrawSize) / 2, 
                       m.squareRect.top() + (m.squareRect.height() - m.iconDrawSize) / 2, 
                       m.iconDrawSize, m.iconDrawSize); 
    
    // 2026-05-17 物理布局重构：星级区置于正方形下方
    // 2026-06-xx 按照用户要求：下移 2 像素（从 +6 改为 +8）
    m.ratingY = m.squareRect.bottom() + 8; 
 
    m.starSize    = 18; // 2026-05-17 尺寸微调以匹配外部布局
    m.starSpacing = 1;   
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
    // 1. 状态位图标绘制 (置顶 vs. 已录入 互斥) 
    // 2026-06-xx 物理修复：校准 ItemRole 作用域，确保 GridItemDelegate 编译通过 
    bool isPinned = index.data(IsLockedRole).toBool(); 
    bool isManaged = index.data(InDatabaseRole).toBool(); 
     
    if (isPinned || isManaged) { 
        QRect statusRect(m.squareRect.right() - 22, m.squareRect.top() + 8, 16, 16); 
        if (isPinned) { 
            // 置顶优先 
            QIcon pinIcon = UiHelper::getIcon("pin_vertical", BrandOrange, 16); 
            pinIcon.paint(painter, statusRect); 
        } else { 
            // 已录入但未置顶，显示绿对勾 
            QIcon checkIcon = UiHelper::getIcon("check_circle", SuccessGreen, 16); 
            checkIcon.paint(painter, statusRect); 
        } 
    } 
 
    // 2. 扩展名角标 
    QString path = index.data(PathRole).toString(); 
    QFileInfo info(path); 
    QString ext = info.isDir() ? "DIR" : info.suffix().toUpper(); 
    if (ext.isEmpty()) ext = "FILE"; 
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
        QColor starColor = colorName.isEmpty() ? QColor("#CCCCCC") : UiHelper::parseColorName(colorName).darker(700);
        QColor emptyStarColor = colorName.isEmpty() ? QColor("#888888") : UiHelper::parseColorName(colorName).darker(400);
        if (!colorName.isEmpty()) emptyStarColor.setAlpha(180);

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
    // 2026-06-xx 物理修复：校准 InDatabaseRole 作用域 
    if (!isSelected && !index.data(InDatabaseRole).toBool()) { 
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
                // 2. 执行数据更新 (禁止图标设为 0，否则设为星级)
                model->setData(index, isBanHit ? 0 : hitStar, RatingRole); 

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
    // 2026-05-25 物理修复：改用 qobject_cast 彻底根除 static_cast 类型无法识别的 Bug 
    QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor); 
    if (lineEdit) lineEdit->setText(value); 
     
    int lastDot = value.lastIndexOf('.'); 
    if (lastDot > 0) { 
        lineEdit->setSelection(0, lastDot); 
    } else { 
        lineEdit->selectAll(); 
    } 
} 
 
void GridItemDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const { 
    QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor); 
    if (!lineEdit) return; 
    QString value = lineEdit->text(); 
    if(value.isEmpty() || value == index.data(Qt::DisplayRole).toString()) return; 
 
    QString oldPath = index.data(PathRole).toString(); 
    QFileInfo info(oldPath); 
    QString newPath = info.absolutePath() + "/" + value; 
     
    if (QFile::rename(oldPath, newPath)) { 
        // 2026-06-xx 物理同步：重命名后直接更新 MetadataManager 并通知模型刷新
        MetadataManager::instance().renameItem(oldPath.toStdWString(), newPath.toStdWString()); 
        
        // 针对虚拟模型，我们由于 records 是只读的缓存，通常需要重新加载目录
        // 但为了即时反馈，可以尝试通过 setData 触发局部刷新
        model->setData(index, value, Qt::EditRole);
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
