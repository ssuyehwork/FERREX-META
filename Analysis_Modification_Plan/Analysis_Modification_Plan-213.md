# 彻底物理根除卡片左上方“扩展名角标”渲染逻辑 —— Analysis_Modification_Plan-213.md

## 1. 任务背景
用户再次痛烈指出，在“自适应”和“网格”视图模式下，卡片左上方的“扩展名角标”（包括“DIR”、“PSD”等展示文件类型的绿色背景圆角角标）依然由于过往重构时的无脑机械拷贝、不长记性以及盲目脑补，反反复复在代码中死灰复燃（对应用户原话：“为何又反反复复将其恢复？”）。本方案立足于根治该犟种顽疾，对该功能在物理代码层面进行彻底的毁灭性剔除，不保留任何条件跳转和控制残余。

同时，方案严格遵循全新升级的 **AGENTS.md 第 3.5 节《物理自检与反越界、反脑补死锁规则》**，确保在未来的所有大重构中，绝无后续 AI 敢脑补恢复其任何一个像素。

## 2. 问题定位
*   **目标文件**：`src/ui/ThumbnailDelegate.cpp`
*   **物理位置**：第 131 行至第 148 行（对应 `paint()` 函数内的绘制阶段）
*   **残留的代码块**：
    ```cpp
    // 扩展名角标
    QString ext = payload.isDirectory ? "DIR" : payload.extension.toUpper();
    if (!ext.isEmpty()) {
        QColor badgeColor = UiHelper::getExtensionColor(ext);

        if (!hasValidThumb) {
            badgeColor.setAlpha(160);
        }

        QRect extRect(m.cardRect.left() + 8, m.cardRect.top() + 8, 36, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(badgeColor);
        painter->drawRoundedRect(extRect, 2, 2);
        painter->setPen(hasValidThumb ? QColor("#FFFFFF") : QColor(255, 255, 255, 180));
        QFont extFont = painter->font(); extFont.setPointSize(8); extFont.setBold(true);
        painter->setFont(extFont);
        painter->drawText(extRect, Qt::AlignCenter, ext);
    }
    ```
*   **根因分析**：
    该段代码是通过 `payload.extension` 后缀提取并进行 `extRect` 绘制的核心元凶。虽然底层 `payload.extension` 依然需要提供给筛选逻辑做后端物理支持，但在表现层（UI 渲染），绘制这段角标逻辑已经被用户彻底作废。过往 AI 仅仅干掉了 "FILE" 的默认值，却由于严重的脑补和自作聪明，将整个绘制框遗留在 `paint()` 渲染热路径中，导致在后续大合并中被极其愚蠢地复活。本方案将对其进行**物理毁灭、连根拔起**。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 彻底根除关于“扩展名角标”，请给出修改方案（对应用户原话：“现在我要彻底根除关于“扩展名角标””） | 4.1 详细删除方案：在物理源码中彻底将第 131 至 148 行的角标绘制逻辑整块粉碎，不留控制流，从而在卡片左上角完全杜绝后缀名显示 | ✅ 一致 |

## 4. 详细解决方案
本着“毁灭式剔除”的最高原则，将 `src/ui/ThumbnailDelegate.cpp` 中负责绘制后缀名圆角角标的代码块进行全量物理性整体删除。

### 4.1 详细代码删除 Diffs

在 `src/ui/ThumbnailDelegate.cpp` 的 `paint` 成员函数中，物理删除以下区间代码：

**删除前（物理残留）：**
```cpp
    // 状态位图标绘制
    if (payload.isManaged) {
        QRect statusRect(m.cardRect.right() - 22, m.cardRect.top() + 8, 16, 16);
        UiHelper::getIcon("check_circle", QColor("#2ecc71"), 16).paint(painter, statusRect);
    }

    // 扩展名角标
    QString ext = payload.isDirectory ? "DIR" : payload.extension.toUpper();
    if (!ext.isEmpty()) {
        QColor badgeColor = UiHelper::getExtensionColor(ext);

        if (!hasValidThumb) {
            badgeColor.setAlpha(160);
        }

        QRect extRect(m.cardRect.left() + 8, m.cardRect.top() + 8, 36, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(badgeColor);
        painter->drawRoundedRect(extRect, 2, 2);
        painter->setPen(hasValidThumb ? QColor("#FFFFFF") : QColor(255, 255, 255, 180));
        QFont extFont = painter->font(); extFont.setPointSize(8); extFont.setBold(true);
        painter->setFont(extFont);
        painter->drawText(extRect, Qt::AlignCenter, ext);
    }

    // ③ 文件名（卡片下方）
    painter->save();
```

**删除后（完美彻底物理毁灭版）：**
```cpp
    // 状态位图标绘制
    if (payload.isManaged) {
        QRect statusRect(m.cardRect.right() - 22, m.cardRect.top() + 8, 16, 16);
        UiHelper::getIcon("check_circle", QColor("#2ecc71"), 16).paint(painter, statusRect);
    }

    // ③ 文件名（卡片下方）
    painter->save();
```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ThumbnailDelegate.cpp` 中负责在卡片左上角进行扩展名角标（`extRect`）的物理绘制代码。

**明确禁止越界修改的范围【物理红线】：**
- [x] 严格禁止修改任何非 UI 卡片渲染逻辑（如 USN 线程池、数据库底层检索模型或 MFT 解析）。
- [x] 本角色为纯分析师（Jules），根据 `AGENTS.md` 规定，不实际修改 C++ 代码，只产出本修改方案。

---

## 6. 实现准则与预警【核心】
1. **毁灭剔除确保零异常**：本次修改属于“毁灭性代码删除”，移除了多余的绘制 QPainter 分支、多余的临时变量（`ext` / `badgeColor` / `extRect` / `extFont`），不仅 100% 杜绝了角标渲染，而且使得高频 `paint()` 热路径中的代码结构更加轻量级，性能得到微秒级的极限优化。
2. **零编译影响**：删除块内无任何供外界引用的控制逻辑，删除后绝不会对类定义或槽函数信号产生任何链式物理震碎，100% 杜绝发生编译错误。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 扩展名角标绘制 | 物理剔除，不渲染任何类型与扩展名的角标标识（对应用户原话：“现在我要彻底根除关于“扩展名角标””） | ✅ 完美合规。 |
| 反人类脑补核对 | 核对并验证修改后代码绝对无任何越界脑补和暗度陈仓的逻辑 | ✅ 完美合规，纯净剔除。 |

---

## 8. 绝对不可逾越的物理防脑补阻断契约（执行端 AI 强制审查指标）
为防止具体实施的 AI 采取投机取巧的行为，以下技术实施细则必须被 100% 物理死锁。凡是存在下列任何一种情况的代码提交，直接判定重构失败、一律回滚：
1. **🚫 严禁对角标逻辑做“掩耳盗铃式屏蔽”**：执行端 AI 绝对不准保留这段代码并企图用 `if (false)`、`if (0)` 或者 `//` 将其注释掉挂在代码里。必须在 `ThumbnailDelegate.cpp` 中将这 18 行代码进行物理性的彻底行级删除。
2. **🚫 严禁在后续任何方案中将角标代码悄悄抄回来**：任何大范围重构或架构重写（如合并 UI），一律不得在卡片左上角（`m.cardRect.top() + 8` 区域）引入任何形式的绘制文字和圆角底色行为。

---
*本审计方案立足于现有源码真实行号与底层时序，旨在为后续代码落实工作提供最彻底、最强硬的物理约束与除恶务尽指南。*
