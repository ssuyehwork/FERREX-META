#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ScanDialog.h"
#include "../core/CacheManager.h"
#include <QPainter>
#include <QTimer>
#include <QIcon>
#include "../mft/MftReader.h"
#include "UiHelper.h"
#include "../meta/MetadataManager.h"
#include <QFileInfo>
#include <QCheckBox>
#include <QFrame>
#include <QProgressBar>
#include <QFuture>
#include <QFutureWatcher>
#include <QCloseEvent>
#include <QLineEdit>
#include <QTableView>
#include <QAbstractTableModel>
#include <QSvgRenderer>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QDateTime>
#include <algorithm>
#include <execution>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QClipboard>
#include <QApplication>
#include <QProcess>
#include <QMessageBox>
#include <QInputDialog>
#include <QPointer>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrent>
#include <QDir>
#include <QReadLocker>
#include <QWriteLocker>
#include <numeric>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <windows.h>
#include <shellapi.h>

#include "ScanController.h"
#include "JustifiedView.h"
#include "ThumbnailDelegate.h"
#include <memory>
#include <algorithm>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef run
#undef run
#endif


namespace ArcMeta {

// --- ScanConfig Implementation ---

void ScanConfig::load() {
    QFile file("arcmeta_scan_config.json");
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject obj = doc.object();
        
        auto loadSet = [&](const QString& key, QSet<QString>& set) {
            set.clear();
            QJsonArray arr = obj[key].toArray();
            for (const auto& v : arr) set.insert(v.toString());
        };
        
        loadSet("activeDrives", activeDrives);
        loadSet("defaultDrives", defaultDrives);
        loadSet("ignoredDrives", ignoredDrives);
        
        QJsonArray qArr = obj["queryHistory"].toArray();
        for (const auto& v : qArr) queryHistory.append(v.toString());
        QJsonArray eArr = obj["extHistory"].toArray();
        for (const auto& v : eArr) extHistory.append(v.toString());
        
        // 2026-05-16 持久化加载：视图、图标尺寸与排序规则
        if (obj.contains("viewMode")) viewMode = obj["viewMode"].toInt();
        if (obj.contains("iconSize")) iconSize = obj["iconSize"].toInt();
        if (obj.contains("sortColumn")) sortColumn = obj["sortColumn"].toInt();
        if (obj.contains("sortOrder")) sortOrder = obj["sortOrder"].toInt();

        if (obj.contains("useRegex")) useRegex = obj["useRegex"].toBool();
        if (obj.contains("caseSensitive")) caseSensitive = obj["caseSensitive"].toBool();
        if (obj.contains("includeHidden")) includeHidden = obj["includeHidden"].toBool();
        if (obj.contains("includeSystem")) includeSystem = obj["includeSystem"].toBool();
        if (obj.contains("includeDollar")) includeDollar = obj["includeDollar"].toBool();
        if (obj.contains("autoDisplay")) autoDisplay = obj["autoDisplay"].toBool();
    }
}

void ScanConfig::save() {
    QFile file("arcmeta_scan_config.json");
    if (file.open(QIODevice::WriteOnly)) {
        QJsonObject obj;
        auto saveSet = [&](const QString& key, const QSet<QString>& set) {
            QJsonArray arr;
            for (const auto& v : set) arr.append(v);
            obj[key] = arr;
        };
        
        saveSet("activeDrives", activeDrives);
        saveSet("defaultDrives", defaultDrives);
        saveSet("ignoredDrives", ignoredDrives);
        
        QJsonArray qArr; for (const auto& v : queryHistory) qArr.append(v);
        obj["queryHistory"] = qArr;
        QJsonArray eArr; for (const auto& v : extHistory) eArr.append(v);
        obj["extHistory"] = eArr;
        
        // 2026-05-16 持久化存盘
        obj["viewMode"] = viewMode;
        obj["iconSize"] = iconSize;
        obj["sortColumn"] = sortColumn;
        obj["sortOrder"] = sortOrder;

        obj["useRegex"] = useRegex;
        obj["caseSensitive"] = caseSensitive;
        obj["includeHidden"] = includeHidden;
        obj["includeSystem"] = includeSystem;
        obj["includeDollar"] = includeDollar;
        obj["autoDisplay"] = autoDisplay;
        
        file.write(QJsonDocument(obj).toJson());
    }
}

// --- ScanTableModel Implementation ---

ScanTableModel::ScanTableModel(ScanController* controller, QObject* parent) 
    : QAbstractTableModel(parent), m_controller(controller) 
{
    m_currentResultSet = std::make_shared<ResultSet>();
    m_thumbCache.setMaxCost(500); // 限制缩略图内存占用
    m_throttleTimer = new QTimer(this);
    m_throttleTimer->setInterval(100); 
    connect(m_throttleTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingRows.isEmpty()) return;
        QList<int> rows = m_pendingRows.values();
        std::sort(rows.begin(), rows.end());
        m_pendingRows.clear();
        int startRow = rows[0];
        int endRow = rows[0];
        for (int i = 1; i < rows.size(); ++i) {
            if (rows[i] == endRow + 1) endRow = rows[i];
            else {
                emit dataChanged(index(startRow, 0), index(endRow, 3));
                startRow = rows[i]; endRow = rows[i];
            }
        }
        emit dataChanged(index(startRow, 0), index(endRow, 3));
    });

    // 2026-06-xx 架构重构：切换至 Controller 驱动的原子快照更新 (使用信号携带的快照，绝对安全)
    connect(m_controller, &ScanController::resultsSwapped, this, [this](std::shared_ptr<ResultSet> newSet) {
        m_currentResultSet = newSet;
        updateResults();
    });
    
    connect(m_controller, &ScanController::entryAdded, this, [this](std::shared_ptr<ResultSet> newSet, uint64_t key, int row) {
        Q_UNUSED(key);
        m_currentResultSet = newSet;
        if (row <= m_displayCount) {
            beginInsertRows(QModelIndex(), row, row);
            m_displayCount++;
            endInsertRows();
        }
    });

    connect(m_controller, &ScanController::entryRemoved, this, [this](std::shared_ptr<ResultSet> newSet, uint64_t key, int row) {
        Q_UNUSED(key);
        m_currentResultSet = newSet;
        if (row < m_displayCount) {
            beginRemoveRows(QModelIndex(), row, row);
            m_displayCount--;
            endRemoveRows();
        }
    });

    connect(m_controller, &ScanController::entryUpdated, this, [this](std::shared_ptr<ResultSet> newSet, uint64_t key, int row) {
        Q_UNUSED(key);
        m_currentResultSet = newSet;
        if (row < m_displayCount) {
            m_pendingRows.insert(row);
            if (!m_throttleTimer->isActive()) m_throttleTimer->start();
        }
    });
}
ScanTableModel::~ScanTableModel() {}

int ScanTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_displayCount;
}

int ScanTableModel::columnCount(const QModelIndex& /*parent*/) const { return 4; }

