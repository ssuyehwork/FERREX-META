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
#include <QWidgetAction>
#include <QLineEdit>
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
    : QWidget(parent) {
    setFixedSize(16, 16);
    setCursor(Qt::PointingHandCursor);
    setData(color, ratio);
}

void ColorPill::setData(const QColor& color, float ratio) {
    m_color = color;
    m_ratio = ratio;
    update();
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
    // 2026-07-xx 按照 Plan-65：悬停触发，timeout = 0
    ToolTipOverlay::instance()->showText(QCursor::pos(), QString("%1 (%2%)").arg(hex).arg(ratio), 0);
    update();
}

void ColorPill::leaveEvent(QEvent*) {
    m_hovered = false;
    ToolTipOverlay::hideTip();
    update();
}

void ColorPill::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        emit colorSelected(m_color);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        QMenu menu(this);
        UiHelper::applyMenuStyle(&menu);
        
        // 顶部搜索框模拟
        QWidgetAction* searchAction = new QWidgetAction(&menu);
        QWidget* searchWidget = new QWidget(&menu);
        QHBoxLayout* searchLayout = new QHBoxLayout(searchWidget);
        searchLayout->setContentsMargins(8, 4, 8, 4);
        searchLayout->setSpacing(8);
        QLabel* searchIcon = new QLabel(searchWidget);
        searchIcon->setPixmap(UiHelper::getIcon("search", QColor("#888888"), 14).pixmap(14, 14));
        searchLayout->addWidget(searchIcon);
        QLineEdit* searchEdit = new QLineEdit(searchWidget);
        searchEdit->setPlaceholderText("搜索...");
        searchEdit->setStyleSheet("QLineEdit { background: transparent; border: none; color: #EEEEEE; font-size: 12px; }");
        searchLayout->addWidget(searchEdit);
        searchAction->setDefaultWidget(searchWidget);
        menu.addAction(searchAction);

        QColor color = m_color;
        menu.addAction("搜索相似颜色的项目", [this, color]() {
            emit colorSelected(color);
        });
        menu.addSeparator();

        // 各种颜色格式复制
        QString hex = color.name().toUpper();
        menu.addAction(QString("复制 %1").arg(hex), [hex]() { QApplication::clipboard()->setText(hex); });
        
        QString rgb = QString("rgb(%1, %2, %3)").arg(color.red()).arg(color.green()).arg(color.blue());
        menu.addAction(QString("复制 %1").arg(rgb), [rgb]() { QApplication::clipboard()->setText(rgb); });

        QString rgba = QString("rgba(%1, %2, %3, 1)").arg(color.red()).arg(color.green()).arg(color.blue());
        menu.addAction(QString("复制 %1").arg(rgba), [rgba]() { QApplication::clipboard()->setText(rgba); });

        QString hsl = QString("hsl(%1, %2%, %3%)").arg(color.hslHue() < 0 ? 0 : color.hslHue()).arg(qRound(color.hslSaturationF() * 100)).arg(qRound(color.lightnessF() * 100));
        menu.addAction(QString("复制 %1").arg(hsl), [hsl]() { QApplication::clipboard()->setText(hsl); });

        QString hsv = QString("hsv(%1, %2%, %3%)").arg(color.hsvHue() < 0 ? 0 : color.hsvHue()).arg(qRound(color.hsvSaturationF() * 100)).arg(qRound(color.valueF() * 100));
        menu.addAction(QString("复制 %1").arg(hsv), [hsv]() { QApplication::clipboard()->setText(hsv); });

        // HWB (Hue, Whiteness, Blackness) - Qt 不直接支持，需要计算
        double r = color.redF(), g = color.greenF(), b = color.blueF();
        double w = qMin(r, qMin(g, b));
        double v = qMax(r, qMax(g, b));
        double bk = 1.0 - v;
        QString hwb = QString("hwb(%1, %2%, %3%)").arg(color.hsvHue() < 0 ? 0 : color.hsvHue()).arg(qRound(w * 100)).arg(qRound(bk * 100));
        menu.addAction(QString("复制 %1").arg(hwb), [hwb]() { QApplication::clipboard()->setText(hwb); });

        QString cmyk = QString("cmyk(%1%, %2%, %3%, %4%)").arg(qRound(color.cyanF() * 100)).arg(qRound(color.magentaF() * 100)).arg(qRound(color.yellowF() * 100)).arg(qRound(color.blackF() * 100));
        menu.addAction(QString("复制 %1").arg(cmyk), [cmyk]() { QApplication::clipboard()->setText(cmyk); });

        menu.addSeparator();
        menu.addAction("设置为自定义主色", [this, color]() {
            emit requestSetAsPrimary(color);
        });
        
        menu.exec(event->globalPosition().toPoint());
    }
    QWidget::mousePressEvent(event);
}

