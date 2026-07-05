#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QScrollArea>
#include <QPushButton>
#include <QMap>
#include <QStringList>

namespace ArcMeta {

// ─── 色相滑块 (内嵌版) ─────────────────────────────────────────────
class InlineHueSlider : public QWidget {
    Q_OBJECT
public:
    explicit InlineHueSlider(QWidget* parent = nullptr);
    void setHue(int h);
    int hue() const { return m_h; }

signals:
    void hueChanged(int h);
    void sliderReleased();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void updateFromPos(int x);
    int m_h = 0;
};

struct FilterState {
    QList<int>   ratings;
    QStringList  colors;
    QStringList  tags;
    QStringList  types;
    QStringList  createDates;   // "today" | "yesterday" | "YYYY-MM-DD"
    QStringList  modifyDates;
    int          colorTolerance = 30; // 2026-05-17 按照用户要求：自定义颜色相近色容差（0~100），由 ColorPicker 准确度滑条驱动
};

/**
 * @brief 筛选面板 — 动态 Adobe Bridge 风格
 *
 * 由 MainWindow 在目录切换后调用 populate() 驱动数据填充。
 * 每行整体可点击（不需要对准复选框）。
 */
class FilterPanel : public QFrame {
    Q_OBJECT

public:
    explicit FilterPanel(QWidget* parent = nullptr);
    ~FilterPanel() override = default;


    void populate(
        const QMap<int, int>&        ratingCounts,
        const QMap<QString, int>&    colorCounts,
        const QMap<QString, int>&    tagCounts,
        const QMap<QString, int>&    typeCounts,
        const QMap<QString, int>&    createDateCounts,
        const QMap<QString, int>&    modifyDateCounts
    );

    FilterState currentFilter() const { return m_filter; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void filterChanged(const FilterState& state);
    void resetSearchRequested();

public slots:
    void clearAllFilters();

private:
    void rebuildGroups();

    // 2026-05-17 根因修复：增加 outHdrLayout 参数，让调用方直接往标题行布局追加按钮
    // 彻底替代绝对定位方案，消除非布局子控件撑高 wrapper 导致的留白
    QWidget*   buildGroup(const QString& title, QVBoxLayout*& outContentLayout,
                          QHBoxLayout** outHdrLayout = nullptr);
    QCheckBox* addFilterRow(QVBoxLayout* layout, const QString& label,
                            int count, const QColor& dotColor = Qt::transparent);

    static QMap<QString, QColor> s_colorMap();

    FilterState m_filter;
    QString     m_hueSliderColor;

    QMap<int, int>      m_ratingCounts;
    QMap<QString, int>  m_colorCounts;
    QMap<QString, int>  m_tagCounts;
    QMap<QString, int>  m_typeCounts;
    QMap<QString, int>  m_createDateCounts;
    QMap<QString, int>  m_modifyDateCounts;

    QVBoxLayout*  m_mainLayout      = nullptr;
    QScrollArea*  m_scrollArea      = nullptr;
    QWidget*      m_container       = nullptr;
    QVBoxLayout*  m_containerLayout = nullptr;
    QPushButton*  m_btnClearAll     = nullptr;
};

} // namespace ArcMeta
