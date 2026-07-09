# JustifiedView 图标视图等高宽网格模式 (GridMode) 切换与持久化方案 —— Analysis_Modification_Plan-162.md

## 1. 任务背景
在先前的性能与视觉重构中，我们为图标视图组件 `JustifiedView` 增加了等宽等高的 **网格排布模式 (GridMode)**。该模式采用了 Contain/Cover 缩略图自适应算法，能极佳地改善特定类型文件的排布美观度。然而，目前的 `ScanDialog` 中缺乏切换此排版模式的 UI 入口和右键菜单绑定，且配置类 `ScanConfig` 中也没有对应的持久化字段。这使得两个模式（传统的不等宽“对齐模式 `JustifiedMode`”与等高宽的“网格模式 `GridMode`”）无法供用户任意切换或落盘记忆。

为了解决该体验盲区，本方案旨在优雅地在 `ScanDialog` 的视图下拉菜单和右键菜单中引入切换逻辑，并为 `ScanConfig` 添加 `layoutMode` 字段以实现持久化存盘记忆。

## 2. 问题定位
* **缺失点 1：配置与持久化**
  `struct ScanConfig` 缺少 `layoutMode` 字段，且 `ScanConfig::load` 和 `ScanConfig::save` 无法加载/持久化该配置。默认排版模式应当为 0 (`JustifiedMode`)。
* **缺失点 2：初始化状态绑定**
  `ScanDialog` 在构造或 `showEvent` 恢复视图配置时，只应用了 `viewMode` 和 `iconSize`，而没有从 `m_config.layoutMode` 读取并调用 `m_iconView->setLayoutMode(...)`。
* **缺失点 3：UI 切换入口**
  - 在标题栏的 **视图切换下拉菜单 (viewBtn)** 中，应该增加对 `GridMode` 的切换项。
  - 在列表空白区域的 **全局右键菜单 (viewMenu)** 中，同样也应该添加模式切换动作，从而满足用户的任何排版习惯。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 似乎缺少了切换模式的功能，虽然目前新增了模式，但是却无法切换 | 在 `ScanConfig` 补全字段并于下拉视图菜单与右键全局视图菜单中引入 `GridMode` 切换入口 | ✅ |

## 4. 详细解决方案

### 第一步：在 `src/ui/ScanDialog.h` 中扩展 `ScanConfig` 的字段
在 `ScanConfig` 结构体中新增 `layoutMode` 成员（0: JustifiedMode, 1: GridMode）。

**修正对比片段：**
```cpp
// src/ui/ScanDialog.h

<<<<<<< SEARCH
    int viewMode = 0;   // 0: Details, 1: Icons
    int iconSize = 128; // 256, 128, 64
    int sortColumn = 0; 
    int sortOrder = 0;  // 0: Asc, 1: Desc
=======
    int viewMode = 0;   // 0: Details, 1: Icons
    int iconSize = 128; // 256, 128, 64
    int layoutMode = 0; // 0: JustifiedMode, 1: GridMode
    int sortColumn = 0; 
    int sortOrder = 0;  // 0: Asc, 1: Desc
>>>>>>> REPLACE
```

### 第二步：在 `src/ui/ScanDialog.cpp` 中实现配置存盘与读取

**修正对比片段 1 (配置读取)：**
```cpp
// src/ui/ScanDialog.cpp

<<<<<<< SEARCH
        if (obj.contains("viewMode")) viewMode = obj["viewMode"].toInt();
        if (obj.contains("iconSize")) iconSize = obj["iconSize"].toInt();
        if (obj.contains("sortColumn")) sortColumn = obj["sortColumn"].toInt();
=======
        if (obj.contains("viewMode")) viewMode = obj["viewMode"].toInt();
        if (obj.contains("iconSize")) iconSize = obj["iconSize"].toInt();
        if (obj.contains("layoutMode")) layoutMode = obj["layoutMode"].toInt();
        if (obj.contains("sortColumn")) sortColumn = obj["sortColumn"].toInt();
>>>>>>> REPLACE
```

**修正对比片段 2 (配置保存)：**
```cpp
// src/ui/ScanDialog.cpp

<<<<<<< SEARCH
        // 2026-05-16 持久化存盘
        obj["viewMode"] = viewMode;
        obj["iconSize"] = iconSize;
        obj["sortColumn"] = sortColumn;
=======
        // 2026-05-16 持久化存盘
        obj["viewMode"] = viewMode;
        obj["iconSize"] = iconSize;
        obj["layoutMode"] = layoutMode;
        obj["sortColumn"] = sortColumn;
>>>>>>> REPLACE
```

### 第三步：在 `ScanDialog` UI 初始化中应用排版模式
在 `ScanDialog` 创建出 `m_iconView` 之后，或者在从配置恢复状态的区域（约第 890 - 905 行），应用并设置排版模式。

