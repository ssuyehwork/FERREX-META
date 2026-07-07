#include "CategoryPanel.h"
#include "MainWindow.h"
#include "CategoryModel.h"
#include "CategoryFilterProxyModel.h"
#include "CategoryLockDialog.h"
#include "CategorySetPasswordDialog.h"
#include "CategoryDelegate.h"
#include "DropTreeView.h"
#include "../util/ImportHelper.h"
#include "UiHelper.h"
#include "StyleLibrary.h"
using namespace ArcMeta::Style;
#include "ToolTipOverlay.h"
#include "FramelessDialog.h"
#include "BatchProgressDialog.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include "../meta/CategoryRepo.h"
#include "../util/ShellHelper.h"


#include "../meta/MetadataManager.h"
#include "../core/CoreController.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QScrollBar>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QRandomGenerator>
#include <QSet>
#include <QDirIterator>
#include "../core/AppConfig.h"


#include "Logger.h"
#include <QtConcurrent>

namespace ArcMeta {

/**
 * @brief 获取默认分类颜色：深灰色 (#555555)
 * 2026-06-xx 按照用户要求：废除随机色，统一默认使用深灰色
 */
static std::wstring getDefaultCategoryColor() {
    return L"#555555";
}

CategoryPanel::CategoryPanel(QWidget* parent)
    : QFrame(parent) {
    // 2026-07-xx 按照 Plan-63：启用右键菜单策略（容器级）
    setContextMenuPolicy(Qt::CustomContextMenu);

    setObjectName("SidebarContainer");
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(230);
    setStyleSheet(QString("color: %1;").arg(qssColor(TextMain)));
    
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    // 2026-06-xx 物理优化：移除此处阻塞主线程的同步初始化。
    // 元数据加载已由 CoreController 在异步线程统一接管。

    // 2026-06-xx 物理削峰：初始化防抖定时器
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
        if (!m_categoryModel) return;
        
        bool needsFullRebuild = m_refreshTimer->property("fullRebuild").toBool();

        if (m_isFirstLoad || needsFullRebuild) {
            m_categoryModel->refresh();
            m_isFirstLoad = false;
            m_refreshTimer->setProperty("fullRebuild", false); // 消费完重置
        }

        // 2026-07-xx 性能优化：执行重建后立即继续统计计算，不再触发 requestRefresh 导致二次等待
        // 2026-06-xx 物理分流：将耗时的统计计算（fullRecount）移出 UI 线程
        // 采用 QPointer 确保线程安全性
        QPointer<CategoryPanel> weakThis(this);
        (void)QtConcurrent::run([weakThis]() {
            auto sysCounts = CategoryRepo::getSystemCounts();
            auto catCountsVec = CategoryRepo::getCounts();
            
            QMap<int, int> catCounts;
            for (const auto& entry : catCountsVec) catCounts[entry.first] = entry.second;
            
            // 计算完成后，通过消息队列回传主线程执行局部 UI 更新
            QMetaObject::invokeMethod(weakThis.data(), [weakThis, sysCounts, catCounts]() {
                if (weakThis && weakThis->m_categoryModel) {
                    // 2026-07-xx 物理修复：若统计数据全空，且系统尚未加载完成，则拒绝执行 UI 更新以防止计数清零
                    if (sysCounts.isEmpty() && catCounts.isEmpty()) {
                        return;
                    }
                    // 第三阶段：执行局部数据更新，杜绝 beginResetModel 引发全量布局计算
                    weakThis->m_categoryModel->updateStatistics(sysCounts, catCounts);
                }
            });
        });
    });

    // 2026-xx-xx 按照 Plan-106：初始化搜索防抖计时器
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(300);
    connect(m_searchTimer, &QTimer::timeout, this, [this]() {
        if (m_searchEdit) onSearchTextChanged(m_searchEdit->text());
    });

    initUi();
    setupContextMenu();

    // 2026-06-xx 物理修复：连接 CoreController 的初始化完成信号
    // 理由：系统启动时的 initFromScchMode 是异步进行的，完成后必须强制刷新侧边栏
    // 以解决数据库加载延迟导致的系统项（如“全部数据”、“未分类”）显示为 0 的问题
    connect(&CoreController::instance(), &CoreController::initializationFinished, this, [this]() {
        m_isFirstLoad = true; // 强制执行 refresh() 重建树结构并拉取最新计数
        requestRefresh();
    });

    // 2026-06-xx 物理修复：监听元数据变更信号，确保删除项或标记状态后计数实时更新
    connect(&MetadataManager::instance(), &MetadataManager::metaChanged, this, [this](const QString& /*path*/) {
        requestRefresh();
    });
}

void CategoryPanel::requestRefresh(bool fullRebuild) {
    // 2026-07-xx 性能优化：缩短防抖时间至 200ms 以提升 UI 响应灵敏度
    if (fullRebuild) {
        m_refreshTimer->setProperty("fullRebuild", true);
    }
    m_refreshTimer->start(200);
}

void CategoryPanel::selectCategory(int id) {
    if (!m_categoryModel) return;
    
    // 递归查找匹配 ID 的索引
    std::function<QModelIndex(const QModelIndex&)> findId;
    findId = [&](const QModelIndex& parent) -> QModelIndex {
        for (int i = 0; i < m_categoryModel->rowCount(parent); ++i) {
            QModelIndex idx = m_categoryModel->index(i, 0, parent);
            if (idx.data(IdRole).toInt() == id) return idx;
            QModelIndex child = findId(idx);
            if (child.isValid()) return child;
        }
        return QModelIndex();
    };

    QModelIndex target = findId(QModelIndex());
    if (target.isValid()) {
        // 2026-xx-xx 按照 Plan-98：映射至代理模型索引
        QModelIndex proxyIdx = m_proxyModel->mapFromSource(target);
        if (proxyIdx.isValid()) {
            // 2026-03-xx 物理阻断：通过代码强制选中时，必须锁定信号发射，防止与 ContentPanel 形成回环死循环
            m_categoryTree->blockSignals(true);
            m_categoryTree->setCurrentIndex(proxyIdx);
            m_categoryTree->scrollTo(proxyIdx);
            m_categoryTree->blockSignals(false);
        }
    }
}

