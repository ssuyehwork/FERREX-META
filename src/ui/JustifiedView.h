#pragma once

#include <QAbstractItemView>
#include <QList>
#include <vector>

namespace FERREX {

class JustifiedView : public QAbstractItemView {
    Q_OBJECT
public:
    enum LayoutMode { JustifiedMode, GridMode };

    explicit JustifiedView(QWidget* parent = nullptr);

    void setTargetRowHeight(int h);
    void setAspectRatioRole(int role);
    void setLayoutMode(LayoutMode mode);
    LayoutMode layoutMode() const;

    QRect visualRect(const QModelIndex& index) const override;
    void scrollTo(const QModelIndex& index, ScrollHint hint = EnsureVisible) override;
    QModelIndex indexAt(const QPoint& point) const override;

    void reset() override;
    void doItemsLayout() override;
    void setModel(QAbstractItemModel* model) override;

protected slots:
    void dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles = QList<int>()) override;
    void rowsInserted(const QModelIndex& parent, int start, int end) override;
    void rowsAboutToBeRemoved(const QModelIndex& parent, int start, int end) override;

protected:
    QModelIndex moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) override;
    int horizontalOffset() const override;
    int verticalOffset() const override;
    bool isIndexHidden(const QModelIndex& index) const override;
    void setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command) override;
    QRegion visualRegionForSelection(const QItemSelection& selection) const override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void updateGeometries() override;

protected slots:
    // 2026-06-xx 性能优化：布局节流槽
    void onLayoutTimerTimeout();

private:
    void doLayout();
    void scheduleLayout();

    struct ItemGeometry {
        QRect rect;
        int index;
    };
    std::vector<ItemGeometry> m_geometries;
    int m_totalHeight = 0;
    QTimer* m_layoutTimer = nullptr;
    bool m_layoutDirty = false;
    int m_targetRowHeight = 128;
    int m_aspectRatioRole = Qt::UserRole + 2;
    int m_anchorRow = -1; // 2026-06-16 物理锚点：锁定 Shift 多选起始行
    LayoutMode m_layoutMode = JustifiedMode;
};

} // namespace FERREX
