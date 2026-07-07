#include "NavPanel.h"
#include "UiHelper.h"
#include "Logger.h"
#include "TreeItemDelegate.h"
#include "DropTreeView.h"
#include "ContentPanel.h"
#include "../core/AppConfig.h"
#include <QHeaderView>
#include <QScrollBar>
#include <QLabel>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QStandardPaths>
#include <QTimer>
#include <QPushButton>
#include <QPointer>
#include <QMenu>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QtConcurrent>
#include <QApplication>

namespace ArcMeta {

/**
 * @brief 构造函数，设置面板属性
 */
NavPanel::NavPanel(QWidget* parent)
    : QFrame(parent) {
    setObjectName("ListContainer");
    setAttribute(Qt::WA_StyledBackground, true);
    // 设置面板宽度（遵循文档：导航面板 230px）
    setMinimumWidth(230);
    
    // 核心修正：移除宽泛的 QWidget QSS，防止其屏蔽 MainWindow 赋予的 ID 边框样式
    setStyleSheet("color: #EEEEEE;");

    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    // 2026-07-xx 按照 Plan-63：启用右键菜单
    setContextMenuPolicy(Qt::CustomContextMenu);
    initUi();
}

/**
 * @brief 初始化 UI 组件
 */
void NavPanel::deferredInit() {
    qDebug() << "[NavPanel] deferredInit 开始执行";
    if (m_model && m_model->rowCount() > 0) {
        qDebug() << "[NavPanel] 模型已存在数据，跳过重复初始化";
        return;
    }

    // 1. 新增：桌面入口 (使用 SVG 语义图标替代原生图标)
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QIcon desktopIcon = UiHelper::getIcon("home", QColor("#3498db"), 18);
    QStandardItem* desktopItem = new QStandardItem(desktopIcon, "桌面");
    desktopItem->setData(desktopPath, Qt::UserRole + 1);
    // 增加虚拟子项以便显示展开箭头
    desktopItem->appendRow(new QStandardItem("Loading..."));
    m_model->appendRow(desktopItem);

    // 2. 新增：此电脑入口 (使用 SVG 语义图标替代原生图标)
    // 2026-03-xx 物理加速：先展示文字项，图标通过延时加载或在主线程空闲时补全，防止磁盘休眠导致启动假死
    QIcon computerIcon = UiHelper::getIcon("monitor", QColor("#3498db"), 18);
    QStandardItem* computerItem = new QStandardItem(computerIcon, "此电脑");
    computerItem->setData("computer://", Qt::UserRole + 1);
    m_model->appendRow(computerItem);

    // 3. 磁盘列表 (逻辑异步预备：先填充基础文字路径)
    const auto drives = QDir::drives();
    for (const QFileInfo& drive : drives) {
        QString driveName = drive.absolutePath();
        QStandardItem* driveItem = new QStandardItem(driveName);
        driveItem->setData(driveName, Qt::UserRole + 1);
        driveItem->appendRow(new QStandardItem("Loading..."));
        m_model->appendRow(driveItem);
    }

    // 2026-03-xx 线程安全修复：图标提取必须在主线程执行。
    // 为了平衡性能与安全，图标提取在主线程分批次（Idle 状态）补全。
    QTimer::singleShot(0, [this, drives]() {
        qDebug() << "[NavPanel] 开始异步填充磁盘图标 (SVG 版)...";
        for (int i = 0; i < drives.size(); ++i) {
            if (i + 2 < m_model->rowCount()) {
                QIcon driveIcon = UiHelper::getIcon("hard_drive", QColor("#95a5a6"), 18);
                m_model->item(i + 2)->setIcon(driveIcon);
            }
        }
        qDebug() << "[NavPanel] 磁盘图标填充完成";
    });

    // 延迟加载收藏夹
    QTimer::singleShot(100, this, &NavPanel::loadFavorites);

    // 2026-xx-xx 按照 Plan-107：按内容自适应 + 从 AppConfig 恢复持久化比例
    if (m_splitter) {
        QByteArray splitterState = AppConfig::instance()
            .getValue("NavPanel/SplitterState").toByteArray();
        if (!splitterState.isEmpty()) {
            m_splitter->restoreState(splitterState);
        } else {
            // 默认：磁盘树占 2/3，收藏夹占 1/3
            m_splitter->setStretchFactor(0, 2);
            m_splitter->setStretchFactor(1, 1);
        }
    }

    qDebug() << "[NavPanel] deferredInit 同步部分执行完毕";
}

void NavPanel::setFocusHighlight(bool visible) {
    if (m_focusLine) m_focusLine->setVisible(visible);
}

void NavPanel::initUi() {
    // 2026-05-07 按照用户要求：修改焦点线颜色为蓝色
    m_focusLine = new QWidget(this);
    m_focusLine->setFixedHeight(1);
    m_focusLine->setStyleSheet("background-color: #007ACC;");
    m_focusLine->hide(); // 初始隐藏
    m_mainLayout->addWidget(m_focusLine);

    // 面板标题 (2026-xx-xx 按照 Plan-96：作为顶层固定标题)
    QWidget* header = new QWidget(this);
    header->setObjectName("ContainerHeader");
    header->setFixedHeight(32);
    header->setStyleSheet(
        "QWidget#ContainerHeader {"
        "  background-color: #252526;"
        "  border-bottom: 1px solid #333;"
        "}"
    );
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(15, 0, 5, 0);
    headerLayout->setSpacing(5);

    QLabel* iconLabel = new QLabel(header);
    iconLabel->setPixmap(UiHelper::getIcon("list_ul", QColor("#2ecc71"), 18).pixmap(18, 18));
    headerLayout->addWidget(iconLabel);

    QLabel* titleLabel = new QLabel("目录导航", header);
    titleLabel->setStyleSheet("color: #2ecc71; font-size: 13px; font-weight: bold; background: transparent; border: none;");
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    m_mainLayout->addWidget(header);

    // 2026-xx-xx 按照 Plan-107：物理还原 QSplitter 弹性架构
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setHandleWidth(1);
    m_splitter->setStyleSheet("QSplitter::handle { background: #333333; }");

    // --- 磁盘树 ---
    m_treeView = new DropTreeView(this);
    m_treeView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_treeView->setHeaderHidden(true);
    m_treeView->setAnimated(true);
    m_treeView->setIndentation(20);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_treeView->setExpandsOnDoubleClick(true);
    m_treeView->setDragEnabled(true);
    m_treeView->setDragDropMode(QAbstractItemView::DragOnly);
    m_treeView->setItemDelegate(new TreeItemDelegate(this, false));
    // 物理恢复：允许内部滚动
    m_treeView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_treeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_model = new QStandardItemModel(this);
    m_treeView->setModel(m_model);

    // --- 分组：收藏夹 ---
    QVBoxLayout* favLayout = nullptr;
    QWidget* favGroup = buildGroup("收藏夹", UiHelper::getIcon("star_filled", QColor("#FDB70A"), 18), QColor("#FDB70A"), favLayout);

    m_favoriteView = new DropTreeView(this);
    m_favoriteView->setHeaderHidden(true);
    m_favoriteView->setIndentation(0);
    m_favoriteView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_favoriteView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_favoriteView->setDragEnabled(true);
    m_favoriteView->setAcceptDrops(true);
    m_favoriteView->setDropIndicatorShown(true);
    m_favoriteView->setDefaultDropAction(Qt::MoveAction);
    m_favoriteView->setDragDropMode(QAbstractItemView::DragDrop);
    // 物理恢复：允许内部滚动
    m_favoriteView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_favoriteView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_favoriteView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_favoriteModel = new QStandardItemModel(this);
    m_favoriteView->setModel(m_favoriteModel);
    favLayout->addWidget(m_favoriteView);

    // 将各组件加入 Splitter
    m_splitter->addWidget(m_treeView);
    m_splitter->addWidget(favGroup);

    m_mainLayout->addWidget(m_splitter, 1);

    // 样式美化
    QString arrowRight = UiHelper::getSvgTempFilePath("arrow_right", QColor("#3498db"));
    QString arrowDown  = UiHelper::getSvgTempFilePath("arrow_down",  QColor("#3498db"));
    QString treeStyle = QString(
        "QTreeView { background-color: transparent; border: none; font-size: 12px; outline: none; padding-left: 15px; }"
        "QTreeView::item { height: 28px; padding-left: 0px; color: #EEEEEE; }"
        "QTreeView::branch { width: 20px; }"
        "QTreeView::branch:has-children:closed { image: url(\"%1\"); }"
        "QTreeView::branch:has-children:open   { image: url(\"%2\"); }"
    ).arg(arrowRight, arrowDown);

    m_treeView->setStyleSheet(treeStyle);
    m_favoriteView->setStyleSheet(treeStyle);

    // 信号连接
    connect(m_treeView, &QTreeView::expanded, this, &NavPanel::onItemExpanded);
    connect(m_treeView, &QTreeView::clicked, this, &NavPanel::onTreeClicked);

    connect(m_favoriteView, &QTreeView::clicked, this, &NavPanel::onFavoriteClicked);
    connect(m_favoriteView, &QWidget::customContextMenuRequested, this, &NavPanel::onFavoriteContextMenu);
    connect(m_favoriteView, &DropTreeView::pathsDropped, this, &NavPanel::onPathsDroppedToFavorite);
    
    // 收藏夹模型监听
    auto updateFavAndSave = [this](){ saveFavorites(); };
    connect(m_favoriteModel, &QStandardItemModel::rowsMoved, this, updateFavAndSave);
    connect(m_favoriteModel, &QStandardItemModel::rowsInserted, this, updateFavAndSave);
    connect(m_favoriteModel, &QStandardItemModel::rowsRemoved, this, updateFavAndSave);

    // 监听 Splitter 变化并持久化
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        AppConfig::instance().setValue(
            "NavPanel/SplitterState",
            m_splitter->saveState());
    });
}

