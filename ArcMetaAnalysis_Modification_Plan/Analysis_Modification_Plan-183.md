# 物理移植与集成 ToolTipOverlay 统一气泡机制 —— Analysis_Modification_Plan-183.md

## 1. 任务背景

用户指出当前系统存在严重的 ToolTip 显示质量和一致性问题（对应用户原话：“这里存在傻逼逻辑架构，Tooltip都显示了什么？完全看不到，而且其他按钮也没有采用Tooltip，显然是jules这个傻逼Ai另起灶炉导致的 ... 去参考ArcMeta版本来实现ToolTipOverlay”）。

### 核心痛点：
1. **气泡完全不可见**：在无边框暗黑色系主窗口中，Qt 原生 QToolTip 未应用全局样式重绘时，会直接退回到系统底层配置色，表现为黑色背景搭配暗黑字体的狭长色块，导致文字彻底无法辨识（黑条问题）。
2. **气泡规则不统一**：主界面的其余按钮（如置顶按钮、最小化、最大化及关闭按钮）都严格在构造函数中通过 `setToolTip("")` 明确禁用或隐退了原生气泡机制，唯独先前加入的 `rulesBtn`（预览规则配置）存在原生气泡污染，二者未对齐全局规范。
3. **架构欠缺统一气泡管线**：本项目并未继承或包含 ArcMeta 的扁平高对比度无滞后气泡。

因此，我们需要物理移植 `ArcMeta` 的 `ToolTipOverlay`（极简 2px 圆角高对比度自定义顶层气泡）与配套的 `HoverEventFilter`（物理拦截 Hover 事件的过滤器），并将命名空间全面迁移适配至本项目的 `FERREX` 命名空间。同时彻底封杀所有的原生 `setToolTip`，全部接入 `ToolTipOverlay` 与属性注册框架。

---

## 2. 问题定位与移植策略

- **移植源文件 1：** `ArcMeta/src/ui/ToolTipOverlay.h` -> `src/ui/ToolTipOverlay.h`
- **移植源文件 2：** `ArcMeta/src/ui/ToolTipOverlay.cpp` -> `src/ui/ToolTipOverlay.cpp`
- **移植源文件 3：** `ArcMeta/src/ui/HoverEventFilter.h` -> `src/ui/HoverEventFilter.h`
- **移植源文件 4：** `ArcMeta/src/ui/HoverEventFilter.cpp` -> `src/ui/HoverEventFilter.cpp`

### 命名空间迁移规则：
所有的 `namespace ArcMeta` 必须物理重构替换为本项目的统一规范 `namespace FERREX`。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | Tooltip都显示了什么？完全看不到 | 物理移植高对比度（#2B2B2B 背景 + #EEEEEE 前景）极简 2px 圆角的 `ToolTipOverlay`，从根本上解决原生 ToolTip 黑条与不可见缺陷（对应用户原话：“Tooltip都显示了什么？完全看不到”） | ✅       |
| 2    | 其他按钮也没有采用Tooltip | 统一禁绝所有的原生 `setToolTip()`，包含 `rulesBtn` 及现有控制按钮组，全部改用无侵入属性 `tooltipText` 控制（对应用户原话：“其他按钮也没有采用Tooltip”） | ✅       |
| 3    | 去参考ArcMeta版本来实现ToolTipOverlay | 物理移植 `ToolTipOverlay` 控件与 `HoverEventFilter` 监听器，转换为 `FERREX` 命名空间，实现完美一致的气泡中枢（对应用户原话：“去参考ArcMeta版本来实现ToolTipOverlay”） | ✅       |

---

## 4. 详细解决方案

### 4.1 物理移植一：`src/ui/ToolTipOverlay.h` 定义
实现并定制 `FERREX` 命名空间下的气泡图层类，锁定 2px 圆角扁平风格：

