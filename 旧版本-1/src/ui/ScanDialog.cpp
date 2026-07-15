#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include "HistoryDropdownController.h"
#include "ResultTableColumnWidthPolicy.h"
#include "StatusBarFormatter.h"
#include "FramelessResizeBorder.h"
#include "ContextMenuExecutor.h"
#include "ThumbnailWarmupPipeline.h"
#include "GlobalKeyboardShortcutHandler.h"
#include "ViewportTooltipController.h"

#include "IScanResultView.h"
#include "ListResultView.h"
#include "JustifiedResultView.h"
#include "GridResultView.h"
#include <QDataStream>
#include "../core/CacheManager.h"
#include <QPainter>
#include <QTimer>
#include <QIcon>
#include "../mft/MftReader.h"
#include "UiHelper.h"
#include "../util/ShellHelper.h"
#include "../meta/MetadataManager.h"
#include <QFileInfo>
#include <QCheckBox>
#include <QFrame>
#include <QProgressBar>
#include <QFuture>
#include <QFutureWatcher>
#include <QCloseEvent>
#include <QWheelEvent>
#include <QLineEdit>
#include <QTextEdit>
#include <QTableView>
#include <QAbstractTableModel>
#include <QSvgRenderer>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QWindow>
#include <QStyle>
#include <QDateTime>
#include <algorithm>
#include <execution>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QClipboard>
#include <QShortcut>
#include <QApplication>
#include <QProcess>
#include <QMessageBox>
#include <QInputDialog>
#include <QPointer>
#include <QThreadStorage>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrent>
#include <QDir>
#include <QReadLocker>
#include <QWriteLocker>
#include <numeric>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <ntddstor.h>

#include "ScanController.h"
#include "JustifiedView.h"
#include "ThumbnailDelegate.h"
#include "QuickLookWindow.h"
#include "ToolTipOverlay.h"
#include "HoverEventFilter.h"
#include <QResizeEvent>
#include <memory>
#include <algorithm>
#include <QWidgetAction>

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

// --- ScanDialog Implementation ---