QVariant ScanTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return QVariant();
    int row = index.row();
    if (row < 0 || row >= (int)m_currentResultSet->keys.size()) return QVariant();
    
    uint64_t key = m_currentResultSet->keys[row];
    auto& reader = MftReader::instance();
    int actualIndex = reader.getIndexByKey(key);
    if (actualIndex == -1) return QVariant(); // 文件可能已被删除
    
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case 0: return reader.getName(actualIndex);
            case 1: return reader.getFullPath(actualIndex);
            case 2: {
                if (reader.isDirectory(actualIndex)) return "-";
                int64_t size = reader.getSize(actualIndex);
                if (size == 0 && !reader.isMetadataFetched(actualIndex)) {
                    const_cast<MftReader&>(reader).requestMetadata(actualIndex);
                    return "...";
                }
                if (size < 1024) return QString("%1 B").arg(size);
                if (size < 1024 * 1024) return QString("%1 KB").arg(size / 1024.0, 0, 'f', 2);
                if (size < 1024LL * 1024 * 1024) return QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
                return QString("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
            }
            case 3: {
                int64_t ts = reader.getModifyTime(actualIndex);
                if (ts == 0 && !reader.isMetadataFetched(actualIndex)) {
                    const_cast<MftReader&>(reader).requestMetadata(actualIndex);
                    return "-";
                }
                if (ts == 0) return "-";
                return QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
            }
        }
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        QString name = reader.getName(actualIndex);
        int dotIdx = name.lastIndexOf('.');
        QString ext = (dotIdx != -1) ? name.mid(dotIdx + 1).toLower() : "";
        
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        if (thumbExts.contains(ext) && !reader.isDirectory(actualIndex)) {
            QString fullPath = reader.getFullPath(actualIndex);
            int64_t size = reader.getSize(actualIndex);
            int64_t mtime = reader.getModifyTime(actualIndex);
            QString cacheKey = QString("%1_%2_%3").arg(fullPath).arg(size).arg(mtime);

            QPixmap* cached = m_thumbCache.object(cacheKey);
            if (cached) return *cached;

            if (!m_requestedThumbs.contains(key)) {
                m_requestedThumbs.insert(key);
                ScanTableModel* mutableThis = const_cast<ScanTableModel*>(this);
                ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                int thumbSize = (dlg && dlg->m_viewStack->currentIndex() == 0) ? 24 : (dlg ? dlg->m_config.iconSize : 64);

                (void)QtConcurrent::run([mutableThis, key, fullPath, cacheKey, thumbSize, ext]() {
                    QImage img;
                    if (ext == "svg") {
                        QSvgRenderer renderer(fullPath);
                        if (renderer.isValid()) {
                            img = QImage(thumbSize, thumbSize, QImage::Format_ARGB32);
                            img.fill(Qt::transparent);
                            QPainter painter(&img);
                            renderer.render(&painter);
                            painter.end();
                        }
                    } else {
                        img = UiHelper::getShellThumbnail(fullPath, thumbSize);
                    }
                    if (!img.isNull()) {
                        double ar = (double)img.width() / img.height();
                        QMetaObject::invokeMethod(mutableThis, [mutableThis, key, cacheKey, img, ar]() {
                            // 物理加固：显式转换并验证，杜绝类型初始化错误
                            QPixmap pix = QPixmap::fromImage(img);
                            if (!pix.isNull()) {
                                mutableThis->m_thumbCache.insert(cacheKey, new QPixmap(pix));
                            }
                            mutableThis->m_aspectRatios[key] = ar;
                            
                            // 2026-06-xx 物理安全：直接从 Snapshot 中定位 Position，杜绝脱节
                            auto snapshot = mutableThis->m_controller->snapshot();
                            auto itPos = snapshot->keyToPos.find(key);
                            if (itPos != snapshot->keyToPos.end() && itPos->second < mutableThis->m_displayCount) {
                                // 2026-06-xx 布局优化：显式发射 UserRole+2 角色，通知 JustifiedView 真实宽高比已就绪，触发重排
                                emit mutableThis->dataChanged(mutableThis->index(itPos->second, 0), mutableThis->index(itPos->second, 0), {Qt::DecorationRole, Qt::UserRole + 1, Qt::UserRole + 2});
                            }
                        });
                    }
                });
            }
        }
        return reader.getCachedIcon(ext, reader.isDirectory(actualIndex));
    } else if (role == Qt::ForegroundRole) {
        // 2026-05-16 视觉同步：从 MetadataManager 获取颜色标记并适配主界面高端色值
        // 2026-05-17 按照用户要求：使用 UiHelper::parseColorName 确保所有颜色（如黄色 #FECF0E）完全一致高雅
        std::wstring path = reader.getFullPath(actualIndex).toStdWString();
        auto meta = MetadataManager::instance().getMeta(path);
        if (!meta.color.empty()) {
            QColor tagC = UiHelper::parseColorName(QString::fromStdWString(meta.color));
            if (tagC.isValid()) return tagC;
        }
        // 2026-06-xx 按照用户要求：名称列（第0列）强制显示为蓝色
        if (index.column() == 0 || reader.isDirectory(actualIndex)) return QColor("#3498db");
    } else if (role == Qt::ToolTipRole) {
        // 2026-05-16 交互同步：显示备注与标签
        // 2026-06-xx 物理修复：移除错误的 QLatin1String 引用，改用 UTF-8 字面量修复中文乱码
        std::wstring path = reader.getFullPath(actualIndex).toStdWString();
        auto meta = MetadataManager::instance().getMeta(path);
        QString tip = QString::fromUtf8("路径: ") + QString::fromStdWString(path);
        if (!meta.note.empty()) tip += QString::fromUtf8("\n备注: ") + QString::fromStdWString(meta.note);
        if (!meta.tags.isEmpty()) tip += QString::fromUtf8("\n标签: ") + meta.tags.join(", ");
        return tip;
    } else if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
            case 0: case 1: return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
            case 2: case 3: return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
    } else if (role == Qt::UserRole) {
        return key;
    } else if (role == Qt::UserRole + 1) {
        // 返回是否是缩略图 (用于 Delegate 区分绘制逻辑)
        QString name = reader.getName(actualIndex);
        int dotIdx = name.lastIndexOf('.');
        QString ext = (dotIdx != -1) ? name.mid(dotIdx + 1).toLower() : "";
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        
        int64_t size = reader.getSize(actualIndex);
        int64_t mtime = reader.getModifyTime(actualIndex);
        QString cacheKey = QString("%1_%2_%3").arg(reader.getFullPath(actualIndex)).arg(size).arg(mtime);
        
        return thumbExts.contains(ext) && m_thumbCache.contains(cacheKey);
    } else if (role == Qt::UserRole + 2) {
        // 返回宽高比 (用于 JustifiedView 布局)
        return m_aspectRatios.value(key, 1.0);
    }
    return QVariant();
}

Qt::ItemFlags ScanTableModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() == 0) {
        // 2026-05-16 物理对标：仅名称列允许行内编辑
        f |= Qt::ItemIsEditable;
    }
    return f;
}

bool ScanTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || role != Qt::EditRole || index.column() != 0) return false;
    
    int row = index.row();
    if (row < 0 || row >= (int)m_currentResultSet->keys.size()) return false;
    
    uint64_t key = m_currentResultSet->keys[row];
    auto& reader = MftReader::instance();
    int actualIndex = reader.getIndexByKey(key);
    if (actualIndex == -1) return false;
    
    QString oldName = reader.getName(actualIndex);
    QString newName = value.toString().trimmed();
    if (newName.isEmpty() || newName == oldName) return false;
    
    QString oldPath = reader.getFullPath(actualIndex);
    QFileInfo fi(oldPath);
    QString newPath = fi.absolutePath() + QLatin1String("/") + newName;
    
    if (QFile::rename(oldPath, newPath)) {
        // 2026-05-16 交互加固：物理重命名后，USN 监听器会捕获事件并自动更新模型。
        // 我们在此处不需要手动修改内存池，等待系统级同步最为稳健。
        return true;
    } else {
        QMessageBox::warning(nullptr, "重命名失败", "无法重命名文件，请检查文件是否被占用或是否有权限。");
        return false;
    }
}

QVariant ScanTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
            case 0: return "名称";
            case 1: return "路径";
            case 2: return "大小";
            case 3: return "修改日期";
        }
    }
    return QVariant();
}

void ScanTableModel::updateResults() {
    beginResetModel();
    m_currentResultSet = m_controller->snapshot();
    m_displayCount = (std::min<int>)(static_cast<int>(m_currentResultSet->keys.size()), 100); 
    
    m_requestedThumbs.clear();
    endResetModel();
}

bool ScanTableModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid()) return false;
    return m_displayCount < (int)m_currentResultSet->keys.size();
}

void ScanTableModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid()) return;
    int remainder = static_cast<int>(m_currentResultSet->keys.size()) - m_displayCount;
    int itemsToFetch = (std::min<int>)(remainder, 100);
    
    beginInsertRows(QModelIndex(), m_displayCount, m_displayCount + itemsToFetch - 1);
    m_displayCount += itemsToFetch;
    endInsertRows();
}

void ScanTableModel::sort(int column, Qt::SortOrder order) {
    // 2026-06-xx 逻辑剥离：Model 不再拥有排序权，仅向 Controller 发起异步请求
    m_controller->sort(column, static_cast<int>(order));
}

Qt::DropActions ScanTableModel::supportedDragActions() const {
    return Qt::CopyAction;
}

QMimeData* ScanTableModel::mimeData(const QModelIndexList& indexes) const {
    QMimeData* data = new QMimeData();
    QList<QUrl> urls;
    QSet<int> seen;
    for (const QModelIndex& idx : indexes) {
        if (idx.column() != 0) continue;
        int row = idx.row();
        if (row < 0 || row >= (int)m_currentResultSet->keys.size()) continue;
        uint64_t key = m_currentResultSet->keys[row];
        int actualIdx = MftReader::instance().getIndexByKey(key);
        if (actualIdx == -1 || seen.contains(actualIdx)) continue;
        seen.insert(actualIdx);
        QString path = MftReader::instance().getFullPath(actualIdx);
        if (!path.isEmpty()) urls << QUrl::fromLocalFile(path);
    }
    data->setUrls(urls);
    return data;
}

// --- ScanDialog Implementation ---