// --- TagPill ---
TagPill::TagPill(const QString& text, QWidget* parent) 
    : QWidget(parent), m_text(text) {
    setFixedHeight(22);
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 4, 0);
    layout->setSpacing(4);
    m_label = new QLabel(text, this);
    m_label->setStyleSheet("color: #EEEEEE; font-size: 12px; border: none; background: transparent;");
    m_closeBtn = new QPushButton(this);
    m_closeBtn->setFixedSize(14, 14);
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setIcon(UiHelper::getIcon("close", QColor("#B0B0B0"), 12));
    m_closeBtn->setIconSize(QSize(10, 10));
    m_closeBtn->setStyleSheet("QPushButton { border: none; background: transparent; } QPushButton:hover { background: #3E3E42; border-radius: 2px; }");
    layout->addWidget(m_label);
    layout->addWidget(m_closeBtn);
    connect(m_closeBtn, &QPushButton::clicked, [this]() { emit deleteRequested(m_text); });
    setData(text);
}

void TagPill::setData(const QString& text) {
    m_text = text;
    setProperty("tagText", text);
    m_label->setText(text);
    QFontMetrics fm(m_label->font());
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
void FlowLayout::addItem(QLayoutItem *item) { itemList.append(item); invalidate(); }
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


// --- MetaPanel ---
MetaPanel::MetaPanel(QWidget* parent) : QFrame(parent) {
    setObjectName("MetadataContainer"); setAttribute(Qt::WA_StyledBackground, true); setMinimumWidth(230); 
    setStyleSheet("color: #EEEEEE;");
    m_mainLayout = new QVBoxLayout(this); m_mainLayout->setContentsMargins(0, 0, 0, 0); m_mainLayout->setSpacing(0);
    
    // 2026-06-xx 性能优化：为布局计算引入防抖计时器
    m_adjustTimer = new QTimer(this);
    m_adjustTimer->setSingleShot(true);
    m_adjustTimer->setInterval(50);
    connect(m_adjustTimer, &QTimer::timeout, this, &MetaPanel::adjustFlowHeights);

    // 2026-07-xx 按照 Plan-63：启用右键菜单
    setContextMenuPolicy(Qt::CustomContextMenu);
    initUi();
}

void MetaPanel::initUi() {
    QWidget* header = new QWidget(this); header->setObjectName("ContainerHeader"); header->setFixedHeight(32);
    header->setStyleSheet("QWidget#ContainerHeader { background-color: #252526; border-bottom: 1px solid #333; }");
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(15, 0, 5, 0); // 2026-xx-xx 按照用户要求：左侧 15px 对齐，右侧 5px 间距
    headerLayout->setSpacing(5);
    QLabel* iconLabel = new QLabel(header); iconLabel->setPixmap(UiHelper::getIcon("all_data", QColor("#4a90e2"), 18).pixmap(18, 18)); headerLayout->addWidget(iconLabel);
    QLabel* titleLabel = new QLabel("元数据", header); titleLabel->setStyleSheet("font-size: 12px; color: #4a90e2; background: transparent; border: none;"); headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    // 2026-07-xx 按照 Plan-63：移除标题栏物理关闭按钮，改为全局右键菜单统一控制
    m_mainLayout->addWidget(header);

    m_scrollArea = new QScrollArea(this); m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded); m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setWidgetResizable(true); m_scrollArea->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    m_container = new QWidget(m_scrollArea); 
    m_containerLayout = new QVBoxLayout(m_container); 
    // 2026-06-xx 工业级强制约束：启用 SetMinAndMaxSize，强制容器高度随子控件动态撑开
    m_containerLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    // 2026-06-xx 修复：恢复右侧间距，确保容器两侧对称
    m_containerLayout->setContentsMargins(10, 10, 10, 10); 
    // 2026-06-01 修正：降低全局间距，消除视觉断层 (原 12px -> 现 8px)
    m_containerLayout->setSpacing(8);
    
    // [Section 1] 调色盘容器 (Palette Box - 模拟 ElasticEdit 样式且支持流式布局)
    m_paletteBox = new QWidget(m_container);
    m_paletteBox->setObjectName("PaletteBox");
    // 工业级视觉统一：最小高度锁定为 28px，1px 边框 (#3c3c3c)，深色背景 (#252526)，4px 圆角
    m_paletteBox->setMinimumHeight(28);
    // 物理修复：使用 ID 选择器限制样式作用域，防止子控件 (ColorPill) 继承边框导致布局错位
    m_paletteBox->setStyleSheet("QWidget#PaletteBox { background: #252526; border: 1px solid #3c3c3c; border-radius: 4px; }");
    
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
    m_tagEdit->installEventFilter(this); // 2026-06-xx 物理修复：安装拦截器以支持 FocusOut 自动保存标签（可选）
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
        if (!m_selectedPaths.isEmpty()) {
            QStringList currentTags;
            for (const QString& path : m_selectedPaths) {
                std::wstring wPath = path.toStdWString();
                RuntimeMeta rm = MetadataManager::instance().getMeta(wPath);
                
                if (!rm.tags.contains(text)) {
                    rm.tags << text;
                    MetadataManager::instance().setTags(wPath, rm.tags);
                }
                if (path == m_selectedPaths.first()) currentTags = rm.tags;
            }
            // 重新刷新显示首个选中项的标签
            setTags(currentTags);
            emit tagsChanged(currentTags);
        }
        m_tagEdit->clear();
        m_tagEdit->adjustHeight();
    }
}