**修正对比片段：**
```cpp
// src/ui/ScanDialog.cpp

<<<<<<< SEARCH
    if (m_config.viewMode == 1) { // 图标模式
        m_iconView->setTargetRowHeight(m_config.iconSize);
    }
=======
    if (m_config.viewMode == 1) { // 图标模式
        m_iconView->setTargetRowHeight(m_config.iconSize);
    }
    // 恢复排版模式：0 -> JustifiedMode, 1 -> GridMode
    if (m_iconView) {
        m_iconView->setLayoutMode(m_config.layoutMode == 1 ? JustifiedView::GridMode : JustifiedView::JustifiedMode);
    }
>>>>>>> REPLACE
```

### 第四步：在标题栏视图下拉菜单 (viewBtn) 中增加模式切换交互项
优化原有的 `ViewDef` 并加入排版模式支持，让用户在切换超大、大、中图标时也能随心选择“等宽网格”或“对齐不等宽”。

为了使排版模式能独立于尺寸切换（即在不改变图标尺寸的情况下，一键将当前的图标排版改成“等宽网格”或“两端对齐”），我们在下拉菜单中单独增加两个勾选动作项，归属于一个新的动作组，与详情视图隔开：

**修正对比片段：**
```cpp
// src/ui/ScanDialog.cpp

<<<<<<< SEARCH
                struct ViewDef { QString label; int stackIdx; int size; }; 
                for (auto& v : QList<ViewDef>{ 
                    {"超大图标", 1, 192}, {"大图标", 1, 128}, {"中图标", 1, 64}, 
                    {}, // separator 
                    {"详情",    0, 0} 
                }) { 
                    if (v.label.isEmpty()) { menu->addSeparator(); continue; } 
                    QAction* act = menu->addAction(v.label); 
                    act->setCheckable(true); 
                    act->setChecked(m_viewStack->currentIndex() == v.stackIdx && 
                                    (v.stackIdx == 0 || m_config.iconSize == v.size)); 
                    connect(act, &QAction::triggered, this, [this, v]() { 
                        m_viewStack->setCurrentIndex(v.stackIdx); 
                        m_config.viewMode = v.stackIdx; 
                        if (v.stackIdx == 1) { 
                            m_config.iconSize = v.size; 
                            m_iconView->setTargetRowHeight(v.size); 
                            if (m_sizeSlider) m_sizeSlider->setValue(v.size); 
                        } 
                        if (v.stackIdx == 0) 
                            m_resultView->verticalHeader()->setDefaultSectionSize(m_config.iconSize); 
                        m_config.save(); 
                    }); 
                } 
=======
                struct ViewDef { QString label; int stackIdx; int size; }; 
                for (auto& v : QList<ViewDef>{ 
                    {"超大图标", 1, 192}, {"大图标", 1, 128}, {"中图标", 1, 64}, 
                    {}, // separator 
                    {"详情",    0, 0} 
                }) { 
                    if (v.label.isEmpty()) { menu->addSeparator(); continue; } 
                    QAction* act = menu->addAction(v.label); 
                    act->setCheckable(true); 
                    act->setChecked(m_viewStack->currentIndex() == v.stackIdx && 
                                    (v.stackIdx == 0 || m_config.iconSize == v.size)); 
                    connect(act, &QAction::triggered, this, [this, v]() { 
                        m_viewStack->setCurrentIndex(v.stackIdx); 
                        m_config.viewMode = v.stackIdx; 
                        if (v.stackIdx == 1) { 
                            m_config.iconSize = v.size; 
                            m_iconView->setTargetRowHeight(v.size); 
                            if (m_sizeSlider) m_sizeSlider->setValue(v.size); 
                        } 
                        if (v.stackIdx == 0) 
                            m_resultView->verticalHeader()->setDefaultSectionSize(m_config.iconSize); 
                        m_config.save(); 
                    }); 
                } 

                menu->addSeparator();

                // 2026-07-xx 新增排版模式自由切换 (标记 1)
                QAction* jModeAct = menu->addAction("两端对齐 (不等宽)");
                jModeAct->setCheckable(true);
                jModeAct->setChecked(m_config.layoutMode == 0);
                jModeAct->setEnabled(m_viewStack->currentIndex() == 1);

                QAction* gModeAct = menu->addAction("网格排版 (等高宽)");
                gModeAct->setCheckable(true);
                gModeAct->setChecked(m_config.layoutMode == 1);
                gModeAct->setEnabled(m_viewStack->currentIndex() == 1);

                QActionGroup* layoutGrp = new QActionGroup(menu);
                layoutGrp->addAction(jModeAct);
                layoutGrp->addAction(gModeAct);

                connect(jModeAct, &QAction::triggered, this, [this]() {
                    m_config.layoutMode = 0;
                    m_iconView->setLayoutMode(JustifiedView::JustifiedMode);
                    m_tableModel->updateResults();
                    m_config.save();
                });
                connect(gModeAct, &QAction::triggered, this, [this]() {
                    m_config.layoutMode = 1;
                    m_iconView->setLayoutMode(JustifiedView::GridMode);
                    m_tableModel->updateResults();
                    m_config.save();
                });
>>>>>>> REPLACE
```

