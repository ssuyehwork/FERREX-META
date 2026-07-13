#include "HistoryDropdownController.h"
#include "ScanDialog.h"
#include "UiHelper.h"
#include <QHBoxLayout>
#include <QPushButton>
#include <QWidgetAction>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QMenu>

namespace FERREX {

class HistoryItemWidget : public QWidget {
public:
    HistoryItemWidget(const QString& text, bool isQuery, QMenu* parentMenu, ScanDialog* dialog, QWidget* parent = nullptr)
        : QWidget(parent), m_text(text), m_isQuery(isQuery), m_parentMenu(parentMenu), m_dialog(dialog)
    {
        // 开启整行悬停高亮样式
        this->setStyleSheet(
            "HistoryItemWidget { background-color: transparent; } "
            "HistoryItemWidget:hover { background-color: #2A2A2A; }"
        );

        // 采用极简扁平与紧凑的布局结构，为窄下拉框（如 120px 的后缀框）腾出空间
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 1, 4, 1);
        layout->setSpacing(4);

        // A. 左侧：条目文本按钮
        auto* btnText = new QPushButton(m_text, this);
        btnText->setCursor(Qt::PointingHandCursor);
        btnText->setStyleSheet(
            "QPushButton { background: transparent; border: none; color: #CCCCCC; text-align: left; font-size: 12px; padding: 4px 0; } "
            "QPushButton:hover { color: #FFFFFF; }"
        );
        connect(btnText, &QPushButton::clicked, this, &HistoryItemWidget::onSelectTriggered);
        layout->addWidget(btnText, 1);

        // B. 右侧：“×” 单项删除按钮
        auto* btnDelete = new QPushButton("×", this);
        btnDelete->setFixedSize(18, 18);
        btnDelete->setCursor(Qt::PointingHandCursor);
        btnDelete->setToolTip("移除该历史记录");
        btnDelete->setStyleSheet(
            "QPushButton { background: transparent; border: none; color: #888888; font-size: 14px; font-weight: bold; border-radius: 3px; line-height: 18px; } "
            "QPushButton:hover { color: #FFFFFF; background-color: #E81123; }" // 悬停显红
        );
        connect(btnDelete, &QPushButton::clicked, this, &HistoryItemWidget::onDeleteTriggered);
        layout->addWidget(btnDelete);
    }

private:
    void onSelectTriggered() {
        if (m_dialog) {
            m_dialog->setHistoryText(m_text, m_isQuery);
        }
        if (m_parentMenu) {
            m_parentMenu->close();
        }
    }

    void onDeleteTriggered() {
        if (m_dialog) {
            m_dialog->removeHistoryItem(m_text, m_isQuery);
        }
        if (m_parentMenu) {
            m_parentMenu->close();
            bool isQuery = m_isQuery;
            ScanDialog* dialog = m_dialog;
            if (dialog) {
                QTimer::singleShot(50, dialog, [dialog, isQuery]() {
                    dialog->reopenHistoryMenu(isQuery);
                });
            }
        }
    }

    QString m_text;
    bool m_isQuery;
    QMenu* m_parentMenu;
    ScanDialog* m_dialog;
};

HistoryDropdownController::HistoryDropdownController(ScanDialog* dialog)
    : QObject(dialog) {}

bool HistoryDropdownController::showDropdown(QLineEdit* edit, bool isQuery) {
    if (!edit) return false;
    ScanDialog* dialog = qobject_cast<ScanDialog*>(parent());
    if (!dialog) return false;

    const QStringList& history = isQuery ? dialog->m_config.queryHistory : dialog->m_config.extHistory;
    if (history.isEmpty()) return false;

    QMenu menu(dialog);
    menu.setStyleSheet(
        "QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; border-radius: 6px; padding: 4px 0; }"
        "QMenu::item { padding: 0px 0px; background: transparent; }"
        "QMenu::separator { height: 1px; background: #333; margin: 4px 0; }"
    );

    menu.setFixedWidth(edit->width());

    for (const QString& item : history) {
        auto* wa = new QWidgetAction(&menu);
        auto* itemWidget = new HistoryItemWidget(item, isQuery, &menu, dialog, &menu);
        wa->setDefaultWidget(itemWidget);
        menu.addAction(wa);
    }

    menu.addSeparator();

    auto* clearAction = menu.addAction("清空历史记录", [dialog, isQuery, &menu]() {
        if (isQuery) dialog->m_config.queryHistory.clear();
        else dialog->m_config.extHistory.clear();
        dialog->m_config.save();
        menu.close();
    });
    clearAction->setIcon(UiHelper::getIcon("close", QColor("#FF4444"), 12));

    menu.exec(edit->mapToGlobal(QPoint(0, edit->height())));
    return true;
}

} // namespace FERREX