void CategoryPanel::selectCategoryByType(const QString& type) {
    if (!m_categoryModel) return;

    // 递归查找匹配类型的索引
    std::function<QModelIndex(const QModelIndex&)> findType;
    findType = [&](const QModelIndex& parent) -> QModelIndex {
        for (int i = 0; i < m_categoryModel->rowCount(parent); ++i) {
            QModelIndex idx = m_categoryModel->index(i, 0, parent);
            if (idx.data(TypeRole).toString() == type) return idx;
            QModelIndex child = findType(idx);
            if (child.isValid()) return child;
        }
        return QModelIndex();
    };

    QModelIndex target = findType(QModelIndex());
    if (target.isValid()) {
        QModelIndex proxyIdx = m_proxyModel->mapFromSource(target);
        if (proxyIdx.isValid()) {
            m_categoryTree->blockSignals(true);
            m_categoryTree->setCurrentIndex(proxyIdx);
            m_categoryTree->scrollTo(proxyIdx);
            m_categoryTree->blockSignals(false);
        }
    }
}

void CategoryPanel::deferredInit() {

    // 2026-04-12 关键修复：延迟执行数据库数据加载
    if (m_categoryModel) {
        m_categoryModel->deferredRefresh();
    }
}

void CategoryPanel::setupContextMenu() {
    m_categoryTree->setContextMenuPolicy(Qt::CustomContextMenu);
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QModelIndex proxyIndex = m_categoryTree->indexAt(pos);
        QModelIndex index = m_proxyModel->mapToSource(proxyIndex);
        
        // 2026-03-xx 按照用户要求：实现右键点击即选中，解决“分类与其子分类”交互一致性问题
        if (proxyIndex.isValid()) {
            m_categoryTree->setCurrentIndex(proxyIndex);
        }

        QMenu menu(this);
        UiHelper::applyMenuStyle(&menu);

        // 基于规范逻辑：如果没有选中项，或者选中了“我的分类”根节点
        QString itemName = index.data(NameRole).toString();
        QString itemType = index.data(TypeRole).toString();

        if (itemType == "trash") {
            // 2026-06-xx 物理级 1:1 还原：回收站专属右键菜单
            menu.addAction(UiHelper::getIcon("trash", ErrorRed, 18), "清空回收站", this, &CategoryPanel::onEmptyTrash);
            menu.addAction(UiHelper::getIcon("sync", PrimaryBlue, 18), "还原全部项目", this, &CategoryPanel::onRestoreAllFromTrash);
        } else if (!index.isValid() || itemName == "我的分类") {
            menu.addAction(UiHelper::getIcon("folder_filled", QColor("#aaaaaa"), 18), "新建分类", this, &CategoryPanel::onCreateCategory);
            
            auto* sortMenu = menu.addMenu(UiHelper::getIcon("list_ul", QColor("#aaaaaa"), 18), "排列");
            sortMenu->setStyleSheet(menu.styleSheet());
            sortMenu->addAction("标题(全部) (A→Z)", this, &CategoryPanel::onSortAllByNameAsc);
            sortMenu->addAction("标题(全部) (Z→A)", this, &CategoryPanel::onSortAllByNameDesc);
        } else {
            // 2026-03-xx 按照用户要求：补全子层级（子分类、文件、文件夹）的右键菜单
            // 物理修复：移除重复声明，使用统一的 itemType 变量
            
            // 只要不是系统根节点，都弹出完整菜单
            if (itemType == "category" || itemType == "file" || itemType == "folder") {
                
                // 2026-06-xx 统一图标
                menu.addAction(UiHelper::getIcon("folder_filled", PrimaryBlue, 18), "归类到此分类", this, &CategoryPanel::onClassifyToCategory);
                
                menu.addSeparator();
                
                menu.addAction(UiHelper::getIcon("palette", WarningOrange, 18), "设置颜色", this, &CategoryPanel::onSetColor);
                menu.addAction(UiHelper::getIcon("random_color", QColor("#e91e63"), 18), "随机颜色", this, &CategoryPanel::onRandomColor);
                menu.addAction(UiHelper::getIcon("tag_filled", QColor("#9b59b6"), 18), "设置预设标签", this, &CategoryPanel::onSetPresetTags);

                menu.addSeparator();

                menu.addAction(UiHelper::getIcon("folder_filled", TextMuted, 18), "新建分类", this, &CategoryPanel::onCreateCategory);
                menu.addAction(UiHelper::getIcon("folder_filled", TextMuted, 18), "新建子分类", this, &CategoryPanel::onCreateSubCategory);

                menu.addSeparator();

                bool isPinned = index.data(PinnedRole).toBool();
                menu.addAction(UiHelper::getIcon("pin_vertical", isPinned ? Style::ActiveOrange : TextMuted, 18), 
                               isPinned ? "从“快速访问”中移除" : "添加至“快速访问”", this, &CategoryPanel::onTogglePin);
                               
                menu.addAction(UiHelper::getIcon("edit", TextMuted, 18), "重命名分类", this, &CategoryPanel::onRenameCategory);
                menu.addAction(UiHelper::getIcon("trash", ErrorRed, 18), "删除分类", this, &CategoryPanel::onDeleteCategory);

                menu.addSeparator();

                // 2026-03-xx 按照用户要求：补全排列与密码保护逻辑
                auto* sortMenu = menu.addMenu(UiHelper::getIcon("list_ul", QColor("#aaaaaa"), 18), "排列");
                sortMenu->setStyleSheet(menu.styleSheet());
                sortMenu->addAction("标题(当前层级) (A→Z)", this, &CategoryPanel::onSortByNameAsc);
                sortMenu->addAction("标题(当前层级) (Z→A)", this, &CategoryPanel::onSortByNameDesc);
                sortMenu->addAction("标题(全部) (A→Z)", this, &CategoryPanel::onSortAllByNameAsc);
                sortMenu->addAction("标题(全部) (Z→A)", this, &CategoryPanel::onSortAllByNameDesc);

                auto* pwdMenu = menu.addMenu(UiHelper::getIcon("lock", QColor("#aaaaaa"), 18), "密码保护");
                pwdMenu->setStyleSheet(menu.styleSheet());
                
                // 2026-03-xx 按照用户要求：通过 EncryptedRole 动态判断显示“设置”或“清除”
                bool isEncrypted = index.data(EncryptedRole).toBool();
                
                if (!isEncrypted) {
                    pwdMenu->addAction("设置密码", this, &CategoryPanel::onSetPassword);
                } else {
                    pwdMenu->addAction("清除密码", this, &CategoryPanel::onClearPassword);
                }
            }
        }
        
        if (!menu.isEmpty()) {
            // 2026-07-xx 按照 Plan-63：注入布局显示控制菜单
            menu.addSeparator();
            QMenu* layoutMenu = menu.addMenu("布局显示");
            UiHelper::applyMenuStyle(layoutMenu);
            
            // 通过向上寻道获取 MainWindow 实例以复用菜单逻辑
            MainWindow* mw = nullptr;
            QWidget* parentWin = window();
            while (parentWin) {
                if ((mw = qobject_cast<MainWindow*>(parentWin))) break;
                parentWin = parentWin->parentWidget();
            }
            if (mw) {
                mw->populatePanelMenu(layoutMenu);
            }

            menu.exec(m_categoryTree->viewport()->mapToGlobal(pos));
        }
    });
}

