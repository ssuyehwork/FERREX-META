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
namespace ArcMeta {

class JustifiedView;
class ThumbnailDelegate;

struct ScanConfig {
    QSet<QString> activeDrives;
    QSet<QString> defaultDrives;
    QSet<QString> ignoredDrives;
    QStringList queryHistory;
    QStringList extHistory;
    
    int viewMode = 0;   // 0: Details, 1: Icons
    int iconSize = 128; // 256, 128, 64
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

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    void updateResults();
    void clearThumbCache() { m_thumbCache.clear(); }

    Qt::DropActions supportedDragActions() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private:
    ScanController* m_controller;
    std::shared_ptr<ResultSet> m_currentResultSet;
    int m_displayCount = 0;

    mutable QCache<QString, QPixmap> m_thumbCache;
    mutable QSet<uint64_t> m_requestedThumbs;
    mutable QMap<uint64_t, double> m_aspectRatios; // 存储宽高比
    
    QSet<int> m_pendingRows;  
    QTimer* m_throttleTimer = nullptr;
};

class ScanDialog : public FramelessDialog {
    Q_OBJECT
    friend class ScanTableModel;
public:
    explicit ScanDialog(QWidget* parent = nullptr);
    ~ScanDialog() override;

private slots:
    void onStartScan();
    void onTriggerSearch();
    void onFilterOptionChanged();
    void onCustomContextMenu(const QPoint& pos);
    void onItemDoubleClicked(const QModelIndex& index);
    void onSelectionChanged();
    void onDriveContextMenu(const QString& drive, const QPoint& pos);
    void onIgnoredDriveContextMenu(const QString& drive, const QPoint& pos);
    void onRenameTriggered();

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
};

} // namespace ArcMeta
