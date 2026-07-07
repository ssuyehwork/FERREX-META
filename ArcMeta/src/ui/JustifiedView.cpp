#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "JustifiedView.h"
#include "../core/ModelContract.h"
#include <QPainter>
#include <QScrollBar>
#include <QResizeEvent>
#include <QStyleOptionViewItem>
#include <QAbstractItemDelegate>
#include <QTimer>
#include <algorithm>

namespace ArcMeta {

JustifiedView::JustifiedView(QWidget* parent) : QAbstractItemView(parent) {
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

}

void JustifiedView::setTargetRowHeight(int h) {
    if (m_targetRowHeight != h) {
        m_targetRowHeight = h;
        doLayout();
    }
}

void JustifiedView::setAspectRatioRole(int role) {
    if (m_aspectRatioRole != role) {
        m_aspectRatioRole = role;
        doLayout();
    }
}

void JustifiedView::reset() {
    QAbstractItemView::reset();
    doLayout();
}

void JustifiedView::doItemsLayout() {
    doLayout();
}

void JustifiedView::setModel(QAbstractItemModel* model) {
    if (this->model()) {
        disconnect(this->model(), &QAbstractItemModel::rowsRemoved, this, nullptr);
    }
    QAbstractItemView::setModel(model);
    if (model) {
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() {
            QTimer::singleShot(0, this, [this]() { doLayout(); });
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
        doLayout();
    }
    QAbstractItemView::dataChanged(topLeft, bottomRight, roles);
}

void JustifiedView::rowsInserted(const QModelIndex& parent, int start, int end) {
    doLayout();
    QAbstractItemView::rowsInserted(parent, start, end);
}

void JustifiedView::rowsAboutToBeRemoved(const QModelIndex& parent, int start, int end) {
    // 直接转发给基类即可，实际重排由 setModel 中连接的 rowsRemoved 信号处理
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
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
        QModelIndex idx = indexAt(event->pos());
        if (!idx.isValid()) {
            m_isDraggingSelection = true;
            m_dragStartPos = event->pos();
            m_selectionRect = QRect();
            selectionModel()->clearSelection();
            event->accept();
            return;
        }
    }

    // 2026-06-xx 物理拨乱反正：仅在按下 Shift 时执行自定义“视觉顺序”选择逻辑
    // 其余所有情况（普通单击、Ctrl、空白处）均退避并转发给基类处理，防止破坏 Model/View 原生多选状态
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

    // 更新锚点：在每次有效点击（非 Shift 点击）时同步锚点，以便后续 Shift 多选定位
    QAbstractItemView::mousePressEvent(event);
    QModelIndex current = currentIndex();
    if (current.isValid()) {
        m_anchorRow = current.row();
    } else {
        m_anchorRow = -1;
    }
}

void JustifiedView::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDraggingSelection) {
        m_selectionRect = QRect(m_dragStartPos, event->pos()).normalized();
        setSelection(m_selectionRect, QItemSelectionModel::ClearAndSelect);
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractItemView::mouseMoveEvent(event);
}

void JustifiedView::mouseReleaseEvent(QMouseEvent* event) {
    if (m_isDraggingSelection) {
        m_isDraggingSelection = false;
        m_selectionRect = QRect();
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractItemView::mouseReleaseEvent(event);
}

void JustifiedView::mouseDoubleClickEvent(QMouseEvent* event) {
    QModelIndex idx = indexAt(event->pos());
    if (!idx.isValid()) {
        QAbstractItemView::mouseDoubleClickEvent(event);
        return;
    }

    // 2026-06-16 工业级纠偏：与 doLayout 及 ThumbnailDelegate::calculateMetrics 保持 100% 同步
    const int textHeight   = 36;
    const int ratingHeight = 20;
    const int gap          = 4;
    const int cardPadding  = 6;
    const int extraHeight  = cardPadding + textHeight + ratingHeight + gap; 

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
    // 2026-06-xx 物理修复：在开启 TranslucentBackground 时手动填充坚实背景，防止透明穿透
    painter.fillRect(viewport()->rect(), QColor("#1E1E1E"));
    
    painter.save();
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

        // 2026-05-20 物理适配：使用接口推荐的 itemDelegateForIndex
        itemDelegateForIndex(idx)->paint(&painter, option, idx);
    }
    painter.restore();

    // 2026-06-xx 按照 1.7 需求：绘制蓝色透明框选矩形
    if (m_isDraggingSelection && !m_selectionRect.isEmpty()) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        // 2026-06-xx 物理对齐：使用标准高亮蓝 (#378ADD) 并增加透明度以达到预期效果
        QColor highlightColor = QColor("#378ADD");
        QColor brushColor = highlightColor;
        brushColor.setAlpha(80); // 适度提升透明度可见度 (Alpha 0-255)
        painter.setBrush(brushColor);
        painter.setPen(QPen(highlightColor, 1, Qt::SolidLine));
        painter.drawRect(m_selectionRect);
        painter.restore();
    }
}

