#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma once

#include <QAbstractTableModel>
#include <QCache>
#include <QSet>
#include <QMap>
#include <QTimer>
#include <QPixmap>
#include <memory>
#include <QList>
#include <QMimeData>
#include <atomic>
#include "ThumbnailManager.h"

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
class ThumbnailManager;

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
    QThreadPool* getThumbPool() const { return ThumbnailManager::instance().getThreadPool(); }

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    void updateResults(std::shared_ptr<ResultSet> nextSet = nullptr);
    void clearThumbCache(bool keepLastCache = false);

    Qt::DropActions supportedDragActions() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private:
    std::atomic<bool> m_isDestroying{false};
    ScanController* m_controller;
    std::shared_ptr<ResultSet> m_currentResultSet;
    int m_displayCount = 0;

    mutable QSet<uint64_t> m_requestedThumbs;
    mutable QMap<uint64_t, double> m_aspectRatios; // 存储宽高比

    QSet<int> m_pendingRows;  
    QTimer* m_throttleTimer = nullptr;

    QTimer* m_metadataTimer = nullptr;
    int m_visibleTop = -1;
    int m_visibleBottom = -1;
};

} // namespace FERREX
