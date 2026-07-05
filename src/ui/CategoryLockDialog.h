#pragma once

#include "FramelessDialog.h"
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>

namespace ArcMeta {

/**
 * @brief 1:1 复刻旧版验证界面
 * 特点：无边框、安全绿图标、扁平化单输入框、回车验证
 */
class CategoryLockDialog : public FramelessDialog {
    Q_OBJECT
public:
    explicit CategoryLockDialog(const QString& hint, QWidget* parent = nullptr);
    
    QString password() const { return m_pwdEdit->text(); }

signals:
    void unlocked();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onVerify();

private:
    QLabel* m_hintLabel;
    QLineEdit* m_pwdEdit;
};

} // namespace ArcMeta
