#include "MetaPanel.h"
#include "SvgIcons.h"
#include "ToolTipOverlay.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QMouseEvent>
#include <QScrollBar>
#include <QStyle>
#include <QScrollArea>
#include <QFileInfo>
#include <QLabel>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QDir>
#include <QAbstractTextDocumentLayout>
#include <QtMath>
#include <QTimer>
#include "Logger.h"
#include "UiHelper.h"
#include "StyleLibrary.h"
#include "../meta/MetadataManager.h"

namespace ArcMeta {

ElasticEdit::ElasticEdit(QWidget* parent) : QTextEdit(parent) {
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setLineWrapMode(QTextEdit::WidgetWidth); // 恢复为窗口宽度换行，由布局控制外部宽度
    
    // 工业级修复：设置换行策略，确保长文本（如物理路径）在无空格时也能强制换行
    QTextOption opt = document()->defaultTextOption();
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document()->setDefaultTextOption(opt);

    document()->setDocumentMargin(0);
    // 关键：QTextEdit 相比 QPlainTextEdit 提供了更稳定的高度属性反馈
    connect(this, &QTextEdit::textChanged, this, &ElasticEdit::adjustHeight);
}

void ElasticEdit::adjustHeight() {
    // 2026-06-xx 工业级重构：基类切换为 QTextEdit 后，使用渲染文档高度
    int horizontalPadding = 20; // 对应 QSS padding: 4px 10px;
    int verticalPadding = 8;    // 4px * 2;
    int border = 2;             // 1px * 2;
    
    int w = width();
    // 只有当宽度合理（>50）时才强行设置文档宽度，防止初始化阶段的 0 宽导致字符级换行
    if (w > 50) {
        int textW = w - horizontalPadding - border;
        if (document()->textWidth() != textW) {
            document()->setTextWidth(textW);
        }
    }

    // 获取文档的实际像素高度（QTextEdit 在内容改变后会自动更新此值）
    qreal docHeight = document()->size().height();
    
    // 计算目标高度：像素高度 + 上下边距 + 边框
    int newHeight = qMax(28, (int)qCeil(docHeight + verticalPadding + border)); 
    
    // 工业级日志：记录换行与高度计算详情
    Logger::log(QString("ElasticEdit [%1] Adjust: Width=%2, DocH=%3, TargetH=%4")
                .arg(placeholderText()).arg(w).arg(docHeight).arg(newHeight));

    if (this->height() != newHeight) {
        setFixedHeight(newHeight);
        updateGeometry(); 
        
        // 级联通知所有父布局刷新
        QWidget* p = parentWidget();
        while (p) {
            if (p->layout()) {
                p->layout()->activate();
            }
            if (qobject_cast<QScrollArea*>(p)) {
                break;
            }
            p = p->parentWidget();
        }
    }
}

void ElasticEdit::resizeEvent(QResizeEvent* e) {
    QTextEdit::resizeEvent(e);
    adjustHeight();
}

void ElasticEdit::keyPressEvent(QKeyEvent* e) {
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier)) {
        emit returnPressed();
        clearFocus();
        return;
    }
    QTextEdit::keyPressEvent(e);
}

ColorPill::ColorPill(const QColor& color, float ratio, QWidget* parent) 
    : QWidget(parent), m_color(color), m_ratio(ratio) {
    setFixedSize(16, 16);
    setCursor(Qt::PointingHandCursor);
}

void ColorPill::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_color);
    painter.drawRoundedRect(rect(), 4, 4);

    if (m_hovered) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::white, 1.0));
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 4, 4);
    }
}

void ColorPill::enterEvent(QEnterEvent*) {
    m_hovered = true;
    QString hex = m_color.name().toUpper();
    int ratio = qRound(m_ratio * 100);
    ToolTipOverlay::instance()->showText(QCursor::pos(), QString("%1 (%2%)").arg(hex).arg(ratio));
    update();
}

void ColorPill::leaveEvent(QEvent*) {
    m_hovered = false;
    ToolTipOverlay::hideTip();
    update();
}