/**
 * @brief 递归保存 QTreeView 的展开状态
 */
void CategoryPanel::saveExpandedState(const QModelIndex& parent, QSet<int>& expandedIds, QStringList& expandedNames) {
    if (!m_categoryTree || !m_categoryTree->model()) return;
    int rowCount = m_categoryTree->model()->rowCount(parent);
    for (int i = 0; i < rowCount; ++i) {
        QModelIndex idx = m_categoryTree->model()->index(i, 0, parent);
        if (m_categoryTree->isExpanded(idx)) {
            int id = idx.data(IdRole).toInt();
            QString name = idx.data(NameRole).toString();
            if (id != 0) {
                expandedIds.insert(id);
            } else {
                expandedNames << name;
            }
            saveExpandedState(idx, expandedIds, expandedNames);
        }
    }
}

void CategoryPanel::onSearchTextChanged(const QString& text) {
    if (m_proxyModel) {
        m_proxyModel->setFilterText(text);
        if (!text.isEmpty()) {
            m_categoryTree->expandAll();
        } else {
            // 搜索清除时，恢复常规展开状态
            loadExpandedStateFromSettings();
        }
    }
}

/**
 * @brief 递归恢复 QTreeView 的展开状态
 * 2026-03-xx 物理拦截：加密且未解锁的分类在恢复时强制跳过展开
 */
void CategoryPanel::restoreExpandedState(const QModelIndex& parent, const QSet<int>& expandedIds, const QStringList& expandedNames) {
    if (!m_categoryTree || !m_categoryTree->model()) return;
    
    bool hasHistory = m_categoryTree->property("hasHistoryRecord").toBool();
    int rowCount = m_categoryTree->model()->rowCount(parent);
    
    for (int i = 0; i < rowCount; ++i) {
        QModelIndex idx = m_categoryTree->model()->index(i, 0, parent);
        int id = idx.data(IdRole).toInt();
        QString name = idx.data(NameRole).toString();
        bool isEncrypted = idx.data(EncryptedRole).toBool();
        
        bool shouldExpand = false;
        
        if (expandedNames.contains(name) || (id != 0 && expandedIds.contains(id))) {
            shouldExpand = true;
        }
        else if (name == "我的分类" || name == "快速访问") {
            shouldExpand = true;
        }
        else if (!hasHistory) {
            QModelIndex pIdx = idx.parent();
            if (pIdx.isValid() && pIdx.data(NameRole).toString() == "我的分类") {
                shouldExpand = true;
            }
        }

        if (shouldExpand && isEncrypted && id > 0 && !m_unlockedIds.contains(id)) {
            shouldExpand = false;
        }

        if (shouldExpand) {
            m_categoryTree->setExpanded(idx, true);
            restoreExpandedState(idx, expandedIds, expandedNames);
        }
    }
}

void CategoryPanel::onCreateCategory() {
    FramelessInputDialog dlg("新建分类", "请输入分类名称:", "", this);
    if (dlg.exec() == QDialog::Accepted) {
        QString text = dlg.text();
        if (!text.isEmpty()) {
            Category cat;
            cat.name = text.toStdWString();
            cat.parentId = 0;
            cat.color = getDefaultCategoryColor();
            
            QSet<int> expandedIds;
            QStringList expandedNames;
            saveExpandedState(QModelIndex(), expandedIds, expandedNames);

            CategoryRepo::add(cat);
            m_categoryModel->refresh();

            restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
        }
    }
}

void CategoryPanel::onCreateSubCategory() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;

    FramelessInputDialog dlg("新建子分类", "请输入子分类名称:", "", this);
    if (dlg.exec() == QDialog::Accepted) {
        QString text = dlg.text();
        if (!text.isEmpty()) {
            Category cat;
            cat.name = text.toStdWString();
            cat.parentId = id;
            cat.color = getDefaultCategoryColor();

            QSet<int> expandedIds;
            QStringList expandedNames;
            saveExpandedState(QModelIndex(), expandedIds, expandedNames);
            expandedIds.insert(id);

            CategoryRepo::add(cat);
            m_categoryModel->refresh();

            restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
        }
    }
}

void CategoryPanel::onClassifyToCategory() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;

    AppConfig::instance().setValue("Category/ExtensionTargetId", id);

    QSet<int> expandedIds;
    QStringList expandedNames;
    saveExpandedState(QModelIndex(), expandedIds, expandedNames);

    m_categoryModel->refresh();

    restoreExpandedState(QModelIndex(), expandedIds, expandedNames);

    QString name = index.data(NameRole).toString();
    ToolTipOverlay::instance()->showText(QCursor::pos(), QString("已设为归类目标: %1").arg(name), 1000);
}

