#include "AddressHistoryPanel.h"
#include "UiHelper.h"
#include <QCursor>
#include <QApplication>
#include <QMouseEvent>

namespace ArcMeta {

AddressHistoryPanel::AddressHistoryPanel(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground, false);
    setObjectName("AddressHistoryPanel");
    setStyleSheet(
        "#AddressHistoryPanel {"
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

void AddressHistoryPanel::setHistory(const QStringList& history) {
    m_history = history;
    rebuild();
}

void AddressHistoryPanel::rebuild() {
    QLayoutItem* child;
    while ((child = m_layout->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }

    if (m_history.isEmpty()) {
        QLabel* empty = new QLabel("暂无历史路径", this);
        empty->setStyleSheet("color: #666666; font-size: 12px; padding: 4px 8px;");
        m_layout->addWidget(empty);
    } else {
        QWidget* titleRow = new QWidget(this);
        titleRow->setStyleSheet("QWidget { background: transparent; }");
        QHBoxLayout* titleLayout = new QHBoxLayout(titleRow);
        titleLayout->setContentsMargins(4, 0, 4, 0);
        titleLayout->setSpacing(0);

        QLabel* titleLabel = new QLabel("最近访问路径", titleRow);
        titleLabel->setStyleSheet("color: #888888; font-size: 11px;");

        QPushButton* btnClearAll = new QPushButton("全部清除", titleRow);
        btnClearAll->setFixedHeight(20);
        btnClearAll->setFlat(true);
        btnClearAll->setStyleSheet(
            "QPushButton { color: #666666; font-size: 11px; border: none; background: transparent; }"
            "QPushButton:hover { color: #378ADD; }"
        );
        connect(btnClearAll, &QPushButton::clicked, this, &AddressHistoryPanel::clearAllRequested);

        titleLayout->addWidget(titleLabel);
        titleLayout->addStretch();
        titleLayout->addWidget(btnClearAll);
        m_layout->addWidget(titleRow);

        QFrame* sep = new QFrame(this);
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("background: #333333; border: none; max-height: 1px;");
        m_layout->addWidget(sep);

        for (const QString& path : m_history) {
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

            QLabel* icon = new QLabel(row);
            icon->setPixmap(UiHelper::getIcon("folder", QColor("#555555"), 14).pixmap(14, 14));
            icon->setFixedSize(14, 14);

            QLabel* pathLabel = new QLabel(path, row);
            pathLabel->setStyleSheet("color: #CCCCCC; font-size: 12px; background: transparent;");

            QPushButton* btnRemove = new QPushButton(row);
            btnRemove->setFixedSize(16, 16);
            btnRemove->setFlat(true);
            btnRemove->setIcon(UiHelper::getIcon("close", QColor("#555555"), 12));
            btnRemove->setIconSize(QSize(12, 12));
            btnRemove->setStyleSheet(
                "QPushButton { background: transparent; border: none; border-radius: 3px; }"
                "QPushButton:hover { background: #3E3E42; }"
            );
            connect(btnRemove, &QPushButton::clicked, this, [this, path]() {
                emit historyItemRemoved(path);
            });

            rowLayout->addWidget(icon);
            rowLayout->addWidget(pathLabel, 1);
            rowLayout->addWidget(btnRemove);

            row->installEventFilter(this);
            row->setProperty("path", path);

            m_layout->addWidget(row);
        }
    }
    adjustSize();
}

void AddressHistoryPanel::showBelow(QWidget* anchor) {
    if (!anchor) return;
    QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height() + 3));
    move(pos);
    setFixedWidth(anchor->width());
    rebuild();
    show();
    raise();
}

bool AddressHistoryPanel::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        QWidget* w = qobject_cast<QWidget*>(obj);
        if (w && w->objectName() == "historyRow") {
            QString path = w->property("path").toString();
            if (!path.isEmpty()) {
                emit historyItemClicked(path);
            }
            return true;
        }
    }
    return QFrame::eventFilter(obj, event);
}

} // namespace ArcMeta
