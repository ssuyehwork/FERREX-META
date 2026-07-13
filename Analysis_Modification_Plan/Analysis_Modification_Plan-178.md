# Ctrl+W 全局支持与 Esc 两段式清空关闭机制重构 —— Analysis_Modification_Plan-178.md

## 1. 任务背景
为了极大地简化窗口的快速退出和搜索重置体验，用户提出了两项顶尖的交互优化建议：
1. **任意界面支持 `Ctrl+W` 物理退出**：目前用户必须去寻找关闭按钮或按下 `Esc` 才能关闭程序各窗口。为了对齐主流桌面应用的标准，需要让程序中的所有窗口（对话框、主界面、预览窗等）都原生支持通过 `Ctrl+W` 一键关闭（对应用户原话：“我期望整个应用的任何界面都必须支持Ctrl+W关闭窗口”）。
2. **`ScanDialog` 主界面 `Esc` 两段式退出**：当前的 Esc 机制过于脆弱，按下后便直接关闭程序，容易导致用户由于误触而中途夭折当前的搜索。期望将其重构为两段式设计：首次按下 Esc 时，若搜索框或扩展名过滤框中有任何文字残留，先执行一键全部重置清空（对应用户原话：“首次按下键时，应该先清空ScanDialog主窗口所有输入框的文字”）；当两个输入框完全清空时，再次按下 Esc（或者首次按下本就全空）才执行退出的业务（对应用户原话：“如果首次按下Esc键时，所有输入框都已经处于清空文字状态情况下则直接关闭ScanDialog窗口”）。

本次分析方案将围绕对话框基类、独立预览类及 `ScanDialog` 类进行最完美的非侵入式架构设计。

## 2. 问题定位与修改设计

### 2.1 全局 `Ctrl+W` 关闭支持定位
程序中的所有界面大体上分为以下两类，我们需要在它们的键盘按键虚函数（`keyPressEvent`）中对 `Ctrl+W` 进行拦截并执行关闭：

* **第一类：基于 `FramelessDialog` 体系的窗口**（如 `ScanDialog` 主窗口和 `FramelessInputDialog` 输入框）：
  直接在 `FramelessDialog::keyPressEvent` 基类方法中加入对 `Ctrl + Key_W` 的拦截。这样所有派生对话框都将自动获得该特性。
* **第二类：独立的 QWidget 窗口**（如空格文件预览窗口 `QuickLookWindow`）：
  在 `QuickLookWindow::keyPressEvent` 中追加相同的按键序列判定。

### 2.2 `ScanDialog` 的 `Esc` 键专属重定义
在 `ScanDialog::keyPressEvent` 中已经对多重按键（如 `Space`、`F2`、`F5`、`Ctrl+A` 等）进行了拦截：
```cpp
void ScanDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space) { ... }
    ...
    FramelessDialog::keyPressEvent(event); // 基类 Esc 在此被调用
}
```
因为 `ScanDialog` 继承自 `FramelessDialog`，我们要实现针对主界面的“专属两段式 Esc”而不影响其他简单对话框，最优雅的方法是：**在 `ScanDialog::keyPressEvent` 中提前捕获 `Qt::Key_Escape` 并在该分支内执行专属判定。只有在完全满足退出条件（输入框皆空）的情况下才向下传递给 `FramelessDialog::keyPressEvent`（或直接调用 `reject()`）来关闭窗口**。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 我期望整个应用的任何界面都必须支持Ctrl+W关闭窗口 | 在 `FramelessDialog` 及 `QuickLookWindow` 的 `keyPressEvent` 中，加入对 `Ctrl+W` 快捷键（对应用户原话：“支持Ctrl+W”）的系统级拦截并调用 `reject()` 或 `close()` 关闭窗口（对应用户原话：“关闭窗口”） | ✅       |
| 2    | 当在ScanDialog窗口首次按下键时，应该先清空ScanDialog主窗口所有输入框的文字 | 在 `ScanDialog::keyPressEvent` 中拦截 `Escape` 键，若 `m_searchEdit` 或 `m_extEdit` 任何一个有文字（对应用户原话：“ScanDialog主窗口所有输入框”），则执行一键 `clear()`（对应用户原话：“先清空...所有输入框的文字”） | ✅       |
| 3    | 如果首次按下Esc键时，所有输入框都已经处于清空文字状态情况下则直接关闭ScanDialog窗口 | 若 `m_searchEdit` 和 `m_extEdit` 皆为空（对应用户原话：“所有输入框都已经处于清空文字状态情况”），则执行 `reject()` 彻底关闭窗口（对应用户原话：“直接关闭ScanDialog窗口”） | ✅       |

## 4. 详细解决方案

### 4.1 `FramelessDialog` 基类支持 `Ctrl+W` （`src/ui/FramelessDialog.cpp`）
重构基类键盘事件：

