#include "ThumbnailDelegate.h"
#include <QPainter>
#include <QPainterPath>
#include <QHelpEvent>
#include "ToolTipOverlay.h"
#include <QIcon>
#include <QPixmap>
#include <QStyleOptionViewItem>
#include <QFileInfo>
#include <QMouseEvent>
#include <QLineEdit>
#include <QTimer>
#include <QAbstractItemView>
#include <QTextLayout>
#include <QTextOption>
#include <QCache>
#include "UiHelper.h"
#include "JustifiedView.h"
#include "../core/ModelContract.h"

namespace FERREX {

ThumbnailDelegate::ThumbnailDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void ThumbnailDelegate::setHasThumbnailRole(int role) { m_hasThumbnailRole = role; }
void ThumbnailDelegate::setPathRole(int role) { m_pathRole = role; }
void ThumbnailDelegate::setManagedRole(int role) { m_managedRole = role; }
void ThumbnailDelegate::setIsEmptyRole(int role) { m_isEmptyRole = role; }

ThumbnailDelegate::Metrics ThumbnailDelegate::calculateMetrics(const QStyleOptionViewItem& option) const {
    Metrics m;
    const int textHeight = 36;
    const int gap = 6; // 卡片与文件名的紧凑间隙

    // 底部预留高度调整：文件名高度 + 间距 + 底部内边距补偿(3px)
    m.cardRect = option.rect.adjusted(3, 3, -3, -(textHeight + gap + 3));
    
    // 文件名框紧贴卡片底部下方 gap 像素的位置
    m.textRect = QRect(option.rect.left() + 3,
                       m.cardRect.bottom() + gap,
                       option.rect.width() - 6,
                       textHeight);
    
    m.banRect = QRect();

    return m;
}

struct LayoutLines {
    QString line1;
    QString line2;
};
static QCache<QString, LayoutLines> s_layoutLinesCache(2000);

void ThumbnailDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    FerrexItemPayload payload = index.data(Qt::UserRole).value<FerrexItemPayload>();

    Metrics m = calculateMetrics(option);
    bool isSelected = (option.state & QStyle::State_Selected);

    // 强类型契约代替 QVariant property 属性
    const JustifiedView* jv = qobject_cast<const JustifiedView*>(option.widget);
    bool isGrid = jv ? (jv->layoutMode() == JustifiedView::GridMode) : false;

    QVariant decoData = index.data(Qt::DecorationRole);
    
    QPixmap thumb;
    bool hasValidThumb = false;

    if (decoData.canConvert<QPixmap>()) {
        thumb = decoData.value<QPixmap>();
        if (!thumb.isNull()) {
            hasValidThumb = true;
        }
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);

