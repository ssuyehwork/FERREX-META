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

using namespace FERREX;
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

    // 建立隔离的缩略图任务专用线程池，避免与主后台任务竞争资源 (降级委托给全局单例托管，本成员在 TableModel 中置空)
    m_thumbPool = nullptr;

    m_thumbCache.setMaxCost(500); // 限制缩略图内存占用
    m_lastPixmapCache.setMaxCost(200); // 消除 data() 中的拦截，统一在此完成初始化分配
    m_throttleTimer = new QTimer(this);
    m_throttleTimer->setInterval(100); 

    m_metadataTimer = new QTimer(this);
    m_metadataTimer->setInterval(150); // 150ms 视口防抖
    m_metadataTimer->setSingleShot(true);
    connect(m_metadataTimer, &QTimer::timeout, this, [this]() {
        if (m_visibleTop < 0 || m_visibleBottom < 0) return;
        
        // 2026-06-xx 物理修复：视口扫描异步化。
        // 理由：虽然 requestMetadata 是异步的，但其内部会触发 MftReader 的读写锁申请。
        // 在 220万数据下，如果 UI 线程密集触发 lock 申请，会造成明显的微卡顿甚至假死。
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
    m_thumbTimer->setInterval(20); // 20ms 任务归并窗口
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
            
            // 2026-06-xx 物理加固：在发射 dataChanged 前强制核对行号边界，防止越界触发断言
            if (startRow >= 0 && endRow < m_displayCount) {
                emit dataChanged(index(startRow, 0), index(endRow, 3));
            }
            i = j;
        }
    });

    // 2026-06-xx 架构重构：切换至 Controller 驱动 of 原子快照更新 (使用信号携带的快照，绝对安全)
    connect(m_controller, &ScanController::resultsSwapped, this, [this](std::shared_ptr<ResultSet> newSet) {
        updateResults(newSet);
    });

    // 连接全局的 ThumbnailManager 信号，实现缩略图就绪时的局部视图自适应刷新
    QPointer<ScanTableModel> weakThis(this);
    connect(&ThumbnailManager::instance(), &ThumbnailManager::thumbnailReady, this, [weakThis](uint64_t key, const QImage&, double ar, const QString&) {
        if (!weakThis || weakThis->m_isDestroying) return;
        weakThis->m_aspectRatios[key] = ar;

        auto snapshot = weakThis->m_controller->snapshot();
        auto itPos = snapshot->keyToPos.find(key);
        if (itPos != snapshot->keyToPos.end() && itPos->second < weakThis->m_displayCount) {
            weakThis->m_pendingRows.insert(itPos->second);
            if (!weakThis->m_throttleTimer->isActive()) weakThis->m_throttleTimer->start();
        }
    });

    connect(&ThumbnailManager::instance(), &ThumbnailManager::thumbnailFailed, this, [weakThis](uint64_t key) {
        if (!weakThis || weakThis->m_isDestroying) return;

        auto snapshot = weakThis->m_controller->snapshot();
        auto itPos = snapshot->keyToPos.find(key);
        if (itPos != snapshot->keyToPos.end() && itPos->second < weakThis->m_displayCount) {
            weakThis->m_pendingRows.insert(itPos->second);
            if (!weakThis->m_throttleTimer->isActive()) weakThis->m_throttleTimer->start();
        }
    });
}
ScanTableModel::~ScanTableModel() {
    m_isDestroying = true;
    disconnect(&ThumbnailManager::instance(), nullptr, this, nullptr);

    if (m_thumbTimer) { m_thumbTimer->stop(); delete m_thumbTimer; m_thumbTimer = nullptr; }
    if (m_throttleTimer) { m_throttleTimer->stop(); delete m_throttleTimer; m_throttleTimer = nullptr; }
    if (m_metadataTimer) { m_metadataTimer->stop(); delete m_metadataTimer; m_metadataTimer = nullptr; }

    if (m_thumbPool) {
        m_thumbPool->clear();
        m_thumbPool->waitForDone();
        delete m_thumbPool;
        m_thumbPool = nullptr;
    }
}

int ScanTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_displayCount;
}

int ScanTableModel::columnCount(const QModelIndex& /*parent*/) const { return 4; }

