#pragma once

#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <QMouseEvent>
#include <QLineEdit>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include "ContentPanel.h"
#include "../meta/MetadataManager.h"
#include "UiHelper.h"
#include "StyleLibrary.h"
using namespace ArcMeta::Style;

namespace ArcMeta {

/**
 * @brief 通用树形视图代理，提供圆角高亮效果
 */
class TreeItemDelegate : public QStyledItemDelegate {
public:
    explicit TreeItemDelegate(QObject* parent = nullptr, bool showStatus = true)
        : QStyledItemDelegate(parent), m_showStatus(showStatus) {}
    
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (!index.isValid()) return;

        bool selected = option.state & QStyle::State_Selected;
        bool hover = option.state & QStyle::State_MouseOver;

        if (selected || hover) {
            painter->save();
            // 2026-06-xx 按照用户最新要求：消除“坑坑洼洼”感，改用全行贯穿式直角高亮，填满整个区域
            QColor bg = selected ? QColor("#378ADD") : QColor("#2a2d2e");
            if (selected) bg.setAlphaF(0.15f); 

            // 物理修复：直接使用 option.rect，不进行 adjust 缩进，不使用圆角，确保色块无缝对接
            painter->setBrush(bg);
            painter->setPen(Qt::NoPen);
            painter->drawRect(option.rect);
            painter->restore();
        }

        QStyleOptionViewItem opt = option;
        opt.state &= ~QStyle::State_Selected;
        opt.state &= ~QStyle::State_MouseOver;
        
        if (selected) {
            opt.palette.setColor(QPalette::Text, Qt::white);
        } else if (m_showStatus) {
            // 2026-06-xx 按照视觉要求：未录入项文字半透明暗淡处理
            // 物理修复：校准作用域
            bool isManaged = index.data(ManagedRole).toBool();
            if (!isManaged) {
                opt.palette.setColor(QPalette::Text, QColor(238, 238, 238, 120));
            }
        }

        // 2026-06-16 按照 8 列架构重构：第 1, 2, 3 列由代理独立绘制
        int col = index.column();
        if (col == 1 || col == 2 || col == 3) {
            // 这三列不调用默认 paint，完全自定义
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            if (col == 1) { // 状态列
                QModelIndex idx0 = index.model()->index(index.row(), 0);
                bool isPinned = idx0.data(IsLockedRole).toBool();
                bool isManaged = idx0.data(ManagedRole).toBool();
                bool isDir = idx0.data(TypeRole).toString() == "folder";
                double progress = idx0.data(RegistrationProgressRole).toDouble();

                QRect iconRect(option.rect.left() + (option.rect.width() - 16) / 2,
                               option.rect.top() + (option.rect.height() - 16) / 2, 16, 16);

                if (isPinned) {
                    UiHelper::getIcon("pin_vertical", QColor("#FF551C"), 16).paint(painter, iconRect);
                } else if (isDir && progress >= 0.0 && progress < 1.0) {
                    // --- 绘制进度环 (开箱即用代码) --- 
                    // 2026-07-xx 按照 Development_Plan 3.1：进度弧线完全通过数据库中的 0 和 1 标记值计算得出
                    painter->save(); 
                    painter->setRenderHint(QPainter::Antialiasing); 
                    painter->setPen(QPen(QColor(60, 60, 60, 180), 2)); 
                    painter->drawEllipse(iconRect.adjusted(1, 1, -1, -1)); 
                    QPen pPen(QColor("#3498db"), 2); 
                    pPen.setCapStyle(Qt::RoundCap); 
                    painter->setPen(pPen); 
                    int spanAngle = -qRound(progress * 360 * 16); 
                    painter->drawArc(iconRect.adjusted(1, 1, -1, -1), 90 * 16, spanAngle); 
                    painter->restore(); 
                } else if (isManaged || (isDir && progress >= 1.0)) {
                    UiHelper::getIcon("check_circle", QColor("#2ecc71"), 16).paint(painter, iconRect);
                }
            } else if (col == 2) { // 星级列
                // 2026-06-16 按照方案 20 纠偏：仅在选中或评分 > 0 时显示图标，减少视觉干扰
                int rating = index.model()->index(index.row(), 0).data(RatingRole).toInt();
                bool isSelected = option.state & QStyle::State_Selected;

                if (rating > 0 || isSelected) {
                    // 绘制“禁止”图标
                    QRect banRect(option.rect.left() + 5, option.rect.top() + (option.rect.height() - 16) / 2, 16, 16);
                    UiHelper::getIcon("no_color", QColor("#888888"), 16).paint(painter, banRect);

                    int starSize = 14;
                    int spacing = 1;
                    int startX = banRect.right() + 5;

                    if (rating > 0) {
                        QPixmap star = UiHelper::getPixmap("star_filled", QSize(starSize, starSize), QColor("#FECF0E"));
                        for (int i = 0; i < rating; ++i) {
                            painter->drawPixmap(startX + i * (starSize + spacing), 
                                                option.rect.top() + (option.rect.height() - starSize) / 2, star);
                        }
                        // 如果选中，补齐剩余的空心星
                        if (isSelected && rating < 5) {
                            QPixmap emptyStar = UiHelper::getPixmap("star", QSize(starSize, starSize), QColor("#555555"));
                            for (int i = rating; i < 5; ++i) {
                                painter->drawPixmap(startX + i * (starSize + spacing), 
                                                    option.rect.top() + (option.rect.height() - starSize) / 2, emptyStar);
                            }
                        }
                    } else {
                        // 仅选中但无评分，显示 5 颗空心星
                        QPixmap emptyStar = UiHelper::getPixmap("star", QSize(starSize, starSize), QColor("#555555"));
                        for (int i = 0; i < 5; ++i) {
                            painter->drawPixmap(startX + i * (starSize + spacing), 
                                                option.rect.top() + (option.rect.height() - starSize) / 2, emptyStar);
                        }
                    }
                }
            } else if (col == 3) { // 颜色列
                QString colorHex = index.model()->index(index.row(), 0).data(ColorRole).toString();
                if (!colorHex.isEmpty()) {
                    QColor c = UiHelper::parseColorName(colorHex);
                    if (c.isValid()) {
                        painter->setBrush(c);
                        painter->setPen(Qt::NoPen);
                        painter->drawEllipse(option.rect.center(), 6, 6);
                    }
                }
            }
            painter->restore();
        } else {
            QStyledItemDelegate::paint(painter, opt, index);
        }
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor);
        if (!lineEdit) return;
        QString value = lineEdit->text();
        if (value.isEmpty() || value == index.data(Qt::DisplayRole).toString()) return;

