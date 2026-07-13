# 无边框窗口边缘拉伸与光标切换方案 —— Analysis_Modification_Plan-173.md

## 1. 任务背景
在当前 FERREX-META 客户端中，基于 `FramelessDialog` 基类的所有对话框（如 `ScanDialog` 和 `FramelessInputDialog`）均采用了自定义的扁平化无边框（`Qt::FramelessWindowHint`）设计。虽然该设计带来了极具现代感的视觉外观与 Windows 11 圆角适配，但也由于脱离了操作系统的原生边框托管，导致了鼠标移动到窗口边缘及四个角部时无法显示双向调整大小的光标（对应用户原话：“当鼠标移动到窗口边缘时也没有出现双向箭头”），同时用户也无法通过按住边缘拖拉的方式拉伸和缩放窗口（对应用户原话：“导致无法通过拖拉方式调整窗口大小”）。本次分析旨在设计一套跨平台且兼顾性能的非侵入式鼠标边缘检测、动态光标更新和实时拖拉缩放计算方案，彻底解决此交互硬伤。

## 2. 问题定位
* **缺失的鼠标追踪机制 (`mouseTracking`)**：
  在 Qt 中，若子部件没有启用鼠标追踪（`setMouseTracking(true)`），则只有在按住鼠标按键时，窗口才会收到 `mouseMoveEvent` 事件。目前 `FramelessDialog` 虽调用了 `setMouseTracking(true)`，但其子部件（如主布局中的各种控件、视图容器）默认并未启用追踪，导致当鼠标仅移动、不按下键时，主窗口无法在全局拦截并捕获边缘位置以更新光标。
* **缺失的边缘区域状态划分与判定算法**：
  现有代码中没有定义窗口边缘的触发判定带宽（如边界像素宽度 `PADDING`），无法识别当前鼠标是处于左边缘、右边缘、上边缘、下边缘，还是四个拐角（左上、右上、左下、右下）或正常的窗口内部区域。
* **缺失的手动拉伸事件循环逻辑**：
  在 `mousePressEvent`、`mouseMoveEvent`、`mouseReleaseEvent` 中，只有当点击标题栏拖拽移动窗口的逻辑，完全没有检测到边缘时启动缩放拉伸窗口的物理处理（通过 `setGeometry()` 并配合 `minimumSize()` 计算），导致即使拖拽边缘也无任何反应。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 当鼠标移动到窗口边缘时也没有出现双向箭头 | 在 `FramelessDialog` 边缘及四个拐角定义检测带（对应用户原话：“当鼠标移动到窗口边缘时”），并在 `mouseMoveEvent` 中实时分析并调用 `setCursor` 改变光标为对应的双向调整剪头（对应用户原话：“出现双向箭头”） | ✅       |
| 2    | 导致无法通过拖拉方式调整窗口大小 | 在 `mousePressEvent` 中捕获边缘点击状态并缓存初始边界信息，在 `mouseMoveEvent` 中根据鼠标偏移差值（对应用户原话：“通过拖拉方式”）实时重算并更新窗口的几何包围盒（对应用户原话：“调整窗口大小”） | ✅       |

## 4. 详细解决方案

### 4.1 核心边界定义与枚举设计
定义无边框边缘检测带的触发像素宽度 `PADDING = 6`。将窗口边缘区域划分为 9 个状态：
```cpp
enum ResizeDir {
    DIR_NONE = 0,
    DIR_TOP,
    DIR_BOTTOM,
    DIR_LEFT,
    DIR_RIGHT,
    DIR_TOPLEFT,
    DIR_TOPRIGHT,
    DIR_BOTTOMLEFT,
    DIR_BOTTOMRIGHT
};
```

### 4.2 边缘区域碰撞判定算法
在接收到局部坐标（`event->pos()`）时，通过以下逻辑动态计算所属区域：
```cpp
ResizeDir getResizeDir(const QPoint& pos) {
    int x = pos.x();
    int y = pos.y();
    int w = this->width();
    int h = this->height();

    bool left = (x >= 0 && x <= PADDING);
    bool right = (x >= w - PADDING && x <= w);
    bool top = (y >= 0 && y <= PADDING);
    bool bottom = (y >= h - PADDING && y <= h);

    if (left && top) return DIR_TOPLEFT;
    if (right && top) return DIR_TOPRIGHT;
    if (left && bottom) return DIR_BOTTOMLEFT;
    if (right && bottom) return DIR_BOTTOMRIGHT;
    if (left) return DIR_LEFT;
    if (right) return DIR_RIGHT;
    if (top) return DIR_TOP;
    if (bottom) return DIR_BOTTOM;

    return DIR_NONE;
}
```

### 4.3 动态光标设置与状态同步
在 `mouseMoveEvent` 阶段，如果用户当前未处于拖拽状态（既没有拖动窗口，也没有拉伸边缘），则实时检测鼠标位置并更换鼠标光标样式。
* `DIR_LEFT` / `DIR_RIGHT` $\rightarrow$ `Qt::SizeHorCursor` (水平双向箭头)
* `DIR_TOP` / `DIR_BOTTOM` $\rightarrow$ `Qt::SizeVerCursor` (垂直双向箭头)
* `DIR_TOPLEFT` / `DIR_BOTTOMRIGHT` $\rightarrow$ `Qt::SizeFDiagCursor` (斜向对角线双向箭头)
* `DIR_TOPRIGHT` / `DIR_BOTTOMLEFT` $\rightarrow$ `Qt::SizeBDiagCursor` (反斜向对角线双向箭头)
* `DIR_NONE` $\rightarrow$ `Qt::ArrowCursor` (标准箭头)