void ColorPill::mousePressEvent(QMouseEvent* event) {
    QMenu menu(this);
    UiHelper::applyMenuStyle(&menu);
    QColor color = m_color;

    menu.addAction("搜索相似颜色的项目", [this, color]() {
        emit colorSelected(color);
    });
    menu.addSeparator();
    QString hex = color.name().toUpper();
    menu.addAction(QString("复制 %1").arg(hex), [hex]() { QApplication::clipboard()->setText(hex); });
    
    menu.exec(event->globalPosition().toPoint());
    QWidget::mousePressEvent(event);
}

// --- TagPill ---
TagPill::TagPill(const QString& text, QWidget* parent) 
    : QWidget(parent), m_text(text) {
    setProperty("tagText", text);
    setFixedHeight(22);
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 4, 0);
    layout->setSpacing(4);
    QLabel* lbl = new QLabel(text, this);
    lbl->setStyleSheet("color: #EEEEEE; font-size: 12px; border: none; background: transparent;");
    m_closeBtn = new QPushButton(this);
    m_closeBtn->setFixedSize(14, 14);
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setIcon(UiHelper::getIcon("close", QColor("#B0B0B0"), 12));
    m_closeBtn->setIconSize(QSize(10, 10));
    m_closeBtn->setStyleSheet("QPushButton { border: none; background: transparent; } QPushButton:hover { background: rgba(255, 255, 255, 0.1); border-radius: 2px; }");
    layout->addWidget(lbl);
    layout->addWidget(m_closeBtn);
    connect(m_closeBtn, &QPushButton::clicked, [this]() { emit deleteRequested(m_text); });
    QFontMetrics fm(lbl->font());
    setFixedWidth(fm.horizontalAdvance(text) + 30); 
}

void TagPill::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor("#2B2B2B"));
    // 统一边框颜色
    painter.setPen(QPen(QColor("#3c3c3c"), 1));
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 11, 11);
}

// --- FlowLayout ---
FlowLayout::FlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}
FlowLayout::~FlowLayout() {
    QLayoutItem *item;
    while ((item = takeAt(0))) delete item;
}
void FlowLayout::addItem(QLayoutItem *item) { itemList.append(item); }
int FlowLayout::horizontalSpacing() const { return m_hSpace >= 0 ? m_hSpace : 4; }
int FlowLayout::verticalSpacing() const { return m_vSpace >= 0 ? m_vSpace : 4; }
int FlowLayout::count() const { return itemList.size(); }
QLayoutItem *FlowLayout::itemAt(int index) const { return itemList.value(index); }
QLayoutItem *FlowLayout::takeAt(int index) { return (index >= 0 && index < itemList.size()) ? itemList.takeAt(index) : nullptr; }
Qt::Orientations FlowLayout::expandingDirections() const { return Qt::Orientations(); }
bool FlowLayout::hasHeightForWidth() const { return true; }
int FlowLayout::heightForWidth(int width) const { return doLayout(QRect(0, 0, width, 0), true); }
void FlowLayout::setGeometry(const QRect &rect) { QLayout::setGeometry(rect); doLayout(rect, false); }
QSize FlowLayout::sizeHint() const { return minimumSize(); }
QSize FlowLayout::minimumSize() const {
    QSize size;
    for (QLayoutItem *item : itemList) size = size.expandedTo(item->minimumSize());
    size += QSize(2 * contentsMargins().top(), 2 * contentsMargins().top());
    return size;
}
int FlowLayout::doLayout(const QRect &rect, bool testOnly) const {
    int left, top, right, bottom;
    getContentsMargins(&left, &top, &right, &bottom);
    QRect effectiveRect = rect.adjusted(+left, +top, -right, -bottom);
    int x = effectiveRect.x();
    int y = effectiveRect.y();
    int lineHeight = 0;
    for (QLayoutItem *item : itemList) {
        int spaceX = horizontalSpacing();
        int spaceY = verticalSpacing();
        int nextX = x + item->sizeHint().width() + spaceX;
        if (nextX - spaceX > effectiveRect.right() && lineHeight > 0) {
            x = effectiveRect.x();
            y = y + lineHeight + spaceY;
            nextX = x + item->sizeHint().width() + spaceX;
            lineHeight = 0;
        }
        if (!testOnly) item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
        x = nextX;
        lineHeight = qMax(lineHeight, item->sizeHint().height());
    }
    return y + lineHeight - rect.y() + bottom;
}

