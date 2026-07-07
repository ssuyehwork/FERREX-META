#include "FilterPanel.h"
#include "../core/AppConfig.h"
#include "ToolTipOverlay.h"
#include "UiHelper.h"
#include "StyleLibrary.h"
#include "ColorPicker.h"
#include "SearchHistoryPanel.h"
#include <QPushButton>
#include <QMouseEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <QLinearGradient>
#include <QComboBox>
#include <QButtonGroup>

using namespace ArcMeta::Style;

namespace ArcMeta {

// ─── 颜色映射表 ────────────────────────────────────────────────────
QMap<QString, QColor> FilterPanel::s_colorMap() {
    return {
        { "",        QColor("#888780") },
        { "#E24B4A", QColor("#E24B4A") },
        { "#EF9F27", QColor("#EF9F27") },
        { "#FECF0E", QColor("#FECF0E") },
        { "#639922", QColor("#639922") },
        { "#1D9E75", QColor("#1D9E75") },
        { "#378ADD", QColor("#378ADD") },
        { "#7F77DD", QColor("#7F77DD") },
        { "#5F5E5A", QColor("#5F5E5A") },
        { "#000000", QColor("#000000") },
        { "#FFFFFF", QColor("#FFFFFF") }
    };
}

static QString ratingDisplayName(int r) {
    return r == 0 ? "无评级" : QString("★").repeated(r);
}

// ─── 自定义勾选框 ──────────────────────────────────────────────────
StyledCheckBox::StyledCheckBox(QWidget* parent) : QCheckBox(parent) {
    setFixedSize(15, 15);
}

void StyledCheckBox::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    bool checked = isChecked();
    
    // 使用 QRectF + 0.5px 内缩，确保笔触四边粗细完全一致
    QRectF rect(0.5, 0.5, width() - 1.0, height() - 1.0);
    QColor borderColor = checked ? QColor("#378ADD") : QColor("#444444");
    
    painter.setPen(QPen(borderColor, 1.0));
    painter.setBrush(QColor("#1E1E1E"));
    painter.drawRoundedRect(rect, 2.0, 2.0);

    if (checked) {
        QPen pen(QColor("#378ADD"), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        // 在 15x15 的区域内绘制对勾折线，坐标相对于 widget 自身
        QPolygonF checkMark;
        checkMark << QPointF(2.5, 7.5)
                  << QPointF(5.5, 11.0)
                  << QPointF(12.0, 3.5);
        painter.drawPolyline(checkMark);
    }
}

// ─── 可整行点击的行控件 ────────────────────────────────────────────
ClickableRow::ClickableRow(StyledCheckBox* cb, QWidget* parent)
    : QWidget(parent), m_cb(cb) {
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_StyledBackground);
}

void ClickableRow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        // 如果点击位置不在复选框上，手动 toggle，避免双重触发
        QPoint local = m_cb->mapFromGlobal(e->globalPosition().toPoint());
        if (!m_cb->rect().contains(local)) {
            m_cb->setChecked(!m_cb->isChecked());
        }
    }
    QWidget::mousePressEvent(e);
}

void ClickableRow::enterEvent(QEnterEvent* e) {
    setStyleSheet("QWidget { background: #2A2A2A; border-radius: 4px; }");
    QWidget::enterEvent(e);
}

void ClickableRow::leaveEvent(QEvent* e) {
    setStyleSheet("");
    QWidget::leaveEvent(e);
}

// ─── ColorBlock ──────────────────────────────────────────────────
ColorBlock::ColorBlock(const QColor& color, QWidget* parent) 
    : QWidget(parent), m_color(color) {
    // 2026-06-xx 物理规格对齐：与复选框 (15x15) 保持一致
    setFixedSize(15, 15);
    setCursor(Qt::PointingHandCursor);
}

void ColorBlock::setCount(int count) {
    m_count = count;
    update();
}

void ColorBlock::setChecked(bool checked) {
    m_checked = checked;
    update();
}

void ColorBlock::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 2026-06-xx 视觉对齐：外框微调，确保选中状态可见
    QRectF r = rect().adjusted(1, 1, -1, -1);
    
    if (m_checked) {
        // 选中态：加粗蓝色边框
        painter.setPen(QPen(QColor("#378ADD"), 1.5));
        painter.setBrush(m_color);
        painter.drawRoundedRect(r, 2.0, 2.0);
    } else {
        // 未选中：悬停时显示浅色边框
        painter.setPen(m_hovered ? QPen(QColor("#AAAAAA"), 1.0) : Qt::NoPen);
        painter.setBrush(m_color);
        painter.drawRoundedRect(r, 2.0, 2.0);
    }
    // 2026-06-xx 物理级同步：根据用户要求移除右上角计数黑点，保持色块纯净
}

void ColorBlock::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit clicked(m_color);
    }
}

void ColorBlock::enterEvent(QEnterEvent*) {
    m_hovered = true;
    update();
    QString tip = QString("颜色: %1\n匹配项: %2").arg(m_color.name().toUpper()).arg(m_count);
    // 2026-07-xx 按照 Plan-65：悬停触发，timeout = 0
    ToolTipOverlay::instance()->showText(QCursor::pos(), tip, 0);
}

void ColorBlock::leaveEvent(QEvent*) {
    m_hovered = false;
    update();
    ToolTipOverlay::hideTip();
}

// ─── InlineHueSlider ─────────────────────────────────────────────
InlineHueSlider::InlineHueSlider(QWidget* parent) : QWidget(parent) {
    setFixedHeight(28); 
    setCursor(Qt::PointingHandCursor);
}

void InlineHueSlider::setHue(int h) {
    m_h = h;
    update();
}

void InlineHueSlider::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int margin = 10; 
    int bwgWidth = 42; // 黑白灰区域总宽
    int gap = 6;
    int barHeight = 12;
    int barY = (height() - barHeight) / 2;

    // 1. 绘制黑白灰特殊色块 (3个 14px 宽度的色块)
    QRectF blackRect(margin, barY, 14, barHeight);
    QRectF grayRect(margin + 14, barY, 14, barHeight);
    QRectF whiteRect(margin + 28, barY, 14, barHeight);

    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawRect(blackRect);
    painter.setBrush(QColor("#808080"));
    painter.drawRect(grayRect);
    painter.setBrush(Qt::white);
    painter.drawRect(whiteRect);
    
    // 给无色系区域加一个极细的边框，防止白色溢出
    painter.setPen(QPen(QColor(80, 80, 80, 100), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(margin, barY, bwgWidth, barHeight);

    // 2. 绘制色相渐变区
    int hueStartX = margin + bwgWidth + gap;
    int hueWidth = width() - hueStartX - margin;
    if (hueWidth > 0) {
        QRectF hueRect(hueStartX, barY, hueWidth, barHeight);
        QLinearGradient grad(hueRect.left(), 0, hueRect.right(), 0);
        grad.setColorAt(0.0/6.0, QColor::fromHsv(0, 220, 220));
        grad.setColorAt(1.0/6.0, QColor::fromHsv(60, 220, 220));
        grad.setColorAt(2.0/6.0, QColor::fromHsv(120, 220, 220));
        grad.setColorAt(3.0/6.0, QColor::fromHsv(180, 220, 220));
        grad.setColorAt(4.0/6.0, QColor::fromHsv(240, 220, 220));
        grad.setColorAt(5.0/6.0, QColor::fromHsv(300, 220, 220));
        grad.setColorAt(6.0/6.0, QColor::fromHsv(359, 220, 220));

        painter.setPen(Qt::NoPen);
        painter.setBrush(grad);
        painter.drawRoundedRect(hueRect, 2, 2);
    }

    // 3. 绘制游标 (Thumb)
    int tx = 0;
    if (m_h == 1000) tx = blackRect.center().x();
    else if (m_h == 1001) tx = grayRect.center().x();
    else if (m_h == 1002) tx = whiteRect.center().x();
    else {
        double ratio = qBound(0, m_h, 359) / 359.0;
        tx = hueStartX + ratio * hueWidth;
    }
    
    painter.setBrush(Qt::white);
    painter.setPen(QPen(QColor(50, 50, 50), 1));
    painter.drawEllipse(QPoint(tx, height() / 2), 8, 8);
}

void InlineHueSlider::updateFromPos(int x) {
    int margin = 10;
    int bwgWidth = 42;
    int gap = 6;
    int hueStartX = margin + bwgWidth + gap;

    if (x < margin + 14) {
        m_h = 1000; // Black
    } else if (x < margin + 28) {
        m_h = 1001; // Gray
    } else if (x < margin + 42) {
        m_h = 1002; // White
    } else {
        int hueWidth = width() - hueStartX - margin;
        if (hueWidth <= 0) return;
        int lx = qBound(0, x - hueStartX, hueWidth);
        m_h = (lx * 359) / hueWidth;
    }
    update();
    emit hueChanged(m_h);
}

void InlineHueSlider::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) updateFromPos(event->pos().x());
}

