#include "AddressBar.h"
#include "UiHelper.h"
#include "ToolTipOverlay.h"
#include <QHBoxLayout>
#include <QDir>
#include <QPushButton>
#include <QTimer>
#include <QStyle>

namespace ArcMeta {

AddressBar::AddressBar(QWidget* parent) : QWidget(parent) {
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 2026-06-xx 按照用户图片：引入一体化容器 AddressContainer，包含路径与刷新按钮
    m_addressContainer = new QWidget(this);
    m_addressContainer->setObjectName("AddressContainer");
    m_addressContainer->setFixedHeight(32);
    m_addressContainer->setStyleSheet(
        "QWidget#AddressContainer { background: #1E1E1E; border: 1px solid #333333; border-radius: 6px; }"
        "QWidget#AddressContainer[focused='true'] { border: 1px solid #3498db; }"
    );
    QHBoxLayout* containerLayout = new QHBoxLayout(m_addressContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    m_pathStack = new QStackedWidget(m_addressContainer);
    m_pathStack->setFixedHeight(30); // 扣除上下边框
    m_pathStack->setStyleSheet("QStackedWidget { background: transparent; border: none; }");

    m_breadcrumbBar = new BreadcrumbBar(m_pathStack);
    m_pathStack->addWidget(m_breadcrumbBar);

    m_pathEdit = new QLineEdit(m_pathStack);
    m_pathEdit->setPlaceholderText("输入路径...");
    m_pathEdit->setFixedHeight(30); 
    m_pathEdit->setClearButtonEnabled(true);
    m_pathEdit->setStyleSheet("QLineEdit { background: transparent; border: none; color: #EEEEEE; padding-left: 8px; }");
    m_pathStack->addWidget(m_pathEdit);

    m_btnRefresh = new QPushButton(m_addressContainer);
    m_btnRefresh->setFixedSize(30, 30);
    m_btnRefresh->setIcon(UiHelper::getIcon("sync", QColor("#CCCCCC"), 16));
    // 2026-07-xx 按照宪法规范：禁绝原生 ToolTip，对接 ToolTipOverlay
    m_btnRefresh->setProperty("tooltipText", "刷新 (F5)");
    m_btnRefresh->setCursor(Qt::ArrowCursor);
    m_btnRefresh->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-left: 1px solid #333333; border-top-right-radius: 6px; border-bottom-right-radius: 6px; }"
    );
    m_btnRefresh->setAttribute(Qt::WA_Hover);
    m_btnRefresh->installEventFilter(this);

    containerLayout->addWidget(m_pathStack, 1);
    containerLayout->addWidget(m_btnRefresh);

    layout->addWidget(m_addressContainer);

    connect(m_btnRefresh, &QPushButton::clicked, this, &AddressBar::refreshRequested);
    connect(m_breadcrumbBar, &BreadcrumbBar::blankAreaClicked, this, &AddressBar::onBreadcrumbBlankClicked);
    connect(m_pathEdit, &QLineEdit::editingFinished, this, &AddressBar::onPathEditFinished);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        QString input = m_pathEdit->text();
        // 2026-06-xx 交互纠偏：跳转前先解除选中并失焦，防止干扰后续双击
        m_pathEdit->deselect();
        m_pathEdit->clearFocus();

        if (QDir(input).exists() || input == "computer://" || input == tr("此电脑")) {
            emit pathChanged(input == tr("此电脑") ? "computer://" : input);
        } else {
            QString rollback = (m_currentPath == "computer://") ? tr("此电脑") : QDir::toNativeSeparators(m_currentPath);
            m_pathEdit->setText(rollback);
            m_pathStack->setCurrentWidget(m_breadcrumbBar);
        }
    });
    connect(m_breadcrumbBar, &BreadcrumbBar::pathClicked, this, &AddressBar::onBreadcrumbClicked);

    m_pathStack->installEventFilter(this);
    m_breadcrumbBar->installEventFilter(this);
    m_pathEdit->installEventFilter(this);

    m_historyPanel = new AddressHistoryPanel(this);
    connect(m_historyPanel, &AddressHistoryPanel::historyItemClicked, this, [this](const QString& path) {
        emit pathChanged(path);
        m_historyPanel->hide();
    });
    connect(m_historyPanel, &AddressHistoryPanel::historyItemRemoved, this, [this](const QString& path) {
        QStringList history = AppConfig::instance().getValue("AddressBar/History").toStringList();
        history.removeAll(path);
        AppConfig::instance().setValue("AddressBar/History", history);
        m_historyPanel->setHistory(history);
    });
    connect(m_historyPanel, &AddressHistoryPanel::clearAllRequested, this, [this]() {
        AppConfig::instance().setValue("AddressBar/History", QStringList());
        m_historyPanel->setHistory(QStringList());
    });
}

