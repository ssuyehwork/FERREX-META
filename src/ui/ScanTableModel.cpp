#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ScanTableModel.h"
#include "ScanDialog.h"
#include "ScanController.h"
#include "IScanResultView.h"
#include "UiHelper.h"
#include "ThumbnailManager.h"
#include "../util/ShellHelper.h"
#include "../mft/MftReader.h"
#include "../meta/MetadataManager.h"

#include <QMessageBox>
#include <QDateTime>
#include <QPointer>
#include <QReadLocker>
#include <QWriteLocker>
#include <QtConcurrent/QtConcurrent>
#include <QUrl>
#include <QThreadStorage>
#include <QSvgRenderer>
#include <QScrollBar>
#include <QFileInfo>
#include <QPainter>
#include <QHeaderView>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef run
#undef run
#endif

namespace FERREX {

// --- ScanTableModel Implementation ---

ScanTableModel::ScanTableModel(ScanController* controller, QObject* parent) 
    : QAbstractTableModel(parent), m_controller(controller) 
{
    m_currentResultSet = std::make_shared<ResultSet>();

    m_throttleTimer = new QTimer(this);
    m_throttleTimer->setInterval(100); 

    m_metadataTimer = new QTimer(this);
    m_metadataTimer->setInterval(150); // 150ms 视口防抖
    m_metadataTimer->setSingleShot(true);
    connect(m_metadataTimer, &QTimer::timeout, this, [this]() {
        if (m_visibleTop < 0 || m_visibleBottom < 0) return;
        
        auto snap = m_currentResultSet;
        int top = m_visibleTop;
        int bottom = m_visibleBottom;

        (void)QtConcurrent::run([snap, top, bottom]() {
            auto& reader = MftReader::instance();
            for (int i = top; i <= bottom; ++i) {
                if (i >= (int)snap->keys.size()) break;
                uint64_t key = snap->keys[i];
                int idx = reader.getIndexByKey(key);
                if (idx != -1 && !reader.isMetadataFetched(idx)) {
                    const_cast<MftReader&>(reader).requestMetadata(idx);
                }
            }
        });
    });

    connect(&ThumbnailManager::instance(), &ThumbnailManager::thumbnailReady, this, [this](uint64_t key, const QPixmap& /*pixmap*/, double aspectRatio) {
        if (m_isDestroying) return;
        m_aspectRatios[key] = aspectRatio;

        auto snapshot = m_controller->snapshot();
        auto itPos = snapshot->keyToPos.find(key);
        if (itPos != snapshot->keyToPos.end() && itPos->second < m_displayCount) {
            m_pendingRows.insert(itPos->second);
            if (!m_throttleTimer->isActive()) m_throttleTimer->start();
        }
    });

    connect(&ThumbnailManager::instance(), &ThumbnailManager::thumbnailFailed, this, [this](uint64_t key) {
        if (m_isDestroying) return;
        auto snapshot = m_controller->snapshot();
        auto itPos = snapshot->keyToPos.find(key);
        if (itPos != snapshot->keyToPos.end() && itPos->second < m_displayCount) {
            m_pendingRows.insert(itPos->second);
            if (!m_throttleTimer->isActive()) m_throttleTimer->start();
        }
    });

    connect(m_throttleTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingRows.isEmpty()) return;
        QList<int> rows = m_pendingRows.values();
        std::sort(rows.begin(), rows.end());
        m_pendingRows.clear();

        for (int i = 0; i < rows.size(); ) {
            int startRow = rows[i];
            int endRow = rows[i];
            int j = i + 1;
            while (j < rows.size() && rows[j] == endRow + 1) {
                endRow = rows[j];
                j++;
            }
            
            if (startRow >= 0 && endRow < m_displayCount) {
                emit dataChanged(index(startRow, 0), index(endRow, 3));
            }
            i = j;
        }
    });

    // 2026-06-xx 架构重构：切换至 Controller 驱动的原子快照更新 (使用信号携带的快照，绝对安全)
    connect(m_controller, &ScanController::resultsSwapped, this, [this](std::shared_ptr<ResultSet> newSet) {
        updateResults(newSet);
    });
}
ScanTableModel::~ScanTableModel() {
    m_isDestroying = true;
    if (m_throttleTimer) { m_throttleTimer->stop(); delete m_throttleTimer; m_throttleTimer = nullptr; }
    if (m_metadataTimer) { m_metadataTimer->stop(); delete m_metadataTimer; m_metadataTimer = nullptr; }
}

int ScanTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_displayCount;
}

int ScanTableModel::columnCount(const QModelIndex& /*parent*/) const { return 4; }

SoAProjRecord ScanTableModel::getSoARecord(int row) const {
    if (row < 0 || row >= (int)m_currentResultSet->keys.size()) return SoAProjRecord();
    uint64_t key = m_currentResultSet->keys[row];

    {
        std::shared_lock<std::shared_mutex> cacheLock(m_currentResultSet->cacheMutex);
        auto it = m_currentResultSet->soaCache.find(key);
        if (it != m_currentResultSet->soaCache.end()) {
            return it->second;
        }
    }

    // 缓存未命中，利用读锁极速获取并塞入局部滑动缓存
    auto& reader = MftReader::instance();
    int actualIndex = reader.getIndexByKey(key);
    if (actualIndex == -1) return SoAProjRecord();

    SoAProjRecord rec;
    rec.name = reader.getName(actualIndex);
    rec.fullPath = reader.getFullPath(actualIndex);
    rec.size = reader.getSize(actualIndex);
    rec.mtime = reader.getModifyTime(actualIndex);
    rec.isDirectory = reader.isDirectory(actualIndex);
    rec.ext = reader.getExtQString(actualIndex);

    {
        std::unique_lock<std::shared_mutex> cacheLock(m_currentResultSet->cacheMutex);
        m_currentResultSet->soaCache[key] = rec;
    }
    return rec;
}

void ScanTableModel::cleanExitedSoACache() const {
    if (m_visibleTop < 0 || m_visibleBottom < 0) return;
    int rangeStart = std::max(0, m_visibleTop - 500);
    int rangeEnd = std::min((int)m_currentResultSet->keys.size() - 1, m_visibleBottom + 500);

    std::unordered_set<uint64_t> keepKeys;
    for (int i = rangeStart; i <= rangeEnd; ++i) {
        keepKeys.insert(m_currentResultSet->keys[i]);
    }

    std::unique_lock<std::shared_mutex> cacheLock(m_currentResultSet->cacheMutex);
    auto it = m_currentResultSet->soaCache.begin();
    while (it != m_currentResultSet->soaCache.end()) {
        if (keepKeys.find(it->first) == keepKeys.end()) {
            it = m_currentResultSet->soaCache.erase(it);
        } else {
            ++it;
        }
    }
}

QVariant ScanTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return QVariant();
    int row = index.row();
    if (row < 0 || row >= (int)m_currentResultSet->keys.size()) return QVariant();
    
    // 主线程零锁直接获取局部滑动视口缓存，绝对性能超越全量装填，且没有读写锁的排队卡死问题！
    SoAProjRecord rec = getSoARecord(row);
    if (rec.name.isEmpty() && rec.fullPath.isEmpty()) return QVariant();

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case 0: return rec.name;
            case 1: return rec.fullPath;
            case 2: {
                if (rec.isDirectory) return "-";
                int64_t size = rec.size;
                if (size == 0) return "...";
                if (size < 1024) return QString("%1 B").arg(size);
                if (size < 1024 * 1024) return QString("%1 KB").arg(size / 1024.0, 0, 'f', 2);
                if (size < 1024LL * 1024 * 1024) return QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
                return QString("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
            }
            case 3: {
                int64_t ts = rec.mtime;
                if (ts == 0) return "-";
                return QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
            }
        }
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        if (thumbExts.contains(rec.ext) && !rec.isDirectory) {
            uint64_t key = m_currentResultSet->keys[row];
            ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
            int thumbSize = dlg ? dlg->m_config.iconSize : 64;

            QPixmap pix = ThumbnailManager::instance().requestThumbnail(key, rec.fullPath, rec.ext, thumbSize, rec.size, rec.mtime);
            if (!pix.isNull()) {
                return pix;
            }
            return QVariant(); // 动态阻断默认图标闪烁
        }
        
        return MftReader::instance().getCachedIcon(rec.ext, rec.isDirectory);
    } else if (role == Qt::ForegroundRole) {
        uint64_t key = m_currentResultSet->keys[row];
        auto it = m_currentResultSet->metadata.find(key);
        if (it != m_currentResultSet->metadata.end()) {
            return it->second.color;
        }

        auto meta = MetadataManager::instance().getMeta(rec.fullPath.toStdWString());
        if (!meta.color.empty()) {
            QColor tagC = UiHelper::parseColorName(QString::fromStdWString(meta.color));
            if (tagC.isValid()) return tagC;
        }
        if (index.column() == 0 || rec.isDirectory) return QColor("#3498db");
    } else if (role == Qt::ToolTipRole) {
        QString sizeStr;
        if (rec.isDirectory) {
            sizeStr = "-";
        } else {
            int64_t size = rec.size;
            if (size == 0) {
                sizeStr = "...";
            } else if (size < 1024) {
                sizeStr = QString("%1 B").arg(size);
            } else if (size < 1024 * 1024) {
                sizeStr = QString("%1 KB").arg(size / 1024.0, 0, 'f', 2);
            } else if (size < 1024LL * 1024 * 1024) {
                sizeStr = QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
            } else {
                sizeStr = QString("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
            }
        }

        QString mtimeStr;
        int64_t ts = rec.mtime;
        if (ts == 0) {
            mtimeStr = "-";
        } else {
            mtimeStr = QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
        }

        QString tip = QString::fromUtf8("名称: ") + rec.name + "\n" +
                      QString::fromUtf8("路径: ") + rec.fullPath + "\n" +
                      QString::fromUtf8("大小: ") + sizeStr + "\n" +
                      QString::fromUtf8("修改时间: ") + mtimeStr;

        return tip;
    } else if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
            case 0: case 1: return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
            case 2: case 3: return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
    } else if (role == Qt::UserRole) {
        return m_currentResultSet->keys[row];
    } else if (role == Qt::UserRole + 1) {
        return QVariant();
    } else if (role == Qt::UserRole + 2) {
        if (rec.isDirectory) {
            return -1.0;
        }

        static const QSet<QString> mediaExts = {
            "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "svg",
            "mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "rmvb"
        };

        if (!mediaExts.contains(rec.ext.toLower())) {
            return -1.0;
        }

        uint64_t key = m_currentResultSet->keys[row];
        return m_aspectRatios.value(key, 1.0);
    }
    return QVariant();
}