ScanDialog::ScanDialog(QWidget* parent)
    : FramelessDialog("FERREX-META", parent), m_config(ConfigManager::instance().getConfig())
{
    // 强制首层主线程实例化 ToolTipOverlay，安全冷启动
    (void)ToolTipOverlay::instance();

    // 初始化子系统控制器实例
    m_resizeFilter = new FramelessResizeBorder(this);
    m_historyDropdownController = new HistoryDropdownController(this);
    m_contextMenuExecutor = new ContextMenuExecutor(this);
    m_thumbnailWarmupPipeline = new ThumbnailWarmupPipeline(this);
    m_globalKeyboardShortcutHandler = new GlobalKeyboardShortcutHandler(this);
    m_viewportTooltipController = new ViewportTooltipController(this);

    QCoreApplication::instance()->installEventFilter(m_resizeFilter);

    if (!UiHelper::isRunAsAdmin()) {
        QMessageBox::critical(nullptr, "权限不足", "访问 MFT/USN 需要管理员权限。\n请右键以管理员身份运行程序。");
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }

    m_config.load();

    m_configSaveTimer = new QTimer(this);
    m_configSaveTimer->setInterval(1000);
    m_configSaveTimer->setSingleShot(true);
    connect(m_configSaveTimer, &QTimer::timeout, this, [this]() {
        m_config.save();
    });

    m_zoomDebounceTimer = new QTimer(this);
    m_zoomDebounceTimer->setInterval(200);
    m_zoomDebounceTimer->setSingleShot(true);
    connect(m_zoomDebounceTimer, &QTimer::timeout, this, [this]() {
        m_tableModel->clearThumbCache(true);
        m_tableModel->updateResults();
    });

    resize(1000, 700);
    setMinimumSize(800, 500);

    m_titleStatusLabel = new QLabel("READY - 0");
    m_titleStatusLabel->setStyleSheet("background: transparent; color: #46B478; font-size: 10px; font-weight: bold; margin-left: 1px;");

    if (m_titleLabel && m_pinBtn && m_pinBtn->parentWidget() && m_pinBtn->parentWidget()->layout()) {
        m_titleLabel->hide(); 
        auto* titleLayout = qobject_cast<QHBoxLayout*>(m_pinBtn->parentWidget()->layout());
        if (titleLayout) {
            m_pinBtn->parentWidget()->setFixedHeight(34);
            titleLayout->setSpacing(4);
            titleLayout->setContentsMargins(12, 0, 8, 0);

            QLabel* logoLabel = new QLabel();
            logoLabel->setFixedSize(16, 16);
            logoLabel->setPixmap(UiHelper::getIcon("ferrex", QColor("#FF8C00"), 16).pixmap(16, 16));
            logoLabel->setStyleSheet("background: transparent; margin: 0px; padding: 0px;"); 
            titleLayout->insertWidget(0, logoLabel);
            
            QLabel* brandLabel = new QLabel("FERREX-META");
            brandLabel->setObjectName("TitleBrandLabel");
            brandLabel->setStyleSheet("background: transparent; color: #FF8C00; font-size: 14px; font-weight: bold; letter-spacing: 1.5px; margin-left: 0px; padding: 0px;");
            titleLayout->insertWidget(1, brandLabel);
            
            titleLayout->insertWidget(2, m_titleStatusLabel);
            titleLayout->insertStretch(titleLayout->indexOf(m_pinBtn));

            auto* hoverFilter = new HoverEventFilter(this);

            QPushButton* viewBtn = new QPushButton(); 
            viewBtn->setFixedSize(20, 20); 
            viewBtn->setIcon(UiHelper::getIcon("grid", QColor("#CCCCCC"), 16)); 
            viewBtn->setIconSize(QSize(16, 16));
            viewBtn->setCursor(Qt::PointingHandCursor); 
            viewBtn->setToolTip(""); 
            viewBtn->setProperty("tooltipText", "排列方式"); 
            viewBtn->installEventFilter(hoverFilter); 
            viewBtn->setStyleSheet( 
                "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 0; }" 
                "QPushButton:hover { background: rgba(255, 255, 255, 0.1); }" 
                "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }" 
            ); 
            connect(viewBtn, &QPushButton::clicked, this, [this, viewBtn]() { 
                QMenu* menu = new QMenu(this); 
                menu->setStyleSheet( 
                    "QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; border-radius: 6px; }" 
                    "QMenu::item { padding: 6px 24px; }" 
                    "QMenu::item:selected { background: #2A2A2A; color: #FFF; }" 
                    "QMenu::item:checked { color: #FF8C00; }" 
                ); 

                QAction* jModeAct = menu->addAction("自适应"); 
                jModeAct->setCheckable(true); 
                jModeAct->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 0); 

                QAction* gModeAct = menu->addAction("网格"); 
                gModeAct->setCheckable(true); 
                gModeAct->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 1); 

                QAction* listModeAct = menu->addAction("列表"); 
                listModeAct->setCheckable(true); 
                listModeAct->setChecked(m_config.viewMode == 0); 

                QActionGroup* modeGrp = new QActionGroup(menu); 
                modeGrp->addAction(jModeAct); 
                modeGrp->addAction(gModeAct); 
                modeGrp->addAction(listModeAct); 

                connect(jModeAct, &QAction::triggered, this, [this]() { 
                    switchToView(1, 0);
                }); 
                connect(gModeAct, &QAction::triggered, this, [this]() { 
                    switchToView(1, 1);
                }); 
                connect(listModeAct, &QAction::triggered, this, [this]() { 
                    switchToView(0, 0);
                }); 

                menu->exec(viewBtn->mapToGlobal(QPoint(0, viewBtn->height() + 2))); 
            }); 

            m_sizeSlider = new QSlider(Qt::Horizontal); 
            m_sizeSlider->setRange(32, 256); 
            m_sizeSlider->setValue(m_config.iconSize > 0 ? m_config.iconSize : 64); 
            m_sizeSlider->setFixedSize(110, 20); 
            m_sizeSlider->setCursor(Qt::PointingHandCursor); 
            m_sizeSlider->installEventFilter(this);
            m_sizeSlider->setStyleSheet( 
                "QSlider { background: transparent; margin-right: 1px; }"
                "QSlider::groove:horizontal { height: 3px; background: #3F3F3F; border-radius: 2px; }" 
                "QSlider::sub-page:horizontal { background: #FF8C00; border-radius: 2px; }" 
                "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0; " 
                "  background: #FF8C00; border-radius: 6px; }" 
            ); 
            connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int v) { 
                m_config.iconSize = v; 
                if (m_currentActiveView) {
                    m_currentActiveView->setIconSize(v);
                }
                
                if (m_config.viewMode == 0 && m_listResultView) {
                    auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
                    if (resultTableView) {
                        int minWidth = calculateNameColumnMinimumWidth();
                        resultTableView->setColumnWidth(0, minWidth);
                    }
                }

                m_zoomDebounceTimer->start();
                m_configSaveTimer->start();
            }); 
            
            QPushButton* rulesBtn = new QPushButton();
            rulesBtn->setFixedSize(20, 20);
            rulesBtn->setIcon(UiHelper::getIcon("settings", QColor("#CCCCCC"), 16));
            rulesBtn->setIconSize(QSize(16, 16));
            rulesBtn->setCursor(Qt::PointingHandCursor);
            rulesBtn->setToolTip(""); 
            rulesBtn->setProperty("tooltipText", "预览配置"); 
            rulesBtn->installEventFilter(hoverFilter); 
            rulesBtn->setStyleSheet(
                "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 0; }"
                "QPushButton:hover { background: rgba(255, 255, 255, 0.1); }"
                "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
            );
            connect(rulesBtn, &QPushButton::clicked, this, [this]() {
                static QPointer<PreviewRulesDialog> activeDlg;
                if (activeDlg) {
                    activeDlg->raise();
                    activeDlg->activateWindow();
                    return;
                }
                auto* dlg = new PreviewRulesDialog(m_config, nullptr);
                activeDlg = dlg;
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                connect(dlg, &QDialog::accepted, this, [this]() {
                    m_config.save();
                });
                connect(this, &QObject::destroyed, dlg, &QObject::deleteLater);
                dlg->show();
            });
            
            titleLayout->insertWidget(titleLayout->indexOf(m_pinBtn), viewBtn);
            titleLayout->insertWidget(titleLayout->indexOf(viewBtn), rulesBtn);
            titleLayout->insertWidget(titleLayout->indexOf(rulesBtn), m_sizeSlider);

            for (auto* btn : {m_pinBtn, m_minBtn, m_maxBtn}) {
                if (!btn) continue;
                btn->setFixedSize(20, 20);
                btn->setIconSize(QSize(16, 16));
                btn->setToolTip(""); 
                
                if (btn == m_pinBtn) {
                    btn->setProperty("tooltipText", "置顶");
                    btn->installEventFilter(hoverFilter);
                    btn->setStyleSheet(
                        "QPushButton { background: transparent; border: none; border-radius: 4px; } "
                        "QPushButton:hover { background: rgba(255, 255, 255, 0.1); } "
                        "QPushButton:checked { background: rgba(255, 85, 28, 0.2); }"
                    );
                } else {
                    if (btn == m_minBtn) {
                        btn->setProperty("tooltipText", "最小化");
                        btn->installEventFilter(hoverFilter);
                    } else if (btn == m_maxBtn) {
                        btn->setProperty("tooltipText", "最大化");
                        btn->installEventFilter(hoverFilter);
                    }
                    btn->setStyleSheet(
                        "QPushButton { background: transparent; border: none; border-radius: 4px; } "
                        "QPushButton:hover { background: rgba(255, 255, 255, 0.1); } "
                        "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
                    );
                }
            }
            if (m_closeBtn) {
                m_closeBtn->setFixedSize(20, 20);
                m_closeBtn->setIconSize(QSize(16, 16));
                m_closeBtn->setToolTip("");
                m_closeBtn->setStyleSheet(
                    "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
                    "QPushButton:hover { background-color: #E81123; } "
                    "QPushButton:pressed { background-color: #A50000; }"
                );
            }
        } else {
            m_titleStatusLabel->hide(); 
        }
    } else {
        m_titleStatusLabel->hide();
    }

    setupUi();

    this->setStyleSheet(this->styleSheet() + R"(
        #TitleBrandLabel {
            background: transparent; 
            color: #FF8C00; 
            font-size: 14px; 
            font-weight: bold; 
            letter-spacing: 1.5px; 
            margin-left: -4px; 
            padding: 0px;
        }

        #DialogContainer {
            background-color: #1E1E1E;
            border: 1px solid #333333;
            border-radius: 6px;
        }

        QWidget#SearchContainer, QWidget#DriveContainer { 
            background: transparent; border: none; 
        }

        QStackedWidget#ViewStack {
            background-color: #1E1E1E;
            border: none;
        }
        
        #mainSearchEdit, #extSearchEdit { 
            background: #2D2D2D; 
            border: 1px solid #FF8C00; 
            border-radius: 6px; 
            color: #EEE; 
            font-size: 14px; 
            padding: 0 10px;
            outline: none;
        }

        #mainSearchEdit:focus, #extSearchEdit:focus { border: 1px solid #FF8C00 !important; }
        #mainSearchEdit:hover, #extSearchEdit:hover { border: 1px solid #FF8C00; }
        
        #mainSearchEdit::placeholder, #extSearchEdit::placeholder {
            color: rgba(238, 238, 238, 0.3);
        }

        QPushButton#searchIconButton { 
            background: #FF8C00; 
            border: 1px solid #FF8C00;
            border-radius: 6px; 
            color: #000;
            font-weight: bold;
            padding: 0 15px;
        } 
        QPushButton#searchIconButton:hover { background: #FFA500; } 
        QPushButton#searchIconButton:pressed { background: #CC6600; }

        QPushButton[isActive="true"] {
            background: rgba(255, 140, 0, 30); 
            color: #FF8C00; 
            border: 1px solid #FF8C00; 
            padding: 0 10px; 
            font-size: 12px; 
            font-weight: bold;
            border-radius: 4px;
        }
        QPushButton[isActive="false"] {
            background: #111519; 
            color: #7A8F9E; 
            border: 1px solid #252E37; 
            padding: 0 10px; 
            font-size: 12px;
            border-radius: 4px;
        }

        QProgressBar#ScanProgressBar { background: transparent; border: none; } 
        QProgressBar#ScanProgressBar::chunk { background: #FF8C00; }

        QCheckBox { color: #AAA; }

        QScrollBar:vertical {
            border: none;
            background: transparent;
            width: 7px;
            margin: 0px;
        }
        QScrollBar::handle:vertical {
            background: #333333;
            min-height: 20px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover {
            background: #444444;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: none;
        }

        QScrollBar:horizontal {
            border: none;
            background: transparent;
            height: 7px;
            margin: 0px;
        }
        QScrollBar::handle:horizontal {
            background: #333333;
            min-width: 20px;
            border-radius: 3px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #444444;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
            background: none;
        }
    )");

    switchToView(m_config.viewMode, m_config.layoutMode);
    
    if (m_listResultView) {
        auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
        if (resultTableView) {
            resultTableView->horizontalHeader()->setSortIndicator(m_config.sortColumn, static_cast<Qt::SortOrder>(m_config.sortOrder));
        }
    }
    m_tableModel->sort(m_config.sortColumn, static_cast<Qt::SortOrder>(m_config.sortOrder));

    connect(&MftReader::instance(), &MftReader::driveLoaded, this, [this](const QString& drive, int count, int total) {
        updateStatus(QString("正在加载快照 %1 (%2)...").arg(drive).arg(formatNumber(count)), true, total);
    });

    connect(&MftReader::instance(), &MftReader::entriesChangedBatch, this, [this]() { updateStatus("就绪"); });

    if (m_pinBtn) {
        disconnect(m_pinBtn, &QPushButton::toggled, nullptr, nullptr);
        connect(m_pinBtn, &QPushButton::toggled, this, [this](bool checked) {
            m_pinBtn->setIcon(UiHelper::getIcon(checked ? "pin_vertical" : "pin_tilted", 
                                                checked ? QColor("#FF551C") : QColor("#CCCCCC"), 18));
            HWND hwnd = reinterpret_cast<HWND>(winId());
            if (checked) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            } else {
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
        });
    }

    QTimer::singleShot(100, this, [this]() {
        updateStatus("按需初始化中...");
        QPointer<ScanDialog> weakThis(this);
        (void)(QtConcurrent::run)([weakThis]() {
            if (!weakThis) return;
            bool anyLoaded = false;
            QStringList toLoad;
            for (const QString& d : weakThis->m_config.defaultDrives) toLoad << d;
            
            if (toLoad.isEmpty()) toLoad << "C:";

            QStringList toScan;
            for (const QString& d : toLoad) {
                if (MftReader::instance().loadDriveFromCache(d)) anyLoaded = true;
                else toScan << d;
            }

            if (!toScan.isEmpty()) {
                QMetaObject::invokeMethod(weakThis.data(), [weakThis, toScan]() {
                    if (weakThis) {
                        for (const QString& d : toScan) weakThis->onStartScan(d);
                    }
                });
            }

            QMetaObject::invokeMethod(weakThis.data(), [weakThis, anyLoaded]() {
                if (!weakThis) return;
                weakThis->updateStatus("就绪");
                weakThis->m_controller->setSearchText("");
                weakThis->refreshDriveList(true); 
                if (weakThis->m_config.autoDisplay) weakThis->onFilterOptionChanged();
                
                weakThis->triggerWarmup();
            });
        });
    });

    m_actJMode = new QAction("自适应(A)", this);
    m_actJMode->setCheckable(true);
    connect(m_actJMode, &QAction::triggered, this, [this]() {
        switchToView(1, 0);
    });

    m_actGMode = new QAction("网格(G)", this);
    m_actGMode->setCheckable(true);
    connect(m_actGMode, &QAction::triggered, this, [this]() {
        switchToView(1, 1);
    });

    m_actListMode = new QAction("列表(L)", this);
    m_actListMode->setCheckable(true);
    connect(m_actListMode, &QAction::triggered, this, [this]() {
        switchToView(0, 0);
    });

    QActionGroup* modeGrp = new QActionGroup(this);
    modeGrp->addAction(m_actJMode);
    modeGrp->addAction(m_actGMode);
    modeGrp->addAction(m_actListMode);
}