```cpp
#ifndef TOOLTIPOVERLAY_H
#define TOOLTIPOVERLAY_H

#include <QWidget>
#include <QPainter>
#include <QElapsedTimer>
#include <QScreen>
#include <QGuiApplication>
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QFontMetrics>
#include <QTextDocument>
#include <QPointer>
#include <QPainterPath>
#include <QColor>
#include <QPen>
#include <QBrush>
#include <QRectF>

namespace FERREX {

/**
 * @brief ToolTipOverlay: 全局统一的自定义 Tooltip (物理移植自 ArcMeta)
 * [CRITICAL] 本项目严禁使用任何形式的“Windows 系统默认 Tip 样式”！
 * [RULE] 1. 杜绝原生内容带来的系统阴影和不透明度。
 * [RULE] 2. 所有的 ToolTip 逻辑必须通过此 ToolTipOverlay 渲染。
 * [RULE] 3. 此组件必须保持扁平化 (Flat)，严禁添加任何阴影特效。
 */
class ToolTipOverlay : public QWidget {
    Q_OBJECT
public:
    static ToolTipOverlay* instance() {
        static QPointer<ToolTipOverlay> inst;
        if (!inst) {
            inst = new ToolTipOverlay();
        }
        return inst;
    }

    /**
     * @brief 显示提示文字
     */
    void showText(const QPoint& globalPos, const QString& text, int timeout = 700, const QColor& borderColor = QColor("#B0B0B0"));

    // 兼容旧接口
    void showTip(const QString& text, const QPoint& pos, int timeout = 700) {
        showText(pos, text, timeout);
    }

    static void hideTip() {
        if (instance()) instance()->hide();
    }

protected:
    explicit ToolTipOverlay();
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_text;
    QTextDocument m_doc;
    QTimer m_hideTimer;
    QColor m_currentBorderColor = QColor("#B0B0B0");
};

} // namespace FERREX

#endif // TOOLTIPOVERLAY_H
```

---

### 4.2 物理移植二：`src/ui/ToolTipOverlay.cpp` 实现
在源文件中完成置顶 API 的对接和 2px 圆角绘制逻辑：