QVariant ScanTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return QVariant();
    int row = index.row();
    if (row < 0 || row >= static_cast<int>(m_currentResultSet->keys.size())) return QVariant();
    
    uint64_t key = m_currentResultSet->keys[row];
    auto& reader = MftReader::instance();

    std::shared_lock<std::shared_mutex> soaLock(m_currentResultSet->soaMutex);

    // 【核心去锁方案】：所有渲染所需的字符/大小属性直接从 SoA 视口投影中快速读取，拒绝高开销引擎锁与递归查询
    bool isLoaded = row < static_cast<int>(m_currentResultSet->cachedPaths.size()) && !m_currentResultSet->cachedPaths[row].isEmpty();
    
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        if (!isLoaded) {
            // 滑动窗口尚未填充完的节点，提供快速骨架屏或优雅占位，杜绝等待
            if (index.column() == 0) return QString("加载中...");
            if (index.column() == 1) return QString("...");
            return "-";
        }

        switch (index.column()) {
            case 0: return m_currentResultSet->cachedNames[row];
            case 1: return m_currentResultSet->cachedPaths[row];
            case 2: {
                if (m_currentResultSet->isDirFlags[row]) return "-";
                int64_t size = m_currentResultSet->cachedSizes[row];
                if (size == 0) return "...";
                if (size < 1024) return QString("%1 B").arg(size);
                if (size < 1024 * 1024) return QString("%1 KB").arg(size / 1024.0, 0, 'f', 2);
                if (size < 1024LL * 1024 * 1024) return QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
                return QString("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
            }
            case 3: {
                int64_t ts = m_currentResultSet->cachedMtimes[row];
                if (ts <= 0) return "-";
                return QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
            }
        }
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        if (!isLoaded) {
            return reader.getCachedIcon("folder", false); // 未就绪前默认返回常规文件系统图标，杜绝黑卡片
        }

        bool isDir = m_currentResultSet->isDirFlags[row];
        QString path = m_currentResultSet->cachedPaths[row];
        QFileInfo fi(path);
        QString ext = fi.suffix().toLower();
        
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        if (thumbExts.contains(ext) && !isDir) {
            int64_t size = m_currentResultSet->cachedSizes[row];
            int64_t mtime = m_currentResultSet->cachedMtimes[row];

            ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
            int thumbSize = dlg ? dlg->m_config.iconSize : 64;

            // 调用双轨 LRU 缓存管家获取资产（100% 同步按需判定，防白屏和系统关联图标闪烁）
            bool hasPerfectMatch = false;
            QPixmap pix = ThumbnailManager::instance().requestThumbnail(key, path, ext, thumbSize, size, mtime, hasPerfectMatch);
            if (!pix.isNull()) {
                return pix; // 返回精确或历史大图拉伸资产
            }

            // 若彻底提取失败，回退使用系统关联图标
            if (ThumbnailManager::instance().isFailed(key)) {
                return reader.getCachedIcon(ext, false);
            }

            return QVariant(); // 纯空，提供给视图进行背景瓦片占位绘制
        }
        
        // 常规文件与目录，直接返回默认关联图标
        return reader.getCachedIcon(ext, isDir);
    } else if (role == Qt::ForegroundRole) {
        // 2026-06-xx 极致性能重构：优先从结果集的预取元数据中获取颜色，消除磁盘 IO 风险
        auto it = m_currentResultSet->metadata.find(key);
        if (it != m_currentResultSet->metadata.end()) {
            return it->second.color;
        }

        // 2026-06-xx 按照用户要求：名称列（第0列）强制显示为蓝色
        bool isDir = isLoaded ? m_currentResultSet->isDirFlags[row] : false;
        if (index.column() == 0 || isDir) return QColor("#3498db");
    } else if (role == Qt::ToolTipRole) {
        if (!isLoaded) return QString("加载中...");

        QString name = m_currentResultSet->cachedNames[row];
        QString qPath = m_currentResultSet->cachedPaths[row];
        bool isDir = m_currentResultSet->isDirFlags[row];
        
        QString sizeStr;
        if (isDir) {
            sizeStr = "-";
        } else {
            int64_t size = m_currentResultSet->cachedSizes[row];
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
        int64_t ts = m_currentResultSet->cachedMtimes[row];
        if (ts <= 0) {
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
        // 返回缩略图物理资产状态：0=未就绪/不支持, 1=有可用缩略图 (用于 Delegate 实施“缩略图第一优先、系统图标靠后兜底”绘制)
        if (!isLoaded) return 0;

        bool isDir = m_currentResultSet->isDirFlags[row];
        if (isDir) return 0;

        QString path = m_currentResultSet->cachedPaths[row];
        QFileInfo fi(path);
        QString ext = fi.suffix().toLower();
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        
        if (!thumbExts.contains(ext)) return 0;

        int64_t size = m_currentResultSet->cachedSizes[row];
        int64_t mtime = m_currentResultSet->cachedMtimes[row];
        QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(mtime);
        
        // 只要 L1 精确匹配命中，或者 L2 历史备份可用，即视为存在可用物理缩略图资产并返回 1，彻底删除任何多余的过渡加载状态。
        bool hasPerfectMatch = false;
        QPixmap pix = ThumbnailManager::instance().requestThumbnail(key, path, ext, 64, size, mtime, hasPerfectMatch);
        if (!pix.isNull()) {
            return 1;
        }
        return 0;
    } else if (role == Qt::UserRole + 2) {
        if (!isLoaded) return -1.0;
        bool isDir = m_currentResultSet->isDirFlags[row];
        if (isDir) {
            return -1.0;
        }

        QString path = m_currentResultSet->cachedPaths[row];
        QFileInfo fi(path);
        QString ext = fi.suffix().toLower();
        static const QSet<QString> mediaExts = {
            // 图形图像类
            "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "svg",
            // 视频类
            "mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "rmvb"
        };

        if (!mediaExts.contains(ext)) {
            return -1.0; // 常规文件类型，不提供有效正数宽高比，禁用自适应拉伸
        }

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

void ScanTableModel::updateResults(std::shared_ptr<ResultSet> nextSet) {
    auto baseSet = nextSet ? nextSet : m_controller->snapshot();
    auto newSet = std::make_shared<ResultSet>();
    newSet->metadata = baseSet->metadata;
    newSet->keyToPos = baseSet->keyToPos;

    // 彻底废除多媒体过滤与限制：三种视图模式全量共享 ResultSet 结果集（全量普通文件及文件夹数据）
    newSet->keys = baseSet->keys;

    int oldSize = (int)m_currentResultSet->keys.size();
    int newSize = (int)newSet->keys.size();

    // 2026-06-xx 极致性能重构：Diffing 局部刷新。
    // 物理铁律：在 emit 信号之前必须确保 m_currentResultSet 已更新，
    // 且信号范围必须与数据量绝对对齐，否则 TableView 内部索引越界会导致程序无响应（假死）。
    
    // 如果变动巨大或初始加载，或者模式切换导致的数据量落差，回退到 Reset 模式
    if (oldSize == 0 || std::abs(newSize - oldSize) > 500) {
        beginResetModel();
        m_currentResultSet = newSet;
        m_displayCount = newSize; 
        m_requestedThumbs.clear();
        m_failedThumbs.clear(); // 2026-07-xx 重置时也必须清理失败跟踪，避免由于路径变动或磁盘更新造成不可恢复的阻断
        m_pendingRows.clear(); // 2026-06-xx 任务修复：重置时必须清空待刷新行，防止索引失效
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

    // 【核心按需滑动装配方案】：引入 100% 视口惰性填充机制，杜绝 200万+ 数据下的全量堆动态分配
    // 理由：仅针对可见视口及上下 500 行的缓冲区 [VisibleTop - 500, VisibleBottom + 500] 范围进行短时装配。
    auto snap = m_currentResultSet;
    if (!snap || snap->keys.empty()) return;

    QPointer<ScanTableModel> weakThis(this);
    (void)QtConcurrent::run([snap, top, bottom, weakThis]() {
        if (!weakThis || weakThis->m_isDestroying) return;
        auto& reader = MftReader::instance();

        int total = static_cast<int>(snap->keys.size());
        int start = std::max<int>(0, top - 500);
        int end = std::min<int>(total - 1, bottom + 500);

        // 申请极短时间的引擎只读锁，一次性装配 1100 行视口数据投影
        // 理由：后台线程对 MftReader 的锁定与递归调用解耦。
        // getFullPath 等本身自带读锁保护，且为多线程安全的快速操作。为避免递归嵌套锁造成读写死锁隐患，
        // 投影填充过程应该在锁外逐行调用引擎方法，最后将临时副本合并，或不使用冗余的嵌套锁包围 getFullPath。
        // 核心优化：锁粒度极致缩减！绝不在耗时的 I/O 和 getFullPath 回溯过程中全程霸占 soaMutex，
        // 仅在最终写回缓存槽的瞬间持有 unique_lock，彻底消灭 UI 在 data() 读取时的死等现象！
        for (int i = start; i <= end; ++i) {
            if (!weakThis || weakThis->m_isDestroying) return;

            // 如果已经被外部滚动再次覆盖，或者本范围已无效，快速提前退出
            if (top != weakThis->m_visibleTop || bottom != weakThis->m_visibleBottom) return;

            uint64_t key = snap->keys[i];

            bool needLoad = false;
            {
                std::shared_lock<std::shared_mutex> soaShared(snap->soaMutex);
                if (snap->cachedPaths[i].isEmpty()) {
                    needLoad = true;
                }
            }

            if (needLoad) {
                int actualIndex = reader.getIndexByKey(key);
                if (actualIndex != -1) {
                    QString name = reader.getName(actualIndex);
                    QString path = reader.getFullPath(actualIndex);
                    int64_t size = reader.getSize(actualIndex);
                    int64_t mtime = reader.getModifyTime(actualIndex);
                    bool isDir = reader.isDirectory(actualIndex);

                    if (!reader.isMetadataFetched(actualIndex)) {
                        const_cast<MftReader&>(reader).requestMetadata(actualIndex);
                    }

                    // 短暂持锁写入
                    {
                        std::unique_lock<std::shared_mutex> soaLock(snap->soaMutex);
                        snap->cachedNames[i] = name;
                        snap->cachedPaths[i] = path;
                        snap->cachedSizes[i] = size;
                        snap->cachedMtimes[i] = mtime;
                        snap->isDirFlags[i] = isDir;
                    }
                }
            }
        }

        // 切回主线程触发局部刷新，信号范围精准锁定在 start 到 end 行，绝不产生越界
        if (weakThis && !weakThis->m_isDestroying) {
            QMetaObject::invokeMethod(weakThis.data(), [weakThis, start, end]() {
                if (!weakThis || weakThis->m_isDestroying) return;
                emit weakThis->dataChanged(weakThis->index(start, 0), weakThis->index(end, 3));
            }, Qt::QueuedConnection);
        }
    });
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

    // 2026-06-xx 任务 4.3：LIFO 优先级调度。
    // 理由：用户通常关注滚动停止后的可视区域，后加入队列的请求往往更具时效性。
    auto currentTasks = std::move(m_thumbTaskQueue);
    std::reverse(currentTasks.begin(), currentTasks.end());

    QPointer<ScanTableModel> weakThis(this);

    // 使用独立线程池异步执行缩略图提取，不使用全局线程池以防饥饿
    for (const auto& t : currentTasks) {
        m_thumbPool->start([weakThis, t]() {
            if (!weakThis || weakThis->m_isDestroying) return;

            // 确保工作线程已初始化 COM 环境
            static QThreadStorage<ScopedComInit> comStorage;
            if (!comStorage.hasLocalData()) {
                comStorage.setLocalData(ScopedComInit());
            }

            auto& reader = MftReader::instance();
            int actualIdx = reader.getIndexByKey(t.key);
            if (actualIdx == -1) return;

            if (!weakThis || weakThis->m_isDestroying) return;
            QString fullPath = reader.getFullPath(actualIdx);
            if (fullPath.isEmpty()) return;

            if (!weakThis || weakThis->m_isDestroying) return;
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

            if (!weakThis || weakThis->m_isDestroying) return;

            if (!img.isNull()) {
                double ar = (double)img.width() / (double)img.height();
                // 切回主线程登记单条结果
                QMetaObject::invokeMethod(weakThis.data(), [weakThis, key = t.key, cacheKey = t.cacheKey, img, ar]() {
                    if (!weakThis || weakThis->m_isDestroying) return;
                    QPixmap pix = QPixmap::fromImage(img);
                    if (!pix.isNull()) {
                        weakThis->m_thumbCache.insert(cacheKey, new QPixmap(pix));
                        weakThis->m_lastPixmapCache.insert(QString::number(key), new QPixmap(pix)); // 实时注册副本，作为下一次调节时的渐进拉伸源
                    }
                    weakThis->m_aspectRatios[key] = ar;

                    auto snapshot = weakThis->m_controller->snapshot();
                    auto itPos = snapshot->keyToPos.find(key);
                    if (itPos != snapshot->keyToPos.end() && itPos->second < weakThis->m_displayCount) {
                        weakThis->m_pendingRows.insert(itPos->second);
                        if (!weakThis->m_throttleTimer->isActive()) weakThis->m_throttleTimer->start();
                    }
                }, Qt::QueuedConnection);
            } else {
                // 【核心改进】：获取失败，记录进失败名单，并强制刷新，退化使用默认图标兜底
                QMetaObject::invokeMethod(weakThis.data(), [weakThis, key = t.key]() {
                    if (!weakThis || weakThis->m_isDestroying) return;
                    weakThis->m_failedThumbs.insert(key);
                    auto snapshot = weakThis->m_controller->snapshot();
                    auto itPos = snapshot->keyToPos.find(key);
                    if (itPos != snapshot->keyToPos.end() && itPos->second < weakThis->m_displayCount) {
                        weakThis->m_pendingRows.insert(itPos->second);
                        if (!weakThis->m_throttleTimer->isActive()) weakThis->m_throttleTimer->start();
                    }
                }, Qt::QueuedConnection);
            }
        });
    }
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

} // namespace FERREX