ScanDialog::~ScanDialog() {
    if (m_configSaveTimer) { m_configSaveTimer->stop(); delete m_configSaveTimer; m_configSaveTimer = nullptr; }
    if (m_zoomDebounceTimer) { m_zoomDebounceTimer->stop(); delete m_zoomDebounceTimer; m_zoomDebounceTimer = nullptr; }

    m_config.save();

    if (m_resizeFilter) {
        QCoreApplication::instance()->removeEventFilter(m_resizeFilter);
    }
}


void ScanDialog::switchToView(int viewMode, int layoutMode) {
    // 还原旧版本-3：零查询视图切换逻辑（还原设计三），此处彻底移除对 onTriggerSearch() 的多余调用，实现零耗时的秒级模式切换，且完美不丢失当前的选中项
    m_config.viewMode = viewMode;
    m_config.layoutMode = layoutMode;

    if (viewMode == 0) {
        m_currentActiveView = m_listResultView;
    } else if (layoutMode == 0) {
        m_currentActiveView = m_justifiedResultView;
    } else {
        m_currentActiveView = m_gridResultView;
    }

    if (m_currentActiveView) {
        m_viewStack->setCurrentWidget(m_currentActiveView->getWidget());
        m_currentActiveView->setIconSize(m_config.iconSize);
        m_currentActiveView->refreshLayout();
    }

    // 还原设计三：switchToView 的末尾还原为直接调用 m_tableModel->updateResults();
    m_tableModel->updateResults();
    m_configSaveTimer->start();
}

void ScanDialog::closeEvent(QCloseEvent* event) {
    m_config.save();
    hide();
    event->ignore();
}

void ScanDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(m_contentArea);
    mainLayout->setContentsMargins(10, 10, 10, 0);
    mainLayout->setSpacing(10);

    auto* driveScroll = new QScrollArea();
    driveScroll->setFixedHeight(45);
    driveScroll->setWidgetResizable(true);
    driveScroll->setFrameShape(QFrame::NoFrame);
    driveScroll->setStyleSheet("background: #252526; border: 1px solid #333; border-radius: 4px;");

    m_driveContainer = new QWidget();
    m_driveContainer->setObjectName("DriveContainer");
    m_driveContainer->setAttribute(Qt::WA_StyledBackground, true);
    m_driveLayout = new QHBoxLayout(m_driveContainer);
    m_driveLayout->setContentsMargins(10, 0, 5, 0);
    m_driveLayout->setSpacing(10);
    driveScroll->setWidget(m_driveContainer);

    auto* topControl = new QHBoxLayout();
    topControl->setContentsMargins(0, 0, 0, 0);
    topControl->addWidget(driveScroll, 1);
    mainLayout->addLayout(topControl);

    auto* optionRow = new QHBoxLayout();
    optionRow->setContentsMargins(0, 0, 0, 0);
    optionRow->setSpacing(15);
    
    m_checkRegex = new QCheckBox("正则");
    m_checkCase = new QCheckBox("大小写");
    m_checkHidden = new QCheckBox("隐藏");
    m_checkSystem = new QCheckBox("系统");
    m_checkDollar = new QCheckBox("显示$");
    m_checkAuto = new QCheckBox("自动显示");

    m_checkRegex->setChecked(m_config.useRegex);
    m_checkCase->setChecked(m_config.caseSensitive);
    m_checkHidden->setChecked(m_config.includeHidden);
    m_checkSystem->setChecked(m_config.includeSystem);
    m_checkDollar->setChecked(m_config.includeDollar);
    m_checkAuto->setChecked(m_config.autoDisplay);

    for (auto* cb : {m_checkRegex, m_checkCase, m_checkHidden, m_checkSystem, m_checkDollar, m_checkAuto}) {
        connect(cb, &QCheckBox::toggled, this, &ScanDialog::onFilterOptionChanged);
        optionRow->addWidget(cb);
    }
    optionRow->addStretch();
    mainLayout->addLayout(optionRow);

    auto* searchContainer = new QWidget();
    searchContainer->setObjectName("SearchContainer");
    searchContainer->setAttribute(Qt::WA_StyledBackground, true);
    auto* searchVLayout = new QVBoxLayout(searchContainer);
    searchVLayout->setContentsMargins(0, 0, 0, 0);
    searchVLayout->setSpacing(10); 

    auto* searchRow = new QHBoxLayout();
    searchRow->setContentsMargins(0, 0, 0, 0); 
    searchRow->setSpacing(10); 

    m_searchEdit = new QLineEdit();
    m_searchEdit->setObjectName("mainSearchEdit");
    m_searchEdit->setPlaceholderText("输入文件名 / 关键词...");
    m_searchEdit->setFixedHeight(36);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_controller->setSearchText(text);
        if (text.isEmpty()) {
            m_controller->triggerSearch(true);
        } else {
            m_controller->triggerSearch(false);
        }
    });
    connect(m_searchEdit, &QLineEdit::editingFinished, this, [this]() { m_config.save(); });
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_searchEdit, 1);

    m_extEdit = new QLineEdit();
    m_extEdit->setObjectName("extSearchEdit");
    m_extEdit->setPlaceholderText("后缀");
    m_extEdit->setFixedWidth(120); 
    m_extEdit->setFixedHeight(36);
    m_extEdit->setClearButtonEnabled(true);
    m_extEdit->installEventFilter(this);
    connect(m_extEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        ScanFilterState state;
        state.useRegex = m_checkRegex->isChecked();
        state.caseSensitive = m_checkCase->isChecked();
        state.includeHidden = m_checkHidden->isChecked();
        state.includeSystem = m_checkSystem->isChecked();
        state.includeDollar = m_checkDollar->isChecked();
        state.autoDisplay = m_checkAuto->isChecked();
        QString extText = m_extEdit->text().toLower();
        if (!extText.isEmpty()) state.extensionList = extText.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
        
        m_controller->setFilterState(state);
        m_controller->triggerSearch(false); 
    });
    connect(m_extEdit, &QLineEdit::editingFinished, this, [this]() { m_config.save(); });
    connect(m_extEdit, &QLineEdit::returnPressed, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_extEdit);

    m_searchBtn = new QPushButton("搜索");
    m_searchBtn->setObjectName("searchIconButton");
    m_searchBtn->setFixedWidth(80);
    m_searchBtn->setFixedHeight(36); 
    m_searchBtn->setCursor(Qt::PointingHandCursor);
    m_searchBtn->setIcon(UiHelper::getIcon("search", QColor("#000000"), 18));
    m_searchBtn->setIconSize(QSize(18, 18));
    connect(m_searchBtn, &QPushButton::clicked, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_searchBtn);

    searchVLayout->addLayout(searchRow);

    m_progressBar = new QProgressBar();
    m_progressBar->setObjectName("ScanProgressBar");
    m_progressBar->setFixedHeight(2);
    m_progressBar->setTextVisible(false);
    m_progressBar->hide();
    searchVLayout->addWidget(m_progressBar);

    mainLayout->addWidget(searchContainer);

    m_controller = new ScanController(this);
    m_tableModel = new ScanTableModel(m_controller, this);

    m_listResultView = new ListResultView(this);
    m_justifiedResultView = new JustifiedResultView(this);
    m_gridResultView = new GridResultView(this);

    m_listResultView->setModel(m_tableModel);
    m_justifiedResultView->setModel(m_tableModel);
    m_gridResultView->setModel(m_tableModel);

    auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());

    // Apply the header drag and width auto-restoration constraint on resultTableView [1]
    if (resultTableView) {
        connect(resultTableView->horizontalHeader(), &QHeaderView::sectionResized, this, [this, resultTableView](int logicalIndex, int /*oldSize*/, int newSize) {
            if (!m_tableModel) return;

            if (logicalIndex == 0) {
                int minWidth = calculateNameColumnMinimumWidth();

                // 动态上限拦截，确保不挤死右侧各列空间
                // 路径列预留宽度与 ListResultView::setMinimumSectionSize(60) 保持一致口径
                int viewportWidth = resultTableView->viewport()->width();
                if (viewportWidth <= 0) viewportWidth = resultTableView->width();
                int pathColumnFloor = resultTableView->horizontalHeader()->minimumSectionSize();
                int reservedWidth = pathColumnFloor + qMax<int>(80, resultTableView->columnWidth(2)) + qMax<int>(90, resultTableView->columnWidth(3));
                int maxWidth = viewportWidth - reservedWidth;
                if (maxWidth < minWidth) maxWidth = minWidth;

                if (newSize < minWidth) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(0, minWidth);
                    resultTableView->horizontalHeader()->blockSignals(false);
                } else if (newSize > maxWidth) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(0, maxWidth);
                    resultTableView->horizontalHeader()->blockSignals(false);
                }
            }
            else if (logicalIndex == 2) {
                // 用户手动调整“大小”列，限制其宽度不得小于 80 像素
                if (newSize < 80) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(2, 80);
                    resultTableView->horizontalHeader()->blockSignals(false);
                }
            }
            else if (logicalIndex == 3) {
                // 用户手动调整“修改日期”列，限制其宽度不得小于 90 像素
                if (newSize < 90) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(3, 90);
                    resultTableView->horizontalHeader()->blockSignals(false);
                }
            }
        });
    }

    m_viewStack = new QStackedWidget();
    m_viewStack->setObjectName("ViewStack");
    m_viewStack->addWidget(m_listResultView->getWidget());
    m_viewStack->addWidget(m_justifiedResultView->getWidget());
    m_viewStack->addWidget(m_gridResultView->getWidget());

    for (auto* resView : {m_listResultView, m_justifiedResultView, m_gridResultView}) {
        QAbstractItemView* base = resView->getBaseView();
        
        base->installEventFilter(this);
        base->viewport()->installEventFilter(this);
        base->viewport()->setMouseTracking(true);

        connect(base->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
            refreshVisibleMetadataRange();
        });

        connect(base, &QAbstractItemView::doubleClicked, this, &ScanDialog::onItemDoubleClicked);
        connect(base, &QAbstractItemView::customContextMenuRequested, this, &ScanDialog::onCustomContextMenu);
        connect(base->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ScanDialog::onSelectionChanged);
    }

    m_quickLook = new QuickLookWindow(this);
    connect(m_quickLook, &QuickLookWindow::prevRequested, this, [this]() {
        auto* view = m_currentActiveView->getBaseView();
        int row = view->currentIndex().row();
        if (row > 0) {
            QModelIndex nextIdx = m_tableModel->index(row - 1, 0);
            view->setCurrentIndex(nextIdx);
            QString path = m_tableModel->data(m_tableModel->index(row - 1, 1)).toString();
            m_quickLook->preview(path);
        }
    });
    connect(m_quickLook, &QuickLookWindow::nextRequested, this, [this]() {
        auto* view = m_currentActiveView->getBaseView();
        int row = view->currentIndex().row();
        if (row < m_tableModel->rowCount() - 1) {
            QModelIndex nextIdx = m_tableModel->index(row + 1, 0);
            view->setCurrentIndex(nextIdx);
            QString path = m_tableModel->data(m_tableModel->index(row + 1, 1)).toString();
            m_quickLook->preview(path);
        }
    });

    mainLayout->addWidget(m_viewStack);

    auto* statusContainer = new QWidget();
    statusContainer->setObjectName("StatusContainer");
    statusContainer->setFixedHeight(20);
    statusContainer->setStyleSheet("QWidget#StatusContainer { background: transparent; border: none; }");
    auto* statusBar = new QHBoxLayout(statusContainer);
    statusBar->setAlignment(Qt::AlignVCenter);
    statusBar->setContentsMargins(16, 0, 16, 10);
    statusBar->setSpacing(0);

    m_statLabelMain = new QLabel("");
    m_statLabelMain->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_statLabelMain);

    m_statLabelTime = new QLabel("");
    m_statLabelTime->setStyleSheet("color: #7A8F9E; font-size: 10px; margin-left: 12px;");
    statusBar->addWidget(m_statLabelTime);

    m_selectionLabel = new QLabel("");
    m_selectionLabel->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_selectionLabel);

    statusBar->addStretch();

    m_statLabelMemory = new QLabel("");
    m_statLabelMemory->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_statLabelMemory);

    mainLayout->addWidget(statusContainer);

    connect(m_controller, &ScanController::searchFinished, this, [this](int count, int64_t elapsedMs) {
        Q_UNUSED(count);
        m_lastSearchMs = elapsedMs;
        m_tableModel->updateResults();
        updateStatus("就绪");
        refreshVisibleMetadataRange();
        
        if (m_config.viewMode == 0 && m_listResultView) {
            auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
            if (resultTableView) {
                resultTableView->setColumnWidth(0, calculateNameColumnMinimumWidth());
            }
        }
    });

    connect(m_controller, &ScanController::resultsSwapped, this, [this]() {
        updateStatus("就绪");
        refreshVisibleMetadataRange();

        if (m_config.viewMode == 0 && m_listResultView) {
            auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
            if (resultTableView) {
                resultTableView->setColumnWidth(0, calculateNameColumnMinimumWidth());
            }
        }
    });

    showDriveLoading();
}

