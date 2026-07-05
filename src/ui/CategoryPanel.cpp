#include "CategoryPanel.h"
#include "CategoryModel.h"
#include "CategoryLockDialog.h"
#include "CategorySetPasswordDialog.h"
#include "CategoryDelegate.h"
#include "DropTreeView.h"
#include "UiHelper.h"
#include "StyleLibrary.h"
using namespace ArcMeta::Style;
#include "ToolTipOverlay.h"
#include "FramelessDialog.h"
#include "ProgressDialog.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include "../db/CategoryRepo.h"
#include "../db/ItemRepo.h"
#include "../db/SyncEngine.h"
#include "../meta/MetadataManager.h"
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
#include <QColorDialog>
#include "../core/AppConfig.h"
#include <QSqlDatabase>
#include <QSqlQuery>
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
    
    setObjectName("SidebarContainer");
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(230);
    setStyleSheet(QString("color: %1;").arg(qssColor(TextMain)));
    
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    // 2026-05-28 物理增强：提前从持久化状态初始化底层引擎模式
    bool isJson = AppConfig::instance().getValue("Category/IsJsonMode", false).toBool();
    CategoryRepo::setJsonMode(isJson);
    if (isJson) {
        MetadataManager::instance().initFromJsonMode();
    } else {
        MetadataManager::instance().initFromDatabase();
    }

    initUi();
    setupContextMenu();
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
        // 2026-03-xx 物理阻断：通过代码强制选中时，必须锁定信号发射，防止与 ContentPanel 形成回环死循环
        m_categoryTree->blockSignals(true);
        m_categoryTree->setCurrentIndex(target);
        m_categoryTree->scrollTo(target);
        m_categoryTree->blockSignals(false);
    }
}

void CategoryPanel::deferredInit() {
    qDebug() << "[CategoryPanel] deferredInit 开始执行";

    // 2026-04-12 关键修复：延迟执行数据库数据加载
    if (m_categoryModel) {
        m_categoryModel->deferredRefresh();
    }
    qDebug() << "[CategoryPanel] deferredInit 执行完毕";
}