// --- StarRatingWidget ---
StarRatingWidget::StarRatingWidget(QWidget* parent) : QWidget(parent) { setFixedSize(5 * 18 + 4 * 1, 20); setCursor(Qt::PointingHandCursor); }
void StarRatingWidget::setRating(int rating) { m_rating = rating; update(); }
void StarRatingWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); 
    int starSize = 18; int spacing = 1;
    QPixmap filledStar = UiHelper::getPixmap("star-svgrepo-com.svg", QSize(starSize, starSize), QColor("#EF9F27"));
    QPixmap emptyStar = UiHelper::getPixmap("star-rate-rating-outline-svgrepo-com.svg", QSize(starSize, starSize), QColor("#444444"));
    for (int i = 0; i < 5; ++i) { QRect r(i * (starSize + spacing), (height() - starSize) / 2, starSize, starSize); painter.drawPixmap(r, (i < m_rating) ? filledStar : emptyStar); }
}
void StarRatingWidget::mousePressEvent(QMouseEvent* e) {
    e->accept();
    int x = e->pos().x();
    int starSize = 18;
    int spacing = 1;
    int index = x / (starSize + spacing);
    if (index >= 0 && index < 5) {
        int newRating = index + 1;
        if (newRating == m_rating) newRating = 0;
        setRating(newRating);
        emit ratingChanged(newRating);
    }
}

// --- ColorPickerWidget ---
ColorPickerWidget::ColorPickerWidget(QWidget* parent) : QWidget(parent) {
    m_colors = {{L"", QColor("#888780")}, {L"red", QColor("#E24B4A")}, {L"orange", QColor("#EF9F27")}, {L"yellow", QColor("#FECF0E")}, {L"green", QColor("#639922")}, {L"cyan", QColor("#1D9E75")}, {L"blue", QColor("#378ADD")}, {L"purple", QColor("#7F77DD")}, {L"gray", QColor("#5F5E5A")}};
    setFixedSize((int)m_colors.size() * 24, 24); setCursor(Qt::PointingHandCursor);
}
void ColorPickerWidget::setColor(const std::wstring& name) {
    m_currentColor = name;
    if (m_colors.size() > 9) m_colors.resize(9);
    if (!name.empty() && name[0] == L'#') {
        QColor customColor(QString::fromStdWString(name));
        if (customColor.isValid()) m_colors.push_back({name, customColor});
    }
    setFixedSize((int)m_colors.size() * 24, 24);
    update();
}
void ColorPickerWidget::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < (int)m_colors.size(); ++i) {
        QRect r(i * 24 + 3, 3, 18, 18);
        if (m_colors[i].name == m_currentColor) { p.setPen(QPen(QColor("#FFFFFF"), 1.5)); p.drawEllipse(r.adjusted(-2, -2, 2, 2)); }
        p.setPen(Qt::NoPen); p.setBrush(m_colors[i].value); p.drawEllipse(r);
    }
}
void ColorPickerWidget::mousePressEvent(QMouseEvent* e) {
    e->accept();
    int x = e->pos().x();
    int index = x / 24;
    if (index >= 0 && index < (int)m_colors.size()) {
        setColor(m_colors[index].name);
        emit colorChanged(m_colors[index].name);
    }
}

// --- MetaPanel ---
MetaPanel::MetaPanel(QWidget* parent) : QFrame(parent) {
    setObjectName("MetadataContainer"); setAttribute(Qt::WA_StyledBackground, true); setMinimumWidth(230); 
    setStyleSheet("color: #EEEEEE;");
    m_mainLayout = new QVBoxLayout(this); m_mainLayout->setContentsMargins(0, 0, 0, 0); m_mainLayout->setSpacing(0);
    initUi();
}

