#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "JustifiedView.h"
#include <QPainter>
#include <QScrollBar>
#include <QResizeEvent>
#include <QStyleOptionViewItem>
#include <QAbstractItemDelegate>
#include <QTimer>
#include <algorithm>

namespace FERREX {

JustifiedView::JustifiedView(QWidget* parent) : QAbstractItemView(parent) {
    m_layoutTimer = new QTimer(this);
    m_layoutTimer->setSingleShot(true);
    m_layoutTimer->setInterval(50); // 50ms 黄金布局节流窗口
    connect(m_layoutTimer, &QTimer::timeout, this, &JustifiedView::onLayoutTimerTimeout);

    horizontalScrollBar()->setRange(0, 0);
    verticalScrollBar()->setSingleStep(20);
    
    // 2026-06-xx 物理加固：彻底消除背景穿透。
    setAutoFillBackground(true);
    viewport()->setAutoFillBackground(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    
    QPalette pal = viewport()->palette();
    pal.setColor(QPalette::Window, QColor("#1E1E1E"));
    viewport()->setPalette(pal);
    setPalette(pal);

    setProperty("gridMode", false);
}

void JustifiedView::setLayoutMode(LayoutMode mode) {
    if (m_layoutMode != mode) {
        m_layoutMode = mode;
        setProperty("gridMode", m_layoutMode == GridMode);
        scheduleLayout();
    }
}

JustifiedView::LayoutMode JustifiedView::layoutMode() const {
    return m_layoutMode;
}

void JustifiedView::setTargetRowHeight(int h) {
    if (m_targetRowHeight != h) {
        m_targetRowHeight = h;
        scheduleLayout();
    }
}

void JustifiedView::setAspectRatioRole(int role) {
    if (m_aspectRatioRole != role) {
        m_aspectRatioRole = role;
        scheduleLayout();
    }
}

void JustifiedView::reset() {
    QAbstractItemView::reset();
    scheduleLayout();
}

void JustifiedView::doItemsLayout() {
    scheduleLayout();
}

void JustifiedView::setModel(QAbstractItemModel* model) {
    if (this->model()) {
        disconnect(this->model(), &QAbstractItemModel::rowsRemoved, this, nullptr);
    }
    QAbstractItemView::setModel(model);
    if (model) {
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() {
            scheduleLayout();
        });
    }
}

QRect JustifiedView::visualRect(const QModelIndex& index) const {
    if (!index.isValid() || index.row() >= (int)m_geometries.size()) return QRect();
    QRect r = m_geometries[index.row()].rect;
    r.translate(0, -verticalScrollBar()->value());
    return r;
}

void JustifiedView::scrollTo(const QModelIndex& index, ScrollHint hint) {
    QRect rect = visualRect(index);
    if (rect.isEmpty()) return;
    
    int viewportHeight = viewport()->height();
    int scrollValue = verticalScrollBar()->value();
    
    if (hint == EnsureVisible) {
        if (rect.top() < 0) verticalScrollBar()->setValue(scrollValue + rect.top());
        else if (rect.bottom() > viewportHeight) verticalScrollBar()->setValue(scrollValue + rect.bottom() - viewportHeight);
    }
}

QModelIndex JustifiedView::indexAt(const QPoint& point) const {
    int y = point.y() + verticalScrollBar()->value();
    for (const auto& geo : m_geometries) {
        if (geo.rect.contains(point.x(), y)) {
            return model()->index(geo.index, 0);
        }
    }
    return QModelIndex();
}

void JustifiedView::dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles) {
    if (roles.isEmpty() || roles.contains(m_aspectRatioRole)) {
        scheduleLayout();
    }
    QAbstractItemView::dataChanged(topLeft, bottomRight, roles);
}

void JustifiedView::rowsInserted(const QModelIndex& parent, int start, int end) {
    scheduleLayout();
    QAbstractItemView::rowsInserted(parent, start, end);
}

void JustifiedView::rowsAboutToBeRemoved(const QModelIndex& parent, int start, int end) {
    QAbstractItemView::rowsAboutToBeRemoved(parent, start, end);
}

QModelIndex JustifiedView::moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers) {
    QModelIndex current = currentIndex();
    if (!current.isValid()) return model()->index(0, 0);
    
    int row = current.row();
    int count = (int)m_geometries.size();
    if (row < 0 || row >= count) return current;

    if (cursorAction == MoveLeft) {
        row = std::max(0, row - 1);
    } else if (cursorAction == MoveRight) {
        row = std::min(count - 1, row + 1);
    } else if (cursorAction == MoveUp || cursorAction == MoveDown) {
        QRect currentRect = m_geometries[row].rect;
        int centerX = currentRect.center().x();
        int bestIdx = -1;
        int minDistance = 1000000;

        for (int i = 0; i < count; ++i) {
            if (i == row) continue;
            QRect targetRect = m_geometries[i].rect;
            
            if (cursorAction == MoveUp && targetRect.bottom() < currentRect.top()) {
                int dy = currentRect.top() - targetRect.bottom();
                int dx = std::abs(targetRect.center().x() - centerX);
                int dist = dy * 100 + dx; 
                if (dist < minDistance) {
                    minDistance = dist;
                    bestIdx = i;
                }
            } else if (cursorAction == MoveDown && targetRect.top() > currentRect.bottom()) {
                int dy = targetRect.top() - currentRect.bottom();
                int dx = std::abs(targetRect.center().x() - centerX);
                int dist = dy * 100 + dx;
                if (dist < minDistance) {
                    minDistance = dist;
                    bestIdx = i;
                }
            }
        }
        if (bestIdx != -1) row = bestIdx;
    }
    
    return model()->index(row, 0);
}

