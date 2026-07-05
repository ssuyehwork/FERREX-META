#pragma once

#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>

namespace ArcMeta {

/**
 * @brief 2026-04-12 按照用户要求：搜索历史悬浮面板
 * 在搜索框下方弹出，显示最近 10 条搜索关键词，支持点击填入及单条删除。
 */
class SearchHistoryPanel : public QFrame {
    Q_OBJECT

public:
    explicit SearchHistoryPanel(QWidget* parent = nullptr);

    /**
     * @brief 填充/刷新历史列表
     */
    void setHistory(const QStringList& history);

    /**
     * @brief 定位并显示在指定锚点控件正下方
     */
    void showBelow(QWidget* anchor);

signals:
    /// 用户点击了某条历史关键词（应填入搜索框并触发搜索）
    void historyItemClicked(const QString& keyword);

    /// 用户点击了某条的 X 删除按钮
    void historyItemRemoved(const QString& keyword);

    /// 用户点击了"全部清除"
    void clearAllRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void rebuild();

    QVBoxLayout* m_layout   = nullptr;
    QStringList  m_history;
};

} // namespace ArcMeta
