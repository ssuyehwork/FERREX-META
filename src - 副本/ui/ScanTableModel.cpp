#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ScanTableModel.h"
#include "ScanDialog.h"
#include "ScanController.h"
#include "IScanResultView.h"
#include "UiHelper.h"
#include "../util/ShellHelper.h"
#include "../mft/MftReader.h"
#include "../meta/MetadataManager.h"

#include <QMessageBox>
#include <QDateTime>
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

ScanTableModel::ScanTableModel(ScanController* controller, QObject* parent) 
    : QAbstractTableModel(parent), m_controller(controller) 
{
    m_currentResultSet = std::make_shared<ResultSet>();

    m_thumbPool = new QThreadPool(this);

    
    bool allSSD = true;
    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent);
    if (dlg) {
        
        for (const QString& d : dlg->m_config.activeDrives) {
            if (!ShellHelper::isSolidStateDrive(d)) {
                allSSD = false;
                break;
            }
        }
    } else {
        allSSD = false; 
    }

    if (allSSD) {
        
        m_thumbPool->setMaxThreadCount(std::max<int>(1, QThread::idealThreadCount() / 2));
    } else {
        
        m_thumbPool->setMaxThreadCount(1);
    }

    m_thumbCache.setMaxCost(500); 
    m_throttleTimer = new QTimer(this);
    m_throttleTimer->setInterval(100); 

    m_metadataTimer = new QTimer(this);
    m_metadataTimer->setInterval(150); 
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

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setInterval(20); 
    m_thumbTimer->setSingleShot(true);
    connect(m_thumbTimer, &QTimer::timeout, this, &ScanTableModel::processThumbQueue);

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

    connect(m_controller, &ScanController::resultsSwapped, this, [this](std::shared_ptr<ResultSet> newSet) {
        updateResults(newSet);
    });
}
ScanTableModel::~ScanTableModel() {
    if (m_thumbPool) {
        m_thumbPool->waitForDone();
    }
}

int ScanTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_displayCount;
}

int ScanTableModel::columnCount(const QModelIndex& ) const { return 4; }