int JustifiedView::horizontalOffset() const { return 0; }
int JustifiedView::verticalOffset() const { return verticalScrollBar()->value(); }
bool JustifiedView::isIndexHidden(const QModelIndex&) const { return false; }

void JustifiedView::setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command) {
    QRect contentsRect = rect.translated(0, verticalScrollBar()->value());
    QItemSelection selection;
    for (const auto& geo : m_geometries) {
        if (geo.rect.intersects(contentsRect)) {
            QModelIndex idx = model()->index(geo.index, 0);
            selection.select(idx, idx);
        }
    }
    selectionModel()->select(selection, command);
}

QRegion JustifiedView::visualRegionForSelection(const QItemSelection& selection) const {
    QRegion region;
    for (const auto& range : selection) {
        for (int i = range.top(); i <= range.bottom(); ++i) {
            region += visualRect(model()->index(i, 0));
        }
    }
    return region;
}

void JustifiedView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier)) {
        QModelIndex clicked = indexAt(event->pos());
        if (clicked.isValid() && m_anchorRow >= 0) {
            int anchorVisual = -1, clickedVisual = -1;
            for (int i = 0; i < (int)m_geometries.size(); ++i) {
                if (m_geometries[i].index == m_anchorRow)      anchorVisual = i;
                if (m_geometries[i].index == clicked.row())    clickedVisual = i;
            }
            if (anchorVisual >= 0 && clickedVisual >= 0) {
                int vFrom = std::min(anchorVisual, clickedVisual);
                int vTo   = std::max(anchorVisual, clickedVisual);
                QItemSelection sel;
                for (int v = vFrom; v <= vTo; ++v) {
                    QModelIndex idx = model()->index(m_geometries[v].index, 0);
                    sel.select(idx, idx);
                }
                selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
                selectionModel()->setCurrentIndex(clicked, QItemSelectionModel::NoUpdate);
                event->accept();
                viewport()->update();
                return;
            }
        }
    }

    QAbstractItemView::mousePressEvent(event);
    QModelIndex current = currentIndex();
    if (current.isValid()) {
        m_anchorRow = current.row();
    } else {
        m_anchorRow = -1;
    }
}

void JustifiedView::mouseDoubleClickEvent(QMouseEvent* event) {
    QModelIndex idx = indexAt(event->pos());
    if (!idx.isValid()) {
        QAbstractItemView::mouseDoubleClickEvent(event);
        return;
    }

    // 2026-07-xx 重构纠偏：移除已弃用的星级高度，使双击判定区域与布局保持 100% 同步
    const int textHeight   = 36;
    const int gap          = 6;  // 紧凑间距
    const int cardPadding  = 6;
    const int extraHeight  = cardPadding + textHeight + gap; 

    QRect itemRect = visualRect(idx);

    // 文字区域 = item 底部 textHeight 像素
    QRect textRect(itemRect.left(), itemRect.bottom() - textHeight, itemRect.width(), textHeight);
    // 缩略图区域 = item 顶部到文字区域之前
    QRect thumbRect(itemRect.left(), itemRect.top(), itemRect.width(), itemRect.height() - extraHeight);

    if (textRect.contains(event->pos())) {
        edit(idx);                  // 双击文字区域 → 触发行内重命名
    } else if (thumbRect.contains(event->pos())) {
        emit doubleClicked(idx);    // 双击缩略图区域 → 触发打开文件
    }
}

void JustifiedView::paintEvent(QPaintEvent*) {
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), QColor("#1E1E1E"));
    
    painter.translate(0, -verticalScrollBar()->value());
    
    for (int i = 0; i < (int)m_geometries.size(); ++i) {
        const auto& geo = m_geometries[i];
        if (geo.rect.bottom() < verticalScrollBar()->value()) continue;
        if (geo.rect.top() > verticalScrollBar()->value() + viewport()->height()) break;

        QModelIndex idx = model()->index(geo.index, 0);
        QStyleOptionViewItem option;
        initViewItemOption(&option); 
        option.rect = geo.rect;
        
        if (selectionModel()->isSelected(idx))
            option.state |= QStyle::State_Selected;
        if (currentIndex() == idx)
            option.state |= QStyle::State_HasFocus;

        itemDelegateForIndex(idx)->paint(&painter, option, idx);
    }
}

void JustifiedView::resizeEvent(QResizeEvent* event) {
    scheduleLayout();
    QAbstractItemView::resizeEvent(event);
}

void JustifiedView::updateGeometries() {
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, std::max(0, m_totalHeight - viewport()->height()));
    QAbstractItemView::updateGeometries();
}