void InlineHueSlider::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) updateFromPos(event->pos().x());
}

void InlineHueSlider::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) emit sliderReleased();
}

// ─── FilterPanel ──────────────────────────────────────────────────

void FilterPanel::syncUIFromFilterState() {
    updateHeaderStatus();
    
    // 遍历所有 StyledCheckBox 及其对应的 ClickableRow
    QList<StyledCheckBox*> allCheckBoxes = findChildren<StyledCheckBox*>();
    for (auto* cb : allCheckBoxes) {
        // 根据 checkbox 所在的上下文寻找对应的标识符
        ClickableRow* row = qobject_cast<ClickableRow*>(cb->parentWidget());
        if (!row) continue;
        
        QLabel* labelWidget = row->findChild<QLabel*>();
        if (!labelWidget) continue;
        
        QString text = labelWidget->text();
        bool shouldCheck = false;
        
        // 1. 评级匹配
        if (text == "无评级") shouldCheck = m_filter.ratings.contains(0);
        else if (text.contains("★")) shouldCheck = m_filter.ratings.contains(text.count("★"));
        
        // 2. 颜色匹配 (无色标)
        else if (text == "无色标") shouldCheck = m_filter.colors.contains("");
        
        // 3. 类型/日期匹配
        else if (m_filter.types.contains(text)) shouldCheck = true;
        else if (m_filter.createDates.contains(text)) shouldCheck = true;
        else if (m_filter.modifyDates.contains(text)) shouldCheck = true;
        
        // 4. 链接/备注/比例匹配
        else if (text == "有链接") shouldCheck = (m_filter.linkPresence == FilterState::Yes);
        else if (text == "无链接") shouldCheck = (m_filter.linkPresence == FilterState::No);
        else if (text == "有备注") shouldCheck = (m_filter.notePresence == FilterState::Yes);
        else if (text == "无备注") shouldCheck = (m_filter.notePresence == FilterState::No);
        else if (text == "横图") shouldCheck = (m_filter.ratio == FilterState::Horizontal);
        else if (text == "竖图") shouldCheck = (m_filter.ratio == FilterState::Vertical);
        else if (text == "方形") shouldCheck = (m_filter.ratio == FilterState::Square);
        else if (text == "16:9") shouldCheck = (m_filter.ratio == FilterState::Ratio169);

        cb->blockSignals(true);
        cb->setChecked(shouldCheck);
        cb->blockSignals(false);
    }

    // 5. 颜色色块同步
    QList<ColorBlock*> allColorBlocks = findChildren<ColorBlock*>();
    for (auto* block : allColorBlocks) {
        block->blockSignals(true);
        block->setChecked(m_filter.colors.contains(block->color().name().toUpper()));
        block->blockSignals(false);
    }
}

FilterPanel::FilterPanel(QWidget* parent) : QFrame(parent) {
    // 2026-07-xx 按照 Plan-63：启用右键菜单
    setContextMenuPolicy(Qt::CustomContextMenu);

    setObjectName("FilterContainer");
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(230);
    
    // 核心修正：移除宽泛的 QWidget QSS，防止其屏蔽 MainWindow 赋予的 ID 边框样式
    // 统一将文字颜色设为 #EEEEEE
    setStyleSheet("color: #EEEEEE;");

    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(0, 0, 0, 0);
    m_mainLayout->setSpacing(0);

    // 顶部标题栏
    // 2026-06-xx 物理对齐：FilterPanel 标题栏必须位于 QScrollArea 之外，
    // 确保滚动条仅在内容区显示（标题下方），符合规范 ②
    QWidget* topBar = new QWidget(this);
    topBar->setObjectName("ContainerHeader");
    topBar->setFixedHeight(32);
    // 重新注入标题栏样式，确保背景色和边框还原
    topBar->setStyleSheet(
        "QWidget#ContainerHeader {"
        "  background-color: #252526;"
        "  border-bottom: none;" // 2026-05-17 按照用户要求：覆盖全局 QSS 的 border-bottom，避免与首个 GroupHdrRow 的 border-top 叠加形成 2px 视觉分割线
        "}"
    );
    QHBoxLayout* topL = new QHBoxLayout(topBar);
    topL->setContentsMargins(15, 0, 5, 0); // 2026-xx-xx 按照用户要求：右侧保留 5px 呼吸边距
    topL->setSpacing(5);                  // 2026-05-17 按照用户要求：间距统一为 5px

    m_iconLabel = new QLabel(topBar);
    topL->addWidget(m_iconLabel);

    m_titleLabel = new QLabel("筛选", topBar);
    topL->addWidget(m_titleLabel);

    m_btnClearAll = new QPushButton(topBar);
    m_btnClearAll->setFixedSize(24, 24); // 2026-05-17 按照用户要求：统一为 24x24 规格以实现像素级对齐
    m_btnClearAll->setIcon(UiHelper::getIcon("reset_filter", QColor("#B0B0B0"))); // 2026-xx-xx 按照用户要求：使用新的 reset_filter 图标
    m_btnClearAll->setIconSize(QSize(16, 16));
    m_btnClearAll->setFlat(true);
    m_btnClearAll->setCursor(Qt::PointingHandCursor);
    m_btnClearAll->setProperty("tooltipText", "重置所有筛选条件");
    m_btnClearAll->installEventFilter(this);
    m_btnClearAll->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 4px; }"
        "QPushButton:hover { background: #3E3E42; }"
        "QPushButton:pressed { background: #4E4E52; }");
    connect(m_btnClearAll, &QPushButton::clicked, this, [this]() { clearAllFilters(true); });

    m_btnPin = new QPushButton(topBar);
    m_btnPin->setFixedSize(24, 24);
    m_btnPin->setIcon(UiHelper::getIcon("pin_tilted", QColor("#B0B0B0")));
    m_btnPin->setIconSize(QSize(18, 18));
    m_btnPin->setFlat(true);
    m_btnPin->setCursor(Qt::PointingHandCursor);
    m_btnPin->setProperty("tooltipText", "锁定当前筛选条件");
    m_btnPin->installEventFilter(this);
    m_btnPin->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 4px; }"
        "QPushButton:hover { background: #3E3E42; }"
        "QPushButton:pressed { background: #4E4E52; }");
    connect(m_btnPin, &QPushButton::clicked, this, [this]() {
        m_isFilterPinned = !m_isFilterPinned;
        if (m_isFilterPinned) {
            m_btnPin->setIcon(UiHelper::getIcon("pin", Style::ActiveOrange)); // 使用物理标准激活色
            m_btnPin->setProperty("tooltipText", "当前筛选条件已锁定（目录切换不重置）");
        } else {
            m_btnPin->setIcon(UiHelper::getIcon("pin_tilted", QColor("#B0B0B0")));
            m_btnPin->setProperty("tooltipText", "锁定当前筛选条件");
        }
    });

    m_btnToggleGroups = new QPushButton(topBar);
    m_btnToggleGroups->setFixedSize(24, 24);
    m_btnToggleGroups->setFlat(true);
    m_btnToggleGroups->setCursor(Qt::PointingHandCursor);
    m_btnToggleGroups->installEventFilter(this);
    m_btnToggleGroups->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 4px; }"
        "QPushButton:hover { background: #3E3E42; }"
        "QPushButton:pressed { background: #4E4E52; }");
    connect(m_btnToggleGroups, &QPushButton::clicked, this, &FilterPanel::onToggleAllGroupsClicked);

    topL->addStretch();
    topL->addWidget(m_btnPin, 0, Qt::AlignVCenter);
    topL->addWidget(m_btnToggleGroups, 0, Qt::AlignVCenter);
    topL->addWidget(m_btnClearAll, 0, Qt::AlignVCenter);
    m_mainLayout->addWidget(topBar);

    updateHeaderStatus();

    // 滚动内容区
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setStyleSheet(
        "QScrollArea { border: none; background: transparent; }"
        "QScrollBar:vertical { border-left: 1px solid #333333; }"
    );

    m_container = new QWidget(m_scrollArea);
    m_container->setStyleSheet("QWidget { background: transparent; }");
    m_containerLayout = new QVBoxLayout(m_container);
    // 恢复旧版边距：右侧和底部留出 0px 缓冲空间，确保滚动条贴边
    m_containerLayout->setContentsMargins(0, 0, 0, 10); 
    m_containerLayout->setSpacing(0);
    m_containerLayout->addStretch();

    m_scrollArea->setWidget(m_container);
    m_mainLayout->addWidget(m_scrollArea, 1);

    // 2026-06-xx 物理对齐：从 AppConfig 加载持久化的最近筛选色
    m_recentColors = AppConfig::instance().getValue("Filter/RecentColors").toStringList();

    m_historyPanel = new SearchHistoryPanel(this);

    // 2026-xx-xx 按照用户要求：当筛选状态改变时，同步更新标题栏图标颜色
    connect(this, &FilterPanel::filterChanged, this, &FilterPanel::updateHeaderStatus);
}