### 4.4 鼠标事件流（拉伸缩放计算公式）
* **在 `mousePressEvent` 中**：
  若鼠标处于边缘（`getResizeDir(pos) != DIR_NONE`），则记录当前的拉伸方向 `m_resizeDir`，并缓存当前鼠标的全局起始坐标 `m_startGlobalPos = event->globalPosition().toPoint()` 及窗口的初始几何大小 `m_startGeometry = geometry()`。
* **在 `mouseMoveEvent` 中**：
  若 `m_resizeDir != DIR_NONE`，计算当前全局鼠标的偏移量：
  $$\Delta x = \text{currentGlobalPos.x()} - \text{m\_startGlobalPos.x()}$$
  $$\Delta y = \text{currentGlobalPos.y()} - \text{m\_startGlobalPos.y()}$$
  根据 `m_resizeDir` 分别重算窗口的新几何区域（`newGeom`）。
  
  **四周边线计算法则（需严格遵循 `setMinimumSize` 物理限制）**：
  * **DIR_RIGHT**：
    $$newWidth = \max(minWidth, m\_startGeometry.width() + \Delta x)$$
    $$newGeom = (x, y, newWidth, height)$$
  * **DIR_BOTTOM**：
    $$newHeight = \max(minHeight, m\_startGeometry.height() + \Delta y)$$
    $$newGeom = (x, y, width, newHeight)$$
  * **DIR_LEFT** (向左拉伸，其右侧边线固定，左侧物理移动)：
    $$newWidth = \max(minWidth, m\_startGeometry.width() - \Delta x)$$
    $$newX = m\_startGeometry.right() - newWidth + 1$$
    $$newGeom = (newX, y, newWidth, height)$$
  * **DIR_TOP** (向上拉伸，其下侧边线固定，上侧物理移动)：
    $$newHeight = \max(minHeight, m\_startGeometry.height() - \Delta y)$$
    $$newY = m\_startGeometry.bottom() - newHeight + 1$$
    $$newGeom = (x, newY, width, newHeight)$$
  * **角部拉伸（DIR_TOPLEFT, DIR_TOPRIGHT, DIR_BOTTOMLEFT, DIR_BOTTOMRIGHT）**：
    正交合并上述法则即可。最后调用 `setGeometry(newGeom)` 刷新布局。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/FramelessDialog.h` （追加拉伸方向枚举、变量、核心位置计算函数声明）
- [ ] 模块/文件：`src/ui/FramelessDialog.cpp` （实现边缘动态判定、更新光标形态、点击抓取以及重算 `setGeometry` 尺寸拉伸流）

**明确禁止越界修改的范围：**
- [ ] 明确禁止在不改变基类的情况下直接在 `ScanDialog` 中重写一套边缘检测。必须在基类 `FramelessDialog` 中全局实现，使其他所有无边框输入框对话框均能秒级同步获得拉伸缩放及光标悬停更新能力。

## 6. 实现准则与预警【核心】
1. **子控件鼠标事件遮挡问题预警**：
   无边框窗口中最经典的缺陷是，当鼠标移动到由于有布局管理器填充的子控件（如输入框、按钮、甚至主体容器 `m_container`）上方时，因为子控件没有开启 `setMouseTracking(true)` 且各自拦截了鼠标移动事件，导致主窗口的 `mouseMoveEvent` 根本无法被触发。
   * **解决方案**：在基类 `FramelessDialog` 构造函数中，对容器 `m_container` 以及所有后期添加的核心边缘子部件安装事件过滤器 `installEventFilter(this)`。在 `eventFilter` 中拦截 `QEvent::HoverMove` 或 `QEvent::MouseMove`，并统一派发、路由回主窗体的边缘区域判定模块，确保光标刷新 100% 灵敏、绝无视觉死角。
2. **圆角切割与边缘检测偏移问题**：
   因为使用了 `DwmSetWindowAttribute` 开启了 Windows 11 的物理原生圆角（6px 到 12px 弧度），窗口四周的最外侧四个像素角已被透明化。如果 `PADDING` 判定带设置得太窄（如只有 2-3 像素），在圆角区域鼠标会提前滑出窗口导致无法检测到。因此 `PADDING` 应锁定在 6 至 8 像素之间，确保圆角弧度区域也能完美触发拉伸指针。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **呼吸窗口/耗时操作** | 数据库等重型耗时操作需释放 CPU 锁防界面卡死 | **不涉及**（本方案纯属于边缘图形交互拉伸逻辑，不触碰任何数据库操作） |
| **标题栏按钮/UI尺寸** | 标题栏按钮及组件样式尺寸对标规范 | ✅（完全基于现有的 20x20 按钮及 1px 物理分割线，绝不改动任何按钮基础排版与背景色样式） |
| **极致性能** | 零分配、避免在循环内构造临时对象 | ✅（边缘区域计算全部使用高效的常数级算术整型、QPoint、QRect 物理操作，不产生任何堆内存分配与性能抖动） |