void JustifiedView::resizeEvent(QResizeEvent* event) {
    doLayout();
    QAbstractItemView::resizeEvent(event);
}

void JustifiedView::updateGeometries() {
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, std::max(0, m_totalHeight - viewport()->height()));
    QAbstractItemView::updateGeometries();
}

void JustifiedView::doLayout() {
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
    // 可用宽度：视口宽度 - 左边距 - 右边距
    int containerWidth = viewport()->width() - (margin * 2); 
    if (containerWidth <= 0) return;

    int currentY = margin; 
    int i = 0;
    
    while (i < count) {
        int rowStart = i;
        double rowAspectRatioSum = 0;
        std::vector<double> aspectRatios;

        bool forceBreak = false;
        while (i < count) {
            QModelIndex idx = model()->index(i, 0);
            double ar = model()->data(idx, m_aspectRatioRole).toDouble();
            if (ar <= 0) ar = 1.0;
            
            // 2026-07-xx 物理分离逻辑：如果当前项是文件，但行首是文件夹（或反之），强制换行
            QString type = model()->data(idx, TypeRole).toString();
            bool isCurrentDir = (type == "folder" || type == "category");
            
            if (i > rowStart) {
                QModelIndex prevIdx = model()->index(i - 1, 0);
                QString prevType = model()->data(prevIdx, TypeRole).toString();
                bool isPrevDir = (prevType == "folder" || prevType == "category");
                
                if (isCurrentDir != isPrevDir) {
                    forceBreak = true;
                    break;
                }
            }

            aspectRatios.push_back(ar);
            rowAspectRatioSum += ar;
            
            int numInRow = (int)aspectRatios.size();
            // 2026-06-xx 物理修正：考虑 ThumbnailDelegate 的内边距 (左右各 3px = 6px)
            // 预估宽度 = (宽高比总和 * 目标高度) + (内边距补偿 6px * 数量) + (项间距 * 间距数量)
            double estimatedWidth = (rowAspectRatioSum * m_targetRowHeight) + (6 * numInRow) + (spacing * (numInRow - 1));
            if (estimatedWidth > containerWidth) {
                // 如果单项就超过了容器宽度，则强制独占一行
                if (numInRow > 1) {
                    aspectRatios.pop_back();
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
        // 2026-07-xx 物理对齐修正：若因类型差异导致的强制换行，该行不执行两端对齐，防止图标拉伸变形
        bool rowIsJustified = !isLastRow && !forceBreak; 

        int availableImageWidth = containerWidth - (spacing * (numInRow - 1)) - (6 * numInRow);

        if (rowIsJustified) {
            actualHeight = qRound(availableImageWidth / rowAspectRatioSum);
            // 工业级纠偏：允许高度在一定范围内浮动以填满行宽，无论是否超出 targetRowHeight 范围均开启对齐
            actualHeight = std::max(actualHeight, (int)(m_targetRowHeight * 0.75));
            actualHeight = std::min(actualHeight, (int)(m_targetRowHeight * 1.5));
            rowIsJustified = true; 
        }

        int currentX = margin;
        const int textHeight = 36;
        const int ratingHeight = 20;
        const int gap = 4;
        const int cardPadding = 6; // 左右内边距总和 (3px + 3px)
        const int extraHeight = cardPadding + textHeight + ratingHeight + gap; // cardPadding 也是上下内边距总和

        for (int j = 0; j < numInRow; ++j) {
            int itemIdx = rowStart + j;
            int itemWidth;

            if (j == numInRow - 1 && rowIsJustified) {
                // 最后一个 item 精确填满剩余宽度，消除舍入误差导致的空隙
                itemWidth = (containerWidth + margin) - currentX;
            } else {
                itemWidth = qRound(aspectRatios[j] * actualHeight) + cardPadding;
            }

            m_geometries[itemIdx] = { QRect(currentX, currentY, itemWidth, actualHeight + extraHeight), itemIdx };
            currentX += itemWidth + spacing; 
        }
        currentY += actualHeight + extraHeight + spacing; // 统一行高推进
    }

    m_totalHeight = currentY + 10;
    updateGeometries();
    viewport()->update();
}

} // namespace ArcMeta