void MetaPanel::onTagDeleted(const QString& text) {
    if (m_selectedPaths.isEmpty()) return;

    QStringList currentTags;
    for (const QString& path : m_selectedPaths) {
        std::wstring wPath = path.toStdWString();
        RuntimeMeta rm = MetadataManager::instance().getMeta(wPath);

        if (rm.tags.contains(text)) {
            rm.tags.removeAll(text);
            MetadataManager::instance().setTags(wPath, rm.tags);
        }
        if (path == m_selectedPaths.first()) currentTags = rm.tags;
    }
    
    emit tagsChanged(currentTags);

    for (int i = 0; i < m_tagFlowLayout->count(); ++i) {
        QLayoutItem* item = m_tagFlowLayout->itemAt(i);
        TagPill* pill = qobject_cast<TagPill*>(item->widget());
        if (pill && pill->property("tagText").toString() == text) {
            m_tagFlowLayout->takeAt(i);
            pill->deleteLater();
            delete item;
            break;
        }
    }
    m_adjustTimer->start();
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
        
        // 2. 内部控件可用宽度（视口宽 - 左边距 10px - 右边距 10px）
        int maxW = viewportW - 20; 
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
        // 物理加固：强制激活布局计算，确保即便高度未变，新加入的色块也能正确排布而非堆叠在 (0,0)
        m_paletteFlowLayout->activate();
    }
    // 2. 调整标签展示容器高度
    if (m_tagContainer && m_tagFlowLayout) {
        int contentH = m_tagFlowLayout->heightForWidth(m_tagContainer->width());
        if (m_tagContainer->height() != contentH) {
            m_tagContainer->setFixedHeight(contentH);
        }
        m_tagFlowLayout->activate();
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
    // 2026-06-xx 物理移除
    Q_UNUSED(rating);
}
void MetaPanel::setColor(const std::wstring& color) { 
    // 2026-06-xx 物理移除
    Q_UNUSED(color);
}
void MetaPanel::setPinned(bool pinned) { 
    Q_UNUSED(pinned); 
    // 这里如果需要持久化 Pin 状态，应调用相关接口
}
void MetaPanel::setTags(const QStringList& tags) {
    // 1. 将现有 Pill 回收到池
    while (QLayoutItem* item = m_tagFlowLayout->takeAt(0)) {
        TagPill* pill = qobject_cast<TagPill*>(item->widget());
        if (pill) {
            pill->hide();
            m_tagPool.append(pill);
        }
        delete item;
    }

    // 2. 从池中复用或创建新 Pill
    for (const QString& tag : tags) {
        TagPill* pill = nullptr;
        if (!m_tagPool.isEmpty()) {
            pill = m_tagPool.takeFirst();
            pill->setData(tag);
        } else {
            pill = new TagPill(tag, m_tagContainer);
            connect(pill, &TagPill::deleteRequested, this, &MetaPanel::onTagDeleted);
        }
        pill->show();
        m_tagFlowLayout->addWidget(pill);
    }
    
    // 3. 异步触发高度调整
    m_adjustTimer->start();
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

    // 1. 回收到池
    while (QLayoutItem* item = m_paletteFlowLayout->takeAt(0)) {
        ColorPill* pill = qobject_cast<ColorPill*>(item->widget());
        if (pill) {
            pill->hide();
            m_colorPool.append(pill);
        }
        delete item;
    }

    // 2. 复用或创建
    for (const auto& entry : palette) {
        ColorPill* pill = nullptr;
        if (!m_colorPool.isEmpty()) {
            pill = m_colorPool.takeFirst();
            pill->setData(entry.first, entry.second);
        } else {
            pill = new ColorPill(entry.first, entry.second, m_paletteBox);
            pill->setStyleSheet("background: transparent; border: none;");
            connect(pill, &ColorPill::colorSelected, [this](const QColor& c){ emit searchByColor(c); });
            connect(pill, &ColorPill::requestSetAsPrimary, this, &MetaPanel::setAsPrimaryColor);
        }
        pill->show();
        m_paletteFlowLayout->addWidget(pill);
    }

    // 2026-07-xx 物理修复：在色块添加完毕后显式调用重排，防止初始化阶段堆叠在左上角
    m_paletteFlowLayout->invalidate();
    m_paletteBox->update();
    
    m_adjustTimer->start();
}