void CategoryPanel::onSetColor() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;

    QColor originalColor = Qt::white;
    auto all_cats = CategoryRepo::getAll();
    for(const auto& c : all_cats) {
        if(c.id == id) {
            originalColor = QColor(QString::fromStdWString(c.color));
            break;
        }
    }

    FramelessColorPicker dlg("选择分类颜色", this);
    dlg.setCurrentColor(originalColor);
    if (dlg.exec() != QDialog::Accepted) return;
    
    QColor color = dlg.selectedColor();

    auto all = CategoryRepo::getAll();
    for(auto& cat : all) {
        if(cat.id == id) {
            cat.color = color.name().toUpper().toStdWString();
            CategoryRepo::update(cat);
            break;
        }
    }
    
    QSet<int> expandedIds;
    QStringList expandedNames;
    saveExpandedState(QModelIndex(), expandedIds, expandedNames);

    m_categoryModel->refresh();

    restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
    
    ToolTipOverlay::instance()->showText(QCursor::pos(), "分类颜色已更新", 1000);
}

void CategoryPanel::onRandomColor() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;
    
    // 2026-03-xx 按照用户要求：从旧版本中迁移调色盘逻辑
    static const QStringList palette = {
        "#FF6B6B", "#4ECDC4", "#45B7D1", "#96CEB4", "#FFEEAD",
        "#D4A5A5", "#9B59B6", "#3498DB", "#E67E22", "#2ECC71",
        "#E74C3C", "#F1C40F", "#1ABC9C", "#34495E", "#95A5A6"
    };
    QString chosenColor = palette.at(QRandomGenerator::global()->bounded(palette.size()));
    
    auto all = CategoryRepo::getAll();
    for(auto& cat : all) {
        if(cat.id == id) {
            cat.color = chosenColor.toStdWString();
            CategoryRepo::update(cat);
            break;
        }
    }
    
    QSet<int> expandedIds;
    QStringList expandedNames;
    saveExpandedState(QModelIndex(), expandedIds, expandedNames);

    m_categoryModel->refresh();

    restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
}

void CategoryPanel::onSetPresetTags() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;

    auto all = CategoryRepo::getAll();
    Category current;
    for(auto& c : all) if(c.id == id) { current = c; break; }

    QString initial;
    for(const auto& t : current.presetTags) initial += QString::fromStdWString(t) + ",";
    if (initial.endsWith(",")) initial.chop(1);

    FramelessInputDialog dlg("设置预设标签", "请输入标签 (用逗号分隔):", initial, this);
    if (dlg.exec() == QDialog::Accepted) {
        QStringList tags = dlg.text().split(QRegularExpression("[,，]"), Qt::SkipEmptyParts);
        current.presetTags.clear();
        for(const QString& t : tags) current.presetTags.push_back(t.trimmed().toStdWString());
        
        QSet<int> expandedIds;
        QStringList expandedNames;
        saveExpandedState(QModelIndex(), expandedIds, expandedNames);

        CategoryRepo::update(current);
        m_categoryModel->refresh();

        restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
        ToolTipOverlay::instance()->showText(QCursor::pos(), "预设标签已更新", 1000);
    }
}

void CategoryPanel::onTogglePin() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;
    
    bool isPinned = index.data(PinnedRole).toBool();

    auto all = CategoryRepo::getAll();
    for(auto& cat : all) {
        if(cat.id == id) {
            cat.pinned = !isPinned;
            CategoryRepo::update(cat);
            break;
        }
    }

    QSet<int> expandedIds;
    QStringList expandedNames;
    saveExpandedState(QModelIndex(), expandedIds, expandedNames);

    m_categoryModel->refresh();

    restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
}

void CategoryPanel::onSetPassword() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;

    // 2026-03-xx 物理级 1:1 还原：废弃通用输入框，调用三字段密码对话框
    CategorySetPasswordDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        QString pwd = dlg.password();
        QString hint = dlg.hint();

        QSet<int> expandedIds;
        QStringList expandedNames;
        saveExpandedState(QModelIndex(), expandedIds, expandedNames);

        auto all = CategoryRepo::getAll();
        for(auto& cat : all) {
            if(cat.id == id) {
                cat.encrypted = true;
                cat.encryptHint = hint.toStdWString();
                CategoryRepo::update(cat);
                break;
            }
        }
        
        m_categoryModel->refresh();

        restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#00A650;'>[OK] 分类已加密</b>", 1000, QColor("#00A650"));
    }
}

void CategoryPanel::onClearPassword() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;

    QString hint = index.data(EncryptHintRole).toString();

    // 2026-03-xx 物理级还原：清除密码需先通过旧版验证界面校验身份
    CategoryLockDialog dlg(hint, this);
    if (dlg.exec() == QDialog::Accepted) {
        // [SIMULATION] 校验成功
        QSet<int> expandedIds;
        QStringList expandedNames;
        saveExpandedState(QModelIndex(), expandedIds, expandedNames);

        auto all = CategoryRepo::getAll();
        for(auto& cat : all) {
            if(cat.id == id) {
                cat.encrypted = false;
                cat.encryptHint = L"";
                CategoryRepo::update(cat);
                break;
            }
        }

        m_categoryModel->refresh();

        restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#00A650;'>[OK] 验证成功，分类已解除加密</b>", 1000, QColor("#00A650"));
    }
}

void CategoryPanel::onRenameCategory() {
    QModelIndex index = m_categoryTree->currentIndex();
    if (index.isValid()) {
        QString type = index.data(TypeRole).toString();
        // 2026-03-xx 物理兼容：允许重命名分类或文件项 (逻辑处理见 Model)
        if (type == "category" || type == "file" || type == "folder") {
            m_categoryTree->edit(index);
        }
    }
}

void CategoryPanel::onDeleteCategory() {
    // 2026-06-xx 彻底重构：支持多选批量删除分类，杜绝单项操作的低效
    QModelIndexList selectedRows = m_categoryTree->selectionModel()->selectedRows();
    if (selectedRows.isEmpty()) {
        // 如果没有整行选中，尝试回退到 currentIndex
        QModelIndex current = m_categoryTree->currentIndex();
        if (current.isValid()) selectedRows << current;
    }

    if (selectedRows.isEmpty()) return;

    QSet<int> idsToDelete;
    
    // 递归收集分类及其所有子分类 ID 的辅助函数
    std::function<void(const QModelIndex&)> collectIds;
    collectIds = [&](const QModelIndex& index) {
        QString type = index.data(TypeRole).toString();
        int id = index.data(IdRole).toInt();
        
        if (type == "category" && id > 0) {
            idsToDelete.insert(id);
            // 递归收集子分类
            for (int i = 0; i < m_categoryModel->rowCount(index); ++i) {
                collectIds(m_categoryModel->index(i, 0, index));
            }
        }
    };

    for (const QModelIndex& index : selectedRows) {
        collectIds(index);
    }

    if (idsToDelete.isEmpty()) return;

    // 2. 后台批量异步落库
    int totalCount = idsToDelete.size();
    QList<int> idList = idsToDelete.values();
    
    (void)QThreadPool::globalInstance()->start([this, idList, totalCount]() {
        for (int id : idList) {
            ArcMeta::CategoryRepo::remove(id);
        }
        
        // 删除完成后回到主线程刷新 UI
        QMetaObject::invokeMethod(this, [this, totalCount]() {
            m_categoryModel->refresh();
            ToolTipOverlay::instance()->showText(QCursor::pos(), 
                QString("<b style='color:%1;'>已成功删除 %2 个分类</b>").arg(qssColor(ErrorRed)).arg(QString::number(totalCount)), 1500, ErrorRed);
        }, Qt::QueuedConnection);
    });
}