### 第五步：在全局右键菜单中增加模式切换动作
在全局右键菜单的“视图 (V)”子菜单中加入两项排版模式控制动作，完美对接。

**修正对比片段：**
```cpp
// src/ui/ScanDialog.cpp

<<<<<<< SEARCH
    QAction* detailsAction = addViewAction("详情(D)", "Ctrl+Shift+6", 0, 0);
    
    // 同步当前视图状态
    if (m_viewStack->currentIndex() == 0) detailsAction->setChecked(true);
    else {
        int currentSize = m_config.iconSize;
        if (currentSize == 192) xLargeAction->setChecked(true);
        else if (currentSize == 128) largeAction->setChecked(true);
        else mediumAction->setChecked(true);
    }
=======
    QAction* detailsAction = addViewAction("详情(D)", "Ctrl+Shift+6", 0, 0);
    
    // 同步当前视图状态
    if (m_viewStack->currentIndex() == 0) detailsAction->setChecked(true);
    else {
        int currentSize = m_config.iconSize;
        if (currentSize == 192) xLargeAction->setChecked(true);
        else if (currentSize == 128) largeAction->setChecked(true);
        else mediumAction->setChecked(true);
    }

    viewMenu->addSeparator();

    // 2026-07-xx 按照用户要求：在右键视图菜单中补充对齐与网格切换逻辑 (标记 2)
    QAction* rcJModeAct = viewMenu->addAction("两端对齐 (不等宽)");
    rcJModeAct->setCheckable(true);
    rcJModeAct->setChecked(m_config.layoutMode == 0);
    rcJModeAct->setEnabled(m_viewStack->currentIndex() == 1);

    QAction* rcGModeAct = viewMenu->addAction("网格排版 (等高宽)");
    rcGModeAct->setCheckable(true);
    rcGModeAct->setChecked(m_config.layoutMode == 1);
    rcGModeAct->setEnabled(m_viewStack->currentIndex() == 1);

    QActionGroup* rcLayoutGrp = new QActionGroup(viewMenu);
    rcLayoutGrp->addAction(rcJModeAct);
    rcLayoutGrp->addAction(rcGModeAct);

    connect(rcJModeAct, &QAction::triggered, this, [this]() {
        m_config.layoutMode = 0;
        m_iconView->setLayoutMode(JustifiedView::JustifiedMode);
        m_tableModel->updateResults();
        m_config.save();
    });
    connect(rcGModeAct, &QAction::triggered, this, [this]() {
        m_config.layoutMode = 1;
        m_iconView->setLayoutMode(JustifiedView::GridMode);
        m_tableModel->updateResults();
        m_config.save();
    });
>>>>>>> REPLACE
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 头文件：`src/ui/ScanDialog.h`（新增配置字段 `int layoutMode`）
- [ ] 源文件：`src/ui/ScanDialog.cpp`（扩展读取/存盘，初始化绑定，viewBtn 下拉菜单及全局右键菜单扩充切换逻辑）

**明确禁止越界修改的范围：**
- [ ] 严禁修改 `JustifiedView.cpp` 本身的排版数学公式和滚动条基础行为，确保核心排版引擎在本次交互性加固中的稳定性。

## 6. 实现准则与预警【核心】
1. **强制级缓存更新**：由于切换模式不仅会改变容器中每个卡片的几何宽（等宽 vs 自适应宽高），而且在 `ThumbnailDelegate` 中会通过动态属性触发 Contain（网格等高宽）与 Cover（不等宽）渲染算法的分流。因此在切换排版模式的槽函数中，**不仅要触发重排，还必须调用 `m_tableModel->updateResults()`。** 这样能安全重绘整个可见区域的图例，以防出现卡片已变等宽，但内部图片依然以拉伸或错误的自适应比例填充的情况。
2. **状态依赖提示**：由于“排版模式”仅在图标模式（视图模式为 1）下生效，因此若当前处于“详情”视图下，切换两端对齐/网格的动作应设为 `setEnabled(false)`，使其呈现合理的中性灰禁选状态，提升交互体感的逻辑自洽性。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 视图布局解耦 | `JustifiedView` 须支持 `JustifiedMode` 与 `GridMode`。通过 QObject 动态属性 `gridMode` 告知 `ThumbnailDelegate` 进行 Contain vs Cover 分流渲染。 | ✅ 符合。本方案将这些抽象底层解耦接口，通过 `ScanDialog` 控件与配置层完全连接，实现开箱即用的多态排版自由切换。 |

## 8. 待确认事项（可选）
* 无