ScanDialog::ScanDialog(QWidget* parent)
    : FramelessDialog("FERREX-META", parent) 
{
    m_config.load();
    resize(1000, 700);
    setMinimumSize(800, 500);

    m_titleStatusLabel = new QLabel("READY - 0");
    // 按照用户要求：间距严格对齐规范。 margin-left: 1px (配合 layout spacing 4px = 5px)
    m_titleStatusLabel->setStyleSheet("background: transparent; color: #46B478; font-size: 10px; font-weight: bold; margin-left: 1px;");

    if (m_titleLabel && m_pinBtn && m_pinBtn->parentWidget() && m_pinBtn->parentWidget()->layout()) {
        m_titleLabel->hide(); 
        auto* titleLayout = qobject_cast<QHBoxLayout*>(m_pinBtn->parentWidget()->layout());
        if (titleLayout) {
            // 按照用户要求：容器规范高度 34px，布局间距严格锁定 4px
            m_pinBtn->parentWidget()->setFixedHeight(34);
            titleLayout->setSpacing(4);
            titleLayout->setContentsMargins(12, 0, 8, 0);

            QLabel* logoLabel = new QLabel();
            logoLabel->setFixedSize(16, 16);
            logoLabel->setPixmap(UiHelper::getIcon("ferrex", QColor("#FF8C00"), 16).pixmap(16, 16));
            // 物理修正：显式清除所有边距以确保基准对齐
            logoLabel->setStyleSheet("background: transparent; margin: 0px; padding: 0px;"); 
            titleLayout->insertWidget(0, logoLabel);
            
            QLabel* brandLabel = new QLabel("FERREX-META");
            brandLabel->setObjectName("TitleBrandLabel");
            // 物理修正：将 margin-left 设为 0px。配合 Layout Spacing 4px 达到 4px 左右的极紧凑视觉
            brandLabel->setStyleSheet("background: transparent; color: #FF8C00; font-size: 14px; font-weight: bold; letter-spacing: 1.5px; margin-left: 0px; padding: 0px;");
            titleLayout->insertWidget(1, brandLabel);
            
            titleLayout->insertWidget(2, m_titleStatusLabel);

            // 按照截图要求调整布局：
            // [Logo/标题/状态] -> [Stretch] -> [滑动条③] -> [视图按钮②] -> [窗口控制按钮①]
            
            // 找到窗口控制按钮（m_pinBtn 等）在 layout 中的起始索引。
            // 在 FramelessDialog 中，它们是依次 addWidget 的。
            // 这里 titleLayout 是从 FramelessDialog 继承而来的，m_pinBtn 应该已经在里面。
            
            titleLayout->insertStretch(titleLayout->indexOf(m_pinBtn));

            // ① 视图切换按钮 (标记 2)
            QPushButton* viewBtn = new QPushButton(); 
            viewBtn->setFixedSize(20, 20); // 严格锁定 20x20
            viewBtn->setIcon(UiHelper::getIcon("grid", QColor("#CCCCCC"), 16)); // 严格锁定图标 16x16
            viewBtn->setIconSize(QSize(16, 16));
            viewBtn->setCursor(Qt::PointingHandCursor); 
            viewBtn->setToolTip(""); // 禁止原生 ToolTip
            viewBtn->setStyleSheet( 
                "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 0; }" 
                "QPushButton:hover { background: rgba(255, 255, 255, 0.1); }" 
                "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }" 
            ); 
            connect(viewBtn, &QPushButton::clicked, this, [this, viewBtn]() { 
                QMenu* menu = new QMenu(this); 
                UiHelper::applyMenuStyle(menu);
                struct ViewDef { QString label; int stackIdx; int size; }; 
                for (auto& v : QList<ViewDef>{ 
                    {"超大图标", 1, 192}, {"大图标", 1, 128}, {"中图标", 1, 64}, 
                    {}, // separator 
                    {"详情",    0, 0} 
                }) { 
                    if (v.label.isEmpty()) { menu->addSeparator(); continue; } 
                    QAction* act = menu->addAction(v.label); 
                    act->setCheckable(true); 
                    act->setChecked(m_viewStack->currentIndex() == v.stackIdx && 
                                    (v.stackIdx == 0 || m_config.iconSize == v.size)); 
                    connect(act, &QAction::triggered, this, [this, v]() { 
                        m_viewStack->setCurrentIndex(v.stackIdx); 
                        m_config.viewMode = v.stackIdx; 
                        if (v.stackIdx == 1) { 
                            m_config.iconSize = v.size; 
                            m_iconView->setTargetRowHeight(v.size); 
                            if (m_sizeSlider) m_sizeSlider->setValue(v.size); 
                        } 
                        if (v.stackIdx == 0) 
                            m_resultView->verticalHeader()->setDefaultSectionSize(m_config.iconSize); 
                        m_config.save(); 
                    }); 
                } 
                menu->exec(viewBtn->mapToGlobal(QPoint(0, viewBtn->height() + 2))); 
            }); 

            // ② 尺寸滑动条 (标记 3)
            m_sizeSlider = new QSlider(Qt::Horizontal); 
            m_sizeSlider->setRange(32, 256); 
            m_sizeSlider->setValue(m_config.iconSize > 0 ? m_config.iconSize : 64); 
            m_sizeSlider->setFixedSize(110, 20); // 高度调整为 20px，避免覆盖/截断
            m_sizeSlider->setCursor(Qt::PointingHandCursor); 
            m_sizeSlider->installEventFilter(this);
            // 间距计算：margin-right 1px + spacing 4px = 5px (精准对标视图按钮)
            m_sizeSlider->setStyleSheet( 
                "QSlider { background: transparent; margin-right: 1px; }"
                "QSlider::groove:horizontal { height: 3px; background: #3F3F3F; border-radius: 2px; }" 
                "QSlider::sub-page:horizontal { background: #FF8C00; border-radius: 2px; }" 
                "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0; " 
                "  background: #FF8C00; border-radius: 6px; }" 
            ); 
            connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int v) { 
                m_config.iconSize = v; 
                m_resultView->verticalHeader()->setDefaultSectionSize(v); 
                m_iconView->setTargetRowHeight(v); 
                m_tableModel->clearThumbCache(); 
                m_tableModel->updateResults(); // 确保触发重新加载并生成新尺寸的缩略图
                m_config.save(); 
            }); 
            
            titleLayout->insertWidget(titleLayout->indexOf(m_pinBtn), viewBtn);
            titleLayout->insertWidget(titleLayout->indexOf(viewBtn), m_sizeSlider);

            // 更新现有控制按钮样式以对标规范
            for (auto* btn : {m_pinBtn, m_minBtn, m_maxBtn}) {
                if (!btn) continue;
                btn->setFixedSize(20, 20);
                btn->setIconSize(QSize(16, 16));
                btn->setToolTip("");
                if (btn == m_pinBtn) {
                     btn->setStyleSheet(
                        "QPushButton { background: transparent; border: none; border-radius: 4px; } "
                        "QPushButton:hover { background: rgba(255, 255, 255, 0.1); } "
                        "QPushButton:checked { background: rgba(255, 85, 28, 0.2); }"
                    );
                } else {
                    btn->setStyleSheet(
                        "QPushButton { background: transparent; border: none; border-radius: 4px; } "
                        "QPushButton:hover { background: rgba(255, 255, 255, 0.1); } "
                        "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
                    );
                }
            }
            if (m_closeBtn) {
                m_closeBtn->setFixedSize(20, 20);
                m_closeBtn->setIconSize(QSize(16, 16));
                m_closeBtn->setToolTip("");
                m_closeBtn->setStyleSheet(
                    "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
                    "QPushButton:hover { background-color: #F1707A; } "
                    "QPushButton:pressed { background-color: #A50000; }"
                );
            }
        } else {
            m_titleStatusLabel->hide(); 
        }
    } else {
        m_titleStatusLabel->hide();
    }

    setupUi();

    // --- 2026-06-xx 架构级 QSS：实现样式沙箱与物理隔离 ---
    // 2026-06-xx 物理修正：将标题栏品牌标签样式移入此处，确保优先级并严格锁定 1px 边距 (+4px Spacing = 5px Gap)
    this->setStyleSheet(this->styleSheet() + R"(
        #TitleBrandLabel {
            background: transparent; 
            color: #FF8C00; 
            font-size: 14px; 
            font-weight: bold; 
            letter-spacing: 1.5px; 
            /* 物理负边距补偿：由 -2px 增加至 -4px，确保产生非常明显的紧凑效果 */
            margin-left: -4px; 
            padding: 0px;
        }

        #DialogContainer {
            background-color: #1E1E1E;
            border: 1px solid #333333;
            border-radius: 6px;
        }

        QWidget#SearchContainer, QWidget#DriveContainer { 
            background: transparent; border: none; 
        }

        QStackedWidget#ViewStack {
            background-color: #1E1E1E;
            border: none;
        }
        
        #mainSearchEdit, #extSearchEdit { 
            background: #2D2D2D; 
            border: 1px solid #FF8C00; 
            border-radius: 6px; 
            color: #EEE; 
            font-size: 14px; 
            padding: 0 10px;
            outline: none;
        }

        /* 显式定义伪类，保持橙色边框 */
        #mainSearchEdit:focus, #extSearchEdit:focus { border: 1px solid #FF8C00 !important; }
        #mainSearchEdit:hover, #extSearchEdit:hover { border: 1px solid #FF8C00; }
        
        #mainSearchEdit::placeholder, #extSearchEdit::placeholder {
            color: rgba(238, 238, 238, 0.3);
        }

        /* 搜索按钮：独立物理实体，拥有完整圆角 */
        QPushButton#searchIconButton { 
            background: #FF8C00; 
            border: 1px solid #FF8C00;
            border-radius: 6px; 
            color: #000;
            font-weight: bold;
            padding: 0 15px;
        } 
        QPushButton#searchIconButton:hover { background: #FFA500; } 
        QPushButton#searchIconButton:pressed { background: #CC6600; }

        /* 盘符按钮：使用属性选择器 */
        QPushButton[isActive="true"] {
            background: rgba(255, 140, 0, 30); 
            color: #FF8C00; 
            border: 1px solid #FF8C00; 
            padding: 0 10px; 
            font-size: 12px; 
            font-weight: bold;
            border-radius: 4px;
        }
        QPushButton[isActive="false"] {
            background: #111519; 
            color: #7A8F9E; 
            border: 1px solid #252E37; 
            padding: 0 10px; 
            font-size: 12px;
            border-radius: 4px;
        }

        QProgressBar#ScanProgressBar { background: transparent; border: none; } 
        QProgressBar#ScanProgressBar::chunk { background: #FF8C00; }

        QCheckBox { color: #AAA; }

        /* 全局滚动条美化 */
        QScrollBar:vertical {
            border: none;
            background: transparent;
            width: 4px;
            margin: 0px;
        }
        QScrollBar::handle:vertical {
            background: #333333;
            min-height: 20px;
            border-radius: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: #444444;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: none;
        }

        QScrollBar:horizontal {
            border: none;
            background: transparent;
            height: 4px;
            margin: 0px;
        }
        QScrollBar::handle:horizontal {
            background: #333333;
            min-width: 20px;
            border-radius: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #444444;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
            background: none;
        }
    )");

    // --- 2026-05-16 持久化恢复：根据配置恢复视图、尺寸与排序状态 ---
        m_viewStack->setCurrentIndex(m_config.viewMode);
    if (m_config.viewMode == 0) {
        m_resultView->verticalHeader()->setDefaultSectionSize(32);
    } else {
        m_resultView->verticalHeader()->setDefaultSectionSize(m_config.iconSize + 10);
    }
    if (m_config.viewMode == 1) { // 图标模式
        m_iconView->setTargetRowHeight(m_config.iconSize);
    }
    
    // 恢复排序状态 (同时作用于模型和表头视觉)
    m_resultView->horizontalHeader()->setSortIndicator(m_config.sortColumn, static_cast<Qt::SortOrder>(m_config.sortOrder));
    m_tableModel->sort(m_config.sortColumn, static_cast<Qt::SortOrder>(m_config.sortOrder));

    // 2026-06-xx 物理对标：监听引擎加载信号，实现“更新数据中...”的体感同步
    connect(&MftReader::instance(), &MftReader::driveLoaded, this, [this](const QString& drive, int count, int total) {
        updateStatus(QString("正在加载快照 %1 (%2)...").arg(drive).arg(formatNumber(count)), true, total);
    });

    // 2026-05-16 物理重载：断开基类 Qt 置顶逻辑，改用 Win32 原生 SetWindowPos 以实现无损切换
    if (m_pinBtn) {
        disconnect(m_pinBtn, &QPushButton::toggled, nullptr, nullptr);
        connect(m_pinBtn, &QPushButton::toggled, this, [this](bool checked) {
            m_pinBtn->setIcon(UiHelper::getIcon(checked ? "pin_vertical" : "pin_tilted", 
                                                checked ? QColor("#FF551C") : QColor("#CCCCCC"), 18));
            HWND hwnd = reinterpret_cast<HWND>(winId());
            if (checked) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            } else {
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
        });
    }

    QTimer::singleShot(100, this, [this]() {
        updateStatus("正在载入本地快照...");
        QPointer<ScanDialog> weakThis(this);
        (void)(QtConcurrent::run)([weakThis]() {
            bool ok = MftReader::instance().loadFromCache();
            QMetaObject::invokeMethod(weakThis.data(), [weakThis, ok]() {
                if (!weakThis) return;
                if (ok) {
                    weakThis->updateStatus("就绪");
                    weakThis->m_controller->setSearchText("");
                    weakThis->refreshDriveList(true); // 后台探测硬件
                    // 2026-06-xx 物理对标：如果开启了“自动显示”，加载快照后立即触发一次全量过滤显示
                    if (weakThis->m_config.autoDisplay) {
                        weakThis->onFilterOptionChanged();
                    }
                } else {
                    weakThis->updateStatus("未检测到快照，全自动初始化...");
                    weakThis->refreshDriveList(true);
                    weakThis->onStartScan();
                }
            });
        });
    });
}