void CategoryPanel::setupContextMenu() {
    m_categoryTree->setContextMenuPolicy(Qt::CustomContextMenu);
    // 2026-05-27 物理加固：补全 this 上下文
    connect(m_categoryTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QModelIndex index = m_categoryTree->indexAt(pos);
        
        // 2026-03-xx 按照用户要求：实现右键点击即选中，解决“分类与其子分类”交互一致性问题
        if (index.isValid()) {
            m_categoryTree->setCurrentIndex(index);
        }

        QMenu menu(this);
        // [PHYSICAL RESTORATION] 8px radius for context menu
    menu.setStyleSheet(QString("QMenu { background-color: #2D2D2D; color: #EEE; border: 1px solid %1; padding: 4px; border-radius: 8px; } "
                           "QMenu::item { padding: 6px 25px 6px 10px; border-radius: 4px; } "
                           "QMenu::icon { margin-left: 6px; } "
                           "QMenu::item:selected { background-color: #3E3E42; color: white; } "
                       "QMenu::separator { height: 1px; background: %1; margin: 4px 8px; }").arg(qssColor(BorderDark)));

        // 基于规范逻辑：如果没有选中项，或者选中了“我的分类”根节点
        QString itemName = index.data(NameRole).toString();
        if (!index.isValid() || itemName == "我的分类") {
            menu.addAction(UiHelper::getIcon("folder_filled", QColor("#aaaaaa"), 18), "新建分类", this, &CategoryPanel::onCreateCategory);
            
            auto* sortMenu = menu.addMenu(UiHelper::getIcon("list_ul", QColor("#aaaaaa"), 18), "排列");
            sortMenu->setStyleSheet(menu.styleSheet());
            sortMenu->addAction("标题(全部) (A→Z)", this, &CategoryPanel::onSortAllByNameAsc);
            sortMenu->addAction("标题(全部) (Z→A)", this, &CategoryPanel::onSortAllByNameDesc);
        } else {
            // 2026-03-xx 按照用户要求：补全子层级（子分类、文件、文件夹）的右键菜单
            QString type = index.data(TypeRole).toString();
            
            // 只要不是系统根节点，都弹出完整菜单
            if (type == "category" || type == "file" || type == "folder") {
                
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
                menu.addAction(UiHelper::getIcon("pin_vertical", isPinned ? BrandOrange : TextMuted, 18), 
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
            menu.exec(m_categoryTree->viewport()->mapToGlobal(pos));
        }
    });
}

/**
 * @brief 递归保存 QTreeView 的展开状态
 */
static void saveExpandedState(QTreeView* tree, const QModelIndex& parent, QSet<int>& expandedIds, QStringList& expandedNames) {
    if (!tree || !tree->model()) return;
    int rowCount = tree->model()->rowCount(parent);
    for (int i = 0; i < rowCount; ++i) {
        QModelIndex idx = tree->model()->index(i, 0, parent);
        if (tree->isExpanded(idx)) {
            int id = idx.data(IdRole).toInt();
            QString name = idx.data(NameRole).toString();
            if (id != 0) {
                expandedIds.insert(id);
                qDebug() << "[CategoryPanel] 正在记录展开项: ID =" << id << "名称 =" << name;
            } else {
                expandedNames << name;
                qDebug() << "[CategoryPanel] 正在记录展开项: 系统项 =" << name;
            }
            saveExpandedState(tree, idx, expandedIds, expandedNames);
        }
    }
}

/**
 * @brief 递归恢复 QTreeView 的展开状态
 * 2026-03-xx 物理拦截：加密且未解锁的分类在恢复时强制跳过展开
 */
static void restoreExpandedState(QTreeView* tree, const QModelIndex& parent, const QSet<int>& expandedIds, const QStringList& expandedNames, const QSet<int>& unlockedIds) {
    if (!tree || !tree->model()) return;
    
    bool hasHistory = tree->property("hasHistoryRecord").toBool();
    int rowCount = tree->model()->rowCount(parent);
    
    for (int i = 0; i < rowCount; ++i) {
        QModelIndex idx = tree->model()->index(i, 0, parent);
        int id = idx.data(IdRole).toInt();
        QString name = idx.data(NameRole).toString();
        bool isEncrypted = idx.data(EncryptedRole).toBool();
        
        bool shouldExpand = false;
        
        // 逻辑优先级：
        // 1. 明确在记录中的项 (无论是 ID 还是名称匹配)
        // 2026-06-xx 物理修复：ID 匹配逻辑支持负数（系统项）
        if (expandedNames.contains(name) || (id != 0 && expandedIds.contains(id))) {
            shouldExpand = true;
        }
        // 2. 核心根节点“我的分类”和“快速访问”始终无条件物理强开，杜绝刷新折叠
        else if (name == "我的分类" || name == "快速访问") {
            shouldExpand = true;
        }
        // 3. 如果没有任何历史记录（首次运行），默认展开“我的分类”下的第一级子分类（红圈部分）
        else if (!hasHistory) {
            QModelIndex pIdx = idx.parent();
            if (pIdx.isValid() && pIdx.data(NameRole).toString() == "我的分类") {
                shouldExpand = true;
            }
        }

        // 物理硬核防御：加密且未解锁的分类禁止展开 (仅针对 ID > 0 的数据库分类)
        if (shouldExpand && isEncrypted && id > 0 && !unlockedIds.contains(id)) {
            qDebug() << "[CategoryPanel] 恢复展开时拦截加密分类:" << name;
            shouldExpand = false;
        }

        if (shouldExpand) {
            qDebug() << "[CategoryPanel] 正在执行展开动作:" << name;
            tree->setExpanded(idx, true);
            restoreExpandedState(tree, idx, expandedIds, expandedNames, unlockedIds);
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
            saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);

            CategoryRepo::add(cat);
            m_categoryModel->refresh();

            restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
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
            saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);
            expandedIds.insert(id);

            CategoryRepo::add(cat);
            m_categoryModel->refresh();

            restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
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
    saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);

    m_categoryModel->refresh();

    restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);

    QString name = index.data(NameRole).toString();
    ToolTipOverlay::instance()->showText(QCursor::pos(), QString("已设为归类目标: %1").arg(name), 1000);
}