/**
 * @brief 设置当前显示的根路径并自动展开
 */
void NavPanel::setRootPath(const QString& path) {
    Q_UNUSED(path);
    // 由于改为扁平化快捷入口列表，不再支持 setRootPath 的树深度同步
}

void NavPanel::selectPath(const QString& path) {
    for (int i = 0; i < m_model->rowCount(); ++i) {
        QStandardItem* item = m_model->item(i);
        if (item->data(Qt::UserRole + 1).toString() == path) {
            m_treeView->setCurrentIndex(item->index());
            m_treeView->setFocus();
            break;
        }
    }
}

/**
 * @brief 当用户点击目录时，发出信号告知外部组件（如内容面板）
 */
void NavPanel::onTreeClicked(const QModelIndex& index) {
    QString path = index.data(Qt::UserRole + 1).toString();
    if (!path.isEmpty() && path != "computer://") {
        emit directorySelected(path);
    } else if (path == "computer://") {
        emit directorySelected("computer://");
    }
}

void NavPanel::onFavoriteClicked(const QModelIndex& index) {
    QString path = index.data(Qt::UserRole + 1).toString();
    if (path.isEmpty()) return;

    QFileInfo fi(path);
    if (fi.isDir()) {
        emit directorySelected(path);
    } else {
        // 文件收藏：跳转到父目录并请求定位文件
        emit requestLocateFile(path);
    }
}

