#include "ThumbnailDelegate.h"
#include "ContentPanel.h"
#include "../meta/MetadataManager.h"
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
#include <QFile>
#include "UiHelper.h"
#include "ToolTipOverlay.h"

namespace ArcMeta {

ThumbnailDelegate::ThumbnailDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void ThumbnailDelegate::setHasThumbnailRole(int role) { m_hasThumbnailRole = role; }
void ThumbnailDelegate::setRatingRole(int role) { m_ratingRole = role; }
void ThumbnailDelegate::setPathRole(int role) { m_pathRole = role; }
void ThumbnailDelegate::setPinnedRole(int role) { m_pinnedRole = role; }
void ThumbnailDelegate::setManagedRole(int role) { m_managedRole = role; }
void ThumbnailDelegate::setTypeRole(int role) { m_typeRole = role; }
void ThumbnailDelegate::setIsEmptyRole(int role) { m_isEmptyRole = role; }
void ThumbnailDelegate::setColorRole(int role) { m_colorRole = role; }
void ThumbnailDelegate::setRegistrationProgressRole(int role) { m_registrationProgressRole = role; }

ThumbnailDelegate::Metrics ThumbnailDelegate::calculateMetrics(const QStyleOptionViewItem& option) const {
    Metrics m;
    const int textHeight = 36;
    const int ratingHeight = 24;
    const int gap = 4;

    m.ratingH = ratingHeight;
    // 底部预留高度增加，包含星级区域和间隙
    m.cardRect = option.rect.adjusted(3, 3, -3, -(textHeight + m.ratingH + gap + 3));
    
    // 星级坐标脱离卡片范围
    m.ratingY = m.cardRect.bottom() + gap;

    m.textRect = QRect(option.rect.left() + 3,
                       m.ratingY + m.ratingH - 5,
                       option.rect.width() - 6,
                       textHeight);
    
    int zoom = option.decorationSize.width(); // 物理缩放级别

    m.starSize = 22;
    m.starSpacing = -4; // 2026-06-08 优化：默认间距调紧
    int banW = 14;

    // 2026-06-08 按照调试增强版 V2 优化：实现“动态比例星级”
    // 虽然底限是 96，但在接近极限 (100) 时提前缩小星级，确保视觉紧凑感
    if (zoom < 100) {
        m.starSize = 18; 
        m.starSpacing = -4;
        banW = 12;
    }

    int banGap = 2; // 保持间隙一致性
    int infoTotalW = banW + banGap + (5 * m.starSize) + (4 * m.starSpacing);
    int infoStartX = m.cardRect.left() + (m.cardRect.width() - infoTotalW) / 2;
    
    m.banRect = QRect(infoStartX, m.ratingY + (m.ratingH - banW) / 2, banW, banW);
    m.starsStartX = infoStartX + banW + banGap;

    return m;
}

void ThumbnailDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    Metrics m = calculateMetrics(option);
    bool isSelected = (option.state & QStyle::State_Selected);

