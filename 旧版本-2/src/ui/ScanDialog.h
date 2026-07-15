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
#include "ConfigManager.h"
#include "SystemDriveScanner.h"

class QTextEdit;
namespace FERREX {

class JustifiedView;
class ThumbnailDelegate;
class QuickLookWindow;
class IScanResultView;

class PreviewRulesDialog : public FramelessDialog {
    Q_OBJECT
public:
    explicit PreviewRulesDialog(ScanConfig& config, QWidget* parent = nullptr);

private slots:
    void onRestoreDefaults();
    void onConfirm();

private:
    ScanConfig& m_config;
    QTextEdit* m_whitelistEdit = nullptr;
    QTextEdit* m_blacklistEdit = nullptr;
};

class ScanTableModel;
class ResultTableColumnWidthPolicy;
class StatusBarFormatter;
class FramelessResizeBorder;
class HistoryDropdownController;
class ContextMenuExecutor;
class ThumbnailWarmupPipeline;
class GlobalKeyboardShortcutHandler;
class ViewportTooltipController;

class ScanDialog : public FramelessDialog {
    Q_OBJECT
    friend class ScanTableModel;
    friend class ResultTableColumnWidthPolicy;
    friend class StatusBarFormatter;
    friend class FramelessResizeBorder;
    friend class HistoryDropdownController;
    friend class ContextMenuExecutor;
    friend class ThumbnailWarmupPipeline;
    friend class GlobalKeyboardShortcutHandler;
    friend class ViewportTooltipController;

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

public:
    void setHistoryText(const QString& text, bool isQuery);
    void removeHistoryItem(const QString& text, bool isQuery);
    void reopenHistoryMenu(bool isQuery);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override; // 重载大小变更
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static constexpr int kResizeMargin = 6; // DPI 基准热区像素宽度

    // 辅助控制子系统实例
    FramelessResizeBorder* m_resizeFilter = nullptr;
    HistoryDropdownController* m_historyDropdownController = nullptr;
    ContextMenuExecutor* m_contextMenuExecutor = nullptr;
    ThumbnailWarmupPipeline* m_thumbnailWarmupPipeline = nullptr;
    GlobalKeyboardShortcutHandler* m_globalKeyboardShortcutHandler = nullptr;
    ViewportTooltipController* m_viewportTooltipController = nullptr;

    void setupUi();
    void showDriveLoading();
    void refreshDriveList(bool forceProbe = false);
    void updateDriveButtonStyles();
    void updateStatus(const QString& text, bool scanning = false, int64_t totalCount = -1);
    void updateStatusBar();
    void refreshVisibleMetadataRange();
    int calculateNameColumnMinimumWidth() const;
    void triggerWarmup();
    void selectAllResults();
    void handleMetadataShortcut(QKeyEvent* event);
    QString formatNumber(int64_t n);
    QString formatSize(int64_t bytes);

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
    
    IScanResultView* m_listResultView = nullptr;
    IScanResultView* m_justifiedResultView = nullptr;
    IScanResultView* m_gridResultView = nullptr;
    IScanResultView* m_currentActiveView = nullptr;

    QStackedWidget* m_viewStack = nullptr;
    ScanTableModel* m_tableModel = nullptr;
    QuickLookWindow* m_quickLook = nullptr;

    ScanController* m_controller = nullptr;

    void switchToView(int viewMode, int layoutMode);

    QLabel* m_titleStatusLabel = nullptr; 
    QLabel* m_statLabelMain = nullptr;    
    QLabel* m_statLabelTime = nullptr;    
    QLabel* m_statLabelMemory = nullptr; 
    QLabel* m_selectionLabel = nullptr;  
    QProgressBar* m_progressBar = nullptr;
    QSlider* m_sizeSlider = nullptr;
    QTimer* m_configSaveTimer = nullptr;
    QTimer* m_zoomDebounceTimer = nullptr;

    int64_t m_lastSearchMs = 0;

    std::unique_ptr<CacheManager> m_cacheManager;
    ScanConfig& m_config;

    QAction* m_actJMode = nullptr;
    QAction* m_actGMode = nullptr;
    QAction* m_actListMode = nullptr;

protected:
    void closeEvent(QCloseEvent* event) override;
};

} // namespace FERREX