ScanDialog::~ScanDialog() {
    // 2026-06-xx 内存优化专项：按照用户要求实现“按需加载、及时卸载”。
    // 关闭搜索窗口时物理卸载 MFT 索引，释放可能高达数百 MB 的内存占用。
    MftReader::instance().clear();
}

void ScanDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(m_contentArea);
    // 2026-06-xx 按照建议：将 mainLayout 的 spacing 设置为 10，给组件之间留出物理切割的空隙
    // 按照用户要求：底部边距设为 0，确保状态栏紧贴底边且高度严格受控
    mainLayout->setContentsMargins(10, 10, 10, 0);
    mainLayout->setSpacing(10);

    auto* driveScroll = new QScrollArea();
    driveScroll->setFixedHeight(45);
    driveScroll->setWidgetResizable(true);
    driveScroll->setFrameShape(QFrame::NoFrame);
    driveScroll->setStyleSheet("background: #252526; border: 1px solid #333; border-radius: 4px;");

    m_driveContainer = new QWidget();
    m_driveContainer->setObjectName("DriveContainer");
    m_driveContainer->setAttribute(Qt::WA_StyledBackground, true);
    m_driveLayout = new QHBoxLayout(m_driveContainer);
    // 2026-06-xx 按照建议：盘符起始坐标向右偏移 5 像素 (5 -> 10)
    m_driveLayout->setContentsMargins(10, 0, 5, 0);
    m_driveLayout->setSpacing(10);
    driveScroll->setWidget(m_driveContainer);

    auto* topControl = new QHBoxLayout();
    // 物理修正：恢复容器边距为 0，防止整个框发生偏移
    topControl->setContentsMargins(0, 0, 0, 0);
    topControl->addWidget(driveScroll, 1);
    mainLayout->addLayout(topControl);

    // B. 搜索选项行 (迁移至盘符与搜索框之间)
    auto* optionRow = new QHBoxLayout();
    optionRow->setContentsMargins(0, 0, 0, 0);
    optionRow->setSpacing(15);
    
    m_checkRegex = new QCheckBox("正则");
    m_checkCase = new QCheckBox("大小写");
    m_checkHidden = new QCheckBox("隐藏");
    m_checkSystem = new QCheckBox("系统");
    m_checkDollar = new QCheckBox("显示$");
    m_checkAuto = new QCheckBox("自动显示");

    m_checkRegex->setChecked(m_config.useRegex);
    m_checkCase->setChecked(m_config.caseSensitive);
    m_checkHidden->setChecked(m_config.includeHidden);
    m_checkSystem->setChecked(m_config.includeSystem);
    m_checkDollar->setChecked(m_config.includeDollar);
    m_checkAuto->setChecked(m_config.autoDisplay);

    for (auto* cb : {m_checkRegex, m_checkCase, m_checkHidden, m_checkSystem, m_checkDollar, m_checkAuto}) {
        connect(cb, &QCheckBox::toggled, this, &ScanDialog::onFilterOptionChanged);
        optionRow->addWidget(cb);
    }
    optionRow->addStretch();
    mainLayout->addLayout(optionRow);

    auto* searchContainer = new QWidget();
    searchContainer->setObjectName("SearchContainer");
    searchContainer->setAttribute(Qt::WA_StyledBackground, true);
    auto* searchVLayout = new QVBoxLayout(searchContainer);
    searchVLayout->setContentsMargins(0, 0, 0, 0);
    searchVLayout->setSpacing(10); // 增加呼吸感

    auto* searchRow = new QHBoxLayout();
    searchRow->setContentsMargins(0, 0, 0, 0); 
    searchRow->setSpacing(10); 

    // A. 物理拆分搜索栏组件：恢复“分开”的视觉风格，确保组件间有明显间距
    m_searchEdit = new QLineEdit();
    m_searchEdit->setObjectName("mainSearchEdit");
    m_searchEdit->setPlaceholderText("输入文件名 / 关键词...");
    m_searchEdit->setFixedHeight(36);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_controller->setSearchText(text);
        m_controller->triggerSearch();
    });
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_searchEdit, 1);

    m_extEdit = new QLineEdit();
    m_extEdit->setObjectName("extSearchEdit");
    m_extEdit->setPlaceholderText("后缀");
    m_extEdit->setFixedWidth(120); 
    m_extEdit->setFixedHeight(36);
    m_extEdit->setClearButtonEnabled(true);
    m_extEdit->installEventFilter(this);
    connect(m_extEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        onFilterOptionChanged();
    });
    connect(m_extEdit, &QLineEdit::returnPressed, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_extEdit);

    m_searchBtn = new QPushButton("搜索");
    m_searchBtn->setObjectName("searchIconButton");
    m_searchBtn->setFixedWidth(80);
    m_searchBtn->setFixedHeight(36); 
    m_searchBtn->setCursor(Qt::PointingHandCursor);
    m_searchBtn->setIcon(UiHelper::getIcon("search", QColor("#000000"), 18));
    m_searchBtn->setIconSize(QSize(18, 18));
    connect(m_searchBtn, &QPushButton::clicked, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_searchBtn);

    searchVLayout->addLayout(searchRow);

    m_progressBar = new QProgressBar();
    m_progressBar->setObjectName("ScanProgressBar");
    m_progressBar->setFixedHeight(2);
    m_progressBar->setTextVisible(false);
    m_progressBar->hide();
    searchVLayout->addWidget(m_progressBar);

    mainLayout->addWidget(searchContainer);

    m_resultView = new QTableView();
    m_resultView->verticalHeader()->setDefaultSectionSize(30); // 默认行高
    m_controller = new ScanController(this);
    m_tableModel = new ScanTableModel(m_controller, this);
    m_resultView->setModel(m_tableModel);
    m_resultView->setContextMenuPolicy(Qt::CustomContextMenu);
    
    // 2026-05-14 视觉优化：基于色码分析，将斑马纹调整为深灰色 (#1E1E1E) 与纯黑色 (#000000) 搭配
    // 2026-06-xx 按照用户要求：设置左侧 10px 间距，确保坐标校准，同时修正表头首列偏移
    m_resultView->setStyleSheet(
        "QTableView { "
        "background-color: #1E1E1E; "
        "alternate-background-color: #000000; "
        "border: 1px solid #333; "
        "color: #D4D4D4; "
        "selection-background-color: #094771; "
        "selection-color: #FFFFFF; "
        "outline: none; "
        "gridline-color: transparent; "
        "padding: 10px 0 0 10px; "
        "}"
        "QTableView::item { border-bottom: 1px solid #252526; }"
        "QHeaderView::section { background-color: #252526; color: #888; border: none; border-right: 1px solid #333; padding: 4px; height: 24px; }"
        "QHeaderView::section:horizontal:first { padding-left: 14px; }" // 10px 基础 + 4px 原有内边距
        "QHeaderView { background-color: #252526; border: none; }"
    );
    
    m_resultView->horizontalHeader()->setStretchLastSection(false); 
    m_resultView->horizontalHeader()->setMinimumSectionSize(60);
    // 2026-05-14 物理修正：强制列标题水平居中对齐
    m_resultView->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    
    m_resultView->setColumnWidth(0, 260); 
    m_resultView->setColumnWidth(2, 100); 
    m_resultView->setColumnWidth(3, 140); 
    
    m_resultView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_resultView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_resultView->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_resultView->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);

    m_resultView->verticalHeader()->setVisible(false);
    m_resultView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultView->setSelectionMode(QAbstractItemView::ExtendedSelection); // 显式启用多选
    m_resultView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    
    // 2026-06-xx 按照用户要求：开启 TableView 拖拽导出功能
    m_resultView->setDragEnabled(true);
    m_resultView->setDragDropMode(QAbstractItemView::DragOnly);
    m_resultView->setDefaultDropAction(Qt::CopyAction);

    m_resultView->setShowGrid(false);
    m_resultView->setAlternatingRowColors(true);
    
    connect(m_resultView, &QTableView::customContextMenuRequested, this, &ScanDialog::onCustomContextMenu);
    connect(m_resultView, &QTableView::doubleClicked, this, &ScanDialog::onItemDoubleClicked);
    connect(m_resultView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ScanDialog::onSelectionChanged);
    
    // 2026-05-16 交互优化：开启行内编辑触发器 (双击或按F2)
    m_resultView->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    
    // 虚拟化加载已由 QAbstractItemModel::fetchMore 处理，无需手动 loadMore
    
    // --- 2026-05-16 多态视图重构：引入 QStackedWidget 与 QListView (IconMode) ---
    m_viewStack = new QStackedWidget();
    m_viewStack->setObjectName("ViewStack");
    m_viewStack->addWidget(m_resultView);
    
    m_iconView = new JustifiedView();
    m_iconView->setModel(m_tableModel);
    m_iconView->setItemDelegate(new ThumbnailDelegate(this));
    m_iconView->setTargetRowHeight(m_config.iconSize);
    m_iconView->setSelectionMode(QAbstractItemView::ExtendedSelection); // 显式启用多选
    m_iconView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_iconView->setEditTriggers(QAbstractItemView::EditKeyPressed);
    
    // 2026-06-xx 按照用户要求：开启 IconView 拖拽导出功能
    m_iconView->setDragEnabled(true);

    // 2026-06-xx 按照用户要求：为网格视图增加 10px 左侧与顶部内边距，确保坐标对准
    m_iconView->setStyleSheet(
        "background-color: #1E1E1E; border: 1px solid #333; color: #D4D4D4; outline: none;"
    );
    
    connect(m_iconView, &QListView::doubleClicked, this, &ScanDialog::onItemDoubleClicked);
    connect(m_iconView, &QListView::customContextMenuRequested, this, &ScanDialog::onCustomContextMenu);
    connect(m_iconView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ScanDialog::onSelectionChanged);
    
    m_viewStack->addWidget(m_iconView);
    m_viewStack->setCurrentIndex(0); // 默认详情视图
    
    mainLayout->addWidget(m_viewStack);

    auto* statusContainer = new QWidget();
    statusContainer->setObjectName("StatusContainer");
    statusContainer->setFixedHeight(20);
    statusContainer->setStyleSheet("QWidget#StatusContainer { background: transparent; border: none; }");
    auto* statusBar = new QHBoxLayout(statusContainer);
    // 2026-06-xx 按照用户要求：显式设置垂直居中对齐，并向上偏移 10px (通过底部边距实现)
    statusBar->setAlignment(Qt::AlignVCenter);
    statusBar->setContentsMargins(16, 0, 16, 10);
    statusBar->setSpacing(0);

    m_statLabelMain = new QLabel("");
    m_statLabelMain->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_statLabelMain);

    m_statLabelTime = new QLabel("");
    m_statLabelTime->setStyleSheet("color: #7A8F9E; font-size: 10px; margin-left: 12px;");
    statusBar->addWidget(m_statLabelTime);

    m_selectionLabel = new QLabel("");
    m_selectionLabel->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_selectionLabel);

    m_csvBtn = new QPushButton("导出所选为 CSV");
    m_csvBtn->setFlat(true);
    m_csvBtn->setCursor(Qt::PointingHandCursor);
    m_csvBtn->setStyleSheet("QPushButton { color: #FF8C00; font-size: 10px; border: none; padding: 0 0 0 8px; text-decoration: none; } QPushButton:hover { text-decoration: underline; }");
    m_csvBtn->hide();
    statusBar->addWidget(m_csvBtn);

    statusBar->addStretch();

    m_statLabelMemory = new QLabel("");
    m_statLabelMemory->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_statLabelMemory);

    mainLayout->addWidget(statusContainer);

    connect(m_controller, &ScanController::searchFinished, this, [this](int count, int64_t elapsedMs) {
        Q_UNUSED(count);
        m_lastSearchMs = elapsedMs;
        m_tableModel->updateResults();
        updateStatusBar();
    });

    connect(m_controller, &ScanController::resultsSwapped, this, [this]() {
        updateStatusBar();
    });

    showDriveLoading();
}

