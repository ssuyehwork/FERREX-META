# ScanDialog 主界面边缘拉伸与光标切换方案 —— Analysis_Modification_Plan-175.md

## 1. 任务背景
在当前 FERREX-META 客户端中，`ScanDialog` 是应用的主对话框。虽然基于无边框窗口的设计带来了极富质感的扁平视觉，但也面临无原生系统边框的局限。先前仅在主窗口的普通 `mouseMoveEvent` 中进行区域判定的设计存在重大缺陷：当鼠标移动至填充有布局控件（如顶部的搜索框、盘符按钮列表、表格视图以及图片视图）的区域时，因为子控件拦截了底层的鼠标移动事件（MouseEvent），导致主窗口无法收到任何悬停信号，最终导致鼠标指针移经边缘时无法显示双向箭头光标（对应用户原话：“当鼠标移动到窗口边缘时也没有出现双向箭头”），同时用户也无法通过按住边缘拖拉的方式拉伸调整窗口的大小（对应用户原话：“导致无法通过拖拉方式调整窗口大小”）。

为了解决该高频交互痛点，本次方案完全参考并对齐 ArcMeta 客户端 `MainWindow` 中已经过工业级验证的成熟无锁边缘检测架构，设计一套针对 `ScanDialog` 主界面的、结合“应用级全局事件过滤器（`ResizeEventFilter`）”与“对话框交互状态机”的全新拉伸及光标切换机制。

## 2. 问题定位
* **事件冒泡被子控件提前阻断**：
  在 Qt 事件分发体系中，若子控件（如 `QLineEdit`、`QTableView` 等）处理了 `MouseMoveEvent`，该事件将默认不会冒泡回传给父级 `ScanDialog`。导致只有在最外侧无子控件附着的极小空白边缘（1-2像素）处才可能勉强触发主窗口的移动事件。
* **缺乏系统级全局拦截器**：
  若想打破子控件的事件垄断，最优雅且高效的做法是使用 `QCoreApplication::installEventFilter` 机制全局挂载专门的边缘感应过滤器。无论当前鼠标在哪一个子部件上方，过滤器均能在事件派发前提前将其拦截，将其物理坐标换算回主窗口 `ScanDialog` 坐标空间，并据此强制修改 `ScanDialog` 的鼠标光标样式。
* **缺乏 `ScanDialog` 级的拉伸状态机与计算流**：
  在主界面 `ScanDialog` 的鼠标事件虚函数中，目前缺乏对拉伸模式（`m_isResizing`）的判定，且未重写 `mousePressEvent`、`mouseMoveEvent`、`mouseReleaseEvent` 等核心拖拽交互流，导致无法实现尺寸更新。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 当鼠标移动到窗口边缘时也没有出现双向箭头 | 实现应用级全局拦截过滤器 `ResizeEventFilter`，并安装至 `QCoreApplication`（对应用户原话：“当鼠标移动到窗口边缘时”），实现 100% 灵敏的、穿透子控件的鼠标双向箭头指针切换效果（对应用户原话：“出现双向箭头”） | ✅       |
| 2    | 导致无法通过拖拉方式调整窗口大小 | 在 `ScanDialog` 中重写三大鼠标交互事件，缓存拖动起始状态，并在 `mouseMoveEvent` 中根据鼠标偏移量（对应用户原话：“通过拖拉方式”）实时调用 `setGeometry` 动态重组包围盒尺寸（对应用户原话：“调整窗口大小”） | ✅       |

## 4. 详细解决方案

### 4.1 全局应用级事件过滤器（`ResizeEventFilter`）设计
在 `src/ui/` 目录下创建 `ResizeEventFilter.h` 与 `ResizeEventFilter.cpp`。

#### A. 接口声明：`src/ui/ResizeEventFilter.h`
```cpp
#pragma once

#include <QObject>
#include <QEvent>

namespace FERREX {

class ScanDialog;

/**
 * @brief 边缘缩放事件过滤器 (完美复刻自 ArcMeta 工业级事件层)
 * 通过全局安装拦截器，解决子控件遮挡导致无法更新双向光标的问题
 */
class ResizeEventFilter : public QObject {
    Q_OBJECT

public:
    explicit ResizeEventFilter(ScanDialog* window);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ScanDialog* m_window;
};

} // namespace FERREX
```