void FilterPanel::saveFilterHistory(const QString& key, const QString& text) {
    if (text.trimmed().isEmpty()) return;
    QString fullKey = "FilterHistory/" + key;
    QStringList history = AppConfig::instance().getValue(fullKey).toStringList();
    history.removeAll(text);
    history.append(text);
    if (history.size() > 10) history.removeFirst();
    AppConfig::instance().setValue(fullKey, history);
}

QStringList FilterPanel::getFilterHistory(const QString& key) const {
    return AppConfig::instance().getValue("FilterHistory/" + key).toStringList();
}

// 2026-03-xx 按照用户要求：物理拦截事件以实现自定义 ToolTipOverlay 的显隐控制
bool FilterPanel::eventFilter(QObject* watched, QEvent* event) {
    // 2026-xx-xx 按照用户要求：处理双击快速输入框显示历史记录
    if (event->type() == QEvent::MouseButtonDblClick) {
        QLineEdit* edit = qobject_cast<QLineEdit*>(watched);
        if (edit && edit->objectName() == "FilterSearchEdit") {
            QString key;
            if (edit == m_editColor) key = "Color";
            else if (edit == m_editType) key = "Type";
            else if (edit == m_editCreateDate) key = "CreateDate";
            else if (edit == m_editModifyDate) key = "ModifyDate";

            if (!key.isEmpty()) {
                QStringList history = getFilterHistory(key);
                m_historyPanel->setHistory(history, "最近搜索");
                
                // 断开之前的连接
                m_historyPanel->disconnect(this);

                connect(m_historyPanel, &SearchHistoryPanel::historyItemClicked, this, [this, edit, key](const QString& text) {
                    edit->setText(text);
                    if (edit == m_editColor) m_filter.colorFilterText = text;
                    else if (edit == m_editType) m_filter.typeFilterText = text;
                    else if (edit == m_editCreateDate) m_filter.createDateFilterText = text;
                    else if (edit == m_editModifyDate) m_filter.modifyDateFilterText = text;

                    saveFilterHistory(key, text);
                    emit filterChanged(m_filter);
                    m_historyPanel->hide();
                });

                connect(m_historyPanel, &SearchHistoryPanel::historyItemRemoved, this, [this, key](const QString& text) {
                    QString fullKey = "FilterHistory/" + key;
                    QStringList history = AppConfig::instance().getValue(fullKey).toStringList();
                    history.removeAll(text);
                    AppConfig::instance().setValue(fullKey, history);
                    m_historyPanel->setHistory(history, "最近搜索");
                });

                connect(m_historyPanel, &SearchHistoryPanel::clearAllRequested, this, [this, key]() {
                    AppConfig::instance().setValue("FilterHistory/" + key, QStringList());
                    m_historyPanel->setHistory(QStringList(), "最近搜索");
                });

                m_historyPanel->showBelow(edit);
                return true;
            }
        }
    }

    if (event->type() == QEvent::HoverEnter) {
        QString text = watched->property("tooltipText").toString();
        if (!text.isEmpty()) {
            // 2026-07-xx 按照 Plan-65：悬停触发，timeout = 0
            ToolTipOverlay::instance()->showText(QCursor::pos(), text, 0);
        }
    } else if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress) {
        // 2026-06-23 按照用户要求：滑杆滑动/悬停显示百分比数值
        if (watched == m_areaSlider) {
            ToolTipOverlay::instance()->showText(QCursor::pos(), QString("%1%").arg(m_areaSlider->value()), 0);
        }
    } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::MouseButtonRelease || (event->type() == QEvent::MouseButtonPress && watched != m_areaSlider)) {
        // 2026-06-23 逻辑修正：滑杆按下时不隐藏（以便滑动回显），离开或释放时隐藏
        ToolTipOverlay::hideTip();
    }
    
    // 2026-05-17 根因修复：已废除 Resize 监听逻辑（原用于绝对定位，现已改为内嵌布局）
    return QWidget::eventFilter(watched, event);
}

// ─── populate ─────────────────────────────────────────────────────
void FilterPanel::populate(
    const QMap<int, int>&       ratingCounts,
    const QMap<QString, int>&   colorCounts,
    const QMap<QString, int>&   typeCounts,
    const QMap<QString, int>&   createDateCounts,
    const QMap<QString, int>&   modifyDateCounts,
    int                         emptyFolderCount)
{
    // 2026-06-xx 物理修复：若所有输入均为空，且当前没有活动的文本过滤，则判定为异步加载中间态，拒绝执行重绘以防止 UI 抖动
    if (ratingCounts.isEmpty() && colorCounts.isEmpty() &&
        typeCounts.isEmpty() && createDateCounts.isEmpty() && modifyDateCounts.isEmpty() &&
        emptyFolderCount == 0 &&
        m_filter.colorFilterText.isEmpty() &&
        m_filter.typeFilterText.isEmpty() && m_filter.createDateFilterText.isEmpty() &&
        m_filter.modifyDateFilterText.isEmpty()) {
        return;
    }

    // 2026-xx-xx 按照 Plan-106：增量更新判定
    // 判定依据：如果各项的数量（Keys）没有发生物理变动，仅执行增量同步，不重建 UI
    bool structureChanged = (m_ratingCounts.keys() != ratingCounts.keys() ||
                             m_colorCounts.keys() != colorCounts.keys() ||
                             m_typeCounts.keys() != typeCounts.keys() ||
                             m_createDateCounts.keys() != createDateCounts.keys() ||
                             m_modifyDateCounts.keys() != modifyDateCounts.keys() ||
                             m_emptyFolderCount != emptyFolderCount);

    m_ratingCounts     = ratingCounts;
    m_colorCounts      = colorCounts;
    m_typeCounts       = typeCounts;
    m_createDateCounts = createDateCounts;
    m_modifyDateCounts = modifyDateCounts;
    m_emptyFolderCount = emptyFolderCount;

    if (structureChanged) {
        rebuildGroups();
    } else {
        syncUIFromFilterState();
        // 更新现有行中的计数 Label (Cnt)
        QList<ClickableRow*> rows = m_container->findChildren<ClickableRow*>();
        for (auto* row : rows) {
             QList<QLabel*> labels = row->findChildren<QLabel*>();
             if (labels.size() >= 2) {
                 QLabel* cntLabel = labels.last(); // 计数 Label 在末尾
                 QLabel* nameLabel = labels.at(labels.size() - 2);
                 QString name = nameLabel->text();
                 
                 int count = 0;
                 if (name == "无评级") count = m_ratingCounts[0];
                 else if (name.contains("★")) count = m_ratingCounts[name.count("★")];
                 else if (name == "空文件夹") count = m_emptyFolderCount;
                 else if (name == "文件夹") count = m_typeCounts["folder"];
                 else if (name == "文件") count = m_typeCounts["file"];
                 else if (m_typeCounts.contains(name)) count = m_typeCounts[name];
                 else if (m_createDateCounts.contains(name)) count = m_createDateCounts[name];
                 else if (m_modifyDateCounts.contains(name)) count = m_modifyDateCounts[name];
                 
                 cntLabel->setText(QString::number(count));
             }
        }
    }
}