void ScanDialog::showDriveLoading() {
    if (!m_driveLayout) return;

    QLayoutItem* child;
    while ((child = m_driveLayout->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }
    QLabel* loadingLbl = new QLabel("更新数据中...");
    loadingLbl->setStyleSheet("background: transparent; border: none; color: #7A8F9E; font-size: 12px; font-weight: bold; margin-left: 10px;");
    m_driveLayout->addWidget(loadingLbl);
    m_driveLayout->addStretch();
}

void ScanDialog::refreshDriveList(bool forceProbe) {
    if (!forceProbe && !m_cachedDriveInfos.isEmpty()) {
        updateDriveButtonStyles();
        return;
    }

    // 2026-06-xx 按照用户要求：加载盘符数据（.scch）之前，先显示占位提示
    showDriveLoading();

    QPointer<ScanDialog> weakThis(this);
    (void)(QtConcurrent::run)([weakThis]() {
        if (!weakThis) return;
        QVector<DriveInfo> drives;
        DWORD driveMask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (driveMask & (1 << i)) {
                QString letter = QString(QChar('A' + i)) + QLatin1String(":");
                WCHAR volName[MAX_PATH + 1] = {0};
                WCHAR fsName[MAX_PATH + 1] = {0};
                QString driveRoot = letter + QLatin1String("\\");
                BOOL ok = GetVolumeInformationW(reinterpret_cast<const wchar_t*>(driveRoot.utf16()), 
                                              volName, MAX_PATH + 1, NULL, NULL, NULL, 
                                              fsName, MAX_PATH + 1);
                DriveInfo info;
                info.letter = letter;
                info.hasMedia = ok;
                if (ok) {
                    info.label = QString::fromWCharArray(volName);
                    info.isNtfs = QString::fromWCharArray(fsName).contains("NTFS", Qt::CaseInsensitive);
                } else {
                    info.isNtfs = false;
                }
                drives.append(info);
            }
        }

        QMetaObject::invokeMethod(weakThis.data(), [weakThis, drives]() {
            if (!weakThis) return;
            weakThis->m_cachedDriveInfos = drives;
            
            QLayoutItem* item;
            while ((item = weakThis->m_driveLayout->takeAt(0)) != nullptr) {
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
            weakThis->m_driveButtonMap.clear();

            // 2026-05-14 用户要求彻底移除 "DRIVES" 标签
            // QLabel* driveLabel = new QLabel("DRIVES");
            // driveLabel->setStyleSheet("color: #3D5060; font-weight: bold; font-size: 10px;");
            // weakThis->m_driveLayout->addWidget(driveLabel);

            for (const auto& info : drives) {
                if (!info.hasMedia || !info.isNtfs) continue;
                if (weakThis->m_config.ignoredDrives.contains(info.letter)) continue;

                QString label = info.label.isEmpty() ? "本地磁盘" : info.label;
                QString btnText = QString("%1 (%2)").arg(info.letter).arg(label);
                
                QPushButton* btn = new QPushButton(btnText);
                btn->setCheckable(true);
                btn->setFixedHeight(24);
                weakThis->m_driveButtonMap[info.letter] = btn;
                
                connect(btn, &QPushButton::clicked, weakThis.data(), [weakThis, letter = info.letter]() {
                    if (!weakThis) return;
                    bool isSelected = false;
                    if (weakThis->m_config.activeDrives.contains(letter)) {
                        if (weakThis->m_config.activeDrives.size() > 1) {
                            weakThis->m_config.activeDrives.remove(letter);
                        } else {
                            isSelected = true; // 保持选中
                        }
                    } else {
                        weakThis->m_config.activeDrives.insert(letter);
                        isSelected = true;
                    }
                    
                    weakThis->updateDriveButtonStyles();

                    // 2026-05-14 核心同步：显式同步盘符状态至搜索引擎掩码，防止视图过滤失效
                    QStringList activeList;
                    for (const QString& d : weakThis->m_config.activeDrives) activeList << d;
                    MftReader::instance().updateActiveDrives(activeList);

                    // 2026-05-14 架构对标优化：如果驱动器已在索引中，仅进行视图过滤（瞬时响应）
                    // 只有当点击的是新驱动器且需要初始扫描时，才调用重量级的 onStartScan
                    if (isSelected && !MftReader::instance().isDriveIndexed(letter)) {
                        weakThis->onStartScan();
                    } else {
                        weakThis->onTriggerSearch();
                    }
                });
                
                btn->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(btn, &QPushButton::customContextMenuRequested, weakThis.data(), [weakThis, letter = info.letter](const QPoint& pos) {
                    if (weakThis) weakThis->onDriveContextMenu(letter, pos);
                });
                
                weakThis->m_driveLayout->addWidget(btn);
            }
            weakThis->m_driveLayout->addStretch();
            weakThis->updateDriveButtonStyles();
        });
    });
}

void ScanDialog::updateDriveButtonStyles() {
    for (auto it = m_driveButtonMap.begin(); it != m_driveButtonMap.end(); ++it) {
        bool isActive = m_config.activeDrives.contains(it.key());
        bool isDefault = m_config.defaultDrives.contains(it.key());
        
        QPushButton* btn = it.value();
        btn->setProperty("isActive", isActive);
        btn->setProperty("isDefault", isDefault);
        
        // 触发 QSS 刷新
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
        
        QString label = "";
        for (const auto& info : m_cachedDriveInfos) { if (info.letter == it.key()) { label = info.label; break; } }
        btn->setText(QString("%1%2 (%3)").arg(isDefault ? "★ " : "").arg(it.key()).arg(label.isEmpty() ? "本地磁盘" : label));
    }
}

void ScanDialog::onDriveContextMenu(const QString& drive, const QPoint& /*pos*/) {
    QMenu menu(this);
    UiHelper::applyMenuStyle(&menu);
    
    bool isDefault = m_config.defaultDrives.contains(drive);
    menu.addAction(isDefault ? "取消默认选项" : "设为默认选项", [this, drive, isDefault]() {
        if (isDefault) m_config.defaultDrives.remove(drive);
        else m_config.defaultDrives.insert(drive);
        m_config.save();
        updateDriveButtonStyles();
    });
    
    menu.addAction("忽略此驱动器", [this, drive]() {
        m_config.ignoredDrives.insert(drive);
        m_config.activeDrives.remove(drive);
        m_config.save();
        refreshDriveList(true); // 重新生成按钮
        onStartScan();
    });
    
    menu.exec(QCursor::pos());
}

void ScanDialog::onIgnoredDriveContextMenu(const QString& drive, const QPoint& pos) {
    Q_UNUSED(pos);
    QMenu menu(this);
    UiHelper::applyMenuStyle(&menu);
    menu.addAction("恢复驱动器", [this, drive]() {
        m_config.ignoredDrives.remove(drive);
        m_config.save();
        refreshDriveList(true);
    });
    menu.exec(QCursor::pos());
}

void ScanDialog::onCustomContextMenu(const QPoint& pos) {
    QAbstractItemView* activeView = (m_viewStack->currentIndex() == 0) ? static_cast<QAbstractItemView*>(m_resultView) : static_cast<QAbstractItemView*>(m_iconView);
    
    // 2026-05-16 空间感知修正：优先探测点击位置
    QModelIndex indexAtPos = activeView->indexAt(pos);
    QModelIndexList selectedRows;

    if (indexAtPos.isValid()) {
        // 1. 点击在项目上：确保该项被选中，并拉取所有选中项用于构建文件操作菜单
        if (!activeView->selectionModel()->isSelected(indexAtPos)) {
            activeView->selectionModel()->select(indexAtPos, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        
        auto allSelected = activeView->selectionModel()->selectedIndexes();
        for (const auto& idx : allSelected) {
            if (idx.column() == 0) selectedRows.append(idx);
        }
    } else {
        // 2. 点击在空白处：清空用于构建菜单的局部索引列表，确保下方 !selectedRows.isEmpty() 判定失败
        // 注意：这里不清除 view 的真实 selectionModel，仅让菜单表现为“无目标”状态
        selectedRows.clear();
    }
    
    QMenu menu(this);
    UiHelper::applyMenuStyle(&menu);

    if (!selectedRows.isEmpty()) {
        int count = selectedRows.size();
        menu.addAction(count > 1 ? "批量打开文件" : "打开文件", [this, selectedRows]() {
            for (const auto& index : selectedRows) onItemDoubleClicked(index);
        });
        
        menu.addAction("在“资源管理器”中显示", [this, selectedRows]() {
            QString path = m_tableModel->data(m_tableModel->index(selectedRows.first().row(), 1)).toString();
            QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(path)});
        });
        
        menu.addSeparator();
        
        menu.addAction(count > 1 ? "批量复制路径" : "复制路径", [this, selectedRows]() {
            QStringList paths;
            for (const auto& idx : selectedRows) paths << m_tableModel->data(m_tableModel->index(idx.row(), 1)).toString();
            QApplication::clipboard()->setText(paths.join("\n"));
        });
        
        menu.addAction(count > 1 ? "批量复制文件名" : "复制文件名", [this, selectedRows]() {
            QStringList names;
            for (const auto& idx : selectedRows) names << m_tableModel->data(m_tableModel->index(idx.row(), 0)).toString();
            QApplication::clipboard()->setText(names.join("\n"));
        });
        
        if (count == 1) {
            menu.addAction("重命名", this, &ScanDialog::onRenameTriggered);
        }
        
        menu.addSeparator();
        
        menu.addAction(count > 1 ? "批量删除" : "删除", [this, selectedRows]() {
            QString msg = (selectedRows.size() == 1) ? QString("确定要永久删除 %1 吗？").arg(m_tableModel->data(m_tableModel->index(selectedRows.first().row(), 0)).toString())
                                                   : QString("确定要永久删除选中的 %1 个项目吗？").arg(selectedRows.size());
            if (QMessageBox::question(this, "确认删除", msg) == QMessageBox::Yes) {
                for (const auto& idx : selectedRows) {
                    QString path = m_tableModel->data(m_tableModel->index(idx.row(), 1)).toString();
                    QFile::remove(path);
                }
                m_controller->triggerSearch(true);
            }
        });
        
        menu.addSeparator();
        menu.addSeparator();
        
        // --- 2026-05-16 深度管理：评分、标记、备注、标签 ---
        if (count == 1) {
            std::wstring path = m_tableModel->data(m_tableModel->index(selectedRows.first().row(), 1)).toString().toStdWString();
            auto meta = MetadataManager::instance().getMeta(path);

            QMenu* ratingMenu = menu.addMenu("评分");
            UiHelper::applyMenuStyle(ratingMenu);
            for (int i = 0; i <= 5; ++i) {
                QString star = (i == 0) ? "无评分" : QString(i, QChar(0x2605));
                QAction* act = ratingMenu->addAction(star, [this, path, i]() {
                    MetadataManager::instance().setRating(path, i);
                    m_controller->triggerSearch(true);
                });
                if (meta.rating == i) act->setCheckable(true), act->setChecked(true);
            }

            QMenu* labelMenu = menu.addMenu("标记颜色");
            UiHelper::applyMenuStyle(labelMenu);
            
            // --- 2026-05-16 图像分析：从图中提取主色调 ---
            QString ext = QFileInfo(QString::fromStdWString(path)).suffix().toLower();
            if (UiHelper::isGraphicsFile(ext)) {
                labelMenu->addAction("解析颜色...", [this, path]() {
                    // 开启异步分析链，防止 UI 阻塞
                    QPointer<ScanDialog> weakThis(this);
                    (void)QtConcurrent::run([weakThis, path]() {
                        auto palette = UiHelper::extractPalette(QString::fromStdWString(path));
                        if (palette.isEmpty()) return;
                        
                        QColor dominant = UiHelper::quantizeColor(palette.first().first);
                        QMetaObject::invokeMethod(weakThis.data(), [weakThis, path, dominant, palette]() {
                            if (weakThis) {
                                // 2026-06-xx 物理同步：强制执行 4-bit 量化
                                MetadataManager::instance().setColor(path, dominant.name().toUpper().toStdWString());
                                MetadataManager::instance().setPalettes(path, palette);
                                weakThis->m_controller->triggerSearch(true);
                            }
                        });
                    });
                });
                labelMenu->addSeparator();
            }

            // 2026-05-17 按照用户要求：重构标记颜色列表，彻底与主界面色彩及存储大一统，使用高雅配色并生成预览图标
            struct ColorItem { QString value; QString label; QColor preview; };
            QList<ColorItem> colorItems = {
                {"", "默认", QColor("#888780")},
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
                QAction* act = labelMenu->addAction(ci.label);
                connect(act, &QAction::triggered, this, [this, path, value = ci.value]() {
                    MetadataManager::instance().setColor(path, value.toStdWString());
                    m_controller->triggerSearch(true);
                });
                if (meta.color == ci.value.toStdWString()) {
                    act->setCheckable(true);
                    act->setChecked(true);
                }
                QPixmap pix(12, 12); pix.fill(Qt::transparent);
                QPainter p(&pix); p.setRenderHint(QPainter::Antialiasing);
                p.setBrush(ci.preview); p.setPen(Qt::NoPen);
                p.drawEllipse(0, 0, 12, 12);
                act->setIcon(QIcon(pix));
            }

            menu.addAction(meta.pinned ? "取消置顶" : "置顶文件", [this, path, meta]() {
                MetadataManager::instance().setPinned(path, !meta.pinned);
                m_controller->triggerSearch(true);
            });

            menu.addAction("编辑标签...", [this, path, meta]() {
                bool ok;
                QString text = QInputDialog::getText(this, "编辑标签", "标签 (逗号分隔):", QLineEdit::Normal, meta.tags.join(","), &ok);
                if (ok) {
                    MetadataManager::instance().setTags(path, text.split(",", Qt::SkipEmptyParts));
                    m_controller->triggerSearch(true);
                }
            });

            menu.addAction("编辑备注...", [this, path, meta]() {
                bool ok;
                QString text = QInputDialog::getMultiLineText(this, "编辑备注", "备注内容:", QString::fromStdWString(meta.note), &ok);
                if (ok) {
                    MetadataManager::instance().setNote(path, text.toStdWString());
                    m_controller->triggerSearch(true);
                }
            });

            menu.addAction(meta.encrypted ? "解密文件" : "加密文件", [this, path, meta]() {
                MetadataManager::instance().setEncrypted(path, !meta.encrypted);
                m_controller->triggerSearch(true);
            });

            menu.addSeparator();
        }
        
        menu.addAction("属性", [this, selectedRows]() {
            QString path = m_tableModel->data(m_tableModel->index(selectedRows.first().row(), 1)).toString();
            std::wstring wpath = path.toStdWString();
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_INVOKEIDLIST;
            sei.lpVerb = L"properties";
            sei.lpFile = wpath.c_str();
            sei.nShow = SW_SHOW;
            ShellExecuteExW(&sei);
        });

        menu.addSeparator();
    }

    // --- 2026-05-16 新增：视图、排序、刷新全局功能菜单 ---
    
    QMenu* viewMenu = menu.addMenu("视图(V)");
    UiHelper::applyMenuStyle(viewMenu);
    QActionGroup* viewGroup = new QActionGroup(this);
    
    auto addViewAction = [this, viewMenu, viewGroup](const QString& text, const QString& shortcut, int stackIdx, int iconSize) {
        QAction* act = viewMenu->addAction(text);
        act->setShortcut(QKeySequence(shortcut));
        act->setCheckable(true);
        viewGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, stackIdx, iconSize]() {
            m_viewStack->setCurrentIndex(stackIdx);
            m_config.viewMode = stackIdx;
            if (stackIdx == 1) { // 图标模式
                m_iconView->setTargetRowHeight(iconSize);
                m_config.iconSize = iconSize;
            }
            if (stackIdx == 0) { // 详情模式
                m_resultView->verticalHeader()->setDefaultSectionSize(32); // 详情模式固定为标准高度
            } else {
                m_resultView->verticalHeader()->setDefaultSectionSize(iconSize + 10);
            }
            m_config.save();
        });
        return act;
    };

    QAction* xLargeAction = addViewAction("超大图标(X)", "Ctrl+Shift+1", 1, 192);
    QAction* largeAction = addViewAction("大图标(L)", "Ctrl+Shift+2", 1, 128);
    QAction* mediumAction = addViewAction("中图标(M)", "Ctrl+Shift+3", 1, 64);
    
    viewMenu->addSeparator();
    
    QAction* detailsAction = addViewAction("详情(D)", "Ctrl+Shift+6", 0, 0);
    
    // 同步当前视图状态
    if (m_viewStack->currentIndex() == 0) detailsAction->setChecked(true);
    else {
        int currentSize = m_config.iconSize;
        if (currentSize == 192) xLargeAction->setChecked(true);
        else if (currentSize == 128) largeAction->setChecked(true);
        else mediumAction->setChecked(true);
    }
    
    QMenu* sortMenu = menu.addMenu("排序(S)");
    UiHelper::applyMenuStyle(sortMenu);
    QStringList sortOptions = {"名称", "路径", "大小", "修改日期"};
    for (int i = 0; i < sortOptions.size(); ++i) {
        QAction* act = sortMenu->addAction(sortOptions[i]);
        connect(act, &QAction::triggered, this, [this, i]() {
            Qt::SortOrder order = m_resultView->horizontalHeader()->sortIndicatorOrder();
            m_resultView->sortByColumn(i, order);
            m_config.sortColumn = i;
            m_config.sortOrder = static_cast<int>(order);
            m_config.save();
        });
    }
    sortMenu->addSeparator();
    QAction* ascAction = sortMenu->addAction("升序(A)");
    QAction* descAction = sortMenu->addAction("降序(D)");
    connect(ascAction, &QAction::triggered, this, [this]() { 
        m_resultView->sortByColumn(m_resultView->horizontalHeader()->sortIndicatorSection(), Qt::AscendingOrder); 
        m_config.sortOrder = 0;
        m_config.save();
    });
    connect(descAction, &QAction::triggered, this, [this]() { 
        m_resultView->sortByColumn(m_resultView->horizontalHeader()->sortIndicatorSection(), Qt::DescendingOrder); 
        m_config.sortOrder = 1;
        m_config.save();
    });

    QAction* refreshAction = menu.addAction("刷新(R)");
    refreshAction->setShortcut(QKeySequence(Qt::Key_F5));
    connect(refreshAction, &QAction::triggered, this, &ScanDialog::onTriggerSearch);

    QAbstractItemView* view = qobject_cast<QAbstractItemView*>(sender());
    if (view) menu.exec(view->viewport()->mapToGlobal(pos));
}