int CategoryPanel::getTargetCategoryId(const QModelIndex& index) {
    if (!index.isValid()) return 0;
    
    int id = index.data(IdRole).toInt();
    // 2026-06-xx 物理修复：允许识别负数 ID（系统项），解除 ID > 0 的硬编码限制
    if (id != 0) return id;
    
    // 递归查找父节点，直到找到 category 类型
    return getTargetCategoryId(index.parent());
}

void CategoryPanel::onSortByNameAsc() {
    QModelIndex index = m_categoryTree->currentIndex();
    // 逻辑：获取该项的父级分类 ID，执行重排
    int parentCatId = 0;
    QModelIndex pIdx = index.parent();
    if (pIdx.isValid()) parentCatId = pIdx.data(IdRole).toInt();

    if (CategoryRepo::reorder(parentCatId, true)) {
        m_categoryModel->refresh();
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#2ecc71;'>[OK] 已按 A→Z 排列</b>");
    }
}

void CategoryPanel::onSortByNameDesc() {
    QModelIndex index = m_categoryTree->currentIndex();
    int parentCatId = 0;
    QModelIndex pIdx = index.parent();
    if (pIdx.isValid()) parentCatId = pIdx.data(IdRole).toInt();

    if (CategoryRepo::reorder(parentCatId, false)) {
        m_categoryModel->refresh();
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#2ecc71;'>[OK] 已按 Z→A 排列</b>");
    }
}

void CategoryPanel::onSortAllByNameAsc() {
    if (CategoryRepo::reorderAll(true)) {
        m_categoryModel->refresh();
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#2ecc71;'>[OK] 全部已按 A→Z 排列</b>");
    }
}

void CategoryPanel::onSortAllByNameDesc() {
    if (CategoryRepo::reorderAll(false)) {
        m_categoryModel->refresh();
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#2ecc71;'>[OK] 全部已按 Z→A 排列</b>");
    }
}

void CategoryPanel::onEmptyTrash() {
    // 1. 获取回收站内所有 FID
    // 物理修复：明确作用域标识符 CategoryRepo::TRASH_CATEGORY_ID
    std::vector<std::string> trashItems = CategoryRepo::getFileIdsInCategory(CategoryRepo::TRASH_CATEGORY_ID);
    if (trashItems.empty()) {
        ToolTipOverlay::instance()->showText(QCursor::pos(), "回收站已空", 1000);
        return;
    }

    // 2. 物理彻底删除
    if (CategoryRepo::permanentlyDeleteBatch(trashItems)) {
        m_categoryModel->refresh();
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#e74c3c;'>[OK] 已清空回收站</b>", 1500, ErrorRed);
    }
}

void CategoryPanel::onRestoreAllFromTrash() {
    // 1. 获取回收站内所有 FID
    // 物理修复：明确作用域标识符 CategoryRepo::TRASH_CATEGORY_ID
    std::vector<std::string> trashItems = CategoryRepo::getFileIdsInCategory(CategoryRepo::TRASH_CATEGORY_ID);
    if (trashItems.empty()) {
        ToolTipOverlay::instance()->showText(QCursor::pos(), "回收站内无项目", 1000);
        return;
    }

    // 2. 物理还原至未分类
    if (CategoryRepo::restoreFromTrashBatch(trashItems)) {
        m_categoryModel->refresh();
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#2ecc71;'>[OK] 已还原全部项目</b>", 1500, QColor("#2ecc71"));
    }
}

void CategoryPanel::setFocusHighlight(bool visible) {
    if (m_focusLine) m_focusLine->setVisible(visible);
}