void MetaPanel::initUi() {
    QWidget* header = new QWidget(this); header->setObjectName("ContainerHeader"); header->setFixedHeight(32);
    header->setStyleSheet("QWidget#ContainerHeader { background-color: #252526; border-bottom: 1px solid #333; }");
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(15, 0, 5, 0);
    headerLayout->setSpacing(5);
    QLabel* iconLabel = new QLabel(header); iconLabel->setPixmap(UiHelper::getIcon("all_data", QColor("#4a90e2"), 18).pixmap(18, 18)); headerLayout->addWidget(iconLabel);
    QLabel* titleLabel = new QLabel("元数据", header); titleLabel->setStyleSheet("font-size: 12px; color: #4a90e2; background: transparent; border: none;"); headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    QPushButton* closeBtn = new QPushButton(header); closeBtn->setIcon(UiHelper::getIcon("close", QColor("#FFFFFF"), 14)); closeBtn->setFixedSize(24, 24); closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet("QPushButton { background-color: #E81123; border: none; border-radius: 4px; } QPushButton:hover { background-color: #F1707A; } QPushButton:pressed { background-color: #A50000; }");
    connect(closeBtn, &QPushButton::clicked, [this]() { this->hide(); });
    headerLayout->addWidget(closeBtn, 0, Qt::AlignVCenter);
    m_mainLayout->addWidget(header);

    m_scrollArea = new QScrollArea(this); m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setWidgetResizable(true); m_scrollArea->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    m_container = new QWidget(m_scrollArea); 
    m_containerLayout = new QVBoxLayout(m_container); 
    // 2026-06-xx 工业级强制约束：启用 SetMinAndMaxSize，强制容器高度随子控件动态撑开
    m_containerLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    // 2026-06-xx 物理对齐：右侧边距设为 0，使滚动条贴合容器边缘
    m_containerLayout->setContentsMargins(10, 10, 0, 10); 
    // 2026-06-01 修正：降低全局间距，消除视觉断层 (原 12px -> 现 8px)
    m_containerLayout->setSpacing(8);
    
    // [Section 1] 调色盘容器 (Palette Box - 模拟 ElasticEdit 样式且支持流式布局)
    m_paletteBox = new QWidget(m_container);
    // 工业级视觉统一：最小高度锁定为 28px，1px 边框 (#3c3c3c)，深色背景 (#252526)，4px 圆角
    m_paletteBox->setMinimumHeight(28);
    m_paletteBox->setStyleSheet("QWidget { background: #252526; border: 1px solid #3c3c3c; border-radius: 4px; }");
    
    // 内边距微调：左右 10px 保持对齐，上下 6px 确保在 28px 高度下色块垂直居中
    m_paletteFlowLayout = new FlowLayout(m_paletteBox, 6, 6, 6);
    m_paletteFlowLayout->setContentsMargins(10, 6, 10, 6);
    m_containerLayout->addWidget(m_paletteBox);

    // [Section 2] 名称输入框 (ElasticEdit)
    m_nameEdit = new ElasticEdit(m_container);
    m_nameEdit->setPlaceholderText("文件名称...");
    // 2026-06-xx 视觉加固：使用通配符选择器确保基类重构为 QTextEdit 后样式依然生效
    m_nameEdit->setStyleSheet("QTextEdit { background: #252526; border: 1px solid #3c3c3c; border-radius: 4px; padding: 4px 10px; font-size: 12px; color: #EEEEEE; font-weight: normal; }");
    m_nameEdit->installEventFilter(this);
    m_containerLayout->addWidget(m_nameEdit);

    // [Section 3] 备注输入框 (ElasticEdit)
    m_noteEdit = new ElasticEdit(m_container);
    m_noteEdit->setPlaceholderText("添加备注说明...");
    m_noteEdit->setStyleSheet("QTextEdit { background: #252526; border: 1px solid #3c3c3c; border-radius: 4px; padding: 4px 10px; font-size: 12px; color: #AAAAAA; font-weight: normal; }");
    m_noteEdit->installEventFilter(this);
    m_containerLayout->addWidget(m_noteEdit);

    // [Section 4] 链接输入框 (ElasticEdit)
    m_linkEdit = new ElasticEdit(m_container);
    m_linkEdit->setPlaceholderText("添加链接...");
    m_linkEdit->setStyleSheet("QTextEdit { background: #252526; border: 1px solid #3c3c3c; border-radius: 4px; padding: 4px 10px; font-size: 12px; color: #4a90e2; font-weight: normal; }");
    m_linkEdit->installEventFilter(this);
    m_containerLayout->addWidget(m_linkEdit);

    // [Section 5] 标签区域 (Tag Flow)
    m_tagBox = new QWidget(m_container);
    QVBoxLayout* tagL = new QVBoxLayout(m_tagBox);
    tagL->setContentsMargins(0, 0, 0, 0);
    tagL->setSpacing(8);
    
    m_tagContainer = new QWidget(m_tagBox);
    m_tagFlowLayout = new FlowLayout(m_tagContainer, 0, 4, 4);
    tagL->addWidget(m_tagContainer);

    m_tagEdit = new ElasticEdit(m_tagBox);
    m_tagEdit->setPlaceholderText("输入标签...");
    // 工业级宽度对齐：统一使用 4px 圆角和 4px 10px padding，彻底消除视觉缺口
    m_tagEdit->setStyleSheet("QTextEdit { background: #252526; border: 1px solid #3c3c3c; border-radius: 4px; padding: 4px 10px; font-size: 12px; color: #AAAAAA; font-weight: normal; }");
    connect(m_tagEdit, &ElasticEdit::returnPressed, this, &MetaPanel::onTagAdded);
    tagL->addWidget(m_tagEdit);
    m_containerLayout->addWidget(m_tagBox);

    // [Section 6] 分类展示 (Category Pills)
    m_categoryEdit = new ElasticEdit(m_container);
    m_categoryEdit->setReadOnly(true);
    m_categoryEdit->setPlaceholderText("所属分类...");
    m_categoryEdit->setStyleSheet("QTextEdit { background: #252526; border: 1px solid #3c3c3c; border-radius: 4px; padding: 4px 8px; font-size: 12px; color: #EEEEEE; font-weight: normal; }");
    m_containerLayout->addWidget(m_categoryEdit);

    m_containerLayout->addWidget(createSeparator());

    // [Section 7] 详情网格 (基本信息)
    addInfoRow("类型", lblType); addInfoRow("大小", lblSize);
    addInfoRow("创建时间", lblCtime); addInfoRow("修改时间", lblMtime); addInfoRow("访问时间", lblAtime);
    
    // 2026-06-xx 工业级重构：物理路径升级为只读 ElasticEdit，彻底解决超长路径不换行与截断问题
    QWidget* pathRow = new QWidget(m_container); 
    QHBoxLayout* pathL = new QHBoxLayout(pathRow);
    pathL->setContentsMargins(0, 2, 0, 2); 
    pathL->setSpacing(8);
    QLabel* pathKey = new QLabel("物理路径", pathRow);
    pathKey->setFixedWidth(80);
    pathKey->setStyleSheet("font-size: 12px; color: #888888;");
    pathL->addWidget(pathKey, 0, Qt::AlignTop);
    
    m_pathEdit = new ElasticEdit(pathRow);
    m_pathEdit->setReadOnly(true);
    // 视觉降权：去除背景和边框，使其融入信息列表，但保留强制换行特性
    m_pathEdit->setStyleSheet("QTextEdit { background: transparent; border: none; padding: 0; font-size: 12px; color: #CCCCCC; }");
    pathL->addWidget(m_pathEdit, 1);
    m_containerLayout->addWidget(pathRow);

    addInfoRow("加密状态", lblEncrypted);

    m_containerLayout->addStretch(1);
    m_scrollArea->setWidget(m_container);
    m_mainLayout->addWidget(m_scrollArea);
}