void NavPanel::onFavoriteContextMenu(const QPoint& pos) {
    QModelIndex index = m_favoriteView->indexAt(pos);
    if (!index.isValid()) return;

    QMenu menu(this);
    UiHelper::applyMenuStyle(&menu);

    QAction* removeAct = menu.addAction(UiHelper::getIcon("close", QColor("#EEEEEE")), "取消收藏");
    connect(removeAct, &QAction::triggered, this, [this, index]() {
        m_favoriteModel->removeRow(index.row());
        // 信号已连接到 saveFavorites()
    });

    menu.exec(m_favoriteView->viewport()->mapToGlobal(pos));
}

void NavPanel::onPathsDroppedToFavorite(const QStringList& paths, const QModelIndex& target) {
    Q_UNUSED(target);
    for (const QString& path : paths) {
        addFavoriteItem(path);
    }
    saveFavorites();
}

void NavPanel::loadFavorites() {
    if (!m_favoriteModel) return;
    m_favoriteModel->clear();

    QVariant val = AppConfig::instance().getValue("NavPanel/Favorites");
    if (!val.isValid()) return;

    QJsonDocument doc = QJsonDocument::fromJson(val.toByteArray());
    if (!doc.isArray()) return;

    QJsonArray arr = doc.array();
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject obj = arr.at(i).toObject();
        QString path = obj.value("path").toString();
        if (!path.isEmpty()) {
            addFavoriteItem(path);
        }
    }
}

void NavPanel::saveFavorites() {
    if (!m_favoriteModel) return;

    QJsonArray arr;
    for (int i = 0; i < m_favoriteModel->rowCount(); ++i) {
        QStandardItem* item = m_favoriteModel->item(i);
        QString path = item->data(Qt::UserRole + 1).toString();
        
        QJsonObject obj;
        obj.insert("path", path);
        arr.append(obj);
    }

    QJsonDocument doc(arr);
    AppConfig::instance().setValue("NavPanel/Favorites", doc.toJson(QJsonDocument::Compact));
}

void NavPanel::addFavoriteItem(const QString& path) {
    // 检查重复
    for (int i = 0; i < m_favoriteModel->rowCount(); ++i) {
        if (m_favoriteModel->item(i)->data(Qt::UserRole + 1).toString() == path) {
            return;
        }
    }

    QFileInfo fi(path);
    if (!fi.exists()) return;

    QIcon icon = UiHelper::getFileIcon(path, 18);
    QStandardItem* item = new QStandardItem(icon, fi.fileName().isEmpty() ? path : fi.fileName());
    item->setData(path, Qt::UserRole + 1);
    
    // 物理红线：收藏项不再显示子节点（扁平化展示）
    m_favoriteModel->appendRow(item);
}

void NavPanel::onItemExpanded(const QModelIndex& index) {
    QStandardItem* item = m_model->itemFromIndex(index);
    if (!item) return;

    // 如果只有一个 Loading 子项，则触发真实加载
    if (item->rowCount() == 1 && item->child(0)->text() == "Loading...") {
        fetchChildDirs(item);
    }
}