void CategoryPanel::initUi() {
    // 2026-05-07 按照用户要求：修改焦点线颜色为蓝色
    m_focusLine = new QWidget(this);
    m_focusLine->setFixedHeight(1);
    m_focusLine->setStyleSheet(QString("background-color: %1;").arg(qssColor(PrimaryBlue)));
    m_focusLine->hide(); // 初始隐藏
    m_mainLayout->addWidget(m_focusLine);

    // 1. 标题栏
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
    headerLayout->setContentsMargins(15, 0, 5, 0); // 2026-xx-xx 按照用户要求：右侧保留 5px 呼吸边距
    headerLayout->setSpacing(5);                  // 2026-05-17 按照用户要求：间距统一为 5px

    QLabel* iconLabel = new QLabel(header);
    iconLabel->setPixmap(UiHelper::getIcon("folder_filled", PrimaryBlue, 18).pixmap(18, 18));
    headerLayout->addWidget(iconLabel);

    QLabel* titleLabel = new QLabel("分类", header);
    titleLabel->setStyleSheet(QString("font-size: 13px; font-weight: bold; color: %1; background: transparent; border: none;").arg(qssColor(PrimaryBlue)));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();

    m_mainLayout->addWidget(header);

    // 2. 内容区包裹容器 (物理还原 8, 8, 0, 8 呼吸边距)
    // 2026-06-xx 物理对齐：右侧边距设为 0，使滚动条贴合容器边缘
    QWidget* sbContent = new QWidget(this);
    sbContent->setStyleSheet("background: transparent; border: none;");
    auto* sbContentLayout = new QVBoxLayout(sbContent);
    sbContentLayout->setContentsMargins(8, 8, 0, 8);
    sbContentLayout->setSpacing(0);

    QString arrowRight = UiHelper::getSvgTempFilePath("arrow_right", PrimaryBlue);
    QString arrowDown  = UiHelper::getSvgTempFilePath("arrow_down",  PrimaryBlue);

    QString treeStyle = QString(R"(
        QTreeView { background-color: transparent; border: none; color: #CCC; outline: none; }
        
        QTreeView::branch {
            background-color: transparent;
            width: 20px;
        }

        QTreeView::branch:has-children:closed { image: url("%1"); }
        QTreeView::branch:has-children:open   { image: url("%2"); }
        QTreeView::branch:has-children:closed:has-siblings { image: url("%1"); }
        QTreeView::branch:has-children:open:has-siblings   { image: url("%2"); }

        QTreeView::item { height: 26px; padding-left: 0px; }
    )").arg(arrowRight).arg(arrowDown);

    // 物理还原：单树架构，合并系统项与用户分类
    m_categoryTree = new DropTreeView(this);
    m_categoryTree->setStyleSheet(treeStyle); 
    m_categoryTree->setItemDelegate(new CategoryDelegate(this));
    
    // 2026-04-12 关键修复：延迟初始化模型数据（仅构造空壳）
    m_categoryModel = new CategoryModel(CategoryModel::Both, this);
    
    // 2026-xx-xx 按照 Plan-98：注入代理模型
    m_proxyModel = new CategoryFilterProxyModel(this);
    m_proxyModel->setSourceModel(m_categoryModel);
    m_categoryTree->setModel(m_proxyModel);
    
    m_categoryTree->setHeaderHidden(true);
    m_categoryTree->setRootIsDecorated(true);
    m_categoryTree->setIndentation(20);
    m_categoryTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_categoryTree->setDragEnabled(true);
    m_categoryTree->setAcceptDrops(true);
    m_categoryTree->setDropIndicatorShown(true);
    // 核心修正：解除 InternalMove 模式封锁，允许接收外部容器（NavPanel/ContentPanel）的拖拽
    m_categoryTree->setDragDropMode(QAbstractItemView::DragDrop);
    m_categoryTree->setDefaultDropAction(Qt::MoveAction);
    m_categoryTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    
    // 2026-06-xx 按照用户要求：支持 Delete 键物理删除选中分类，使用 Action 提升快捷键响应等级
    QAction* deleteCatAction = new QAction(this);
    deleteCatAction->setShortcut(QKeySequence::Delete);
    deleteCatAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteCatAction, &QAction::triggered, this, &CategoryPanel::onDeleteCategory);
    m_categoryTree->addAction(deleteCatAction);

    m_categoryTree->installEventFilter(this);

    // 2026-03-xx 物理拦截：严禁加密分类在未解锁时被展开
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryTree, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        int id = index.data(IdRole).toInt();
        bool isEncrypted = index.data(EncryptedRole).toBool();
        
        // 物理修复：加密校验仅针对数据库分类（ID > 0），跳过系统项（ID < 0）
        if (isEncrypted && id > 0 && !m_unlockedIds.contains(id)) {
            // 物理阻断：立即折叠，防止闪烁
            m_categoryTree->collapse(index);
            // 异步触发校验，避免在信号回调中处理复杂 UI
            QTimer::singleShot(0, [this, index]() {
                if (tryUnlockCategory(index)) {
                    // 解锁成功后刷新状态并重新展开
                    m_categoryModel->setUnlockedIds(m_unlockedIds);
                    m_categoryModel->refresh();
                    m_categoryTree->expand(index);
                }
            });
        } else {
            // 2026-05-27 物理修复：展开时按需动态加载分类关联的文件，杜绝启动挂起
            m_categoryModel->loadCategoryItems(index);
        }
    });

    // 2026-03-xx 物理兼容：监听模型重置信号，在刷新后尝试恢复展开状态
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryModel, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
        // 同步解锁 ID 到模型
        m_categoryModel->setUnlockedIds(m_unlockedIds);
        
        // 物理防护：只有当模型确实有真实数据时，才暂存当前 UI 状态。
        // 如果当前是“加载中”或者为空，则不覆盖暂存值，保留从 Settings 加载或上一次有效的记录。
        bool hasRealData = false;
        if (m_categoryModel->rowCount() > 1) {
            hasRealData = true;
        } else if (m_categoryModel->rowCount() == 1) {
            QString type = m_categoryModel->index(0, 0).data(TypeRole).toString();
            if (type != "placeholder" && !m_categoryModel->index(0,0).data(Qt::DisplayRole).toString().contains("正在统计")) {
                hasRealData = true;
            }
        }

        if (hasRealData) {
            QSet<int> expandedIds;
            QStringList expandedNames;
            saveExpandedState(QModelIndex(), expandedIds, expandedNames);
            
            QList<int> idList;
            for (int id : expandedIds) idList << id;
            m_categoryTree->setProperty("expandedIds", QVariant::fromValue(idList));
            m_categoryTree->setProperty("expandedNames", expandedNames);
        } else {
        }
    });

    connect(m_categoryModel, &QAbstractItemModel::modelReset, this, [this]() {
        // 2026-06-xx 物理修复：采用 singleShot(0) 解决视图节点生成竞态，确保 setExpanded 绝对生效
        QTimer::singleShot(0, this, [this]() {
            QList<int> idList = m_categoryTree->property("expandedIds").value<QList<int>>();
            QStringList expandedNames = m_categoryTree->property("expandedNames").toStringList();
            
            QSet<int> expandedIds;
            for (int id : idList) expandedIds.insert(id);

            m_isRestoringState = true;
            m_categoryTree->blockSignals(true); // 物理阻断：防止展开动作触发 saveExpandedStateToSettings
            restoreExpandedState(QModelIndex(), expandedIds, expandedNames);
            m_categoryTree->blockSignals(false);
            m_isRestoringState = false;
        });
    });

    connect(m_categoryTree, &QTreeView::clicked, this, [this](const QModelIndex& proxyIndex) {
        QModelIndex index = m_proxyModel->mapToSource(proxyIndex);
        QString type = index.data(TypeRole).toString();
        QString name = index.data(NameRole).toString();
        int id = index.data(IdRole).toInt();
        QString path = index.data(PathRole).toString();
        bool isEncrypted = index.data(EncryptedRole).toBool();

        // 2026-03-xx 物理防御：加密分类点击时触发校验
        if (isEncrypted && id > 0 && !m_unlockedIds.contains(id)) {
            tryUnlockCategory(index);
            return;
        }

        // 核心联动：如果点击的是有效的分类、系统项或快速访问项
        if (!type.isEmpty()) {
             // 2026-06-xx 重构：点击项不再加载文件到树中，而是直接通过信号触发 ContentPanel 加载
             emit categorySelected(id, name, type, path);
        }
    });

    connect(m_categoryTree, &DropTreeView::pathsDropped, this, [this](const QStringList& paths, const QModelIndex& proxyIndex) {
        QModelIndex index = m_proxyModel->mapToSource(proxyIndex);
        // 2026-06-xx 彻底重构：物理递归遍历 + 分类镜像创建 + SHA-256 物理加固
        // 核心规则：文件夹拖入空白/分类均递归建树；文件入空白归未分类，入分类归该分类。
        int targetCatId = 0;
        bool isBlankDrop = false;

        if (index.isValid()) {
            QString type = index.data(TypeRole).toString();
            QString name = index.data(NameRole).toString();

            // 2026-06-xx 物理联动：拖拽到回收站
            if (type == "trash") {
                if (ShellHelper::moveToTrash(paths)) {
                    m_categoryModel->refresh();
                    MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
                    ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#e74c3c;'>已成功移入回收站</b>", 1500, ErrorRed);
                }
                return;
            }

            if (type == "category" && index.data(IdRole).toInt() > 0) {
                targetCatId = index.data(IdRole).toInt();
            } else if (name == "我的分类") {
                targetCatId = 0;
            } else {
                targetCatId = 0;
                isBlankDrop = true;
            }
        } else {
            targetCatId = 0;
            isBlankDrop = true;
        }

        // 2026-07-xx 按照 Plan-116：归一化逻辑，改为物理迁移至托管库根目录
        // 理由：入库动作由且仅由 USN Journal 触发。此处仅负责物理层面的“归拢”。
        if (!paths.isEmpty()) {
            // 2026-07-xx 按照 Development_Plan 2.2：拖拽入库冲突拦截
            QStringList finalPaths;
            for (const QString& p : paths) {
                std::wstring wp = p.toStdWString();
                if (MetadataManager::isInsideManagedLibrary(wp)) {
                    RuntimeMeta meta = MetadataManager::instance().getMeta(wp);
                    if (meta.ingestionStatus == 1) {
                        ToolTipOverlay::instance()->showText(QCursor::pos(), "该项目已入库，无需再次入库", 1500, QColor("#FECF0E"));
                        continue; 
                    }
                }
                finalPaths << p;
            }

            if (finalPaths.isEmpty()) return;

            QString firstPath = finalPaths.first();
            std::wstring volSerial = MetadataManager::getVolumeSerialNumber(firstPath.toStdWString());
            QString key = QString("ManagedFolder/Volume_%1").arg(QString::fromStdWString(volSerial));
            QString relPath = AppConfig::instance().getValue(key, "").toString();
            QString drive = firstPath.left(3);
            QString managedRoot = QDir::toNativeSeparators(drive + relPath);

            if (!managedRoot.isEmpty()) {
                ImportHelper::importPaths(finalPaths, managedRoot, this);
            }
        }
    });
    
    sbContentLayout->addWidget(m_categoryTree);
    m_mainLayout->addWidget(sbContent, 1);

    // 2026-xx-xx 按照 Plan-98：新增底部搜索过滤框
    QWidget* searchContainer = new QWidget(this);
    searchContainer->setFixedHeight(40);
    // 2026-06-xx 视觉优化：移除冗余 border-top 分割线
    searchContainer->setStyleSheet("background: transparent; border-top: none;");
    QHBoxLayout* searchLayout = new QHBoxLayout(searchContainer);
    searchLayout->setContentsMargins(10, 0, 10, 0);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("筛选分类...");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(32); // 2026-06-xx 物理归一化：与主搜索框对齐至 32px
    
    m_searchEdit->setStyleSheet(QString(
        "QLineEdit {"
        "  background: #1E1E1E;"
        "  color: #EEEEEE;"
        "  border: 1px solid #444;"
        "  border-radius: 6px;"        // 2026-06-xx 规范修正：从 8px 回归至全局 6px 规范
        "  padding: 0 8px 0 1px;"     // 2026-06-xx 物理修正：1px Padding + 约 7px 系统预留 = 8px 视觉间距
        "  font-size: 12px;"
        "}"
        "QLineEdit:focus { border-color: %1; }"
    ).arg(qssColor(PrimaryBlue)));

    // 2026-06-xx 视觉优化：将 select 图标替换为更符合语境的 filter_funnel_outline
    QAction* leadingIcon = m_searchEdit->addAction(UiHelper::getIcon("filter_funnel_outline", QColor("#888888"), 16), QLineEdit::LeadingPosition);
    Q_UNUSED(leadingIcon);

    searchLayout->addWidget(m_searchEdit);
    m_mainLayout->addWidget(searchContainer);

    // 2026-xx-xx 按照 Plan-106：防抖处理
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (text.isEmpty()) {
            m_searchTimer->stop();
            onSearchTextChanged(""); // 清空时立即响应
            return;
        }
        m_searchTimer->start();
    });

    // 2026-03-xx 物理记忆：初始化后加载持久化的展开状态
    QTimer::singleShot(100, this, &CategoryPanel::loadExpandedStateFromSettings);

    // 2026-03-xx 物理记忆：连接展开/折叠信号，实时持久化
    connect(m_categoryTree, &QTreeView::expanded, this, &CategoryPanel::saveExpandedStateToSettings);
    connect(m_categoryTree, &QTreeView::collapsed, this, &CategoryPanel::saveExpandedStateToSettings);
    // 2026-06-xx 物理同步：支持内部拖拽重排持久化
    connect(m_categoryModel, &QAbstractItemModel::rowsMoved, this, [this](const QModelIndex&, int, int, const QModelIndex&, int) {
        // 核心逻辑：深度优先遍历“我的分类”子树，根据 UI 层级物理同步 DB 中的 parent_id 与 sort_order
        std::function<void(const QModelIndex&, int)> syncSubtree;
        syncSubtree = [&](const QModelIndex& parentIdx, int parentIdInDb) {
            for (int i = 0; i < m_categoryModel->rowCount(parentIdx); ++i) {
                QModelIndex childIdx = m_categoryModel->index(i, 0, parentIdx);
                int id = childIdx.data(IdRole).toInt();
                QString type = childIdx.data(TypeRole).toString();
                bool isPinned = childIdx.data(PinnedRole).toBool();

                // 物理阻断：严禁处理“镜像节点”（即 Pinned 为 true 且其父项不是“我的分类”的节点）。
                // 理由：镜像节点仅作为 UI 快捷方式，其移动不应改写原始数据库中的 parentId 关系。
                if (parentIdInDb != -1 && isPinned && parentIdx.data(NameRole).toString() != "我的分类" && parentIdx.data(TypeRole).toString() != "category") {
                    continue;
                }

                if (type == "category" && id > 0) {
                    // 只有在数据真正发生位移时才触发数据库 UPDATE，优化性能
                    auto all = CategoryRepo::getAll();
                    for (auto& cat : all) {
                        if (cat.id == id) {
                            if (cat.parentId != parentIdInDb || cat.sortOrder != i) {
                                cat.parentId = parentIdInDb;
                                cat.sortOrder = i;
                                CategoryRepo::update(cat);
                            }
                            break;
                        }
                    }
                    // 递归同步子分类
                    syncSubtree(childIdx, id);
                } else if (childIdx.data(NameRole).toString() == "我的分类") {
                    // 进入“我的分类”根容器
                    syncSubtree(childIdx, 0);
                }
            }
        };
        syncSubtree(QModelIndex(), -1); // 从隐式根开始，-1 表示尚未进入有效分类区
    });
}

