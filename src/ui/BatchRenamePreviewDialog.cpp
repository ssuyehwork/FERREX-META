#include "BatchRenamePreviewDialog.h"
#include <QHeaderView>
#include <QFileInfo>
#include <QTableWidgetItem>

namespace ArcMeta {

BatchRenamePreviewDialog::BatchRenamePreviewDialog(QWidget* parent)
    : FramelessDialog("批量重命名预览 - 高清对比模式", parent) {
    resize(800, 500);
    initContent();
    applyTheme();
}

void BatchRenamePreviewDialog::initContent() {
    QVBoxLayout* mainL = new QVBoxLayout(m_contentArea);
    mainL->setContentsMargins(15, 15, 15, 15);
    mainL->setSpacing(15);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({"当前文件名", "重命名后名称"});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    
    mainL->addWidget(m_table);

    m_btnClose = new QPushButton("关闭预览", this);
    m_btnClose->setFixedWidth(120);
    m_btnClose->setFixedHeight(32);
    m_btnClose->setStyleSheet("QPushButton { background: #333; color: #EEE; border-radius: 4px; } QPushButton:hover { background: #444; }");
    
    QHBoxLayout* btnL = new QHBoxLayout();
    btnL->addStretch();
    btnL->addWidget(m_btnClose);
    mainL->addLayout(btnL);

    connect(m_btnClose, &QPushButton::clicked, this, &QDialog::accept);
}

void BatchRenamePreviewDialog::setPreviewData(const std::vector<std::wstring>& originalPaths, const std::vector<std::wstring>& previewNames) {
    m_table->setRowCount(static_cast<int>(originalPaths.size()));
    
    for (int i = 0; i < static_cast<int>(originalPaths.size()); ++i) {
        QFileInfo info(QString::fromStdWString(originalPaths[static_cast<size_t>(i)]));
        
        // 当前文件名
        auto* itemOld = new QTableWidgetItem(info.fileName());
        itemOld->setForeground(QColor("#B0B0B0"));
        m_table->setItem(i, 0, itemOld);

        // 新文件名
        QString newName = QString::fromStdWString(previewNames[static_cast<size_t>(i)]);
        auto* itemNew = new QTableWidgetItem(newName);
        itemNew->setForeground(QColor("#2ecc71")); // 绿色代表预期结果
        m_table->setItem(i, 1, itemNew);
    }
}

void BatchRenamePreviewDialog::applyTheme() {
    setStyleSheet(
        "QDialog { background-color: #1E1E1E; color: #EEEEEE; }"
        "QTableWidget { background-color: #252526; border: 1px solid #333; gridline-color: transparent; selection-background-color: rgba(55, 138, 221, 0.2); }"
        "QHeaderView::section { background-color: #2D2D2D; color: #888; border: none; height: 32px; font-weight: bold; font-size: 11px; }"
    );
}

} // namespace ArcMeta
