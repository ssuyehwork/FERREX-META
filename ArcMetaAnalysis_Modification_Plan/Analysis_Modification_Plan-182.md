# “预览规则配置”窗口非模态重构 —— Analysis_Modification_Plan-182.md

## 1. 任务背景

在当前系统设计中，用户点击标题栏的“预览规则配置”按钮（`rulesBtn`）时，系统会以模态方式打开一个对话框窗口。目前这一步是通过局部变量创建并在栈上调用其 `exec()` 成员函数实现的（对应用户原话：““预览规则配置”窗口，禁止采用 exec() 这种模态阻塞的傻逼方式”）。

传统的 `exec()` 方法在调用时会启动一个内部局部的事件循环（Nested Event Loop），导致调用者线程被阻塞在 `exec()` 执行点。这种模态方式会完全锁死主界面的底层操作（如无法拖拽、无法响应背景点击、无法并行筛选与搜索），严重影响百万级高性能系统的极速感官。

因此，我们需要将“预览规则配置”窗口改造为非模态、非阻塞的架构模式，使用 `show()` 进行异步非模态渲染，并通过信号槽异步保存结果、依靠 Qt 内置机制自主释放生命周期。

---

## 2. 问题定位

在主界面源文件 `src/ui/ScanDialog.cpp` 的 `rulesBtn` 信号连接部分（约第 1120 行），存在以下直接调用阻塞的槽函数实现：
```cpp
            connect(rulesBtn, &QPushButton::clicked, this, [this]() {
                PreviewRulesDialog dlg(m_config, this);
                if (dlg.exec() == QDialog::Accepted) {
                    m_config.save();
                }
            });
```

### 缺点分析：
1. **阻塞主交互：** 在 `dlg.exec()` 返回之前，用户完全无法点击或操作主界面窗口。
2. **多重弹窗风险：** 嵌套事件循环在高频点击下可能会引起不期而遇的重绘卡顿。
3. **栈生命周期局限：** `PreviewRulesDialog dlg(...)` 作为局部的栈对象，不能通过 `show()` 直接调用（一旦离开 lambda 作用域，该栈对象就会被立即析构导致窗口闪现随即消失）。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | “预览规则配置”窗口，禁止采用 exec() 这种模态阻塞的傻逼方式 | 废除在槽函数中通过栈对象调用 `exec()` 的逻辑，改造为堆分配并用 `show()` 显示非模态窗口（对应用户原话：““预览规则配置”窗口，禁止采用 exec() 这种模态阻塞的傻逼方式”） | ✅       |

---

## 4. 详细解决方案

### 4.1 重构设计思路
为了保证不阻塞主界面并优雅释放资源：
1. **堆内存分配：** 改在堆上动态创建对象，避免作用域结束而被自动销毁：
   ```cpp
   PreviewRulesDialog* dlg = new PreviewRulesDialog(m_config, this);
   ```
2. **非模态显示：** 取代 `dlg.exec()`，直接调用：
   ```cpp
   dlg->show();
   ```
3. **安全自析构属性：** 为了彻底避免内存泄漏（Memory Leak），必须要显式设置 Qt 窗口自销毁属性。该属性指引 Qt 在窗口关闭后自动调用 `deleteLater()` 释放其对应的堆内存：
   ```cpp
   dlg->setAttribute(Qt::WA_DeleteOnClose);
   ```
4. **异步信号监听与存盘：**
   原先 `exec() == QDialog::Accepted` 的判断是在阻塞返回后同步进行的。
   在非模态异步模式下，应通过 Qt 信号槽机制，监听 `QDialog::accepted` 信号：
   ```cpp
   connect(dlg, &QDialog::accepted, this, [this]() {
       m_config.save();
   });
   ```

### 4.2 最终代码重构效果对比（拟修改处）

在 `src/ui/ScanDialog.cpp` 中执行以下替换：

```cpp
<<<<<<< SEARCH
            connect(rulesBtn, &QPushButton::clicked, this, [this]() {
                PreviewRulesDialog dlg(m_config, this);
                if (dlg.exec() == QDialog::Accepted) {
                    m_config.save();
                }
            });
=======
            connect(rulesBtn, &QPushButton::clicked, this, [this]() {
                // 彻底废除模态阻塞机制，采用堆上非模态窗口 (对应用户原话：““预览规则配置”窗口，禁止采用 exec() 这种模态阻塞的傻逼方式”)
                auto* dlg = new PreviewRulesDialog(m_config, this);

                // 确保用户关闭该窗口时能安全触发自我析构，杜绝任何内存泄露
                dlg->setAttribute(Qt::WA_DeleteOnClose);

                // 通过非模态异步槽函数，当用户在配置窗口中点击确认 (Accepted) 时，自动保存当前最新的配置参数
                connect(dlg, &QDialog::accepted, this, [this]() {
                    m_config.save();
                });

                // 使用非阻塞的 show() 唤醒视图，保障主界面的顺畅渲染与交互
                dlg->show();
            });
>>>>>>> REPLACE
```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.cpp`（重构 `rulesBtn` 信号触发逻辑，去除 `exec()` 并全面替换为非模态堆对象托管方案）

**明确禁止越界修改的范围：**
- [ ] 严禁修改 `PreviewRulesDialog` 类定义及其构造逻辑。
- [ ] 严禁在其他与“预览规则配置”无关的模态/非模态对话框上擅自扩展该逻辑。
- [ ] 作为“纯分析师”模式，本方案文档严禁包含实际的代码写入动作，Jules 严禁直接通过 `write_file` 等操作去修改任何 C++ 源码。

---

## 6. 实现准则与预警【核心】
1. **防止内存泄露：** 必须百分之百设置 `Qt::WA_DeleteOnClose` 属性，若缺失此项会导致每次点击都发生不可回收的 QWidget 对象内存积压。
2. **生命周期绑定：** 将 `this`（ScanDialog）作为 `PreviewRulesDialog` 的父对象（Parent），这样当主程序或主对话框被销毁时，所有尚未关闭的配置子窗口能被 Qt 的父子对象树（Object Tree）安全、自动、一次性地完全释放销毁，提升应用的整体内敛防线。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **首选项与交互管理** | 配置文件的更改与应用在用户点击确认时必须具备即时事务性 | ✅（通过监听 `QDialog::accepted` 信号，在非模态场景下实现完美的事务一致性，完全合规） |
