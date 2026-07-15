#include "ListResultView.h"
#include "UiHelper.h"
#include <QTableView>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QPainterPath>
#include <QLineEdit>

namespace FERREX {

class ListDefaultColumnDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        int hoveredRow = -1;
        if (option.widget) {
            hoveredRow = option.widget->property("hoveredRow").toInt();
        }
        bool isSelected = (option.state & QStyle::State_Selected);
        bool isHovered = (index.row() == hoveredRow);

        if (isSelected) {
            painter->fillRect(option.rect, QColor("#094771"));
        } else if (isHovered) {
            painter->fillRect(option.rect, QColor("#2A2A2A"));
        }

        QStyleOptionViewItem opt = option;
        opt.state &= ~QStyle::State_Selected;
        opt.state &= ~QStyle::State_MouseOver;
        
        QStyledItemDelegate::paint(painter, opt, index);
        painter->restore();
    }
};

class ListThumbnailDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);

        int hoveredRow = -1;
        if (option.widget) {
            hoveredRow = option.widget->property("hoveredRow").toInt();
        }
        bool isSelected = (option.state & QStyle::State_Selected);
        bool isHovered = (index.row() == hoveredRow);

        if (isSelected) {
            painter->fillRect(option.rect, QColor("#094771")); 
        } else if (isHovered) {
            painter->fillRect(option.rect, QColor("#2A2A2A"));
        }

        int padding = 3;
        int side = option.rect.height() - (padding * 2);
        if (side <= 0) side = 16; 

        QRect squareRect(option.rect.left() + 6, option.rect.top() + padding, side, side);

        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#2d2d2d"));
        QPainterPath cardPath;
        cardPath.addRoundedRect(squareRect, 4, 4);
        painter->drawPath(cardPath);

        QVariant decoData = index.data(Qt::DecorationRole);

        if (decoData.canConvert<QPixmap>()) {
            QPixmap thumb = decoData.value<QPixmap>();
            if (!thumb.isNull()) {
                QPixmap scaled = thumb.scaled(squareRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                int x = squareRect.center().x() - scaled.width() / 2;
                int y = squareRect.center().y() - scaled.height() / 2;
                painter->drawPixmap(x, y, scaled);
            }
        } else {
            QIcon icon = qvariant_cast<QIcon>(decoData);
            if (!icon.isNull()) {
                int iconSize = side * 0.6; 
                QRect iconRect(squareRect.center().x() - iconSize / 2,
                               squareRect.center().y() - iconSize / 2,
                               iconSize, iconSize);
                icon.paint(painter, iconRect);
            }
        }

        QString name = index.data(Qt::DisplayRole).toString();
        QColor textColor = isSelected ? QColor("#FFFFFF") : QColor("#3498db");

        painter->setPen(textColor);
        painter->setFont(option.font);

        QRect textRect = option.rect;
        textRect.setLeft(squareRect.right() + 10);

        QString elidedText = option.fontMetrics.elidedText(name, Qt::ElideMiddle, textRect.width() - 10);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elidedText);

        // 【物理自绘底部分割线补齐逻辑】（对应用户原话：“唯独名称列到路径列出现这样的截断 / 冗余像素...排查根本原因”）
        // 在 delegate 绘制的最后，使用与 QSS 相同的 #252526 灰色画笔，在最底下一像素处画一条贯通线，使之与第二列无缝相接
        painter->save();
        // 显式关闭抗锯齿，确保绘制出 1 像素物理线，避免变粗和模糊
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setPen(QColor("#252526"));
        // QRect::bottom() 是底部的 y 坐标，绘制从左边缘到右边缘的 1px 水平线
        painter->drawLine(option.rect.left(), option.rect.bottom(), option.rect.right(), option.rect.bottom());
        painter->restore();

        painter->restore();
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (editor) {
            editor->setStyleSheet(
                "background-color: #2D2D2D; color: white; "
                "selection-background-color: #3498db; "
                "border: 1px solid #3498db; border-radius: 4px; padding: 0 4px;"
            );
        }
        return editor;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        QString value = index.model()->data(index, Qt::EditRole).toString();
        QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor); 
        if (lineEdit) {
            lineEdit->setText(value); 
            int lastDot = value.lastIndexOf('.'); 
            if (lastDot > 0) { 
                lineEdit->setSelection(0, lastDot); 
            } else { 
                lineEdit->selectAll(); 
            }
        }
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& /*index*/) const override {
        int padding = 3;
        int side = option.rect.height() - (padding * 2);
        if (side <= 0) side = 16;

        int textLeft = option.rect.left() + 6 + side + 10;
        const int targetEditorHeight = 28;
        int yOffset = (option.rect.height() - targetEditorHeight) / 2;
        int editorTop = option.rect.top() + yOffset;

        int editorWidth = option.rect.width() - (textLeft - option.rect.left()) - 10;
        QRect editorRect(textLeft, editorTop, editorWidth, targetEditorHeight);

        editor->setGeometry(editorRect);
    }
};

