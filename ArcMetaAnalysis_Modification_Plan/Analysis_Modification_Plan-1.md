# 彻底移除 MainWindow 架构简化分析 —— Analysis_Modification_Plan-136.md

## 1. 任务背景
<!-- 简述本次分析的触发原因与上下文 -->
用户要求将程序架构从“双主界面”模式（MainWindow + ScanDialog）简化为“单界面”模式。目标是彻底移除 `MainWindow` 及其所属的复杂六栏布局组件，仅保留轻量级的 `ScanDialog` 作为唯一主界面。本次任务需识别并列出所有因此不再参与编译的冗余文件。

## 2. 问题定位
<!-- 精确描述问题所在的模块、函数、行号（如已知），以及根因分析 -->
当前 `MainWindow` 采用了高度模块化的六面板布局，其依赖链涉及大量 UI 专用组件、数据模型及交互过滤器。这些文件在 `ScanDialog` 模式下完全处于隔离状态，且没有被其他核心逻辑引用。
- **核心冗余点**：`MainWindow.cpp` 及其子面板（Category/Nav/Content/Meta/Filter/AddressBar）。
- **递归冗余点**：由上述面板引用的专用 Model（CategoryModel）、Delegate（TreeItemDelegate）、对话框（BatchRenameDialog）及过滤器。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 将 mainwindow 界面彻底移除掉 | 在 4.1 节列出 MainWindow 及其直接关联文件 | ✅ |
| 2    | 只保留 ScanDialog 界面 | 审计并确保保留 ScanDialog 及其核心依赖 | ✅ |
| 3    | 将“src”文件夹里的哪些文件将不再参与编译...列出来 | 在 4.1 节提供详尽的文件清理清单 | ✅ |

## 4. 详细解决方案
<!-- 分步骤描述解决方案，可包含伪代码、流程说明、接口设计。禁止直接输出可执行代码文件。 -->

### 4.1 冗余文件清单（不再参与编译）

根据依赖审计结果，以下位于 `src/ui` 文件夹下的文件在移除 `MainWindow` 后将不再被任何逻辑调用，建议从编译系统中移除：

**【主窗口与导航类】**
- `src/ui/MainWindow.cpp` / `MainWindow.h`（核心移除目标）
- `src/ui/AddressBar.cpp` / `AddressBar.h`
- `src/ui/BreadcrumbBar.cpp` / `BreadcrumbBar.h`
- `src/ui/NavPanel.cpp` / `NavPanel.h`
- `src/ui/CategoryPanel.cpp` / `CategoryPanel.h`

**【内容与元数据管理类】**
- `src/ui/ContentPanel.cpp` / `ContentPanel.h`
- `src/ui/MetaPanel.cpp` / `MetaPanel.h`
- `src/ui/FilterPanel.cpp` / `FilterPanel.h`
- `src/ui/SearchHistoryPanel.cpp` / `SearchHistoryPanel.h`
- `src/ui/QuickLookWindow.cpp` / `QuickLookWindow.h`

**【专用对话框与逻辑类】**
- `src/ui/BatchRenameDialog.cpp` / `BatchRenameDialog.h`
- `src/ui/BatchRenamePreviewDialog.cpp` / `BatchRenamePreviewDialog.h`
- `src/ui/RuleRow.cpp` / `RuleRow.h`
- `src/ui/CategoryLockDialog.cpp` / `CategoryLockDialog.h`
- `src/ui/CategorySetPasswordDialog.cpp` / `CategorySetPasswordDialog.h`
- `src/ui/ColorPicker.cpp` / `ColorPicker.h`

**【模型、代理与自定义视图类】**
- `src/ui/CategoryModel.cpp` / `CategoryModel.h`
- `src/ui/CategoryDelegate.h`
- `src/ui/TreeItemDelegate.h`
- `src/ui/DropJustifiedView.cpp` / `DropJustifiedView.h`
- `src/ui/DropListView.cpp` / `DropListView.h`
- `src/ui/DropTreeView.cpp` / `DropTreeView.h`

