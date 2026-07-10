# 修复全局视图切换快捷键失效问题 —— Analysis_Modification_Plan-161.md

## 1. 任务背景
在窗口中含有 `QLineEdit`、`QTableView` 等深度嵌套子组件时，直接在 `QAction` 上设置的快捷键可能因为子控件的焦点竞争或事件吞噬而失效 [1]。需要引入高优先级的 `QShortcut` 机制，强制在窗口顶层拦截并触发视图模式的无缝切换 [1]。

## 2. 问题定位
- **模块**：`src/ui/ScanDialog.cpp`
- **位点**：`ScanDialog::ScanDialog` 构造函数末尾。

## 3. 详细解决方案 (代码级指引)

在 `ScanDialog::ScanDialog` 构造函数末尾，定位到初始化 `m_actJMode`、`m_actGMode`、`m_actListMode` 及其 `QActionGroup` 的地方，将原有逻辑重构并追加 **`QShortcut` 强制拦截器**：

```cpp
    // 1. 初始化持久 Action 并绑定核心业务槽函数
    m_actJMode = new QAction("自适应(A)", this);
    m_actJMode->setShortcut(QKeySequence("Ctrl+Shift+1")); // 仅用于在右键菜单上渲染展示快捷键文本 [1]
    m_actJMode->setCheckable(true);
    connect(m_actJMode, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(1);
        m_config.viewMode = 1;
        m_config.layoutMode = 0;
        m_iconView->setLayoutMode(JustifiedView::JustifiedMode);
        m_tableModel->updateResults();
        m_config.save();
    });

    m_actGMode = new QAction("网格(G)", this);
    m_actGMode->setShortcut(QKeySequence("Ctrl+Shift+2")); // 仅用于菜单文本渲染 [1]
    m_actGMode->setCheckable(true);
    connect(m_actGMode, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(1);
        m_config.viewMode = 1;
        m_config.layoutMode = 1;
        m_iconView->setLayoutMode(JustifiedView::GridMode);
        m_tableModel->updateResults();
        m_config.save();
    });

    m_actListMode = new QAction("列表(L)", this);
    m_actListMode->setShortcut(QKeySequence("Ctrl+Shift+3")); // 仅用于菜单文本渲染 [1]
    m_actListMode->setCheckable(true);
    connect(m_actListMode, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(0);
        m_config.viewMode = 0;
        m_resultView->verticalHeader()->setDefaultSectionSize(m_config.iconSize);
        m_tableModel->updateResults();
        m_config.save();
    });

    // 2. 通过 QActionGroup 保持物理单选互斥
    QActionGroup* modeGrp = new QActionGroup(this);
    modeGrp->addAction(m_actJMode);
    modeGrp->addAction(m_actGMode);
    modeGrp->addAction(m_actListMode);

    // 3. 【核心修复】：使用 QShortcut 进行顶层物理拦截，彻底杜绝子控件焦点吞噬快捷键的问题 [1]
    QShortcut* shortcutJ = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_1), this);
    shortcutJ->setContext(Qt::WindowShortcut); // 限制仅在当前窗口处于激活状态时生效 [1]
    connect(shortcutJ, &QShortcut::activated, m_actJMode, &QAction::trigger); // 激活时直接向 Action 发送物理触发指令 [1]

    QShortcut* shortcutG = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_2), this);
    shortcutG->setContext(Qt::WindowShortcut);
    connect(shortcutG, &QShortcut::activated, m_actGMode, &QAction::trigger);

    QShortcut* shortcutList = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_3), this);
    shortcutList->setContext(Qt::WindowShortcut);
    connect(shortcutList, &QShortcut::activated, m_actListMode, &QAction::trigger);
```

---

## 4. 修改边界声明【红线】
- **禁止使用字符串定义快捷键组合**：在 `QShortcut` 初始化时，务必使用显式的位或运算按键码（如 `Qt::CTRL | Qt::SHIFT | Qt::Key_1`），这能避免在部分语种/非标键盘布局中，因 `Shift+1` 映射为 `!` 而导致快捷键识别失败的兼容性问题。
- **防止内存泄漏**：在定义 `QShortcut` 时，其 parent 必须显式传入 `this`（即 `ScanDialog`），确保窗口关闭析构时其内存能被 Qt 的对象树自动清理释放。