void MetaPanel::addInfoRow(const QString& label, QLabel*& valueLabel) {
    QWidget* row = new QWidget(m_container); 
    QHBoxLayout* rl = new QHBoxLayout(row); 
    // 2026-06-01 视觉密度优化：压缩行间距 (原 4px -> 现 2px)
    rl->setContentsMargins(0, 2, 0, 2); 
    rl->setSpacing(8); 
    
    QLabel* kl = new QLabel(label, row); 
    kl->setFixedWidth(80); // 适度增加宽度以支持长标签
    kl->setStyleSheet("font-size: 12px; color: #888888;"); 
    rl->addWidget(kl, 0, Qt::AlignTop);

    valueLabel = new QLabel("-", row); 
    valueLabel->setWordWrap(true); 
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse); // 允许复制路径等物理信息
    valueLabel->setStyleSheet("font-size: 12px; color: #CCCCCC; line-height: 1.5;");
    valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop); 
    rl->addWidget(valueLabel, 1); 
    
    m_containerLayout->addWidget(row);
}

QFrame* MetaPanel::createSeparator() { QFrame* l = new QFrame(this); l->setFrameShape(QFrame::HLine); l->setFixedHeight(1); l->setStyleSheet("background-color: #333333; border: none; margin: 4px 0;"); return l; }

