#include "MetaPanel.h"
#include "SvgIcons.h"
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
#include "UiHelper.h"
#include "../meta/MetadataManager.h"

namespace ArcMeta {

/**
 * @brief PaletteSwatch: 调色盘圆形色块组件 (16px)
 */
class PaletteSwatch : public QWidget {
public:
    explicit PaletteSwatch(const QColor& color, const QString& path, QWidget* parent = nullptr)
        : QWidget(parent), m_color(color), m_path(path) {
        setFixedSize(16, 16);
        setCursor(Qt::PointingHandCursor);
        setToolTip(color.name().toUpper());
    }

    void setSelected(bool selected) {
        m_selected = selected;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        
        // 绘制主圆
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_color);
        painter.drawEllipse(rect().adjusted(1, 1, -1, -1));

        // 选中态：白色外环 (参考 ColorPickerWidget)
        if (m_selected) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Qt::white, 1.5));
            painter.drawEllipse(rect().adjusted(1, 1, -1, -1));
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        setSelected(true);
        showMenu(event->globalPosition().toPoint());
    }

private:
    void showMenu(const QPoint& pos) {
        QMenu menu(this);
        UiHelper::applyMenuStyle(&menu);

        menu.addAction("搜索相似颜项目的项目", [this]() {
            // 通过父组件发射信号
            MetaPanel* panel = qobject_cast<MetaPanel*>(window()->findChild<MetaPanel*>("MetadataContainer"));
            if (!panel) {
                // 备选方案：向上寻找
                QWidget* p = this->parentWidget();
                while (p && !qobject_cast<MetaPanel*>(p)) p = p->parentWidget();
                panel = qobject_cast<MetaPanel*>(p);
            }
            if (panel) emit panel->searchByColor(m_color);
        });

        menu.addSeparator();

        menu.addAction(QString("复制 %1").arg(m_color.name().toUpper()), [this]() {
            QApplication::clipboard()->setText(m_color.name().toUpper());
        });

        menu.addAction(QString("复制 rgb(%1, %2, %3)").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue()), [this]() {
            QApplication::clipboard()->setText(QString("rgb(%1, %2, %3)").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue()));
        });

        menu.addAction(QString("复制 rgba(%1, %2, %3, 1)").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue()), [this]() {
            QApplication::clipboard()->setText(QString("rgba(%1, %2, %3, 1)").arg(m_color.red()).arg(m_color.green()).arg(m_color.blue()));
        });

        menu.addAction("复制 hsl格式", [this]() {
            int h, s, l;
            m_color.getHsl(&h, &s, &l);
            QApplication::clipboard()->setText(QString("hsl(%1, %2%, %3%)").arg(h == -1 ? 0 : h).arg(qRound(s/2.55)).arg(qRound(l/2.55)));
        });

        menu.addAction("复制 hsv格式", [this]() {
            int h, s, v;
            m_color.getHsv(&h, &s, &v);
            QApplication::clipboard()->setText(QString("hsv(%1, %2%, %3%)").arg(h == -1 ? 0 : h).arg(qRound(s/2.55)).arg(qRound(v/2.55)));
        });

        menu.addAction("复制 hwb格式", [this]() {
            // HWB: W = (1-S)*V, B = 1-V
            double h = m_color.hsvHueF(); if (h < 0) h = 0;
            double s = m_color.hsvSaturationF();
            double v = m_color.valueF();
            double w = (1.0 - s) * v;
            double b = 1.0 - v;
            QApplication::clipboard()->setText(QString("hwb(%1, %2%, %3%)").arg(qRound(h * 360)).arg(qRound(w * 100)).arg(qRound(b * 100)));
        });

        menu.addAction("复制 cmyk格式", [this]() {
            float c, m, y, k;
            m_color.getCmykF(&c, &m, &y, &k);
            QApplication::clipboard()->setText(QString("cmyk(%1%, %2%, %3%, %4%)")
                .arg(qRound(c * 100)).arg(qRound(m * 100)).arg(qRound(y * 100)).arg(qRound(k * 100)));
        });

        menu.addSeparator();

        menu.addAction("设置为自定义主色", [this]() {
            if (!m_path.isEmpty() && m_path != "-") {
                MetadataManager::instance().setColor(m_path.toStdWString(), m_color.name().toUpper().toStdWString());
            }
        });

        menu.exec(pos);
        setSelected(false);
    }

    QColor m_color;
    QString m_path;
    bool m_selected = false;
};

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
    painter.setPen(QPen(QColor("#444444"), 1));
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
        // 如果点击的是当前已选择的星级，则支持反选清除为 0
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
    // 2026-05-17 按照要求：如果有第 10 个提取色点，重置还原为前 9 个系统色点
    if (m_colors.size() > 9) {
        m_colors.resize(9);
    }
    // 动态检测是否为自定义的十六进制主色码，并作为第 10 个临时色点进行渲染以提供直观反馈
    if (!name.empty() && name[0] == L'#') {
        QColor customColor(QString::fromStdWString(name));
        if (customColor.isValid()) {
            m_colors.push_back({name, customColor});
        }
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
    headerLayout->setContentsMargins(15, 0, 5, 0); // 2026-05-17 按照用户要求：右侧边距统一设为 5px，消除 15px 留白
    headerLayout->setSpacing(5);                  // 2026-05-17 按照用户要求：间距统一为 5px
    QLabel* iconLabel = new QLabel(header); iconLabel->setPixmap(UiHelper::getIcon("all_data", QColor("#4a90e2"), 18).pixmap(18, 18)); headerLayout->addWidget(iconLabel);
    QLabel* titleLabel = new QLabel("元数据", header); titleLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #4a90e2; background: transparent; border: none;"); headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    QPushButton* closeBtn = new QPushButton(header); closeBtn->setIcon(UiHelper::getIcon("close", QColor("#FFFFFF"), 14)); closeBtn->setFixedSize(24, 24); closeBtn->setCursor(Qt::PointingHandCursor);
    // 按照用户要求：侧边栏关闭按钮同样强制常驻红色高亮
    closeBtn->setStyleSheet(
        "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
        "QPushButton:hover { background-color: #F1707A; } "
        "QPushButton:pressed { background-color: #A50000; }"
    );
    connect(closeBtn, &QPushButton::clicked, [this]() { this->hide(); });
    headerLayout->addWidget(closeBtn, 0, Qt::AlignVCenter);
    m_mainLayout->addWidget(header);
    m_scrollArea = new QScrollArea(this); m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setWidgetResizable(true); m_scrollArea->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    m_container = new QWidget(m_scrollArea); m_containerLayout = new QVBoxLayout(m_container); m_containerLayout->setContentsMargins(0, 0, 0, 0); m_containerLayout->setSpacing(0);
    addInfoRow("名称", lblName); addInfoRow("类型", lblType); addInfoRow("大小", lblSize);
    addInfoRow("创建时间", lblCtime); addInfoRow("修改时间", lblMtime); addInfoRow("访问时间", lblAtime);
    addInfoRow("物理路径", lblPath); addInfoRow("加密状态", lblEncrypted);

    // 2026-06-xx 调色盘显示行
    QWidget* paletteRow = new QWidget(m_container);
    QHBoxLayout* paletteL = new QHBoxLayout(paletteRow);
    paletteL->setContentsMargins(10, 4, 10, 4);
    paletteL->setSpacing(6);
    QLabel* palLbl = new QLabel("颜色", paletteRow);
    palLbl->setStyleSheet("font-size: 12px; color: #888888; font-weight: bold;");
    palLbl->setFixedWidth(60);
    paletteL->addWidget(palLbl);
    
    m_paletteContainer = new QWidget(paletteRow);
    m_paletteLayout = new QHBoxLayout(m_paletteContainer);
    m_paletteLayout->setContentsMargins(0, 0, 0, 0);
    m_paletteLayout->setSpacing(4);
    m_paletteLayout->addStretch();
    paletteL->addWidget(m_paletteContainer, 1);
    m_containerLayout->addWidget(paletteRow);

    m_containerLayout->addWidget(createSeparator());
    m_containerLayout->addSpacing(6);  // separator 后统一留 6px

    // 2026-05-07 按照用户要求：重新组织布局，统一间距节奏，消除留白
    // 备注 header
    QWidget* noteHeader = new QWidget(m_container);
    QHBoxLayout* noteHeaderL = new QHBoxLayout(noteHeader);
    noteHeaderL->setContentsMargins(10, 0, 8, 2);  // 上下对称，去掉 top:4
    noteHeaderL->setSpacing(6);
    QLabel* noteIcon = new QLabel(noteHeader);
    noteIcon->setPixmap(UiHelper::getIcon("edit", QColor("#888888"), 16).pixmap(16, 16));
    noteHeaderL->addWidget(noteIcon);
    QLabel* noteTitle = new QLabel("备注", noteHeader);
    noteTitle->setStyleSheet("font-size: 11px; font-weight: bold; color: #888888; text-transform: uppercase;");
    noteHeaderL->addWidget(noteTitle);
    noteHeaderL->addStretch();
    m_containerLayout->addWidget(noteHeader);

    // 备注输入框
    m_noteEdit = new QPlainTextEdit(this);
    m_noteEdit->setPlaceholderText("添加备注说明...");
    m_noteEdit->setFixedHeight(64);
    m_noteEdit->setStyleSheet(
        "QPlainTextEdit { background: transparent; border: none; "
        "font-size: 13px; color: #DDDDDD; padding: 2px 10px; }");
    m_noteEdit->installEventFilter(this);
    m_containerLayout->addWidget(m_noteEdit);
    m_containerLayout->addSpacing(5);

    // 标签 header
    QWidget* tagHeader = new QWidget(m_container);
    QHBoxLayout* tagHeaderL = new QHBoxLayout(tagHeader);
    tagHeaderL->setContentsMargins(10, 0, 8, 0);  // bottom 改为 0，避免叠加
    tagHeaderL->setSpacing(6);
    QLabel* tagIcon = new QLabel(tagHeader);
    tagIcon->setPixmap(UiHelper::getIcon("tag", QColor("#888888"), 16).pixmap(16, 16));
    tagHeaderL->addWidget(tagIcon);
    QLabel* tagTitle = new QLabel("标签", tagHeader);
    tagTitle->setStyleSheet("font-size: 11px; font-weight: bold; color: #888888; text-transform: uppercase;");
    tagHeaderL->addWidget(tagTitle);
    tagHeaderL->addStretch();
    m_containerLayout->addWidget(tagHeader);

    // 标签流式容器
    m_tagContainer = new QWidget(this);
    m_tagFlowLayout = new FlowLayout(m_tagContainer, 6, 4, 4);
    m_containerLayout->addWidget(m_tagContainer);
    m_containerLayout->addSpacing(5);  // 标签与输入框垂直间距 5px

    // 标签输入框
    m_tagEdit = new QLineEdit(m_container);
    m_tagEdit->setPlaceholderText("输入标签添加... (双击更多)");
    m_tagEdit->setFixedHeight(26);
    // 2026-05-07 按照用户要求：左右两边向内缩进5像素
    m_tagEdit->setStyleSheet(
        "QLineEdit { background: #252526; border: 1px solid #333333; "
        "border-radius: 5px; padding-left: 10px; margin-left: 5px; margin-right: 5px; "
        "font-size: 12px; color: #AAAAAA; } "
        "QLineEdit:focus { border-color: #4a90e2; color: #EEEEEE; }");
    connect(m_tagEdit, &QLineEdit::returnPressed, this, &MetaPanel::onTagAdded);
    m_containerLayout->addWidget(m_tagEdit);
    m_containerLayout->addSpacing(10);

    m_containerLayout->addStretch(1); // ← 所有多余空间推到这里，消除留白
    m_scrollArea->setWidget(m_container);
    m_mainLayout->addWidget(m_scrollArea);
}
void MetaPanel::addInfoRow(const QString& label, QLabel*& valueLabel) {
    QWidget* row = new QWidget(m_container); QHBoxLayout* rl = new QHBoxLayout(row); rl->setContentsMargins(8, 4, 8, 4); rl->setSpacing(4);
    QLabel* kl = new QLabel(label, row); kl->setFixedWidth(65); kl->setStyleSheet("font-size: 11px; color: #888888;"); rl->addWidget(kl);
    valueLabel = new QLabel("-", row); valueLabel->setWordWrap(true); valueLabel->setStyleSheet("font-size: 12px; color: #EEEEEE; font-weight: 500;");
    valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter); rl->addWidget(valueLabel, 1); m_containerLayout->addWidget(row);
}
QFrame* MetaPanel::createSeparator() { QFrame* l = new QFrame(this); l->setFrameShape(QFrame::HLine); l->setFixedHeight(1); l->setStyleSheet("background-color: #333333; border: none;"); return l; }
QWidget* MetaPanel::createSectionBox(const QString& iconName, const QString& title, QWidget* content) {
    // 2026-05-07 按照用户要求：彻底消除留白
    QFrame* box = new QFrame(this); box->setStyleSheet("QFrame { background-color: transparent; border: none; border-radius: 0px; }");
    QVBoxLayout* layout = new QVBoxLayout(box); layout->setContentsMargins(2, 0, 2, 0); layout->setSpacing(0);
    QHBoxLayout* header = new QHBoxLayout(); header->setSpacing(8);
    QLabel* iconLbl = new QLabel(box); iconLbl->setPixmap(UiHelper::getIcon(iconName, QColor("#888888"), 16).pixmap(16, 16)); header->addWidget(iconLbl);
    QLabel* titleLbl = new QLabel(title, box); titleLbl->setStyleSheet("font-size: 11px; font-weight: bold; color: #888888; text-transform: uppercase;"); header->addWidget(titleLbl);
    header->addStretch(); layout->addLayout(header); layout->addWidget(content); return box;
}
void MetaPanel::onTagAdded() {
    QString text = m_tagEdit->text().trimmed();
    if (!text.isEmpty()) {
        QString currentPath = lblPath->text();
        if (currentPath != "-" && !currentPath.isEmpty()) {
            std::wstring wPath = currentPath.toStdWString();
            RuntimeMeta rm = MetadataManager::instance().getMeta(wPath);
            if (!rm.tags.contains(text)) {
                rm.tags << text; MetadataManager::instance().setTags(wPath, rm.tags);
                TagPill* pill = new TagPill(text, m_tagContainer); connect(pill, &TagPill::deleteRequested, this, &MetaPanel::onTagDeleted); m_tagFlowLayout->addWidget(pill);
            }
        }
        m_tagEdit->clear();
    }
}
void MetaPanel::onTagDeleted(const QString& text) {
    for (int i = 0; i < m_tagFlowLayout->count(); ++i) {
        QLayoutItem* item = m_tagFlowLayout->itemAt(i); TagPill* pill = qobject_cast<TagPill*>(item->widget());
        if (pill && pill->property("tagText").toString() == text) {
            m_tagFlowLayout->takeAt(i); pill->deleteLater(); delete item;
            QString currentPath = lblPath->text();
            if (currentPath != "-" && !currentPath.isEmpty()) {
                std::wstring wPath = currentPath.toStdWString(); RuntimeMeta rm = MetadataManager::instance().getMeta(wPath); rm.tags.removeAll(text); MetadataManager::instance().setTags(wPath, rm.tags);
            }
            return;
        }
    }
}
void MetaPanel::updateInfo(const QString& n, const QString& t, const QString& s, const QString& ct, const QString& mt, const QString& at, const QString& p, bool e) {
    lblName->setText(n); lblType->setText(t); lblSize->setText(s); lblCtime->setText(ct); lblMtime->setText(mt); lblAtime->setText(at); lblPath->setText(p); lblEncrypted->setText(e ? "已加密" : "未加密");
}
void MetaPanel::setRating(int rating) { Q_UNUSED(rating); }
void MetaPanel::setColor(const std::wstring& color) { Q_UNUSED(color); }
void MetaPanel::setPinned(bool pinned) { Q_UNUSED(pinned); }
void MetaPanel::setTags(const QStringList& tags) {
    while (QLayoutItem* item = m_tagFlowLayout->takeAt(0)) { if (QWidget* w = item->widget()) w->deleteLater(); delete item; }
    for (const QString& tag : tags) { TagPill* pill = new TagPill(tag, m_tagContainer); connect(pill, &TagPill::deleteRequested, this, &MetaPanel::onTagDeleted); m_tagFlowLayout->addWidget(pill); }
}
void MetaPanel::setNote(const std::wstring& note) { m_noteEdit->blockSignals(true); m_noteEdit->setPlainText(QString::fromStdWString(note)); m_noteEdit->blockSignals(false); }

void MetaPanel::setPalettes(const QVector<QPair<QColor, float>>& palette) {
    if (!m_paletteLayout) return;
    
    // 清理旧色块 (保留末尾 stretch)
    while (m_paletteLayout->count() > 1) {
        QLayoutItem* item = m_paletteLayout->takeAt(0);
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    QString currentPath = lblPath->text();
    for (const auto& p : palette) {
        PaletteSwatch* swatch = new PaletteSwatch(p.first, currentPath, m_paletteContainer);
        m_paletteLayout->insertWidget(m_paletteLayout->count() - 1, swatch);
    }
}
bool MetaPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_noteEdit && event->type() == QEvent::FocusOut) {
        QString currentPath = lblPath->text(); if (currentPath != "-" && !currentPath.isEmpty()) MetadataManager::instance().setNote(currentPath.toStdWString(), m_noteEdit->toPlainText().toStdWString());
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace ArcMeta