void FilterPanel::rebuildDateCheckboxes(bool isCreateDate, bool descending) {
    QVBoxLayout* layout = isCreateDate ? m_createDateLayout : m_modifyDateLayout;
    if (!layout) return;

    // 清除现有的复选框行 (保留 QLineEdit)
    while (layout->count() > 1) {
        QLayoutItem* item = layout->takeAt(1);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const QMap<QString, int>& counts = isCreateDate ? m_createDateCounts : m_modifyDateCounts;
    // 2026-07-xx Plan-92: 通过引用访问当前筛选状态中的日期列表，确保勾选状态保留
    QStringList& selected = isCreateDate ? m_filter.createDates : m_filter.modifyDates;

    QStringList dates = counts.keys();
    std::sort(dates.begin(), dates.end(), [descending](const QString& a, const QString& b) {
        return descending ? (a > b) : (a < b);
    });

    for (const QString& d : dates) {
        QCheckBox* cb = addFilterRow(layout, d, counts[d]);
        cb->blockSignals(true);
        cb->setChecked(selected.contains(d));
        cb->blockSignals(false);
        // 2026-07-xx 物理修复：禁止捕获局部引用以防止悬空指针崩溃，改为按值捕获 isCreateDate
        connect(cb, &QCheckBox::toggled, this, [this, isCreateDate, d](bool on) {
            QStringList& targetList = isCreateDate ? m_filter.createDates : m_filter.modifyDates;
            if (on) { if (!targetList.contains(d)) targetList.append(d); }
            else targetList.removeAll(d);
            emit filterChanged(m_filter);
        });
    }
}

// ─── rebuildGroups ────────────────────────────────────────────────
void FilterPanel::rebuildGroups() {
    updateHeaderStatus();
    m_groupHeaders.clear();

    // 2026-xx-xx 物理安全：重置快速输入框指针，防止 Directory 切换导致的野指针崩溃
    m_editColor = nullptr;
    m_editType = nullptr;
    m_editCreateDate = nullptr;
    m_editModifyDate = nullptr;
    m_createDateLayout = nullptr;
    m_modifyDateLayout = nullptr;
    m_accuracySlider = nullptr;
    m_areaSlider = nullptr;

    // 清空旧内容（保留末尾 stretch）
    while (m_containerLayout->count() > 1) {
        QLayoutItem* item = m_containerLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    auto colorMap = s_colorMap();


    // ── 1. 评级 ──────────────────────────────────────────────
    if (!m_ratingCounts.isEmpty() && m_isMirrorSource) {
        QVBoxLayout* gl = nullptr;
        QWidget* g = buildGroup("评级", gl);
        m_groupRating = g;
        for (int r : {0, 1, 2, 3, 4, 5}) {
            if (!m_ratingCounts.contains(r)) continue;
            QCheckBox* cb = addFilterRow(gl, ratingDisplayName(r), m_ratingCounts[r]);
            cb->blockSignals(true);
            cb->setChecked(m_filter.ratings.contains(r));
            cb->blockSignals(false);
            connect(cb, &QCheckBox::toggled, this, [this, r](bool on) {
                if (on) { if (!m_filter.ratings.contains(r)) m_filter.ratings.append(r); }
                else m_filter.ratings.removeAll(r);
                emit filterChanged(m_filter);
            });
        }
        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }


    // ── 2. 颜色标记 (Plan-18: 矩阵重构版) ─────────────────────────
    if (m_isMirrorSource) { // 2026-07-xx 按照 Plan-118：仅在镜像源下显示颜色标记
        QVBoxLayout* gl = nullptr;
        QHBoxLayout* hdrLayout = nullptr;
        QWidget* g = buildGroup("颜色标记", gl, &hdrLayout);
        m_groupColor = g;

        // 新增快速输入框
        m_editColor = new QLineEdit(g);
        m_editColor->setClearButtonEnabled(true);
        m_editColor->setPlaceholderText("例： 红 / #E24B4A / 无色标");
        m_editColor->setText(m_filter.colorFilterText);
        m_editColor->setObjectName("FilterSearchEdit");
        m_editColor->setStyleSheet(
            "QLineEdit#FilterSearchEdit {"
            "  background: #2D2D2D;"
            "  color: #CCCCCC;"
            "  border: 1px solid #444444;"
            "  border-radius: 6px;" // 2026-07-xx 按照《Plan-21》：输入框统一使用 6px 圆角
            "  padding: 4px 8px;"
            "  margin: 4px 5px;"
            "  font-size: 11px;"
            "}"
            "QLineEdit#FilterSearchEdit:focus { border-color: #378ADD; color: #FFFFFF; }"
        );
        m_editColor->installEventFilter(this);
        connect(m_editColor, &QLineEdit::returnPressed, this, [this]() {
            m_filter.colorFilterText = m_editColor->text();
            saveFilterHistory("Color", m_filter.colorFilterText);
            emit filterChanged(m_filter);
        });
        connect(m_editColor, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (text.isEmpty() && !m_filter.colorFilterText.isEmpty()) {
                m_filter.colorFilterText = "";
                emit filterChanged(m_filter);
            }
        });
        gl->addWidget(m_editColor);

        // 2.1 顶部色相滑块
        // 2026-06-xx 物理对齐：滑块及其容器增加 4px 左右边距（相对于 gl 的 0 边距），实现视觉平衡
        QWidget* hueContainer = new QWidget(g);
        QHBoxLayout* hueLayout = new QHBoxLayout(hueContainer);
        hueLayout->setContentsMargins(5, 0, 5, 0);
        hueLayout->setSpacing(0);
        
        InlineHueSlider* hueSlider = new InlineHueSlider(hueContainer);
        hueLayout->addWidget(hueSlider);
        connect(hueSlider, &InlineHueSlider::sliderReleased, this, [this, hueSlider]() {
            int h = hueSlider->hue();
            QColor c;
            if (h == 1000) c = Qt::black;
            else if (h == 1001) c = QColor("#808080");
            else if (h == 1002) c = Qt::white;
            else c = QColor::fromHsv(h, 220, 220);

            QString hex = c.name().toUpper();
            m_filter.colors.clear();
            m_filter.colors.append(hex);
            
            // LRU 更新 (2026-06-xx: 容量扩展至 50 个，且由左上向右下按时间排布)
            m_recentColors.removeAll(hex);
            m_recentColors.prepend(hex);
            if (m_recentColors.size() > 50) m_recentColors.removeLast();
            AppConfig::instance().setValue("Filter/RecentColors", m_recentColors);

            emit filterChanged(m_filter);
            rebuildGroups();
        });
        gl->addWidget(hueContainer);

        // 2.1.5 颜色准确度 (容差) 滑块 ─────────────────────────
        // 2026-07-xx 按照用户要求：还原此前被误删的准确度控制条
        QWidget* accContainer = new QWidget(g);
        QHBoxLayout* accLayout = new QHBoxLayout(accContainer);
        accLayout->setContentsMargins(10, 4, 10, 4);
        accLayout->setSpacing(8);

        QLabel* lblAcc = new QLabel("准确度:", accContainer);
        lblAcc->setStyleSheet("color: #AAAAAA; font-size: 11px;");
        accLayout->addWidget(lblAcc);

        m_accuracySlider = new QSlider(Qt::Horizontal, accContainer);
        m_accuracySlider->setRange(0, 100);
        m_accuracySlider->setValue(m_filter.colorTolerance);
        m_accuracySlider->setCursor(Qt::PointingHandCursor);
        m_accuracySlider->setStyleSheet(
            "QSlider::groove:horizontal { height: 2px; background: #444; border-radius: 1px; }"
            "QSlider::handle:horizontal { background: #EEE; border: 1px solid #777; width: 10px; height: 10px; margin: -4px 0; border-radius: 5px; }"
            "QSlider::handle:horizontal:hover { background: #FFF; border-color: #378ADD; }"
        );
        accLayout->addWidget(m_accuracySlider, 1);

        connect(m_accuracySlider, &QSlider::valueChanged, this, [this](int val) {
            m_filter.colorTolerance = val;
            emit filterChanged(m_filter);
        });

        gl->addWidget(accContainer);

        // 2.1.6 颜色占比滑块 ─────────────────────────────────
        // 2026-06-23 按照用户要求：新增颜色面积占比过滤逻辑
        QWidget* areaContainer = new QWidget(g);
        QHBoxLayout* areaLayout = new QHBoxLayout(areaContainer);
        areaLayout->setContentsMargins(10, 4, 10, 4);
        areaLayout->setSpacing(8);

        QLabel* lblArea = new QLabel("占比:", areaContainer);
        lblArea->setStyleSheet("color: #AAAAAA; font-size: 11px;");
        areaLayout->addWidget(lblArea);

        m_areaSlider = new QSlider(Qt::Horizontal, areaContainer);
        m_areaSlider->setRange(0, 100);
        m_areaSlider->setValue(m_filter.minColorArea);
        m_areaSlider->setCursor(Qt::PointingHandCursor);
        m_areaSlider->setMouseTracking(true); // 2026-06-23 按照用户要求：支持悬停/滑动实时回显百分比
        m_areaSlider->installEventFilter(this);
        m_areaSlider->setStyleSheet(
            "QSlider::groove:horizontal { height: 2px; background: #444; border-radius: 1px; }"
            "QSlider::handle:horizontal { background: #EEE; border: 1px solid #777; width: 10px; height: 10px; margin: -4px 0; border-radius: 5px; }"
            "QSlider::handle:horizontal:hover { background: #FFF; border-color: #378ADD; }"
        );
        areaLayout->addWidget(m_areaSlider, 1);

        connect(m_areaSlider, &QSlider::valueChanged, this, [this](int val) {
            m_filter.minColorArea = val;
            emit filterChanged(m_filter);
        });

        gl->addWidget(areaContainer);

        // 2.2 标准色矩阵 (12色)
        // 2026-06-xx 物理对齐：设置左边距 8px 以对齐下方的复选框视觉线
        QLabel* lblStatic = new QLabel("标准色系", g);
        lblStatic->setStyleSheet("color: #666; font-size: 10px; margin-top: 4px; margin-left: 5px;");
        gl->addWidget(lblStatic);

        QWidget* staticGrid = new QWidget(g);
        staticGrid->setContentsMargins(5, 0, 5, 0); 
        // 2026-06-xx 物理微调：间距从 4px 缩减至 2px
        FlowLayout* staticFlow = new FlowLayout(staticGrid, 0, 2, 2);
        staticGrid->setLayout(staticFlow);
        
        QStringList standardHex = {
            "#E24B4A", "#EF9F27", "#FECF0E", "#639922", 
            "#1D9E75", "#378ADD", "#7F77DD", "#E91E63",
            "#000000", "#808080", "#FFFFFF", "#795548"
        };

        for (const QString& hex : standardHex) {
            ColorBlock* block = new ColorBlock(QColor(hex), staticGrid);
            block->setChecked(m_filter.colors.contains(hex));
            
            // 异步统计对账 (模拟：此处可后续接入真正的数据查询)
            int count = 0;
            for (auto it = m_colorCounts.begin(); it != m_colorCounts.end(); ++it) {
                if (UiHelper::calculateDeltaE(QColor(hex), UiHelper::parseColorName(it.key())) < 10.0) {
                    count += it.value();
                }
            }
            block->setCount(count);

            connect(block, &ColorBlock::clicked, this, [this, hex](const QColor& /*c*/) {
                if (m_filter.colors.contains(hex)) {
                    m_filter.colors.removeAll(hex);
                } else {
                    m_filter.colors.clear(); // 单选模式
                    m_filter.colors.append(hex);
                    
                    // LRU 更新
                    m_recentColors.removeAll(hex);
                    m_recentColors.prepend(hex);
                    if (m_recentColors.size() > 50) m_recentColors.removeLast();
                    AppConfig::instance().setValue("Filter/RecentColors", m_recentColors);
                }
                emit filterChanged(m_filter);
                rebuildGroups();
            });
            staticFlow->addWidget(block);
        }
        gl->addWidget(staticGrid);

        // 2.3 最近筛选 (LRU)
        if (!m_recentColors.isEmpty()) {
            QLabel* lblRecent = new QLabel("最近筛选", g);
            lblRecent->setStyleSheet("color: #666; font-size: 10px; margin-top: 8px; margin-left: 5px;");
            gl->addWidget(lblRecent);

            QWidget* recentGrid = new QWidget(g);
            recentGrid->setContentsMargins(5, 0, 5, 0);
            // 2026-06-xx 物理微调：间距从 4px 缩减至 2px
            FlowLayout* recentFlow = new FlowLayout(recentGrid, 0, 2, 2);
            recentGrid->setLayout(recentFlow);

            for (const QString& hex : m_recentColors) {
                ColorBlock* block = new ColorBlock(QColor(hex), recentGrid);
                block->setChecked(m_filter.colors.contains(hex));
                
                int count = 0;
                for (auto it = m_colorCounts.begin(); it != m_colorCounts.end(); ++it) {
                    if (UiHelper::calculateDeltaE(QColor(hex), UiHelper::parseColorName(it.key())) < 10.0) {
                        count += it.value();
                    }
                }
                block->setCount(count);

                connect(block, &ColorBlock::clicked, this, [this, hex](const QColor& /*c*/) {
                    if (m_filter.colors.contains(hex)) {
                        m_filter.colors.removeAll(hex);
                    } else {
                        m_filter.colors.clear();
                        m_filter.colors.append(hex);
                        
                        // 即使是在最近面板中点击，也应更新排序使其置顶
                        m_recentColors.removeAll(hex);
                        m_recentColors.prepend(hex);
                        AppConfig::instance().setValue("Filter/RecentColors", m_recentColors);
                    }
                    emit filterChanged(m_filter);
                    rebuildGroups();
                });
                recentFlow->addWidget(block);
            }
            gl->addWidget(recentGrid);
        }

        // 2.4 无色标处理
        if (m_colorCounts.contains("")) {
             QCheckBox* cb = addFilterRow(gl, "无色标", m_colorCounts[""], QColor("#888780"));
             cb->setChecked(m_filter.colors.contains(""));
             connect(cb, &QCheckBox::toggled, this, [this](bool on) {
                 if (on) { m_filter.colors.clear(); m_filter.colors.append(""); }
                 else m_filter.colors.removeAll("");
                 emit filterChanged(m_filter);
                 rebuildGroups();
             });
        }

        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }


    // ── 4. 文件类型 ──────────────────────────────────────────
    if (!m_typeCounts.isEmpty() || !m_filter.typeFilterText.isEmpty() || m_emptyFolderCount > 0) {
        QVBoxLayout* gl = nullptr;
        QWidget* g = buildGroup("文件类型", gl);

        m_editType = new QLineEdit(g);
        m_editType->setClearButtonEnabled(true);
        m_editType->setPlaceholderText("例： png / 文件夹...");
        m_editType->setText(m_filter.typeFilterText);
        m_editType->setObjectName("FilterSearchEdit");
        m_editType->setStyleSheet(
            "QLineEdit#FilterSearchEdit { background: #2D2D2D; color: #CCCCCC; border: 1px solid #444444; border-radius: 6px; padding: 4px 8px; margin: 4px 5px; font-size: 11px; }"
            "QLineEdit#FilterSearchEdit:focus { border-color: #378ADD; color: #FFFFFF; }"
        );
        m_editType->installEventFilter(this);
        connect(m_editType, &QLineEdit::returnPressed, this, [this]() {
            m_filter.typeFilterText = m_editType->text();
            saveFilterHistory("Type", m_filter.typeFilterText);
            emit filterChanged(m_filter);
        });
        connect(m_editType, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (text.isEmpty() && !m_filter.typeFilterText.isEmpty()) {
                m_filter.typeFilterText = "";
                emit filterChanged(m_filter);
            }
        });
        gl->addWidget(m_editType);

        if (m_emptyFolderCount > 0) {
            QCheckBox* cb = addFilterRow(gl, "空文件夹", m_emptyFolderCount);
            cb->blockSignals(true);
            cb->setChecked(m_filter.types.contains("空文件夹"));
            cb->blockSignals(false);
            connect(cb, &QCheckBox::toggled, this, [this](bool on) {
                if (on) { if (!m_filter.types.contains("空文件夹")) m_filter.types.append("空文件夹"); }
                else    m_filter.types.removeAll("空文件夹");
                emit filterChanged(m_filter);
            });
        }

        if (m_typeCounts.contains("folder")) {
            QCheckBox* cb = addFilterRow(gl, "文件夹", m_typeCounts["folder"]);
            cb->blockSignals(true);
            cb->setChecked(m_filter.types.contains("folder"));
            cb->blockSignals(false);
            connect(cb, &QCheckBox::toggled, this, [this](bool on) {
                if (on) { if (!m_filter.types.contains("folder")) m_filter.types.append("folder"); }
                else    m_filter.types.removeAll("folder");
                emit filterChanged(m_filter);
            });
        }
        if (m_typeCounts.contains("file")) {
            QCheckBox* cb = addFilterRow(gl, "文件", m_typeCounts["file"]);
            cb->blockSignals(true);
            cb->setChecked(m_filter.types.contains("file"));
            cb->blockSignals(false);
            connect(cb, &QCheckBox::toggled, this, [this](bool on) {
                if (on) { if (!m_filter.types.contains("file")) m_filter.types.append("file"); }
                else    m_filter.types.removeAll("file");
                emit filterChanged(m_filter);
            });
        }
        QStringList exts = m_typeCounts.keys(); exts.sort();
        for (const QString& ext : exts) {
            if (ext == "folder" || ext == "file" || ext == "空文件夹") continue;
            QString label = ext.isEmpty() ? "无扩展名" : ext;
            QCheckBox* cb = addFilterRow(gl, label, m_typeCounts[ext]);
            cb->blockSignals(true);
            cb->setChecked(m_filter.types.contains(ext));
            cb->blockSignals(false);
            connect(cb, &QCheckBox::toggled, this, [this, ext](bool on) {
                if (on) { if (!m_filter.types.contains(ext)) m_filter.types.append(ext); }
                else m_filter.types.removeAll(ext);
                emit filterChanged(m_filter);
            });
        }
        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }

    // ── 5. 创建日期 (Plan-92: 排序支持) ──────────────────────────
    if (!m_createDateCounts.isEmpty() || !m_filter.createDateFilterText.isEmpty()) {
        QVBoxLayout* gl = nullptr;
        QHBoxLayout* hdrLayout = nullptr;
        QWidget* g = buildGroup("创建日期", gl, &hdrLayout);
        m_createDateLayout = gl;

        QPushButton* btnSort = new QPushButton(g);
        btnSort->setFixedSize(16, 16);
        btnSort->setIcon(UiHelper::getIcon(m_createDateDesc ? "arrow_down" : "arrow_up", QColor("#B0B0B0")));
        btnSort->setFlat(true);
        btnSort->setCursor(Qt::PointingHandCursor);
        btnSort->setStyleSheet("QPushButton { background: transparent; border: none; } QPushButton:hover { background: #3E3E42; border-radius: 2px; }");
        hdrLayout->addWidget(btnSort);
        connect(btnSort, &QPushButton::clicked, this, [this, btnSort]() {
            m_createDateDesc = !m_createDateDesc;
            btnSort->setIcon(UiHelper::getIcon(m_createDateDesc ? "arrow_down" : "arrow_up", QColor("#B0B0B0")));
            rebuildDateCheckboxes(true, m_createDateDesc);
        });

        m_editCreateDate = new QLineEdit(g);
        m_editCreateDate->setClearButtonEnabled(true);
        m_editCreateDate->setPlaceholderText("例： 2025 / 03-2025...");
        m_editCreateDate->setText(m_filter.createDateFilterText);
        m_editCreateDate->setObjectName("FilterSearchEdit");
        m_editCreateDate->setStyleSheet(
            "QLineEdit#FilterSearchEdit { background: #2D2D2D; color: #CCCCCC; border: 1px solid #444444; border-radius: 6px; padding: 4px 8px; margin: 4px 5px; font-size: 11px; }"
            "QLineEdit#FilterSearchEdit:focus { border-color: #378ADD; color: #FFFFFF; }"
        );
        m_editCreateDate->installEventFilter(this);
        connect(m_editCreateDate, &QLineEdit::returnPressed, this, [this]() {
            m_filter.createDateFilterText = m_editCreateDate->text();
            saveFilterHistory("CreateDate", m_filter.createDateFilterText);
            emit filterChanged(m_filter);
        });
        connect(m_editCreateDate, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (text.isEmpty() && !m_filter.createDateFilterText.isEmpty()) {
                m_filter.createDateFilterText = "";
                emit filterChanged(m_filter);
            }
        });
        gl->addWidget(m_editCreateDate);

        rebuildDateCheckboxes(true, m_createDateDesc);
        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }

    // ── 6. 修改日期 (Plan-92: 排序支持) ──────────────────────────
    if (!m_modifyDateCounts.isEmpty() || !m_filter.modifyDateFilterText.isEmpty()) {
        QVBoxLayout* gl = nullptr;
        QHBoxLayout* hdrLayout = nullptr;
        QWidget* g = buildGroup("修改日期", gl, &hdrLayout);
        m_modifyDateLayout = gl;

        QPushButton* btnSort = new QPushButton(g);
        btnSort->setFixedSize(16, 16);
        btnSort->setIcon(UiHelper::getIcon(m_modifyDateDesc ? "arrow_down" : "arrow_up", QColor("#B0B0B0")));
        btnSort->setFlat(true);
        btnSort->setCursor(Qt::PointingHandCursor);
        btnSort->setStyleSheet("QPushButton { background: transparent; border: none; } QPushButton:hover { background: #3E3E42; border-radius: 2px; }");
        hdrLayout->addWidget(btnSort);
        connect(btnSort, &QPushButton::clicked, this, [this, btnSort]() {
            m_modifyDateDesc = !m_modifyDateDesc;
            btnSort->setIcon(UiHelper::getIcon(m_modifyDateDesc ? "arrow_down" : "arrow_up", QColor("#B0B0B0")));
            rebuildDateCheckboxes(false, m_modifyDateDesc);
        });

        m_editModifyDate = new QLineEdit(g);
        m_editModifyDate->setClearButtonEnabled(true);
        m_editModifyDate->setPlaceholderText("例： 2025 / 03-2025...");
        m_editModifyDate->setText(m_filter.modifyDateFilterText);
        m_editModifyDate->setObjectName("FilterSearchEdit");
        m_editModifyDate->setStyleSheet(
            "QLineEdit#FilterSearchEdit { background: #2D2D2D; color: #CCCCCC; border: 1px solid #444444; border-radius: 6px; padding: 4px 8px; margin: 4px 5px; font-size: 11px; }"
            "QLineEdit#FilterSearchEdit:focus { border-color: #378ADD; color: #FFFFFF; }"
        );
        m_editModifyDate->installEventFilter(this);
        connect(m_editModifyDate, &QLineEdit::returnPressed, this, [this]() {
            m_filter.modifyDateFilterText = m_editModifyDate->text();
            saveFilterHistory("ModifyDate", m_filter.modifyDateFilterText);
            emit filterChanged(m_filter);
        });
        connect(m_editModifyDate, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (text.isEmpty() && !m_filter.modifyDateFilterText.isEmpty()) {
                m_filter.modifyDateFilterText = "";
                emit filterChanged(m_filter);
            }
        });
        gl->addWidget(m_editModifyDate);

        rebuildDateCheckboxes(false, m_modifyDateDesc);
        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }

    // ── 7. 链接 (独立主选项) ──────────────────────────────────────────
    if (m_isMirrorSource) {
        QVBoxLayout* gl = nullptr;
        QWidget* g = buildGroup("链接", gl);
        m_groupLink = g;

        QButtonGroup* linkGroup = new QButtonGroup(g);
        linkGroup->setExclusive(false); // 改为非排他性，允许取消勾选
        for (auto p : {FilterState::Yes, FilterState::No}) {
            QString label = (p == FilterState::Yes ? "有链接" : "无链接");
            
            StyledCheckBox* cb = new StyledCheckBox();
            ClickableRow* row = new ClickableRow(cb);
            row->setFixedHeight(24);
            QHBoxLayout* rl = new QHBoxLayout(row);
            rl->setContentsMargins(5, 0, 5, 0);
            rl->setSpacing(5);
            rl->addWidget(cb);
            QLabel* lbl = new QLabel(label, row);
            lbl->setStyleSheet("font-size: 12px; color: #CCCCCC; background: transparent;");
            rl->addWidget(lbl, 1);
            gl->addWidget(row);

            if (m_filter.linkPresence == p) cb->setChecked(true);
            connect(cb, &QCheckBox::toggled, this, [this, p, linkGroup, cb](bool on) {
                if (on) {
                    // 手动实现单选逻辑：勾选当前项时，取消同组其他项
                    for (QAbstractButton* b : linkGroup->buttons()) {
                        if (b != cb && b->isChecked()) {
                            b->blockSignals(true);
                            b->setChecked(false);
                            b->blockSignals(false);
                        }
                    }
                    m_filter.linkPresence = p;
                } else {
                    m_filter.linkPresence = FilterState::All;
                }
                emit filterChanged(m_filter);
            });
            linkGroup->addButton(cb);
        }
        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }

    // ── 8. 备注 (独立主选项) ──────────────────────────────────────────
    if (m_isMirrorSource) {
        QVBoxLayout* gl = nullptr;
        QWidget* g = buildGroup("备注", gl);
        m_groupNote = g;

        QButtonGroup* noteGroup = new QButtonGroup(g);
        noteGroup->setExclusive(false); // 改为非排他性
        for (auto p : {FilterState::Yes, FilterState::No}) {
            QString label = (p == FilterState::Yes ? "有备注" : "无备注");
            
            StyledCheckBox* cb = new StyledCheckBox();
            ClickableRow* row = new ClickableRow(cb);
            row->setFixedHeight(24);
            QHBoxLayout* rl = new QHBoxLayout(row);
            rl->setContentsMargins(5, 0, 5, 0);
            rl->setSpacing(5);
            rl->addWidget(cb);
            QLabel* lbl = new QLabel(label, row);
            lbl->setStyleSheet("font-size: 12px; color: #CCCCCC; background: transparent;");
            rl->addWidget(lbl, 1);
            gl->addWidget(row);

            if (m_filter.notePresence == p) cb->setChecked(true);
            connect(cb, &QCheckBox::toggled, this, [this, p, noteGroup, cb](bool on) {
                if (on) {
                    for (QAbstractButton* b : noteGroup->buttons()) {
                        if (b != cb && b->isChecked()) {
                            b->blockSignals(true);
                            b->setChecked(false);
                            b->blockSignals(false);
                        }
                    }
                    m_filter.notePresence = p;
                } else {
                    m_filter.notePresence = FilterState::All;
                }
                emit filterChanged(m_filter);
            });
            noteGroup->addButton(cb);
        }
        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }

    // ── 9. 文件大小 (独立主选项) ──────────────────────────────────────────
    {
        QVBoxLayout* gl = nullptr;
        QWidget* g = buildGroup("文件大小", gl);

        QHBoxLayout* hs = new QHBoxLayout();
        hs->setContentsMargins(5, 4, 5, 8);
        hs->setSpacing(8); // 增加间距
        
        QLineEdit* minEdit = new QLineEdit(g);
        minEdit->setClearButtonEnabled(true);
        QLineEdit* maxEdit = new QLineEdit(g);
        maxEdit->setClearButtonEnabled(true);
        QComboBox* unitCombo = new QComboBox(g);
        unitCombo->addItems({"KB", "MB", "GB"});
        unitCombo->setCurrentIndex(1); // Default MB

        auto sizeEditStyle = "QLineEdit { background: #2D2D2D; color: #EEE; border: 1px solid #444; border-radius: 4px; padding: 2px 4px; font-size: 11px; }";
        minEdit->setStyleSheet(sizeEditStyle);
        maxEdit->setStyleSheet(sizeEditStyle);
        minEdit->setPlaceholderText("最小");
        maxEdit->setPlaceholderText("最大");
        minEdit->setFixedHeight(24);
        maxEdit->setFixedHeight(24);

        // 优化 QComboBox 样式 (使用物理 SVG 三角形并紧凑布局)
        QString arrowPath = UiHelper::getSvgTempFilePath("menu_triangle", QColor("#AAAAAA"));
        unitCombo->setFixedHeight(24);
        unitCombo->setFixedWidth(52); 
        unitCombo->setStyleSheet(QString(
            "QComboBox { background: #2D2D2D; color: #EEEEEE; border: 1px solid #444444; border-radius: 4px; font-size: 11px; padding-left: 6px; }"
            "QComboBox::drop-down { border: none; width: 18px; }"
            "QComboBox::down-arrow { image: url(%1); width: 10px; height: 10px; }"
            "QComboBox QAbstractItemView { background-color: #252526; color: #EEEEEE; selection-background-color: #3E3E42; border: 1px solid #444444; outline: none; }"
        ).arg(arrowPath));

        hs->addWidget(minEdit);
        QLabel* sep = new QLabel("-", g); sep->setStyleSheet("color: #AAA;"); hs->addWidget(sep);
        hs->addWidget(maxEdit);
        hs->addWidget(unitCombo);
        gl->addLayout(hs);

        auto updateSizeFilter = [this, minEdit, maxEdit, unitCombo]() {
            auto toBytes = [](const QString& txt, const QString& unit) -> long long {
                if (txt.isEmpty()) return -1;
                bool ok;
                double val = txt.toDouble(&ok);
                if (!ok) return -1;
                long long factor = 1024;
                if (unit == "MB") factor = 1024 * 1024;
                else if (unit == "GB") factor = 1024 * 1024 * 1024;
                return (long long)(val * factor);
            };
            m_filter.minSize = toBytes(minEdit->text(), unitCombo->currentText());
            m_filter.maxSize = toBytes(maxEdit->text(), unitCombo->currentText());
            emit filterChanged(m_filter);
        };

        connect(minEdit, &QLineEdit::editingFinished, this, updateSizeFilter);
        connect(minEdit, &QLineEdit::textChanged, this, [updateSizeFilter](const QString& text) {
            if (text.isEmpty()) updateSizeFilter();
        });
        connect(maxEdit, &QLineEdit::editingFinished, this, updateSizeFilter);
        connect(maxEdit, &QLineEdit::textChanged, this, [updateSizeFilter](const QString& text) {
            if (text.isEmpty()) updateSizeFilter();
        });
        connect(unitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [updateSizeFilter](int){ updateSizeFilter(); });

        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }


    // ── 11. 图像比例 (独立主选项) ──────────────────────────────────────────
    if (m_isMirrorSource) {
        QVBoxLayout* gl = nullptr;
        QWidget* g = buildGroup("图像比例", gl);
        m_groupRatio = g;

        static const QList<QPair<FilterState::AspectRatio, QString>> ratios = {
            {FilterState::Horizontal, "横图"}, {FilterState::Vertical, "竖图"},
            {FilterState::Square, "方形"}, {FilterState::Ratio169, "16:9"}
        };
        QButtonGroup* ratioGroup = new QButtonGroup(g);
        ratioGroup->setExclusive(false); // 改为非排他性
        for (const auto& pair : ratios) {
            StyledCheckBox* cb = new StyledCheckBox();
            ClickableRow* row = new ClickableRow(cb);
            row->setFixedHeight(24);
            QHBoxLayout* rl = new QHBoxLayout(row);
            rl->setContentsMargins(5, 0, 5, 0);
            rl->setSpacing(5);
            rl->addWidget(cb);
            QLabel* lbl = new QLabel(pair.second, row);
            lbl->setStyleSheet("font-size: 12px; color: #CCCCCC; background: transparent;");
            rl->addWidget(lbl, 1);
            gl->addWidget(row);

            if (m_filter.ratio == pair.first) cb->setChecked(true);
            connect(cb, &QCheckBox::toggled, this, [this, pair, ratioGroup, cb](bool on) {
                if (on) {
                    for (QAbstractButton* b : ratioGroup->buttons()) {
                        if (b != cb && b->isChecked()) {
                            b->blockSignals(true);
                            b->setChecked(false);
                            b->blockSignals(false);
                        }
                    }
                    m_filter.ratio = pair.first;
                } else {
                    m_filter.ratio = FilterState::AspectAny;
                }
                emit filterChanged(m_filter);
            });
            ratioGroup->addButton(cb);
        }
        m_containerLayout->insertWidget(m_containerLayout->count() - 1, g);
    }
}

