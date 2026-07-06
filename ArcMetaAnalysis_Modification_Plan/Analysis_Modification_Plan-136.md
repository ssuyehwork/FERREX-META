# 彻底移除 MainWindow 架构简化分析 —— Analysis_Modification_Plan-136.md

## 1. 任务背景
<!-- 简述本次分析的触发原因与上下文 -->
用户要求将程序架构从“双主界面”（对应用户原话：“共有两个主界面”）简化为“单界面”模式。具体目标是将 `MainWindow` 彻底移除（对应用户原话：“彻底移除掉”），仅保留（对应用户原话：“只保留 ScanDialog 界面”） `ScanDialog` 作为唯一主界面。同时，需实现配置独享（对应用户原话：“有些配置值应该是独享的”）并保留托盘支持（对应用户原话：“保留托盘图标支持”）。

## 2. 问题定位
<!-- 精确描述问题所在的模块、函数、行号（如已知），以及根因分析 -->
- **架构冗余**：现有系统并行维护两套主界面入口。
- **配置冲突**：共享配置项可能导致窗口状态逻辑冲突（对应用户原话：“避免发生冲突”）。
- **托盘依赖**：`TrayController` 原本绑定于 `QMainWindow`，移除后者需重新适配。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 将 mainwindow 界面彻底移除掉 | 4.1 节列出冗余文件 | ✅ |
| 2    | 只保留 ScanDialog 界面 | 4.2 节保留核心组件 | ✅ |
| 3    | 各自有各自的配置值 | 4.3 节定义独立配置域 | ✅ |
| 4    | 保留托盘图标支持 | 4.4 节集成托盘支持 | ✅ |

## 4. 详细解决方案

### 4.1 冗余文件清单（不再参与编译）
根据审计，以下位于 `src/ui` 文件夹（对应用户原话：“src”文件夹里的哪些文件）的文件将不再参与编译：
- `src/ui/MainWindow.cpp` / `MainWindow.h`
- `src/ui/AddressBar.cpp` / `AddressBar.h`
- `src/ui/BreadcrumbBar.cpp` / `BreadcrumbBar.h`
- `src/ui/NavPanel.cpp` / `NavPanel.h`
- `src/ui/CategoryPanel.cpp` / `CategoryPanel.h`
- `src/ui/ContentPanel.cpp` / `ContentPanel.h`
- `src/ui/MetaPanel.cpp` / `MetaPanel.h`
- `src/ui/FilterPanel.cpp` / `FilterPanel.h`
- `src/ui/SearchHistoryPanel.cpp` / `SearchHistoryPanel.h`
- `src/ui/QuickLookWindow.cpp` / `QuickLookWindow.h`
- `src/ui/BatchRenameDialog.cpp` / `BatchRenameDialog.h`
- `src/ui/BatchRenamePreviewDialog.cpp` / `BatchRenamePreviewDialog.h`
- `src/ui/RuleRow.cpp` / `RuleRow.h`
- `src/ui/CategoryLockDialog.cpp` / `CategoryLockDialog.h`
- `src/ui/CategorySetPasswordDialog.cpp` / `CategorySetPasswordDialog.h`
- `src/ui/ColorPicker.cpp` / `ColorPicker.h`
- `src/ui/CategoryModel.cpp` / `CategoryModel.h`
- `src/ui/CategoryDelegate.h`
- `src/ui/TreeItemDelegate.h`
- `src/ui/DropJustifiedView.cpp` / `DropJustifiedView.h`
- `src/ui/DropListView.cpp` / `DropListView.h`
- `src/ui/DropTreeView.cpp` / `DropTreeView.h`
- `src/ui/LoadingWindow.cpp` / `LoadingWindow.h`
- `src/ui/ToolTipOverlay.cpp` / `ToolTipOverlay.h`
- `src/ui/HoverEventFilter.cpp` / `HoverEventFilter.h`
- `src/ui/ResizeEventFilter.cpp` / `ResizeEventFilter.h`
- `src/ui/ProgressDialog.h`
- `src/ui/Logger.h`
- `src/ui/StyleLibrary.h`

### 4.2 必须保留的核心依赖清单
- `src/ui/ScanDialog.cpp / .h`
- `src/ui/ScanController.cpp / .h`
- `src/ui/JustifiedView.cpp / .h`
- `src/ui/ThumbnailDelegate.cpp / .h`
- `src/ui/FramelessDialog.cpp / .h`
- `src/ui/UiHelper.h`
- `src/ui/SvgIcons.h`
- **`src/ui/TrayController.cpp / .h`**（对应用户原话：“保留托盘图标支持”）

### 4.3 配置独立化方案
为实现“独享的”（对应用户原话：“有些配置值应该是独享的”）配置管理：
- **重定向**：ScanDialog 使用 `ScanDialog/` 命名空间替代原有的 `MainWindow/` 前缀。
- **目的**：确保两个主界面逻辑解耦（对应用户原话：“避免发生冲突”）。

### 4.4 托盘图标集成
- **功能保留**：完整保留 `TrayController` 并在 `ScanDialog` 中初始化（对应用户原话：“保留托盘图标支持”）。
- **逻辑重定向**：将托盘菜单的激活目标从 `MainWindow` 指向 `ScanDialog`。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [x] 审计范围：`src/ui` 内部组件活跃度。

**明确禁止越界修改的范围：**
- [ ] 严禁修改底层数据库（Database.h）的连接逻辑。

## 6. 实现准则与预警【核心】
1. **入口重定向**：必须在 `main.cpp` 中将启动实例指向 `ScanDialog`。
2. **符号校验**：确认剥离后无残留的界面引用。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 界面架构 | 移除上帝对象 MainWindow | ✅ 符合 |
| 托盘管理 | 完整保留并适配 | ✅ 符合 |

## 8. 待确认事项
（暂无）
