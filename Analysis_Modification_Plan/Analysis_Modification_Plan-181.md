# 下拉面板历史选项删除按钮重构 —— Analysis_Modification_Plan-181.md

## 1. 任务背景

在当前的 `FERREX-META` 系统中，用户在双击主界面的 `m_searchEdit`（文件名搜索框）或 `m_extEdit`（后缀名搜索框）时，系统会弹出一个展示最近搜索历史的历史记录“下来面板”（下拉菜单 QMenu）。
目前，该历史下来面板只支持一键“清空所有历史记录”，并不支持对特定单个历史选项的单独删除和定制（对应用户原话：“下来面板的每一个选项右侧都应该有一个“×”，这样的话用户就可以轻松移除某个选项了”）。

为了提高操作灵活性，满足用户对单项记录的快捷清除和极致人机交互需求，我们需要重构下拉历史面板，在每一个条目选项的最右侧加置一个可交互的“×”物理删除按钮。

---

## 2. 问题定位与原因剖析

### 2.1 历史下拉面板定位
在 `src/ui/ScanDialog.cpp` 的 `eventFilter` 虚函数中，关于双击输入框事件处理的分支如下：
```cpp
    if ((watched == m_searchEdit || watched == m_extEdit) && event->type() == QEvent::MouseButtonDblClick) {
        bool isQuery = (watched == m_searchEdit);
        const QStringList& history = isQuery ? m_config.queryHistory : m_config.extHistory;
        
        if (!history.isEmpty()) {
            QMenu menu(this);
            menu.setStyleSheet("QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; } QMenu::item:selected { background: #232D37; color: #FFF; }");
            
            for (const QString& item : history) {
                menu.addAction(item, [this, isQuery, item]() {
                    if (isQuery) m_searchEdit->setText(item);
                    else m_extEdit->setText(item);
                    onTriggerSearch();
                });
            }
            ...
```
### 2.2 核心限制与重构思路
原生的 `QMenu::addAction(text)` 只能创建一个标准的、不可在右端放置子组件的 `QAction` 项。为了在每一个选项的右侧（极右侧）成功插入一个用于轻松移除该项的“×”删除按钮：
1. 我们需要废除传统的 `menu.addAction(item, ...)`，改用 Qt 高级菜单自定义机制 **`QWidgetAction`**。
2. 通过实现一个专门用于填充该 Action 的轻量化容器组件 **`HistoryItemWidget`**。
3. `HistoryItemWidget` 在水平布局（`QHBoxLayout`）的两端分别放置：
   - 左侧为包含历史选项文本的文本点击按钮 `QPushButton`（或 QLabel，但使用扁平的 QPushButton 可以原生捕获悬停与点击事件，并响应选中后将值填充回搜索框并关闭菜单）。
   - 右侧为专享的物理删除按钮 `QPushButton`，按钮尺寸固定为 `18x18` 或 `16x16` 像素。
   - 删除按钮的图标使用高对比度的淡灰色“×”（Unicode 符号 `✕` 或字符 `×`），在其悬停时提供舒适的半透明微红背景反馈并切换为手型光标（`Qt::PointingHandCursor`）。
4. 当用户点击右侧“×”时，触发特定的局部槽函数，将该项文本从 `queryHistory` 或 `extHistory` 中移除，并触发存盘。随后关闭当前菜单，并重新展示最新列表状态，达到极致的顺滑度。

---

## 3. 详细解决方案

### 3.1 引入头文件依赖与防范说明
由于新增了 `QTimer`、`QWidgetAction` 和布局容器依赖，必须确保在 `src/ui/ScanDialog.cpp` 头文件包含区添加以下头文件：
```cpp
#include <QTimer>
#include <QWidgetAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
```

### 3.2 定义自定义下拉菜单项 QWidgetAction
在 `src/ui/ScanDialog.cpp` 的匿名命名空间中，或作为独立辅助小控件，引入 **`HistoryItemWidget`** 和自定义的 `QWidgetAction`。

这里直接声明一个轻量级的自定义控件，以便开箱即用：