#### B. 核心实现：`src/ui/ResizeEventFilter.cpp`
```cpp
#include "ResizeEventFilter.h"
#include "ScanDialog.h"
#include <QMouseEvent>

namespace FERREX {

ResizeEventFilter::ResizeEventFilter(ScanDialog* window) 
    : QObject(window), m_window(window) {}

bool ResizeEventFilter::eventFilter(QObject* watched, QEvent* event) {
    // 1. 若主对话框已被最大化，则退化关闭边缘热区判定
    if (m_window->isMaximized()) {
        return QObject::eventFilter(watched, event);
    }

    // 2. 捕捉未按下鼠标时的普通悬停移动 (QEvent::MouseMove)
    if (event->type() == QEvent::MouseMove) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        
        // 3. 将全局坐标映射回 ScanDialog 的局部坐标系中
        QPoint localPos = m_window->mapFromGlobal(me->globalPosition().toPoint());
        
        // 4. 计算边缘拉伸方向，并直接调用主窗体方法更新光标
        ScanDialog::ResizeDirection dir = m_window->getResizeDirection(localPos);
        m_window->updateCursorShape(dir);
    } 
    // 5. 捕捉鼠标离开主窗口的事件，强制复位光标
    else if (event->type() == QEvent::Leave && watched == m_window) {
        m_window->setCursor(Qt::ArrowCursor);
    }

    return QObject::eventFilter(watched, event);
}

} // namespace FERREX
```

---

### 4.2 `ScanDialog` 类核心声明改造 (`src/ui/ScanDialog.h`)
需要在 `ScanDialog` 类的私有与保护成员区域，增加对应的事件监听声明与状态变量。

```cpp
<<<<<<< SEARCH
protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
=======
public:
    // 定义物理拉伸的 8 方向枚举与空状态 (对标 ArcMeta 规范)
    enum ResizeDirection {
        None = 0,
        Left, Right, Top, Bottom,
        TopLeft, TopRight, BottomLeft, BottomRight
    };

    // 公开暴露供事件过滤器调用
    ResizeDirection getResizeDirection(const QPoint& localPos) const;
    void updateCursorShape(ResizeDirection dir);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    // 重写三大鼠标底层事件，承接窗口边缘拉伸拖拽逻辑
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // 拉伸状态机核心变量
    ResizeDirection m_resizeDir = None;
    bool m_isResizing = false;
    bool m_isDragging = false;             // 控制标题栏拖拽状态
    QPoint m_resizeStartGlobal;
    QRect  m_resizeStartGeometry;
    QPoint m_dragPosition;                 // 标题栏拖动起始差值

    static constexpr int kResizeMargin = 6; // DPI 基准热区像素宽度

    class ResizeEventFilter* m_resizeFilter = nullptr; // 全局拦截事件过滤器

    void setupUi();
>>>>>>> REPLACE
```

---

### 4.3 `ScanDialog` 鼠标拉伸状态机与拉伸流实现 (`src/ui/ScanDialog.cpp`)

#### A. 构造函数初始化与全局过滤器安装
在 `ScanDialog` 构造函数中实例化并全局注册 `ResizeEventFilter`：
```cpp
// 2026-07-10 参考 ArcMeta 重构：在 UI 构造的最前期创建并注册全局事件过滤器
m_resizeFilter = new ResizeEventFilter(this);
QCoreApplication::instance()->installEventFilter(m_resizeFilter);
```

#### B. DPI 自适应判定与光标形状更新
```cpp
// 2026-07-10 物理移植自 ArcMeta：DPI 自适应感应宽度检测
ScanDialog::ResizeDirection ScanDialog::getResizeDirection(const QPoint& pos) const {
    int m = kResizeMargin;
    if (windowHandle()) {
        m = qRound(screen()->logicalDotsPerInch() / 96.0 * (double)kResizeMargin);
    }
    const int w = width(), h = height();
    bool left   = pos.x() < m;
    bool right  = pos.x() > w - m;
    bool top    = pos.y() < m;
    bool bottom = pos.y() > h - m;

    if (top    && left)  return TopLeft;
    if (top    && right) return TopRight;
    if (bottom && left)  return BottomLeft;
    if (bottom && right) return BottomRight;
    if (left)   return Left;
    if (right)  return Right;
    if (top)    return Top;
    if (bottom) return Bottom;
    return None;
}

// 2026-07-10 物理移植自 ArcMeta：实时光标形状更新
void ScanDialog::updateCursorShape(ResizeDirection dir) {
    switch (dir) {
        case Left:        case Right:       setCursor(Qt::SizeHorCursor);  break;
        case Top:         case Bottom:      setCursor(Qt::SizeVerCursor);  break;
        case TopLeft:     case BottomRight: setCursor(Qt::SizeFDiagCursor); break;
        case TopRight:    case BottomLeft:  setCursor(Qt::SizeBDiagCursor); break;
        default:                            setCursor(Qt::ArrowCursor);    break;
    }
}
```