QWidget* MetaPanel::createSectionBox(const QString& iconName, const QString& title, QWidget* content) {
    QFrame* box = new QFrame(this); box->setStyleSheet("QFrame { background-color: transparent; border: none; }");
    QVBoxLayout* layout = new QVBoxLayout(box); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(4);
    QHBoxLayout* header = new QHBoxLayout(); header->setSpacing(8);
    QLabel* iconLbl = new QLabel(box); iconLbl->setPixmap(UiHelper::getIcon(iconName, QColor("#888888"), 16).pixmap(16, 16)); header->addWidget(iconLbl);
    QLabel* titleLbl = new QLabel(title, box); titleLbl->setStyleSheet("font-size: 12px; color: #888888; text-transform: uppercase;"); header->addWidget(titleLbl);
    header->addStretch(); layout->addLayout(header); layout->addWidget(content); return box;
}

void MetaPanel::onTagAdded() {
    QString text = m_tagEdit->toPlainText().trimmed();
    if (!text.isEmpty()) {
        QString currentPath = m_pathEdit->toPlainText().trimmed();
        if (currentPath != "-" && !currentPath.isEmpty()) {
            std::wstring wPath = currentPath.toStdWString();
            RuntimeMeta rm = MetadataManager::instance().getMeta(wPath);
            if (!rm.tags.contains(text)) {
                rm.tags << text; MetadataManager::instance().setTags(wPath, rm.tags);
                TagPill* pill = new TagPill(text, m_tagContainer); connect(pill, &TagPill::deleteRequested, this, &MetaPanel::onTagDeleted); m_tagFlowLayout->addWidget(pill);
            }
        }
        m_tagEdit->clear();
        m_tagEdit->adjustHeight();
    }
}

void MetaPanel::onTagDeleted(const QString& text) {
    for (int i = 0; i < m_tagFlowLayout->count(); ++i) {
        QLayoutItem* item = m_tagFlowLayout->itemAt(i); TagPill* pill = qobject_cast<TagPill*>(item->widget());
        if (pill && pill->property("tagText").toString() == text) {
            m_tagFlowLayout->takeAt(i); pill->deleteLater(); delete item;
            QString currentPath = m_pathEdit->toPlainText().trimmed();
            if (currentPath != "-" && !currentPath.isEmpty()) {
                std::wstring wPath = currentPath.toStdWString(); RuntimeMeta rm = MetadataManager::instance().getMeta(wPath); rm.tags.removeAll(text); MetadataManager::instance().setTags(wPath, rm.tags);
            }
            return;
        }
    }
}

void MetaPanel::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    
    // 2026-06-xx 工业级加固：由于 ScrollArea 可能刚出现滚动条导致 Viewport 变化，
    // 使用异步触发确保获取到的是最终稳定的物理宽度，防止因宽度抖动导致的错误换行。
    QTimer::singleShot(0, this, [this]() {
        if (!m_scrollArea || !m_container) return;

        // 获取视口真实物理宽度
        int viewportW = m_scrollArea->viewport()->width();
        
        // 如果视口过窄（通常发生在初始化或隐藏状态），则延迟处理，避免触发字符级换行
        if (viewportW < 100) return;

        // 1. 同步容器宽度
        if (m_container->width() != viewportW) {
            m_container->setFixedWidth(viewportW);
        }
        
        // 2. 内部控件可用宽度（视口宽 - 左边距 10px - 右边距 0px）
        int maxW = viewportW - 10; 
        if (maxW > 50) {
            auto syncWidthAndHeight = [maxW](ElasticEdit* edit) {
                if (edit && edit->width() != maxW) {
                    edit->setFixedWidth(maxW);
                    edit->adjustHeight();
                }
            };

            syncWidthAndHeight(m_nameEdit);
            syncWidthAndHeight(m_noteEdit);
            syncWidthAndHeight(m_linkEdit);
            syncWidthAndHeight(m_tagEdit);
            syncWidthAndHeight(m_categoryEdit);
            
            // 物理路径宽度：视口宽 - 边距(20) - 标签宽(80) - 间距(8)
            int pathW = maxW - 88;
            if (m_pathEdit && pathW > 0) {
                m_pathEdit->setFixedWidth(pathW);
                m_pathEdit->adjustHeight();
            }
            
            if (m_paletteBox) m_paletteBox->setFixedWidth(maxW);
            if (m_tagBox) m_tagBox->setFixedWidth(maxW);
            if (m_tagContainer) m_tagContainer->setFixedWidth(maxW);
            
            adjustFlowHeights();

            // 3. 强制容器重算高度以撑开滚动区域
            m_container->adjustSize();
        }
    });
}

