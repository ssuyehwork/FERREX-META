#include "SearchHistoryPanel.h"
#include "SvgIcons.h"
#include "UiHelper.h"

#include <QCursor>
#include <QApplication>
#include <QMouseEvent>

namespace ArcMeta {

SearchHistoryPanel::SearchHistoryPanel(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    // 2026-06-xx 架构重构：使用 Qt::Popup 代替 Qt::Tool，Popup 标志位自带失焦自动关闭逻辑
    // 移除 Qt::WindowStaysOnTopHint，防止在主窗口关闭/最小化后依然置顶显示
    setAttribute(Qt::WA_TranslucentBackground, false);

    setObjectName("SearchHistoryPanel");
    setStyleSheet(
        "#SearchHistoryPanel {"
        "  background-color: #252526;"
        "  border: 1px solid #444444;"
        "  border-radius: 6px;"
        "}"
    );

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(6, 6, 6, 6);
    m_layout->setSpacing(2);

    hide();
}

void SearchHistoryPanel::setHistory(const QStringList& history, const QString& title) {
    m_history = history;
    m_currentTitle = title;
    rebuild();
}

void SearchHistoryPanel::rebuild() {
    // 清除旧的内容行
    QLayoutItem* child;
    while ((child = m_layout->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }

    if (m_history.isEmpty()) {
        QLabel* empty = new QLabel(QString("暂无%1").arg(m_currentTitle), this);
        empty->setStyleSheet("color: #666666; font-size: 12px; padding: 4px 8px;");
        m_layout->addWidget(empty);
    } else {
        // 标题行
        QWidget* titleRow = new QWidget(this);
        titleRow->setStyleSheet("QWidget { background: transparent; }");
        QHBoxLayout* titleLayout = new QHBoxLayout(titleRow);
        titleLayout->setContentsMargins(4, 0, 4, 0);
        titleLayout->setSpacing(0);

        QLabel* titleLabel = new QLabel(m_currentTitle, titleRow);
        titleLabel->setStyleSheet("color: #888888; font-size: 11px;");

        QPushButton* btnClearAll = new QPushButton("全部清除", titleRow);
        btnClearAll->setFixedHeight(20);
        btnClearAll->setFlat(true);
        btnClearAll->setStyleSheet(
            "QPushButton { color: #666666; font-size: 11px; border: none; background: transparent; }"
            "QPushButton:hover { color: #378ADD; }"
        );
        connect(btnClearAll, &QPushButton::clicked, this, &SearchHistoryPanel::clearAllRequested);

        titleLayout->addWidget(titleLabel);
        titleLayout->addStretch();
        titleLayout->addWidget(btnClearAll);
        m_layout->addWidget(titleRow);

        // 分割线
        QFrame* sep = new QFrame(this);
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("background: #333333; border: none; max-height: 1px;");
        m_layout->addWidget(sep);

        // 历史条目（最新的显示在最上方）
        // 2026-06-xx 逻辑纠偏：m_history 索引 0 为最新，应正向遍历以确保最新项在布局顶端
        for (int i = 0; i < m_history.size(); ++i) {
            const QString& keyword = m_history[i];

            QWidget* row = new QWidget(this);
            row->setObjectName("historyRow");
            row->setStyleSheet(
                "QWidget#historyRow { background: transparent; border-radius: 4px; }"
                "QWidget#historyRow:hover { background: #2A2A2A; }"
            );
            row->setCursor(Qt::PointingHandCursor);
            row->setFixedHeight(30);

            QHBoxLayout* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(6, 0, 4, 0);
            rowLayout->setSpacing(8);

            // 搜索图标
            QLabel* icon = new QLabel(row);
            icon->setPixmap(UiHelper::getIcon("search", QColor("#555555"), 14).pixmap(14, 14));
            icon->setFixedSize(14, 14);

            // 关键词文字
            QLabel* keywordLabel = new QLabel(keyword, row);
            keywordLabel->setStyleSheet("color: #CCCCCC; font-size: 12px; background: transparent;");

            // X 删除按钮
            QPushButton* btnRemove = new QPushButton(row);
            btnRemove->setFixedSize(16, 16);
            btnRemove->setFlat(true);
            btnRemove->setIcon(UiHelper::getIcon("close", QColor("#555555"), 12));
            btnRemove->setIconSize(QSize(12, 12));
            btnRemove->setStyleSheet(
                "QPushButton { background: transparent; border: none; border-radius: 3px; }"
                "QPushButton:hover { background: #3E3E42; }"
            );
            connect(btnRemove, &QPushButton::clicked, this, [this, keyword]() {
                emit historyItemRemoved(keyword);
            });

            rowLayout->addWidget(icon);
            rowLayout->addWidget(keywordLabel, 1);
            rowLayout->addWidget(btnRemove);

            // 点击整行（排除 X 按钮）触发关键词选中
            row->installEventFilter(this);
            row->setProperty("keyword", keyword);

            m_layout->addWidget(row);
        }
    }

    adjustSize();
}

void SearchHistoryPanel::showBelow(QWidget* anchor) {
    if (!anchor) return;
    QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height() + 3));
    move(pos);
    setFixedWidth(anchor->width());
    rebuild();
    show();
    raise();
}

bool SearchHistoryPanel::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        QWidget* w = qobject_cast<QWidget*>(obj);
        if (w && w->objectName() == "historyRow") {
            QString keyword = w->property("keyword").toString();
            if (!keyword.isEmpty()) {
                emit historyItemClicked(keyword);
            }
            return true;
        }
    }
    return QFrame::eventFilter(obj, event);
}

} // namespace ArcMeta
