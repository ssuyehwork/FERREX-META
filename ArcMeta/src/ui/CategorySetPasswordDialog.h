#pragma once

#include "FramelessDialog.h"
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>

namespace ArcMeta {

/**
 * @brief 1:1 物理还原旧版“设置密码”对话框 (根据截图 PixPin_2026-04-03_11-39-19.png)
 */
class CategorySetPasswordDialog : public FramelessDialog {
    Q_OBJECT
public:
    explicit CategorySetPasswordDialog(QWidget* parent = nullptr);

    QString password() const { return m_pwdEdit->text(); }
    QString confirmPassword() const { return m_confirmEdit->text(); }
    QString hint() const { return m_hintEdit->text(); }

private slots:
    void onSave();

private:
    QLineEdit* m_pwdEdit;
    QLineEdit* m_confirmEdit;
    QLineEdit* m_hintEdit;
};

} // namespace ArcMeta
