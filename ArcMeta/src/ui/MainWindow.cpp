#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MainWindow.h"
#include <QDateTime>
#include "Logger.h"
#include "../core/UndoManager.h"
#include "../core/BasicCommands.h"
#include "TrayController.h"
#include "HoverEventFilter.h"
#include "ResizeEventFilter.h"
#include "AddressBar.h"
#include "../core/CoreController.h"
#include "CategoryPanel.h"
#include "CategoryModel.h"
#include "NavPanel.h"
#include "ContentPanel.h"
#include "MetaPanel.h"
#include "FilterPanel.h"
#include "TagManagerView.h"
#include "InvalidDataListView.h"
#include "QuickLookWindow.h"
#include "ToolTipOverlay.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include "../mft/MftReader.h"
#include "../meta/CategoryRepo.h"

#include "SearchHistoryPanel.h"
#include "SvgIcons.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSvgRenderer>
#include <QPainter>
#include <QIcon>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCursor>
#include <QApplication>
#include "../core/AppConfig.h"
#include <QCloseEvent>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include "UiHelper.h"
#include "StyleLibrary.h"
#include "../core/SyncStatusService.h"
#include "DriveButton.h"
#include "../util/ShellHelper.h"
using namespace ArcMeta::Style;
#include "../core/ModelContract.h"
#include <QFileInfo>
#include <QDir>
#include "../meta/MetadataManager.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <Dbt.h>
#include <psapi.h>
#endif


#include <QtConcurrent>

namespace ArcMeta {

// 【物理护栏-禁止修改/禁止改为0】全局边缘留白基准值，统一应用于标题栏/导航栏/主体容器右侧
// 及状态栏左右两侧。2026-06-xx 曾被错误改为0导致搜索框/元数据/筛选面板右侧被截断，
// 任何"贴合边缘/滚动条对齐/物理修正"等理由都不能作为改动此常量或下方四处引用的依据。
constexpr int kEdgeMargin = 5;
constexpr int kStatusBarMargin = 12;

MainWindow::~MainWindow() {
    // 对应 initUi() 中 QCoreApplication::instance()->installEventFilter(m_resizeFilter)
    // 安装位置和卸载位置必须严格一致，禁止改成 this->removeEventFilter(...)
    if (m_resizeFilter) {
        QCoreApplication::instance()->removeEventFilter(m_resizeFilter);
    }
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_currentDataSource("nav"), m_currentCategoryId(0) {
    // 2026-04-12 关键修复：显式初始化面板加载状态锁，防止未定义行为导致闪退
    m_panelsInitialized = false;
    qDebug() << "[Main] MainWindow 构造开始执行";

    // 2026-04-11 按照用户要求：在程序启动的最顶端预初始化 ToolTipOverlay
    // 配合 ToolTipOverlay 内部的 winId() 强行预热，消除初次显示延迟
    ToolTipOverlay::instance();

    resize(1200, 800);
    setMinimumSize(1180, 653); // 物理对齐：5x230px面板 + 20px分割手柄 + 10px全局边距
    setWindowTitle("FERREX");

    // ============================================================
    // 【物理护栏 - 禁止移动】事件过滤器必须在 initUi() 之前创建
    // 原因：initUi() -> initToolbar()/setupCustomTitleBarButtons() 会调用
    //       installEventFilter(m_hoverFilter)，setupCustomTitleBarButtons()
    //       内部还依赖 m_resizeFilter 做全局安装。
    //       若此处移到 initUi() 之后，installEventFilter 会收到 nullptr，
    //       Qt 不会报错也不会崩溃，但功能（hover提示/边缘缩放）会静默失效，
    //       极难排查。2026-06-xx 已踩坑一次。
    // ============================================================
    m_hoverFilter = new HoverEventFilter(this);
    m_resizeFilter = new ResizeEventFilter(this);
    Q_ASSERT(m_hoverFilter && m_resizeFilter);
    // ============================================================

    // 从设置读取置顶状态
    m_isPinned = AppConfig::instance().getValue("MainWindow/AlwaysOnTop", false).toBool();
    
    // 设置基础窗口标志 (保持无边框)
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint);

    // 初始应用置顶 (WinAPI)
    // 2026-03-xx 关键修复：构造函数内不再调用 winId() 或 SetWindowPos 避免触发窗口提前显示
    // 置顶逻辑现在改为按需由 external 或 showEvent 安全触发
    if (m_isPinned) {
#ifdef Q_OS_WIN
        QTimer::singleShot(0, this, [this]() {
            HWND hwnd = reinterpret_cast<HWND>(winId());
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        });
#else
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
#endif
    }

    // 应用全局样式（优先尝试从资源系统加载以支持动态同步）
    // 2026-06-xx 物理修复：如果资源加载失败，则回退到内联样式以确保“物理切割感”永不消失
    QFile file(":/style.qss");
    if (file.open(QFile::ReadOnly)) {
        setStyleSheet(QLatin1String(file.readAll()));
    } else {
        QString qss = QString(R"(
            QMainWindow { background-color: %1; }
            #SidebarContainer, #ListContainer, #EditorContainer, #MetadataContainer, #FilterContainer {
                background-color: %1; border: 1px solid %2; border-radius: 0px;
            }
            #ContainerHeader {
                background-color: %3; border-bottom: 1px solid %2;
            }
            QScrollBar:vertical { border: none; background: transparent; width: 10px; }
            QScrollBar::handle:vertical { background: %2; min-height: 20px; border-radius: 3px; }
            QScrollBar::handle:vertical:hover { background: %4; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { width: 0px; height: 0px; }
            QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
            QScrollBar:horizontal { border: none; background: transparent; height: 10px; }
            QScrollBar::handle:horizontal { background: %2; min-width: 20px; border-radius: 3px; }
            QScrollBar::handle:horizontal:hover { background: %4; }
            QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; height: 0px; }
            QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
            QLineEdit, QPlainTextEdit, QTextEdit {
                background: %1; border: 1px solid %2; border-radius: 6px; color: %5; padding-left: 8px;
            }
            QLineEdit:focus { border: 1px solid %6; }
        )")
        .arg(qssColor(BackgroundDeep))
        .arg(qssColor(BorderColor))
        .arg(qssColor(BackgroundHeader))
        .arg(qssColor(BorderDark))
        .arg(qssColor(TextMain))
        .arg(qssColor(PrimaryBlue));
        setStyleSheet(qss);
    }

    initUi();

    m_trayController = new TrayController(this);
    m_trayController->show();

    // 2026-05-29 性能优化：事件过滤器仅安装在 MainWindow 实例上，减少 qApp 全局事件分发的 overhead。
    this->installEventFilter(this);

    qDebug() << "[Main] MainWindow 构造函数 UI/托盘初始化完成";

    // 2026-03-xx 性能优化：严禁在构造函数中执行任何可能导致阻塞的同步加载 (如 unifiedNavigateTo)。
    // 改为延迟 200ms 触发首次加载，确保 MainWindow 框架先瞬间弹出，提升用户感知的“秒开”响应速度。
    QTimer::singleShot(200, [this]() {
        QString lastPath = AppConfig::instance().getValue("MainWindow/LastPath", "computer://").toString();
        
        // 2026-04-11 按照用户要求：物理还原最后一次开启的内容 (Plan-56)
        // 校验：如果是协议路径或存在的磁盘路径，则载入
        bool isValid = lastPath.contains("://") || QDir(lastPath).exists();
        if (isValid) {
            qDebug() << "[Main] 执行延迟首次加载: 恢复历史状态 ->" << lastPath;
            unifiedNavigateTo(lastPath);
        } else {
            qDebug() << "[Main] 历史路径无效，回退至: 此电脑";
            unifiedNavigateTo("computer://");
        }
    });
}

