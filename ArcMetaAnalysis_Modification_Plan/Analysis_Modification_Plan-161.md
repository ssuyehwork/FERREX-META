# 标题栏还原/恢复按钮图标不一致问题修正 —— Analysis_Modification_Plan-161.md

## 1. 任务背景
在目前 FERREX 版本的无边框窗口还原/恢复（Restore）状态下，标题栏还原按钮引用的 SVG 图标与 ArcMeta 版本存在不一致（引用了不正确的 `"restore_window"` 图标，其原始视口不规则且在小尺寸下渲染不够精致）。为了保持两个版本在视觉外观上的高度统一和像素级精致感，必须将还原按钮引用的图标更正为 ArcMeta 版本的 `"restore_line"`。

## 2. 问题定位
* **引入差异位置 1**：FERREX 版本的图标库文件 `src/ui/SvgIcons.h` 中缺少 `"restore_line"` 对应的精美 SVG 原始码定义。
* **引入差异位置 2**：无边框对话框基类文件 `src/ui/FramelessDialog.cpp` 的第 119 行和第 122 行，在窗口状态最大化切换和初始化时，将恢复按钮（Restore Button）的图标错误地引用为了 `"restore_window"`。
  ```cpp
  // src/ui/FramelessDialog.cpp
  m_maxBtn = createTitleBtn("maximize", "最大化", "#333333");
  connect(m_maxBtn, &QPushButton::clicked, this, [this]() {
      if (isMaximized()) {
          showNormal();
          m_maxBtn->setIcon(UiHelper::getIcon("maximize", QColor("#CCCCCC"), 18));
      } else {
          showMaximized();
          m_maxBtn->setIcon(UiHelper::getIcon("restore_window", QColor("#CCCCCC"), 18));
      }
  });
  ```

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 当前版本 FERREX 恢复按钮的 svg 图标引用是错误的，必须按照 ArcMeta 版本那样 | 将 `src/ui/FramelessDialog.cpp` 中的 `"restore_window"` 图标引用更正为 `"restore_line"` | ✅ |
| 2    | 如果缺少相应的 svg 图标可以直接从 ArcMeta 版本里复制过来 | 从 `ArcMeta/src/ui/SvgIcons.h` 复制 `"restore_line"` 对应的 SVG 源码定义到 `src/ui/SvgIcons.h` 中 | ✅ |

## 4. 详细解决方案

### 第一步：在 `src/ui/SvgIcons.h` 中补全 `"restore_line"` SVG 图标源码
从 `ArcMeta/src/ui/SvgIcons.h` 复制 `"restore_line"` 键值项，并将其加入至 FERREX 版本的 `src/ui/SvgIcons.h` 的 `inline const QMap<QString, QString> icons` 字典中。

**伪代码/注释化片段：**
```cpp
// 在 src/ui/SvgIcons.h 的 icons 映射表里新增 restore_line 键：
        {"restore_line", R"svg(<svg viewBox="0 0 24 24" fill="currentColor" fill-rule="evenodd"><path d="M19,3 C20.0543909,3 20.9181678,3.81587733 20.9945144,4.85073759 L21,5 L21,15 C21,16.0543909 20.18415,16.9181678 19.1492661,16.9945144 L19,17 L17,17 L17,19 C17,20.0543909 16.18415,20.9181678 15.1492661,20.9945144 L15,21 L5,21 C3.94563773,21 3.08183483,20.18415 3.00548573,19.1492661 L3,19 L3,9 C3,7.94563773 3.81587733,7.08183483 4.85073759,7.00548573 L5,7 L7,7 L7,5 C7,3.94563773 7.81587733,3.08183483 8.85073759,3.00548573 L9,3 L19,3 Z M15,9 L5,9 L5,19 L15,19 L15,9 Z M19,5 L9,5 L9,7 L15,7 L15.1492661,7.00548573 C16.1324058,7.07801738 16.9178674,7.86122607 16.9939557,8.84334947 L17,9 L17,15 L19,15 L19,5 Z"/></svg>)svg"},
```

### 第二步：修改 `src/ui/FramelessDialog.cpp`
将无边框对话框切换最大化/还原状态时引用的图标名由 `"restore_window"` 更正为 `"restore_line"`。

**修正对比片段：**
```cpp
// src/ui/FramelessDialog.cpp

<<<<<<< SEARCH
    m_maxBtn = createTitleBtn("maximize", "最大化", "#333333");
    connect(m_maxBtn, &QPushButton::clicked, this, [this]() {
        if (isMaximized()) {
            showNormal();
            m_maxBtn->setIcon(UiHelper::getIcon("maximize", QColor("#CCCCCC"), 18));
        } else {
            showMaximized();
            m_maxBtn->setIcon(UiHelper::getIcon("restore_window", QColor("#CCCCCC"), 18));
        }
    });
=======
    m_maxBtn = createTitleBtn("maximize", "最大化", "#333333");
    connect(m_maxBtn, &QPushButton::clicked, this, [this]() {
        if (isMaximized()) {
            showNormal();
            m_maxBtn->setIcon(UiHelper::getIcon("maximize", QColor("#CCCCCC"), 18));
        } else {
            showMaximized();
            m_maxBtn->setIcon(UiHelper::getIcon("restore_line", QColor("#CCCCCC"), 18));
        }
    });
>>>>>>> REPLACE
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 头文件：`src/ui/SvgIcons.h`（新增 `"restore_line"` SVG 数据项映射）
- [ ] 源文件：`src/ui/FramelessDialog.cpp`（修正状态切换图标键名为 `"restore_line"`）

**明确禁止越界修改的范围：**
- [ ] 严禁修改其他无边框窗口行为或标题栏交互逻辑。

## 6. 实现准则与预警【核心】
1. **依赖头文件**：`src/ui/FramelessDialog.cpp` 已经通过 `#include "UiHelper.h"` 间接引入了图标系统。本次修改只需变更传入 `UiHelper::getIcon` 的 `QString` 字面量，不产生新的头文件依赖。
2. **风险避让**：由于修改仅涉及 SVG 字符串声明与图标加载的键名，不存在跨线程安全风险，对原有无边框窗口布局计算、DWM 阴影、以及 Windows 11 原生圆角等逻辑不构成任何物理破坏，做到真正的开箱即用。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 还原按钮图标外观 | 必须按照 ArcMeta 版本那样，保持高度一致与像素精致度 | ✅ 符合，完美平移 `"restore_line"` SVG 图标 |

## 8. 待确认事项（可选）
* 无
