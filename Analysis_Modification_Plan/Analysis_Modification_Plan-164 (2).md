# QuickLook 预览文本时空格键拦截与关闭失效问题修复 —— Analysis_Modification_Plan-164.md

## 1. 任务背景
在 FERREX 搜索对话框或主程序中，按 `空格键`（Space）可以快速唤起和关闭文件预览窗口 `QuickLookWindow`。然而，当预览的对象是文本文件时，该窗口使用只读的文本框组件 `m_textEdit` (`QPlainTextEdit`) 渲染内容。当该控件由于被激活而获取到焦点后，如果用户再次按下 `空格键` 意图关闭预览，此按键事件会被 `QPlainTextEdit` 自身的原生行为拦截消耗（触发向上或向下翻滚逻辑），阻止了事件向上冒泡传递给 `QuickLookWindow::keyPressEvent`，从而导致预览窗口无法被正常关闭。

为了彻底纠正此逻辑偏差，本方案拟在文本框控件上安装主窗口事件过滤器，并在事件分发早期阶段强行拦截并拦截空格键，以此实现精准的、任何媒体类型下一致的“空格键一键开启与关闭”交互设计。

## 2. 问题定位
* **焦点阻断源**：`src/ui/QuickLookWindow.cpp` 中的 `m_textEdit` 继承自 `QPlainTextEdit`。当用户点击或聚焦至只读文本区域时，键盘输入流会首先经过该控件的 `keyPressEvent`。
* **按键吞噬**：`QPlainTextEdit` 原生硬编码了对 `Qt::Key_Space` 的消费（用作翻页），没有将其 `ignore()`，故无法冒泡给父窗口 `QuickLookWindow`。
* **过滤机制缺失**：目前的 `m_textEdit` 没有安装任何事件过滤器来将这一特权按键（空格键）上交/移交给父级窗口做状态反转。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 当按下空格键预览的是文本文件时，打开预览界面之后，再次按下空格键却没有关闭预览界面，反而执行了原生的向下翻滚逻辑 | 在事件过滤器中，对文本框控件的按键输入进行过滤，一旦拦截到 `Qt::Key_Space`，立刻调用 `closePreview()` 并返回 `true` 拦截，杜绝向下翻页原生逻辑。 | ✅ |
| 2    | 我期望的是按下空格键只可以打开 / 关闭预览界面 | 使得无论在任何媒体展示状态下（即使文本获得焦点），空格键都恒定且只用于打开或反转关闭预览。 | ✅ |

## 4. 详细解决方案

### 第一步：在 `src/ui/QuickLookWindow.cpp` 构造函数中为 `m_textEdit` 安装事件过滤器
我们找到 `m_textEdit` 创建的位置（约第 229 行），在其被加入布局后，显式安装 `QuickLookWindow` 作为其事件过滤器。

**修正对比片段：**
```cpp
// src/ui/QuickLookWindow.cpp

<<<<<<< SEARCH
    // 文本渲染控件
    m_textEdit = new QPlainTextEdit();
    m_textEdit->setReadOnly(true);
    m_textEdit->hide();
    m_textEdit->verticalScrollBar()->setStyleSheet(R"(
        QScrollBar:vertical { width: 4px; background: transparent; }
        QScrollBar::handle:vertical { background: #444; border-radius: 2px; }
    )");
    layout->addWidget(m_textEdit);
=======
    // 文本渲染控件
    m_textEdit = new QPlainTextEdit();
    m_textEdit->setReadOnly(true);
    m_textEdit->hide();
    m_textEdit->verticalScrollBar()->setStyleSheet(R"(
        QScrollBar:vertical { width: 4px; background: transparent; }
        QScrollBar::handle:vertical { background: #444; border-radius: 2px; }
    )");
    m_textEdit->installEventFilter(this); // 2026-07-xx 交互优化：安装事件过滤器拦截空格键以防吞噬
    layout->addWidget(m_textEdit);
>>>>>>> REPLACE
```

### 第二步：在 `QuickLookWindow::eventFilter` 中拦截空格按键
重构 `eventFilter` 过滤函数。若被监视的对象是 `m_textEdit` 且触发了 `QEvent::KeyPress` 类型的 `Qt::Key_Space` 按键，直接调用窗口关闭逻辑 `closePreview()`，并返回 `true` 以防事件继续下发到 `QPlainTextEdit`。

**修正对比片段：**
```cpp
// src/ui/QuickLookWindow.cpp

<<<<<<< SEARCH
bool QuickLookWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::WindowDeactivate) {
        if (m_ignoreDeactivate) {
            return true;
        }
        closePreview();
    }
    return QWidget::eventFilter(watched, event);
}
=======
bool QuickLookWindow::eventFilter(QObject* watched, QEvent* event) {
    // 2026-07-xx 核心改进：当文本框组件获得焦点并按下空格键时，将其拦截，改为执行关闭预览逻辑
    if (watched == m_textEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space) {
            closePreview();
            return true; // 100% 拦截，阻止 QPlainTextEdit 响应并向下翻页
        }
    }

    if (event->type() == QEvent::WindowDeactivate) {
        if (m_ignoreDeactivate) {
            return true;
        }
        closePreview();
    }
    return QWidget::eventFilter(watched, event);
}
>>>>>>> REPLACE
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 源文件：`src/ui/QuickLookWindow.cpp`（安装过滤器并重写 `eventFilter` 拦截键流）

**明确禁止越界修改的范围：**
- [ ] 严禁修改 `QuickLookWindow::keyPressEvent` 原有的音视频播放按键（如 P 键）或其他方向键切图行为。
- [ ] 严禁在文本框过滤中拦截除空格键外的其它导航键（如 PageUp / PageDown），确保普通鼠标和键盘翻页体验原封不动。

## 6. 实现准则与预警【核心】
1. **中性无副作用**：通过 Qt 的 `eventFilter` 进行特定控件的早期按键截断，是 C++ GUI 编程中极其标准且安全的重写方式，能对原有的 QWidget 事件分发系统实现 100% 物理无害化拦截，绝不导致任何空指针或线程崩溃。
2. **多态兼容**：即使未来文本渲染控件由 `QPlainTextEdit` 升级为 `QTextBrowser` 或自定义 HTML 引擎，只需确保对其安装并响应此 `eventFilter` 规则即可，具备极佳的维护性。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| QuickLook 居中悬浮窗 | 支持居中、圆角 12px 及 1-5 键打分打色与 ToolTip 联动。 | ✅ 符合。本方案纯粹加固了空格键一键开启与关闭的反转动作流，完美兼容已有评级机制。 |

## 8. 待确认事项（可选）
* 无