void CategoryPanel::saveExpandedStateToSettings() {
    if (m_isRestoringState) {
        return;
    }
    if (!m_categoryModel || m_categoryModel->rowCount() <= 0) return;

    // 物理防御：如果只有一个项且是加载中占位符，严禁保存，防止清空用户的历史记忆
    if (m_categoryModel->rowCount() == 1) {
        QModelIndex first = m_categoryModel->index(0, 0);
        QString type = first.data(TypeRole).toString();
        if (type == "placeholder" || first.data(Qt::DisplayRole).toString().contains("正在统计")) {
            return;
        }
    }

    QSet<int> ids;
    QStringList names;
    saveExpandedState(QModelIndex(), ids, names);


    QList<QVariant> idList;
    for (int id : ids) idList << id;
    AppConfig::instance().setValue("Category/ExpandedIds", idList);
    AppConfig::instance().setValue("Category/ExpandedNames", names);
    AppConfig::instance().sync(); // 物理落盘
}

void CategoryPanel::loadExpandedStateFromSettings() {
    bool hasRecord = !AppConfig::instance().getValue("Category/ExpandedIds").isNull() || !AppConfig::instance().getValue("Category/ExpandedNames").isNull();
    
    QList<QVariant> idList = AppConfig::instance().getValue("Category/ExpandedIds").toList();
    QStringList names = AppConfig::instance().getValue("Category/ExpandedNames").toStringList();


    // 核心修复：将从设置读取的状态同步到 Tree 属性中，确保异步加载完成后 modelReset 能自动恢复
    m_categoryTree->setProperty("expandedIds", QVariant::fromValue(idList));
    m_categoryTree->setProperty("expandedNames", names);
    m_categoryTree->setProperty("hasHistoryRecord", hasRecord);

    QSet<int> ids;
    for (const auto& v : idList) ids.insert(v.toInt());

    // 同时也尝试立即恢复一次（兼容同步加载场景）
    m_isRestoringState = true;
    m_categoryTree->blockSignals(true);
    restoreExpandedState(QModelIndex(), ids, names);
    m_categoryTree->blockSignals(false);
    m_isRestoringState = false;
}

