# 全局滚动条宽度样式优化 —— Analysis_Modification_Plan-174.md

## 1. 任务背景
在当前 FERREX-META 客户端中，为了维持整体界面的暗黑、扁平极简风，全局滚动条（`QScrollBar`）被赋予了自定义 QSS 样式表，其宽度和高度被设定为极窄的 `4px`。这一尺寸虽然在视觉上极具“无边框、无干扰”的边缘美感，但在高分辨率屏幕（如 2K / 4K）或百万级数据量导致垂直滚动条句柄极短的物理交互环境下，鼠标指针对极窄滚动条的捕获、悬停、点按、拖拽等操作变得非常困难且吃力（对应用户原话：“右侧滚动条的宽度过于太窄了，导致操作吃力”）。因此，为了解决这一极易引发操作疲劳的缺陷，需要对样式表进行升级，将滚动条的宽度精准优化拓宽至 `7px`（对应用户原话：“将其宽度调整为7像素”），并同步重构其边角圆角。

## 2. 问题定位
* **QSS 像素硬编码过窄**：
  在 `src/ui/ScanDialog.cpp` 的样式配置中，有针对全局垂直/水平滚动条宽高度的硬编码定义：
  * 垂直滚动条：`QScrollBar:vertical { width: 4px; ... }`
  * 水平滚动条：`QScrollBar:horizontal { height: 4px; ... }`
  这一设计是导致用户拖拽极其困难的罪魁祸首。
* **圆角半径比例失调风险**：
  若仅仅将宽度或高度变大为 `7px`，但手柄的 `border-radius` 依旧维持在 `2px`，手柄在视觉上会呈现出类似“长方形”而非“完美过渡胶囊形”的突兀感。因此必须同步对 `border-radius` 做等比、科学的微调（微调为 `3px`），以保证手柄的两端呈现为优雅的半圆形包覆感，确保与整体现代无框设计的精细美学完美契合。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 右侧滚动条的宽度过于太窄了，导致操作吃力 | 将垂直和水平滚动条（对应用户原话：“右侧滚动条”）美化样式表的尺寸进行全局拓宽提升，彻底解决操作定位吃力问题 | ✅       |
| 2    | 将其宽度调整为7像素 | 精准将样式中的 `width`（垂直）和 `height`（水平）设定为 `7px`（对应用户原话：“将其宽度调整为7像素”），并将圆角等比调优 | ✅       |

## 4. 详细解决方案

### 4.1 QSS 样式微调细节与完美平滑过渡设计
将 `src/ui/ScanDialog.cpp` 的 `QScrollBar` 全局样式段落升级为如下 QSS：

```css
/* 全局滚动条美化 */
QScrollBar:vertical {
    border: none;
    background: transparent;
    width: 7px; /* 完美拓宽（对应用户原话：“将其宽度调整为7像素”） */
    margin: 0px;
}
QScrollBar::handle:vertical {
    background: #333333;
    min-height: 20px;
    border-radius: 3px; /* 从 2px 改为 3px，以确保 7px 宽度下依然是完美的胶囊半圆边缘 */
}
QScrollBar::handle:vertical:hover {
    background: #444444; /* 经典灰暗色提升聚焦对比度 */
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: none;
}

QScrollBar:horizontal {
    border: none;
    background: transparent;
    height: 7px; /* 与垂直滚动条同步调整为 7px（对应用户原话：“将其宽度调整为7像素”） */
    margin: 0px;
}
QScrollBar::handle:horizontal {
    background: #333333;
    min-width: 20px;
    border-radius: 3px; /* 圆角半径从 2px 等比微调至 3px */
}
QScrollBar::handle:horizontal:hover {
    background: #444444;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    width: 0px;
}
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    background: none;
}
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` （仅限优化该文件中的 `QScrollBar` 全局 QSS 美化规则硬编码像素大小）

**明确禁止越界修改的范围：**
- [ ] 严禁修改外部第三方或无关文件（如 `QuickLookWindow.cpp`）内部的预览窗口专用特窄滚动条（其 4px 是专门为了紧凑浮窗设计的，不属于主视口“右侧滚动条”范围），保持改动的精确、纯净、安全。

## 6. 实现准则与预警【核心】
* **无外部覆盖预警**：
  在 `ScanDialog.cpp` 中定义的样式表是采用 `setStyleSheet` 直接应用在 `ScanDialog` 或整个对话框容器上的。这意味着在其下的 `m_iconView`（类型为 `JustifiedView`）和 `m_resultView`（类型为 `QTableView`）的内部子滚动条将会自动继承该美化规则，而不需要手动对每个视图再重复进行局部样式设置，这最大程度避免了 QSS 代码冗余。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **呼吸窗口/耗时操作** | 数据库等重型耗时操作需释放 CPU 锁防界面卡死 | **不涉及**（本方案纯属于边缘 QSS 美化调整，不涉及任何 C++ 级逻辑计算与 I/O） |
| **标题栏按钮/UI尺寸** | 标题栏及关键 UI 组件对标已有尺寸规范 | ✅（完美对齐，滚动条宽度直接提升至 7px 物理参数，不干扰主视口布局，保持视图的高内聚性） |
| **极致性能** | 零分配、避免在运行循环中产生多余堆分配 | ✅（纯静态 QSS 文本定义，运行时由 Qt 的样式引擎解析并在 GPU 渲染阶段共享，不产生任何多余堆内存分配） |