// ─── buildGroup ───────────────────────────────────────────────────
// 2026-05-17 终极根因修复：引入独立 hdrRow(QWidget) 作为标题行容器
// hdr(QPushButton) 恢复原始有文字写法，绝对不携带任何子控件和内嵌布局
// btnCustomColor 等右侧按钮通过 outHdrLayout 追加到 hdrRow 的 QHBoxLayout 里
// 这是 Qt 最正规的写法，彻底消除 QPushButton 内嵌布局引发的渲染冲突和留白
QWidget* FilterPanel::buildGroup(const QString& title, QVBoxLayout*& outContentLayout,
                                  QHBoxLayout** outHdrLayout) {
    QWidget* wrapper = new QWidget(m_container);
    // 2026-05-17 根因修复：不使用 "QWidget { background: transparent; }" 类选择器
    // 该选择器会级联到所有子孙 QWidget，与 MainWindow 全局 QSS 相互叠加造成混乱
    // 改为仅对 wrapper 自身设置透明背景（借助 WA_StyledBackground 隔离传播）
    wrapper->setAttribute(Qt::WA_StyledBackground, true);
    wrapper->setStyleSheet("background: transparent;"); // 无选择器，仅作用于 wrapper 自身
    QVBoxLayout* wl = new QVBoxLayout(wrapper);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(0);

    // hdrRow：整个标题行的容器，负责背景色和上边框
    // 2026-05-17 终极根因修复：必须使用 ID 选择器 QWidget#GroupHdrRow
    // 原因：MainWindow 全局 QSS 有 #FilterContainer { background-color: #1E1E1E } 的规则，
    //       会级联到所有 QWidget 子孙，将 hdrRow 的背景强制变为 #1E1E1E（深黑色），
    //       造成标题行与内容区视觉上无法区分，形成"留白"假象。
    //       ID 选择器的 QSS 特异性高于祖先容器的规则，可以正确覆盖。
    QWidget* hdrRow = new QWidget(wrapper);
    hdrRow->setObjectName("GroupHdrRow");   // ← 关键：打上 ID 以提升 QSS 优先级
    hdrRow->setFixedHeight(24);
    hdrRow->setAttribute(Qt::WA_StyledBackground, true);
    hdrRow->setStyleSheet(
        "QWidget#GroupHdrRow {"            // ID 选择器，特异性高于全局级联
        "  background: #252526;"
        "  border-top: 1px solid #333333;"
        "}");

    QHBoxLayout* hdrRowLayout = new QHBoxLayout(hdrRow);
    hdrRowLayout->setContentsMargins(0, 0, 0, 0);
    hdrRowLayout->setSpacing(0);

    // 2026-05-07 按照用户要求：QPushButton（有文字，text-align left），纯净无子控件
    // 2026-05-17 终极修复：parent 改为 hdrRow，背景 transparent（由 hdrRow 统一提供）
    QPushButton* hdr = new QPushButton(title, hdrRow);
    hdr->setCheckable(true);
    hdr->setChecked(true);
    hdr->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    hdr->setFixedHeight(24);
    hdr->setStyleSheet(
        "QPushButton {"
        "  background: transparent;"   // hdrRow 已提供背景，此处透明即可
        "  border: none;"
        "  color: #AAAAAA;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "  text-align: left;"
        "  padding-left: 5px;"
        "  padding-right: 5px;"
        "  padding-top: 0px;"
        "  padding-bottom: 0px;"
        "  margin: 0px;"
        "}"
        "QPushButton:hover { color: #EEEEEE; }"
        "QPushButton:pressed { background: transparent; }");
    hdrRowLayout->addWidget(hdr);   // hdr 占满剩余宽度

    if (outHdrLayout) *outHdrLayout = hdrRowLayout; // 暴露 hdrRow 布局，供追加右侧按钮

    QWidget* content = new QWidget(wrapper);
    content->setAttribute(Qt::WA_StyledBackground, true);
    content->setStyleSheet("background: transparent;"); // 无选择器，仅作用于 content 自身
    outContentLayout = new QVBoxLayout(content);
    outContentLayout->setContentsMargins(0, 0, 0, 0);
    outContentLayout->setSpacing(0);

    connect(hdr, &QPushButton::toggled, content, &QWidget::setVisible);

    m_groupHeaders.append(hdr);
    // 2026-07-xx 按照 Plan-77：应用全局持久化折叠状态
    bool allCollapsed = AppConfig::instance().getValue("FilterPanel/AllGroupsCollapsed", false).toBool();
    if (allCollapsed) {
        hdr->setChecked(false);
    }

    wl->addWidget(hdrRow);     // 加入 hdrRow，不再直接加 hdr
    wl->addWidget(content);
    return wrapper;
}

