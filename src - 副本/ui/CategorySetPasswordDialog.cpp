#include "CategorySetPasswordDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include "UiHelper.h"
#include "ToolTipOverlay.h"

namespace ArcMeta {

CategorySetPasswordDialog::CategorySetPasswordDialog(QWidget* parent)
    : FramelessDialog("设置密码", parent) {
    
    setFixedSize(320, 420); // 根据截图 PixPin_2026-04-03_11-39-19.png 调整比例

    auto* layout = new QVBoxLayout(m_contentArea);
    layout->setContentsMargins(20, 10, 20, 20);
    layout->setSpacing(0);
    layout->addSpacing(15);

    // 辅助宏生成输入组
    auto addInputGroup = [&](const QString& labelText, QLineEdit*& edit, bool isPassword = true) {
        auto* lbl = new QLabel(labelText);
        lbl->setStyleSheet("color: white; font-size: 13px; font-weight: bold; margin-bottom: 8px; background: transparent;");
        layout->addWidget(lbl);

        edit = new QLineEdit();
        if (isPassword) edit->setEchoMode(QLineEdit::Password);
        edit->setFixedHeight(36);
        // 物理还原截图视觉：深黑色背景 + 细微边框 + 蓝色 Focus
        edit->setStyleSheet(
            "QLineEdit {"
            "  background-color: #0F0F0F; border: 1px solid #333; border-radius: 6px;"
            "  color: white; padding: 0 10px; font-size: 13px;"
            "}"
            "QLineEdit:focus { border: 1px solid #3a90ff; }"
        );
        layout->addWidget(edit);
        layout->addSpacing(18);
    };

    addInputGroup("密码", m_pwdEdit);
    addInputGroup("密码确认", m_confirmEdit);
    addInputGroup("密码提示", m_hintEdit, false);
    
    // 物理还原：密码提示框可能更宽敞 (如果是 QTextEdit 则需要调整，截图看起来是单行)
    // 根据 PixPin_2026-04-03_11-39-19.png，提示框高度略高
    m_hintEdit->setFixedHeight(60); 

    layout->addStretch();

    // 2. 底部蓝色大按钮
    auto* btnSave = new QPushButton("保存密码设置");
    btnSave->setFixedHeight(45);
    btnSave->setStyleSheet(
        "QPushButton {"
        "  background-color: #3B82F6; color: white; border: none; border-radius: 8px;"
        "  font-size: 14px; font-weight: bold;"
        "}"
        "QPushButton:hover { background-color: #2563EB; }"
        "QPushButton:pressed { background-color: #1D4ED8; }"
    );
    connect(btnSave, &QPushButton::clicked, this, &CategorySetPasswordDialog::onSave);
    layout->addWidget(btnSave);
}

void CategorySetPasswordDialog::onSave() {
    if (m_pwdEdit->text().isEmpty()) {
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#e74c3c;'>请输入密码</b>");
        return;
    }
    if (m_pwdEdit->text() != m_confirmEdit->text()) {
        ToolTipOverlay::instance()->showText(QCursor::pos(), "<b style='color:#e74c3c;'>两次密码输入不一致</b>");
        return;
    }
    accept();
}

} // namespace ArcMeta