void AddressBar::setPath(const QString& path) {
    m_currentPath = path;
    QString displayPath = (path == "computer://") ? tr("此电脑") : QDir::toNativeSeparators(path);
    m_pathEdit->setText(displayPath);
    m_breadcrumbBar->setPath(path);
    m_pathStack->setCurrentWidget(m_breadcrumbBar);
    saveToHistory(path);
}

void AddressBar::saveToHistory(const QString& path) {
    if (path.isEmpty() || path == "computer://" || path.startsWith("分类: ")) return;
    QStringList history = AppConfig::instance().getValue("AddressBar/History").toStringList();
    history.removeAll(path);
    history.prepend(path);
    while (history.size() > 10) history.removeLast();
    AppConfig::instance().setValue("AddressBar/History", history);
}

void AddressBar::onBreadcrumbBlankClicked() {
    QString displayPath = (m_currentPath == "computer://") ? tr("此电脑") : QDir::toNativeSeparators(m_currentPath);
    m_pathEdit->setText(displayPath);
    m_pathStack->setCurrentWidget(m_pathEdit);
    m_pathEdit->setFocus();
    
    // 2026-06-xx 交互优化：使用 singleShot 延迟全选，防止双击事件的第一击触发全选后，
    // 第二击被全选状态下的系统默认逻辑（取消全选）拦截，确保 eventFilter 能稳定捕获 DblClick。
    QTimer::singleShot(50, m_pathEdit, &QLineEdit::selectAll);
}

void AddressBar::onPathEditFinished() {
    if (m_pathStack->currentWidget() == m_pathEdit) {
        m_pathStack->setCurrentWidget(m_breadcrumbBar);
    }
}

void AddressBar::onBreadcrumbClicked(const QString& path) {
    emit pathChanged(path);
}

bool AddressBar::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_btnRefresh) {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::Enter) {
            m_btnRefresh->setIcon(UiHelper::getIcon("sync", Qt::white, 16));
            // 2026-07-xx 按照 Plan-65：悬停触发，timeout = 0
            QString text = m_btnRefresh->property("tooltipText").toString();
            if (!text.isEmpty()) {
                ToolTipOverlay::instance()->showText(QCursor::pos(), text, 0);
            }
        } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave) {
            m_btnRefresh->setIcon(UiHelper::getIcon("sync", QColor("#CCCCCC"), 16));
            ToolTipOverlay::hideTip();
        }
    }

    // 2026-06-xx 物理联动：由于 QSS 不支持 :focus-within，此处手动驱动容器焦点边框
    if (obj == m_pathEdit) {
        if (event->type() == QEvent::FocusIn) {
            m_addressContainer->setProperty("focused", true);
            m_addressContainer->style()->unpolish(m_addressContainer);
            m_addressContainer->style()->polish(m_addressContainer);
        } else if (event->type() == QEvent::FocusOut) {
            m_addressContainer->setProperty("focused", false);
            m_addressContainer->style()->unpolish(m_addressContainer);
            m_addressContainer->style()->polish(m_addressContainer);
        }
    }

    if ((obj == m_pathStack || obj == m_breadcrumbBar || obj == m_pathEdit) && 
        event->type() == QEvent::MouseButtonDblClick) {
        
        // 2026-06-xx 物理拦截：双击时立即弹出历史面板，并防止文本编辑器吞掉事件
        QStringList history = AppConfig::instance().getValue("AddressBar/History").toStringList();
        if (!history.isEmpty()) {
            m_historyPanel->setHistory(history);
            // 锚定到整个 Stack 以获得正确的对齐
            m_historyPanel->showBelow(m_pathStack);
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace ArcMeta
