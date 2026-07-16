#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma once

#include <QAbstractTableModel>
#include <QThreadPool>
#include <QCache>
#include <QSet>
#include <QMap>
#include <QTimer>
#include <QPixmap>
#include <memory>
#include <QList>
#include <QMimeData>
#include <atomic>

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

class ScanController;
struct ResultSet;

class ScanTableModel : public QAbstractTableModel {
    Q_OBJECT
    friend class ScanDialog;
public:
    explicit ScanTableModel(ScanController* controller, QObject* parent = nullptr);
    ~ScanTableModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    // 虚拟化加载支持
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    void setVisibleRange(int top, int bottom);
    void forceFetchAll(); // 2026-07-07 物理修复：强制加载全部结果以支持全选
    QThreadPool* getThumbPool() const { return m_thumbPool; }

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    void updateResults(std::shared_ptr<ResultSet> nextSet = nullptr);
    void clearThumbCache(bool keepLastCache = false) { 
        m_thumbCache.clear(); 
        m_requestedThumbs.clear(); 
        m_thumbTaskQueue.clear();
        m_failedThumbs.clear();
        if (!keepLastCache) {
            m_lastPixmapCache.clear();
        }
    }

    Qt::DropActions supportedDragActions() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private slots:
    void processThumbQueue();

private:
    std::atomic<bool> m_isDestroying{false};
    ScanController* m_controller;
    std::shared_ptr<ResultSet> m_currentResultSet;
    int m_displayCount = 0;

    QThreadPool* m_thumbPool = nullptr; // 2026-06-xx 任务二：缩略图生成专用隔离线程池

    mutable QCache<QString, QPixmap> m_thumbCache;
    mutable QCache<QString, QPixmap> m_lastPixmapCache; // 2026-07-xx 渐进式占位双轨缓存 (Key 为 QString::number(key))
    mutable QSet<uint64_t> m_requestedThumbs;
    mutable QSet<uint64_t> m_failedThumbs; // 记录由于格式损坏或物理错误导致提取失败的 FRN key，避免重复开销并允许兜底退化
    mutable QMap<uint64_t, double> m_aspectRatios; // 存储宽高比
    
    // 2026-06-xx 极致架构：并行批处理缩略图队列
    struct ThumbTask {
        uint64_t key;
        int size;
        QString ext;
        QString cacheKey;
    };
    mutable QList<ThumbTask> m_thumbTaskQueue;
    QTimer* m_thumbTimer = nullptr;

    QSet<int> m_pendingRows;  
    QTimer* m_throttleTimer = nullptr;

    QTimer* m_metadataTimer = nullptr;
    int m_visibleTop = -1;
    int m_visibleBottom = -1;
};

} // namespace FERREX
