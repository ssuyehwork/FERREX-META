#pragma once

#include <QWidget>
#include <QTreeView>
#include <QVBoxLayout>
#include <QPushButton>
#include <QHBoxLayout>
#include "InvalidDataModel.h"
#include "../meta/MetadataManager.h"
#include "../meta/CategoryRepo.h"
#include "UiHelper.h"
#include "FramelessDialog.h"
#include "ToolTipOverlay.h"
#include <QHeaderView>
#include <QFileInfo>
#include <QCursor>

namespace ArcMeta {

class InvalidDataListView : public QWidget {
    Q_OBJECT
public:
    explicit InvalidDataListView(QWidget* parent = nullptr) : QWidget(parent) {
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // 1. 工具栏
        QWidget* toolbar = new QWidget(this);
        toolbar->setFixedHeight(32);
        toolbar->setStyleSheet("background-color: #252526; border-bottom: 1px solid #333;");
        QHBoxLayout* toolL = new QHBoxLayout(toolbar);
        toolL->setContentsMargins(10, 0, 10, 0);
        toolL->setSpacing(8);

        auto createBtn = [&](const QString& text, const QString& icon, const QColor& color) {
            QPushButton* btn = new QPushButton(text, toolbar);
            btn->setIcon(UiHelper::getIcon(icon, color, 16));
            btn->setStyleSheet("QPushButton { background: transparent; border: 1px solid #444; border-radius: 4px; padding: 2px 10px; color: #EEE; }"
                               "QPushButton:hover { background: #3E3E42; }"
                               "QPushButton:pressed { background: #4E4E52; }");
            return btn;
        };

        m_btnCheckAll = createBtn("全选", "check_circle", QColor("#3498db"));
        m_btnUncheckAll = createBtn("取消", "no_color", QColor("#95a5a6"));
        m_btnDelete = createBtn("永久删除选中的记录", "trash", QColor("#e74c3c"));

        toolL->addWidget(m_btnCheckAll);
        toolL->addWidget(m_btnUncheckAll);
        toolL->addStretch();
        toolL->addWidget(m_btnDelete);
        layout->addWidget(toolbar);

        // 2. 列表视图
        m_view = new QTreeView(this);
        m_model = new InvalidDataModel(this);
        m_view->setModel(m_model);
        m_view->setRootIsDecorated(false);
        m_view->setAlternatingRowColors(true);
        m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_view->setStyleSheet("QTreeView { background-color: #1E1E1E; alternate-background-color: #252526; border: none; color: #EEE; }"
                             "QTreeView::item { height: 32px; }"
                             "QTreeView::item:selected { background-color: #378ADD; }");
        
        auto* header = m_view->header();
        header->setStretchLastSection(true);
        header->setSectionResizeMode(InvalidDataModel::Check, QHeaderView::Fixed);
        header->resizeSection(InvalidDataModel::Check, 30);
        header->resizeSection(InvalidDataModel::FileName, 200);
        header->resizeSection(InvalidDataModel::InvalidTime, 150);
        
        layout->addWidget(m_view);

        // 绑定逻辑
        connect(m_btnCheckAll, &QPushButton::clicked, [this]() { m_model->setAllChecked(true); });
        connect(m_btnUncheckAll, &QPushButton::clicked, [this]() { m_model->setAllChecked(false); });
        connect(m_btnDelete, &QPushButton::clicked, this, &InvalidDataListView::performBatchDelete);
    }

    void refresh() {
        QStringList paths = CategoryRepo::getSystemCategoryPaths("invalid_data");
        std::vector<InvalidItem> records;
        for (const QString& p : paths) {
            std::wstring wp = p.toStdWString();
            RuntimeMeta meta = MetadataManager::instance().getMeta(wp);
            InvalidItem item;
            item.path = p;
            item.originalPath = QString::fromStdWString(meta.originalPath.empty() ? wp : meta.originalPath);
            item.fileName = QFileInfo(item.originalPath).fileName();
            item.invalidAt = meta.mtime; // 暂时用 mtime 代表检测到失效的时间
            item.size = meta.fileSize;
            item.type = QFileInfo(item.originalPath).suffix().toUpper();
            records.push_back(item);
        }
        m_model->setRecords(records);
    }

private slots:
    void performBatchDelete() {
        QStringList checked = m_model->getCheckedPaths();
        if (checked.isEmpty()) {
            ToolTipOverlay::instance()->showText(QCursor::pos(), "请先勾选需要删除的记录", 2000, QColor("#e74c3c"));
            return;
        }

        QString msg = QString("确定要永久删除选中的 %1 条失效记录吗？\n该操作将从数据库彻底抹除元数据，且不可撤销。").arg(checked.size());
        if (FramelessMessageBox::question(this, "确认批量删除", msg)) {
            MetadataManager::instance().removeMetadataBatchSync(checked);
            refresh();
            // 通知侧边栏刷新计数
            MetadataManager::instance().notifyCategoryCountChanged();
            ToolTipOverlay::instance()->showText(QCursor::pos(), "批量删除成功", 1500, QColor("#2ecc71"));
        }
    }

private:
    QTreeView* m_view;
    InvalidDataModel* m_model;
    QPushButton* m_btnCheckAll;
    QPushButton* m_btnUncheckAll;
    QPushButton* m_btnDelete;
};

} // namespace ArcMeta
