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
#include "UiHelper.h"

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

    m.cardRect = option.rect.adjusted(3, 3, -3, -(textHeight + gap + 3));
    
    m.textRect = QRect(option.rect.left() + 3,
                       m.cardRect.bottom() + gap,
                       option.rect.width() - 6,
                       textHeight);
    
    m.banRect = QRect();

    return m;
}

void ThumbnailDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    Metrics m = calculateMetrics(option);
    bool isSelected = (option.state & QStyle::State_Selected);
    bool isGrid = option.widget ? option.widget->property("gridMode").toBool() : false;

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

    painter->save();
    if (isSelected) {
        painter->setPen(QPen(QColor("#3498db"), 3));
    } else {
        painter->setPen(QPen(QColor("#4a4a4a"), 1));
    }
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(m.cardRect, 6, 6);
    painter->restore();

    if (m_managedRole != -1) {
        bool isManaged = index.data(m_managedRole).toBool();
        if (isManaged) {
            QRect statusRect(m.cardRect.right() - 22, m.cardRect.top() + 8, 16, 16);
            UiHelper::getIcon("check_circle", QColor("#2ecc71"), 16).paint(painter, statusRect);
        }
    }

    if (m_pathRole != -1) {
        QString path = index.data(m_pathRole).toString();
        QFileInfo info(path);
        QString ext = info.isDir() ? "DIR" : info.suffix().toUpper();
        if (!ext.isEmpty()) {
            QColor badgeColor = UiHelper::getExtensionColor(ext);

            if (!hasValidThumb) {
                badgeColor.setAlpha(160);
            }

            QRect extRect(m.cardRect.left() + 8, m.cardRect.top() + 8, 36, 18);
            painter->setPen(Qt::NoPen);
            painter->setBrush(badgeColor);
            painter->drawRoundedRect(extRect, 2, 2);
            painter->setPen(hasValidThumb ? QColor("#FFFFFF") : QColor(255, 255, 255, 180));
            QFont extFont = painter->font(); extFont.setPointSize(8); extFont.setBold(true);
            painter->setFont(extFont);
            painter->drawText(extRect, Qt::AlignCenter, ext);
        }
    }

    painter->save();
    QString name = index.data(Qt::DisplayRole).toString();
    painter->setPen(isSelected ? QColor("#3498db") : QColor("#EEEEEE"));

    if (m_managedRole != -1 && !isSelected && !index.data(m_managedRole).toBool()) {
        painter->setPen(QColor(238, 238, 238, 120));
    }

    QFont textFont = painter->font();
    textFont.setPointSize(8);
    painter->setFont(textFont);

    QString displayName = name;
    displayName.replace("_", "_\u200B");
    displayName.replace(".", ".\u200B");

    QTextLayout textLayout(displayName, painter->font());
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    textOption.setAlignment(Qt::AlignCenter);
    textLayout.setTextOption(textOption);

    textLayout.beginLayout();
    int lineCount = 0;
    int textWidth = m.textRect.width() - 8;
    int currentY = m.textRect.top();
    int fontHeight = option.fontMetrics.height();

    QList<RenderLine> linesToRender;

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
            linesToRender.append({displayName.mid(start, len), currentY});
            currentY += fontHeight;
        } else if (lineCount == 2) {
            QTextLine nextLine = textLayout.createLine();
            if (nextLine.isValid()) {
                int start = line.textStart();
                QString remainingText = displayName.mid(start);
                QString elidedRemaining = option.fontMetrics.elidedText(remainingText, Qt::ElideMiddle, textWidth);
                linesToRender.append({elidedRemaining, currentY});
            } else {
                int start = line.textStart();
                int len = line.textLength();
                linesToRender.append({displayName.mid(start, len), currentY});
            }
            break;
        }
    }
    textLayout.endLayout();

    for (const auto& rLine : linesToRender) {
        QRect lineRect(m.textRect.left() + 4, rLine.y, textWidth, fontHeight);
        painter->drawText(lineRect, Qt::AlignCenter, rLine.text);
    }

    painter->restore();

    if (!isSelected && m_isEmptyRole != -1) {
        if (index.data(m_isEmptyRole).toBool()) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(QColor("#41F2F2"), 1, Qt::DashLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(m.cardRect, 6, 6);
            painter->restore();
        }
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
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace FERREX