void ScanDialog::onItemDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    
    QString path = m_tableModel->data(m_tableModel->index(index.row(), 1)).toString();
    ShellExecuteW(NULL, L"open", reinterpret_cast<const wchar_t*>(path.utf16()), NULL, NULL, SW_SHOWNORMAL);
}

void ScanDialog::onSelectionChanged() {
    updateStatusBar();
}

void ScanDialog::onStartScan() {
    QStringList selectedDrives;
    for (const auto& d : m_config.activeDrives) selectedDrives << (d + QLatin1String("\\"));
    if (selectedDrives.isEmpty()) { onTriggerSearch(); return; }
    updateStatus("正在扫描...", true);

    QPointer<ScanDialog> weakThis(this);
    (void)(QtConcurrent::run)([weakThis, selectedDrives]() {
        MftReader::instance().buildIndex(selectedDrives);
        QMetaObject::invokeMethod(weakThis.data(), [weakThis]() {
            if (!weakThis) return;
            weakThis->updateStatus("就绪");
            weakThis->onTriggerSearch();
        });
    });
}

void ScanDialog::onTriggerSearch() {
    QString q = m_searchEdit->text().trimmed();
    QString e = m_extEdit->text().trimmed();
    
    QTimer::singleShot(10, this, [this, q, e]() {
        bool changed = false;
        if (!q.isEmpty() && (m_config.queryHistory.isEmpty() || m_config.queryHistory.first() != q)) {
            m_config.queryHistory.removeAll(q);
            m_config.queryHistory.prepend(q);
            if (m_config.queryHistory.size() > 10) m_config.queryHistory.removeLast();
            changed = true;
        }
        if (!e.isEmpty() && (m_config.extHistory.isEmpty() || m_config.extHistory.first() != e)) {
            m_config.extHistory.removeAll(e);
            m_config.extHistory.prepend(e);
            if (m_config.extHistory.size() > 10) m_config.extHistory.removeLast();
            changed = true;
        }
        if (changed) m_config.save();
    });

    QStringList activeList;
    for (const QString& drive : m_config.activeDrives) activeList << drive;
    MftReader::instance().updateActiveDrives(activeList);

    onFilterOptionChanged();
    m_controller->setSearchText(m_searchEdit->text());
    m_controller->triggerSearch(true); // 按钮点击立即搜索
}