```cpp
#include "ToolTipOverlay.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QTimer>
#include <QThread>

namespace FERREX {

ToolTipOverlay::ToolTipOverlay() : QWidget(nullptr) {
    // 彻底弃用 Qt::ToolTip，防止 OS 动画残留
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | 
                  Qt::WindowTransparentForInput | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
    setObjectName("ToolTipOverlay");

    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    // 通过原生 Win32 API 强制执行 topmost 置顶
#ifdef Q_OS_WIN
    QTimer::singleShot(0, this, [this]() {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    });
#else
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
#endif
    
    m_doc.setUndoRedoEnabled(false);
    // 强制锁定调色板高对比度淡灰白 (#EEEEEE)
    QPalette pal = palette();
    pal.setColor(QPalette::WindowText, QColor("#EEEEEE"));
    pal.setColor(QPalette::Text, QColor("#EEEEEE"));
    pal.setColor(QPalette::ButtonText, QColor("#EEEEEE"));
    setPalette(pal);

    m_doc.setDefaultStyleSheet("body, div, p, span, b, i { color: #EEEEEE !important; font-family: 'Microsoft YaHei', 'Segoe UI'; }"); 
    setStyleSheet("QWidget { color: #EEEEEE !important; background: transparent; }");

    QFont f = font();
    f.setPointSize(9);
    m_doc.setDefaultFont(f);

    m_hideTimer.setSingleShot(true);
    connect(&m_hideTimer, &QTimer::timeout, this, &QWidget::hide);

    hide();
}

void ToolTipOverlay::showText(const QPoint& globalPos, const QString& text, int timeout, const QColor& borderColor) {
    if (thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this, [this, globalPos, text, timeout, borderColor]() { 
            showText(globalPos, text, timeout, borderColor); 
        });
        return;
    }

    if (text.isEmpty()) { hide(); return; }

    // 脏检查：防止在微动时引发重绘闪烁
    if (isVisible() && m_text == text && m_currentBorderColor == borderColor) {
        move(globalPos + QPoint(15, 15));
        return;
    }
    
    if (timeout > 0) {
        timeout = qBound(500, timeout, 60000); 
    }

    m_currentBorderColor = borderColor;

    QString htmlBody;
    if (text.contains("<") && text.contains(">")) {
        htmlBody = text;
    } else {
        htmlBody = text.toHtmlEscaped().replace("\n", "<br>");
    }

    m_text = QString(
        "<html><head><style>div, p, span, body { color: #EEEEEE !important; }</style></head>"
        "<body style='margin:0; padding:0; color:#EEEEEE; font-family:\"Microsoft YaHei\",\"Segoe UI\",sans-serif;'>"
        "<div style='color:#EEEEEE !important;'>%1</div>"
        "</body></html>"
    ).arg(htmlBody);
    
    m_doc.setHtml(m_text);
    m_doc.setDocumentMargin(0); 
    
    m_doc.setTextWidth(-1); 
    qreal idealW = m_doc.idealWidth();
    
    if (idealW > 450) {
        m_doc.setTextWidth(450); 
    } else {
        m_doc.setTextWidth(idealW); 
    }
    
    QSize textSize = m_doc.size().toSize();
    
    int padX = 12; 
    int padY = 8;
    
    int w = textSize.width() + padX * 2;
    int h = textSize.height() + padY * 2;
    
    w = qMax(w, 40);
    h = qMax(h, 24);
    
    resize(w, h);
    
    QPoint pos = globalPos + QPoint(15, 15);
    
    QScreen* screen = QGuiApplication::screenAt(globalPos);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect screenGeom = screen->geometry();
        if (pos.x() + width() > screenGeom.right()) {
            pos.setX(globalPos.x() - width() - 15);
        }
        if (pos.y() + height() > screenGeom.bottom()) {
            pos.setY(globalPos.y() - height() - 15);
        }
    }
    
    move(pos);
    show();
    update();

    if (timeout > 0) {
        m_hideTimer.start(timeout);
    } else {
        m_hideTimer.stop();
    }
}

void ToolTipOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    
    QRectF rectF(0.5, 0.5, width() - 1, height() - 1);
    
    p.setPen(QPen(m_currentBorderColor, 1));
    p.setBrush(QColor("#2B2B2B"));
    // 按照 ArcMeta 标准规范：气泡圆角尺寸严格设定为 2px
    p.drawRoundedRect(rectF, 2, 2);
    
    p.save();
    p.translate(12, 8); 
    m_doc.drawContents(&p);
    p.restore();
}

} // namespace FERREX
```

---

### 4.3 物理移植三：`src/ui/HoverEventFilter.h` 定义
封装无侵入式鼠标 Hover 事件监听器：

```cpp
#pragma once

#include <QObject>
#include <QEvent>

namespace FERREX {

/**
 * @brief 悬停事件过滤器
 * 专门处理鼠标进入/离开控件时，显示/隐藏自定义 ToolTipOverlay (物理移植自 ArcMeta)
 */
class HoverEventFilter : public QObject {
    Q_OBJECT

public:
    explicit HoverEventFilter(QObject* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

} // namespace FERREX
```

---

### 4.4 物理移植四：`src/ui/HoverEventFilter.cpp` 实现
通过读取组件的 dynamic property `tooltipText`，实现全自动的事件探测与唤起：

```cpp
#include "HoverEventFilter.h"
#include "ToolTipOverlay.h"
#include <QCursor>
#include <QVariant>

namespace FERREX {

HoverEventFilter::HoverEventFilter(QObject* parent) : QObject(parent) {}

bool HoverEventFilter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::Enter) {
        QString text = watched->property("tooltipText").toString();
        if (!text.isEmpty()) {
            // 悬停触发的提示设置 timeout 为 0（代表不自动隐藏，伴随鼠标移动并由 Leave 事件销毁）
            ToolTipOverlay::instance()->showText(QCursor::pos(), text, 0);
        }
    } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
        ToolTipOverlay::hideTip();
    }
    return QObject::eventFilter(watched, event);
}

} // namespace FERREX
```