// ─── addFilterRow ─────────────────────────────────────────────────
QCheckBox* FilterPanel::addFilterRow(QVBoxLayout* layout, const QString& label, int count, const QColor& dotColor) {
    StyledCheckBox* cb = new StyledCheckBox();

    // 整行可点击容器
    // 增加高度至 24px 以适配各种系统缩放，避免文字截断
    ClickableRow* row = new ClickableRow(cb);
    row->setFixedHeight(24);

    QHBoxLayout* rl = new QHBoxLayout(row);
    rl->setContentsMargins(5, 0, 5, 0);
    rl->setSpacing(5);
    rl->addWidget(cb);

    if (dotColor.isValid() && dotColor != Qt::transparent) {
        QLabel* dot = new QLabel(row);
        dot->setFixedSize(10, 10);
        dot->setStyleSheet(QString("background: %1; border-radius: 5px;").arg(dotColor.name()));
        rl->addWidget(dot);
    }

    QLabel* lbl = new QLabel(label, row);
    lbl->setStyleSheet("font-size: 12px; color: #CCCCCC; background: transparent;");
    rl->addWidget(lbl, 1);

    QLabel* cnt = new QLabel(QString::number(count), row);
    cnt->setStyleSheet("font-size: 11px; color: #555555; background: transparent;");
    cnt->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rl->addWidget(cnt);

    layout->addWidget(row);
    return cb;
}

