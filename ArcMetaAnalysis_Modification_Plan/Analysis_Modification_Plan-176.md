# 物理清除冗余快捷键绑定 —— Analysis_Modification_Plan-176.md

## 1. 任务背景
在当前 FERREX-META 客户端的 `ScanDialog` 主对话框中，历史遗留了 `Ctrl+Shift+1`（自适应模式）、`Ctrl+Shift+2`（网格模式）以及 `Ctrl+Shift+3`（列表模式）三大视图排版切换的局内快捷键绑定。经过深度技术审计，发现这组快捷键由于在同一个主窗口下通过 `QAction::setShortcut` 与顶层 `QShortcut` 进行了重复注册，在实际运行时会直接触发 Qt 快捷键引擎的“多重载模糊（Ambiguous Shortcut Overload）”拦截机制，从而导致按键完全瘫痪失效。

鉴于这三个排版快捷键在实际操作场景中用途极小（用户更倾向于直接通过右上角直观的排版切换菜单按钮进行交互），为了彻底精简主窗体事件拦截、降低底层的热键监听冗余和冲突开销，用户决定直接将其物理移除。本次分析旨在制定出一套 100% 干净、彻底的代码清理和安全剔除方案（对应用户原话：“直接将Ctrl+Shift+1 - 3快捷键移除掉吧”）。

## 2. 问题定位与清理点
在 `src/ui/ScanDialog.cpp` 的 `initUi()` 阶段（约第 1170 - 1235 行左右），物理锁定了以下几处代码，这些代码正是快捷键冲突与注册源头：

* **清理点 A：`QAction` 绑定的冗余物理快捷键**
  * 修改前：
    ```cpp
    m_actJMode->setShortcut(QKeySequence("Ctrl+Shift+1"));
    m_actGMode->setShortcut(QKeySequence("Ctrl+Shift+2"));
    m_actListMode->setShortcut(QKeySequence("Ctrl+Shift+3"));
    ```
  * 修改方案：物理**删除这三行代码**。此时，右键菜单依然正常作为操作入口，但其 Action 上将不再有物理快捷键硬注册，干净释放注册槽。

* **清理点 B：顶层 QShortcut 注册及信号连接**
  * 修改前：
    ```cpp
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
  * 修改方案：物理**整段删除这十二行代码**，不再注册和拦截这些按键序列，彻底清除 QShortcut 堆内存开销与事件拦截开销（对应用户原话：“直接将Ctrl+Shift+1 - 3快捷键移除掉吧”）。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 反正Ctrl+Shift+1 - 3快捷键用途不大 | 审计并锁定 `ScanDialog.cpp` 中全部相关的 Action 快捷键注册及对应 `QShortcut` 实例注册逻辑进行物理物理整除 | ✅       |
| 2    | 直接将Ctrl+Shift+1 - 3快捷键移除掉吧 | 彻底删除 `setShortcut` 调用，并整段删除 `shortcutJ`、`shortcutG` 和 `shortcutList` 的初始化与槽连接逻辑（对应用户原话：“直接将Ctrl+Shift+1 - 3快捷键移除掉吧”） | ✅       |

## 4. 详细解决方案

在执行重构修改时，精准应用以下 Git Merge 差异修改：

```cpp
<<<<<<< SEARCH
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
=======
    // 1. 初始化持久 Action 并绑定核心业务槽函数
    m_actJMode = new QAction("自适应(A)", this);
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
>>>>>>> REPLACE
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` （仅限物理删除此文件中初始化排版 Action 时对快捷键设置及底层的三个 `QShortcut` 事件监听绑定的物理注册）

**明确禁止越界修改的范围：**
- [ ] 严禁修改或删除 `m_actJMode`、`m_actGMode` 和 `m_actListMode` 的核心 `connect` 业务逻辑（即切换视图堆栈以及存盘设置），确保右键菜单手动点按选择功能保持 100% 完好无损。
- [ ] 严禁在其他与视图切换无关的文件中进行修改，严格聚焦在快捷键移除的逻辑闭环内。

## 6. 实现准则与预警【核心】
1. **删除彻底度核验**：
   重构修改完成后，开发人员需在终端或 IDE 中全局检索 `shortcutJ`、`shortcutG` 以及 `shortcutList` 这些标识符。确保不存在由于在 `ScanDialog.h` 的头文件中声明、而在本重构中漏删导致的任何“未声明的标识符”或“未定义”等潜在编译错误。目前经核对 `ScanDialog.h` 的类成员声明，这三个 `QShortcut` 均是局部变量形式定义的，因此直接在 `.cpp` 文件中整除安全，无任何头文件残留。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **呼吸窗口/耗时操作** | 数据库等重型耗时操作需释放 CPU 锁防界面卡死 | **不涉及**（本方案纯属于交互快捷键的静态代码移除与清理，无运行态高开销） |
| **标题栏按钮/UI尺寸** | 标题栏及关键 UI 组件对标已有尺寸规范 | ✅（完美对齐极简化、去冗余的要求，极大地清空了多余的无用后台注册） |
| **极致性能** | 零分配、避免多余的对象创建 | ✅（物理清空了 3 个 `QShortcut` 对象和 QAction 内含 Shortcut 的动态分配，直接实现内存與事件分发效率双重提升） |