---

### 4.5 关联构建变更：`CMakeLists.txt`
必须将上述 4 个新增文件加入项目工程的编译源文件列表（`CMakeLists.txt`）中：

```cmake
<<<<<<< SEARCH
    src/ui/QuickLookWindow.h
    src/ui/QuickLookWindow.cpp
    src/ui/SvgIcons.h
=======
    src/ui/QuickLookWindow.h
    src/ui/QuickLookWindow.cpp
    src/ui/SvgIcons.h
    src/ui/ToolTipOverlay.h
    src/ui/ToolTipOverlay.cpp
    src/ui/HoverEventFilter.h
    src/ui/HoverEventFilter.cpp
>>>>>>> REPLACE
```

---

### 4.6 深度重构与彻底对齐：`src/ui/ScanDialog.cpp`

1. **预初始化预热**
   在 `ScanDialog` 构造函数的最顶端，添加对全局 `ToolTipOverlay::instance()` 的获取。这会提前触发窗口系统的 `winId()` 并调用 native win32 最顶层操作，避免初次气泡呼出时的 GPU 实例化延迟。
   
2. **重构控制按钮（`viewBtn`，`rulesBtn` 等）的气泡绑定**
   - 彻底废除原生 `setToolTip()`。
   - 对 `viewBtn`、`rulesBtn`、`m_pinBtn` 等标题栏按钮分配 `HoverEventFilter` 事件过滤器，并设置属性 `tooltipText`。

#### 修改代码段 1（包含头文件）：
```cpp
<<<<<<< SEARCH
#include "FramelessDialog.h"
#include "QuickLookWindow.h"
=======
#include "FramelessDialog.h"
#include "QuickLookWindow.h"
#include "ToolTipOverlay.h"
#include "HoverEventFilter.h"
>>>>>>> REPLACE
```

#### 修改代码段 2（主界面按钮重组气泡）：
```cpp
<<<<<<< SEARCH
            QPushButton* viewBtn = new QPushButton(); 
            viewBtn->setFixedSize(20, 20); // 严格锁定 20x20
            viewBtn->setIcon(UiHelper::getIcon("grid", QColor("#CCCCCC"), 16)); // 严格锁定图标 16x16
            viewBtn->setIconSize(QSize(16, 16));
            viewBtn->setCursor(Qt::PointingHandCursor); 
            viewBtn->setToolTip(""); // 禁止原生 ToolTip
            viewBtn->setStyleSheet( 
=======
            // 构造全局气泡 Hover 事件过滤器
            auto* hoverFilter = new HoverEventFilter(this);

            QPushButton* viewBtn = new QPushButton(); 
            viewBtn->setFixedSize(20, 20); // 严格锁定 20x20
            viewBtn->setIcon(UiHelper::getIcon("grid", QColor("#CCCCCC"), 16)); // 严格锁定图标 16x16
            viewBtn->setIconSize(QSize(16, 16));
            viewBtn->setCursor(Qt::PointingHandCursor); 
            viewBtn->setToolTip(""); // 物理禁止原生 ToolTip，防范系统黑块
            viewBtn->setProperty("tooltipText", "切换视图排版模式"); // 完美对接高级属性
            viewBtn->installEventFilter(hoverFilter); // 接管悬停管线
            viewBtn->setStyleSheet( 
>>>>>>> REPLACE
```