void ScanDialog::showDriveLoading() {
    if (!m_driveLayout) return;

    QLayoutItem* child;
    while ((child = m_driveLayout->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }
    QLabel* loadingLbl = new QLabel("更新数据中...");
    loadingLbl->setStyleSheet("background: transparent; border: none; color: #7A8F9E; font-size: 12px; font-weight: bold; margin-left: 10px;");
    m_driveLayout->addWidget(loadingLbl);
    m_driveLayout->addStretch();
}

void ScanDialog::refreshDriveList(bool forceProbe) {
    if (!forceProbe && !m_cachedDriveInfos.isEmpty()) {
        updateDriveButtonStyles();
        return;
    }

    showDriveLoading();

    QPointer<ScanDialog> weakThis(this);
    (void)(QtConcurrent::run)([weakThis]() {
        if (!weakThis) return;
        QVector<DriveInfo> drives = SystemDriveScanner::scanDrives();

        QMetaObject::invokeMethod(weakThis.data(), [weakThis, drives]() {
            if (!weakThis) return;
            weakThis->m_cachedDriveInfos = drives;
            
            if (weakThis->m_config.activeDrives.isEmpty()) {
                for (const auto& info : drives) {
                    if (info.hasMedia && info.isNtfs) {
                        weakThis->m_config.activeDrives.insert(info.letter);
                    }
                }
                weakThis->m_config.save();
            }

            QLayoutItem* item;
            while ((item = weakThis->m_driveLayout->takeAt(0)) != nullptr) {
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
            weakThis->m_driveButtonMap.clear();

            for (const auto& info : drives) {
                if (!info.hasMedia) continue; 
                if (!info.isNtfs && info.letter != "C:") continue;

                QString label = info.label.isEmpty() ? "本地磁盘" : info.label;
                QString btnText = QString("%1 (%2)").arg(info.letter).arg(label);
                
                QPushButton* btn = new QPushButton(btnText);
                btn->setCheckable(true);
                btn->setFixedHeight(24);
                weakThis->m_driveButtonMap[info.letter] = btn;
                
                connect(btn, &QPushButton::clicked, weakThis.data(), [weakThis, letter = info.letter]() {
                    if (!weakThis) return;
                    bool isSelected = false;
                    if (weakThis->m_config.activeDrives.contains(letter)) {
                        if (weakThis->m_config.activeDrives.size() > 1) {
                            weakThis->m_config.activeDrives.remove(letter);
                        } else {
                            isSelected = true; 
                        }
                    } else {
                        weakThis->m_config.activeDrives.insert(letter);
                        isSelected = true;
                    }
                    
                    weakThis->updateDriveButtonStyles();

                    QStringList activeList;
                    for (const QString& d : weakThis->m_config.activeDrives) activeList << d;
                    MftReader::instance().updateActiveDrives(activeList);

                    if (isSelected && !MftReader::instance().isDriveIndexed(letter)) {
                        weakThis->updateStatus("请先通过右键菜单‘加载数据’");
                        weakThis->m_config.activeDrives.remove(letter);
                        weakThis->updateDriveButtonStyles();
                    } else {
                        weakThis->onTriggerSearch();
                    }
                });
                
                btn->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(btn, &QPushButton::customContextMenuRequested, weakThis.data(), [weakThis, letter = info.letter](const QPoint& pos) {
                    if (weakThis) weakThis->onDriveContextMenu(letter, pos);
                });
                
                weakThis->m_driveLayout->addWidget(btn);
            }
            weakThis->m_driveLayout->addStretch();
            weakThis->updateDriveButtonStyles();
        });
    });
}

void ScanDialog::updateDriveButtonStyles() {
    for (auto it = m_driveButtonMap.begin(); it != m_driveButtonMap.end(); ++it) {
        bool isActive = m_config.activeDrives.contains(it.key());
        bool isDefault = m_config.defaultDrives.contains(it.key());
        bool isLoaded = MftReader::instance().isDriveIndexed(it.key());
        
        QPushButton* btn = it.value();
        btn->setProperty("isActive", isActive);
        btn->setProperty("isDefault", isDefault);
        btn->setProperty("isLoaded", isLoaded);
        
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
        
        QString label = "";
        for (const auto& info : m_cachedDriveInfos) { if (info.letter == it.key()) { label = info.label; break; } }
        QString statusSuffix = isLoaded ? "" : " [未加载]";
        btn->setText(QString("%1%2 (%3)%4").arg(isDefault ? "★ " : "").arg(it.key()).arg(label.isEmpty() ? "本地磁盘" : label).arg(statusSuffix));
        
        if (!isLoaded) {
            btn->setStyleSheet(btn->styleSheet() + " color: #555; ");
        } else {
            btn->setStyleSheet(btn->styleSheet().remove(" color: #555; "));
        }
    }
}

void ScanDialog::onDriveContextMenu(const QString& drive, const QPoint& /*pos*/) {
    QMenu menu(this);
    menu.setStyleSheet("QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; } QMenu::item:selected { background: #232D37; color: #FFF; }");
    
    bool isDefault = m_config.defaultDrives.contains(drive);
    menu.addAction(isDefault ? "取消默认选项" : "设为默认选项", [this, drive, isDefault]() {
        if (isDefault) m_config.defaultDrives.remove(drive);
        else m_config.defaultDrives.insert(drive);
        m_config.save();
        updateDriveButtonStyles();
    });

    menu.addSeparator();

    bool isLoaded = MftReader::instance().isDriveIndexed(drive);
    auto* loadAct = menu.addAction("加载数据 (快速)", [this, drive]() {
        updateStatus(QString("正在加载 %1...").arg(drive), true);
        QPointer<ScanDialog> weakThis(this);
        (void)QtConcurrent::run([weakThis, drive]() {
            if (!MftReader::instance().loadDriveFromCache(drive)) {
                QMetaObject::invokeMethod(weakThis.data(), [weakThis, drive]() {
                    if (weakThis) weakThis->onStartScan(drive);
                });
            } else {
                QMetaObject::invokeMethod(weakThis.data(), [weakThis]() {
                    if (weakThis) {
                        weakThis->updateStatus("就绪");
                        weakThis->updateDriveButtonStyles();
                        weakThis->onTriggerSearch();
                        weakThis->triggerWarmup();
                    }
                });
            }
        });
    });
    loadAct->setEnabled(!isLoaded);

    auto* scanAct = menu.addAction("立即扫描并索引", [this, drive]() {
        onStartScan(drive);
    });
    scanAct->setEnabled(!isLoaded);

    auto* unloadAct = menu.addAction("卸载数据", [this, drive]() {
        MftReader::instance().unloadDrive(drive);
        updateDriveButtonStyles();
        onTriggerSearch();
    });
    unloadAct->setEnabled(isLoaded);
    
    menu.exec(QCursor::pos());
}

void ScanDialog::onCustomContextMenu(const QPoint& pos) {
    if (m_contextMenuExecutor) {
        m_contextMenuExecutor->executeContextMenu(pos);
    }
}

void ScanDialog::onItemDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    
    QString path = m_tableModel->data(m_tableModel->index(index.row(), 1)).toString();
    ShellHelper::openInExplorer(path);
}

void ScanDialog::onSelectionChanged() {
    updateStatus("就绪");
}


void ScanDialog::onStartScan(const QString& drive) {
    QStringList selectedDrives;
    if (drive.isEmpty()) {
        for (const auto& d : m_config.activeDrives) selectedDrives << (d + QLatin1String("\\"));
    } else {
        selectedDrives << (drive + QLatin1String("\\"));
    }
    
    if (selectedDrives.isEmpty()) { onTriggerSearch(); return; }
    updateStatus("正在扫描...", true);

    QPointer<ScanDialog> weakThis(this);
    (void)(QtConcurrent::run)([weakThis, selectedDrives]() {
        MftReader::instance().buildIndex(selectedDrives);
        QMetaObject::invokeMethod(weakThis.data(), [weakThis]() {
            if (!weakThis) return;
            weakThis->updateStatus("就绪");
            weakThis->updateDriveButtonStyles();
            weakThis->onTriggerSearch();
            weakThis->triggerWarmup();
        });
    });
}

void ScanDialog::onTriggerSearch() {
    QString q = m_searchEdit->text().trimmed();
    QString e = m_extEdit->text().trimmed();
    
    QTimer::singleShot(10, this, [this, q, e]() {
        bool changed = false;
        if (!q.isEmpty() && (m_config.queryHistory.isEmpty() || m_config.queryHistory.first() != q)) {
            m_config.queryHistory.removeAll(q);
            m_config.queryHistory.prepend(q);
            if (m_config.queryHistory.size() > 10) m_config.queryHistory.removeLast();
            changed = true;
        }
        if (!e.isEmpty() && (m_config.extHistory.isEmpty() || m_config.extHistory.first() != e)) {
            m_config.extHistory.removeAll(e);
            m_config.extHistory.prepend(e);
            if (m_config.extHistory.size() > 10) m_config.extHistory.removeLast();
            changed = true;
        }
        if (changed) m_config.save();
    });

    QStringList activeList;
    for (const QString& drive : m_config.activeDrives) activeList << drive;
    MftReader::instance().updateActiveDrives(activeList);

    onFilterOptionChanged();
    m_controller->setSearchText(m_searchEdit->text());
    m_controller->triggerSearch(true); 
}

void ScanDialog::onFilterOptionChanged() {
    m_config.useRegex = m_checkRegex->isChecked();
    m_config.caseSensitive = m_checkCase->isChecked();
    m_config.includeHidden = m_checkHidden->isChecked();
    m_config.includeSystem = m_checkSystem->isChecked();
    m_config.includeDollar = m_checkDollar->isChecked();
    m_config.autoDisplay = m_checkAuto->isChecked();

    ScanFilterState state;
    state.useRegex = m_config.useRegex;
    state.caseSensitive = m_config.caseSensitive;
    state.includeHidden = m_config.includeHidden;
    state.includeSystem = m_config.includeSystem;
    state.includeDollar = m_config.includeDollar;
    state.autoDisplay = m_config.autoDisplay;
    QString extText = m_extEdit->text().toLower();
    if (!extText.isEmpty()) state.extensionList = extText.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
    
    m_controller->setFilterState(state);
    m_controller->triggerSearch(true);
}

void ScanDialog::updateStatus(const QString& text, bool scanning, int64_t totalCount) {
    Q_UNUSED(text);
    if (m_titleStatusLabel) {
        int64_t total = (totalCount >= 0) ? totalCount : MftReader::instance().activeCount();
        m_titleStatusLabel->setText(QString("%1 - %2").arg(scanning ? "SCANNING" : "READY").arg(formatNumber(total)));
        m_titleStatusLabel->setStyleSheet(scanning ? "color: #FF8C00; font-size: 10px; font-weight: bold;" : "color: #46B478; font-size: 10px; font-weight: bold;");
    }
    
    if (scanning) { m_progressBar->show(); m_progressBar->setRange(0, 0); }
    else { m_progressBar->hide(); updateStatusBar(); }
}

void ScanDialog::updateStatusBar() {
    if (!m_currentActiveView) return;
    auto view = m_currentActiveView->getBaseView();
    auto selectedRows = view->selectionModel()->selectedRows();
    
    int totalMatch = m_controller->resultCount();
    int loadedDrives = 0;
    for (const auto& info : m_cachedDriveInfos) {
        if (MftReader::instance().isDriveIndexed(info.letter)) loadedDrives++;
    }
    m_statLabelMain->setText(QString("当前仅在 %1 个已加载盘符范围内搜索 (匹配: %2)").arg(loadedDrives).arg(formatNumber(totalMatch)));
    m_statLabelTime->setText(QString("耗时 %1 ms").arg(m_lastSearchMs));

    if (!selectedRows.isEmpty()) {
        m_selectionLabel->show();
        int64_t totalSize = 0;
        auto& reader = MftReader::instance();
        for (const auto& index : selectedRows) {
            uint64_t key = m_tableModel->data(index, Qt::UserRole).toULongLong();
            int actualIdx = reader.getIndexByKey(key);
            if (actualIdx != -1 && !reader.isDirectory(actualIdx)) totalSize += reader.getSize(actualIdx);
        }
        m_selectionLabel->setText(QString(" | 已选择 %1 项 (%2)").arg(selectedRows.size()).arg(formatSize(totalSize)));
    } else {
        m_selectionLabel->hide();
    }
    
    int64_t dbTotal = MftReader::instance().totalCount();
    double memoryMb = (dbTotal * 184.0) / 1024.0 / 1024.0;
    m_statLabelMemory->setText(QString("索引总量: %1 | 数据占用: %2 MB").arg(formatNumber(dbTotal)).arg(memoryMb, 0, 'f', 1));
}

void ScanDialog::refreshVisibleMetadataRange() { 
    if (!m_tableModel || !m_currentActiveView) return; 
    QAbstractItemView* view = m_currentActiveView->getBaseView(); 
     
    int top = 0; 
    int bottom = m_tableModel->rowCount() - 1; 
     
    QModelIndex topIdx = view->indexAt(QPoint(10, 10)); 
    QModelIndex bottomIdx = view->indexAt(QPoint(view->viewport()->width() - 10, view->viewport()->height() - 10)); 
     
    if (topIdx.isValid()) top = topIdx.row(); 
    if (bottomIdx.isValid()) bottom = bottomIdx.row(); 
     
    m_tableModel->setVisibleRange(top, bottom); 
}

int ScanDialog::calculateNameColumnMinimumWidth() const {
    if (!m_tableModel || !m_listResultView) return 260;
    auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
    if (!resultTableView) return 260;

    int rowHeight = m_config.iconSize;
    int cardWidth = rowHeight - 6; 
    int basePadding = 6 + 10 + 10; 

    QFontMetrics fm = resultTableView->fontMetrics();
    int maxTextWidth = 100; 

    auto snapshot = m_controller->snapshot();
    if (!snapshot) return 260;
    int count = std::min<int>(1000, (int)snapshot->keys.size());
    auto& reader = MftReader::instance();

    for (int i = 0; i < count; ++i) {
        uint64_t key = snapshot->keys[i];
        int actualIdx = reader.getIndexByKey(key);
        if (actualIdx != -1) {
            QString name = reader.getName(actualIdx);
            int textWidth = fm.horizontalAdvance(name);
            if (textWidth > maxTextWidth) {
                maxTextWidth = textWidth;
            }
        }
    }

    int calculatedWidth = cardWidth + maxTextWidth + basePadding;
    int maxAllowedWidth = ResultTableColumnWidthPolicy::calculateNameColumnWidthLimit(resultTableView);
    if (calculatedWidth > maxAllowedWidth) {
        calculatedWidth = maxAllowedWidth;
    }

    return calculatedWidth;
}

QString ScanDialog::formatNumber(int64_t n) {
    return StatusBarFormatter::formatNumber(n);
}

QString ScanDialog::formatSize(int64_t bytes) {
    return StatusBarFormatter::formatSize(bytes);
}

void ScanDialog::onRenameTriggered() {
    if (!m_currentActiveView) return;
    auto view = m_currentActiveView->getBaseView();
    auto selection = view->selectionModel()->selectedRows();
    if (selection.isEmpty()) return;
    
    view->edit(selection.first());
}

void ScanDialog::onCopyTriggered(bool isCut) {
    if (!m_currentActiveView) return;
    auto view = m_currentActiveView->getBaseView();
    auto selectedIndexes = view->selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty()) return;

    QMimeData* mimeData = m_tableModel->mimeData(selectedIndexes);
    if (!mimeData) return;

    QByteArray effectData;
    QDataStream stream(&effectData, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << (isCut ? (quint32)2 : (quint32)1); 
    mimeData->setData("Preferred DropEffect", effectData);

    QApplication::clipboard()->setMimeData(mimeData);
}

void ScanDialog::setHistoryText(const QString& text, bool isQuery) {
    if (isQuery) {
        m_searchEdit->setText(text);
    } else {
        m_extEdit->setText(text);
    }
    onTriggerSearch();
}

void ScanDialog::removeHistoryItem(const QString& text, bool isQuery) {
    if (isQuery) {
        m_config.queryHistory.removeAll(text);
    } else {
        m_config.extHistory.removeAll(text);
    }
    m_config.save(); 
}

void ScanDialog::reopenHistoryMenu(bool isQuery) {
    QWidget* target = isQuery ? static_cast<QWidget*>(m_searchEdit) : static_cast<QWidget*>(m_extEdit);
    if (target) {
        QMouseEvent me(QEvent::MouseButtonDblClick, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &me);
    }
}


void ScanDialog::resizeEvent(QResizeEvent* event) {
    FramelessDialog::resizeEvent(event); 

    if (m_config.viewMode == 0 && m_listResultView) {
        auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
        if (resultTableView) {
            int minWidth = calculateNameColumnMinimumWidth();
            resultTableView->setColumnWidth(0, minWidth);
        }
    }
}

void ScanDialog::keyPressEvent(QKeyEvent* event) {
    if (m_globalKeyboardShortcutHandler && m_globalKeyboardShortcutHandler->handleKeyPress(event)) {
        return;
    }
    FramelessDialog::keyPressEvent(event);
}

void ScanDialog::selectAllResults() {
    m_tableModel->forceFetchAll();
    int total = m_tableModel->rowCount();
    if (total <= 0) return;

    if (!m_currentActiveView) return;
    auto view = m_currentActiveView->getBaseView();

    QItemSelection selection(
        m_tableModel->index(0, 0), 
        m_tableModel->index(total - 1, m_tableModel->columnCount() - 1)
    );
    view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    
    updateStatusBar();
}

void ScanDialog::handleMetadataShortcut(QKeyEvent* event) {
    Q_UNUSED(event);
}

void ScanDialog::triggerWarmup() {
    if (m_thumbnailWarmupPipeline) {
        m_thumbnailWarmupPipeline->triggerWarmup();
    }
}

bool ScanDialog::eventFilter(QObject* watched, QEvent* event) {
    if (!m_listResultView || !m_justifiedResultView || !m_gridResultView || !m_tableModel) {
        return FramelessDialog::eventFilter(watched, event);
    }

    if (m_viewportTooltipController && m_viewportTooltipController->handleEvent(watched, event)) {
        return true;
    }

    bool isViewOrViewport = false;
    QAbstractItemView* view = nullptr;

    for (auto* resView : {m_listResultView, m_justifiedResultView, m_gridResultView}) {
        if (!resView) continue;
        QAbstractItemView* base = resView->getBaseView();
        if (watched == base || watched == base->viewport()) {
            isViewOrViewport = true;
            view = base;
            break;
        }
    }

    if (isViewOrViewport && event->type() == QEvent::Wheel) {
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
        if (wheelEvent->modifiers() & Qt::ControlModifier) {
            int deltaY = wheelEvent->angleDelta().y();
            if (deltaY > 0) {
                m_sizeSlider->setValue(m_sizeSlider->value() + 10);
            } else if (deltaY < 0) {
                m_sizeSlider->setValue(m_sizeSlider->value() - 10);
            }
            return true; 
        }
    }

    if (isViewOrViewport && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space) {
            if (m_globalKeyboardShortcutHandler) {
                m_globalKeyboardShortcutHandler->handleKeyPress(keyEvent);
            }
            return true; 
        }
    }

    if (watched == m_sizeSlider && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            int val = QStyle::sliderValueFromPosition(m_sizeSlider->minimum(), m_sizeSlider->maximum(), me->pos().x(), m_sizeSlider->width());
            m_sizeSlider->setValue(val);
            return true;
        }
    }

    if ((watched == m_searchEdit || watched == m_extEdit) && event->type() == QEvent::MouseButtonDblClick) {
        bool isQuery = (watched == m_searchEdit);
        if (m_historyDropdownController) {
            m_historyDropdownController->showDropdown(static_cast<QLineEdit*>(watched), isQuery);
            return true;
        }
    }

    return FramelessDialog::eventFilter(watched, event);
}

