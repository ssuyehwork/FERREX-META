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
    qRegisterMetaType<FERREX::FerrexItemPayload>("FERREX::FerrexItemPayload");
    m_currentResultSet = std::make_shared<ResultSet>();

    // 建立隔离的缩略图任务专用线程池，避免与主后台任务竞争资源
    m_thumbPool = new QThreadPool(this);
    
    // 2026-06-xx 任务三：磁盘类型感知的线程调度
    // 默认保守策略：若无法获取配置或存在 HDD，则使用串行模式 (1) 保护寻道性能
    bool allSSD = true;
    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent);
    if (dlg) {
        // 2026-06-xx 物理修复：使用 ShellHelper::isSolidStateDrive 统一探测
        for (const QString& d : dlg->m_config.activeDrives) {
            if (!ShellHelper::isSolidStateDrive(d)) {
                allSSD = false;
                break;
            }
        }
    } else {
        allSSD = false; // 未知环境下保守处理
    }

    // 强制缩略图和大图解析线程并发限制为 1，杜绝多线程 COM STA 套间冲突引发的假死和底层卡顿
    m_thumbPool->setMaxThreadCount(1);

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
                emit dataChanged(index(startRow, 0), index(endRow, 3), {Qt::DecorationRole, Qt::DisplayRole});
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
    if (m_thumbTimer) { m_thumbTimer->stop(); delete m_thumbTimer; m_thumbTimer = nullptr; }
    if (m_throttleTimer) { m_throttleTimer->stop(); delete m_throttleTimer; m_throttleTimer = nullptr; }
    if (m_metadataTimer) { m_metadataTimer->stop(); delete m_metadataTimer; m_metadataTimer = nullptr; }

    if (m_thumbPool) {
        // 1. 快速排空排队任务
        m_thumbPool->clear();

        // 2. 将线程池的同步销毁工作在后台线程中异步执行，防止主线程（GUI 线程）退出时发生同步 waitForDone 阻塞死等！
        auto* pool = m_thumbPool;
        (void)QtConcurrent::run([pool]() {
            pool->waitForDone();
            delete pool;
        });
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
    if (row < 0 || row >= (int)m_currentResultSet->keys.size()) return QVariant();
    
    uint64_t key = m_currentResultSet->keys[row];
    auto& reader = MftReader::instance();
    int actualIndex = reader.getIndexByKey(key);
    if (actualIndex == -1) return QVariant(); // 文件可能已被删除

    // 工业级 SoA 前置缓存投影获取（完全免除 UI 线程加锁与递归调用）
    QString name;
    QString fullPath;
    int64_t size = 0;
    int64_t ts = 0;
    bool isDirectory = false;
    QString ext;

    if (row < (int)m_currentResultSet->cachedNames.size()) {
        name = m_currentResultSet->cachedNames[row];
        fullPath = m_currentResultSet->cachedPaths[row];
        size = m_currentResultSet->cachedSizes[row];
        ts = m_currentResultSet->cachedMtimes[row];
        isDirectory = m_currentResultSet->isDirFlags[row];
    } else {
        // 安全回退（若尚未投影缓存）
        name = reader.getName(actualIndex);
        fullPath = reader.getFullPath(actualIndex);
        size = reader.getSize(actualIndex);
        ts = reader.getModifyTime(actualIndex);
        isDirectory = reader.isDirectory(actualIndex);
    }
    ext = reader.getExtQString(actualIndex);

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case 0: return name;
            case 1: return fullPath;
            case 2: {
                if (isDirectory) return "-";
                if (size == 0 && !reader.isMetadataFetched(actualIndex)) {
                    return "...";
                }
                if (size < 1024) return QString("%1 B").arg(size);
                if (size < 1024 * 1024) return QString("%1 KB").arg(size / 1024.0, 0, 'f', 2);
                if (size < 1024LL * 1024 * 1024) return QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
                return QString("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
            }
            case 3: {
                if (ts == 0 && !reader.isMetadataFetched(actualIndex)) {
                    return "-";
                }
                if (ts == 0) return "-";
                return QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
            }
        }
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        if (thumbExts.contains(ext) && !isDirectory) {
            // 2026-06-xx 极致性能优化：使用 CompositeKey + Size + Mtime 构建 O(1) 的原子 CacheKey
            QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(ts);

            // 1. 精确尺寸缓存匹配
            QPixmap* cached = m_thumbCache.object(cacheKey);
            if (cached) return *cached;

            // 2.【核心改进：先判断历史缩略图并做平滑拉伸】
            QPixmap* lastCached = m_lastPixmapCache.object(QString::number(key));
            if (lastCached) {
                // 后台静默生成符合全新精确尺寸的高画质大图
                if (!m_requestedThumbs.contains(key)) {
                    m_requestedThumbs.insert(key);
                    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                    int thumbSize = dlg ? dlg->m_config.iconSize : 64; // 不再对列表视图强行截断 24px，使其跟随滚轮联动缩放 [1]
                    m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                    if (!m_thumbTimer->isActive()) m_thumbTimer->start();
                }

                // 物理资产优先：直接返回原始的历史 Pixmap 资产，由 Delegate 进行后续 Cover/Contain 平滑拉伸，绝不闪现系统默认图标
                return *lastCached;
            }

            // C. 失败兜底阻断器：如果已经被标记为彻底提取失败，则可以穿透放行，退回到最下方的系统默认图标展示。
            if (m_failedThumbs.contains(key)) {
                return reader.getCachedIcon(ext, false);
            }

            // D. 加载期强制阻断方案：此时缩略图在加载队列中尚未产生。为了杜绝默认图标的插足闪跃，模型层强制返回“符合规范的空 QVariant()”。
            if (!m_requestedThumbs.contains(key)) {
                m_requestedThumbs.insert(key);
                ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                int thumbSize = dlg ? dlg->m_config.iconSize : 64; // 不再对列表视图强行截断 24px，使其跟随滚轮联动缩放 [1]
                
                m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                if (!m_thumbTimer->isActive()) m_thumbTimer->start();
            }

            return QVariant(); // 【核心物理阻断点】向视图提供空数据，掐断默认图标的透传通路！
        }
        
        // 常规不支持缩略图的后缀（如 txt, exe），直接放行，回退到系统默认图标
        return reader.getCachedIcon(ext, isDirectory);
    } else if (role == Qt::ForegroundRole) {
        // 2026-06-xx 极致性能重构：优先从结果集的预取元数据中获取颜色，消除磁盘 IO 风险
        auto it = m_currentResultSet->metadata.find(key);
        if (it != m_currentResultSet->metadata.end()) {
            return it->second.color;
        }

        // 2026-06-xx 兜底逻辑：若未预取，则计算路径查询，由于 getPath 带有行内缓存，性能依然可控
        auto meta = MetadataManager::instance().getMeta(fullPath.toStdWString());
        if (!meta.color.empty()) {
            QColor tagC = UiHelper::parseColorName(QString::fromStdWString(meta.color));
            if (tagC.isValid()) return tagC;
        }
        // 2026-06-xx 按照用户要求：名称列（第0列）强制显示为蓝色
        if (index.column() == 0 || isDirectory) return QColor("#3498db");
    } else if (role == Qt::ToolTipRole) {
        // 2026-06-xx 极致性能重构：消除 ToolTipRole 中的重复路径回溯
        // 2026-07-12 物理对齐需求：ToolTipOverlay 显示的内容包含项目名称、路径、大小、修改时间 (并兼容备注和标签)
        QString sizeStr;
        if (isDirectory) {
            sizeStr = "-";
        } else {
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
        if (ts == 0 && !reader.isMetadataFetched(actualIndex)) {
            mtimeStr = "-";
        } else if (ts == 0) {
            mtimeStr = "-";
        } else {
            mtimeStr = QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
        }

        QString tip = QString::fromUtf8("名称: ") + name + "\n" +
                      QString::fromUtf8("路径: ") + fullPath + "\n" +
                      QString::fromUtf8("大小: ") + sizeStr + "\n" +
                      QString::fromUtf8("修改时间: ") + mtimeStr;

        return tip;
    } else if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
            case 0: case 1: return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
            case 2: case 3: return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
    } else if (role == Qt::UserRole) {
        FerrexItemPayload p;
        p.key = key;
        p.name = name;
        p.fullPath = fullPath;
        p.extension = ext;
        p.isDirectory = isDirectory;

        // 1. 宽高比 aspectRatio
        if (p.isDirectory) {
            p.aspectRatio = -1.0;
        } else {
            QString extLower = p.extension.toLower();
            static const QSet<QString> mediaExts = {
                "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "svg",
                "mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "rmvb"
            };
            if (!mediaExts.contains(extLower)) {
                p.aspectRatio = -1.0;
            } else {
                p.aspectRatio = m_aspectRatios.value(key, 1.0);
            }
        }

        // 2. 缩略图就绪状态 thumbStatus
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        if (p.isDirectory || !thumbExts.contains(p.extension)) {
            p.thumbStatus = 0;
        } else {
            QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(ts);
            if (m_thumbCache.contains(cacheKey) || m_lastPixmapCache.contains(QString::number(key))) {
                p.thumbStatus = 1;
            } else {
                p.thumbStatus = 0;
            }
        }

        // 3. 关系管理状态 isManaged
        p.isManaged = false;
        auto it = m_currentResultSet->metadata.find(key);
        if (it != m_currentResultSet->metadata.end()) {
            p.isManaged = it->second.color.isValid();
        } else {
            auto meta = MetadataManager::instance().getMeta(p.fullPath.toStdWString());
            if (!meta.color.empty()) {
                p.isManaged = true;
            }
        }

        // 4. 是否为空文件夹 isEmptyFolder
        p.isEmptyFolder = false;

        // 5. 极致绘图优化：DisplayName 预生成，插入零宽字符，杜绝高频 paint 分配
        QString dispName = p.name;
        dispName.replace("_", "_\u200B");
        dispName.replace(".", ".\u200B");
        p.displayName = dispName;

        return QVariant::fromValue(p);
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
    QSet<uint64_t> seen;
    for (const QModelIndex& idx : indexes) {
        if (idx.column() != 0) continue;
        int row = idx.row();
        if (row < 0 || row >= (int)m_currentResultSet->keys.size()) continue;
        uint64_t key = m_currentResultSet->keys[row];
        if (seen.contains(key)) continue;
        seen.insert(key);

        QModelIndex modelIdx = index(row, 0);
        FerrexItemPayload payload = this->data(modelIdx, Qt::UserRole).value<FerrexItemPayload>();
        QString path = payload.fullPath;
        if (!path.isEmpty()) urls << QUrl::fromLocalFile(path);
    }
    data->setUrls(urls);
    return data;
}

} // namespace FERREX
