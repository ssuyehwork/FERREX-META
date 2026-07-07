#include "TagManagerView.h"
#include "UiHelper.h"
#include "StyleLibrary.h"
#include "MetaPanel.h"
#include "../meta/MetadataManager.h"
#include "../meta/DatabaseManager.h"
#include "sqlite3.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QFrame>
#include <QGridLayout>
#include <QMenu>
#include <QDebug>
#include <QPointer>
#include <QtConcurrent>
#include "FramelessDialog.h"

using namespace ArcMeta::Style;

namespace ArcMeta {

TagManagerView::TagManagerView(QWidget* parent) : QWidget(parent) {
    initUi();
}

void TagManagerView::initUi() {
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(5);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setStyleSheet(QString(
        "QSplitter { background: transparent; border: none; }"
        "QSplitter::handle { background-color: %1; width: 5px; }"
        "QSplitter::handle:hover { background-color: %2; }" 
    ).arg(qssColor(BackgroundDeep)).arg(qssColor(BackgroundHover)));

    setupSidebar();
    setupContentArea();

    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_contentContainer);
    
    // 侧边栏固定 230px
    m_splitter->setSizes({230, 1000});
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(m_splitter);
}

QWidget* TagManagerView::createSidebarItem(const QString& icon, const QString& name, const QString& countText, QLabel** outCountLabel) {
    QWidget* item = new QWidget();
    item->setFixedHeight(26);
    item->setCursor(Qt::PointingHandCursor);
    
    auto* layout = new QHBoxLayout(item);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(0);
    
    QWidget* inner = new QWidget(item);
    inner->setObjectName("SidebarItemInner");
    inner->setStyleSheet(
        "QWidget#SidebarItemInner { background: transparent; border-radius: 4px; }"
        "QWidget#SidebarItemInner:hover { background-color: #2a2d2e; }"
    );
    auto* innerLayout = new QHBoxLayout(inner);
    innerLayout->setContentsMargins(0, 0, 15, 0);
    innerLayout->setSpacing(6);
    
    QLabel* iconLabel = new QLabel(inner);
    iconLabel->setPixmap(UiHelper::getIcon(icon, TextDim, 16).pixmap(16, 16));
    innerLayout->addWidget(iconLabel);
    
    QLabel* nameLabel = new QLabel(name, inner);
    nameLabel->setStyleSheet("color: #CCC; font-size: 12px; background: transparent;");
    innerLayout->addWidget(nameLabel);
    innerLayout->addStretch();
    
    QLabel* countLabel = new QLabel(countText, inner);
    countLabel->setStyleSheet("color: #666; font-size: 11px; background: transparent;");
    innerLayout->addWidget(countLabel);
    if (outCountLabel) *outCountLabel = countLabel;
    
    layout->addWidget(inner);
    return item;
}