QVariant ScanTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return QVariant();
    int row = index.row();
    if (row < 0 || row >= (int)m_currentResultSet->keys.size()) return QVariant();
    
    uint64_t key = m_currentResultSet->keys[row];
    auto& reader = MftReader::instance();
    int actualIndex = reader.getIndexByKey(key);
    if (actualIndex == -1) return QVariant(); 

    
    
    thread_local static int lastRow = -1;
    thread_local static uint64_t lastKey = 0;
    thread_local static QString cachedPath;
    
    auto getPath = [&]() {
        if (lastRow == row && lastKey == key && !cachedPath.isEmpty()) return cachedPath;
        lastRow = row; lastKey = key;
        cachedPath = reader.getFullPath(actualIndex);
        return cachedPath;
    };
    
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case 0: return reader.getName(actualIndex);
            case 1: return getPath();
            case 2: {
                if (reader.isDirectory(actualIndex)) return "-";
                int64_t size = reader.getSize(actualIndex);
                if (size == 0 && !reader.isMetadataFetched(actualIndex)) {
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
                    return "-";
                }
                if (ts == 0) return "-";
                return QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
            }
        }
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        
        QString ext = reader.getExtQString(actualIndex);
        
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        if (thumbExts.contains(ext) && !reader.isDirectory(actualIndex)) {
            
            int64_t size = reader.getSize(actualIndex);
            int64_t mtime = reader.getModifyTime(actualIndex);
            QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(mtime);

            if (m_lastPixmapCache.maxCost() == 0) {
                m_lastPixmapCache.setMaxCost(200); 
            }

            QPixmap* cached = m_thumbCache.object(cacheKey);
            if (cached) return *cached;

            QPixmap* lastCached = m_lastPixmapCache.object(QString::number(key));
            if (lastCached) {
                
                if (!m_requestedThumbs.contains(key)) {
                    m_requestedThumbs.insert(key);
                    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                    int thumbSize = dlg ? dlg->m_config.iconSize : 64; 
                    m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                    if (!m_thumbTimer->isActive()) m_thumbTimer->start();
                }

                return *lastCached;
            }

            if (m_failedThumbs.contains(key)) {
                return reader.getCachedIcon(ext, false);
            }

            if (!m_requestedThumbs.contains(key)) {
                m_requestedThumbs.insert(key);
                ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                int thumbSize = dlg ? dlg->m_config.iconSize : 64; 
                
                m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                if (!m_thumbTimer->isActive()) m_thumbTimer->start();
            }

            return QVariant(); 
        }

        return reader.getCachedIcon(ext, reader.isDirectory(actualIndex));
    } else if (role == Qt::ForegroundRole) {
        
        auto it = m_currentResultSet->metadata.find(key);
        if (it != m_currentResultSet->metadata.end()) {
            return it->second.color;
        }

        QString qPath = getPath();
        auto meta = MetadataManager::instance().getMeta(qPath.toStdWString());
        if (!meta.color.empty()) {
            QColor tagC = UiHelper::parseColorName(QString::fromStdWString(meta.color));
            if (tagC.isValid()) return tagC;
        }
        
        if (index.column() == 0 || reader.isDirectory(actualIndex)) return QColor("#3498db");
    } else if (role == Qt::ToolTipRole) {

        QString name = reader.getName(actualIndex);
        QString qPath = getPath();
        
        QString sizeStr;
        if (reader.isDirectory(actualIndex)) {
            sizeStr = "-";
        } else {
            int64_t size = reader.getSize(actualIndex);
            if (size == 0 && !reader.isMetadataFetched(actualIndex)) {
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
        int64_t ts = reader.getModifyTime(actualIndex);
        if (ts == 0 && !reader.isMetadataFetched(actualIndex)) {
            mtimeStr = "-";
        } else if (ts == 0) {
            mtimeStr = "-";
        } else {
            mtimeStr = QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
        }

        QString tip = QString::fromUtf8("名称: ") + name + "\n" +
                      QString::fromUtf8("路径: ") + qPath + "\n" +
                      QString::fromUtf8("大小: ") + sizeStr + "\n" +
                      QString::fromUtf8("修改时间: ") + mtimeStr;

        return tip;
    } else if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
            case 0: case 1: return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
            case 2: case 3: return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
    } else if (role == Qt::UserRole) {
        return key;
    } else if (role == Qt::UserRole + 1) {

        QString ext = reader.getExtQString(actualIndex);
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        
        if (!thumbExts.contains(ext) || reader.isDirectory(actualIndex)) return 0;

        int64_t size = reader.getSize(actualIndex);
        int64_t mtime = reader.getModifyTime(actualIndex);
        QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(mtime);

        if (m_thumbCache.contains(cacheKey) || m_lastPixmapCache.contains(QString::number(key))) {
            return 1;
        }
        return 0;
    } else if (role == Qt::UserRole + 2) {

        if (reader.isDirectory(actualIndex)) {
            return -1.0;
        }

        QString ext = reader.getExtQString(actualIndex).toLower();
        static const QSet<QString> mediaExts = {
            
            "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "eps", "pdf", "svg",
            
            "mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "3gp", "ts", "rmvb", "rm", "vob"
        };

        if (!mediaExts.contains(ext)) {
            return -1.0; 
        }

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
        m_failedThumbs.clear(); 
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
}

void ScanTableModel::forceFetchAll() {
    int total = (int)m_currentResultSet->keys.size();
    if (m_displayCount >= total) return;
    
    beginInsertRows(QModelIndex(), m_displayCount, total - 1);
    m_displayCount = total;
    endInsertRows();
}

void ScanTableModel::processThumbQueue() {
    if (m_thumbTaskQueue.isEmpty()) return;

    
    auto currentTasks = std::move(m_thumbTaskQueue);
    std::reverse(currentTasks.begin(), currentTasks.end());

    for (const auto& t : currentTasks) {
        m_thumbPool->start([this, t]() {
            
            static QThreadStorage<ScopedComInit> comStorage;
            if (!comStorage.hasLocalData()) {
                comStorage.setLocalData(ScopedComInit());
            }

            auto& reader = MftReader::instance();
            int actualIdx = reader.getIndexByKey(t.key);
            if (actualIdx == -1) return;

            QString fullPath = reader.getFullPath(actualIdx);
            if (fullPath.isEmpty()) return;

            QImage img;
            if (t.ext == "svg") {
                QSvgRenderer renderer(fullPath);
                if (renderer.isValid()) {
                    img = QImage(QSize(t.size, t.size), QImage::Format_ARGB32);
                    img.fill(Qt::transparent);
                    QPainter painter(&img);
                    renderer.render(&painter);
                    painter.end();
                }
            } else {
                img = FERREX::UiHelper::getShellThumbnail(fullPath, t.size);
            }

            if (!img.isNull()) {
                double ar = (double)img.width() / (double)img.height();
                
                QMetaObject::invokeMethod(this, [this, key = t.key, cacheKey = t.cacheKey, img, ar]() {
                    QPixmap pix = QPixmap::fromImage(img);
                    if (!pix.isNull()) {
                        m_thumbCache.insert(cacheKey, new QPixmap(pix));
                        m_lastPixmapCache.insert(QString::number(key), new QPixmap(pix)); 
                    }
                    m_aspectRatios[key] = ar;

                    auto snapshot = m_controller->snapshot();
                    auto itPos = snapshot->keyToPos.find(key);
                    if (itPos != snapshot->keyToPos.end() && itPos->second < m_displayCount) {
                        m_pendingRows.insert(itPos->second);
                        if (!m_throttleTimer->isActive()) m_throttleTimer->start();
                    }
                }, Qt::QueuedConnection);
            } else {
                
                QMetaObject::invokeMethod(this, [this, key = t.key]() {
                    m_failedThumbs.insert(key);
                    auto snapshot = m_controller->snapshot();
                    auto itPos = snapshot->keyToPos.find(key);
                    if (itPos != snapshot->keyToPos.end() && itPos->second < m_displayCount) {
                        m_pendingRows.insert(itPos->second);
                        if (!m_throttleTimer->isActive()) m_throttleTimer->start();
                    }
                }, Qt::QueuedConnection);
            }
        });
    }
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

} 
