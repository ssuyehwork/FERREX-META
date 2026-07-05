#pragma once

#include "FramelessDialog.h"
#include <QRadioButton>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QGroupBox>
#include <QList>
#include <vector>
#include <string>
#include "../meta/BatchRenameEngine.h"

namespace ArcMeta {

class RuleRow;

/**
 * @brief 批量重命名高级对话框 (Adobe Bridge 风格)
 */
class BatchRenameDialog : public FramelessDialog {
    Q_OBJECT
public:
    explicit BatchRenameDialog(const std::vector<std::wstring>& originalPaths, QWidget* parent = nullptr);
    ~BatchRenameDialog() override = default;

private slots:
    void onAddRow();
    void updatePreview();
    void onExecute();
    void onPreview();
    void onBrowseTarget();

private:
    void initContent();
    void applyTheme();

    std::vector<std::wstring> m_originalPaths;
    
    // 预设相关
    QComboBox* m_presetCombo = nullptr;
    QPushButton* m_btnSavePreset = nullptr;
    QPushButton* m_btnDeletePreset = nullptr;

    // 目标操作
    QRadioButton* m_rbRename = nullptr;
    QRadioButton* m_rbMove = nullptr;
    QRadioButton* m_rbCopy = nullptr;
    QLineEdit* m_targetPathEdit = nullptr;
    QPushButton* m_btnBrowse = nullptr;
    
    // 命名规则
    QWidget* m_rulesContainer = nullptr;
    QVBoxLayout* m_rulesLayout = nullptr;
    QList<RuleRow*> m_ruleRows;
    
    // 动作按钮 (右侧栏)
    QPushButton* m_btnExecute = nullptr;
    QPushButton* m_btnCancel = nullptr;
    QPushButton* m_btnPreview = nullptr;
};

} // namespace ArcMeta
