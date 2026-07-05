#pragma once

#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <QLineEdit>
#include "CategoryModel.h"
#include "StyleLibrary.h"
using namespace ArcMeta::Style;

namespace ArcMeta {

class CategoryDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!index.isValid()) return;

        if (option.state & QStyle::State_Editing) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        bool selected = option.state & QStyle::State_Selected;
        bool hover = option.state & QStyle::State_MouseOver;
        bool isSelectable = index.flags() & Qt::ItemIsSelectable;

        if (isSelectable && (selected || hover)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            QString colorHex = index.data(ColorRole).toString();
            QColor baseColor = colorHex.isEmpty() ? QColor("#3498db") : QColor(colorHex);
            QColor bg = selected ? baseColor : QColor("#2a2d2e");
            if (selected) bg.setAlphaF(0.2f); 

            // 2026-03-xx 按照用户要求：物理隔离 branch 区域，解决选中背景遮挡折叠图标的问题
            // 严格执行宪法第五定律第 6 条：padding: 2px 4px, margin: 1px 2px
            QStyle* style = option.widget ? option.widget->style() : QApplication::style();
            QRect decoRect = style->subElementRect(QStyle::SE_ItemViewItemDecoration, &option, option.widget);
            QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget);
            
            // 高亮矩形仅包含图标与文本，不触碰左侧 branch 区域
            QRect contentRect = decoRect.united(textRect);
            
            // 2026-03-xx 物理对齐修正：确保 contentRect 的左边界从图标开始，右边界延展至 widget 边缘（减去右边距）
            if (option.widget) {
                contentRect.setRight(option.widget->width() - 4);
            }

            // 应用宪法规范：margin 1px 2px (上下 1px, 左右 2px)
            contentRect.adjust(2, 1, -2, -1);
            
            painter->setBrush(bg);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(contentRect, 4, 4);
            painter->restore();
        }

        QStyleOptionViewItem opt = option;
        opt.state &= ~QStyle::State_Selected;
        opt.state &= ~QStyle::State_MouseOver;
        
        if (selected) {
            opt.palette.setColor(QPalette::Text, Qt::white);
            opt.palette.setColor(QPalette::HighlightedText, Qt::white);
        }
        
        QStyledItemDelegate::paint(painter, opt, index);
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(option);
        Q_UNUSED(index);
        QLineEdit* editor = new QLineEdit(parent);
        editor->setStyleSheet(
            "QLineEdit {"
            "  background-color: #2D2D2D;"
            "  color: white;"
            "  border: 1px solid #4a90e2;"
            "  border-radius: 6px;"
            "  padding: 0px 4px;"
            "  margin: 0px;"
            "}"
        );
        return editor;
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        Q_UNUSED(index);
        QStyle* style = option.widget ? option.widget->style() : QApplication::style();
        QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &option, option.widget);
        textRect.adjust(0, -1, 0, 1);
        editor->setGeometry(textRect);
    }
};

} // namespace ArcMeta