// ============================================================================
// PreviewRulesDialog 实现
// ============================================================================
PreviewRulesDialog::PreviewRulesDialog(ScanConfig& config, QWidget* parent)
    : FramelessDialog("预览配置", parent), m_config(config)
{
    resize(600, 480);
    setMinimumSize(500, 400);

    auto* layout = new QVBoxLayout(m_contentArea);
    layout->setContentsMargins(20, 15, 20, 20);
    layout->setSpacing(10);

    // 1. Whitelist label and editor
    auto* lblWhite = new QLabel("文件预览白名单 (放行规则，以中英文逗号分隔):");
    lblWhite->setStyleSheet("color: #EEEEEE; font-size: 12px; font-weight: bold;");
    layout->addWidget(lblWhite);

    m_whitelistEdit = new QTextEdit();
    m_whitelistEdit->setPlaceholderText("例如: jpg, png, txt, cpp, h, py");
    m_whitelistEdit->setStyleSheet(
        "QTextEdit {"
        "  background-color: #2D2D2D; border: 1px solid #444; border-radius: 6px;"
        "  padding: 8px; color: white; selection-background-color: #3498db;"
        "  font-size: 13px; line-height: 1.4;"
        "}"
        "QTextEdit:focus { border: 1px solid #3498db; }"
    );
    layout->addWidget(m_whitelistEdit);

    // 2. Blacklist label and editor
    auto* lblBlack = new QLabel("文件预览黑名单 (拦截规则，以中英文逗号分隔):");
    lblBlack->setStyleSheet("color: #EEEEEE; font-size: 12px; font-weight: bold;");
    layout->addWidget(lblBlack);

    m_blacklistEdit = new QTextEdit();
    m_blacklistEdit->setPlaceholderText("例如: exe, dll, zip, rar, mp4");
    m_blacklistEdit->setStyleSheet(
        "QTextEdit {"
        "  background-color: #2D2D2D; border: 1px solid #444; border-radius: 6px;"
        "  padding: 8px; color: white; selection-background-color: #3498db;"
        "  font-size: 13px; line-height: 1.4;"
        "}"
        "QTextEdit:focus { border: 1px solid #3498db; }"
    );
    layout->addWidget(m_blacklistEdit);

    // Populate data
    QStringList whiteList;
    for (const auto& ext : m_config.previewWhitelist) whiteList.append(ext);
    whiteList.sort();
    m_whitelistEdit->setPlainText(whiteList.join(", "));

    QStringList blackList;
    for (const auto& ext : m_config.previewBlacklist) blackList.append(ext);
    blackList.sort();
    m_blacklistEdit->setPlainText(blackList.join(", "));

    // 3. Buttons row
    auto* btnLayout = new QHBoxLayout();
    
    auto* btnRestore = new QPushButton("恢复默认");
    btnRestore->setFixedSize(100, 32);
    btnRestore->setCursor(Qt::PointingHandCursor);
    btnRestore->setStyleSheet(
        "QPushButton { background-color: transparent; color: #FF8C00; border: 1px solid #FF8C00; border-radius: 4px; } "
        "QPushButton:hover { background-color: rgba(255, 140, 0, 0.1); }"
    );
    connect(btnRestore, &QPushButton::clicked, this, &PreviewRulesDialog::onRestoreDefaults);
    btnLayout->addWidget(btnRestore);

    btnLayout->addStretch();
    
    auto* btnCancel = new QPushButton("取消");
    btnCancel->setFixedSize(80, 32);
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnCancel->setStyleSheet(
        "QPushButton { background-color: transparent; color: #888; border: 1px solid #444; border-radius: 4px; } "
        "QPushButton:hover { color: #EEE; background-color: #333; }"
    );
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(btnCancel);

    auto* btnOk = new QPushButton("确定");
    btnOk->setFixedSize(80, 32);
    btnOk->setCursor(Qt::PointingHandCursor);
    btnOk->setStyleSheet(
        "QPushButton { background-color: #3498db; color: white; border: none; border-radius: 4px; font-weight: bold; } "
        "QPushButton:hover { background-color: #1a7abf; }" 
    );
    connect(btnOk, &QPushButton::clicked, this, &PreviewRulesDialog::onConfirm);
    btnLayout->addWidget(btnOk);

    layout->addLayout(btnLayout);
}

