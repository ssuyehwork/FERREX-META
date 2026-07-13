# 动态文件预览规则管理 —— Analysis_Modification_Plan-180.md

## 1. 任务背景
目前空格预览的文件类型拦截（黑名单）和准入（白名单）逻辑是硬编码在代码中的，这限制了用户的扩展性。为了提高灵活性，我们需要将这些规则持久化到 `ScanConfig` 中，并在 `ScanDialog` 标题栏新增一个“预览规则配置”按钮。点击该按钮会弹出无边框的设置窗口，供用户动态修改白名单和黑名单扩展名。

## 2. 解决方案

### 2.1 配置类定义扩展
在 `ScanConfig` 中引入 `previewBlacklist` 和 `previewWhitelist`。
在 `load()` 时如果检测到缺失，则以出厂默认值进行兜底填充，并自动回写。

### 2.2 辅助校验函数重构
将 `isPathPreviewable` 修改为接收 `ScanConfig` 引用的函数，以便能够根据内存中的动态配置进行实时拦截：
```cpp
static bool isPathPreviewable(const QString& path, const ScanConfig& config);
```

### 2.3 设置界面实现
构建 `PreviewRulesDialog` 继承自 `FramelessDialog`，在其 `contentArea` 中放置两个 `QTextEdit` 编辑框，分别对应白名单与黑名单，输入支持以空格或逗号分隔。

---

## 3. 详细修改

- `src/ui/ScanDialog.h`
- `src/ui/ScanDialog.cpp`