#### C. 三大鼠标事件流完美集成
```cpp
// 1. 鼠标按下事件：锁定边缘区域拉伸模式，或退化至标题栏拖拽
void ScanDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;

    const QPoint localPos = event->position().toPoint();
    ResizeDirection dir = getResizeDirection(localPos);

    if (dir != None) {
        m_isResizing = true;
        m_isDragging = false;
        m_resizeDir = dir;
        m_resizeStartGlobal   = event->globalPosition().toPoint();
        m_resizeStartGeometry = geometry();
        event->accept();
        return;
    }

    // 拖动逻辑：判断是否在标题栏区域（高度小于等于 34px）
    if (localPos.y() <= 34) {
        m_isDragging = true;
        m_dragPosition = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
    }
}

// 2. 鼠标移动事件：执行实时多维度高阶包围盒计算与位置调整
void ScanDialog::mouseMoveEvent(QMouseEvent* event) {
    // A. 正在执行边缘拉伸
    if (m_isResizing) {
        const QPoint delta = event->globalPosition().toPoint() - m_resizeStartGlobal;
        QRect r = m_resizeStartGeometry;

        if (m_resizeDir == Left || m_resizeDir == TopLeft || m_resizeDir == BottomLeft)
            r.setLeft(r.left() + delta.x());
        if (m_resizeDir == Right || m_resizeDir == TopRight || m_resizeDir == BottomRight)
            r.setRight(r.right() + delta.x());
        if (m_resizeDir == Top || m_resizeDir == TopLeft || m_resizeDir == TopRight)
            r.setTop(r.top() + delta.y());
        if (m_resizeDir == Bottom || m_resizeDir == BottomLeft || m_resizeDir == BottomRight)
            r.setBottom(r.bottom() + delta.y());

        // 严格遵循窗口自身的最小尺寸边界
        if (r.width() >= minimumWidth() && r.height() >= minimumHeight()) {
            setGeometry(r);
        }

        event->accept();
        return;
    }

    // B. 正在执行标题栏移动窗口
    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragPosition);
        event->accept();
        return;
    }

    // C. 无操作悬停：由事件过滤器提供持续、平滑的光标联动更新
    if (!m_isDragging) {
        updateCursorShape(getResizeDirection(event->position().toPoint()));
    }
}

// 3. 鼠标释放事件：清空所有状态机变量，还原光标
void ScanDialog::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    m_isDragging  = false;
    m_isResizing  = false;
    m_resizeDir   = None;
    setCursor(Qt::ArrowCursor);
}
```

#### D. 析构函数中彻底注销全局事件过滤器
为避免窗口销毁后全局事件过滤器悬空产生未定义崩溃：
```cpp
ScanDialog::~ScanDialog() {
    if (m_resizeFilter) {
        QCoreApplication::instance()->removeEventFilter(m_resizeFilter);
    }
}
```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ResizeEventFilter.h` 和 `src/ui/ResizeEventFilter.cpp`（物理新增，全局注入并捕获穿透事件）
- [ ] 模块/文件：`src/ui/ScanDialog.h` （引入拉伸状态声明、边缘判定及三大底层事件声明）
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` （实现全局注册过滤、DPI 自适应判定及鼠标拉伸状态机逻辑）

**明确禁止越界修改的范围：**
- [ ] 明确禁止破坏 `ScanDialog` 的布局完整性，禁止为了边缘检测而修改 `JustifiedView` 或 `ThumbnailDelegate` 等视图内部绘制及其他 UI 层物理事件。

## 6. 实现准则与预警【核心】
1. **DPI 缩放下的判定精度保障**：
   在 4K 屏幕或 150%+ 的 Windows 系统高分屏 DPI 下，原本硬编码的 6 像素边缘在视觉和物理坐标上可能会急剧缩小。因此，必须像 ArcMeta 一样，通过 `logicalDotsPerInch() / 96.0` 的比例对感应边框宽度进行等比缩放计算，从而保证无论在任何分辨率及显示缩放倍率下，用户都能获得极其平滑且精准的手感。
2. **全局过滤器生命周期预警**：
   `ResizeEventFilter` 被全局安装到了 `QCoreApplication`。在 `ScanDialog` 被完全析构关闭时，必须在析构函数中**显式调用 `removeEventFilter`** 将其安全移除，以彻底防止后续全局事件循环派发到已悬空的死对象引发内存溢出崩溃。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **呼吸窗口/耗时操作** | 数据库等重型耗时操作需释放 CPU 锁防界面卡死 | **不涉及**（本方案完全运行于 UI 线程中的底层事件循环和微秒级局部算术运算，极速返回，零线程阻塞风险） |
| **标题栏按钮/UI尺寸** | 标题栏及关键 UI 组件对标已有尺寸规范 | ✅（完美对齐 ArcMeta 客户端的 `MainWindow` 工业级拉伸标准规范与 DPI 自适应算法） |
| **极致性能** | 零分配、避免在运行循环中产生多余堆分配 | ✅（无动态堆内存分配、无字符串转换，纯布尔及 QPoint 几何四则运算，无任何性能损耗） |