void NavPanel::updateTreeHeight() {
    // 2026-xx-xx 按照 Plan-107：废弃手动高度计算，解锁 Splitter 自由拉伸
}

void NavPanel::updateFavoriteHeight() {
    // 2026-xx-xx 按照 Plan-107：废弃手动高度计算
}

QWidget* NavPanel::buildGroup(const QString& title, const QIcon& icon, const QColor& color, QVBoxLayout*& outContentLayout) {
    QWidget* wrapper = new QWidget(this);
    wrapper->setAttribute(Qt::WA_StyledBackground, true);
    wrapper->setStyleSheet("background: transparent;");
    QVBoxLayout* wl = new QVBoxLayout(wrapper);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(0);

    QWidget* hdrRow = new QWidget(wrapper);
    hdrRow->setObjectName("GroupHdrRow");
    hdrRow->setFixedHeight(32);
    hdrRow->setAttribute(Qt::WA_StyledBackground, true);
    hdrRow->setStyleSheet(
        "QWidget#GroupHdrRow {"
        "  background: #252526;"
        "  border-bottom: 1px solid #333333;"
        "  border-top: 1px solid #333333;"
        "}");

    QHBoxLayout* hdrLayout = new QHBoxLayout(hdrRow);
    hdrLayout->setContentsMargins(15, 0, 5, 0);
    hdrLayout->setSpacing(5);

    QLabel* iconLabel = new QLabel(hdrRow);
    iconLabel->setPixmap(icon.pixmap(18, 18));
    hdrLayout->addWidget(iconLabel);

    QPushButton* hdrBtn = new QPushButton(title, hdrRow);
    hdrBtn->setCheckable(false);
    hdrBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    hdrBtn->setFixedHeight(32);
    hdrBtn->setCursor(Qt::PointingHandCursor);
    hdrBtn->setStyleSheet(QString(
        "QPushButton {"
        "  background: transparent;"
        "  border: none;"
        "  color: %1;"
        "  font-size: 13px;"
        "  font-weight: bold;"
        "  text-align: left;"
        "  padding: 0px;"
        "  margin: 0px;"
        "}"
    ).arg(color.name()));
    hdrLayout->addWidget(hdrBtn);

    QWidget* content = new QWidget(wrapper);
    content->setAttribute(Qt::WA_StyledBackground, true);
    content->setStyleSheet("background: transparent;");
    outContentLayout = new QVBoxLayout(content);
    outContentLayout->setContentsMargins(0, 0, 0, 0);
    outContentLayout->setSpacing(0);

    wl->addWidget(hdrRow);
    wl->addWidget(content, 1);
    return wrapper;
}

/**
 * @brief 异步获取子目录，解决展开文件夹时的界面假死 (2026-05-25 物理加速)
 */
void NavPanel::fetchChildDirs(QStandardItem* parent) {
    QString path = parent->data(Qt::UserRole + 1).toString();
    if (path.isEmpty() || path == "computer://") return;

    parent->removeRows(0, parent->rowCount());
    parent->appendRow(new QStandardItem("正在读取..."));

    // 2026-05-25 编译修复：QStandardItem 不继承自 QObject，严禁使用 QPointer。
    // 改用 QPersistentModelIndex 确保异步回调时索引的有效性。
    QPersistentModelIndex pIdx(parent->index());
    (void)QtConcurrent::run([this, pIdx, path]() {
        QDir dir(path);
        // 执行耗时的物理磁盘读取
        QFileInfoList list = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        
        struct DirInfo { QString name; QString absPath; bool hasSub; };
        QList<DirInfo> results;
        for (const QFileInfo& info : list) {
            QDir subDir(info.absoluteFilePath());
            bool hasSub = !subDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
            results << DirInfo{info.fileName(), info.absoluteFilePath(), hasSub};
        }

        // 投递回主线程进行 UI 更新
        QMetaObject::invokeMethod(QCoreApplication::instance(), [this, pIdx, results]() {
            if (!pIdx.isValid()) return;
            QStandardItem* safeParent = m_model->itemFromIndex(pIdx);
            if (!safeParent) return;

            safeParent->removeRows(0, safeParent->rowCount());
            
            for (const auto& info : results) {
                QIcon folderIcon = UiHelper::getFileIcon(info.absPath, 18);
                QStandardItem* child = new QStandardItem(folderIcon, info.name);
                child->setData(info.absPath, Qt::UserRole + 1);
                
                if (info.hasSub) {
                    child->appendRow(new QStandardItem("Loading..."));
                }
                safeParent->appendRow(child);
            }
        }, Qt::QueuedConnection);
    });
}

} // namespace ArcMeta
