#include "RuleRow.h"
#include "UiHelper.h"
#include <QLabel>

namespace ArcMeta {

RuleRow::RuleRow(QWidget* parent) : QWidget(parent) {
    initUi();
}

void RuleRow::initUi() {
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(6);

    m_typeCombo = new QComboBox(this);
    m_typeCombo->addItem("文本", static_cast<int>(RenameComponentType::Text));
    m_typeCombo->addItem("序列数字", static_cast<int>(RenameComponentType::Sequence));
    m_typeCombo->addItem("原文件名", static_cast<int>(RenameComponentType::OriginalName));
    m_typeCombo->addItem("日期", static_cast<int>(RenameComponentType::Date));
    m_typeCombo->setFixedWidth(100);
    m_typeCombo->setFixedHeight(25); // 2026-04-11 按照用户要求：物理锁定最高高度为25像素
    m_typeCombo->setStyleSheet(
        "QComboBox { background: #2B2B2B; border: 1px solid #444; border-radius: 4px; padding: 2px 4px; color: #EEE; }"
        "QComboBox QAbstractItemView { background-color: #2D2D2D; border: 1px solid #444; selection-background-color: #378ADD; color: #EEE; outline: 0; }"
        "QComboBox QAbstractItemView::item { height: 22px; padding: 2px; }" // 修复下拉列表子项高度
    );

    m_paramStack = new QStackedWidget(this);
    m_paramStack->setFixedHeight(25);

    // 1. Text Param
    m_textEdit = new QLineEdit(this);
    m_textEdit->setPlaceholderText("输入固定文本...");
    m_textEdit->setFixedHeight(25);
    m_textEdit->setStyleSheet("QLineEdit { background: #1E1E1E; border: 1px solid #444; border-radius: 4px; padding: 2px 5px; color: #EEE; }");
    m_paramStack->addWidget(m_textEdit);

    // 2. Sequence Param
    QWidget* seqW = new QWidget(this);
    QHBoxLayout* seqL = new QHBoxLayout(seqW);
    seqL->setContentsMargins(0, 0, 0, 0);
    seqL->setSpacing(5);
    
    m_startSpin = new QSpinBox(seqW);
    m_startSpin->setRange(0, 999999);
    m_startSpin->setValue(1);
    m_startSpin->setFixedHeight(25);
    m_startSpin->setFixedWidth(70);
    m_startSpin->setStyleSheet("QSpinBox { background: #1E1E1E; border: 1px solid #444; border-radius: 4px; color: #EEE; }");
    
    m_paddingCombo = new QComboBox(seqW);
    for(int i=1; i<=6; ++i) m_paddingCombo->addItem(QString("%1 位数").arg(i), i);
    m_paddingCombo->setCurrentIndex(2); // Default 3
    m_paddingCombo->setFixedHeight(25);
    m_paddingCombo->setFixedWidth(90);
    m_paddingCombo->setStyleSheet(
        "QComboBox { background: #1E1E1E; border: 1px solid #444; border-radius: 4px; color: #EEE; }"
        "QComboBox QAbstractItemView { background-color: #2D2D2D; selection-background-color: #378ADD; }"
    );
    
    seqL->addWidget(m_startSpin);
    seqL->addWidget(m_paddingCombo);
    seqL->addStretch();
    m_paramStack->addWidget(seqW);

    // 3. OriginalName Param (Empty)
    QWidget* emptyW = new QWidget(this);
    m_paramStack->addWidget(emptyW);

    // 4. Date Param
    m_dateFormatCombo = new QComboBox(this);
    m_dateFormatCombo->addItems({"yyyyMMdd", "yyyy-MM-dd", "yyyy_MM_dd", "yyyy", "MM", "dd"});
    m_dateFormatCombo->setEditable(true);
    m_dateFormatCombo->setFixedHeight(25);
    m_dateFormatCombo->setStyleSheet(
        "QComboBox { background: #1E1E1E; border: 1px solid #444; border-radius: 4px; color: #EEE; }"
        "QComboBox QAbstractItemView { background-color: #2D2D2D; selection-background-color: #378ADD; }"
    );
    m_paramStack->addWidget(m_dateFormatCombo);

    auto createBtn = [this](const QString& text) {
        QPushButton* btn = new QPushButton(text, this);
        btn->setFixedSize(22, 22); // 回归紧凑尺寸
        btn->setStyleSheet(
            "QPushButton { background: transparent; border: 1px solid #434343; border-radius: 2px; color: #888; font-weight: bold; font-size: 14px; }"
            "QPushButton:hover { background: rgba(255, 255, 255, 0.05); color: #EEE; border-color: #666; }"
        );
        return btn;
    };

    m_btnRemove = createBtn("-"); 
    m_btnAdd = createBtn("+");

    layout->addWidget(m_typeCombo);
    layout->addWidget(m_paramStack, 1);
    layout->addWidget(m_btnRemove);
    layout->addWidget(m_btnAdd);

    // Connections
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
        m_paramStack->setCurrentIndex(index);
        emit changed();
    });

    connect(m_textEdit, &QLineEdit::textChanged, this, &RuleRow::changed);
    connect(m_startSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &RuleRow::changed);
    connect(m_paddingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &RuleRow::changed);
    connect(m_dateFormatCombo, &QComboBox::currentTextChanged, this, &RuleRow::changed);

    connect(m_btnAdd, &QPushButton::clicked, this, &RuleRow::addRequested);
    connect(m_btnRemove, &QPushButton::clicked, this, &RuleRow::removeRequested);
}

RenameRule RuleRow::getRule() const {
    RenameRule rule;
    rule.type = static_cast<RenameComponentType>(m_typeCombo->currentData().toInt());
    
    if (rule.type == RenameComponentType::Text) {
        rule.value = m_textEdit->text();
    } else if (rule.type == RenameComponentType::Sequence) {
        rule.start = m_startSpin->value();
        rule.padding = m_paddingCombo->currentData().toInt();
    } else if (rule.type == RenameComponentType::Date) {
        rule.value = m_dateFormatCombo->currentText();
    }
    
    return rule;
}

} // namespace ArcMeta