void MainWindow::initUi() {
    // 物理断言：确保过滤器已就绪，防止静默失效
    Q_ASSERT(m_hoverFilter && m_resizeFilter && "事件过滤器必须在 initUi() 之前创建，见构造函数顶部注释");

    initToolbar();
    setupSplitters();

    // 全局安装：拦截子控件边缘鼠标事件以支持无边框窗口缩放
    QCoreApplication::instance()->installEventFilter(m_resizeFilter);

    setupCustomTitleBarButtons();
    
    // 2026-04-11 按照用户要求：物理锁定侧边栏宽度，最大化时仅“内容”区拉伸
    m_mainSplitter->setStretchFactor(0, 0); // 分类
    m_mainSplitter->setStretchFactor(1, 0); // 目录导航
    m_mainSplitter->setStretchFactor(2, 1); // 内容 (主拉伸区)
    m_mainSplitter->setStretchFactor(3, 0); // 元数据
    m_mainSplitter->setStretchFactor(4, 0); // 筛选

    // 2026-04-11 按照用户要求：物理还原/记忆侧边栏宽度
    QByteArray state = AppConfig::instance().getValue("MainWindow/SplitterState").toByteArray();
    if (!state.isEmpty()) {
        m_mainSplitter->restoreState(state);
        // 2026-07-xx 按照 Plan-63：恢复面板显隐状态 (必须在 restoreState 之后以防布局错乱)
        loadPanelVisibility();
    } else {
        // 初始默认分配: 230 | 230 | 600 | 230 | 230
        QList<int> sizes;
        sizes << 230 << 230 << 600 << 230 << 230;
        m_mainSplitter->setSizes(sizes);
    }

    // 核心红线：建立各面板间的信号联动 (Data Linkage)
    
    // 1. 导航/收藏/内容面板 双击跳转 -> 统一导航中枢 (Plan-56)
    connect(m_navPanel, &NavPanel::directorySelected, this, [this](const QString& path) {
        unifiedNavigateTo(path);
    });

    // 2026-xx-xx 按照 Plan-95：实现收藏文件定位逻辑
    connect(m_navPanel, &NavPanel::requestLocateFile, this, [this](const QString& path) {
        QFileInfo fi(path);
        QString parentDir = fi.absolutePath();
        
        // 1. 设置待定位的文件名 (不开启编辑模式)
        m_contentPanel->setPendingSelectName(fi.fileName(), false);

        // 2. 跳转到父目录
        unifiedNavigateTo(parentDir);
    });

    connect(m_contentPanel, &ContentPanel::directorySelected, this, [this](const QString& path) {
        unifiedNavigateTo(path);
    });

    // 1a. 分类选择 -> 统一导航中枢 (Plan-56)
    connect(m_categoryPanel, &CategoryPanel::categorySelected, this, [this](int id, const QString& name, const QString& type, const QString& path) {
        m_currentCategoryId = id;
        
        // 标签管理模式属于特殊 UI 模式，暂时保持独立
        if (type == "tags") {
            m_navPanel->hide(); m_contentPanel->hide(); m_metaPanel->hide(); m_filterPanel->hide();
            m_tagManagerView->refresh(); m_tagManagerView->show();
            m_isTagManagerMode = true;
            if (m_addressBar) m_addressBar->setPath("标签管理");
            if (m_searchEdit && !m_searchEdit->text().isEmpty()) m_tagManagerView->search(m_searchEdit->text().trimmed());
            return;
        } else if (m_isTagManagerMode) {
            m_tagManagerView->hide(); m_navPanel->show(); m_contentPanel->show(); m_metaPanel->show(); m_filterPanel->show();
            m_isTagManagerMode = false;
        }

        // 构建协议 URL 并跳转
        if (type == "category") {
            unifiedNavigateTo(kProtocolCategory + QString::number(id) + "?name=" + name);
        } else if (type == "bookmark" && !path.isEmpty()) {
            unifiedNavigateTo(path);
        } else if (type == "all" || type == "uncategorized" || type == "untagged" || 
                   type == "recently_visited" || type == "trash" || type == "invalid_data") {
            unifiedNavigateTo(kProtocolSystem + type);
        } else {
            // 回滚：对于未识别的系统项，仅执行搜索展示
            if (m_searchEdit) { m_searchEdit->blockSignals(true); m_searchEdit->clear(); m_searchEdit->blockSignals(false); }
            if (m_addressBar) m_addressBar->setPath("分类: " + name);
            if (!name.isEmpty()) m_contentPanel->search(name);
        }
    });

    // 1b. 内容面板内部跳转分类 (双击同步) -> 统一导航中枢 (Plan-56)
    connect(m_contentPanel, &ContentPanel::categoryClicked, this, [this](int id) {
        // 通过 Repo 获取分类名称以构建完整的协议 URL
        auto all = CategoryRepo::getAll();
        QString name = QString::number(id);
        for(const auto& cat : all) if(cat.id == id) { name = QString::fromStdWString(cat.name); break; }
        unifiedNavigateTo(kProtocolCategory + QString::number(id) + "?name=" + name);
    });

    // 2. 内容面板选中项改变 -> 元数据面板刷新 & 自动预览
    // 2026-03-xx 按照高性能要求，优先从模型 Role 读取元数据缓存，避免频繁磁盘 IO
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_contentPanel, &ContentPanel::selectionChanged, this, [this](const QStringList& paths) {
        m_metaPanel->setSelectedPaths(paths);
        if (paths.isEmpty()) {
            m_metaPanel->updateInfo("-", "-", "-", "-", "-", "-", "-", false);
            m_metaPanel->setRating(0);
            m_metaPanel->setColor(L"");
            m_metaPanel->setPinned(false);
            m_metaPanel->setTags(QStringList());
            m_metaPanel->setNote(L"");
            m_metaPanel->setURL(L"");
            m_metaPanel->setCategory("-");
        } else {
            // 2026-03-xx 高性能优化：优先从模型缓存中读取元数据，避免频繁磁盘访问
            auto indexes = m_contentPanel->getSelectedIndexes();
            if (indexes.isEmpty()) return;
            
            QModelIndex idx = indexes.first();
            QString path = paths.first();
            QFileInfo info(path);
            
            // 基础信息展示
            m_metaPanel->updateInfo(
                info.fileName().isEmpty() ? path : info.fileName(), 
                info.isDir() ? "文件夹" : info.suffix().toUpper() + " 文件",
                info.isDir() ? "-" : QString::number(info.size() / 1024) + " KB",
                info.birthTime().toString("yyyy-MM-dd"),
                info.lastModified().toString("yyyy-MM-dd"),
                info.lastRead().toString("yyyy-MM-dd"),
                info.absoluteFilePath(),
                idx.data(EncryptedRole).toBool()
            );

            // 应用缓存中的元数据状态
            m_metaPanel->setRating(idx.data(RatingRole).toInt());
            m_metaPanel->setColor(idx.data(ColorRole).toString().toStdWString());
            m_metaPanel->setPinned(idx.data(IsLockedRole).toBool());
            m_metaPanel->setTags(idx.data(TagsRole).toStringList());
            
            // 加载备注、URL和色板
            RuntimeMeta rm = MetadataManager::instance().getMeta(path.toStdWString());
            m_metaPanel->setNote(rm.note);
            m_metaPanel->setURL(rm.url);

            // 设置分类显示 (根据当前 UI 状态或路径推导)
            QString category = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
            m_metaPanel->setCategory(category);

            // 将色板数据转换为 QVector<QPair<QColor, float>>
            QVector<QPair<QColor, float>> pal;
            for (const auto& p : rm.palettes) pal.append({p.color, p.ratio});
            m_metaPanel->setPalettes(pal);
        }
        
        // 触发状态栏更新以显示选中状态
        int totalCount = m_contentPanel->getProxyModel()->rowCount();
        onStatusBarStatsUpdated(0, 0, totalCount);
    });

    // 3. 内容面板请求预览 -> QuickLook
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_contentPanel, &ContentPanel::requestQuickLook, this, [this](const QString& path) {
        m_currentQuickLookPath = path;
        QuickLookWindow::instance().previewFile(path);
    });

    // 4. 内容面板统计信息更新 -> 状态栏
    // 2026-05-08 按照用户要求：连接状态栏统计信号
    connect(m_contentPanel, &ContentPanel::statusBarStatsUpdated, this, &MainWindow::onStatusBarStatsUpdated);

    // 2026-04-11 按照用户要求：双向联动，实现预览窗内方向键切图导航
    // 2026-05-27 物理加固：补全 this 上下文
    connect(&QuickLookWindow::instance(), &QuickLookWindow::prevRequested, this, [this]() {
        QString prev = m_contentPanel->getAdjacentFilePath(m_currentQuickLookPath, -1);
        if (!prev.isEmpty()) {
            m_currentQuickLookPath = prev;
            QuickLookWindow::instance().previewFile(prev);
        }
    });

    connect(&QuickLookWindow::instance(), &QuickLookWindow::nextRequested, this, [this]() {
        QString next = m_contentPanel->getAdjacentFilePath(m_currentQuickLookPath, 1);
        if (!next.isEmpty()) {
            m_currentQuickLookPath = next;
            QuickLookWindow::instance().previewFile(next);
        }
    });

    // 4. 元数据变化 -> 同步元数据面板
    connect(&QuickLookWindow::instance(), &QuickLookWindow::ratingRequested, this, [this](int rating) {
        if (m_currentQuickLookPath.isEmpty()) return;

        // 2026-04-11 按照用户要求：补全物理持久化逻辑 (MetadataManager 直接入库)
        MetadataManager::instance().setRating(m_currentQuickLookPath.toStdWString(), rating);

        m_metaPanel->setRating(rating);
        // 2026-04-11 按照用户要求：在预览窗设定星级时，左上方即时反馈
        QString msg = QString("已设定星级: <span style='color: #FECF0E;'>%1 星</span>").arg(rating);
        ToolTipOverlay::instance()->showText(QPoint(50, 50), msg, 1500, QColor("#FECF0E"));
    });

    connect(&QuickLookWindow::instance(), &QuickLookWindow::colorRequested, this, [this](const QString& color) {
        if (m_currentQuickLookPath.isEmpty()) return;

        // 2026-04-11 按照用户要求：补全物理持久化逻辑 (MetadataManager 直接入库)
        MetadataManager::instance().setColor(m_currentQuickLookPath.toStdWString(), color.toStdWString());

        m_metaPanel->setColor(color.toStdWString());
        
        QString colorName = "无颜色";
        if (color == "red") colorName = "红色";
        else if (color == "orange") colorName = "橙色";
        else if (color == "yellow") colorName = "黄色";
        else if (color == "green") colorName = "绿色";
        else if (color == "cyan") colorName = "青色";
        else if (color == "blue") colorName = "蓝色";
        else if (color == "purple") colorName = "紫色";
        else if (color == "gray") colorName = "灰色";

        QString msg = QString("已设定颜色: <span style='color: #41F2F2;'>%1</span>").arg(colorName);
        ToolTipOverlay::instance()->showText(QPoint(50, 50), msg, 1500, QColor("#41F2F2"));
    });

    // 5a. 目录装载完成 -> FilterPanel 动态填充 (六参数版本: 移除标签统计)
    connect(m_contentPanel, &ContentPanel::directoryStatsReady, this,
        [this](const QMap<int,int>& r, const QMap<QString,int>& c,
               const QMap<QString,int>& tp,
               const QMap<QString,int>& cd, const QMap<QString,int>& md,
               int ef) {
            m_filterPanel->populate(r, c, tp, cd, md, ef);
        });

    // 5b. FilterPanel 状态变化 -> 内容面板过滤 (Plan-92: 统一搜索词合并)
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_filterPanel, &FilterPanel::filterChanged, this, [this](const FilterState& state) {
        FilterState mergedState = state;
        if (m_searchEdit) {
            mergedState.keyword = m_searchEdit->text().trimmed();
        }
        m_contentPanel->applyFilters(mergedState);
        updateStatusBar(); // 筛选后立即更新底栏可见项目总数
    });


    // 6. 地址栏路径跳转与刷新 -> 统一导航中枢 (Plan-56)
    connect(m_addressBar, &AddressBar::pathChanged, this, [this](const QString& path) {
        unifiedNavigateTo(path);
    });

    connect(m_addressBar, &AddressBar::refreshRequested, this, [this]() {
        if (m_contentPanel) m_contentPanel->refreshAll();
    });

    // 7. 搜索框回车触发逻辑 (2026-07-xx 按照 Plan-57 升级为异步流式展示)
    m_searchHistory = AppConfig::instance().getValue("Search/History").toStringList();
    
    m_searchHistoryPanel = new SearchHistoryPanel(this);
    m_searchHistoryPanel->setHistory(m_searchHistory);

    // 2026-xx-xx 按照 Plan-106：初始化搜索防抖计时器
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(300);

    // 回车搜索核心逻辑 (2026-07-xx 定点修复：搜索框作为筛选器的一个输入组件，走本地过滤引擎)
    auto doSearch = [this](const QString& keyword) {
        if (m_isTagManagerMode) {
            m_tagManagerView->search(keyword);
            return;
        }

        // 2026-07-xx 按照 Plan-118：搜索行为回归筛选流。
        // 搜索框作为当前视图的本地过滤器，不再执行外部 fs 拼装，直接驱动 ContentPanel。
        m_contentPanel->search(keyword);
        updateStatusBar();

        // 维护历史记录
        if (!keyword.isEmpty()) {
            m_searchHistory.removeAll(keyword);
            m_searchHistory.prepend(keyword);
            if (m_searchHistory.size() > 10) m_searchHistory.removeLast();
            AppConfig::instance().setValue("Search/History", m_searchHistory);
            m_searchHistoryPanel->setHistory(m_searchHistory);
        }
        m_searchHistoryPanel->hide();
    };

    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this, doSearch]() {
        doSearch(m_searchEdit->text().trimmed());
    });

    // 2026-04-xx 按照用户要求：支持标签管理模式下的实时搜索
    // 2026-xx-xx 按照 Plan-106：防抖处理
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this, doSearch](const QString& text) {
        if (text.isEmpty()) {
            m_searchTimer->stop();
            doSearch(""); // 清空时立即响应
            return;
        }
        m_searchTimer->start();
    });

    connect(m_searchTimer, &QTimer::timeout, this, [this, doSearch]() {
        doSearch(m_searchEdit->text().trimmed());
    });
    
    m_searchEdit->installEventFilter(this); // 拦截 FocusIn 事件展示历史面板

    // 历史面板信号对接
    connect(m_searchHistoryPanel, &SearchHistoryPanel::historyItemClicked, this, [this, doSearch](const QString& keyword) {
        m_searchEdit->setText(keyword);
        doSearch(keyword);
    });

    connect(m_searchHistoryPanel, &SearchHistoryPanel::historyItemRemoved, this, [this](const QString& keyword) {
        m_searchHistory.removeAll(keyword);
        AppConfig::instance().setValue("Search/History", m_searchHistory);
        m_searchHistoryPanel->setHistory(m_searchHistory);
    });

    connect(m_searchHistoryPanel, &SearchHistoryPanel::clearAllRequested, this, [this]() {
        m_searchHistory.clear();
        AppConfig::instance().setValue("Search/History", m_searchHistory);
        m_searchHistoryPanel->setHistory(m_searchHistory);
    });

    // 2026-06-xx 物理清理：移除 prefetchDirectory 调用。中心化缓存已在启动时加载，无需手动预取。

    // 8. 响应元数据面板自己的星级/颜色变更
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_metaPanel, &MetaPanel::metadataChanged, this, [this](int rating, const std::wstring& color) {
        auto indexes = m_contentPanel->getSelectedIndexes();
        for (const auto& idx : indexes) {
            QString path = idx.data(PathRole).toString(); 
            if(path.isEmpty()) continue;
            
            if (rating != -1) {
                // 2026-05-24 按照用户要求：彻底移除 SCCH，改为中心化异步持久化
                MetadataManager::instance().setRating(path.toStdWString(), rating);
            }
            if (color != L"__NO_CHANGE__") {
                MetadataManager::instance().setColor(path.toStdWString(), color);
            }
        }
    });

    // 2026-07-xx 按照用户要求：响应元数据面板标签批量变更信号
    connect(m_metaPanel, &MetaPanel::tagsChanged, this, [this](const QStringList& tags) {
        auto indexes = m_contentPanel->getSelectedIndexes();
        for (const auto& idx : indexes) {
            QString path = idx.data(PathRole).toString();
            if (path.isEmpty()) continue;
            
            std::wstring wPath = path.toStdWString();
            
            MetadataManager::instance().setTags(wPath, tags);
        }
    });

    // 2026-06-xx调色盘搜索联动：将颜色喂给筛选器，由筛选器驱动过滤
    connect(m_metaPanel, &MetaPanel::searchByColor, this, [this](const QColor& color) {
        if (m_filterPanel) {
            m_filterPanel->selectColor(color);
        }
    });

    // 9. 2026-03-xx 按照用户要求：响应元数据全局变更，同步刷新 UI
    
    // 9a. 全局内容区同步：只要元数据变了，通知内容面板刷新对应的行
    connect(&MetadataManager::instance(), &MetadataManager::metaChanged, this, [this](const QString& path) {
        if (path == "__RELOAD_ALL__") {
            m_contentPanel->refreshAll();
            return;
        }
        // 关键修复：通知 ContentPanel 局部更新该路径的数据
        m_contentPanel->updateItemMetadata(path);

        // 2026-06-xx 物理修复：实时刷新联动
        // 当元数据（如颜色、备注、标签）发生变更时，如果当前选中项正好是该文件，
        // 则强制触发元数据面板刷新，确保 UI 与后台数据物理一致。
        auto indexes = m_contentPanel->getSelectedIndexes();
        if (!indexes.isEmpty()) {
            if (indexes.first().data(PathRole).toString() == path) {
                emit m_contentPanel->selectionChanged({path});
            }
        }
    });

    // 9b. 侧边栏刷新防抖
    m_sidebarRefreshTimer = new QTimer(this);
    m_sidebarRefreshTimer->setInterval(800);
    m_sidebarRefreshTimer->setSingleShot(true);

    connect(&MetadataManager::instance(), &MetadataManager::metaChanged, this, [this](const QString& path) {
        if (!m_categoryPanel) return;

        // __RELOAD_ALL__ 信号立即刷新，其他信号防抖
        if (path == "__RELOAD_ALL__") {
            m_categoryPanel->requestRefresh(true);
        } else {
            m_sidebarRefreshTimer->start(); // 重置计时器
        }
    });

    connect(m_sidebarRefreshTimer, &QTimer::timeout, this, [this]() {
        if (m_categoryPanel) m_categoryPanel->requestRefresh();
    });

    // 10. 侧边栏点击物理项（文件预览或文件夹跳转）
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryPanel, &CategoryPanel::fileSelected, this, [this](const QString& path) {
        QFileInfo fi(path);
        if (fi.isDir()) {
            // 如果是文件夹，执行界面跳转联动
            unifiedNavigateTo(path);
        } else {
            // 2026-03-xx 按照用户要求：侧边栏选中任何物理文件，立即执行即时全能预览
            m_contentPanel->previewFile(path);
        }
    });
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    Q_UNUSED(eventType);
    Q_UNUSED(result);

    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_DEVICECHANGE) {
        // [Plan-131 方案 E] 职责剥离：UI 仅转发硬件消息，不处理逻辑
        CoreController::instance().handleDeviceChange(static_cast<unsigned long>(msg->wParam), static_cast<unsigned long long>(msg->lParam));
    }
    return false;
}
#endif

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // 2026-04-12 关键修复：延迟初始化面板数据（确保窗口先渲染，避免主线程卡死导致无法显示）
    qDebug() << "[Main] showEvent 触发, m_panelsInitialized =" << m_panelsInitialized;
    if (!m_panelsInitialized) {
        m_panelsInitialized = true;
        qint64 scheduleStart = QDateTime::currentMSecsSinceEpoch();
        qDebug() << "[Main] 正在排期延迟加载任务 (QTimer::singleShot(0))...";
        QTimer::singleShot(0, [this, scheduleStart]() {
            qint64 taskStart = QDateTime::currentMSecsSinceEpoch();
            qDebug() << "[Main] 延迟加载任务开始执行，排期等待耗时:" << (taskStart - scheduleStart) << "ms";
            
            if (m_categoryPanel) {
                qint64 start = QDateTime::currentMSecsSinceEpoch();
                m_categoryPanel->deferredInit();
                qDebug() << "[PERF] CategoryPanel 初始化耗时:" << (QDateTime::currentMSecsSinceEpoch() - start) << "ms";
            }
            if (m_navPanel) {
                qint64 start = QDateTime::currentMSecsSinceEpoch();
                m_navPanel->deferredInit();
                qDebug() << "[PERF] NavPanel 初始化耗时:" << (QDateTime::currentMSecsSinceEpoch() - start) << "ms";
            }
            if (m_contentPanel) {
                qint64 start = QDateTime::currentMSecsSinceEpoch();
                m_contentPanel->deferredInit();
                qDebug() << "[PERF] ContentPanel 初始化耗时:" << (QDateTime::currentMSecsSinceEpoch() - start) << "ms";
            }
            // MetaPanel 和 FilterPanel 暂时不需要延迟数据加载，因为它们通常随选中项动态刷新
            
            // 2026-04-14 按照用户要求：物理禁用"最后一个窗口关闭时退出"逻辑
            // 确保程序只能通过托盘菜单显式退出，提高驻留稳定性
            qDebug() << "[PERF] 所有核心面板数据延迟加载完成，总耗时:" << (QDateTime::currentMSecsSinceEpoch() - taskStart) << "ms";
        });
    }
    
    // 2026-04-11 按照用户要求：此处是执行 ToolTipOverlay 真实 GPU 预热的唯一合法时机。
    // 只有当 MainWindow 的原生窗口句柄（HWND）被 Windows 完整创建后，ToolTipOverlay 的
    // show() + hide() 序列才能真正触发 DWM 桌面合成器分配 GPU 内存驻留资源。
    // 在构造函数中执行该操作毫无意义，因为此时 MainWindow 本身尚未拥有有效句柄。
    // 使用 singleShot(0) 延迟到下一个事件循环帧，确保当前帧窗口绘制不受打扰。
    static bool s_warmedUp = false;
    if (!s_warmedUp) {
        s_warmedUp = true;
        QTimer::singleShot(0, []() {
            auto* tip = ToolTipOverlay::instance();
            // 闪烁显示再立即隐藏，令 DWM 将其纹理资源常驻 GPU 显存
            tip->show();
            tip->hide();
        });
    }
}