class ListRowHoverFilter : public QObject {
public:
    explicit ListRowHoverFilter(QTableView* tableView) : QObject(tableView), m_tableView(tableView) {
        m_tableView->viewport()->setAttribute(Qt::WA_Hover, true);
        m_tableView->viewport()->setMouseTracking(true);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_tableView->viewport()) {
            if (event->type() == QEvent::MouseMove || event->type() == QEvent::HoverMove ||
                event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
                QPoint pos = m_tableView->viewport()->mapFromGlobal(QCursor::pos());
                QModelIndex idx = m_tableView->indexAt(pos);
                int row = idx.isValid() ? idx.row() : -1;
                
                int oldRow = m_tableView->property("hoveredRow").toInt();
                if (row != oldRow) {
                    m_tableView->setProperty("hoveredRow", row);
                    m_tableView->viewport()->update();
                }
            } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
                int oldRow = m_tableView->property("hoveredRow").toInt();
                if (oldRow != -1) {
                    m_tableView->setProperty("hoveredRow", -1);
                    m_tableView->viewport()->update();
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QTableView* m_tableView;
};

ListResultView::ListResultView(QWidget* parent) : IScanResultView(parent) {
    m_tableView = new QTableView(parent);
    m_tableView->verticalHeader()->setDefaultSectionSize(30); 
    m_tableView->setItemDelegateForColumn(0, new ListThumbnailDelegate(m_tableView)); 
    m_tableView->setItemDelegateForColumn(1, new ListDefaultColumnDelegate(m_tableView)); 
    m_tableView->setItemDelegateForColumn(2, new ListDefaultColumnDelegate(m_tableView)); 
    m_tableView->setItemDelegateForColumn(3, new ListDefaultColumnDelegate(m_tableView)); 
    m_tableView->setContextMenuPolicy(Qt::CustomContextMenu);
    
    m_tableView->setProperty("hoveredRow", -1);
    ListRowHoverFilter* hoverFilter = new ListRowHoverFilter(m_tableView);
    m_tableView->viewport()->installEventFilter(hoverFilter);
    
    m_tableView->setStyleSheet(
        "QTableView { "
        "background-color: #1E1E1E; "
        "alternate-background-color: #000000; "
        "border: 1px solid #333; "
        "color: #D4D4D4; "
        "selection-background-color: #094771; "
        "selection-color: #FFFFFF; "
        "outline: none; "
        "gridline-color: transparent; "
        "}"
        "QTableView::item { border-bottom: 1px solid #252526; }"
        "QHeaderView::section { background-color: #252526; color: #888; border: none; border-right: 1px solid #333; padding: 4px; height: 24px; }"
        "QHeaderView::section:horizontal:first { padding-left: 14px; }" 
        "QHeaderView { background-color: #252526; border: none; }"
    );
    
    m_tableView->horizontalHeader()->setStretchLastSection(false); 
    m_tableView->horizontalHeader()->setMinimumSectionSize(60);
    m_tableView->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    
    m_tableView->setColumnWidth(0, 260); 
    m_tableView->setColumnWidth(2, 100); 
    m_tableView->setColumnWidth(3, 140); 
    
    m_tableView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tableView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tableView->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_tableView->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);

    m_tableView->verticalHeader()->setVisible(false);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableView->setSelectionMode(QAbstractItemView::ExtendedSelection); 
    m_tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    
    m_tableView->setDragEnabled(true);
    m_tableView->setDragDropMode(QAbstractItemView::DragOnly);
    m_tableView->setDefaultDropAction(Qt::CopyAction);

    m_tableView->setShowGrid(false);
    m_tableView->setAlternatingRowColors(true);
    m_tableView->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
}

ListResultView::~ListResultView() {
}

QWidget* ListResultView::getWidget() {
    return m_tableView;
}

QAbstractItemView* ListResultView::getBaseView() {
    return m_tableView;
}

void ListResultView::setModel(QAbstractItemModel* model) {
    m_tableView->setModel(model);
    m_tableView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tableView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tableView->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_tableView->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
}

void ListResultView::setIconSize(int size) {
    m_tableView->verticalHeader()->setDefaultSectionSize(size);
}

void ListResultView::refreshLayout() {
}

} // namespace FERREX