**【辅助功能与视觉增强类】**
- `src/ui/LoadingWindow.cpp` / `LoadingWindow.h`（启动预热窗）
- `src/ui/ToolTipOverlay.cpp` / `ToolTipOverlay.h`（自定义悬浮提示）
- `src/ui/HoverEventFilter.cpp` / `HoverEventFilter.h`
- `src/ui/ResizeEventFilter.cpp` / `ResizeEventFilter.h`
- `src/ui/TrayController.cpp` / `TrayController.h`（系统托盘管理）
- `src/ui/ProgressDialog.h`（长耗时任务进度条）
- `src/ui/Logger.h`（UI 层调试日志）
- `src/ui/StyleLibrary.h`（UI 样式常量库）

### 4.2 必须保留的核心依赖清单
以下文件虽然位于 `ui` 文件夹，但由于被 `ScanDialog` 或底层引擎引用，**严禁移除**：
- `src/ui/ScanDialog.cpp / .h`
- `src/ui/ScanController.cpp / .h`
- `src/ui/JustifiedView.cpp / .h`
- `src/ui/ThumbnailDelegate.cpp / .h`
- `src/ui/FramelessDialog.cpp / .h`（ScanDialog 基类）
- `src/ui/UiHelper.h`（核心工具类）
- `src/ui/SvgIcons.h`（图标资源索引）

## 5. 修改边界声明【红线】
<!-- 明确列出本方案涉及的范围，以及明确禁止触碰的范围 -->

**本次方案涉及范围：**
- [x] 审计范围：`src/ui` 文件夹及其内部所有子组件。
- [x] 逻辑范围：基于 `MainWindow` 及其依赖链的文件活跃度分析。

**明确禁止越界修改的范围：**
- [ ] 严禁移除 `src/core`, `src/db`, `src/meta`, `src/mft` 目录下的任何非 UI 逻辑文件。
- [ ] 严禁修改 `ScanDialog` 的内部功能实现（对应用户原话：“只保留 ScanDialog 界面”）。

## 6. 实现准则与预警【核心】
<!-- 
1. 必须精准指出方案依赖的头文件（#include），防止出现"找不到标识符"等编译错误。 
2. 必须核对并指出需要新增或调整的命名空间、变量声明位置。 
3. 对方案中涉及的高风险操作（如跨线程调用、异步回调中的 UI 操作）提供显式预警。 
4. 修改代码必须结合上下文来修改代码，做到开箱即用。 
-->
1. **入口重定向预警**：在执行清理后，必须在 `src/main.cpp` 中将 `MainWindow` 引用替换为 `ScanDialog`。
2. **符号依赖检查**：移除 `StyleLibrary.h` 前需确认 `UiHelper` 是否有隐式宏定义依赖（当前审计未发现）。
3. **资源清理**：建议在 `.pro` 或 `CMakeLists.txt` 中同步移除对应的文件声明，以避免构建系统因找不到物理文件而报错。

## 7. Memories.md 合规检查
<!-- 列出本次方案新增或涉及的组件，逐条对照 Memories.md 中的标准规范，确认合规 -->

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| MainWindow 剥离 | MainWindow 上帝对象剥离 | ✅ 符合 (彻底物理剥离) |
| ScanDialog 架构 | 采用异步控制器驱动 | ✅ 符合 (保留 ScanController) |
| 启动逻辑 | 直接显示主窗口 (2026-04-13 规约) | ✅ 符合 (ScanDialog 取代 MainWindow) |

## 8. 待确认事项（可选）
<!-- 如果方案存在不确定点，列于此处，等待用户进一步确认，不得自行假设 -->
1. **系统托盘功能**：`TrayController` 伴随 `MainWindow` 移除后，程序将失去系统托盘功能。是否确认 `ScanDialog` 模式下不需要托盘图标？（对应用户原话：“彻底移除掉 mainwindow 界面”）
2. **批量重命名功能**：`BatchRenameDialog` 被归类为冗余文件。如果 `ScanDialog` 后续需要批量重命名，则需将其迁移或保留。当前 `ScanDialog` 仅支持单文件 F2 重命名。是否确认不需要批量重命名功能？
