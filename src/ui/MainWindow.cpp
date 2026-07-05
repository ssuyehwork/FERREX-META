#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MainWindow.h"
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
#include "QuickLookWindow.h"
#include "ToolTipOverlay.h"
#include "ScanDialog.h"
#include "../mft/MftReader.h"
#include "../db/CategoryRepo.h"
#include "../db/ItemRepo.h"
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
using namespace ArcMeta::Style;
#include "../core/ModelContract.h"
#include <QFileInfo>
#include <QDir>
#include "../meta/MetadataManager.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <Dbt.h>
#endif

#include "../db/SyncEngine.h"
#include <QtConcurrent>

namespace ArcMeta {

MainWindow::~MainWindow() {
    if (m_resizeFilter) {
        qApp->removeEventFilter(m_resizeFilter);
    }
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    // 2026-04-12 关键修复：显式初始化面板加载状态锁，防止未定义行为导致闪退
    m_panelsInitialized = false;
    qDebug() << "[Main] MainWindow 构造开始执行";

    // 2026-04-11 按照用户要求：在程序启动的最顶端预初始化 ToolTipOverlay
    // 配合 ToolTipOverlay 内部的 winId() 强行预热，消除初次显示延迟
    ToolTipOverlay::instance();

    resize(1200, 800);
    setMinimumSize(1180, 653); // 物理对齐：5x230px面板 + 20px分割手柄 + 10px全局边距
    setWindowTitle("FERREX");

    

    // 从设置读取置顶状态
    m_isPinned = AppConfig::instance().getValue("MainWindow/AlwaysOnTop", false).toBool();
    
    // 设置基础窗口标志 (保持无边框)
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint);

    // 初始应用置顶 (WinAPI)
    // 2026-03-xx 关键修复：构造函数内不再调用 winId() 或 SetWindowPos 避免触发窗口提前显示
    // 置顶逻辑现在改为按需由 external 或 showEvent 安全触发
    if (m_isPinned) {
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
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
            QScrollBar:vertical { border: none; background: transparent; width: 4px; }
            QScrollBar::handle:vertical { background: %2; min-height: 20px; border-radius: 2px; }
            QScrollBar::handle:vertical:hover { background: %4; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
            QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
            QScrollBar:horizontal { border: none; background: transparent; height: 4px; }
            QScrollBar::handle:horizontal { background: %2; min-width: 20px; border-radius: 2px; }
            QScrollBar::handle:horizontal:hover { background: %4; }
            QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }
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
    
    m_hoverFilter = new HoverEventFilter(this);
    m_resizeFilter = new ResizeEventFilter(this);

    m_trayController = new TrayController(this);
    m_trayController->show();

    initIdleDetector();
    qDebug() << "[Main] MainWindow 构造函数 UI/托盘/闲置检测初始化完成";

    // 2026-03-xx 性能优化：严禁在构造函数中执行任何可能导致阻塞的同步加载 (如 navigateTo)。
    // 改为延迟 200ms 触发首次加载，确保 MainWindow 框架先瞬间弹出，提升用户感知的“秒开”响应速度。
    QTimer::singleShot(200, [this]() {
        QString lastPath = AppConfig::instance().getValue("MainWindow/LastPath", "computer://").toString();
        
        // 2026-04-11 按照用户要求：物理还原最后一次开启的文件夹
        // 校验路径：如果是虚拟路径或真实存在的磁盘路径，则载入
        if (lastPath == "computer://" || QDir(lastPath).exists()) {
            qDebug() << "[Main] 执行延迟首次加载: 恢复历史路径 ->" << lastPath;
            navigateTo(lastPath);
            m_navPanel->selectPath(lastPath);
        } else {
            qDebug() << "[Main] 历史路径无效，回退至: 此电脑";
            navigateTo("computer://");
            m_navPanel->selectPath("computer://");
        }
    });
}

void MainWindow::initUi() {
    initToolbar();
    setupSplitters();

    // 2026-05-29 性能优化：ResizeEventFilter 仅针对 MainWindow 安装
    if (m_resizeFilter) {
        this->installEventFilter(m_resizeFilter);
    }
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
    } else {
        // 初始默认分配: 230 | 230 | 600 | 230 | 230
        QList<int> sizes;
        sizes << 230 << 230 << 600 << 230 << 230;
        m_mainSplitter->setSizes(sizes);
    }

