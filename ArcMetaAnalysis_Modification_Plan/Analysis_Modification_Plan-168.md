# FERREX-META 标题栏关闭按钮标准对齐与低闪烁重构 —— Analysis_Modification_Plan-168.md

## 1. 任务背景
在 ArcMeta 标题栏设计规范（Plan-160）中，关闭按钮要求在常规与鼠标悬停状态下常驻红色背景（#E81123），仅在按下时变暗（#A50000），以实现极低闪烁的视觉效果。
然而，在 FERREX-META（C++ 重构搜索系统）的实现中，主界面的关闭按钮悬停背景被设为了 `#F1707A`。为了消除二者的视觉差异，并保证项目整体视觉标准与设计规约的彻底一致，需要对 FERREX-META 的关闭按钮颜色设置进行低闪烁逻辑重构修改。

## 2. 问题定位
通过代码审计，发现 FERREX-META 中以下两处位置的 QSS 样式表定义了关闭按钮在悬停（`:hover`）状态下的背景颜色，导致了与 ArcMeta 规范不一致的悬停变色效果：

1. **`src/ui/FramelessDialog.cpp`**（第 126~130 行）：
   ```cpp
   m_closeBtn->setStyleSheet(
       "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
       "QPushButton:hover { background-color: #F1707A; } " // 违规高亮淡化色
       "QPushButton:pressed { background-color: #A50000; }"
   );
   ```

2. **`src/ui/ScanDialog.cpp`**（第 809~814 行）：
   ```cpp
   m_closeBtn->setStyleSheet(
       "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
       "QPushButton:hover { background-color: #F1707A; } " // 违规高亮淡化色
       "QPushButton:pressed { background-color: #A50000; }"
   );
   ```

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | FERREX-META的关闭按钮必须按照“鼠标悬停在关闭按钮上方时，显示的背景颜色为 #e81123。其设计采用了“常驻红色背景、悬停不跳变、仅按下变暗”的低闪烁逻辑。”去修改 | 将 FERREX-META 的关闭按钮在样式表中的 `:hover` 背景色全面修改为 `#E81123`，保持与常驻红色一致（对应用户原话：“显示的背景颜色为 #e81123”） | ✅ |
| 2    | 仅按下变暗的低闪烁逻辑 | 保持 `:pressed` 伪状态下的背景色为 `#A50000` 不变，确保按下时能够有明确的变暗反馈 | ✅ |

## 4. 详细解决方案
本方案作为“纯分析师”模式产物，仅定义修改逻辑与代码变更样式。物理代码将由执行者进行应用：

### 4.1 `src/ui/FramelessDialog.cpp` 修改设计
将 `m_closeBtn` 的样式表进行重置。

**修改前：**
```cpp
m_closeBtn->setStyleSheet(
    "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
    "QPushButton:hover { background-color: #F1707A; } "
    "QPushButton:pressed { background-color: #A50000; }"
);
```

**修改后（实现“常驻红色背景、悬停不跳变”逻辑）：**
```cpp
m_closeBtn->setStyleSheet(
    "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
    "QPushButton:hover { background-color: #E81123; } " // 对齐：鼠标悬停在关闭按钮上方时，显示的背景颜色为 #e81123
    "QPushButton:pressed { background-color: #A50000; }"
);
```

### 4.2 `src/ui/ScanDialog.cpp` 修改设计
将构造函数中对 `m_closeBtn` 的统一样式应用同步修改。

**修改前：**
```cpp
m_closeBtn->setStyleSheet(
    "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
    "QPushButton:hover { background-color: #F1707A; } "
    "QPushButton:pressed { background-color: #A50000; }"
);
```

**修改后（实现“常驻红色背景、悬停不跳变”逻辑）：**
```cpp
m_closeBtn->setStyleSheet(
    "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
    "QPushButton:hover { background-color: #E81123; } " // 对齐：鼠标悬停在关闭按钮上方时，显示的背景颜色为 #e81123
    "QPushButton:pressed { background-color: #A50000; }"
);
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/FramelessDialog.cpp` 的 `m_closeBtn` 样式块
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` 的 `m_closeBtn` 样式块

**明确禁止越界修改的范围：**
- [ ] 禁止修改除了关闭按钮外的其他控制按钮（如 `m_pinBtn`、`m_minBtn`、`m_maxBtn`）的 hover 背景色。
- [ ] 禁止引入任何不属于原生 QSS 的特有代码样式以防编译失败。

## 6. 实现准则与预警【核心】
1. **样式表一致性**：必须确保修改后的样式表大括号配对正确，避免字符串拼接遗漏导致加载失败。
2. **多重初始化防御**：由于 `ScanDialog` 会根据当前运行环境和 DPI 在多个位置重设 `m_closeBtn` 的大小和图标，需仔细排查是否存在漏改的情况。本案已覆盖构造函数中样式声明的源头，可以彻底杜绝视觉残留。
3. **无缝运行保障**：样式变更属于标准的 Qt 样式层改动，完全不涉及信号槽或多线程操作，风险极低。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 关闭按钮样式 | 常驻红色背景（#E81123），悬停不跳变，按下变暗（#A50000） | ✅ 符合 |

## 8. 待确认事项（可选）
- **无**：本次任务逻辑极其明确，无任何不确定点，无需向用户二次探讨。