void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;

    const QPoint localPos = event->position().toPoint();
    ResizeDirection dir = getResizeDirection(localPos);

    if (dir != None) {
        // 2026-05-08 按照用户要求：进入 Resize 模式
        m_isResizing = true;
        m_isDragging = false;
        m_resizeDir = dir;
        m_resizeStartGlobal   = event->globalPosition().toPoint();
        m_resizeStartGeometry = geometry();
        event->accept();
        return;
    }

    // 原有拖动逻辑：仅标题栏区域
    if (localPos.y() <= 34) {
        m_isDragging = true;
        m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
    }
}


void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_isResizing) {
        const QPoint delta = event->globalPosition().toPoint() - m_resizeStartGlobal;
        QRect r = m_resizeStartGeometry;

        if (m_resizeDir == Left || m_resizeDir == TopLeft || m_resizeDir == BottomLeft)
            r.setLeft(r.left() + delta.x());
        if (m_resizeDir == Right || m_resizeDir == TopRight || m_resizeDir == BottomRight)
            r.setRight(r.right() + delta.x());
        if (m_resizeDir == Top || m_resizeDir == TopLeft || m_resizeDir == TopRight)
            r.setTop(r.top() + delta.y());
        if (m_resizeDir == Bottom || m_resizeDir == BottomLeft || m_resizeDir == BottomRight)
            r.setBottom(r.bottom() + delta.y());

        // 尊重最小尺寸约束
        if (r.width() >= minimumWidth() && r.height() >= minimumHeight())
            setGeometry(r);

        event->accept();
        return;
    }

    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragPosition);
        event->accept();
        return;
    }

    // 2026-05-08 按照用户要求：悬停时动态更新光标（未按下状态）
    if (!m_isDragging) {
        updateCursorShape(getResizeDirection(event->position().toPoint()));
    }
}

