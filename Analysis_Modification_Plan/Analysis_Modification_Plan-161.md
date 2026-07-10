# 修复全局视图切换快捷键失效问题 —— Analysis_Modification_Plan-161.md

## 1. 任务背景
右键菜单中的“自适应 (Ctrl+Shift+1)”、“网格 (Ctrl+Shift+2)”、“列表 (Ctrl+Shift+3)”快捷键由于生命周期被限制在右键菜单函数内部，导致菜单关闭后快捷键失效。需要将这三个 Action 改造为窗口全局持久 Action，使其在整个窗口生命周期内随时可用。

## 2. 问题定位
- **模块**：`src/ui/ScanDialog.cpp`
- **位点一（初始化注册）**：`ScanDialog::ScanDialog` 构造函数。
- **位点二（菜单装载）**：`ScanDialog::onCustomContextMenu` (约第 1850 行起)。

## 3. 详细解决方案 (代码级指引)

### 3.1 声明持久成员变量
在 `ScanDialog.h` 头文件的 `private` 声明段中添加这三个 Action 指针：
```cpp
// 在 ScanDialog.h 成员变量区添加：
QAction* m_actJMode = nullptr;
QAction* m_actGMode = nullptr;
QAction* m_actListMode = nullptr;
```

### 3.2 在构造函数中持久化初始化并注册全局快捷键
在 `ScanDialog::ScanDialog` 构造函数末尾，进行如下一次性初始化与信号连接，并通过 `this->addAction` 注册到窗口：

```cpp
    // 1. 初始化持久 Action，并赋予全局快捷键
    m_actJMode = new QAction("自适应(A)", this);
    m_actJMode->setShortcut(QKeySequence("Ctrl+Shift+1"));
    m_actJMode->setCheckable(true);
    connect(m_actJMode, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(1);
        m_config.viewMode = 1;
        m_config.layoutMode = 0;
        m_iconView->setLayoutMode(JustifiedView::JustifiedMode);
        m_tableModel->updateResults();
        m_config.save();
    });
    this->addAction(m_actJMode); // 【核心步骤】：注册至窗口，使其在全局事件循环中生效 [1]

    m_actGMode = new QAction("网格(G)", this);
    m_actGMode->setShortcut(QKeySequence("Ctrl+Shift+2"));
    m_actGMode->setCheckable(true);
    connect(m_actGMode, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(1);
        m_config.viewMode = 1;
        m_config.layoutMode = 1;
        m_iconView->setLayoutMode(JustifiedView::GridMode);
        m_tableModel->updateResults();
        m_config.save();
    });
    this->addAction(m_actGMode);

    m_actListMode = new QAction("列表(L)", this);
    m_actListMode->setShortcut(QKeySequence("Ctrl+Shift+3"));
    m_actListMode->setCheckable(true);
    connect(m_actListMode, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(0);
        m_config.viewMode = 0;
        m_resultView->verticalHeader()->setDefaultSectionSize(m_config.iconSize);
        m_tableModel->updateResults();
        m_config.save();
    });
    this->addAction(m_actListMode);

    // 2. 通过 QActionGroup 保持物理单选互斥
    QActionGroup* modeGrp = new QActionGroup(this);
    modeGrp->addAction(m_actJMode);
    modeGrp->addAction(m_actGMode);
    modeGrp->addAction(m_actListMode);
```

### 3.3 重构右键菜单，重用全局 Action
修改 `ScanDialog::onCustomContextMenu` 中的相应区域，删除原本临时 `new` 的动作，改为直接向菜单中挂载这些已存在的持久 Action，并在弹出前动态刷新状态：

```cpp
    // --- 2026-05-16 新增：视图、排序、刷新全局功能菜单 ---

    QMenu* viewMenu = menu.addMenu("视图(V)");

    // 在菜单弹出前，根据当前真实配置刷新勾选状态 [1]
    if (m_actJMode && m_actGMode && m_actListMode) {
        m_actJMode->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 0);
        m_actGMode->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 1);
        m_actListMode->setChecked(m_config.viewMode == 0);

        // 直接将持久 Action 插入菜单中展现
        viewMenu->addAction(m_actJMode);
        viewMenu->addAction(m_actGMode);
        viewMenu->addAction(m_actListMode);
    }
```

---

## 4. 修改边界声明【红线】
- **严禁重复定义 Action 信号槽**：不可在 `onCustomContextMenu` 内再次连接这三个 Action 的 `triggered` 信号，所有的业务切换逻辑必须在构造函数初始化时单次绑定绑定完毕，避免产生信号多次触发的累积性 Bug。