    bool hasThumb = index.data(m_hasThumbnailRole).toBool();
    QVariant decoData = index.data(Qt::DecorationRole);
    QPixmap thumb;
    if (decoData.canConvert<QPixmap>()) {
        thumb = decoData.value<QPixmap>();
    } else if (decoData.canConvert<QIcon>()) {
        QIcon icon = decoData.value<QIcon>();
        if (!icon.isNull()) {
            thumb = icon.pixmap(m.cardRect.size());
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

    // 绘制卡片背景
    painter->setPen(Qt::NoPen);

    // 2026-11-14 执行第三步：图形文件等待缩略图时，绘制轻量灰色占位背景
    bool isWaitingThumb = false;
    if (m_pathRole != -1 && thumb.isNull()) {
        QString path = index.data(m_pathRole).toString();
        QString ext = QFileInfo(path).suffix().toLower();
        if (UiHelper::isGraphicsFile(ext) || ext == "svg") {
            isWaitingThumb = true;
        }
    }

    painter->setBrush(isWaitingThumb ? QColor("#3A3A3A") : QColor("#2d2d2d"));
    painter->drawRect(m.cardRect);

    if (hasThumb) {
        if (!thumb.isNull()) {
            QPixmap scaled = thumb.scaled(m.cardRect.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            int x = m.cardRect.center().x() - scaled.width() / 2;
            int y = m.cardRect.center().y() - scaled.height() / 2;
            painter->drawPixmap(x, y, scaled);
        }
        // 2026-xx-xx 按照 Plan-114：当 hasThumb 为 true 时，即便 thumb 为空（加载中），
        // 也不再回退到 60% 缩小模式，而是保持占位状态，确保视觉比例统一。
    } else {
        QIcon icon = qvariant_cast<QIcon>(decoData);
        if (!icon.isNull()) {
            // 针对普通文件（非图形/视频），保持 60% 比例缩小的图标绘制逻辑
            int iconSize = qMin(m.cardRect.width(), m.cardRect.height()) * 0.6;
            QRect iconRect(m.cardRect.center().x() - iconSize / 2,
                           m.cardRect.center().y() - iconSize / 2,
                           iconSize, iconSize);
            icon.paint(painter, iconRect);
        }
    }
    painter->restore();

    // ③ 绘制卡片边框 (选中 3px 蓝色，未选中 1px #4a4a4a)
    painter->save();
    if (isSelected) {
        painter->setPen(QPen(QColor("#3498db"), 3));
    } else {
        painter->setPen(QPen(QColor("#4a4a4a"), 1));
    }
    painter->setBrush(Qt::NoBrush);
    // 抵消画笔宽度导致的一半粗细落在矩形外的问题
    painter->drawRoundedRect(m.cardRect, 6, 6);
    painter->restore();

    // [新增] 状态位图标绘制 (置顶 vs. 进度环 vs. 已录入 互斥)
    if (m_pinnedRole != -1 && m_managedRole != -1) {
        bool isPinned = index.data(m_pinnedRole).toBool();
        bool isManaged = index.data(m_managedRole).toBool();
        bool isDir = index.data(m_typeRole).toString() == "folder";
        double progress = (m_registrationProgressRole != -1) ? index.data(m_registrationProgressRole).toDouble() : -1.0;

        QRect statusRect(m.cardRect.right() - 22, m.cardRect.top() + 8, 16, 16);
        if (isPinned) {
            UiHelper::getIcon("pin_vertical", QColor("#FF551C"), 16).paint(painter, statusRect);
        } else if (isDir && progress >= 0.0 && progress < 1.0) {
            // --- 绘制进度环 (开箱即用代码) --- 
            // 2026-07-xx 按照 Development_Plan 3.1：进度弧线完全通过数据库中的 0 和 1 标记值计算得出
            painter->save(); 
            painter->setRenderHint(QPainter::Antialiasing); 
             
            // 1. 底环 
            painter->setPen(QPen(QColor(60, 60, 60, 180), 2)); 
            painter->drawEllipse(statusRect.adjusted(1, 1, -1, -1)); 
             
            // 2. 进度弧 (品牌蓝 #3498db) 
            QPen pPen(QColor("#3498db"), 2); 
            pPen.setCapStyle(Qt::RoundCap); 
            painter->setPen(pPen); 
             
            int spanAngle = -qRound(progress * 360 * 16); // 逆时针计算 
            painter->drawArc(statusRect.adjusted(1, 1, -1, -1), 90 * 16, spanAngle); 
            painter->restore(); 
        } else if (isManaged || (isDir && progress >= 1.0)) {
            UiHelper::getIcon("check_circle", QColor("#2ecc71"), 16).paint(painter, statusRect);
        }
    }

    // [新增] 扩展名角标
    if (m_pathRole != -1) {
        QString type = (m_typeRole != -1) ? index.data(m_typeRole).toString() : "";
        QString path = index.data(m_pathRole).toString();
        QFileInfo info(path);
        QString ext;
        if (type == "category" || type == "folder") {
            ext = "DIR"; // 2026-06-xx 物理校准：分类与文件夹均强制显示为 "DIR" 徽章，增强视觉一致性
        } else {
            ext = info.isDir() ? "DIR" : info.suffix().toUpper();
        }
        if (ext.isEmpty()) ext = "FILE";
        QColor badgeColor = UiHelper::getExtensionColor(ext);

        // 2026-06-xx 物理优化：针对无缩略图项应用半透明角标，减少视觉冲击
        if (!hasThumb) {
            badgeColor.setAlpha(160);
        }

        QRect extRect(m.cardRect.left() + 8, m.cardRect.top() + 8, 36, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(badgeColor);
        painter->drawRoundedRect(extRect, 2, 2);
        painter->setPen(hasThumb ? QColor("#FFFFFF") : QColor(255, 255, 255, 180));
        QFont extFont = painter->font(); extFont.setPointSize(8); extFont.setBold(true);
        painter->setFont(extFont);
        painter->drawText(extRect, Qt::AlignCenter, ext);
    }

    // [新增] 评级星级 (现在绘制在裁剪区外，处于卡片与文件名的间隙处)
    if (m_ratingRole != -1) {
        int rating = index.data(m_ratingRole).toInt();
        QString colorStr = (m_colorRole != -1) ? index.data(m_colorRole).toString() : "";
        
        // 2026-06-xx 逻辑重构：彩色胶囊背景独立于星级显示
        if (!colorStr.isEmpty()) {
            QColor bgColor = UiHelper::parseColorName(colorStr);
            if (bgColor.isValid()) {
                painter->save();
                painter->setBrush(bgColor);
                painter->setPen(Qt::NoPen);
                QRect totalRect = m.banRect.united(m.starRect(4));
                painter->drawRoundedRect(totalRect.adjusted(-4, -1, 4, 1), 4, 4);
                painter->restore();
            }
        }

        bool shouldShowRating = (rating > 0) || isSelected;
        if (shouldShowRating) {
            QColor bgColor = colorStr.isEmpty() ? QColor(0,0,0,0) : UiHelper::parseColorName(colorStr);
            
            // 2026-06-xx 物理修复：采用感知亮度对比色计算
            double luminance = 0.0;
            if (bgColor.isValid() && bgColor.alpha() > 0) {
                luminance = (0.299 * bgColor.red() + 0.587 * bgColor.green() + 0.114 * bgColor.blue()) / 255.0;
            }

            QColor starColor, emptyStarColor;
            if (colorStr.isEmpty()) {
                starColor      = QColor("#CCCCCC");
                emptyStarColor = QColor("#888888");
            } else if (luminance < 0.5) {
                starColor      = QColor("#FFFFFF");
                emptyStarColor = QColor(255, 255, 255, 160);
            } else {
                starColor      = QColor("#1A1A1A");
                emptyStarColor = QColor(0, 0, 0, 140);
            }

            UiHelper::getIcon("no_color", starColor, m.banRect.width()).paint(painter, m.banRect);
            QPixmap filledStar = UiHelper::getPixmap("star_filled", QSize(m.starSize, m.starSize), starColor);
            QPixmap emptyStar = UiHelper::getPixmap("star", QSize(m.starSize, m.starSize), emptyStarColor);
            for (int i = 0; i < 5; ++i) {
                painter->drawPixmap(m.starRect(i), (i < rating) ? filledStar : emptyStar);
            }
        }
    }


    // ③ 文件名（卡片下方）
    painter->save();
    QString name = index.data(Qt::DisplayRole).toString();
    painter->setPen(isSelected ? QColor("#3498db") : QColor("#EEEEEE"));

    // 2026-06-xx 物理同步：针对未录入项目应用半透明效果
    if (m_managedRole != -1 && !isSelected && !index.data(m_managedRole).toBool()) {
        painter->setPen(QColor(238, 238, 238, 120));
    }

    QFont textFont = painter->font();
    textFont.setPointSize(8);
    painter->setFont(textFont);

    // 2026-06-05 按照用户要求：限制内容面板卡片文件名最多显示2行，超出用"..."省略
    auto elidedName = [](const QString& name, const QFontMetrics& fm, int width) -> QString {
        QString line1 = fm.elidedText(name, Qt::ElideRight, width);
        if (line1 == name) return name; // 单行就够了
        
        // 尝试找到第一行断点，计算第二行
        int breakPos = 0;
        for (int i = 1; i <= name.length(); ++i) {
            if (fm.horizontalAdvance(name.left(i)) > width) {
                breakPos = i - 1;
                break;
            }
        }
        if (breakPos <= 0) return line1;
        
        QString remaining = name.mid(breakPos);
        QString line2 = fm.elidedText(remaining, Qt::ElideRight, width);
        return name.left(breakPos) + "\n" + line2;
    };

    QString displayName = elidedName(name, option.fontMetrics, m.textRect.width() - 8);
    painter->drawText(m.textRect.adjusted(4, 0, -4, 0), Qt::AlignCenter | Qt::TextWordWrap, displayName);
    painter->restore();

    // ④ [新增] 空文件夹特殊标记 (ContentPanel 移植)
    // 物理优化：如果已选中，则不显示虚线，避免与 3px 蓝色边框冲突
    if (!isSelected && m_isEmptyRole != -1 && m_typeRole != -1) {
        if (index.data(m_typeRole).toString() == "folder" && index.data(m_isEmptyRole).toBool()) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(QColor("#41F2F2"), 1, Qt::DashLine));
            painter->setBrush(Qt::NoBrush); // 2026-06-xx 物理优化：确保空文件夹标记为全透明
            painter->drawRoundedRect(m.cardRect, 6, 6);
            painter->restore();
        }
    }

    painter->restore(); // 物理还原：释放 paint 函数开始处的 painter->save()
}

QSize ThumbnailDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    return QStyledItemDelegate::sizeHint(option, index);
}

QWidget* ThumbnailDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
    if (editor) {
        // 按照用户要求：修改为项目标准蓝 (#3498db)
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
    // 修正编辑器位置，使其与文件名文字区域对齐并留出少量边距
    // 高度降低 2 像素：通过上下各收缩 1 像素实现 (从 4 变 5)
    editor->setGeometry(m.textRect.adjusted(1, 5, -1, -5));
}

void ThumbnailDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    QString value = index.model()->data(index, Qt::EditRole).toString();
    QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor); 
    if (lineEdit) {
        lineEdit->setText(value); 

        // 2026-xx-xx 按照要求：如果是文件夹或分类，全选；如果是文件，仅选中名称部分
        // 使用 QTimer::singleShot 确保在 Qt 内部默认全选逻辑之后执行，彻底解决失效问题
        bool isFolder = (index.data(m_typeRole).toString() == "folder" || index.data(m_typeRole).toString() == "category");
        
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
}

void ThumbnailDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const {
    QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor);
    if (!lineEdit) return;
    QString value = lineEdit->text();
    if (value.isEmpty() || value == index.data(Qt::DisplayRole).toString()) return;

    // 2026-06-xx 架构解耦修复：物理重命名职责已彻底移至 Model 层的 setData。
    // Delegate 仅负责触发数据变更。这消除了“重复重命名”导致的静默失败 Bug。
    if (model->setData(index, value, Qt::EditRole)) {
        // 2026-xx-xx 按照用户要求：触发刷新信号，驱动元数据面板同步
        // 物理修复：编辑器挂在 viewport 上，需多跳一级 parent
        QAbstractItemView* view = qobject_cast<QAbstractItemView*>(editor->parentWidget()->parentWidget());
        if (view) {
            // 向上寻找 ContentPanel
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

bool ThumbnailDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view, 
                                const QStyleOptionViewItem& option, const QModelIndex& index) {
    Metrics m = calculateMetrics(option);
    QRect statusRect(m.cardRect.right() - 22, m.cardRect.top() + 8, 16, 16);

    if (statusRect.contains(event->pos())) {
        double p = (m_registrationProgressRole != -1) ? index.data(m_registrationProgressRole).toDouble() : -1.0;
        if (p >= 0.0) {
            // 2026-07-xx 按照 Plan-65：悬停触发，timeout = 0
            ToolTipOverlay::instance()->showText(event->globalPos(), 
                QString("登记进度: %1%").arg(qRound(p * 100)), 0);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

bool ThumbnailDelegate::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) {
    if (m_ratingRole != -1 && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* mEvent = reinterpret_cast<QMouseEvent*>(event);
        if (mEvent->button() == Qt::LeftButton) {
            QAbstractItemView* view = qobject_cast<QAbstractItemView*>(const_cast<QWidget*>(option.widget));

            // 物理加固：未选中项严禁直接通过 Delegate 修改元数据
            // 2026-06-xx 稳健性增强：通过 View 获取实时的选中状态
            bool isSelected = (option.state & QStyle::State_Selected);
            if (view && view->selectionModel()) {
                isSelected = view->selectionModel()->isSelected(index);
            }
            if (!isSelected) return false;

            // 2026-06-xx 物理对齐：补全 decorationSize，确保计算出的星级区域与绘制时完全对齐
            QStyleOptionViewItem opt = option;
            if (opt.decorationSize.width() <= 0 && view) opt.decorationSize = view->iconSize();
            Metrics m = calculateMetrics(opt);

            // 1. 区域判定
            bool isBanHit = m.banRect.contains(mEvent->pos());
            int hitStar = -1;
            for (int i = 0; i < 5; ++i) {
                if (m.starRect(i).contains(mEvent->pos())) {
                    hitStar = i + 1;
                    break;
                }
            }

            if (isBanHit || hitStar != -1) {
                // 2. 执行数据更新 (支持选区感知批量操作)
                int newValue = isBanHit ? 0 : hitStar;
                if (view && view->selectionModel() && view->selectionModel()->isSelected(index)) {
                    auto selectedIndexes = view->selectionModel()->selectedIndexes();
                    // 2026-07-xx 按照 Plan-86：遍历选区实现批量评分
                    for (const auto& selIdx : selectedIndexes) {
                        if (selIdx.column() == 0) {
                            model->setData(selIdx, newValue, m_ratingRole);
                        }
                    }
                } else {
                    model->setData(index, newValue, m_ratingRole);
                }

                // 3. 物理修复：直接执行禁用逻辑，杜绝 Lambda 嵌套导致的编译错误
                // 2026-06-xx 按照用户报错纠偏：改用更稳健的类型获取方式
                if (view) {
                    QAbstractItemView::EditTriggers currentTriggers = view->editTriggers();
                    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
                    // 延迟恢复触发器
                    QTimer::singleShot(0, view, [view, currentTriggers]() {
                        view->setEditTriggers(currentTriggers);
                    });
                }

                event->accept();
                return true;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace ArcMeta