void TagManagerView::setupSidebar() {
    m_sidebar = new QFrame(this);
    m_sidebar->setObjectName("ListContainer"); // 物理对标：复用导航栏样式 ID
    m_sidebar->setFixedWidth(230);
    m_sidebar->setAttribute(Qt::WA_StyledBackground, true);
    
    m_sidebarLayout = new QVBoxLayout(m_sidebar);
    m_sidebarLayout->setContentsMargins(0, 0, 0, 0);
    m_sidebarLayout->setSpacing(0);

    // 标题栏
    QWidget* header = new QWidget(m_sidebar);
    header->setObjectName("ContainerHeader");
    header->setFixedHeight(32);
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setStyleSheet(
        "QWidget#ContainerHeader {"
        "  background-color: #252526;"
        "  border-bottom: 1px solid #333;"
        "}"
    );
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(15, 0, 5, 0);
    
    QLabel* iconLabel = new QLabel(header);
    iconLabel->setPixmap(UiHelper::getIcon("tag_filled", QColor("#1abc9c"), 18).pixmap(18, 18));
    headerLayout->addWidget(iconLabel);

    QLabel* titleLabel = new QLabel("标签管理", header);
    titleLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #1abc9c;");
    headerLayout->addWidget(titleLabel);
    
    headerLayout->addStretch();

    // 2026-04-xx 按照用户要求：在标题栏新增“新建组”按钮
    QPushButton* btnAddGroup = new QPushButton(header);
    btnAddGroup->setFixedSize(24, 24);
    btnAddGroup->setCursor(Qt::PointingHandCursor);
    btnAddGroup->setIcon(UiHelper::getIcon("add", TextMain, 16));
    btnAddGroup->setToolTip("新建标签组");
    btnAddGroup->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 4px; }"
        "QPushButton:hover { background-color: #3E3E42; }"
        "QPushButton:pressed { background-color: #4E4E52; }"
    );
    connect(btnAddGroup, &QPushButton::clicked, this, &TagManagerView::createNewGroup);
    headerLayout->addWidget(btnAddGroup);
    
    m_sidebarLayout->addWidget(header);

    // 2026-07-xx 按照用户要求：为列表内容包裹容器，恢复旧版 (15, 8, 0, 8) 的呼吸边距
    QWidget* contentWrapper = new QWidget(m_sidebar);
    contentWrapper->setStyleSheet("background: transparent; border: none;");
    QVBoxLayout* sidebarContentLayout = new QVBoxLayout(contentWrapper);
    sidebarContentLayout->setContentsMargins(15, 8, 0, 8);
    sidebarContentLayout->setSpacing(0);

    // 静态项
    QWidget* allItem = createSidebarItem("all_data", "全部标签", "0", &m_allTagsCountLabel);
    allItem->setProperty("sidebarAction", "all");
    allItem->installEventFilter(this);
    sidebarContentLayout->addWidget(allItem);

    QWidget* uncatItem = createSidebarItem("uncategorized", "未分类", "0", &m_uncategorizedTagsCountLabel);
    uncatItem->setProperty("sidebarAction", "uncategorized");
    uncatItem->installEventFilter(this);
    sidebarContentLayout->addWidget(uncatItem);

    QWidget* freqItem = createSidebarItem("star_filled", "常用标签", "0", &m_frequentTagsCountLabel);
    freqItem->setProperty("sidebarAction", "frequent");
    freqItem->installEventFilter(this);
    sidebarContentLayout->addWidget(freqItem);

    // 标签组容器
    m_groupContainer = new QWidget(contentWrapper);
    auto* groupLayout = new QVBoxLayout(m_groupContainer);
    groupLayout->setContentsMargins(0, 0, 0, 0);
    groupLayout->setSpacing(0);
    sidebarContentLayout->addWidget(m_groupContainer);

    sidebarContentLayout->addStretch();
    
    m_sidebarLayout->addWidget(contentWrapper, 1);
}

void TagManagerView::setupContentArea() {
    m_contentContainer = new QFrame(this);
    m_contentContainer->setObjectName("EditorContainer"); // 物理对标：复用内容面板 ID
    m_contentContainer->setAttribute(Qt::WA_StyledBackground, true);
    auto* mainL = new QVBoxLayout(m_contentContainer);
    mainL->setContentsMargins(0, 0, 0, 0);
    mainL->setSpacing(0);

    // 1. 标题栏 (物理对齐 ContentPanel)
    QWidget* titleBar = new QWidget(m_contentContainer);
    titleBar->setObjectName("ContainerHeader");
    titleBar->setFixedHeight(32);
    titleBar->setAttribute(Qt::WA_StyledBackground, true);
    titleBar->setStyleSheet(
        "QWidget#ContainerHeader {"
        "  background-color: #252526;"
        "  border-bottom: 1px solid #333;"
        "}"
    );
    QHBoxLayout* titleL = new QHBoxLayout(titleBar);
    titleL->setContentsMargins(15, 0, 5, 0);
    titleL->setSpacing(5);

    QLabel* iconLabel = new QLabel(titleBar);
    iconLabel->setPixmap(UiHelper::getIcon("tag", QColor("#1abc9c"), 18).pixmap(18, 18));
    titleL->addWidget(iconLabel);

    m_contentTitleLabel = new QLabel("标签", titleBar);
    m_contentTitleLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #1abc9c; background: transparent; border: none;");
    titleL->addWidget(m_contentTitleLabel);
    titleL->addStretch();

    mainL->addWidget(titleBar);

    // 1.5 常用标签区 (Plan-82)
    QWidget* popularTagsBox = new QWidget(m_contentContainer);
    popularTagsBox->setObjectName("PopularTagsBox");
    popularTagsBox->setFixedHeight(110); // 固定高度，防止晃动
    popularTagsBox->setStyleSheet(
        "QWidget#PopularTagsBox { background-color: #1e1e1e; border-bottom: 1px solid #333; }"
    );
    QVBoxLayout* popularL = new QVBoxLayout(popularTagsBox);
    popularL->setContentsMargins(20, 10, 20, 10);
    popularL->setSpacing(8);

    QHBoxLayout* popTitleL = new QHBoxLayout();
    QLabel* popIcon = new QLabel(popularTagsBox);
    popIcon->setPixmap(UiHelper::getIcon("sparkles", QColor("#f1c40f"), 16).pixmap(16, 16));
    popTitleL->addWidget(popIcon);
    QLabel* popTitle = new QLabel("常用标签", popularTagsBox);
    popTitle->setStyleSheet("font-size: 12px; font-weight: bold; color: #f1c40f;");
    popTitleL->addWidget(popTitle);
    popTitleL->addStretch();
    popularL->addLayout(popTitleL);

    QWidget* popFlowContainer = new QWidget(popularTagsBox);
    popFlowContainer->setObjectName("PopularTagsFlowContainer");
    new FlowLayout(popFlowContainer, 0, 10, 8);
    popularL->addWidget(popFlowContainer, 1);

    mainL->addWidget(popularTagsBox);

    // 2. 滚动区
    m_scrollArea = new QScrollArea(m_contentContainer);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");
    
    m_contentWidget = new QWidget();
    m_contentWidget->setObjectName("TagContentContainer");
    m_contentWidget->setStyleSheet("QWidget#TagContentContainer { background-color: transparent; }");
    
    auto* contentLayout = new QVBoxLayout(m_contentWidget);
    contentLayout->setContentsMargins(20, 20, 20, 20);
    contentLayout->setSpacing(20);
    
    m_scrollArea->setWidget(m_contentWidget);
    mainL->addWidget(m_scrollArea, 1);
}