    // ① 绘制内容与裁剪 (Cover 模式)
    painter->save();
    QPainterPath clipPath;
    clipPath.addRoundedRect(m.cardRect, 6, 6);
    painter->setClipPath(clipPath);

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor("#2d2d2d"));
    painter->drawRect(m.cardRect);

    if (hasValidThumb) {
        QPixmap scaled = thumb.scaled(m.cardRect.size(), 
                                      isGrid ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding, 
                                      Qt::SmoothTransformation);
        int x = m.cardRect.center().x() - scaled.width() / 2;
        int y = m.cardRect.center().y() - scaled.height() / 2;
        
        painter->drawPixmap(x, y, scaled);
    } else {
        QIcon icon = qvariant_cast<QIcon>(decoData);
        if (!icon.isNull()) {
            painter->setOpacity(1.0);
            int iconSize = qMin(m.cardRect.width(), m.cardRect.height()) * 0.55;
            QRect iconRect(m.cardRect.center().x() - iconSize / 2,
                           m.cardRect.center().y() - iconSize / 2,
                           iconSize, iconSize);
            
            icon.paint(painter, iconRect);
            painter->setOpacity(1.0);
        }
    }
    painter->restore();

    // ③ 绘制卡片边框
    painter->save();
    if (isSelected) {
        painter->setPen(QPen(QColor("#3498db"), 3));
    } else {
        painter->setPen(QPen(QColor("#4a4a4a"), 1));
    }
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(m.cardRect, 6, 6);
    painter->restore();

    // 状态位图标绘制
    if (payload.isManaged) {
        QRect statusRect(m.cardRect.right() - 22, m.cardRect.top() + 8, 16, 16);
        UiHelper::getIcon("check_circle", QColor("#2ecc71"), 16).paint(painter, statusRect);
    }

    // ③ 文件名（卡片下方）
    painter->save();
    painter->setPen(isSelected ? QColor("#3498db") : QColor("#EEEEEE"));

    if (!isSelected && !payload.isManaged) {
        painter->setPen(QColor(238, 238, 238, 120));
    }

    QFont textFont = painter->font();
    textFont.setPointSize(8);
    painter->setFont(textFont);

    int textWidth = m.textRect.width() - 8;
    int fontHeight = option.fontMetrics.height();

    // 缓存判断：避免高频 Heap 内存分配并适应重命名刷新
    QString cacheKey = QString("%1_%2_%3").arg(payload.key).arg(payload.name).arg(textWidth);
    LayoutLines* cached = s_layoutLinesCache.object(cacheKey);
    
    if (!cached) {
        QTextLayout textLayout(payload.displayName, painter->font());
        QTextOption textOption;
        textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        textOption.setAlignment(Qt::AlignCenter);
        textLayout.setTextOption(textOption);

        textLayout.beginLayout();
        int lineCount = 0;
        QString line1, line2;

        while (true) {
            QTextLine line = textLayout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(textWidth);
            lineCount++;

            if (lineCount == 1) {
                int start = line.textStart();
                int len = line.textLength();
                line1 = payload.displayName.mid(start, len);
            } else if (lineCount == 2) {
                QTextLine nextLine = textLayout.createLine();
                if (nextLine.isValid()) {
                    int start = line.textStart();
                    QString remainingText = payload.displayName.mid(start);
                    line2 = option.fontMetrics.elidedText(remainingText, Qt::ElideMiddle, textWidth);
                } else {
                    int start = line.textStart();
                    int len = line.textLength();
                    line2 = payload.displayName.mid(start, len);
                }
                break;
            }
        }
        textLayout.endLayout();
        
        cached = new LayoutLines{line1, line2};
        s_layoutLinesCache.insert(cacheKey, cached);
    }

    // 物理无分配渲染
    if (!cached->line1.isEmpty()) {
        QRect lineRect(m.textRect.left() + 4, m.textRect.top(), textWidth, fontHeight);
        painter->drawText(lineRect, Qt::AlignCenter, cached->line1);
    }
    if (!cached->line2.isEmpty()) {
        QRect lineRect(m.textRect.left() + 4, m.textRect.top() + fontHeight, textWidth, fontHeight);
        painter->drawText(lineRect, Qt::AlignCenter, cached->line2);
    }

    painter->restore();

    // ④ 空文件夹特殊标记
    if (!isSelected && payload.isEmptyFolder) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(QColor("#41F2F2"), 1, Qt::DashLine));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(m.cardRect, 6, 6);
        painter->restore();
    }

    painter->restore();
}

QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    return QStyledItemDelegate::sizeHint(option, index);
}

QWidget* ThumbnailDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
    if (editor) {
        editor->setStyleSheet(
            "background-color: #2D2D2D; color: white; selection-background-color: #3498db; "
            "border: 1px solid #3498db; border-radius: 4px; padding: 0 4px;"
        );
    }
    return editor;
}

void ThumbnailDelegate::updateEditorGeometry(QWidget* editor,
                                              const QStyleOptionViewItem& option,
                                              const QModelIndex& /*index*/) const {
    Metrics m = calculateMetrics(option);
    // 重命名编辑框位置微调，完美契合没有星级后的新 textRect 布局 [1]
    editor->setGeometry(m.textRect.adjusted(1, 5, -1, -5));
}

void ThumbnailDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
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

bool ThumbnailDelegate::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = reinterpret_cast<QKeyEvent*>(event); 
        QLineEdit* editor = qobject_cast<QLineEdit*>(obj); 
        if (editor) { 
            switch (keyEvent->key()) { 
                case Qt::Key_Left: 
                case Qt::Key_Right: 
                case Qt::Key_Up: 
                case Qt::Key_Down: 
                case Qt::Key_Home: 
                case Qt::Key_End: 
                    keyEvent->accept(); 
                    return false; 
                default: 
                    break; 
            } 
        } 
    } 
    return QStyledItemDelegate::eventFilter(obj, event); 
} 

bool ThumbnailDelegate::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) {
    // 2026-07-xx 重构：星级已不再使用，不拦截 any 鼠标按下事件修改星级，直接走基类逻辑 [1]
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace FERREX