void ScanDialog::onFilterOptionChanged() {
    // 2026-06-xx 物理修复：在过滤选项变更时强制同步驱动器掩码。
    // 如果不在此处同步，当用户切换“自动显示”开关时，搜索引擎可能因为默认 Mask 为 0 而导致搜索结果为空。
    QStringList activeList;
    for (const QString& d : m_config.activeDrives) activeList << d;
    MftReader::instance().updateActiveDrives(activeList);

    m_config.useRegex = m_checkRegex->isChecked();
    m_config.caseSensitive = m_checkCase->isChecked();
    m_config.includeHidden = m_checkHidden->isChecked();
    m_config.includeSystem = m_checkSystem->isChecked();
    m_config.includeDollar = m_checkDollar->isChecked();
    m_config.autoDisplay = m_checkAuto->isChecked();
    m_config.save();

    ScanFilterState state;
    state.useRegex = m_config.useRegex;
    state.caseSensitive = m_config.caseSensitive;
    state.includeHidden = m_config.includeHidden;
    state.includeSystem = m_config.includeSystem;
    state.includeDollar = m_config.includeDollar;
    state.autoDisplay = m_config.autoDisplay;
    QString extText = m_extEdit->text().toLower();
    if (!extText.isEmpty()) state.extensionList = extText.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
    
    m_controller->setFilterState(state);
    // 2026-06-xx 物理对标：配置变更时触发立即搜索，以响应“自动显示”等开关状态
    m_controller->triggerSearch(true);
}