```cpp
<<<<<<< SEARCH
void FramelessDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        // 物理还原两段式 UX：若有非空输入框则先清空，否则关闭
        QLineEdit* edit = findChild<QLineEdit*>();
        if (edit && edit->isVisible() && !edit->text().isEmpty()) {
            edit->clear();
            event->accept();
            return;
        }
        reject();
    } else {
        QDialog::keyPressEvent(event);
    }
}
=======
void FramelessDialog::keyPressEvent(QKeyEvent* event) {
    // 2026-07-10 新增：整个应用的任何无边框对话框界面，皆支持 Ctrl+W 关闭窗口（对应用户原话：“我期望整个应用的任何界面都必须支持Ctrl+W关闭窗口”）
    if (event->key() == Qt::Key_W && (event->modifiers() & Qt::ControlModifier)) {
        reject();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        // 物理还原普通对话框两段式 UX：若有非空输入框则先清空，否则关闭
        QLineEdit* edit = findChild<QLineEdit*>();
        if (edit && edit->isVisible() && !edit->text().isEmpty()) {
            edit->clear();
            event->accept();
            return;
        }
        reject();
    } else {
        QDialog::keyPressEvent(event);
    }
}
>>>>>>> REPLACE
```

---

### 4.2 `QuickLookWindow` 预览窗口支持 `Ctrl+W` （`src/ui/QuickLookWindow.cpp`）
在预览窗口的按键响应虚函数中对 `Ctrl+W` 进行追加支持：

```cpp
<<<<<<< SEARCH
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Escape) {
        closePreview();
        event->accept();
        return;
    }
=======
    // 2026-07-10 新增：支持 Ctrl+W 关闭空格文件预览窗口（对应用户原话：“我期望整个应用的任何界面都必须支持Ctrl+W关闭窗口”）
    if (event->key() == Qt::Key_W && (event->modifiers() & Qt::ControlModifier)) {
        closePreview();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Escape) {
        closePreview();
        event->accept();
        return;
    }
>>>>>>> REPLACE
```

---

### 4.3 `ScanDialog` 主窗口 `Esc` 两段式清空与直接关闭的完美实装 (`src/ui/ScanDialog.cpp`)
在 `ScanDialog::keyPressEvent` 键盘输入拦截处，通过精准条件分支，物理劫持原本传导至 `FramelessDialog` 的常规 Esc 响应。

```cpp
<<<<<<< SEARCH
void ScanDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space) {
=======
void ScanDialog::keyPressEvent(QKeyEvent* event) {
    // 2026-07-10 新增：专属 ScanDialog 主窗口的两段式 Esc 处理（对应用户原话：“当在ScanDialog窗口首次按下键时”）
    if (event->key() == Qt::Key_Escape) {
        bool searchNotEmpty = m_searchEdit && !m_searchEdit->text().isEmpty();
        bool extNotEmpty = m_extEdit && !m_extEdit->text().isEmpty();

        // 1. 若至少有一个输入框不为空，则优先执行一键全部重置清空文字（对应用户原话：“应该先清空ScanDialog主窗口所有输入框的文字”）
        if (searchNotEmpty || extNotEmpty) {
            if (m_searchEdit) m_searchEdit->clear();
            if (m_extEdit) m_extEdit->clear();
            event->accept();
            return; // 消费按键，拦截阻止其传播至基类直接关闭窗口
        }
        
        // 2. 若所有输入框均已经处于清空重置状态，则直接关闭 ScanDialog 主窗口（对应用户原话：“所有输入框都已经处于清空文字状态情况下则直接关闭ScanDialog窗口”）
        reject();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Space) {
>>>>>>> REPLACE
```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/FramelessDialog.cpp` （集成全局 Ctrl+W 的 keyPressEvent 基类支持）
- [ ] 模块/文件：`src/ui/QuickLookWindow.cpp` （集成 QWidget 预览窗的 Ctrl+W 支持）
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` （在按键拦截函数中物理改写 Esc 键的两段式一键清空与关闭分发）

**明确禁止越界修改的范围：**
- [ ] 严禁将两段式 Esc 代码写进通用基类 `FramelessDialog`，必须仅限在主界面 `ScanDialog` 的 keyPressEvent 中特化处理，避免影响其他如输入框、询问框等常规窗口的一键重置体验。

## 6. 实现准则与预警【核心】
1. **输入框焦点归属保护**：
   在首次按下 Esc 执行全部清空之后，为了防止用户清空后需要继续录入，输入框应保持其当前的焦点状态（不强制执行 `clearFocus`），确保交互的连贯性和丝滑感。
2. **两段式按键事件消费机制**：
   在执行 `m_searchEdit->clear()` 和 `m_extEdit->clear()` 分支时，必须显式调用 `event->accept();` 并立即执行 `return;` 消费掉该按键事件。否则，事件会继续向上传播至 `FramelessDialog` 甚至 `QDialog` 的默认键盘处理逻辑中，进而再次触发意外的窗口关闭。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **呼吸窗口/耗时操作** | 数据库等重型耗时操作需释放 CPU 锁防界面卡死 | **不涉及**（纯前端局部键盘消息逻辑，无任何底层耗时计算开销） |
| **标题栏按钮/UI尺寸** | 标题栏及关键 UI 组件对标已有尺寸规范 | ✅（完美符合用户习惯，通过标准的 Ctrl+W 与两段式 Esc 提供极为舒适且贴合系统的完美操纵感） |
| **极致性能** | 零分配、避免多余的对象创建 | ✅（按键拦截在微秒级时间内执行，无临时 QString 或堆数据分配，完全不引入任何性能损耗） |