// 2026-05-08 按照用户要求：实现边缘resize方向检测函数
MainWindow::ResizeDirection MainWindow::getResizeDirection(const QPoint& pos) const {
    // 按照用户建议：将感应宽度改为根据 DPI 动态计算
    int m = kResizeMargin;
    if (windowHandle()) {
        m = qRound(screen()->logicalDotsPerInch() / 96.0 * (double)kResizeMargin);
    }
    const int w = width(), h = height();
    bool left   = pos.x() < m;
    bool right  = pos.x() > w - m;
    bool top    = pos.y() < m;
    bool bottom = pos.y() > h - m;

    if (top    && left)  return TopLeft;
    if (top    && right) return TopRight;
    if (bottom && left)  return BottomLeft;
    if (bottom && right) return BottomRight;
    if (left)   return Left;
    if (right)  return Right;
    if (top)    return Top;
    if (bottom) return Bottom;
    return None;
}

// 2026-05-08 按照用户要求：实现光标形状更新函数
void MainWindow::updateCursorShape(ResizeDirection dir) {
    switch (dir) {
        case Left:        case Right:       setCursor(Qt::SizeHorCursor);  break;
        case Top:         case Bottom:      setCursor(Qt::SizeVerCursor);  break;
        case TopLeft:     case BottomRight: setCursor(Qt::SizeFDiagCursor); break;
        case TopRight:    case BottomLeft:  setCursor(Qt::SizeBDiagCursor); break;
        default:                            setCursor(Qt::ArrowCursor);    break;
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    m_isDragging  = false;
    m_isResizing  = false;
    m_resizeDir   = None;
    setCursor(Qt::ArrowCursor);
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    // 0. F5: 刷新当前目录
    if (event->key() == Qt::Key_F5) {
        if (m_contentPanel) m_contentPanel->refreshAll();
        event->accept();
        return;
    }

    // 0.1 Ctrl+Z / Ctrl+Shift+Z: 撤销与重做
    if (event->key() == Qt::Key_Z && (event->modifiers() & Qt::ControlModifier)) {
        if (event->modifiers() & Qt::ShiftModifier) {
            UndoManager::instance().redo();
        } else {
            UndoManager::instance().undo();
        }
        event->accept();
        return;
    }

    // 1. Alt+Q: 切换窗口置顶状态
    if (event->key() == Qt::Key_Q && (event->modifiers() & Qt::AltModifier)) {
        m_btnPinTop->setChecked(!m_btnPinTop->isChecked());
        event->accept();
        return;
    }

    // 2026-05-20 极致性能：MainWindow 自身也需支持悬停识别，确保自定义标题栏操作灵敏
    setAttribute(Qt::WA_Hover);

    // 2. Ctrl+F: 聚焦搜索过滤框
    if (event->key() == Qt::Key_F && (event->modifiers() & Qt::ControlModifier)) {
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

// 2026-03-xx 按照用户要求：物理拦截事件以实现自定义 ToolTipOverlay 的显隐控制
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // 2026-06-xx 物理修复：双击搜索框时弹出历史记录
    if (event->type() == QEvent::MouseButtonDblClick && watched == m_searchEdit) {
        if (!m_searchHistory.isEmpty()) {
            m_searchHistoryPanel->showBelow(m_searchEdit);
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::initToolbar() {
    auto createBtn = [this](const QString& iconKey, const QString& tip) {
        QPushButton* btn = new QPushButton(this);
        btn->setAttribute(Qt::WA_Hover); // 2026-05-20 性能优化：必须开启 Hover 属性以触发悬停事件
        btn->setFixedSize(32, 28); // 极致精简宽度
        
        QIcon icon = UiHelper::getIcon(iconKey, QColor("#EEEEEE"));
        btn->setIcon(icon);
        btn->setIconSize(QSize(18, 18));
        
        // 2026-03-xx 按照宪法要求：禁绝原生 ToolTip，强制对接 ToolTipOverlay
        btn->setProperty("tooltipText", tip);
        btn->installEventFilter(this);

        // 极致精简样式：无边框，仅悬停可见背景
        // 按照要求：杜绝 rgba 蒙版，普通按钮使用 #3E3E42
        btn->setStyleSheet(
            "QPushButton { background: transparent; border: none; border-radius: 4px; }"
            "QPushButton:hover { background: #3E3E42; }"
            "QPushButton:pressed { background: #4E4E52; }"
            "QPushButton:disabled { opacity: 0.3; }"
        );
        return btn;
    };

    m_btnBack = createBtn("nav_prev", "");
    m_btnBack->setProperty("tooltipText", "后退");
    m_btnBack->installEventFilter(m_hoverFilter);

    m_btnForward = createBtn("nav_next", "");
    m_btnForward->setProperty("tooltipText", "前进");
    m_btnForward->installEventFilter(m_hoverFilter);

    m_btnUp = createBtn("arrow_up", "");
    m_btnUp->setProperty("tooltipText", "上级");
    m_btnUp->installEventFilter(m_hoverFilter);

    connect(m_btnBack, &QPushButton::clicked, this, &MainWindow::onBackClicked);
    connect(m_btnForward, &QPushButton::clicked, this, &MainWindow::onForwardClicked);
    connect(m_btnUp, &QPushButton::clicked, this, &MainWindow::onUpClicked);

    // --- 路径地址栏重构 (复合 AddressBar) ---
    m_addressBar = new AddressBar(this);
    m_addressBar->setMinimumWidth(300); // 2026-06-xx 物理红线：地址栏最小宽度，防止被搜索框挤压

    // 2026-04-12 按照用户要求：搜索框容器（搜索框 + 模式切换按钮）
    m_searchContainer = new QWidget(this);
    m_searchContainer->setStyleSheet("background: transparent;");
    QHBoxLayout* searchLayout = new QHBoxLayout(m_searchContainer);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(0);

    m_searchEdit = new QLineEdit(m_searchContainer);
    m_searchEdit->setPlaceholderText("搜索...");
    // 2026-06-xx 物理红线：强制锁定 230 像素，禁止脑补拉伸
    m_searchEdit->setFixedSize(230, 32);
    m_searchEdit->addAction(UiHelper::getIcon("search", TextMuted), QLineEdit::LeadingPosition);
    
    // 2026-xx-xx 工业级拨乱反正：废除手动 Action 模拟，回归原生清除按钮以确保项目视觉一致性
    m_searchEdit->setClearButtonEnabled(true);

    m_searchEdit->setStyleSheet(QString(
        "QLineEdit { background: %1; border: 1px solid %2;"
        "  border-radius: 6px;"
        "  color: %3; padding-left: 5px; padding-right: 5px; }"
        "QLineEdit:focus { border: 1px solid %4; }"
    ).arg(qssColor(BackgroundDeep)).arg(qssColor(BorderColor)).arg(qssColor(TextMain)).arg(qssColor(PrimaryBlue)));

    searchLayout->addWidget(m_searchEdit);
}



void MainWindow::setupSplitters() {
    QWidget* centralC = new QWidget(this);
    centralC->setObjectName("CentralWidget");
    centralC->setStyleSheet("#CentralWidget { background-color: #1E1E1E; }"); 
    QVBoxLayout* mainL = new QVBoxLayout(centralC);
    mainL->setContentsMargins(0, 0, 0, 0); 
    mainL->setSpacing(0); 

    // --- 1. 自定义标题栏 (第一行) ---
    m_titleBarWidget = new QWidget(centralC);
    m_titleBarWidget->setObjectName("TitleBar");
    // 2026-xx-xx 按照用户要求：添加 1px 底部切割线，与全局规范对齐
    m_titleBarWidget->setStyleSheet(QString("QWidget#TitleBar { border: none; border-bottom: 1px solid %1; background: transparent; }").arg(qssColor(BorderColor)));
    m_titleBarWidget->setFixedHeight(34);
    m_titleBarLayout = new QHBoxLayout(m_titleBarWidget);
    // 2026-xx-xx 按照用户要求：标题栏左侧与右侧均保持 5px 呼吸边距
    m_titleBarLayout->setContentsMargins(5, 0, kEdgeMargin, 0); 
    m_titleBarLayout->setSpacing(8);

    m_logoLabel = new QLabel(m_titleBarWidget);
    m_logoLabel->setFixedSize(18, 18);
    m_logoLabel->setPixmap(UiHelper::getIcon("ferrex", BrandOrange).pixmap(16, 16));
    m_logoLabel->setAlignment(Qt::AlignCenter);
    m_logoLabel->setStyleSheet("background: transparent; border: none;");
    m_titleBarLayout->addWidget(m_logoLabel);

    m_appNameLabel = new QLabel("FERREX", m_titleBarWidget);
    m_appNameLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: bold;").arg(BrandOrange.name()));
    m_titleBarLayout->addWidget(m_appNameLabel);
    m_titleBarLayout->addStretch();

    // --- 2. 统一导航栏 (第二行) ---
    m_navBarWidget = new QWidget(centralC);
    m_navBarWidget->setObjectName("NavBar");
    m_navBarWidget->setStyleSheet("QWidget#NavBar { border: none; background: transparent; }");
    m_navBarWidget->setFixedHeight(42); 
    
    m_navBarLayout = new QHBoxLayout(m_navBarWidget);
    // 右边距使用 kEdgeMargin，与 navBar/body 保持统一基准线，禁止改为0
    m_navBarLayout->setContentsMargins(kEdgeMargin, kEdgeMargin, kEdgeMargin, kEdgeMargin); 
    m_navBarLayout->setSpacing(5);
    m_navBarLayout->setAlignment(Qt::AlignVCenter);

    m_navBarLayout->addWidget(m_btnBack);
    m_navBarLayout->addWidget(m_btnForward);
    m_navBarLayout->addWidget(m_btnUp);
    m_navBarLayout->addWidget(m_addressBar, 1);
    // 2026-06-xx 物理对标：移除额外 addSpacing，直接依赖 layout 默认 5px spacing 达到精准 5 像素间距
    m_navBarLayout->addWidget(m_searchContainer);

    // --- 3. 主体核心容器 (物理还原：10px 全局边距包裹，确保边缘resize可用) ---
    QWidget* bodyWrapper = new QWidget(centralC);
    bodyWrapper->setStyleSheet("background: transparent;"); // 确保背景透明不遮挡阴影
    m_bodyLayout = new QVBoxLayout(bodyWrapper);
    // 必须与 setupSplitters() 中的初始值保持一致，禁止改为0，否则筛选面板/元数据面板右侧会贴边截断
    m_bodyLayout->setContentsMargins(kEdgeMargin, 0, kEdgeMargin, kEdgeMargin); 
    m_bodyLayout->setSpacing(0);

    // --- 3. 主拆分条 (物理还原：5px 物理缝隙) ---
    m_mainSplitter = new QSplitter(Qt::Horizontal, bodyWrapper);
    m_mainSplitter->setHandleWidth(5); 
    m_mainSplitter->setChildrenCollapsible(false);
    // 物理还原：显式设置手柄样式，增强物理切割感
    // 2026-06-xx 物理强化：手柄背景设为 #1E1E1E，并在两侧增加深色线条，强化“切割”视觉效果
    m_mainSplitter->setStyleSheet(QString(
        "QSplitter { background: transparent; border: none; }"
        "QSplitter::handle { background-color: %1; width: 5px; }"
        "QSplitter::handle:hover { background-color: %2; }" 
    ).arg(qssColor(BackgroundDeep)).arg(qssColor(BackgroundHover)));

    m_categoryPanel = new CategoryPanel(this);
    m_categoryPanel->setObjectName("SidebarContainer");
    
    m_navPanel = new NavPanel(this);
    m_navPanel->setObjectName("ListContainer");
    
    m_contentPanel = new ContentPanel(this);
    m_contentPanel->setObjectName("EditorContainer");
    
    m_metaPanel = new MetaPanel(this);
    m_metaPanel->setObjectName("MetadataContainer");
    
    m_filterPanel = new FilterPanel(this);
    m_filterPanel->setObjectName("FilterContainer");

    m_tagManagerView = new TagManagerView(this);
    m_tagManagerView->hide();

    m_invalidDataListView = new InvalidDataListView(this);
    m_invalidDataListView->hide();

    // 2026-05-07 按照用户要求：焦点线持久化显示，基于数据来源而非焦点位置
    connect(m_contentPanel, &ContentPanel::dataSourceChanged, this, [this](const QString& source) {
        m_currentDataSource = source;
        // 重置所有面板高亮
        if (m_navPanel)      m_navPanel->setFocusHighlight(false);
        if (m_categoryPanel) m_categoryPanel->setFocusHighlight(false);

        // 根据数据来源显示焦点线
        if (source == "category") {
            if (m_categoryPanel) m_categoryPanel->setFocusHighlight(true);
        } else if (source == "nav") {
            if (m_navPanel) m_navPanel->setFocusHighlight(true);
        }
        // 其他来源（搜索、筛选等）不显示焦点线
    });

    m_mainSplitter->addWidget(m_categoryPanel);
    m_mainSplitter->addWidget(m_navPanel);
    m_mainSplitter->addWidget(m_contentPanel);
    m_mainSplitter->addWidget(m_metaPanel);
    m_mainSplitter->addWidget(m_filterPanel);
    m_mainSplitter->addWidget(m_tagManagerView);
    m_mainSplitter->addWidget(m_invalidDataListView);


    // 2026-07-xx 按照用户要求：标签搜索联动
    connect(m_tagManagerView, &TagManagerView::requestSearchTag, this, [this](const QString& tag) {
        // 自动切回正常模式并搜索
        if (m_categoryPanel) m_categoryPanel->selectCategory(-1); // 选中“全部数据”
        if (m_searchEdit) m_searchEdit->setText(tag);
        
        // 标签跳转默认作为全局搜索处理 (不限范围)
        QStringList paths = MetadataManager::instance().searchInCache(tag);
        m_contentPanel->loadPaths(paths);
    });

    m_bodyLayout->addWidget(m_mainSplitter);

    // --- 4. 底部状态栏 (0 边距) ---
    QWidget* statusBar = new QWidget(centralC);
    statusBar->setObjectName("StatusBar");
    statusBar->setFixedHeight(28);
    QHBoxLayout* statusL = new QHBoxLayout(statusBar);
    statusL->setContentsMargins(kStatusBarMargin, 0, kStatusBarMargin, 0);
    statusL->setSpacing(0);

    m_statusLeft = new QLabel("就绪中...", statusBar);
    m_statusLeft->setStyleSheet(QString("font-size: 11px; color: %1; background: transparent;").arg(qssColor(TextDim)));

    statusL->addWidget(m_statusLeft);
    statusL->addStretch(1);

    // 绑定 CoreController 状态到状态栏
    auto updateStatus = [this]() {
        m_statusLeft->setText(CoreController::instance().statusText());
        if (CoreController::instance().isIndexing()) {
            m_statusLeft->setStyleSheet(QString("font-size: 11px; color: %1; background: transparent; font-weight: bold;")
                                      .arg(qssColor(PrimaryBlue)));
        } else {
            m_statusLeft->setStyleSheet(QString("font-size: 11px; color: %1; background: transparent;")
                                      .arg(qssColor(TextDim)));
        }
    };
    connect(&CoreController::instance(), &CoreController::statusTextChanged, this, updateStatus);
    connect(&CoreController::instance(), &CoreController::isIndexingChanged, this, updateStatus);
    updateStatus();

    initDriveBar();

    mainL->addWidget(m_titleBarWidget);
    mainL->addWidget(m_driveBarWidget);
    mainL->addWidget(m_navBarWidget);
    mainL->addWidget(bodyWrapper, 1);
    mainL->addWidget(statusBar);

    setCentralWidget(centralC);
}

/**
 * @brief 实现符合 funcBtnStyle 规范的自定义按钮组
 */
void MainWindow::setupCustomTitleBarButtons() {
    QWidget* titleBarBtns = new QWidget(this);
    QHBoxLayout* layout = new QHBoxLayout(titleBarBtns);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5); // 2026-xx-xx 按照用户要求：按钮高亮间距统一为 5px

    auto createTitleBtn = [this](const QString& iconKey, const QString& hoverColor = "#3E3E42") {
        QPushButton* btn = new QPushButton(this);
        btn->setAttribute(Qt::WA_Hover); // 2026-05-20 性能优化：必须开启 Hover 属性以触发悬停事件
        btn->setFixedSize(24, 24); // 固定 24x24px
        
        // 使用 UiHelper 全局辅助类
        QIcon icon = UiHelper::getIcon(iconKey, QColor("#EEEEEE"));
        btn->setIcon(icon);
        btn->setIconSize(QSize(18, 18));
        
        btn->setStyleSheet(QString(
            "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 0; }"
            "QPushButton:hover { background: %1; }"
            "QPushButton:pressed { background: #4E4E52; }"
        ).arg(hoverColor));
        return btn;
    };

    m_btnToggleDriveBar = createTitleBtn("chevrons_down");
    m_btnToggleDriveBar->setProperty("tooltipText", "展开/收起盘符管理栏");
    m_btnToggleDriveBar->installEventFilter(m_hoverFilter);
    m_btnToggleDriveBar->setCheckable(true);
    m_btnToggleDriveBar->setChecked(true);
    connect(m_btnToggleDriveBar, &QPushButton::toggled, this, [this](bool checked) {
        if (m_driveBarWidget) {
            m_driveBarWidget->setVisible(checked);
            m_btnToggleDriveBar->setIcon(UiHelper::getIcon(checked ? "chevrons_down" : "chevrons_up", QColor("#EEEEEE")));
        }
    });

    m_btnSync = createTitleBtn("sync");
    m_btnSync->setProperty("tooltipText", "元数据已同步至物理文件");
    m_btnSync->installEventFilter(m_hoverFilter);

    // 2026-07-xx 按照用户要求 (Plan-121)：通过试点服务解耦同步状态展示
    connect(&SyncStatusService::instance(), &SyncStatusService::statusUpdated, this, [this](bool syncing, int count) {
        if (syncing) {
            m_btnSync->setIcon(UiHelper::getIcon("sync", ErrorRed)); // 强制红色
            m_btnSync->setProperty("tooltipText", QString("正在同步元数据 (%1 项待落盘)...").arg(count));
        } else {
            m_btnSync->setIcon(UiHelper::getIcon("sync", TextMain)); // 恢复正常
            m_btnSync->setProperty("tooltipText", "元数据已同步至物理文件");
        }
    });

    // 2026-06-15 按照用户要求：手动点击同步 (仅作交互反馈)
    connect(m_btnSync, &QPushButton::clicked, this, [this]() {
        if (SyncStatusService::instance().isSyncing()) {
            ToolTipOverlay::instance()->showText(m_btnSync->mapToGlobal(QPoint(0,0)), "同步正在进行中...", 1500);
        } else {
            ToolTipOverlay::instance()->showText(m_btnSync->mapToGlobal(QPoint(0,0)), "元数据已全部落地", 1500);
        }
    });

    m_btnLayout = createTitleBtn("layout");
    m_btnLayout->setProperty("tooltipText", "布局管理与重置");
    m_btnLayout->installEventFilter(m_hoverFilter);
    connect(m_btnLayout, &QPushButton::clicked, this, [this]() {
        QMenu menu(this);
        UiHelper::applyMenuStyle(&menu);
        populatePanelMenu(&menu);
        menu.exec(m_btnLayout->mapToGlobal(QPoint(0, m_btnLayout->height())));
    });

    m_btnCreate = createTitleBtn("add"); // 2026-03-xx 规范化：“+”按钮图标修正
    m_btnCreate->setProperty("tooltipText", "新建...");
    QMenu* createMenu = new QMenu(m_btnCreate);
    UiHelper::applyMenuStyle(createMenu);
    
    QAction* actNewFolder = createMenu->addAction(UiHelper::getIcon("folder_filled", QColor("#EEEEEE")), "创建文件夹");
    QAction* actNewMd     = createMenu->addAction(UiHelper::getIcon("text", QColor("#EEEEEE")), "创建 Markdown");
    QAction* actNewTxt    = createMenu->addAction(UiHelper::getIcon("text", QColor("#EEEEEE")), "创建纯文本文件 (txt)");
    
    // 2026-03-xx 按照用户要求修正居中对齐：
    // 不再使用 setMenu，避免按钮进入“菜单模式”从而为指示器预留空间导致图标偏左。
    // 采用手动 popup 方式展示菜单。
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_btnCreate, &QPushButton::clicked, this, [this, createMenu]() {
        createMenu->popup(m_btnCreate->mapToGlobal(QPoint(0, m_btnCreate->height())));
    });

    auto handleCreate = [this](const QString& type) {
        m_contentPanel->createNewItem(type);
    };
    // 2026-05-27 物理加固：补全 this 上下文
    connect(actNewFolder, &QAction::triggered, this, [handleCreate](){ handleCreate("folder"); });
    connect(actNewMd,     &QAction::triggered, this, [handleCreate](){ handleCreate("md"); });
    connect(actNewTxt,    &QAction::triggered, this, [handleCreate](){ handleCreate("txt"); });

    m_btnPinTop = createTitleBtn(m_isPinned ? "pin_vertical" : "pin_tilted");
    m_btnPinTop->setProperty("tooltipText", "置顶窗口");
    m_btnPinTop->installEventFilter(m_hoverFilter);
    m_btnPinTop->setCheckable(true);
    m_btnPinTop->setChecked(m_isPinned);
    if (m_isPinned) {
        m_btnPinTop->setIcon(UiHelper::getIcon("pin_vertical", Style::ActiveOrange));
    }

    m_btnMin = createTitleBtn("minimize");
    m_btnMin->setProperty("tooltipText", "最小化");
    m_btnMin->installEventFilter(m_hoverFilter);

    m_btnMax = createTitleBtn(isMaximized() ? "restore_line" : "maximize");
    m_btnMax->setProperty("tooltipText", "最大化/还原");
    m_btnMax->installEventFilter(m_hoverFilter);

    m_btnClose = createTitleBtn("close", qssColor(ErrorRed)); // 初始创建
    // 按照用户要求：关闭按钮持续显示红色高亮，不再仅悬停显示
    m_btnClose->setStyleSheet(QString(
        "QPushButton { background-color: %1; border: none; border-radius: 4px; padding: 0; }"
        "QPushButton:hover { background-color: %1; }"
        "QPushButton:pressed { background-color: #A50000; }"
    ).arg(qssColor(ErrorRed)));
    m_btnClose->setProperty("tooltipText", "关闭项目");
    m_btnClose->installEventFilter(m_hoverFilter);

    m_btnCreate->installEventFilter(m_hoverFilter);
    layout->addWidget(m_btnToggleDriveBar, 0, Qt::AlignVCenter);
    layout->addWidget(m_btnSync, 0, Qt::AlignVCenter);
    layout->addWidget(m_btnLayout, 0, Qt::AlignVCenter);
    layout->addWidget(m_btnCreate, 0, Qt::AlignVCenter);
    layout->addWidget(m_btnPinTop, 0, Qt::AlignVCenter);
    layout->addWidget(m_btnMin, 0, Qt::AlignVCenter);
    layout->addWidget(m_btnMax, 0, Qt::AlignVCenter);
    layout->addWidget(m_btnClose, 0, Qt::AlignVCenter);

    // 绑定基础逻辑
    connect(m_btnMin, &QPushButton::clicked, this, &MainWindow::showMinimized);
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_btnMax, &QPushButton::clicked, this, [this]() {
        if (isMaximized()) showNormal();
        else showMaximized();
    });
    connect(m_btnClose, &QPushButton::clicked, this, &MainWindow::close);

    if (m_titleBarLayout) {
        m_titleBarLayout->addWidget(titleBarBtns);
    }

    // 逻辑：置顶切换
    connect(m_btnPinTop, &QPushButton::toggled, this, &MainWindow::onPinToggled);
}


void MainWindow::unifiedNavigateTo(const QString& url, bool record) {
    if (url.isEmpty()) return;
    ArcMeta::Logger::log(QString("[Main] 统一导航调度 -> %1 %2").arg(url).arg(record ? "(记录历史)" : "(不记录)"));

    // 1. 物理重置搜索与筛选状态
    if (m_searchEdit) {
        m_searchEdit->blockSignals(true);
        m_searchEdit->clear();
        m_searchEdit->blockSignals(false);
    }
    
    // 2026-07-xx 物理强化：无论搜索框原先是否为空，在导航行为发生时都必须强制重置内容面板的搜索词，
    // 防止因代理模型中残留的旧搜索词导致新加载的目录内容被错误过滤。
    if (m_contentPanel) {
        m_contentPanel->search("");
    }

    if (m_filterPanel) m_filterPanel->clearAllFilters();

    // 2. 压栈逻辑 (原子化)
    if (record) {
        if (m_historyIndex < static_cast<int>(m_history.size()) - 1) {
            m_history = m_history.mid(0, m_historyIndex + 1);
        }
        if (m_history.isEmpty() || m_history.last() != url) {
            m_history.append(url);
            m_historyIndex = static_cast<int>(m_history.size()) - 1;
        }
    }

    // 3. 协议分流加载
    if (url.startsWith(kProtocolCategory)) {
        // category://{id}?name={name}
        QString params = url.mid(kProtocolCategory.length());
        int qMark = params.indexOf('?');
        int id = params.left(qMark == -1 ? params.length() : qMark).toInt();
        QString name = (qMark != -1) ? params.mid(qMark + 6) : QString::number(id);

        if (m_categoryPanel) {
            m_categoryPanel->blockSignals(true);
            m_categoryPanel->selectCategory(id);
            m_categoryPanel->blockSignals(false);
        }
        if (m_contentPanel) m_contentPanel->loadCategory(id);
        if (m_addressBar) m_addressBar->setPath("分类: " + name);
        m_currentPath = url; // 逻辑路径
    }
    else if (url.startsWith(kProtocolSystem)) {
        // system://all | trash | etc.
        QString type = url.mid(kProtocolSystem.length());
        
        // 2026-08-xx 按照 Plan-128：失效数据审计模式下的容器收敛
        if (type == "invalid_data") {
            m_navPanel->hide();
            m_contentPanel->hide();
            m_filterPanel->hide();
            m_metaPanel->hide();
            
            m_invalidDataListView->refresh();
            m_invalidDataListView->show();
        } else {
            m_invalidDataListView->hide();
            m_contentPanel->show(); // 基础显示
            loadPanelVisibility();
        }

        if (m_categoryPanel) {
            m_categoryPanel->blockSignals(true);
            m_categoryPanel->selectCategoryByType(type);
            m_categoryPanel->blockSignals(false);
        }
        if (m_contentPanel) {
            m_contentPanel->setCurrentCategoryType(type);
            m_contentPanel->loadPaths(CategoryRepo::getSystemCategoryPaths(type));
        }
        if (m_addressBar) m_addressBar->setPath("系统: " + type);
        m_currentPath = url;
    }
    else {
        // 2026-08-xx 按照 Plan-128：常规导航，根据记忆状态恢复显示
        m_invalidDataListView->hide();
        m_contentPanel->show();
        loadPanelVisibility();

        // 物理路径 (file:// 或 原生路径)
        QString path = url;
        if (path.startsWith(kProtocolFile)) path = path.mid(kProtocolFile.length());
        
        if (path == "computer://") {
            if (m_addressBar) m_addressBar->setPath("computer://");
            if (m_contentPanel) m_contentPanel->loadDirectory("");
            if (m_navPanel) m_navPanel->selectPath("computer://");
            m_currentPath = "computer://";
        } else {
            QString normPath = QDir::toNativeSeparators(path);
            if (m_addressBar) m_addressBar->setPath(normPath);
            if (m_contentPanel) m_contentPanel->loadDirectory(normPath);
            if (m_navPanel) m_navPanel->selectPath(normPath);
            m_currentPath = normPath;
        }
    }

    // 2026-07-xx 按照 Plan-118：通知筛选面板当前数据源性质
    if (m_filterPanel && m_contentPanel) {
        bool isMirror = !m_contentPanel->getCurrentCategoryType().isEmpty();
        if (!isMirror && !m_currentPath.isEmpty() && m_currentPath != "computer://") {
            isMirror = MetadataManager::isInsideManagedLibrary(m_currentPath.toStdWString());
        }
        m_filterPanel->setMirrorSource(isMirror);
    }

    updateNavButtons();
    updateStatusBar();
}

void MainWindow::onBackClicked() {
    if (m_historyIndex > 0) {
        m_historyIndex--;
        unifiedNavigateTo(m_history[m_historyIndex], false);
    }
}

void MainWindow::onForwardClicked() {
    if (m_historyIndex < m_history.size() - 1) {
        m_historyIndex++;
        unifiedNavigateTo(m_history[m_historyIndex], false);
    }
}

void MainWindow::onUpClicked() {
    // 物理路径支持向上，逻辑路径默认回退至“此电脑”
    if (m_currentPath.contains("://") && m_currentPath != "computer://") {
        unifiedNavigateTo("computer://");
        return;
    }

    QDir dir(m_currentPath);
    if (dir.cdUp()) {
        unifiedNavigateTo(dir.absolutePath());
    }
}

void MainWindow::updateNavButtons() {
    m_btnBack->setEnabled(m_historyIndex > 0);
    m_btnForward->setEnabled(m_historyIndex < m_history.size() - 1);
    
    bool isLogic = m_currentPath.contains("://");
    bool atRoot = (m_currentPath == "computer://" || (!isLogic && QDir(m_currentPath).isRoot()));
    m_btnUp->setEnabled(!atRoot && !m_currentPath.isEmpty());
}

void MainWindow::onStatusBarStatsUpdated(int fileCount, int folderCount, int totalCount) {
    if (!m_statusLeft) return;
    
    // 2026-05-08 按照用户要求：只显示总项目数量和选中数量，不区分文件/文件夹
    auto selectedIndexes = m_contentPanel->getSelectedIndexes();
    QSet<int> uniqueRows;
    for (const QModelIndex& index : selectedIndexes) {
        uniqueRows.insert(index.row());
    }
    int selectedCount = uniqueRows.size();
    
    m_statusLeft->setText(QString("%1 个项目, 已选中 %2 个").arg(QString::number(totalCount)).arg(QString::number(selectedCount)));
    
    Q_UNUSED(fileCount);
    Q_UNUSED(folderCount);
}

void MainWindow::updateStatusBar() {
    if (!m_statusLeft) return;
    
    // 修正：显示经过过滤后的可见项目总数
    int visibleCount = m_contentPanel->getProxyModel()->rowCount();
    m_statusLeft->setText(QString("%1 个项目").arg(visibleCount));
}

void MainWindow::onPinToggled(bool checked) {
    // 2026-03-xx 按照用户要求优化置顶逻辑：
    // 避免重复调用导致卡顿，并优化 WinAPI 标志位以减少冗余消息推送
    if (m_isPinned == checked) return;
    m_isPinned = checked;

#ifdef Q_OS_WIN
    HWND hwnd = (HWND)winId();
    // 使用 SWP_NOSENDCHANGING 拦截冗余消息，减少 UI 线程的消息风暴，从而解决卡顿
    SetWindowPos(hwnd, checked ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
#else
    setWindowFlag(Qt::WindowStaysOnTopHint, checked);
    show(); // 非 Windows 平台修改 Flag 后通常需要重新显示
#endif

    // 更新图标和颜色 (按下置顶为品牌橙色)
    if (m_isPinned) {
        m_btnPinTop->setIcon(UiHelper::getIcon("pin_vertical", Style::ActiveOrange));
    } else {
        m_btnPinTop->setIcon(UiHelper::getIcon("pin_tilted", TextMain));
    }

    // 持久化存储
    AppConfig::instance().setValue("MainWindow/AlwaysOnTop", m_isPinned);
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) {
        // 2026-06-xx 物理对标：当窗口最小化时，显式隐藏搜索历史面板
        if (isMinimized() && m_searchHistoryPanel) {
            m_searchHistoryPanel->hide();
        }

        // 2026-04-11 按照用户要求：物理识别窗口状态，精准切换最大化/还原图标
        if (m_btnMax) {
            QString iconKey = isMaximized() ? "restore_line" : "maximize";
            m_btnMax->setIcon(UiHelper::getIcon(iconKey, QColor("#EEEEEE")));
        }

        // 2026-06-xx 按照用户要求：顶部始终为 0，确保容器顶部边框作为物理切割线；无论是否最大化，左右和底部的 5px (kEdgeMargin) 留白均需保留
        if (m_bodyLayout) {
            m_bodyLayout->setContentsMargins(kEdgeMargin, 0, kEdgeMargin, kEdgeMargin);
        }
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    AppConfig::instance().setValue("MainWindow/LastPath", m_currentPath);
    // 2026-04-11 按照用户要求：物理保存各容器宽度状态
    if (m_mainSplitter) {
        AppConfig::instance().setValue("MainWindow/SplitterState", m_mainSplitter->saveState());
        // 2026-07-xx 按照 Plan-63：保存面板显隐状态
        savePanelVisibility();
    }
    AppConfig::instance().sync();

    // 2026-06-xx 物理加固：退出前强制所有分类数据落盘，彻底解决因防抖计时器导致的重启回滚问题
    CategoryRepo::saveImmediately();

    QMainWindow::closeEvent(event);
}

void MainWindow::showPanelContextMenu(const QPoint& globalPos) {
    QMenu menu(this);
    UiHelper::applyMenuStyle(&menu);
    populatePanelMenu(&menu);
    menu.exec(globalPos);
}

void MainWindow::populatePanelMenu(QMenu* menu) {
    auto addToggleAction = [&](const QString& text, QWidget* panel, bool canHide = true) {
        QAction* action = menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(panel->isVisible());
        action->setEnabled(canHide);
        // 使用 Lambda 捕获成员变量，确保连接有效
        connect(action, &QAction::toggled, panel, [panel](bool visible) {
            panel->setVisible(visible);
        });
    };

    addToggleAction("显示分类栏", m_categoryPanel);
    addToggleAction("显示目录导航", m_navPanel);
    addToggleAction("显示内容区", m_contentPanel, false); // 核心区锁定不可隐藏
    addToggleAction("显示元数据栏", m_metaPanel);
    addToggleAction("显示筛选栏", m_filterPanel);

    // 2. 新增重置选项
    menu->addSeparator();
    QAction* resetAct = menu->addAction("重置分栏");
    connect(resetAct, &QAction::triggered, this, &MainWindow::resetSplitterLayout);
}

void MainWindow::resetSplitterLayout() {
    // 1. 物理恢复可见性并退出特殊模式
    m_isTagManagerMode = false;
    m_tagManagerView->hide();
    if (m_invalidDataListView) m_invalidDataListView->hide();

    m_categoryPanel->show();
    m_navPanel->show();
    m_contentPanel->show();
    m_metaPanel->show();
    m_filterPanel->show();

    // 2. 物理恢复尺寸比例 (索引 0-4)
    QList<int> sizes;
    sizes << 230 << 230 << 600 << 230 << 230;
    // 如果 TagManagerView 在索引 5，需确保其 size 为 0 或处于 hide 状态
    if (m_mainSplitter->count() > 5) sizes << 0;

    m_mainSplitter->setSizes(sizes);

    // 3. 清除持久化状态，防止重启后回滚旧布局
    AppConfig::instance().remove("MainWindow/SplitterState");
    AppConfig::instance().remove("MainWindow/PanelVisibility");
    AppConfig::instance().sync();

    ToolTipOverlay::instance()->showText(QCursor::pos(), "布局已重置为默认值", 1500);
}

void MainWindow::loadPanelVisibility() {
    QVariant val = AppConfig::instance().getValue("MainWindow/PanelVisibility");
    if (!val.isValid()) return;

    QStringList hiddenPanels = val.toStringList();
    if (hiddenPanels.contains("category")) m_categoryPanel->hide();
    if (hiddenPanels.contains("nav"))      m_navPanel->hide();
    if (hiddenPanels.contains("meta"))     m_metaPanel->hide();
    if (hiddenPanels.contains("filter"))   m_filterPanel->hide();
}

void MainWindow::savePanelVisibility() {
    // 2026-08-xx 物理回避：在失效数据或标签管理等收敛模式下，禁止保存当前布局状态，
    // 防止覆盖用户正常的日常分栏配置。
    if (m_isTagManagerMode || (m_invalidDataListView && m_invalidDataListView->isVisible())) {
        return;
    }

    QStringList hiddenPanels;
    if (!m_categoryPanel->isVisible()) hiddenPanels << "category";
    if (!m_navPanel->isVisible())      hiddenPanels << "nav";
    if (!m_metaPanel->isVisible())     hiddenPanels << "meta";
    if (!m_filterPanel->isVisible())   hiddenPanels << "filter";
    
    AppConfig::instance().setValue("MainWindow/PanelVisibility", hiddenPanels);
}

void MainWindow::initDriveBar() {
    m_driveBarWidget = new QWidget(this);
    m_driveBarWidget->setObjectName("DriveBar");
    m_driveBarWidget->setFixedHeight(42);
    m_driveBarWidget->setStyleSheet(QString(
        "QWidget#DriveBar { background-color: %1; border-bottom: 1px solid %2; }"
    ).arg(qssColor(BackgroundHeader)).arg(qssColor(BorderColor)));

    m_driveBarLayout = new QHBoxLayout(m_driveBarWidget);
    m_driveBarLayout->setContentsMargins(15, 5, 15, 5);
    m_driveBarLayout->setSpacing(8);

    auto drives = QDir::drives();
    for (const QFileInfo& drive : drives) {
        QString letter = drive.absolutePath().left(2);
        if (letter.endsWith("/")) letter = letter.left(1) + ":";
        
        DriveButton* btn = new DriveButton(letter, m_driveBarWidget);
        m_driveButtons[letter] = btn;
        m_driveBarLayout->addWidget(btn);

        connect(btn, &QPushButton::clicked, this, &MainWindow::onDriveButtonClicked);
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QWidget::customContextMenuRequested, this, &MainWindow::onDriveButtonContextMenu);
        
        // 根据托管库是否存在初始化状态
        QString managedPath = drive.absolutePath() + "ArcMeta.Library_" + letter.left(1);
        if (QDir(managedPath).exists()) {
            btn->setState(DriveButton::Active);
        } else {
            btn->setState(DriveButton::Inactive);
        }
    }
    m_driveBarLayout->addStretch();
}

void MainWindow::onDriveButtonClicked() {
    // TODO: 盘符点击逻辑待后期安排
    qDebug() << "[Main] Drive button clicked (TODO)";
}

void MainWindow::onDriveButtonContextMenu(const QPoint& pos) {
    DriveButton* btn = qobject_cast<DriveButton*>(sender());
    if (!btn) return;
    QString letter = btn->driveLetter();
    QString managedPath = letter + "/ArcMeta.Library_" + letter.left(1);

    QMenu menu(this);
    UiHelper::applyMenuStyle(&menu);
    if (!QDir(managedPath).exists()) {
        menu.addAction("创建托管文件夹")->setData(1);
    } else {
        menu.addAction("打开托管文件夹")->setData(2);
        menu.addAction("重新扫描该盘")->setData(3);
    }

    QAction* act = menu.exec(btn->mapToGlobal(pos));
    if (!act) return;
    int val = act->data().toInt();
    if (val == 1) {
        if (QDir().mkpath(managedPath)) {
            btn->setState(DriveButton::Active);
            
            // 2026-08-xx 物理同步：创建托管库时，同步注册逻辑分类并锚定 FRN
            std::wstring wPath = QDir::toNativeSeparators(managedPath).toStdWString();
            std::string fid;
            std::wstring frnStr;
            if (MetadataManager::fetchWinApiMetadataDirect(wPath, fid, &frnStr)) {
                try {
                    Category cat;
                    cat.name = QFileInfo(managedPath).fileName().toStdWString();
                    cat.physicalFrn = std::stoull(frnStr, nullptr, 16);
                    cat.physicalPath = wPath;
                    cat.color = CategoryRepo::getDefaultColor();
                    if (CategoryRepo::add(cat)) {
                        MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
                    }
                } catch (...) {}
            }

            // [Plan-129] USN 监控热激活：新建库后立即点火该盘符的监控引擎 (需预检防止重复启动)
            if (!MftReader::instance().isDriveIndexed(letter)) {
                MftReader::instance().buildIndex({letter});
            }
            
            ToolTipOverlay::instance()->showText(QCursor::pos(), "托管库创建成功", 1500, Style::SuccessGreen);
        }
    } else if (val == 2) {
        ShellHelper::openInExplorer(managedPath);
    } else if (val == 3) {
        // 2026-07-xx 按照 Development_Plan 2.1：“重新扫描该盘”是指扫描该盘符下的“ArcMeta.Library_[盘符]”文件夹里所有的数据
        if (QDir(managedPath).exists()) {
            MetadataManager::instance().markAsRegistered(managedPath.toStdWString());
            ToolTipOverlay::instance()->showText(QCursor::pos(), "已开始重新扫描托管库", 1500, QColor("#378ADD"));
        }
    }
}

} // namespace ArcMeta
