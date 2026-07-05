#include "CategoryLockDialog.h"
#include "UiHelper.h"
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QApplication>

namespace ArcMeta {

CategoryLockDialog::CategoryLockDialog(const QString& hint, QWidget* parent) 
    : FramelessDialog("分类解锁", parent) 
{
    setFixedSize(300, 240); // 紧凑尺寸

    auto* layout = new QVBoxLayout(m_contentArea);
    layout->setContentsMargins(20, 15, 20, 25);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(8);

    // 1. 锁图标 (100% 还原安全绿 #00A650)
    auto* lockIcon = new QLabel();
    lockIcon->setPixmap(UiHelper::getIcon("lock", QColor("#00A650"), 32).pixmap(32, 32));
    lockIcon->setAlignment(Qt::AlignCenter);
    layout->addWidget(lockIcon);

    // 2. 提示文字 (安全绿加粗)
    auto* titleLabel = new QLabel("输入密码查看内容");
    titleLabel->setStyleSheet("color: #00A650; font-size: 13px; font-weight: bold; background: transparent;");
    titleLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLabel);

    // 3. 密码提示 (灰色精简)
    m_hintLabel = new QLabel(QString("密码提示: %1").arg(hint.isEmpty() ? "无" : hint));
    m_hintLabel->setStyleSheet("color: #555555; font-size: 11px; background: transparent;");
    m_hintLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_hintLabel);

    layout->addSpacing(10);

    // 4. 扁平化密码输入框 (180px)
    m_pwdEdit = new QLineEdit();
    m_pwdEdit->setPlaceholderText("输入密码");
    m_pwdEdit->setEchoMode(QLineEdit::Password);
    m_pwdEdit->setFixedWidth(180);
    m_pwdEdit->setFixedHeight(28);
    // 严格对齐 RapidNotes 扁平风格：深黑背景 + 细边框
    m_pwdEdit->setStyleSheet(
        "QLineEdit {"
        "  background-color: #121212; border: 1px solid #333; border-radius: 4px;"
        "  padding: 0 8px; color: white; font-size: 12px;"
        "}"
        "QLineEdit:focus { border: 1px solid #3a90ff; }"
    );
    connect(m_pwdEdit, &QLineEdit::returnPressed, this, &CategoryLockDialog::onVerify);
    m_pwdEdit->installEventFilter(this);
    layout->addWidget(m_pwdEdit, 0, Qt::AlignHCenter);

    layout->addStretch();
    
    m_pwdEdit->setFocus();
}

bool CategoryLockDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_pwdEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            if (!m_pwdEdit->text().isEmpty()) {
                m_pwdEdit->clear();
                return true;
            }
        }
    }
    return FramelessDialog::eventFilter(watched, event);
}

void CategoryLockDialog::keyPressEvent(QKeyEvent* event) {
    FramelessDialog::keyPressEvent(event);
}

void CategoryLockDialog::onVerify() {
    // 此处由调用方处理具体的验证逻辑，此处仅发送 accept 或 unlocked 信号
    accept();
}

} // namespace ArcMeta