void TagManagerView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    adjustFlowHeights();
}

bool TagManagerView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        int gid = watched->property("groupId").toInt();
        if (gid > 0) {
            // 筛选属于该组的标签
            QStringList groupTags;
            for (const auto& group : m_tagGroups) {
                if (group.id == gid) {
                    groupTags = group.tags;
                    break;
                }
            }
            
            if (!m_contentWidget || !m_contentWidget->layout()) return false;
            QVBoxLayout* contentLayout = qobject_cast<QVBoxLayout*>(m_contentWidget->layout());
            for (int i = 0; i < contentLayout->count(); ++i) {
                QWidget* groupWidget = contentLayout->itemAt(i)->widget();
                if (!groupWidget) continue;
                bool groupHasVisibleTag = false;
                const auto buttons = groupWidget->findChildren<QPushButton*>();
                for (QPushButton* btn : buttons) {
                    // 解析按钮文本中的标签名，例如 "测试 (4)" -> "测试"
                    QString btnText = btn->text();
                    int lastParen = btnText.lastIndexOf(" (");
                    QString tagName = (lastParen != -1) ? btnText.left(lastParen) : btnText;
                    
                    bool visible = groupTags.contains(tagName);
                    btn->setVisible(visible);
                    if (visible) groupHasVisibleTag = true;
                }
                groupWidget->setVisible(groupHasVisibleTag);
            }
            QTimer::singleShot(0, this, &TagManagerView::adjustFlowHeights);
            return true;
        } else {
            QString action = watched->property("sidebarAction").toString();
            if (action == "all") {
                search("");
                return true;
            } else if (action == "uncategorized") {
                // 筛选未归组标签
                QSet<QString> groupedTags;
                for (const auto& group : m_tagGroups) {
                    for (const auto& tag : group.tags) groupedTags.insert(tag);
                }
                
                QVBoxLayout* contentLayout = qobject_cast<QVBoxLayout*>(m_contentWidget->layout());
                for (int i = 0; i < contentLayout->count(); ++i) {
                    QWidget* groupWidget = contentLayout->itemAt(i)->widget();
                    if (!groupWidget) continue;
                    bool groupHasVisibleTag = false;
                    const auto buttons = groupWidget->findChildren<QPushButton*>();
                    for (QPushButton* btn : buttons) {
                        QString btnText = btn->text();
                        int lastParen = btnText.lastIndexOf(" (");
                        QString tagName = (lastParen != -1) ? btnText.left(lastParen) : btnText;
                        bool visible = !groupedTags.contains(tagName);
                        btn->setVisible(visible);
                        if (visible) groupHasVisibleTag = true;
                    }
                    groupWidget->setVisible(groupHasVisibleTag);
                }
                QTimer::singleShot(0, this, &TagManagerView::adjustFlowHeights);
                return true;
            } else if (action == "frequent") {
                // TODO: 常用标签逻辑（目前暂无权重统计，显示为空）
                search("___NON_EXISTENT_TAG___");
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TagManagerView::addTagToGroup(const QString& tagName, int groupId) {
    QPointer<TagManagerView> weakThis(this);
    (void)QtConcurrent::run([weakThis, tagName, groupId]() {
        sqlite3* db = DatabaseManager::instance().getMemoryDb(L"C");
        if (!db) return;
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO tag_group_items (group_id, tag_name) SELECT ?, ? WHERE NOT EXISTS (SELECT 1 FROM tag_group_items WHERE group_id = ? AND tag_name = ?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, groupId);
            QByteArray tagUtf8 = tagName.toUtf8();
            sqlite3_bind_text(stmt, 2, tagUtf8.constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, groupId);
            sqlite3_bind_text(stmt, 4, tagUtf8.constData(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (weakThis) QMetaObject::invokeMethod(weakThis.data(), "refresh", Qt::QueuedConnection);
        }
    });
}

void TagManagerView::removeTagFromGroup(const QString& tagName, int groupId) {
    QPointer<TagManagerView> weakThis(this);
    (void)QtConcurrent::run([weakThis, tagName, groupId]() {
        sqlite3* db = DatabaseManager::instance().getMemoryDb(L"C");
        if (!db) return;
        sqlite3_stmt* stmt;
        QString sql;
        if (groupId == -1) {
            sql = "DELETE FROM tag_group_items WHERE tag_name = ?";
        } else {
            sql = "DELETE FROM tag_group_items WHERE group_id = ? AND tag_name = ?";
        }
        if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
            if (groupId == -1) {
                sqlite3_bind_text(stmt, 1, tagName.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_int(stmt, 1, groupId);
                sqlite3_bind_text(stmt, 2, tagName.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            }
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (weakThis) QMetaObject::invokeMethod(weakThis.data(), "refresh", Qt::QueuedConnection);
        }
    });
}

void TagManagerView::renameGroup(int groupId, const QString& newName) {
    QPointer<TagManagerView> weakThis(this);
    (void)QtConcurrent::run([weakThis, groupId, newName]() {
        sqlite3* db = DatabaseManager::instance().getMemoryDb(L"C");
        if (!db) return;
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "UPDATE tag_groups SET name = ? WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, newName.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, groupId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (weakThis) QMetaObject::invokeMethod(weakThis.data(), "refresh", Qt::QueuedConnection);
        }
    });
}

void TagManagerView::deleteGroup(int groupId) {
    QPointer<TagManagerView> weakThis(this);
    (void)QtConcurrent::run([weakThis, groupId]() {
        sqlite3* db = DatabaseManager::instance().getMemoryDb(L"C");
        if (!db) return;
        sqlite3_stmt* stmt;
        sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
        if (sqlite3_prepare_v2(db, "DELETE FROM tag_groups WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, groupId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        if (sqlite3_prepare_v2(db, "DELETE FROM tag_group_items WHERE group_id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, groupId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        if (weakThis) QMetaObject::invokeMethod(weakThis.data(), "refresh", Qt::QueuedConnection);
    });
}

void TagManagerView::createNewGroup() {
    FramelessInputDialog dlg("新建标签组", "标签组名称:", "", this);
    if (dlg.exec() == QDialog::Accepted) {
        QString name = dlg.text();
        if (name.isEmpty()) return;

        sqlite3* db = DatabaseManager::instance().getMemoryDb(L"C");
        if (db) {
            sqlite3_stmt* stmt;
            const char* sql = "INSERT INTO tag_groups (name, color, sort_order) VALUES (?, ?, (SELECT IFNULL(MAX(sort_order), 0) + 1 FROM tag_groups))";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                QByteArray utf8Name = name.trimmed().toUtf8();
                sqlite3_bind_text(stmt, 1, utf8Name.constData(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, "#3498db", -1, SQLITE_TRANSIENT); // 默认蓝色
                if (sqlite3_step(stmt) == SQLITE_DONE) {
                    refresh();
                } else {
                    FramelessMessageBox::warning(this, "错误", "创建标签组失败");
                }
                sqlite3_finalize(stmt);
            }
        }
    }
}

void TagManagerView::adjustFlowHeights() {
    if (!m_contentWidget || !m_contentWidget->layout()) return;
    
    int contentWidth = m_contentWidget->width();
    int sideMargin = 20 * 2; // contentLayout->setContentsMargins(20, 20, 20, 20)
    int availableWidth = contentWidth - sideMargin;

    // 调整常用标签高度 (Plan-82)
    QWidget* popFlow = findChild<QWidget*>("PopularTagsFlowContainer");
    if (popFlow && popFlow->layout()) {
        int h = static_cast<FlowLayout*>(popFlow->layout())->heightForWidth(availableWidth);
        popFlow->parentWidget()->setFixedHeight(std::max(60, 16 + 10 + h + 20));
    }

    QVBoxLayout* contentLayout = qobject_cast<QVBoxLayout*>(m_contentWidget->layout());
    if (!contentLayout) return;

    for (int i = 0; i < contentLayout->count(); ++i) {
        QWidget* groupWidget = contentLayout->itemAt(i)->widget();
        if (!groupWidget || !groupWidget->isVisible() || groupWidget->objectName() == "Spacer") continue;
        
        // 查找其中的 tagsContainer
        QWidget* tagsContainer = groupWidget->findChild<QWidget*>("TagsFlowContainer");
        
        if (tagsContainer && tagsContainer->layout()) {
            FlowLayout* flow = static_cast<FlowLayout*>(tagsContainer->layout());
            int h = flow->heightForWidth(availableWidth);
            // 2026-04-xx 按照用户指令实施标准化高度计算：16(标题) + 20(Spacing) + h(内容) + 20(Margin)
            groupWidget->setFixedHeight(16 + 20 + h + 20);
        }
    }
}

void TagManagerView::search(const QString& keyword) {
    if (!m_contentWidget || !m_contentWidget->layout()) return;
    
    QString kw = keyword.trimmed().toLower();
    QVBoxLayout* contentLayout = qobject_cast<QVBoxLayout*>(m_contentWidget->layout());
    if (!contentLayout) return;

    for (int i = 0; i < contentLayout->count(); ++i) {
        QWidget* groupWidget = contentLayout->itemAt(i)->widget();
        if (!groupWidget || groupWidget->objectName() == "Spacer") continue;
        
        bool groupHasVisibleTag = false;
        const auto buttons = groupWidget->findChildren<QPushButton*>();
        for (QPushButton* btn : buttons) {
            bool visible = kw.isEmpty() || btn->text().toLower().contains(kw);
            btn->setVisible(visible);
            if (visible) groupHasVisibleTag = true;
        }
        
        groupWidget->setVisible(groupHasVisibleTag);
    }
    
    // 重新调整高度
    QTimer::singleShot(0, this, &TagManagerView::adjustFlowHeights);
}

void TagManagerView::refresh() {
    m_tagCounts = MetadataManager::instance().getAllTags();

    // 渲染常用标签 (Plan-82)
    QWidget* popFlow = findChild<QWidget*>("PopularTagsFlowContainer");
    if (popFlow && popFlow->layout()) {
        QLayoutItem* child;
        while ((child = popFlow->layout()->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }

        auto topTags = MetadataManager::instance().getTopTags(20);
        if (topTags.isEmpty()) {
            popFlow->parentWidget()->hide();
        } else {
            popFlow->parentWidget()->show();
            for (const auto& pair : topTags) {
                QPushButton* btn = new QPushButton(QString("%1 (%2)").arg(pair.first).arg(pair.second), popFlow);
                btn->setCursor(Qt::PointingHandCursor);
                btn->setStyleSheet(
                    "QPushButton { background: #2d2d30; border: 1px solid #3e3e42; color: #EEE; border-radius: 4px; padding: 4px 10px; font-size: 12px; }"
                    "QPushButton:hover { background: #3e3e42; border-color: #f1c40f; color: #f1c40f; }"
                );
                QString tagName = pair.first;
                connect(btn, &QPushButton::clicked, this, [this, tagName]() { emit requestSearchTag(tagName); });
                popFlow->layout()->addWidget(btn);
            }
        }
    }
    
    // 加载标签组
    m_tagGroups.clear();
    sqlite3* db = DatabaseManager::instance().getMemoryDb(L"C");
    if (db) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT id, name, color FROM tag_groups ORDER BY sort_order ASC", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                TagGroup tg;
                tg.id = sqlite3_column_int(stmt, 0);
                tg.name = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 1));
                tg.color = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 2));
                
                sqlite3_stmt* itemStmt;
                if (sqlite3_prepare_v2(db, "SELECT tag_name FROM tag_group_items WHERE group_id = ?", -1, &itemStmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(itemStmt, 1, tg.id);
                    while (sqlite3_step(itemStmt) == SQLITE_ROW) {
                        tg.tags << QString::fromUtf8((const char*)sqlite3_column_text(itemStmt, 0));
                    }
                    sqlite3_finalize(itemStmt);
                }
                m_tagGroups.append(tg);
            }
            sqlite3_finalize(stmt);
        }
    }

    // 更新侧边栏
    int allCount = m_tagCounts.size();
    if (m_allTagsCountLabel) m_allTagsCountLabel->setText(QString::number(allCount));
    
    // 统计未分类 (此处逻辑：不在任何 TagGroup 中的标签)
    QSet<QString> groupedTags;
    for (const auto& group : m_tagGroups) {
        for (const auto& tag : group.tags) groupedTags.insert(tag);
    }
    int uncategorizedCount = 0;
    for (auto it = m_tagCounts.begin(); it != m_tagCounts.end(); ++it) {
        if (!groupedTags.contains(it.key())) uncategorizedCount++;
    }
    if (m_uncategorizedTagsCountLabel) m_uncategorizedTagsCountLabel->setText(QString::number(uncategorizedCount));
    
    QVBoxLayout* groupLayout = qobject_cast<QVBoxLayout*>(m_groupContainer->layout());
    QLayoutItem* child;
    if (groupLayout) {
        while ((child = groupLayout->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }
        for (const auto& group : m_tagGroups) {
            auto* item = createSidebarItem("folder_filled", group.name, QString::number(group.tags.size()));
            int gid = group.id;
            QString gname = group.name;
            
            // 点击筛选
            item->installEventFilter(this);
            item->setProperty("groupId", gid);

            // 右键菜单
            item->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(item, &QWidget::customContextMenuRequested, this, [this, gid, gname](const QPoint& /*pos*/) {
                QMenu menu(this);
                UiHelper::applyMenuStyle(&menu);
                menu.addAction(UiHelper::getIcon("edit", TextMain), "重命名组", [this, gid, gname]() {
                    FramelessInputDialog dlg("重命名组", "组名称:", gname, this);
                    if (dlg.exec() == QDialog::Accepted) {
                        QString newName = dlg.text();
                        if (!newName.isEmpty()) renameGroup(gid, newName);
                    }
                });
                menu.addAction(UiHelper::getIcon("trash", ErrorRed), "删除组", [this, gid]() {
                    if (FramelessMessageBox::question(this, "删除", "确定要删除该标签组吗？（不会删除标签本身）")) {
                        deleteGroup(gid);
                    }
                });
                menu.exec(QCursor::pos());
            });

            groupLayout->addWidget(item);
        }
    }

    // 重建内容区
    QVBoxLayout* contentLayout = qobject_cast<QVBoxLayout*>(m_contentWidget->layout());
    if (contentLayout) {
        while ((child = contentLayout->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }
        // 物理对标：移除旧式内联标题，改用 top titleBar
    }

    // 按字母分组
    QMap<QChar, QMap<QString, int>> groups;
    for (auto it = m_tagCounts.begin(); it != m_tagCounts.end(); ++it) {
        QString tag = it.key();
        if (tag.isEmpty()) continue;
        QChar first = tag.at(0).toUpper();
        if (first < 'A' || first > 'Z') first = '#';
        groups[first][tag] = it.value();
    }

    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QWidget* groupWidget = new QWidget(m_contentWidget);
        groupWidget->setObjectName("TagGroupWidget");
        groupWidget->setAttribute(Qt::WA_StyledBackground, true);
        // 使用 ID 选择器锁定边框，防止子 QWidget (如标签按钮) 误继承下划线
        groupWidget->setStyleSheet("QWidget#TagGroupWidget { border-bottom: 1px solid #333; }");
        
        auto* vLayout = new QVBoxLayout(groupWidget);
        vLayout->setContentsMargins(0, 0, 0, 20);
        vLayout->setSpacing(20);
        
        QLabel* groupTitle = new QLabel(QString(it.key()), groupWidget);
        groupTitle->setFixedHeight(16);
        // 彻底移除标题底线，确保其下方为纯净留白
        groupTitle->setStyleSheet("font-size: 16px; font-weight: bold; color: #1abc9c; border: none; padding: 0; margin: 0;");
        vLayout->addWidget(groupTitle);
        
        QWidget* tagsContainer = new QWidget(groupWidget);
        tagsContainer->setObjectName("TagsFlowContainer");
        // margin=0, hSpacing=10, vSpacing=0
        auto* flow = new FlowLayout(tagsContainer, 0, 10, 0);
        
        auto tagsInGroup = it.value();
        for (auto tagIt = tagsInGroup.begin(); tagIt != tagsInGroup.end(); ++tagIt) {
            QPushButton* tagBtn = new QPushButton(QString("%1 (%2)").arg(tagIt.key()).arg(tagIt.value()), tagsContainer);
            tagBtn->setCursor(Qt::PointingHandCursor);
            tagBtn->setStyleSheet(
                "QPushButton { background: transparent; border: none; color: #AAA; text-align: left; font-size: 13px; padding: 2px 5px; }"
                "QPushButton:hover { color: #3498db; text-decoration: underline; }"
            );
            
            QString tagName = tagIt.key();
            connect(tagBtn, &QPushButton::clicked, this, [this, tagName]() {
                emit requestSearchTag(tagName);
            });

            // 右键菜单
            tagBtn->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(tagBtn, &QWidget::customContextMenuRequested, this, [this, tagName](const QPoint&) {
                QMenu menu(this);
                UiHelper::applyMenuStyle(&menu);
                menu.addAction(UiHelper::getIcon("search", TextMain), "搜索含此标签的项目", this, [this, tagName](){ emit requestSearchTag(tagName); });
                menu.addSeparator();
                menu.addAction(UiHelper::getIcon("star_filled", WarningOrange), "设为常用标签");
                
                auto* addToMenu = menu.addMenu(UiHelper::getIcon("add", SuccessGreen), "加入标签组");
                for (const auto& group : m_tagGroups) {
                    int gid = group.id;
                    addToMenu->addAction(group.name, [this, tagName, gid]() {
                        addTagToGroup(tagName, gid);
                    });
                }
                
                auto* moveToMenu = menu.addMenu(UiHelper::getIcon("folder_filled", PrimaryBlue), "移动至标签组");
                for (const auto& group : m_tagGroups) {
                    int gid = group.id;
                    moveToMenu->addAction(group.name, [this, tagName, gid]() {
                        removeTagFromGroup(tagName, -1);
                        addTagToGroup(tagName, gid);
                    });
                }

                menu.addAction(UiHelper::getIcon("close", ErrorRed), "从标签组中移除", [this, tagName]() {
                    removeTagFromGroup(tagName);
                });
                menu.addSeparator();
                
                menu.addAction(UiHelper::getIcon("edit", TextMain), "重命名标签", [this, tagName]() {
                    FramelessInputDialog dlg("重命名标签", "新标签名称:", tagName, this);
                    if (dlg.exec() == QDialog::Accepted) {
                        QString newName = dlg.text();
                        if (!newName.isEmpty() && newName != tagName) {
                            MetadataManager::instance().renameTag(tagName, newName);
                            refresh();
                        }
                    }
                });

                menu.addAction(UiHelper::getIcon("trash", ErrorRed), "删除标签", [this, tagName]() {
                    if (FramelessMessageBox::question(this, "删除标签", QString("确定要全局删除标签 \"%1\" 吗？此操作不可撤销。").arg(tagName))) {
                        MetadataManager::instance().removeTag(tagName);
                        refresh();
                    }
                });

                menu.exec(QCursor::pos());
            });
            
            flow->addWidget(tagBtn);
        }
        vLayout->addWidget(tagsContainer);
        if (contentLayout) contentLayout->addWidget(groupWidget);
    }
    if (contentLayout) {
        contentLayout->addStretch();
    }
    
    // 数据刷新后立即同步一次高度
    QTimer::singleShot(0, this, &TagManagerView::adjustFlowHeights);
}

} // namespace ArcMeta