void MetaPanel::adjustFlowHeights() {
    // 1. 调整调色盘容器高度
    if (m_paletteBox && m_paletteFlowLayout) {
        int contentH = m_paletteFlowLayout->heightForWidth(m_paletteBox->width());
        int newH = qMax(28, contentH);
        if (m_paletteBox->height() != newH) {
            m_paletteBox->setFixedHeight(newH);
        }
    }
    // 2. 调整标签展示容器高度
    if (m_tagContainer && m_tagFlowLayout) {
        int contentH = m_tagFlowLayout->heightForWidth(m_tagContainer->width());
        if (m_tagContainer->height() != contentH) {
            m_tagContainer->setFixedHeight(contentH);
        }
    }
}

void MetaPanel::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    // 初始显示时强制触发一次几何更新
    QResizeEvent e(size(), size());
    MetaPanel::resizeEvent(&e);
}

void MetaPanel::updateInfo(const QString& n, const QString& t, const QString& s, const QString& ct, const QString& mt, const QString& at, const QString& p, bool e) {
    Logger::log(QString("MetaPanel::updateInfo for Path: %1").arg(p));
    m_nameEdit->blockSignals(true);
    QFileInfo info(n);
    m_nameEdit->setPlainText(info.completeBaseName());
    m_nameEdit->adjustHeight();
    m_nameEdit->setProperty("oldPath", p);
    m_nameEdit->setProperty("suffix", info.suffix());
    m_nameEdit->blockSignals(false);
    
    lblType->setText(t); lblSize->setText(s); lblCtime->setText(ct); lblMtime->setText(mt); lblAtime->setText(at); 
    
    m_pathEdit->blockSignals(true);
    m_pathEdit->setPlainText(p);
    m_pathEdit->adjustHeight();
    m_pathEdit->blockSignals(false);

    lblEncrypted->setText(e ? "已加密" : "未加密");
    
    if (p != "-" && !p.isEmpty()) {
        RuntimeMeta rm = MetadataManager::instance().getMeta(p.toStdWString());
        setNote(rm.note);
        setURL(rm.url);
        setTags(rm.tags);
        
        QVector<QPair<QColor, float>> pal;
        for (const auto& entry : rm.palettes) {
            pal.append({entry.color, entry.ratio});
        }
        setPalettes(pal);
    }
    if (m_container) m_container->adjustSize();
}

