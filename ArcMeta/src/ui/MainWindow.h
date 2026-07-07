#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QToolBar>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QSystemTrayIcon>
#include <QSet>

namespace ArcMeta {

class TrayController;
class HoverEventFilter;
class ResizeEventFilter;
class AddressBar;
class CategoryPanel;
class NavPanel;
class ContentPanel;
class MetaPanel;
class FilterPanel;
class SearchHistoryPanel;

/**
 * @brief 主窗口类
 * 负责六栏布局的组装、QSplitter 管理及自定义标题栏按钮
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    // 2026-04-11 按照用户要求：showEvent 是执行 ToolTipOverlay GPU 真实预热的唯一合法时机
    void showEvent(QShowEvent* event) override;

    /**
     * @brief 2026-05-24 按照用户要求：拦截 Windows 原生事件监听硬件变化
     */
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

private slots:
    void onPinToggled(bool checked);
    void onBackClicked();
    void onForwardClicked();
    void onUpClicked();
    void onStatusBarStatsUpdated(int fileCount, int folderCount, int totalCount);
    void onDriveButtonClicked();
    void onDriveButtonContextMenu(const QPoint& pos);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // 2026-05-08 按照用户要求：添加边缘resize相关成员变量
    enum ResizeDirection {
        None = 0,
        Left, Right, Top, Bottom,
        TopLeft, TopRight, BottomLeft, BottomRight
    };

    ResizeDirection m_resizeDir = None;
    bool m_isResizing = false;
    QPoint m_resizeStartGlobal;
    QRect  m_resizeStartGeometry;
    
    static constexpr int kResizeMargin = 6; // 边缘热区宽度（像素）
    
    ResizeDirection getResizeDirection(const QPoint& localPos) const;
    void updateCursorShape(ResizeDirection dir);

    QWidget* m_titleBarWidget = nullptr;
    QHBoxLayout* m_titleBarLayout = nullptr;
    QLabel* m_logoLabel = nullptr;
    QLabel* m_appNameLabel = nullptr;
    QWidget* m_navBarWidget = nullptr;
    QHBoxLayout* m_navBarLayout = nullptr;
    QVBoxLayout* m_bodyLayout = nullptr; // 2026-05-08 按照用户要求：提升为成员变量以支持动态边距切换

    void initUi();
    void updateNavButtons();
    void updateStatusBar();
    void initDriveBar();

    // 2026-07-xx 导航协议常量
    static inline const QString kProtocolFile     = "file://";
    static inline const QString kProtocolCategory = "category://";
    static inline const QString kProtocolSystem   = "system://";

    /**
     * @brief 2026-07-xx 按照 Plan-56：统一导航调度中心
     * 支持 file://, category://, system:// 等协议
     */
    void unifiedNavigateTo(const QString& url, bool record = true);

    void initToolbar();
    void setupSplitters();
    void setupCustomTitleBarButtons();
    void resetSplitterLayout();

    // 复合地址栏
    AddressBar* m_addressBar = nullptr;

    // 六个面板
    CategoryPanel* m_categoryPanel = nullptr;
    // 2026-04-11 按照用户要求：记录当前预览的文件路径，用于驱动方向键切图
    QString m_currentQuickLookPath;
    
    // UI Panels
    NavPanel* m_navPanel = nullptr;
    ContentPanel* m_contentPanel = nullptr;
    MetaPanel* m_metaPanel = nullptr;
    FilterPanel* m_filterPanel = nullptr;
    class TagManagerView* m_tagManagerView = nullptr;
    class InvalidDataListView* m_invalidDataListView = nullptr;

    QSplitter* m_mainSplitter = nullptr;

    // 工具栏组件
    QToolBar* m_toolbar    = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QPushButton* m_btnBack    = nullptr;
    QPushButton* m_btnForward = nullptr;
    QPushButton* m_btnUp      = nullptr;

    // 2026-04-12 按照用户要求：搜索历史悬浮面板及历史记录
    QWidget* m_searchContainer = nullptr; // 搜索框容器
    SearchHistoryPanel* m_searchHistoryPanel = nullptr;
    QStringList  m_searchHistory;             // 最近 10 条关键词
    
    // 标题栏按钮组 (用于 frameless 时的模拟，此处作为标准按钮展示)
    QPushButton* m_btnToggleDriveBar = nullptr;
    QPushButton* m_btnSync   = nullptr;
    QPushButton* m_btnLayout = nullptr;
    QPushButton* m_btnCreate = nullptr;
    QPushButton* m_btnPinTop = nullptr;
    QPushButton* m_btnMin = nullptr;
    QPushButton* m_btnMax = nullptr;
    QPushButton* m_btnClose = nullptr;

    // 盘符管理栏组件
    QWidget* m_driveBarWidget = nullptr;
    QHBoxLayout* m_driveBarLayout = nullptr;
    QMap<QString, class DriveButton*> m_driveButtons;

    // 状态管理
    bool m_isPinned = false;
    bool m_isTagManagerMode = false;
    QString m_currentDataSource; // "category" or "nav"
    int m_currentCategoryId = 0;
    bool m_panelsInitialized = false; // 2026-04-12 状态锁：确保面板仅初始化一次
    QTimer* m_searchTimer = nullptr; // 2026-xx-xx 按照 Plan-106：搜索防抖计时器
    QString m_currentPath;
    QStringList m_history;
    int m_historyIndex = -1;

    // 底部状态栏
    QLabel* m_statusLeft = nullptr;

    // 窗口拖动
    bool m_isDragging = false;
    QPoint m_dragPosition;

    // 系统托盘控制器
    TrayController* m_trayController = nullptr;
    HoverEventFilter* m_hoverFilter = nullptr;
    ResizeEventFilter* m_resizeFilter = nullptr;
    QTimer* m_sidebarRefreshTimer = nullptr;

public slots:
    /**
     * @brief 2026-07-xx 按照 Plan-63：显示统一的面板显隐控制菜单
     */
    void showPanelContextMenu(const QPoint& globalPos);

    /**
     * @brief 2026-07-xx 按照 Plan-63：为已有菜单填充面板显隐 Action
     */
    void populatePanelMenu(QMenu* menu);

private:
    void loadPanelVisibility();
    void savePanelVisibility();
};

} // namespace ArcMeta