// ─── clearAllFilters ──────────────────────────────────────────────
void FilterPanel::clearAllFilters(bool force) {
    // 2026-06-23 按照用户要求：若处于锁定状态且非强制重置，则拒绝清理
    if (!force && m_isFilterPinned) {
        return;
    }

    // 2026-06-23 按照用户要求：若按下“重置”按钮，则同步解除锁定状态
    if (force && m_isFilterPinned) {
        m_isFilterPinned = false;
        if (m_btnPin) {
            m_btnPin->setIcon(UiHelper::getIcon("pin_tilted", QColor("#B0B0B0")));
            m_btnPin->setProperty("tooltipText", "锁定当前筛选条件");
        }
    }

    // 2026-06-xx 物理修复：重置所有筛选内存状态
    m_filter = FilterState{};
    m_hueSliderColor.clear();

    // 2026-xx-xx 按照用户要求：清空剩余输入框的文字
    if (m_editColor) m_editColor->clear();
    if (m_editType) m_editType->clear();
    if (m_editCreateDate) m_editCreateDate->clear();
    if (m_editModifyDate) m_editModifyDate->clear();
    
    // 2026-06-xx 逻辑重构：由于 Plan-18 引入了色块矩阵，必须调用 rebuildGroups 
    // 以实现全量 UI 组件的选中态物理归零，杜绝手动遍历子控件的傻逼逻辑。
    rebuildGroups();
    
    // 2026-06-xx 按照用户铁律：点击“清除”仅重置筛选器内部状态，严禁干扰搜索框或上下文
    emit filterChanged(m_filter);
}