bool CategoryPanel::tryUnlockCategory(const QModelIndex& index) {
    int id = index.data(IdRole).toInt();
    if (id <= 0) return false;

    QString hint = index.data(EncryptHintRole).toString();

    // 2026-03-xx 物理级还原：废弃通用输入框，改用 1:1 复刻的旧版验证界面
    CategoryLockDialog dlg(hint, this);
    if (dlg.exec() == QDialog::Accepted) {
        // [SIMULATION] 校验成功
        m_unlockedIds.insert(id);
        
        // 物理补丁：解锁后由于图标需要刷新，强制同步 ID 并进行一次模型重刷
        m_categoryModel->setUnlockedIds(m_unlockedIds);
        m_categoryModel->refresh();
        
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#00A650;'>[OK] 验证成功，分类已解锁</b>", 1000, QColor("#00A650"));
        return true;
    }
    return false;
}

bool CategoryPanel::eventFilter(QObject* obj, QEvent* event) {
    // 2026-06-xx 按照用户要求：补全对 ToolTipOverlay 的物理拦截与映射逻辑
    // 理由：主窗口无法自动拦截深层嵌套子组件的 Hover 事件，需在组件层手动分发
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::Enter) {
        QString text = obj->property("tooltipText").toString();
        if (!text.isEmpty()) {
            ToolTipOverlay::instance()->showText(QCursor::pos(), text);
        }
    } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave) {
        ToolTipOverlay::hideTip();
    }

    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        
        // 2026-06-xx 按照用户要求：禁用 Ctrl+A 全选
        if (obj == m_categoryTree && keyEvent->modifiers() == Qt::ControlModifier && keyEvent->key() == Qt::Key_A) {
            return true; 
        }

        // 2026-06-xx 按照用户要求：支持 Delete 键物理删除选中分类
        if (obj == m_categoryTree && keyEvent->key() == Qt::Key_Delete) {
            onDeleteCategory();
            return true;
        }

        if (keyEvent->key() == Qt::Key_Escape) {
            // [UX] 两段式：查找对话框内的第一个非空输入框
            QLineEdit* edit = findChild<QLineEdit*>();
            if (edit && !edit->text().isEmpty()) {
                edit->clear();
                return true;
            }
        }
    }
    return QFrame::eventFilter(obj, event);
}

} // namespace ArcMeta
