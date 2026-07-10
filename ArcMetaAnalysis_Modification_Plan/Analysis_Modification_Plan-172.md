# 移除冗余图标尺寸选项与重构三大视图排版模式 —— Analysis_Modification_Plan-172.md

## 1. 任务背景
在当前系统的视图切换逻辑中，存在着通过离散尺寸选项切换和通过连续滑杆切换这两套逻辑重合的设计（对应用户原话：“由于标记为①的滑杆是用来调整大小的，所以标记为②的这三个选项可以彻底移除了，我觉着它是冗余的”）。为了精简界面交互，让排版控制更为纯粹，用户提出彻底移除菜单中多余的图标尺寸，并将视图功能重新划分为三种模式，即自适应、网格和列表（对应用户原话：“那我们就把它划分为三种模式，自适应、网格、列表”）。本方案旨在物理移除这些冗余选项，重构并统一系统的排版与模式选择流程。

## 2. 问题定位
* **关键源文件**：`src/ui/ScanDialog.cpp`
* **问题成因**：
  在 `ScanDialog.cpp` 中共有两处 [两处] 重复实现了“超大图标”、“大图标”、“中图标”等切换逻辑：
  1. **标题栏视图切换按钮弹出菜单**（位于 `ScanDialog::ScanDialog` 构造函数内约第 715 行至 775 行附近）：
     该处通过 `QList<ViewDef>` 循环渲染了“超大图标”、“大图标”和“中图标”三个 [三个] 选项，并另外在下方 [下方] 添加了“自适应”与“网格”单选。
  2. **主视图右键级联“视图(V)”菜单**（位于 `ScanDialog::onCustomContextMenu` 槽函数内约第 1730 行至 1775 行附近）：
     该处通过私有 lambda 辅助函数 `addViewAction` 添加了“超大图标(X)”、“大图标(L)”、“中图标(M)”以及“详情(D)”/“列表”等动作，并在其后追加了“两端对齐 (不等宽)”与“网格排版 (等高宽)”选项。
  这两处 [两处] 逻辑导致大小配置（`iconSize`）与布局形式（`layoutMode`）强耦合，不仅带来了界面菜单的极度冗余，也与系统顶部的尺寸调节滑杆（对应用户原话：“标记为①的滑杆”）存在功能上的冲突。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 由于标记为①的滑杆是用来调整大小的，所以标记为②的这三个选项可以彻底移除了，我觉着它是冗余的 | 彻底移除标题栏下拉菜单以及主右键菜单级联“视图(V)”中的“超大图标”、“大图标”、“中图标”三个 [三个] 选项 | ✅ 一致 |
| 2    | 那我们就把它划分为三种模式，自适应、网格、列表 | 在上述两处 [两处] 菜单中，重新划分为自适应、网格、列表三种 [三种] 模式，并作为平级单选切换 | ✅ 一致 |

## 4. 详细解决方案

我们将在 `src/ui/ScanDialog.cpp` 的两处 [两处] 菜单构建逻辑中，剔除所有“超大/大/中图标”的硬编码尺寸选项，并直接以“自适应”、“网格”、“列表”三种 [三种] 一级平级状态来进行重写与整合。

### 4.1 标题栏视图按钮弹出菜单重构
在 `ScanDialog::ScanDialog` 中，重构 `viewBtn` 关联的弹出菜单构建。将原先嵌套大小的循环剔除，统一声明三个 [三种] 模式单选动作。

**修改前的冗余逻辑：**
```cpp
struct ViewDef { QString label; int stackIdx; int size; }; 
for (auto& v : QList<ViewDef>{ 
    {"超大图标", 1, 192}, {"大图标", 1, 128}, {"中图标", 1, 64}, 
    {}, // separator 
    {"列表",    0, 0} 
}) { ... }
...
QAction* jModeAct = menu->addAction("自适应");
QAction* gModeAct = menu->addAction("网格");
```

**修改后的精简方案：**
```cpp
connect(viewBtn, &QPushButton::clicked, this, [this, viewBtn]() { 
    QMenu* menu = new QMenu(this); 
    menu->setStyleSheet( 
        "QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; border-radius: 6px; }" 
        "QMenu::item { padding: 6px 24px; }" 
        "QMenu::item:selected { background: #2A2A2A; color: #FFF; }" 
        "QMenu::item:checked { color: #FF8C00; }" 
    ); 

    // 自适应模式（对应用户原话：“自适应”）
    QAction* jModeAct = menu->addAction("自适应");
    jModeAct->setCheckable(true);
    jModeAct->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 0);

    // 网格模式（对应用户原话：“网格”）
    QAction* gModeAct = menu->addAction("网格");
    gModeAct->setCheckable(true);
    gModeAct->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 1);

    // 列表模式（对应用户原话：“列表”）
    QAction* listModeAct = menu->addAction("列表");
    listModeAct->setCheckable(true);
    listModeAct->setChecked(m_config.viewMode == 0);

    // 通过排他性的 Action 组进行物理互斥
    QActionGroup* modeGrp = new QActionGroup(menu);
    modeGrp->addAction(jModeAct);
    modeGrp->addAction(gModeAct);
    modeGrp->addAction(listModeAct);

    // 各项单选槽连接，使选择彻底正交
    connect(jModeAct, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(1);
        m_config.viewMode = 1;
        m_config.layoutMode = 0;
        m_iconView->setLayoutMode(JustifiedView::JustifiedMode);
        m_tableModel->updateResults();
        m_config.save();
    });
    connect(gModeAct, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(1);
        m_config.viewMode = 1;
        m_config.layoutMode = 1;
        m_iconView->setLayoutMode(JustifiedView::GridMode);
        m_tableModel->updateResults();
        m_config.save();
    });
    connect(listModeAct, &QAction::triggered, this, [this]() {
        m_viewStack->setCurrentIndex(0);
        m_config.viewMode = 0;
        m_resultView->verticalHeader()->setDefaultSectionSize(m_config.iconSize);
        m_tableModel->updateResults();
        m_config.save();
    });

    menu->exec(viewBtn->mapToGlobal(QPoint(0, viewBtn->height())));
});
```