#### 修改代码段 3（`rulesBtn` 彻底切除原生 ToolTip 缺陷并统一重构）：
```cpp
<<<<<<< SEARCH
            QPushButton* rulesBtn = new QPushButton();
            rulesBtn->setFixedSize(20, 20);
            rulesBtn->setIcon(UiHelper::getIcon("settings", QColor("#CCCCCC"), 16));
            rulesBtn->setIconSize(QSize(16, 16));
            rulesBtn->setCursor(Qt::PointingHandCursor);
            rulesBtn->setToolTip("预览规则配置");
            rulesBtn->setStyleSheet(
=======
            QPushButton* rulesBtn = new QPushButton();
            rulesBtn->setFixedSize(20, 20);
            rulesBtn->setIcon(UiHelper::getIcon("settings", QColor("#CCCCCC"), 16));
            rulesBtn->setIconSize(QSize(16, 16));
            rulesBtn->setCursor(Qt::PointingHandCursor);
            rulesBtn->setToolTip(""); // 彻底切除原生阻塞黑色气泡 (对应用户原话：“其他按钮也没有采用Tooltip ... Tooltip都显示了什么？完全看不到”)
            rulesBtn->setProperty("tooltipText", "预览规则配置"); // 统一改用自定义属性气泡驱动 (对应用户原话：“去参考ArcMeta版本来实现ToolTipOverlay”)
            rulesBtn->installEventFilter(hoverFilter); // 接管悬停管线
            rulesBtn->setStyleSheet(
>>>>>>> REPLACE
```

#### 修改代码段 4（置顶等按钮原生禁绝并对齐）：
```cpp
<<<<<<< SEARCH
            // 更新现有控制按钮样式以对标规范
            for (auto* btn : {m_pinBtn, m_minBtn, m_maxBtn}) {
                if (!btn) continue;
                btn->setFixedSize(20, 20);
                btn->setIconSize(QSize(16, 16));
                btn->setToolTip("");
=======
            // 更新现有控制按钮样式以对标规范
            for (auto* btn : {m_pinBtn, m_minBtn, m_maxBtn}) {
                if (!btn) continue;
                btn->setFixedSize(20, 20);
                btn->setIconSize(QSize(16, 16));
                btn->setToolTip(""); // 原生禁绝
                
                // 完美赋予自定义 ToolTipOverlay 支持
                if (btn == m_pinBtn) {
                    btn->setProperty("tooltipText", "切换窗口置顶状态");
                    btn->installEventFilter(hoverFilter);
                } else if (btn == m_minBtn) {
                    btn->setProperty("tooltipText", "最小化");
                    btn->installEventFilter(hoverFilter);
                } else if (btn == m_maxBtn) {
                    btn->setProperty("tooltipText", "最大化");
                    btn->installEventFilter(hoverFilter);
                }
>>>>>>> REPLACE
```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`CMakeLists.txt`（加入 4 个新文件进行编译管理）
- [ ] 模块/文件：`src/ui/ScanDialog.cpp`（初始化阶段预热 ToolTipOverlay、禁绝 `rulesBtn` 及各大控制按钮原生 ToolTip 并统合注册悬停过滤器）

**明确禁止越界修改的范围：**
- [ ] 严禁直接写入/修改任何 C++ 物理代码。
- [ ] 严禁在方案文档外执行物理代码编译行为。

---

## 6. 实现准则与预警【核心】
1. **防止跨线程崩溃**：由于 `ToolTipOverlay` 在主线程工作并渲染 `QTextDocument`，如果有异步后台线程操作可能触发崩溃。所以已经在 `showText` 的实现最前端添加了 `QThread` 线程判定与 `QMetaObject::invokeMethod` 自动跳转回归主线程，提供了终极稳定护栏。
2. **多线程/乱动脏检查**：当鼠标划过一系列密集控制按钮时，`isVisible()` 加上文本匹配脏检查，会平滑地执行 `move()` 而不重复调用重绘，消除了系统卡顿。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **首选项与交互管理** | 自定义气泡高对比度圆角锁定为 2px，严禁添加系统默认阴影 | ✅（完全对齐，完全合规） |