    // 核心红线：建立各面板间的信号联动 (Data Linkage)
    
    // 1. 导航/收藏/内容面板 双击跳转 -> 统一路径调度
    connect(m_navPanel, &NavPanel::directorySelected, this, [this](const QString& path) {
        navigateTo(path);
    });

    connect(m_contentPanel, &ContentPanel::directorySelected, this, [this](const QString& path) {
        navigateTo(path);
    });

    // 1a. 分类选择 -> 内容面板执行数据加载 (针对问题 2)
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryPanel, &CategoryPanel::categorySelected, this, [this](int id, const QString& name, const QString& type, const QString& path) {
        // 2026-04-12 关键修正：跳转分类时，物理重置搜索状态，防止逻辑锁死
        if (m_searchEdit) m_searchEdit->clear();
        m_contentPanel->search("");

        if (m_addressBar) m_addressBar->setPath("分类: " + name);
        
        if (type == "category") {
            // 2026-06-xx 重构逻辑：内容面板负责展示该分类下的子分类与绑定文件
            m_contentPanel->loadCategory(id);
        } else if (type == "bookmark") {
            // 2026-06-xx 处理快速访问项跳转 (Favorite 路径加载)
            if (!path.isEmpty()) {
                navigateTo(path);
            } else {
                m_contentPanel->search(name);
            }
        } else if (type == "all" || type == "uncategorized" || type == "untagged" || 
                   type == "today" || type == "yesterday" || type == "recently_visited" || type == "trash") {
            // 2026-06-xx 物理修复：所有系统项直接走数据库专用路径接口，彻底断开与搜索框的傻逼耦合
            m_contentPanel->setCurrentCategoryType(type);
            m_contentPanel->loadPaths(ItemRepo::getPathsBySystemType(type));
        } else {
            // 其余系统项 (标签管理等) 维持搜索逻辑
            m_contentPanel->search(name); 
        }
    });

    // 1b. 内容面板内部跳转分类 (双击同步)
    connect(m_contentPanel, &ContentPanel::categoryClicked, this, [this](int id) {
        if (m_categoryPanel) m_categoryPanel->selectCategory(id);
    });

    // 2. 内容面板选中项改变 -> 元数据面板刷新 & 自动预览
    // 2026-03-xx 按照高性能要求，优先从模型 Role 读取元数据缓存，避免频繁磁盘 IO
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_contentPanel, &ContentPanel::selectionChanged, this, [this](const QStringList& paths) {
        if (paths.isEmpty()) {
            m_metaPanel->updateInfo("-", "-", "-", "-", "-", "-", "-", false);
            m_metaPanel->setRating(0);
            m_metaPanel->setColor(L"");
            m_metaPanel->setPinned(false);
            m_metaPanel->setTags(QStringList());
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
            
            // 加载备注和色板
            RuntimeMeta rm = MetadataManager::instance().getMeta(path.toStdWString());
            m_metaPanel->setNote(rm.note);

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

        // 物理同步：实时刷新 ContentPanel 列表模型，确保主界面同步变迁
        auto* proxy = m_contentPanel->getProxyModel();
        for (int i = 0; i < proxy->rowCount(); ++i) {
            QModelIndex idx = proxy->index(i, 0);
            if (idx.data(PathRole).toString() == m_currentQuickLookPath) {
                proxy->setData(idx, rating, RatingRole);
                break;
            }
        }

        m_metaPanel->setRating(rating);
        // 2026-04-11 按照用户要求：在预览窗设定星级时，左上方即时反馈
        QString msg = QString("已设定星级: <span style='color: #FECF0E;'>%1 星</span>").arg(rating);
        ToolTipOverlay::instance()->showText(QPoint(50, 50), msg, 1500, QColor("#FECF0E"));
    });

    connect(&QuickLookWindow::instance(), &QuickLookWindow::colorRequested, this, [this](const QString& color) {
        if (m_currentQuickLookPath.isEmpty()) return;

        // 2026-04-11 按照用户要求：补全物理持久化逻辑 (MetadataManager 直接入库)
        MetadataManager::instance().setColor(m_currentQuickLookPath.toStdWString(), color.toStdWString());

        // 物理同步：实时刷新 ContentPanel 列表模型，确保主界面同步变迁
        auto* proxy = m_contentPanel->getProxyModel();
        for (int i = 0; i < proxy->rowCount(); ++i) {
            QModelIndex idx = proxy->index(i, 0);
            if (idx.data(PathRole).toString() == m_currentQuickLookPath) {
                proxy->setData(idx, color, ColorRole);
                break;
            }
        }

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

    // 5a. 目录装载完成 -> FilterPanel 动态填充 (六参数版本)
    connect(m_contentPanel, &ContentPanel::directoryStatsReady, this,
        [this](const QMap<int,int>& r, const QMap<QString,int>& c,
               const QMap<QString,int>& t, const QMap<QString,int>& tp,
               const QMap<QString,int>& cd, const QMap<QString,int>& md) {
            m_filterPanel->populate(r, c, t, tp, cd, md);
        });

    // 5b. FilterPanel 勾选变化 -> 内容面板过滤
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_filterPanel, &FilterPanel::filterChanged, this, [this](const FilterState& state) {
        m_contentPanel->applyFilters(state);
        updateStatusBar(); // 筛选后立即更新底栏可见项目总数
    });

    connect(m_filterPanel, &FilterPanel::resetSearchRequested, this, [this]() {
        // 2026-04-12 关键同步：点击右侧“清除”时，同步清空顶部搜索框
        if (m_searchEdit) m_searchEdit->clear();
        m_contentPanel->search("");
    });

    // 6. 地址栏路径跳转
    connect(m_addressBar, &AddressBar::pathChanged, this, [this](const QString& path) {
        navigateTo(path);
    });

    // 7. 搜索框回车触发逻辑 (带历史记录和搜索模式分流)
    m_searchHistory = AppConfig::instance().getValue("Search/History").toStringList();
    
    m_searchHistoryPanel = new SearchHistoryPanel(this);
    m_searchHistoryPanel->setHistory(m_searchHistory);

    // 回车搜索核心逻辑
    auto doSearch = [this](const QString& keyword) {
        if (keyword.isEmpty()) {
            m_contentPanel->loadDirectory(m_currentPath);
            m_searchHistoryPanel->hide();
            return;
        }

        // 维护历史记录（去重，置顶，最多保持10条）
        m_searchHistory.removeAll(keyword);
        m_searchHistory.prepend(keyword);
        if (m_searchHistory.size() > 10) m_searchHistory.removeLast();
        AppConfig::instance().setValue("Search/History", m_searchHistory);
        m_searchHistoryPanel->setHistory(m_searchHistory);
        m_searchHistoryPanel->hide();

        // 使用 CoreController 的中枢搜索接口
        QStringList paths = CoreController::instance().performSearch(keyword);
        m_contentPanel->loadPaths(paths);
    };

    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this, doSearch]() {
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
                // 2026-05-24 按照用户要求：彻底移除 JSON，改为中心化异步持久化
                m_contentPanel->getProxyModel()->setData(idx, rating, RatingRole);
                MetadataManager::instance().setRating(path.toStdWString(), rating);
            }
            if (color != L"__NO_CHANGE__") {
                m_contentPanel->getProxyModel()->setData(idx, QString::fromStdWString(color), ColorRole);
                MetadataManager::instance().setColor(path.toStdWString(), color);
            }
        }
    });

    // 2026-06-xx 调色盘搜索联动
    connect(m_metaPanel, &MetaPanel::searchByColor, this, [this](const QColor& color) {
        if (m_contentPanel) {
            m_contentPanel->search(color.name().toUpper());
        }
    });

    // 9. 2026-03-xx 按照用户要求：响应元数据全局变更，同步刷新侧边栏计数
    // 确保在内容面板收藏项目后，侧边栏的“收藏 (X)”数字能实时跳变
    // 2026-05-27 物理修复：显式增加 this 上下文参数
    // 理由：MetadataManager 可能在后台线程（如初始化扫描阶段）发射信号，
    // 若无 context 参数，Lambda 将在发射信号的后台线程执行，导致非法线程操作 UI (refresh()) 引起崩溃。
    connect(&MetadataManager::instance(), &MetadataManager::metaChanged, this, [this]() {
        if (m_categoryPanel && m_categoryPanel->model()) {
            m_categoryPanel->model()->refresh();
        }
    });

    // 10. 侧边栏点击物理项（文件预览或文件夹跳转）
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryPanel, &CategoryPanel::fileSelected, this, [this](const QString& path) {
        QFileInfo fi(path);
        if (fi.isDir()) {
            // 如果是文件夹，执行界面跳转联动
            navigateTo(path);
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
        // 2026-05-24 按照用户要求：捕捉硬件变更，硬盘插入时触发 GLOB 扫描对账
        if (msg->wParam == DBT_DEVICEARRIVAL || msg->wParam == DBT_DEVICEREMOVECOMPLETE) {
            qDebug() << "[Main] 检测到磁盘硬件变更，触发全量 GLOB 对账对账...";
            // 异步触发扫描，防止阻塞 UI
            (void)QtConcurrent::run([]() {
                SyncEngine::instance().runFullScan({}, nullptr);
            });
        }
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
        qDebug() << "[Main] 正在排期延迟加载任务 (QTimer::singleShot(0))...";
        QTimer::singleShot(0, [this]() {
            qDebug() << "[Main] 延迟加载任务开始执行";
            if (m_categoryPanel) {
                qDebug() << "[Main] 正在初始化 CategoryPanel...";
                m_categoryPanel->deferredInit();
            }
            if (m_navPanel) {
                qDebug() << "[Main] 正在初始化 NavPanel...";
                m_navPanel->deferredInit();
            }
            if (m_contentPanel) {
                qDebug() << "[Main] 正在初始化 ContentPanel...";
                m_contentPanel->deferredInit();
            }
            // MetaPanel 和 FilterPanel 暂时不需要延迟数据加载，因为它们通常随选中项动态刷新
            
            // 2026-04-14 按照用户要求：物理禁用"最后一个窗口关闭时退出"逻辑
            // 确保程序只能通过托盘菜单显式退出，提高驻留稳定性
            qDebug() << "[Main] 所有核心面板数据延迟初始化完成，UI 响应已恢复";
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
    if (localPos.y() <= 32) {
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
    // 2026-06-15 闲置感应：拦截用户交互事件，重置同步倒计时
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress || 
        event->type() == QEvent::KeyPress || event->type() == QEvent::Wheel) {
        if (m_idleTimer) {
            m_idleTimer->start(); // 重新开始 30 秒计次
        }
    }

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
        btn->setStyleSheet(
            "QPushButton { background: transparent; border: none; border-radius: 4px; }"
            "QPushButton:hover { background: rgba(255, 255, 255, 0.1); }"
            "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
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


    // 2026-04-12 按照用户要求：搜索框容器（搜索框 + 模式切换按钮）
    m_searchContainer = new QWidget(this);
    m_searchContainer->setStyleSheet("background: transparent;");
    QHBoxLayout* searchLayout = new QHBoxLayout(m_searchContainer);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(0);

    m_searchEdit = new QLineEdit(m_searchContainer);
    m_searchEdit->setPlaceholderText("查找文件...");
    m_searchEdit->setMinimumWidth(180);
    m_searchEdit->setFixedHeight(32);
    m_searchEdit->addAction(UiHelper::getIcon("search", TextMuted), QLineEdit::LeadingPosition);
    // 按照用户要求：移除局部/全局切换按钮，恢复搜索框 6px 完整圆角
    m_searchEdit->setStyleSheet(QString(
        "QLineEdit { background: %1; border: 1px solid %2;"
        "  border-radius: 6px;"
        "  color: %3; padding-left: 5px; }"
        "QLineEdit:focus { border: 1px solid %4; }"
    ).arg(qssColor(BackgroundDeep)).arg(qssColor(BorderColor)).arg(qssColor(TextMain)).arg(qssColor(PrimaryBlue)));

    searchLayout->addWidget(m_searchEdit, 1);
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
    m_titleBarWidget->setFixedHeight(32);
    m_titleBarLayout = new QHBoxLayout(m_titleBarWidget);
    m_titleBarLayout->setContentsMargins(8, 0, 5, 0); // 右侧对齐 5px 物理边距
    m_titleBarLayout->setSpacing(8);

    m_appNameLabel = new QLabel("FERREX", m_titleBarWidget);
    m_appNameLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: bold;").arg(qssColor(TextDark)));
    m_titleBarLayout->addWidget(m_appNameLabel);
    m_titleBarLayout->addStretch();

    // --- 2. 统一导航栏 (第二行) ---
    m_navBarWidget = new QWidget(centralC);
    m_navBarWidget->setObjectName("NavBar");
    // 2026-06-xx 物理修正：移除 QWidget 可能自带的 1px 底部边框干扰，确保 100% 重合
    m_navBarWidget->setStyleSheet("QWidget#NavBar { border: none; background: transparent; }");
    // 2026-06-xx 物理修正：将固定高度从 37px 增加到 42px (32+5+5)
    m_navBarWidget->setFixedHeight(42); 
    
    m_navBarLayout = new QHBoxLayout(m_navBarWidget);
    // 2026-06-xx 物理修正：增加底部 5px 间距，确保切割线与地址栏不重合
    m_navBarLayout->setContentsMargins(5, 5, 5, 5); 
    m_navBarLayout->setSpacing(5);
    m_navBarLayout->setAlignment(Qt::AlignVCenter);

    m_navBarLayout->addWidget(m_btnBack);
    m_navBarLayout->addWidget(m_btnForward);
    m_navBarLayout->addWidget(m_btnUp);
    m_navBarLayout->addWidget(m_addressBar, 1);
    m_navBarLayout->addWidget(m_searchContainer);

    // --- 3. 主体核心容器 (物理还原：10px 全局边距包裹，确保边缘resize可用) ---
    QWidget* bodyWrapper = new QWidget(centralC);
    bodyWrapper->setStyleSheet("background: transparent;"); // 确保背景透明不遮挡阴影
    m_bodyLayout = new QVBoxLayout(bodyWrapper);
    // 2026-06-xx 物理修正：顶部边距设为 0，使容器顶部边框作为切割线与地址栏保持 5px 间距
    m_bodyLayout->setContentsMargins(5, 0, 5, 5); 
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

    // 2026-05-07 按照用户要求：焦点线持久化显示，基于数据来源而非焦点位置
    connect(m_contentPanel, &ContentPanel::dataSourceChanged, this, [this](const QString& source) {
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

    m_bodyLayout->addWidget(m_mainSplitter);

    // --- 4. 底部状态栏 (0 边距) ---
    QWidget* statusBar = new QWidget(centralC);
    statusBar->setObjectName("StatusBar");
    statusBar->setFixedHeight(28);
    QHBoxLayout* statusL = new QHBoxLayout(statusBar);
    statusL->setContentsMargins(12, 0, 12, 0);
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

    mainL->addWidget(m_titleBarWidget);
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
    layout->setSpacing(4);

    auto createTitleBtn = [this](const QString& iconKey, const QString& hoverColor = "rgba(255, 255, 255, 0.1)") {
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
            "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
        ).arg(hoverColor));
        return btn;
    };

    m_btnSync = createTitleBtn("sync");
    m_btnSync->setProperty("tooltipText", "元数据已同步至物理文件");
    m_btnSync->installEventFilter(m_hoverFilter);

    // 2026-06-15 按照用户要求：手动点击同步
    connect(m_btnSync, &QPushButton::clicked, this, [this]() {
        SyncEngine::instance().runIncrementalSync();
        ToolTipOverlay::instance()->showText(m_btnSync->mapToGlobal(QPoint(0,0)), "正在启动延时同步...", 1500);
    });

    // 联动同步按钮颜色状态 (红色预警)
    auto updateSyncBtnState = [this](bool hasPending) {
        if (hasPending) {
            m_btnSync->setIcon(UiHelper::getIcon("sync", ErrorRed)); // 强制红色
            m_btnSync->setProperty("tooltipText", "存在待同步元数据，请点击或等待闲置同步");
        } else {
            m_btnSync->setIcon(UiHelper::getIcon("sync", TextMain)); // 恢复正常
            m_btnSync->setProperty("tooltipText", "元数据已同步至物理文件");
        }
    };
    
    connect(&MetadataManager::instance(), &MetadataManager::pendingSyncChanged, this, updateSyncBtnState);
    connect(&SyncEngine::instance(), &SyncEngine::syncStatusChanged, this, [this, updateSyncBtnState](bool running) {
        if (!running) {
            updateSyncBtnState(SyncEngine::instance().hasPendingTasks());
        }
    });

    // 启动时初始化一次状态
    QTimer::singleShot(500, this, [this, updateSyncBtnState]() {
        updateSyncBtnState(SyncEngine::instance().hasPendingTasks());
    });

    m_btnScan = createTitleBtn("scan");
    m_btnScan->setProperty("tooltipText", "实时扫描与查找...");
    m_btnScan->installEventFilter(m_hoverFilter);
    // 2026-05-09 按照用户要求：扫描窗口与主界面操作相互不干扰，去除父子关系
    connect(m_btnScan, &QPushButton::clicked, this, [this]() {
        auto* dlg = new ScanDialog(nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose); // 2026-05-27 物理修复：关闭后自动释放内存
        dlg->setModal(false);
        dlg->show();
    });

    m_btnCreate = createTitleBtn("add"); // 2026-03-xx 规范化：“+”按钮图标修正
    m_btnCreate->setProperty("tooltipText", "新建...");
    QMenu* createMenu = new QMenu(m_btnCreate);
    createMenu->setStyleSheet(
        "QMenu { background-color: #2D2D2D; color: #EEE; border: 1px solid #444; padding: 4px; border-radius: 8px; }"
        "QMenu::item { padding: 6px 25px 6px 10px; border-radius: 4px; font-size: 12px; }"
        "QMenu::item:selected { background-color: #3E3E42; color: white; }"
        "QMenu::right-arrow { image: url(data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0ibm9uZSIgc3Ryb2tlPSIjRUVFRUVFIiBzdHJva2Utd2lkdGg9IjIiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCI+PHBvbHlsaW5lIHBvaW50cz0iOSAxOCAxNSAxMiA5IDYiPjwvcG9seWxpbmU+PC9zdmc+); width: 12px; height: 12px; right: 8px; }"
    );
    
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
        m_btnPinTop->setIcon(UiHelper::getIcon("pin_vertical", BrandOrange));
    }

    m_btnMin = createTitleBtn("minimize");
    m_btnMin->setProperty("tooltipText", "最小化");
    m_btnMin->installEventFilter(m_hoverFilter);

    m_btnMax = createTitleBtn(isMaximized() ? "restore_window" : "maximize");
    m_btnMax->setProperty("tooltipText", "最大化/还原");
    m_btnMax->installEventFilter(m_hoverFilter);

    m_btnClose = createTitleBtn("close", qssColor(ErrorRed)); // 初始创建
    // 按照用户要求：关闭按钮持续显示红色高亮，不再仅悬停显示
    m_btnClose->setStyleSheet(QString(
        "QPushButton { background-color: %1; border: none; border-radius: 4px; padding: 0; }"
        "QPushButton:hover { background-color: #F1707A; }"
        "QPushButton:pressed { background-color: #A50000; }"
    ).arg(qssColor(ErrorRed)));
    m_btnClose->setProperty("tooltipText", "关闭项目");
    m_btnClose->installEventFilter(m_hoverFilter);

    m_btnCreate->installEventFilter(m_hoverFilter);
    layout->addWidget(m_btnSync);
    layout->addWidget(m_btnScan);
    layout->addWidget(m_btnCreate);
    layout->addWidget(m_btnPinTop);
    layout->addWidget(m_btnMin);
    layout->addWidget(m_btnMax);
    layout->addWidget(m_btnClose);

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

void MainWindow::initIdleDetector() {
    // 2026-06-15 按照用户要求：建立 30 秒闲置自动同步机制
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(30000); // 30秒
    m_idleTimer->setSingleShot(true);
    
    connect(m_idleTimer, &QTimer::timeout, this, [this]() {
        qDebug() << "[Main] 检测到系统闲置超过30秒，触发自动对账同步...";
        SyncEngine::instance().runIncrementalSync();
    });

    // 启动闲置计时
    m_idleTimer->start();
    
    // 2026-05-29 性能优化：事件过滤器仅安装在 MainWindow 实例上，减少 qApp 全局事件分发的 overhead。
    this->installEventFilter(this);
}

void MainWindow::navigateTo(const QString& path, bool record) {
    if (path.isEmpty()) return;
    qDebug() << "[Main] 执行跳转 ->" << path << (record ? "(记录历史)" : "(不记录)");

    // 2026-04-12 关键协议：任何导航操作（手动输入、点击、后退、上级）都应强制重置搜索态与筛选态
    if (m_searchEdit && !m_searchEdit->text().isEmpty()) {
        qDebug() << "[Main] 检测到导航操作，物理清空搜索关键词残留:" << m_searchEdit->text();
        m_searchEdit->clear();
        m_contentPanel->search("");
    }
    
    if (m_filterPanel) {
        m_filterPanel->clearAllFilters();
    }
    
    // 处理虚拟路径 "computer://" —— 此电脑（磁盘分区列表）
    if (path == "computer://") {
        m_currentPath = "computer://";
        if (record) {
            if (m_history.isEmpty() || m_history.last() != path) {
                m_history.append(path);
                m_historyIndex = static_cast<int>(m_history.size()) - 1;
            }
        }
        if (m_addressBar) m_addressBar->setPath("computer://");
        m_contentPanel->loadDirectory(""); 
        int driveCount = static_cast<int>(QDir::drives().count());
        m_statusLeft->setText(QString("%1 个分区").arg(driveCount));
        updateNavButtons();
        return;
    }

    QString normPath = QDir::toNativeSeparators(path);
    m_currentPath = normPath;

    if (record) {
            if (m_historyIndex < static_cast<int>(m_history.size()) - 1) {
            m_history = m_history.mid(0, m_historyIndex + 1);
        }
        if (m_history.isEmpty() || m_history.last() != normPath) {
            m_history.append(normPath);
                m_historyIndex = static_cast<int>(m_history.size()) - 1;
        }
    }
    
    if (m_addressBar) m_addressBar->setPath(normPath);
    m_contentPanel->loadDirectory(normPath);
    updateNavButtons();
    updateStatusBar();
}

void MainWindow::onBackClicked() {
    if (m_historyIndex > 0) {
        m_historyIndex--;
        navigateTo(m_history[m_historyIndex], false);
    }
}

void MainWindow::onForwardClicked() {
    if (m_historyIndex < m_history.size() - 1) {
        m_historyIndex++;
        navigateTo(m_history[m_historyIndex], false);
    }
}

void MainWindow::onUpClicked() {
    QDir dir(m_currentPath);
    if (dir.cdUp()) {
        navigateTo(dir.absolutePath());
    }
}

void MainWindow::updateNavButtons() {
    m_btnBack->setEnabled(m_historyIndex > 0);
    m_btnForward->setEnabled(m_historyIndex < m_history.size() - 1);
    
    bool atRoot = (m_currentPath == "computer://" || (QDir(m_currentPath).isRoot()));
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
        m_btnPinTop->setIcon(UiHelper::getIcon("pin_vertical", BrandOrange));
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
            QString iconKey = isMaximized() ? "restore_window" : "maximize";
            m_btnMax->setIcon(UiHelper::getIcon(iconKey, QColor("#EEEEEE")));
        }

        // 2026-06-xx 按照用户要求：顶部始终为 0，确保容器顶部边框作为物理切割线
        if (m_bodyLayout) {
            if (isMaximized()) {
                m_bodyLayout->setContentsMargins(0, 0, 0, 0);
            } else {
                m_bodyLayout->setContentsMargins(5, 0, 5, 5);
            }
        }
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    AppConfig::instance().setValue("MainWindow/LastPath", m_currentPath);
    // 2026-04-11 按照用户要求：物理保存各容器宽度状态
    if (m_mainSplitter) {
        AppConfig::instance().setValue("MainWindow/SplitterState", m_mainSplitter->saveState());
    }
    AppConfig::instance().sync();
    QMainWindow::closeEvent(event);
}

} // namespace ArcMeta