Qt::ItemFlags ScanTableModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() == 0) {
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

void ScanTableModel::updateResults(std::shared_ptr<ResultSet> nextSet) {
    auto baseSet = nextSet ? nextSet : m_controller->snapshot();
    auto newSet = std::make_shared<ResultSet>();
    newSet->metadata = baseSet->metadata;
    newSet->keyToPos = baseSet->keyToPos;

    newSet->keys = baseSet->keys;

    int oldSize = (int)m_currentResultSet->keys.size();
    int newSize = (int)newSet->keys.size();

    if (oldSize == 0 || std::abs(newSize - oldSize) > 500) {
        beginResetModel();
        m_currentResultSet = newSet;
        m_displayCount = newSize; 
        m_requestedThumbs.clear();
        m_pendingRows.clear();
        endResetModel();
        return;
    }

    if (newSize > oldSize) {
        beginInsertRows(QModelIndex(), oldSize, newSize - 1);
        m_currentResultSet = newSet;
        m_displayCount = newSize; 
        endInsertRows();
    } else if (newSize < oldSize) {
        beginRemoveRows(QModelIndex(), newSize, oldSize - 1);
        m_currentResultSet = newSet;
        m_displayCount = newSize;
        endRemoveRows();
    } else if (newSize > 0) {
        m_currentResultSet = newSet;
        emit dataChanged(index(0, 0), index(newSize - 1, 3));
    } else {
        m_currentResultSet = newSet;
    }
}

bool ScanTableModel::canFetchMore(const QModelIndex& parent) const {
    Q_UNUSED(parent);
    return false;
}

void ScanTableModel::fetchMore(const QModelIndex& parent) {
    Q_UNUSED(parent);
}

void ScanTableModel::setVisibleRange(int top, int bottom) {
    m_visibleTop = top;
    m_visibleBottom = bottom;
    m_metadataTimer->start();
    cleanExitedSoACache(); // 滚动时动态释放移出视口的局部 SoA 内存，极度节省堆开销
}

void ScanTableModel::forceFetchAll() {
    int total = (int)m_currentResultSet->keys.size();
    if (m_displayCount >= total) return;
    
    beginInsertRows(QModelIndex(), m_displayCount, total - 1);
    m_displayCount = total;
    endInsertRows();
}

void ScanTableModel::clearThumbCache(bool keepLastCache) {
    Q_UNUSED(keepLastCache);
    ThumbnailManager::instance().clearCache();
    m_requestedThumbs.clear();
}

void ScanTableModel::sort(int column, Qt::SortOrder order) {
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

} // namespace FERREX