void MetaPanel::setRating(int rating) { 
    // 实现缺失的接口逻辑
    emit metadataChanged(rating, L"__NO_CHANGE__");
}
void MetaPanel::setColor(const std::wstring& color) { 
    // 实现缺失的接口逻辑
    emit metadataChanged(-1, color);
}
void MetaPanel::setPinned(bool pinned) { 
    Q_UNUSED(pinned); 
    // 这里如果需要持久化 Pin 状态，应调用相关接口
}
void MetaPanel::setTags(const QStringList& tags) {
    while (QLayoutItem* item = m_tagFlowLayout->takeAt(0)) { if (QWidget* w = item->widget()) w->deleteLater(); delete item; }
    for (const QString& tag : tags) { TagPill* pill = new TagPill(tag, m_tagContainer); connect(pill, &TagPill::deleteRequested, this, &MetaPanel::onTagDeleted); m_tagFlowLayout->addWidget(pill); }
}
void MetaPanel::setNote(const std::wstring& note) { 
    m_noteEdit->blockSignals(true); 
    m_noteEdit->setPlainText(QString::fromStdWString(note)); 
    m_noteEdit->adjustHeight();
    m_noteEdit->blockSignals(false); 
    if (m_container) m_container->adjustSize();
}
void MetaPanel::setURL(const std::wstring& url) { 
    m_linkEdit->blockSignals(true); 
    m_linkEdit->setPlainText(QString::fromStdWString(url)); 
    m_linkEdit->adjustHeight();
    m_linkEdit->blockSignals(false); 
    if (m_container) m_container->adjustSize();
}
void MetaPanel::setCategory(const QString& category) { 
    m_categoryEdit->blockSignals(true);
    m_categoryEdit->setPlainText(category); 
    m_categoryEdit->adjustHeight();
    m_categoryEdit->blockSignals(false);
    if (m_container) m_container->adjustSize();
}

void MetaPanel::setPalettes(const QVector<QPair<QColor, float>>& palette) {
    if (!m_paletteFlowLayout) return;

    // 清空现有色块
    while (QLayoutItem* item = m_paletteFlowLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    // 动态添加新色块
    for (const auto& entry : palette) {
        ColorPill* pill = new ColorPill(entry.first, entry.second, m_paletteBox);
        // 关键：子控件背景设为透明，否则会遮挡容器的圆角边框
        pill->setStyleSheet("background: transparent; border: none;");
        connect(pill, &ColorPill::colorSelected, this, &MetaPanel::searchByColor);
        m_paletteFlowLayout->addWidget(pill);
    }

    if (m_container) {
        m_container->adjustSize();
        // 动态添加色块后，立即根据当前宽度计算容器高度，防止“死板”高度截断内容
        adjustFlowHeights();
    }
}

bool MetaPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_noteEdit && event->type() == QEvent::FocusOut) {
        QString currentPath = m_pathEdit->toPlainText().trimmed(); if (currentPath != "-" && !currentPath.isEmpty()) MetadataManager::instance().setNote(currentPath.toStdWString(), m_noteEdit->toPlainText().toStdWString());
    } else if (watched == m_linkEdit && event->type() == QEvent::FocusOut) {
        QString currentPath = m_pathEdit->toPlainText().trimmed(); if (currentPath != "-" && !currentPath.isEmpty()) MetadataManager::instance().setURL(currentPath.toStdWString(), m_linkEdit->toPlainText().toStdWString());
    } else if (watched == m_nameEdit && event->type() == QEvent::FocusOut) {
        QString oldPath = m_nameEdit->property("oldPath").toString();
        QString newName = m_nameEdit->toPlainText().trimmed();
        
        // 2026-06-xx 物理加固：过滤非法文件名字符，防止重命名失败或破坏路径
        static const QString illegalChars = "\\/:*?\"<>|";
        for (auto c : illegalChars) newName.remove(c);
        m_nameEdit->setPlainText(newName);

        QString suffix = m_nameEdit->property("suffix").toString();
        if (!oldPath.isEmpty() && !newName.isEmpty()) {
            QFileInfo oldInfo(oldPath);
            if (newName != oldInfo.completeBaseName()) {
                QString newPath = oldInfo.absolutePath() + "/" + newName + (suffix.isEmpty() ? "" : "." + suffix);
                newPath = QDir::toNativeSeparators(newPath);
                
                // 2026-06-xx 工业级改进：检查目标路径是否已存在
                if (QFile::exists(newPath)) {
                    m_nameEdit->setPlainText(oldInfo.completeBaseName());
                    return true;
                }

                if (QFile::rename(oldPath, newPath)) {
                    MetadataManager::instance().renameItem(oldPath.toStdWString(), newPath.toStdWString());
                    m_pathEdit->setPlainText(newPath);
                    m_pathEdit->adjustHeight();
                    m_nameEdit->setProperty("oldPath", newPath);
                } else {
                    // 重命名失败，回滚文本
                    m_nameEdit->setPlainText(oldInfo.completeBaseName());
                }
            }
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace ArcMeta
