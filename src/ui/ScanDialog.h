#pragma once

#include "FramelessDialog.h"
#include "../core/IndexedEntry.h"
#include "../core/CacheManager.h"
#include "../mft/UsnWatcher.h"
#include <QListWidget>
#include <QCheckBox>
#include <QFrame>
#include <QProgressBar>
#include <QFuture>
#include <QFutureWatcher>
#include <QCloseEvent>
#include <QLineEdit>
#include <QTableView>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QScrollArea>
#include <QFileIconProvider>
#include <QFileInfo>
#include <memory>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QMap>
#include <QCache>
#include <QTimer>
#include <QReadWriteLock>
#include <QStackedWidget>
#include <QListView>
#include <QActionGroup>
#include <atomic>

#include "ScanController.h"
namespace FERREX {

class JustifiedView;
class ThumbnailDelegate;
class QuickLookWindow;

struct ScanConfig {
    QSet<QString> activeDrives;
    QSet<QString> defaultDrives;
    QStringList queryHistory;
    QStringList extHistory;
    
    int viewMode = 0;   // 0: Details, 1: Icons
    int iconSize = 128; // 256, 128, 64
    int layoutMode = 0; // 0: JustifiedMode, 1: GridMode
    int sortColumn = 0; 
    int sortOrder = 0;  // 0: Asc, 1: Desc

    bool useRegex = true;
    bool caseSensitive = false;
    bool includeHidden = false;
    bool includeSystem = false;
    bool includeDollar = false;
    bool autoDisplay = false;

    void load();
    void save();
};


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

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    void updateResults(std::shared_ptr<ResultSet> nextSet = nullptr);
    void clearThumbCache(bool keepLastCache = false) {
        m_thumbCache.clear(); 
        m_requestedThumbs.clear(); 
        m_thumbTaskQueue.clear();
        if (!keepLastCache) {
            m_lastPixmapCache.clear();
        }
    }

    Qt::DropActions supportedDragActions() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private slots:
    void processThumbQueue();

private:
    ScanController* m_controller;
    std::shared_ptr<ResultSet> m_currentResultSet;
    int m_displayCount = 0;

    QThreadPool* m_thumbPool = nullptr; // 2026-06-xx 任务二：缩略图生成专用隔离线程池

    mutable QCache<QString, QPixmap> m_thumbCache;
    mutable QCache<QString, QPixmap> m_lastPixmapCache; // 2026-07-xx 渐进式占位双轨缓存 (Key 为 QString::number(key))
    mutable QSet<uint64_t> m_requestedThumbs;
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

class ScanDialog : public FramelessDialog {
    Q_OBJECT
    friend class ScanTableModel;
public:
    explicit ScanDialog(QWidget* parent = nullptr);
    ~ScanDialog() override;

private slots:
    void onStartScan(const QString& drive = QString());
    void onTriggerSearch();
    void onFilterOptionChanged();
    void onCustomContextMenu(const QPoint& pos);
    void onItemDoubleClicked(const QModelIndex& index);
    void onSelectionChanged();
    void onDriveContextMenu(const QString& drive, const QPoint& pos);
    void onRenameTriggered();
    void onCopyTriggered(bool isCut = false);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void showDriveLoading();
    void refreshDriveList(bool forceProbe = false);
    void updateDriveButtonStyles();
    void updateStatus(const QString& text, bool scanning = false, int64_t totalCount = -1);
    void updateStatusBar();
    void triggerWarmup(); // 2026-07-07 新增：缩略图预热流水线 (Analysis_Modification_Plan-154.md)
    void selectAllResults(); // 2026-07-07 物理修复：实现绕过视图布局状态的全量选择逻辑
    void handleMetadataShortcut(QKeyEvent* event);
    QString formatNumber(int64_t n);
    QString formatSize(int64_t bytes);

    struct DriveInfo {
        QString letter;
        QString label;
        bool isNtfs;
        bool hasMedia;
    };
    QVector<DriveInfo> m_cachedDriveInfos;
    QMap<QString, QPushButton*> m_driveButtonMap;

    QLineEdit* m_searchEdit = nullptr;
    QLineEdit* m_extEdit = nullptr;
    QPushButton* m_searchBtn = nullptr;
    QCheckBox* m_checkRegex = nullptr;
    QCheckBox* m_checkCase = nullptr;
    QCheckBox* m_checkHidden = nullptr;
    QCheckBox* m_checkSystem = nullptr;
    QCheckBox* m_checkDollar = nullptr;
    QCheckBox* m_checkAuto = nullptr;
    
    QHBoxLayout* m_driveLayout = nullptr;
    QWidget* m_driveContainer = nullptr;
    
    QTableView* m_resultView = nullptr;
    JustifiedView* m_iconView = nullptr;
    QStackedWidget* m_viewStack = nullptr;
    ScanTableModel* m_tableModel = nullptr;
    QuickLookWindow* m_quickLook = nullptr;

    ScanController* m_controller = nullptr;

    QLabel* m_titleStatusLabel = nullptr; 
    QLabel* m_statLabelMain = nullptr;    
    QLabel* m_statLabelTime = nullptr;    
    QLabel* m_statLabelMemory = nullptr; 
    QLabel* m_selectionLabel = nullptr;  
    QPushButton* m_csvBtn = nullptr;     
    QProgressBar* m_progressBar = nullptr;
    QSlider* m_sizeSlider = nullptr;

    int64_t m_lastSearchMs = 0;

    std::unique_ptr<CacheManager> m_cacheManager;
    ScanConfig m_config;

protected:
    void closeEvent(QCloseEvent* event) override;
};

} // namespace FERREX