bool MetaPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_noteEdit && event->type() == QEvent::FocusOut) {
        if (!m_selectedPaths.isEmpty()) {
            std::wstring newNote = m_noteEdit->toPlainText().toStdWString();
            for (const QString& path : m_selectedPaths) {
                std::wstring wPath = path.toStdWString();
                RuntimeMeta rm = MetadataManager::instance().getMeta(wPath);

                if (newNote != rm.note) {
                    MetadataManager::instance().setNote(wPath, newNote);
                }
            }
        }
    } else if (watched == m_linkEdit && event->type() == QEvent::FocusOut) {
        if (!m_selectedPaths.isEmpty()) {
            std::wstring newUrl = m_linkEdit->toPlainText().toStdWString();
            for (const QString& path : m_selectedPaths) {
                std::wstring wPath = path.toStdWString();
                RuntimeMeta rm = MetadataManager::instance().getMeta(wPath);

                if (newUrl != rm.url) {
                    MetadataManager::instance().setURL(wPath, newUrl);
                }
            }
        }
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

void MetaPanel::setAsPrimaryColor(const QColor& color) {
    QString currentPath = m_pathEdit->toPlainText().trimmed();
    if (currentPath != "-" && !currentPath.isEmpty()) {
        MetadataManager::instance().setColor(currentPath.toStdWString(), color.name().toStdWString());
    }
}

} // namespace ArcMeta