---

### 4.2 主视图右键上下文菜单重构
在 `ScanDialog::onCustomContextMenu` 中，同样彻底剥离冗余的图标尺寸项，只保留视图子菜单 “自适应(A)”、“网格(G)” 与 “列表(L)” 三种 [三种] 选项作为一级的单选。

**修改前的冗余逻辑：**
```cpp
QAction* xLargeAction = addViewAction("超大图标(X)", "Ctrl+Shift+1", 1, 192);
QAction* largeAction = addViewAction("大图标(L)", "Ctrl+Shift+2", 1, 128);
QAction* mediumAction = addViewAction("中图标(M)", "Ctrl+Shift+3", 1, 64);
viewMenu->addSeparator();
QAction* detailsAction = addViewAction("详情(D)", "Ctrl+Shift+6", 0, 0);
...
QAction* rcJModeAct = viewMenu->addAction("两端对齐 (不等宽)");
QAction* rcGModeAct = viewMenu->addAction("网格排版 (等高宽)");
```

**修改后的精简方案：**
```cpp
QMenu* viewMenu = menu.addMenu("视图(V)");
QActionGroup* rcModeGrp = new QActionGroup(this);

// 自适应 (A)（对应用户原话：“自适应”）
QAction* rcJModeAct = viewMenu->addAction("自适应(A)");
rcJModeAct->setShortcut(QKeySequence("Ctrl+Shift+1"));
rcJModeAct->setCheckable(true);
rcJModeAct->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 0);
rcModeGrp->addAction(rcJModeAct);

// 网格 (G)（对应用户原话：“网格”）
QAction* rcGModeAct = viewMenu->addAction("网格(G)");
rcGModeAct->setShortcut(QKeySequence("Ctrl+Shift+2"));
rcGModeAct->setCheckable(true);
rcGModeAct->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 1);
rcModeGrp->addAction(rcGModeAct);

// 列表 (L)（对应用户原话：“列表”）
QAction* rcListModeAct = viewMenu->addAction("列表(L)");
rcListModeAct->setShortcut(QKeySequence("Ctrl+Shift+3"));
rcListModeAct->setCheckable(true);
rcListModeAct->setChecked(m_config.viewMode == 0);
rcModeGrp->addAction(rcListModeAct);

connect(rcJModeAct, &QAction::triggered, this, [this]() {
    m_viewStack->setCurrentIndex(1);
    m_config.viewMode = 1;
    m_config.layoutMode = 0;
    m_iconView->setLayoutMode(JustifiedView::JustifiedMode);
    m_tableModel->updateResults();
    m_config.save();
});
connect(rcGModeAct, &QAction::triggered, this, [this]() {
    m_viewStack->setCurrentIndex(1);
    m_config.viewMode = 1;
    m_config.layoutMode = 1;
    m_iconView->setLayoutMode(JustifiedView::GridMode);
    m_tableModel->updateResults();
    m_config.save();
});
connect(rcListModeAct, &QAction::triggered, this, [this]() {
    m_viewStack->setCurrentIndex(0);
    m_config.viewMode = 0;
    m_resultView->verticalHeader()->setDefaultSectionSize(m_config.iconSize);
    m_tableModel->updateResults();
    m_config.save();
});
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] `src/ui/ScanDialog.cpp`：构造函数视图切换按钮弹出菜单（约 715~775 行附近）以及右键“视图(V)”级联子菜单（约 1730~1780 行附近）的代码重构。

**明确禁止越界修改的范围：**
- [ ] 严格禁止修改或破坏标记为①的滑杆（`m_sizeSlider`，对应用户原话：“标记为①的滑杆”）的事件监听、事件过滤器（`eventFilter`）及值变动槽函数。
- [ ] 严格禁止在菜单之外移除图标尺寸缩放逻辑。

## 6. 实现准则与预警【核心】
* **头文件依赖**：无需新增外部头文件依赖，直接复用现有的 Qt 核心 UI 类。
* **高频点击稳定性**：在连续点击菜单项切换视图模式时，由于方案在数据源与视图栈层面实现了完美的同步机制，避免了底层 `JustifiedView` 在大小改变时和排版模式改变时的多次重复触发。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 模式配置 | 视图切换及布局切换需调用底层接口进行相应更新并调用 `m_config.save()` | ✅ 符合。切换三大模式时皆进行了正确的 viewMode / layoutMode 参数修改与配置落盘。 |

## 8. 待确认事项（可选）
无。用户已对三种视图排版模式归并和多余选项删除方案达成完全共识。