void JustifiedView::scheduleLayout() {
    m_layoutDirty = true;
    if (!m_layoutTimer->isActive()) {
        m_layoutTimer->start();
    }
}

void JustifiedView::onLayoutTimerTimeout() {
    if (m_layoutDirty) {
        doLayout();
    }
}

void JustifiedView::doLayout() {
    m_layoutDirty = false;
    if (!model()) return;
    int count = model()->rowCount();
    
    if (count == 0) {
        m_geometries.clear();
        m_totalHeight = 0;
        updateGeometries();
        viewport()->update();
        return;
    }

    if (viewport()->width() < 50) {
        QTimer::singleShot(100, this, [this]() { doLayout(); });
        return;
    }

    m_geometries.resize(count);
    const int margin = 10;
    const int spacing = 5;
    int containerWidth = viewport()->width() - (margin * 2); 
    if (containerWidth <= 0) return;

    int currentY = margin;

    // 物理常数统一定义
    const int textHeight = 36;
    const int gap = 6;                     // 卡片与文件名的精致间隙
    const int cardPadding = 6;             // 内边距补偿 (上下各 3px)
    const int extraHeight = cardPadding + textHeight + gap;

    if (m_layoutMode == GridMode) {
        // GridMode 核心布局算法：等宽等高网格排布
        int itemWidth = m_targetRowHeight + cardPadding;
        int itemHeight = m_targetRowHeight + extraHeight;

        // 预先计算标准满行情况下的最多容纳数（最少能放 1 个）
        int maxNumInRow = (containerWidth + spacing) / (itemWidth + spacing);
        if (maxNumInRow <= 0) maxNumInRow = 1;

        // 确定标准的物理列间距（使跨行垂直列线严格对齐，避免末行由于未满排而被强行拉伸分散）
        int standardSpacing = spacing;
        if (maxNumInRow > 1) {
            standardSpacing = (containerWidth - (maxNumInRow * itemWidth)) / (maxNumInRow - 1);
        }

        int i = 0;
        while (i < count) {
            int numInRow = std::min(maxNumInRow, count - i);

            int currentX = margin;
            for (int j = 0; j < numInRow; ++j) {
                int itemIdx = i + j;
                m_geometries[itemIdx] = { QRect(currentX, currentY, itemWidth, itemHeight), itemIdx };
                currentX += itemWidth + standardSpacing;
            }
            currentY += itemHeight + spacing;
            i += numInRow;
        }
    } else {
        // JustifiedMode 逻辑保持原有自适应宽高
        int i = 0;
        while (i < count) {
            int rowStart = i;
            double rowAspectRatioSum = 0;
            std::vector<double> aspectRatios;
            std::vector<bool> isRegularFlags;

            while (i < count) {
                double origAr = model()->data(model()->index(i, 0), m_aspectRatioRole).toDouble();
                bool isReg = (origAr <= 0.0);
                double ar = origAr;
                if (ar <= 0.01) ar = 1.0;
                
                aspectRatios.push_back(ar);
                isRegularFlags.push_back(isReg);
                rowAspectRatioSum += ar;
                
                int numInRow = (int)aspectRatios.size();
                double estimatedWidth = (rowAspectRatioSum * m_targetRowHeight) + (6 * numInRow) + (spacing * (numInRow - 1));
                if (estimatedWidth > containerWidth) {
                    if (numInRow > 1) {
                        aspectRatios.pop_back();
                        isRegularFlags.pop_back();
                        rowAspectRatioSum -= ar;
                    } else {
                        i++;
                    }
                    break; 
                }
                i++;
            }

            int rowEnd = i;
            int numInRow = rowEnd - rowStart;
            if (numInRow <= 0) break;

            int actualHeight = m_targetRowHeight;
            bool isLastRow = (i == count);
            
            bool containsRegular = false;
            for (bool isReg : isRegularFlags) {
                if (isReg) { containsRegular = true; break; }
            }
            bool rowIsJustified = !isLastRow && !containsRegular;

            int availableImageWidth = containerWidth - (spacing * (numInRow - 1)) - (6 * numInRow);

            if (rowIsJustified) {
                actualHeight = qRound(availableImageWidth / rowAspectRatioSum);
                actualHeight = std::max(actualHeight, (int)(m_targetRowHeight * 0.75));
                actualHeight = std::min(actualHeight, (int)(m_targetRowHeight * 1.5));
            }

            int currentX = margin;

            for (int j = 0; j < numInRow; ++j) {
                int itemIdx = rowStart + j;
                int itemWidth;

                if (j == numInRow - 1 && rowIsJustified) {
                    itemWidth = (containerWidth + margin) - currentX;
                } else {
                    itemWidth = qRound(aspectRatios[j] * actualHeight) + cardPadding;
                }

                m_geometries[itemIdx] = { QRect(currentX, currentY, itemWidth, actualHeight + extraHeight), itemIdx };
                currentX += itemWidth + spacing; 
            }
            currentY += actualHeight + extraHeight + spacing; 
        }
    }

    m_totalHeight = currentY + 10;
    updateGeometries();
    viewport()->update();
}

} // namespace FERREX