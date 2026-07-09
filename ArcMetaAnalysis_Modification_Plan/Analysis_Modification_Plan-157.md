# 复刻“原始优雅版”QuickLook 预览功能 —— Analysis_Modification_Plan-157.md

## 1. 任务背景
用户需要在 FERREX-META 中复刻 ArcMeta 早期版本的“空格键预览”功能。由于代码库中现存的 `QuickLookWindow` 属于已退化的“全屏重构版”，本方案将根据用户提供的截图，重新设计并实现具备居中悬浮、棋盘格背景、大图标反馈以及 Notepad++ 级兼容性的预览器。

## 2. 问题定位
- **架构断层**：仓库中的 `QuickLookWindow.cpp` (2026版) 误用了 `showFullScreen()`，丢失了截图中的悬浮质感。
- **视觉逻辑缺失**：现有源码完全缺失透明图像预览时的**棋盘格背景**绘制逻辑。
- **反馈引擎脱节**：截图中的“Ctrl Shift >”巨大交互提示需要移植并升级 `ToolTipOverlay` 逻辑。
- **兼容性瓶颈**：原版文本预览缺乏编码自动探测与二进制安全拦截，不符合“Notepad++ 级”要求。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 复刻 ArcMeta 版本空格键预览功能 (对应用户原话) | 移植并重构悬浮版 QuickLookWindow | ✅ |
| 2    | 居中悬浮窗口，非全屏 (对应用户理解) | 抛弃 showFullScreen，改用 fixedSize + SetWindowPos 居中悬浮 | ✅ |
| 3    | 预览透明图像必须显示棋盘格背景 (对应用户原话/截图) | 实现 #2B2B2B / #333333 双色瓦片渲染算法 | ✅ |
| 4    | 屏幕中心显示巨大图标反馈 (对应用户截图) | 集成反馈引擎，渲染类似“Ctrl Shift >”的半透明大图标 | ✅ |
| 5    | 文本预览对标 Notepad++ (对应用户原话) | 实现 GBK/UTF-8 自动探测与二进制 \0 字符拦截 | ✅ |

## 4. 详细解决方案

### 4.1 悬浮窗架构设计 (对应用户理解："居中悬浮窗口")
- **窗口属性**：使用 `Qt::FramelessWindowHint`，圆角锁定为 **12px**。
- **置顶逻辑**：复用 `ToolTipOverlay` 的 Win32 置顶方案，调用 `SetWindowPos(HWND_TOPMOST)`，并配合 `SWP_NOACTIVATE` 确保预览窗不夺取主窗口焦点。
- **几何尺寸**：初始大小对标截图比例（如 800x600 或屏幕 0.6 倍），始终在屏幕中心弹出。

### 4.2 棋盘格背景渲染 (对应用户截图："预览图背景")
- **算法实现**：在 `QuickLookWindow::paintEvent` 中，对于图像预览模式，首先绘制棋盘格。
- **色值规约**：使用 16x16 的瓦片。
    - 颜色 A：`#2B2B2B`
    - 颜色 B：`#333333`
- **代码思路**：使用 `QPainter::drawTiledPixmap` 渲染预生成的双色瓦片图，确保其覆盖整个图像展示区域。

### 4.3 “Ctrl Shift >”大图标反馈引擎 (对应用户截图)
- **提示逻辑**：当用户在预览窗内按下方向键切换文件或按下 1-5 打分时，触发 `ToolTipOverlay::showFeedbackIcon`。
- **视觉效果**：
    - 在窗口中心（或屏幕中心）显示 128px 以上的半透明图标。
    - 动画：由淡入到淡出，停留时长约 500ms。
- **资源调用**：直接调用 `resources/Icon/` 下的 SVG 资源进行着色渲染。

### 4.4 Notepad++ 级文本预览 (对应用户原话："对标 Notepad++")
- **编码探测**：检测文件头前 4KB。若包含 UTF-8 特征码则使用 `QString::fromUtf8`；否则退回至 GBK（`QTextCodec::codecForName("GBK")`）编码。
- **二进制安全**：若前 1KB 包含超过 2 个 `\0` 字节，立即停止预览，显示“此文件为二进制格式，不支持预览”的占位符。
- **性能锚定**：物理限额读取 **128KB** (对应原话要求)。

### 4.5 集成至 ScanDialog
- **键盘拦截**：在 `ScanDialog::keyPressEvent` 拦截空格键。
- **首行定位**：预览窗通过信号 `prevRequested/nextRequested` 回传给 `ScanDialog`，驱动其修改 `TableView` 的当前行，实现“边看边切”。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/QuickLookWindow.h`, `src/ui/QuickLookWindow.cpp`, `src/ui/ToolTipOverlay.cpp`, `src/ui/ScanDialog.cpp`

**明确禁止越界修改的范围：**
- [ ] 禁止为预览窗添加语法高亮库。
- [ ] 禁止修改 MFT 索引数据结构。

## 6. 实现准则与预警【核心】
1. **头文件依赖**：必须包含 `<windows.h>` 以使用 `SetWindowPos`。
2. **GPU 预热预警**：在 `QuickLookWindow` 构造时应执行一次静默的 `show()/hide()`，以消除首次绘制棋盘格背景时的 GPU 显存分配延迟。
3. **坐标校准**：居中计算必须考虑多显示器情况（使用 `QGuiApplication::screenAt(QCursor::pos())`）。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| UI 考古原则 | 实现新组件前必须先搜索现有同类案例 | ✅ (已深度考古 ToolTipOverlay 并纠正退化的源码) |
| 窗口置顶 | 必须使用 Win32 SetWindowPos | ✅ (方案明确采用此原生 API) |
| 编码规范 | 全程使用中文解说，不使用英文答复用户 | ✅ |

## 8. 待确认事项
- **大图标资源**：截图中显示的特定图标（如 Ctrl、Shift 的矩形边框风格）是否需要 1:1 绘制，还是直接使用现有的 SVG 图标？（本方案暂定使用现有 SVG 配合半透明边框渲染）。