```cpp
#include <QWidgetAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace FERREX {

// 1. 专门用于下来面板中展示历史选项和右侧删除按钮的自定义微控件
class HistoryItemWidget : public QWidget {
public:
    HistoryItemWidget(const QString& text, bool isQuery, QMenu* parentMenu, ScanDialog* dialog, QWidget* parent = nullptr)
        : QWidget(parent), m_text(text), m_isQuery(isQuery), m_parentMenu(parentMenu), m_dialog(dialog)
    {
        // 采用极致扁平与紧凑的布局结构
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 2, 8, 2);
        layout->setSpacing(8);

        // A. 左侧：条目文本按钮（扁平设计，点击后将内容填入主搜索框，并执行搜索）
        auto* btnText = new QPushButton(m_text, this);
        btnText->setCursor(Qt::PointingHandCursor);
        btnText->setStyleSheet(
            "QPushButton { background: transparent; border: none; color: #CCCCCC; text-align: left; font-size: 12px; padding: 4px 0; } "
            "QPushButton:hover { color: #FFFFFF; }"
        );
        connect(btnText, &QPushButton::clicked, this, &HistoryItemWidget::onSelectTriggered);
        layout->addWidget(btnText, 1); // 占据所有的剩余伸缩空间

        // B. 右侧：精致的 “×” 单项删除按钮 (对应用户原话：“下来面板的每一个选项右侧都应该有一个“×”，这样的话用户就可以轻松移除某个选项了”)
        auto* btnDelete = new QPushButton("×", this);
        btnDelete->setFixedSize(18, 18);
        btnDelete->setCursor(Qt::PointingHandCursor);
        btnDelete->setToolTip("移除该历史记录");
        btnDelete->setStyleSheet(
            "QPushButton { background: transparent; border: none; color: #888888; font-size: 14px; font-weight: bold; border-radius: 3px; line-height: 18px; } "
            "QPushButton:hover { color: #FFFFFF; background-color: #E81123; }" // 悬停显红
        );
        connect(btnDelete, &QPushButton::clicked, this, &HistoryItemWidget::onDeleteTriggered);
        layout->addWidget(btnDelete);
    }

private:
    void onSelectTriggered() {
        if (m_dialog) {
            m_dialog->setHistoryText(m_text, m_isQuery);
        }
        if (m_parentMenu) {
            m_parentMenu->close(); // 选中后关闭下拉菜单
        }
    }

    void onDeleteTriggered() {
        // 安全机制防范：为防止 m_parentMenu->close() 导致 QMenu 栈对象销毁并级联释放本 HistoryItemWidget 导致 Use-After-Free (UAF) 崩溃，
        // 在触发任何导致菜单退出的动作前，必须首先在当前栈上完全拷贝所需的成员状态，严禁在 menu 闭合后访问 `this` 或任何成员变量。
        bool isQuery = m_isQuery;
        QString text = m_text;
        ScanDialog* dialog = m_dialog;
        QMenu* parentMenu = m_parentMenu;

        if (dialog) {
            dialog->removeHistoryItem(text, isQuery);
        }
        
        if (parentMenu) {
            parentMenu->close(); // 删除后立即关闭旧菜单，外界会重新唤醒最新菜单以完成无缝重绘
            
            // 使用完全安全的栈备份变量，在 QTimer 异步事件中安全执行重新唤醒
            if (dialog) {
                QTimer::singleShot(50, dialog, [dialog, isQuery]() {
                    dialog->reopenHistoryMenu(isQuery);
                });
            }
        }
    }

    QString m_text;
    bool m_isQuery;
    QMenu* m_parentMenu;
    ScanDialog* m_dialog;
};

} // namespace FERREX
```

### 3.2 在 `ScanDialog` 中支持历史纪录操作辅助虚函数
为了配合 `HistoryItemWidget` 执行状态修改与重绘，在 `ScanDialog` 类中追加三个轻量级公共辅助方法（在 `src/ui/ScanDialog.h` 中声明，在 `src/ui/ScanDialog.cpp` 中实现）：

#### A. 在 `src/ui/ScanDialog.h` 中追加声明：
```cpp
<<<<<<< SEARCH
    void onTriggerSearch();
=======
    void onTriggerSearch();
    
    // 2026-07-11 下拉面板历史单项“×”删除辅助操作链 (对应用户原话：“每个选项右侧都应该有一个“×”……轻松移除某个选项”)
    void setHistoryText(const QString& text, bool isQuery);
    void removeHistoryItem(const QString& text, bool isQuery);
    void reopenHistoryMenu(bool isQuery);
>>>>>>> REPLACE
```

#### B. 在 `src/ui/ScanDialog.cpp` 中追加实现：
```cpp
void ScanDialog::setHistoryText(const QString& text, bool isQuery) {
    if (isQuery) {
        m_searchEdit->setText(text);
    } else {
        m_extEdit->setText(text);
    }
    onTriggerSearch();
}

void ScanDialog::removeHistoryItem(const QString& text, bool isQuery) {
    if (isQuery) {
        m_config.queryHistory.removeAll(text);
    } else {
        m_config.extHistory.removeAll(text);
    }
    m_config.save(); // 保存最新的去重单项状态
}

void ScanDialog::reopenHistoryMenu(bool isQuery) {
    // 重新模拟鼠标双击从而完美重新唤醒刷新后的最新下拉面板，提供完美无感动画
    QWidget* target = isQuery ? static_cast<QWidget*>(m_searchEdit) : static_cast<QWidget*>(m_extEdit);
    if (target) {
        QMouseEvent me(QEvent::MouseButtonDblClick, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &me);
    }
}
```

---

### 3.3 重构 `eventFilter` 历史下来菜单渲染逻辑
修改双击事件下的 `eventFilter`，使用 `QWidgetAction` 来包裹自定义的 `HistoryItemWidget`：