void ScanDialog::updateStatus(const QString& text, bool scanning, int64_t totalCount) {
    Q_UNUSED(text);
    if (m_titleStatusLabel) {
        int64_t total = (totalCount >= 0) ? totalCount : MftReader::instance().totalCount();
        m_titleStatusLabel->setText(QString("%1 - %2").arg(scanning ? "SCANNING" : "READY").arg(formatNumber(total)));
        m_titleStatusLabel->setStyleSheet(scanning ? "color: #FF8C00; font-size: 10px; font-weight: bold;" : "color: #46B478; font-size: 10px; font-weight: bold;");
    }
    
    if (scanning) { m_progressBar->show(); m_progressBar->setRange(0, 0); }
    else { m_progressBar->hide(); updateStatusBar(); }
}

void ScanDialog::updateStatusBar() {
    auto view = (m_viewStack->currentIndex() == 0) ? static_cast<QAbstractItemView*>(m_resultView) : static_cast<QAbstractItemView*>(m_iconView);
    auto selectedRows = view->selectionModel()->selectedRows();
    
    int totalMatch = m_controller->resultCount();
    m_statLabelMain->setText(QString("共找到 %1 条项目").arg(formatNumber(totalMatch)));
    m_statLabelTime->setText(QString("耗时 %1 ms").arg(m_lastSearchMs));

    if (!selectedRows.isEmpty()) {
        m_selectionLabel->show();
        int64_t totalSize = 0;
        auto& reader = MftReader::instance();
        for (const auto& index : selectedRows) {
            uint64_t key = m_tableModel->data(index, Qt::UserRole).toULongLong();
            int actualIdx = reader.getIndexByKey(key);
            if (actualIdx != -1 && !reader.isDirectory(actualIdx)) totalSize += reader.getSize(actualIdx);
        }
        m_selectionLabel->setText(QString(" | 已选择 %1 项 (%2)").arg(selectedRows.size()).arg(formatSize(totalSize)));
        
        if (selectedRows.size() > 1) m_csvBtn->show();
        else m_csvBtn->hide();
    } else {
        m_selectionLabel->hide();
        m_csvBtn->hide();
    }
    
    double memoryMb = (MftReader::instance().totalCount() * 184.0) / 1024.0 / 1024.0;
    m_statLabelMemory->setText(QString("数据占用 %1 MB").arg(memoryMb, 0, 'f', 1));
}

QString ScanDialog::formatNumber(int64_t n) {
    return QLocale(QLocale::English).toString(n);
}

QString ScanDialog::formatSize(int64_t bytes) {
    if (bytes == 0) return "0 B";
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < units.size() - 1) {
        size /= 1024.0;
        unit++;
    }
    return QString("%1 %2").arg(size, 0, 'f', 2).arg(units[unit]);
}

void ScanDialog::onRenameTriggered() {
    auto view = (m_viewStack->currentIndex() == 0) ? static_cast<QAbstractItemView*>(m_resultView) : static_cast<QAbstractItemView*>(m_iconView);
    auto selection = view->selectionModel()->selectedRows();
    if (selection.isEmpty()) return;
    
    // 2026-05-16 交互进化：触发行内编辑
    view->edit(selection.first());
}

void ScanDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F2) {
        onRenameTriggered();
        return;
    }
    if (event->key() == Qt::Key_F5) {
        onTriggerSearch();
        return;
    }
    if (event->key() == Qt::Key_A && event->modifiers() == Qt::ControlModifier) { 
        auto view = (m_viewStack->currentIndex() == 0) ? static_cast<QAbstractItemView*>(m_resultView) : static_cast<QAbstractItemView*>(m_iconView);
        view->selectAll(); 
        return; 
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_searchEdit->hasFocus() || m_extEdit->hasFocus()) {
            onTriggerSearch();
        } else {
            auto view = (m_viewStack->currentIndex() == 0) ? static_cast<QAbstractItemView*>(m_resultView) : static_cast<QAbstractItemView*>(m_iconView);
            auto index = view->currentIndex();
            if (index.isValid()) onItemDoubleClicked(index);
        }
        return;
    }
    handleMetadataShortcut(event);
    FramelessDialog::keyPressEvent(event);
}

// 2026-05-16 快捷键核心处理逻辑：支持评分、置顶、标签等深度管理快捷键
void ScanDialog::handleMetadataShortcut(QKeyEvent* event) {
    auto view = (m_viewStack->currentIndex() == 0) ? static_cast<QAbstractItemView*>(m_resultView) : static_cast<QAbstractItemView*>(m_iconView);
    auto selection = view->selectionModel()->selectedRows();
    if (selection.isEmpty()) return;
    
    std::wstring path = m_tableModel->data(m_tableModel->index(selection.first().row(), 1)).toString().toStdWString();
    auto meta = MetadataManager::instance().getMeta(path);

    // Alt + 1-8: 颜色标记
    if (event->modifiers() == Qt::AltModifier && event->key() >= Qt::Key_1 && event->key() <= Qt::Key_8) {
        QString colorValue;
        switch (event->key()) {
            case Qt::Key_1: colorValue = "#E24B4A"; break; // red
            case Qt::Key_2: colorValue = "#EF9F27"; break; // orange
            case Qt::Key_3: colorValue = "#FECF0E"; break; // yellow
            case Qt::Key_4: colorValue = "#639922"; break; // green
            case Qt::Key_5: colorValue = "#1D9E75"; break; // cyan
            case Qt::Key_6: colorValue = "#378ADD"; break; // blue
            case Qt::Key_7: colorValue = "#7F77DD"; break; // purple
            case Qt::Key_8: colorValue = "#5F5E5A"; break; // gray
        }
        MetadataManager::instance().setColor(path, colorValue.toStdWString());
        m_controller->triggerSearch(true);
        return;
    }

    // Ctrl + 0-5: 评分
    if (event->modifiers() == Qt::ControlModifier && event->key() >= Qt::Key_0 && event->key() <= Qt::Key_5) {
        int rating = event->key() - Qt::Key_0;
        MetadataManager::instance().setRating(path, rating);
        m_controller->triggerSearch(true);
        return;
    }

    // Alt 快捷键
    if (event->modifiers() == Qt::AltModifier) {
        if (event->key() == Qt::Key_P) { // 置顶
            MetadataManager::instance().setPinned(path, !meta.pinned);
            m_controller->triggerSearch(true);
        } else if (event->key() == Qt::Key_L) { // 加密
            MetadataManager::instance().setEncrypted(path, !meta.encrypted);
            m_controller->triggerSearch(true);
        } else if (event->key() == Qt::Key_T) { // 标签
            bool ok;
            QString text = QInputDialog::getText(this, "编辑标签", "标签 (逗号分隔):", QLineEdit::Normal, meta.tags.join(","), &ok);
            if (ok) {
                MetadataManager::instance().setTags(path, text.split(",", Qt::SkipEmptyParts));
                m_controller->triggerSearch(true);
            }
        } else if (event->key() == Qt::Key_N) { // 备注
            bool ok;
            QString text = QInputDialog::getMultiLineText(this, "编辑备注", "备注内容:", QString::fromStdWString(meta.note), &ok);
            if (ok) {
                MetadataManager::instance().setNote(path, text.toStdWString());
                m_controller->triggerSearch(true);
            }
        }
    }
}

bool ScanDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_sizeSlider && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            int val = QStyle::sliderValueFromPosition(m_sizeSlider->minimum(), m_sizeSlider->maximum(), me->pos().x(), m_sizeSlider->width());
            m_sizeSlider->setValue(val);
            return true;
        }
    }
    if ((watched == m_searchEdit || watched == m_extEdit) && event->type() == QEvent::MouseButtonDblClick) {
        bool isQuery = (watched == m_searchEdit);
        const QStringList& history = isQuery ? m_config.queryHistory : m_config.extHistory;
        
        if (!history.isEmpty()) {
            QMenu menu(this);
            UiHelper::applyMenuStyle(&menu);
            
            for (const QString& item : history) {
                menu.addAction(item, [this, isQuery, item]() {
                    if (isQuery) m_searchEdit->setText(item);
                    else m_extEdit->setText(item);
                    onTriggerSearch();
                });
            }
            
            menu.addSeparator();
            menu.addAction("清空历史记录", [this, isQuery]() {
                if (isQuery) m_config.queryHistory.clear();
                else m_config.extHistory.clear();
                m_config.save();
            });
            
            menu.exec(static_cast<QWidget*>(watched)->mapToGlobal(QPoint(0, static_cast<QWidget*>(watched)->height())));
            return true;
        }
    }
    return FramelessDialog::eventFilter(watched, event);
}

} // namespace ArcMeta