void FilterPanel::updateHeaderStatus() {
    if (!m_iconLabel || !m_titleLabel || !m_btnClearAll || !m_btnToggleGroups) return;
    
    bool active = !m_filter.isEmpty();
    
    // 标记 ①：始终保持彩色
    QColor brandYellow = QColor("#f1c40f");
    m_iconLabel->setPixmap(UiHelper::getIcon("filter_funnel_outline", brandYellow, 18).pixmap(18, 18));
    m_titleLabel->setStyleSheet(QString("font-size: 13px; font-weight: bold; color: %1; background: transparent; border: none;").arg(brandYellow.name()));

    // 标记 ②：根据筛选状态动态切换颜色（激活为彩色，空闲为灰色）
    QColor btnColor = active ? brandYellow : QColor("#B0B0B0");
    m_btnClearAll->setIcon(UiHelper::getIcon("reset_filter", btnColor));

    // 标记 ③：全局折叠按钮状态
    // 2026-07-xx 按照评审意见：校准图标映射。要求：展开状态(false)显示 down，折叠状态(true)显示 up
    bool allCollapsed = AppConfig::instance().getValue("FilterPanel/AllGroupsCollapsed", false).toBool();
    m_btnToggleGroups->setIcon(UiHelper::getIcon(allCollapsed ? "chevrons_up" : "chevrons_down", QColor("#B0B0B0"), 16));
    m_btnToggleGroups->setProperty("tooltipText", allCollapsed ? "展开所有分组" : "折叠所有分组");
}

void FilterPanel::onToggleAllGroupsClicked() {
    bool currentlyCollapsed = AppConfig::instance().getValue("FilterPanel/AllGroupsCollapsed", false).toBool();
    bool targetCollapsed = !currentlyCollapsed;

    for (QPushButton* hdr : m_groupHeaders) {
        if (hdr) hdr->setChecked(!targetCollapsed);
    }

    AppConfig::instance().setValue("FilterPanel/AllGroupsCollapsed", targetCollapsed);
    updateHeaderStatus();
}

void FilterPanel::setMirrorSource(bool isMirror) {
    if (m_isMirrorSource == isMirror) return;
    m_isMirrorSource = isMirror;
    
    // 2026-07-xx 按照 Plan-118：强制重绘以刷新受控分组显隐
    rebuildGroups();
}

void FilterPanel::selectColor(const QColor& color) {
    QString hex = color.name().toUpper();
    
    // 1. 设置当前过滤色（单选模式）
    m_filter.colors.clear();
    m_filter.colors.append(hex);

    // 2. 更新“最近筛选”列表（置顶、去重、上限50）
    m_recentColors.removeAll(hex);
    m_recentColors.prepend(hex);
    if (m_recentColors.size() > 50) m_recentColors.removeLast();

    // 3. 持久化
    AppConfig::instance().setValue("Filter/RecentColors", m_recentColors);

    // 4. 刷新 UI 表现（同步复选框与最近筛选色块）
    rebuildGroups();

    // 5. 驱动数据过滤
    emit filterChanged(m_filter);
}

} // namespace ArcMeta

