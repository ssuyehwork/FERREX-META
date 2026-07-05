#pragma once

#include "FramelessDialog.h"
#include <QTableWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <vector>
#include <string>

namespace ArcMeta {

/**
 * @brief 批量重命名独立预览对话框 (选项 A)
 */
class BatchRenamePreviewDialog : public FramelessDialog {
    Q_OBJECT
public:
    explicit BatchRenamePreviewDialog(QWidget* parent = nullptr);
    ~BatchRenamePreviewDialog() override = default;

    /**
     * @brief 设置预览数据并刷新表格
     * @param originalPaths 原始文件路径
     * @param previewNames 计算后的新名称
     */
    void setPreviewData(const std::vector<std::wstring>& originalPaths, const std::vector<std::wstring>& previewNames);

private:
    void initContent();
    void applyTheme();

    QTableWidget* m_table = nullptr;
    QPushButton* m_btnClose = nullptr;
};

} // namespace ArcMeta