        // 2026-06-xx 架构解耦修复：物理重命名职责已彻底移至 Model 层的 setData。
        // Delegate 仅负责触发数据变更。这消除了“重复重命名”导致的静默失败 Bug。
        if (model->setData(index, value, Qt::EditRole)) {
            // 2026-xx-xx 按照用户要求：触发刷新信号
            // 物理修复：editor->parent() 返回 QObject*，需先转为 QWidget*
            QAbstractItemView* view = qobject_cast<QAbstractItemView*>(editor->parentWidget()->parentWidget());
            if (view) {
                // 对于 TreeView，editor 的 parent 是 viewport，viewport 的 parent 是 TreeView
                // 向上寻找 ContentPanel 以调用 onSelectionChanged
                QWidget* p = view->parentWidget();
                while (p) {
                    ContentPanel* cp = qobject_cast<ContentPanel*>(p);
                    if (cp) {
                        cp->onSelectionChanged();
                        break;
                    }
                    p = p->parentWidget();
                }
            }
        }
    }

public:
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override {
        // 2026-06-16 按照方案 20：交互逻辑闭环修正
        if (event->type() == QEvent::MouseButtonPress && index.column() == 2) {
            QAbstractItemView* view = qobject_cast<QAbstractItemView*>(const_cast<QWidget*>(option.widget));

            // 物理加固：未选中项严禁直接通过 Delegate 修改元数据
            // 2026-06-xx 稳健性增强：通过 View 获取实时的选中状态（检查整行是否被选中）
            bool isSelected = (option.state & QStyle::State_Selected);
            if (view && view->selectionModel()) {
                // 在 TreeView 中，我们通常关心的是当前行是否被选中
                isSelected = view->selectionModel()->isRowSelected(index.row(), index.parent());
            }
            if (!isSelected) return false;

            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            
            // 精准 Hitbox：禁止图标
            QRect banHitbox(option.rect.left() + 5, option.rect.top() + (option.rect.height() - 16)/2, 16, 16);
            
            if (banHitbox.contains(me->pos())) {
                // 核心意图：统一由模型 setData 触发持久化，消除双写冲突
                model->setData(index.model()->index(index.row(), 0), 0, RatingRole);
                return true;
            }

            // 处理点击星级评分
            int starSize = 14;
            int spacing = 1;
            int startX = option.rect.left() + 5 + 16 + 5; 
            for (int i = 0; i < 5; ++i) {
                QRect starRect(startX + i * (starSize + spacing), option.rect.top() + (option.rect.height() - starSize) / 2, starSize, starSize);
                if (starRect.contains(me->pos())) {
                    model->setData(index.model()->index(index.row(), 0), i + 1, RatingRole);
                    return true;
                }
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        return QStyledItemDelegate::createEditor(parent, option, index);
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        QString value = index.model()->data(index, Qt::EditRole).toString();
        QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor);
        if (!lineEdit) return;

        lineEdit->setText(value);

        // 2026-xx-xx 按照用户要求：仅选中不含扩展名的部分
        // 物理修复：使用 QTimer 确保在 Qt 默认 selectAll 之后执行，防止逻辑被覆盖
        bool isFolder = (index.data(TypeRole).toString() == "folder" || index.data(TypeRole).toString() == "category");
        QTimer::singleShot(0, lineEdit, [lineEdit, value, isFolder]() {
            if (!lineEdit) return;
            if (isFolder) {
                lineEdit->selectAll();
            } else {
                int lastDot = value.lastIndexOf('.');
                if (lastDot > 0) {
                    lineEdit->setSelection(0, lastDot);
                } else {
                    lineEdit->selectAll();
                }
            }
        });
    }

private:
    bool m_showStatus;
};

} // namespace ArcMeta
