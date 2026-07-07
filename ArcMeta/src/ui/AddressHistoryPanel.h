#pragma once

#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>

namespace ArcMeta {

/**
 * @brief 地址栏历史路径悬浮面板
 */
class AddressHistoryPanel : public QFrame {
    Q_OBJECT

public:
    explicit AddressHistoryPanel(QWidget* parent = nullptr);

    void setHistory(const QStringList& history);
    void showBelow(QWidget* anchor);

signals:
    void historyItemClicked(const QString& path);
    void historyItemRemoved(const QString& path);
    void clearAllRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void rebuild();

    QVBoxLayout* m_layout   = nullptr;
    QStringList  m_history;
};

} // namespace ArcMeta