void PreviewRulesDialog::onRestoreDefaults() {
    QStringList whiteList;
    for (const auto& ext : DEFAULT_WHITELIST) whiteList.append(ext);
    whiteList.sort();
    m_whitelistEdit->setPlainText(whiteList.join(", "));

    QStringList blackList;
    for (const auto& ext : DEFAULT_BLACKLIST) blackList.append(ext);
    blackList.sort();
    m_blacklistEdit->setPlainText(blackList.join(", "));
}

void PreviewRulesDialog::onConfirm() {
    auto parseExtensions = [](const QString& text) -> QSet<QString> {
        QSet<QString> set;
        QString temp = text;
        // 1. 将中文逗号、回车、换行全部统一替换为西文逗号（对应用户原话：“支持中英文逗号分割”）
        temp.replace(QString::fromUtf8("，"), ",");
        temp.replace('\n', ',');
        temp.replace('\r', ',');
        
        // 2. 严格按逗号进行物理分割，而不是空格分割（对应用户原话：“采用逗号分割……而不是空格分割”）
        QStringList list = temp.split(',', Qt::SkipEmptyParts);
        for (const QString& item : list) {
            // 去除多余的空格（例如逗号后的空格）
            QString clean = item.trimmed().toLower();
            if (clean.startsWith('.')) {
                clean = clean.mid(1);
            }
            if (!clean.isEmpty()) {
                set.insert(clean);
            }
        }
        return set;
    };

    m_config.previewWhitelist = parseExtensions(m_whitelistEdit->toPlainText());
    m_config.previewBlacklist = parseExtensions(m_blacklistEdit->toPlainText());

    accept();
}

} // namespace FERREX