void CategoryPanel::onSetColor() {
    QModelIndex index = m_categoryTree->currentIndex();
    int id = getTargetCategoryId(index);
    if (id <= 0) return;

    // 2026-03-xx 按照用户要求：使用 QColorDialog 弹出颜色选择
    QColor color = QColorDialog::getColor(Qt::white, this, "选择分类颜色");
    if (!color.isValid()) return;

    auto all = CategoryRepo::getAll();
    for(auto& cat : all) {
        if(cat.id == id) {
            cat.color = color.name().toStdWString();
            CategoryRepo::update(cat);
            break;
        }
    }
    
    QSet<int> expandedIds;
    QStringList expandedNames;
    saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);

    m_categoryModel->refresh();

    restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
    
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
    saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);

    m_categoryModel->refresh();

    restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
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
        saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);

        CategoryRepo::update(current);
        m_categoryModel->refresh();

        restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
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
    saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);

    m_categoryModel->refresh();

    restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
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
        saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);

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

        restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
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
        saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);

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

        restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
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
    headerLayout->setContentsMargins(15, 0, 5, 0); // 2026-05-17 按照用户要求：右侧边距统一设为 5px，上下 0px 垂直居中
    headerLayout->setSpacing(5);                  // 2026-05-17 按照用户要求：间距统一为 5px

    QLabel* iconLabel = new QLabel(header);
    iconLabel->setPixmap(UiHelper::getIcon("folder_filled", PrimaryBlue, 18).pixmap(18, 18));
    headerLayout->addWidget(iconLabel);

    QLabel* titleLabel = new QLabel("分类", header);
    titleLabel->setStyleSheet(QString("font-size: 13px; font-weight: bold; color: %1; background: transparent; border: none;").arg(qssColor(PrimaryBlue)));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();

    // 2026-05-17 按照用户要求：在“手动全量扫描与对账”按钮左侧新增一个开关切换按钮
    // 2026-06-xx 物理增强：实现状态持久化加载 (方案 A)
    m_btnSwitch = new QPushButton(header);
    m_btnSwitch->setFixedSize(24, 24);
    m_btnSwitch->setCheckable(true);
    m_btnSwitch->setFlat(true);
    m_btnSwitch->setCursor(Qt::PointingHandCursor);
    m_btnSwitch->setIconSize(QSize(16, 16));
    m_btnSwitch->setStyleSheet("QPushButton { border: none; background: transparent; } QPushButton:hover { background: rgba(255,255,255,0.1); border-radius: 4px; }");
    m_btnSwitch->installEventFilter(this);

    // 回填初始状态
    bool isJsonMode = AppConfig::instance().getValue("Category/IsJsonMode", false).toBool();
    m_btnSwitch->setChecked(isJsonMode);
    if (isJsonMode) {
        m_btnSwitch->setIcon(UiHelper::getIcon("switch_on", SuccessGreen));
        m_btnSwitch->setProperty("tooltipText", "工作模式切换：当前为JSON模式（内存）");
    } else {
        m_btnSwitch->setIcon(UiHelper::getIcon("switch_off", TextDim));
        m_btnSwitch->setProperty("tooltipText", "工作模式切换：当前为数据库模式");
    }

    connect(m_btnSwitch, &QPushButton::clicked, this, [this](bool checked) {
        // 2026-05-29 优化点 1：增加切换确认机制 (Plan-44)
        QString modeName = checked ? "JSON 模式 (内存)" : "数据库模式";
        FramelessConfirmDialog confirmDlg("模式切换确认", QString("切换到<b>%1</b>将重新加载数据引擎，是否继续？").arg(modeName), this);
        if (confirmDlg.exec() != QDialog::Accepted) {
            m_btnSwitch->blockSignals(true);
            m_btnSwitch->setChecked(!checked);
            m_btnSwitch->blockSignals(false);
            return;
        }

        // 2026-05-29 优化点 2：增强切换时的视觉反馈 (UX)
        m_btnSwitch->setEnabled(false);
        m_btnSwitch->setIcon(UiHelper::getIcon("sync", WarningOrange)); // 临时旋转图标或等待图标

        // 2026-05-29 优化点 3：异常处理与原子性保证
        if (!CategoryRepo::syncDatabaseAndJson()) {
            ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#E74C3C;'>[ERROR] 双轨同步失败，已中止切换</b>", 2000, ErrorRed);
            m_btnSwitch->blockSignals(true);
            m_btnSwitch->setChecked(!checked);
            m_btnSwitch->setIcon(UiHelper::getIcon(!checked ? "switch_on" : "switch_off", !checked ? SuccessGreen : TextDim));
            m_btnSwitch->blockSignals(false);
            m_btnSwitch->setEnabled(true);
            return;
        }

        if (checked) {
            m_btnSwitch->setIcon(UiHelper::getIcon("switch_on", SuccessGreen));
            m_btnSwitch->setProperty("tooltipText", "工作模式切换：当前为JSON模式（内存）");
        } else {
            m_btnSwitch->setIcon(UiHelper::getIcon("switch_off", TextDim));
            m_btnSwitch->setProperty("tooltipText", "工作模式切换：当前为数据库模式");
        }

        // 1. 设置底层分类库模式
        CategoryRepo::setJsonMode(checked);

        // 2. 促使元数据管理器进行数据重载与重构
        if (checked) {
            MetadataManager::instance().initFromJsonMode();
        } else {
            MetadataManager::instance().initFromDatabase();
        }

        // 3. 持久化状态
        AppConfig::instance().setValue("Category/IsJsonMode", checked);
        AppConfig::instance().sync(); // 物理落盘，防止异常退出回滚

        // 4. 树模型重新拉取
        if (m_categoryModel) {
            m_categoryModel->refresh();
        }

        m_btnSwitch->setEnabled(true);
        // 2026-05-29 优化点 4：状态栏/气泡即时反馈
        ToolTipOverlay::instance()->showText(QCursor::pos(), QString("<b style='color:#2ecc71;'>已切换至 %1</b>").arg(modeName), 1500, QColor("#2ecc71"));
    });
    headerLayout->addWidget(m_btnSwitch, 0, Qt::AlignVCenter);

    // 2026-06-xx 按照用户要求：从状态栏迁移至此，执行手动全量扫描与对账
    QPushButton* btnRescan = new QPushButton(header);
    btnRescan->setFixedSize(24, 24); // 适当放大以适应标题栏高度
    btnRescan->setIcon(UiHelper::getIcon("sync", TextDim)); 
    btnRescan->setIconSize(QSize(16, 16));
    btnRescan->setFlat(true);
    btnRescan->setCursor(Qt::PointingHandCursor);
    btnRescan->setStyleSheet("QPushButton { border: none; background: transparent; } QPushButton:hover { background: rgba(255,255,255,0.1); border-radius: 4px; }");
    btnRescan->setProperty("tooltipText", "手动全量扫描与对账");
    btnRescan->installEventFilter(this); // 2026-06-xx 按照规范：安装过滤器以驱动自定义 ToolTip
    connect(btnRescan, &QPushButton::clicked, this, [this]() {
        // 1. 全量双向分类与项映射物理对账同步
        CategoryRepo::syncDatabaseAndJson();

        // 2. 瞬时刷新界面树展示
        if (m_categoryModel) {
            m_categoryModel->refresh();
        }

        // 3. 启动后台文件与分布式 USN 对账扫描
        (void)QtConcurrent::run([]() {
            SyncEngine::instance().runFullScan({}, nullptr);
        });
    });
    headerLayout->addWidget(btnRescan, 0, Qt::AlignVCenter);

    m_mainLayout->addWidget(header);

    // 2. 内容区包裹容器 (物理还原 8, 8, 8, 8 呼吸边距)
    QWidget* sbContent = new QWidget(this);
    sbContent->setStyleSheet("background: transparent; border: none;");
    auto* sbContentLayout = new QVBoxLayout(sbContent);
    sbContentLayout->setContentsMargins(8, 8, 8, 8);
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
    m_categoryTree->setModel(m_categoryModel);
    
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
    QAction* deleteAction = new QAction(this);
    deleteAction->setShortcut(QKeySequence::Delete);
    deleteAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteAction, &QAction::triggered, this, &CategoryPanel::onDeleteCategory);
    m_categoryTree->addAction(deleteAction);

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
            qDebug() << "[CategoryPanel] 模型即将重置，暂存当前有效展开状态...";
            QSet<int> expandedIds;
            QStringList expandedNames;
            saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);
            
            QList<int> idList;
            for (int id : expandedIds) idList << id;
            m_categoryTree->setProperty("expandedIds", QVariant::fromValue(idList));
            m_categoryTree->setProperty("expandedNames", expandedNames);
        } else {
            qDebug() << "[CategoryPanel] 当前模型处于加载态或为空，跳过暂存以保护既有恢复属性。";
        }
    });

    connect(m_categoryModel, &QAbstractItemModel::modelReset, this, [this]() {
        // 2026-06-xx 物理修复：采用 singleShot(0) 解决视图节点生成竞态，确保 setExpanded 绝对生效
        QTimer::singleShot(0, this, [this]() {
            qDebug() << "[CategoryPanel] 模型已重置且视图已就绪，正在执行物理强开恢复...";
            QList<int> idList = m_categoryTree->property("expandedIds").value<QList<int>>();
            QStringList expandedNames = m_categoryTree->property("expandedNames").toStringList();
            
            QSet<int> expandedIds;
            for (int id : idList) expandedIds.insert(id);

            m_isRestoringState = true;
            m_categoryTree->blockSignals(true); // 物理阻断：防止展开动作触发 saveExpandedStateToSettings
            restoreExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames, m_unlockedIds);
            m_categoryTree->blockSignals(false);
            m_isRestoringState = false;
            qDebug() << "[CategoryPanel] 物理级展开状态恢复完成。";
        });
    });

    connect(m_categoryTree, &QTreeView::clicked, this, [this](const QModelIndex& index) {
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

    connect(m_categoryTree, &DropTreeView::pathsDropped, this, [this](const QStringList& paths, const QModelIndex& index) {
        // 2026-06-xx 彻底重构：物理递归遍历 + 分类镜像创建 + SHA-256 物理加固
        // 核心规则：文件夹拖入空白/分类均递归建树；文件入空白归未分类，入分类归该分类。
        int targetCatId = 0;
        bool isBlankDrop = false;

        if (index.isValid()) {
            QString type = index.data(TypeRole).toString();
            QString name = index.data(NameRole).toString();
            if (type == "category") {
                targetCatId = index.data(IdRole).toInt();
            } else if (name == "我的分类") {
                // 2026-06-xx 物理修复：拖拽到“我的分类”根节点，同样触发自动创建分类
                targetCatId = 0;
                isBlankDrop = true;
            } else {
                isBlankDrop = true;
            }
        } else {
            isBlankDrop = true;
        }

        // 2026-06-xx 工业级 UX 优化：拖拽到空白处或根节点时，自动以第一个项目的名称创建新分类
        if (isBlankDrop && !paths.isEmpty()) {
            QString firstPath = paths.first();
            QString newCatName = QFileInfo(firstPath).fileName();
            
            Category newCat;
            newCat.name = newCatName.toStdWString();
            newCat.parentId = 0;
            newCat.color = getDefaultCategoryColor();
            
            if (CategoryRepo::add(newCat)) {
                targetCatId = newCat.id;
                m_categoryModel->refresh(); // 刷新模型以显示新分类
                Logger::log(QString("[分类面板] 自动创建分类: %1 (ID: %2)").arg(newCatName).arg(targetCatId));
            }
        }

        ProgressDialog* progress = new ProgressDialog("正在同步导入文件夹(递归)...", this);
        progress->show();
        
        (void)QThreadPool::globalInstance()->start([this, paths, targetCatId, progress]() {
            QSqlDatabase db = ArcMeta::Database::instance().getThreadDatabase();
            
            // A. 第一阶段：快速物理统计总项数 (包含文件夹)
            int totalItems = 0;
            for (const QString& path : paths) {
                QFileInfo info(path);
                if (info.isDir()) {
                    QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
                    while (it.hasNext()) { 
                        QString p = it.next();
                        if (QFileInfo(p).fileName() == ".am_meta.json") continue; // 2026-06-xx 物理隔离
                        totalItems++; 
                    }
                }
                totalItems++;
            }

            QMetaObject::invokeMethod(progress, [progress, totalItems]() {
                progress->setRange(0, totalItems);
                progress->setValue(0);
            });

            auto allCats = CategoryRepo::getAll();
            int currentTask = 0;
            db.transaction();

            // 辅助 Lambda：确保分类存在，不存在则创建
            auto ensureCategory = [&](const std::wstring& name, int parentId) -> int {
                for (const auto& c : allCats) {
                    if (c.parentId == parentId && c.name == name) return c.id;
                }
                Category cat;
                cat.name = name;
                cat.parentId = parentId;
                cat.color = getDefaultCategoryColor();
                if (CategoryRepo::add(cat)) {
                    allCats.push_back(cat);
                    return cat.id;
                }
                return 0;
            };

            QMap<QString, int> pathIdMap;

            // 辅助处理函数：执行物理入库并归类
            auto processItem = [&](const QString& itemPath, int catId) {
                QFileInfo info(itemPath);
                std::wstring wPath = QDir::toNativeSeparators(itemPath).toStdWString();
                std::string fid;
                std::wstring frn; // 物理身份 FRN (强制要求使用)
                long long size = 0, ctime = 0, mtime = 0;

                // 2026-06-xx 物理加固：调用 fetchWinApiMetadataDirect 提取 FRN 与 SHA-256 ID
                if (MetadataManager::fetchWinApiMetadataDirect(wPath, fid, &frn, &size, nullptr, &ctime, &mtime)) {
                    std::wstring vol = MetadataManager::getVolumeSerialNumber(wPath);
                    std::wstring parentDir = QDir::toNativeSeparators(info.absolutePath()).toStdWString();
                    
                    // 1. 物理入库（使用真实 FRN 杜绝冲突）
                    ItemRepo::saveBasicInfo(vol, frn, wPath, parentDir, info.isDir(), (double)mtime, (double)size, (double)ctime, fid);
                    
                    // 2. 执行归类关联 (如果 catId > 0)
                    if (catId > 0) {
                        CategoryRepo::addItemToCategory(catId, fid);
                    }

                    // 2026-06-xx 物理同步：触发元数据持久化以生成 .am_meta.json，实现“目录导航”状态感应
                    MetadataManager::instance().syncPhysicalMetadata(wPath);

                    // 2026-06-xx 按照用户要求：针对特定格式文件，在拖拽导入时自动进行颜色解析
                    QString ext = info.suffix().toLower();
                    if (UiHelper::isGraphicsFile(ext)) {
                        // 1. 提取全量色板
                        auto palette = UiHelper::extractPalette(itemPath);
                        if (!palette.isEmpty()) {
                            // 2. 提取第一个颜色作为主色调并量化
                            QColor dominant = UiHelper::quantizeColor(palette.first().first);
                            QString colorHex = dominant.name().toUpper();

                            // 3. 物理双重存储：主色 + 全量变长色板
                            MetadataManager::instance().setColor(wPath, colorHex.toStdWString());
                            MetadataManager::instance().setPalettes(wPath, palette);
                        }
                    }
                }
            };

            for (const QString& rootPath : paths) {
                QFileInfo rootInfo(rootPath);
                QString nativeRoot = QDir::toNativeSeparators(rootPath);
                
                if (rootInfo.isDir()) {
                    // 文件夹逻辑：自动创建分类节点
                    int rootCatId = ensureCategory(rootInfo.fileName().toStdWString(), targetCatId);
                    pathIdMap[nativeRoot] = rootCatId;
                    
                    // 物理递归：1:1 复刻文件夹层级到分类树
                    QDirIterator it(rootPath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
                    while (it.hasNext()) {
                        QString itemPath = it.next();
                        QFileInfo info = it.fileInfo();
                        QString nativePath = QDir::toNativeSeparators(itemPath);
                        QString nativeParent = QDir::toNativeSeparators(info.absolutePath());
                        
                        int currentParentId = pathIdMap.value(nativeParent, rootCatId);

                        if (info.isDir()) {
                            // 创建子分类并记录 ID
                            int newId = ensureCategory(info.fileName().toStdWString(), currentParentId);
                            pathIdMap[nativePath] = newId;
                            // 2026-06-xx 物理同步：文件夹入库后同样同步元数据
                            MetadataManager::instance().syncPhysicalMetadata(info.absoluteFilePath().toStdWString());
                        } else {
                            if (info.fileName() == ".am_meta.json") continue; // 2026-06-xx 物理隔离
                            // 文件入库并关联到当前层级分类
                            processItem(itemPath, currentParentId);
                        }

                        currentTask++;
                        if (currentTask % 100 == 0) {
                            db.commit(); db.transaction();
                            QMetaObject::invokeMethod(progress, [progress, currentTask, nativePath]() {
                                progress->setValue(currentTask);
                                progress->setStatus("正在导入: " + QFileInfo(nativePath).fileName());
                            });
                        }
                    }
                } else {
                    // 单个文件逻辑：入空白归未分类(catId=0)，入分类归该分类
                    processItem(rootPath, targetCatId);
                    currentTask++;
                }
            }
            
            db.commit();

            QMetaObject::invokeMethod(this, [this, progress]() {
                progress->accept();
                progress->deleteLater();
                
                QSet<int> expandedIds;
                QStringList expandedNames;
                saveExpandedState(m_categoryTree, QModelIndex(), expandedIds, expandedNames);
                
                QList<int> idList;
                for (int id : expandedIds) idList << id;
                m_categoryTree->setProperty("expandedIds", QVariant::fromValue(idList));
                m_categoryTree->setProperty("expandedNames", expandedNames);

                m_categoryModel->refresh();
                
                ToolTipOverlay::instance()->showText(QCursor::pos(), 
                    "<b style='color:#2ecc71;'>已完成递归分类镜像导入</b>", 1500, QColor("#2ecc71"));
            }, Qt::QueuedConnection);
        });
    });
    
    sbContentLayout->addWidget(m_categoryTree);
    m_mainLayout->addWidget(sbContent, 1);

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
        qDebug() << "[CategoryPanel] 正在恢复状态中，锁定保存信号，防止由于 UI 展开动作反向覆盖磁盘记录。";
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
    saveExpandedState(m_categoryTree, QModelIndex(), ids, names);

    qDebug() << "[CategoryPanel] 正在持久化展开状态到磁盘 - 记录总数:" << ids.size() + names.size();

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

    qDebug() << "[CategoryPanel] 从 AppConfig 加载记忆 - 记录存在:" << hasRecord << "ID数:" << idList.size() << "系统项数:" << names.size();

    // 核心修复：将从设置读取的状态同步到 Tree 属性中，确保异步加载完成后 modelReset 能自动恢复
    m_categoryTree->setProperty("expandedIds", QVariant::fromValue(idList));
    m_categoryTree->setProperty("expandedNames", names);
    m_categoryTree->setProperty("hasHistoryRecord", hasRecord);

    QSet<int> ids;
    for (const auto& v : idList) ids.insert(v.toInt());

    // 同时也尝试立即恢复一次（兼容同步加载场景）
    m_isRestoringState = true;
    m_categoryTree->blockSignals(true);
    restoreExpandedState(m_categoryTree, QModelIndex(), ids, names, m_unlockedIds);
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