```cpp
<<<<<<< SEARCH
    if ((watched == m_searchEdit || watched == m_extEdit) && event->type() == QEvent::MouseButtonDblClick) {
        bool isQuery = (watched == m_searchEdit);
        const QStringList& history = isQuery ? m_config.queryHistory : m_config.extHistory;
        
        if (!history.isEmpty()) {
            QMenu menu(this);
            menu.setStyleSheet("QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; } QMenu::item:selected { background: #232D37; color: #FFF; }");
            
            for (const QString& item : history) {
                menu.addAction(item, [this, isQuery, item]() {
                    if (isQuery) m_searchEdit->setText(item);
                    else m_extEdit->setText(item);
                    onTriggerSearch();
                });
            }
            
            menu.addSeparator();
            menu.addAction("清空历史记录", [this, isQuery]() {
                if (isQuery) m_config.queryHistory.clear();
                else m_config.extHistory.clear();
                m_config.save();
            });
            
            menu.exec(static_cast<QWidget*>(watched)->mapToGlobal(QPoint(0, static_cast<QWidget*>(watched)->height())));
            return true;
        }
    }
=======
    if ((watched == m_searchEdit || watched == m_extEdit) && event->type() == QEvent::MouseButtonDblClick) {
        bool isQuery = (watched == m_searchEdit);
        const QStringList& history = isQuery ? m_config.queryHistory : m_config.extHistory;
        
        if (!history.isEmpty()) {
            QMenu menu(this);
            // 升级样式：加入对 QWidgetAction 自定义组件的 QMenu 内部样式修饰，保持 1A1A1A 深色系的高级感
            menu.setStyleSheet(
                "QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; border-radius: 6px; padding: 4px 0; }"
                "QMenu::separator { height: 1px; background: #333; margin: 4px 0; }"
            );

            // 使下拉面板的宽度与输入框的宽度保持一致
            QWidget* editWidget = static_cast<QWidget*>(watched);
            if (editWidget) {
                menu.setFixedWidth(editWidget->width());
            }
            
            // 使用 QWidgetAction 为每一条历史记录嵌入带有“×”的交互式控件 (对应用户原话：“每个选项右侧都应该有一个“×”……轻松移除某个选项”)
            for (const QString& item : history) {
                auto* wa = new QWidgetAction(&menu);
                auto* itemWidget = new HistoryItemWidget(item, isQuery, &menu, this, &menu);
                wa->setDefaultWidget(itemWidget);
                menu.addAction(wa);
            }
            
            menu.addSeparator();
            
            // 保留一键清空机制，同样以 QWidgetAction 来适配高阶菜单视觉
            auto* clearAction = menu.addAction("清空历史记录", [this, isQuery, &menu]() {
                if (isQuery) m_config.queryHistory.clear();
                else m_config.extHistory.clear();
                m_config.save();
                menu.close();
            });
            clearAction->setIcon(UiHelper::getIcon("close", QColor("#FF4444"), 12));
            
            menu.exec(static_cast<QWidget*>(watched)->mapToGlobal(QPoint(0, static_cast<QWidget*>(watched)->height())));
            return true;
        }
    }
>>>>>>> REPLACE
```

---

## 4. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 下来面板的每一个选项右侧都应该有一个“×”，这样的话用户就可以轻松移除某个选项了 | 在双击输入框弹出的 QMenu 选项中使用 `QWidgetAction` 嵌入 `HistoryItemWidget`，在极右侧渲染“×”按钮，并在点击时触发单项移除、本地存盘、关闭并实时刷新重绘下拉菜单（对应用户原话：“下来面板的每一个选项右侧都应该有一个“×”，这样的话用户就可以轻松移除某个选项了”） | ✅       |
| 2    | 之前的所有问题都全部作废，不需要再理会 | 物理物理重置所有过往作废需求，保持本地 C++ 代码纯净（对应用户原话：“之前的所有问题都全部作废，不需要再理会”） | ✅       |

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.h`（声明用于和 QWidgetAction 组件通信及操作单项历史的 3 个轻量级辅助接口）
- [ ] 模块/文件：`src/ui/ScanDialog.cpp`（实现 QWidgetAction / HistoryItemWidget 控件定义、事件拦截器双击下拉重构、清空/单独删除、主界面历史文本填充并搜索、以及重绘下来面板）

**明确禁止越界修改的范围：**
- [ ] 严禁修改 `ScanConfig` 本身保存 JSON 的底层数据流格式。
- [ ] 作为“纯分析师”模式，本方案文档严禁包含实际的代码写入动作，Jules 严禁通过任何手段（如 `write_file`）修改 C++ 源码。

---

## 6. 实现准则与预警【核心】
1. **QWidgetAction 父子管理**：
   在 `HistoryItemWidget` 的析构和生命周期控制中，避免直接捕获 `this` 导致 Use-After-Free。
2. **重绘动画优雅度提升**：
   使用 QTimer 异步重开，确保旧 menu 的 exec 事件流完全安全闭合退栈后再重新弹出。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **首选项与交互管理** | 任何下拉框和列表项的单独删除必须在主线程数据库/缓存及配置文件中同步生效 | ✅（执行 `m_config.save()` 做到强一致性实时落盘存储，完全合规） |
