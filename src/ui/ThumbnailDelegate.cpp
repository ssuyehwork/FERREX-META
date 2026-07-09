#include "ThumbnailDelegate.h"
#include <QPainter>
#include <QPainterPath>
#include <QIcon>
#include <QPixmap>
#include <QStyleOptionViewItem>
#include <QFileInfo>
#include <QMouseEvent>
#include <QLineEdit>
#include <QTimer>
#include <QAbstractItemView>
#include "UiHelper.h"

namespace FERREX {

ThumbnailDelegate::ThumbnailDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void ThumbnailDelegate::setHasThumbnailRole(int role) { m_hasThumbnailRole = role; }
void ThumbnailDelegate::setRatingRole(int role) { m_ratingRole = role; }
void ThumbnailDelegate::setPathRole(int role) { m_pathRole = role; }
void ThumbnailDelegate::setPinnedRole(int role) { m_pinnedRole = role; }
void ThumbnailDelegate::setManagedRole(int role) { m_managedRole = role; }
void ThumbnailDelegate::setTypeRole(int role) { m_typeRole = role; }
void ThumbnailDelegate::setIsEmptyRole(int role) { m_isEmptyRole = role; }
void ThumbnailDelegate::setColorRole(int role) { m_colorRole = role; }

ThumbnailDelegate::Metrics ThumbnailDelegate::calculateMetrics(const QStyleOptionViewItem& option) const {
    Metrics m;
    const int textHeight = 36;
    const int gap = 6; // 卡片与文件名的紧凑间隙

    m.ratingH = 0; // 彻底停用星级占位

    // 底部预留高度调整：文件名高度 + 间距 + 底部内边距补偿(3px)
    m.cardRect = option.rect.adjusted(3, 3, -3, -(textHeight + gap + 3));
    
    m.ratingY = 0;

    // 文件名框紧贴卡片底部下方 gap 像素的位置
    m.textRect = QRect(option.rect.left() + 3,
                       m.cardRect.bottom() + gap,
                       option.rect.width() - 6,
                       textHeight);
    
    // 初始化无用变量，防止其他潜在编译未定义警告
    m.starSize = 0;
    m.starSpacing = 0;
    m.banRect = QRect();
    m.starsStartX = 0;

    return m;
}

void ThumbnailDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    Metrics m = calculateMetrics(option);
    bool isSelected = (option.state & QStyle::State_Selected);
    bool isGrid = option.widget ? option.widget->property("gridMode").toBool() : false;

    int thumbStatus = index.data(m_hasThumbnailRole).toInt(); // 0=不支持, 1=就绪, 2=加载中
    QVariant decoData = index.data(Qt::DecorationRole);

    QPixmap thumb;
    bool hasValidThumb = false;

    // 只要 decoData 能转换为 QPixmap，不管当前状态是 1（就绪）还是 2（加载中），
    // 都认为这属于可用的、高质量的缩略图物理资产（包括上一代多态拉伸暂位图），优先提取！
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

    // 关键路径重构：优先绘制缩略图
    if (hasValidThumb) {
        // 缩略图平滑拉伸并充满卡片（100% Cover/Contain）
        QPixmap scaled = thumb.scaled(m.cardRect.size(), 
                                      isGrid ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding, 
                                      Qt::SmoothTransformation);
        int x = m.cardRect.center().x() - scaled.width() / 2;
        int y = m.cardRect.center().y() - scaled.height() / 2;

        // 异步加载升级期间，赋予平滑淡入感
        if (thumbStatus == 2) {
            painter->setOpacity(0.95); // 95% 透明度，给高精大图的替换预留非常自然的过渡
        }
        painter->drawPixmap(x, y, scaled);
        if (thumbStatus == 2) {
            painter->setOpacity(1.0);
        }
    } else {
        // 仅当 Pixmap 为空且判定无法加载缩略图、或提取彻底失败后，才作为最后的手段去绘制默认的文件类型关联图标
        QIcon icon = qvariant_cast<QIcon>(decoData);
        if (!icon.isNull()) {
            if (thumbStatus == 2) {
                painter->setOpacity(0.25); // 降低到 25% 灰度透明，作为极静默的隐式过渡
            } else {
                painter->setOpacity(0.7);  // 常规不支持缩略图的项，也采取略显高级的 70% 亮度
            }
            
            int iconSize = qMin(m.cardRect.width(), m.cardRect.height()) * 0.6;
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
    if (m_pinnedRole != -1 && m_managedRole != -1) {
        bool isPinned = index.data(m_pinnedRole).toBool();
        bool isManaged = index.data(m_managedRole).toBool();
        if (isPinned || isManaged) {
            QRect statusRect(m.cardRect.right() - 22, m.cardRect.top() + 8, 16, 16);
            if (isPinned) {
                UiHelper::getIcon("pin_vertical", QColor("#FF551C"), 16).paint(painter, statusRect);
            } else {
                UiHelper::getIcon("check_circle", QColor("#2ecc71"), 16).paint(painter, statusRect);
            }
        }
    }

    // 扩展名角标
    if (m_pathRole != -1) {
        QString path = index.data(m_pathRole).toString();
        QFileInfo info(path);
        QString ext = info.isDir() ? "DIR" : info.suffix().toUpper();
        if (ext.isEmpty()) ext = "FILE";
        QColor badgeColor = UiHelper::getExtensionColor(ext);

        if (thumbStatus != 1) {
            badgeColor.setAlpha(160);
        }

        QRect extRect(m.cardRect.left() + 8, m.cardRect.top() + 8, 36, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(badgeColor);
        painter->drawRoundedRect(extRect, 2, 2);
        painter->setPen(thumbStatus == 1 ? QColor("#FFFFFF") : QColor(255, 255, 255, 180));
        QFont extFont = painter->font(); extFont.setPointSize(8); extFont.setBold(true);
        painter->setFont(extFont);
        painter->drawText(extRect, Qt::AlignCenter, ext);
    }

    // [已停用] 星级渲染逻辑：星级已不再使用，此处直接跳过以节省 CPU 消耗

    // ③ 文件名（卡片下方）
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

    painter->drawText(m.textRect.adjusted(4, 0, -4, 0), Qt::AlignCenter | Qt::TextWordWrap,
        option.fontMetrics.elidedText(displayName, Qt::ElideMiddle, m.textRect.width() * 2));
    painter->restore();

    // ④ 空文件夹特殊标记
    if (!isSelected && m_isEmptyRole != -1 && m_typeRole != -1) {
        if (index.data(m_typeRole).toString() == "folder" && index.data(m_isEmptyRole).toBool()) {
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
    // 2026-07-xx 重构：星级已不再使用，不拦截任何鼠标按下事件修改星级，直接走基类逻辑 [1]
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace FERREX