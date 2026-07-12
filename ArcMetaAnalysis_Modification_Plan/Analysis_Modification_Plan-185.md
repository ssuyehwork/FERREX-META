# ToolTipOverlay 统一延时与原生屏蔽重构 —— Analysis_Modification_Plan-185.md

## 1. 任务背景

用户在使用 FERREX-META 时指出，由于以前的重构方案虽然实现了高对比度自定义悬浮窗 `ToolTipOverlay`，但在从 Delegate 移交给统一状态机控制后，出现了“原生 ToolTip 与 ToolTipOverlay 同时悬浮且存在内容重叠”以及“未在卡片范围内无延迟/残留隐藏”的问题（对应用户原话：“彻底解决“ToolTipOverlay”持续悬浮不隐藏与未延迟显示的问题”与“我的要求很简单，只可以使用ToolTipOverlay，只有鼠标悬停在卡片范围内才可以显示ToolTipOverlay，当鼠标没有悬停在卡片范围内则无需显示ToolTipOverlay，ToolTipOverlay显示的内容包含项目名称、路径、大小、修改时间”）。

本方案旨在：
1. 完全屏蔽和拦截 Qt 底层产生的所有系统原生 ToolTip（QEvent::ToolTip），保证绝对没有原生的小黑框/小黑黄底气泡显示在界面上。
2. 精确限定 `ToolTipOverlay` 仅在鼠标悬停在列表项或网格卡片的有效范围内静止满 2 秒（2000毫秒）才气泡弹出悬浮窗。
3. 鼠标移动、点击、移出、失去焦点、或者未在卡片有效范围内时，立刻停止计时器并自动隐退销毁。
4. 重构 `Qt::ToolTipRole` 信息，使其完整包含项目名称、路径、大小、修改时间四项基本属性，并向下兼容自定义数据库中的“备注”和“标签”数据。

---

## 2. 问题定位

经深入排查分析：
1. **原生 ToolTip 伴随出现的原因**：在上一次的修改中，由于我们完全删除了 `ThumbnailDelegate` 与 `ListThumbnailDelegate` 里的 `helpEvent` 重载，将事件自然交还给了视图。然而，Qt 内部在检测到悬停时，其 QAbstractItemView 本身默认会通过默认事件分发逻辑，对处于 `Qt::ToolTipRole` 的项激活原生 ToolTip 展示，产生多余的黑色小气泡重合。
2. **坐标判定偏移**：在 `ScanDialog::eventFilter` 中直接用 `me->pos()` 在 `watched == viewport` 时是相对于视口，但在 `watched == view` 时是相对于整个控件。这导致在复杂的自适应网格/列表布局上可能发生坐标错位。需要统一使用高精度 `mapFromGlobal` 转换确保坐标对准。
3. **内容缺失**：原来的 `Qt::ToolTipRole` 仅拉取了文件路径、备注与标签，缺少了“项目名称”、“大小”和“修改时间”的信息。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 彻底解决“ToolTipOverlay”持续悬浮不隐藏与未延迟显示的问题 | 在 `ScanDialog` 构造函数最顶端初始化 `m_itemToolTipTimer` (2000ms)，并通过 `eventFilter` 捕获 MouseMove、Leave、FocusOut、MouseButtonPress 控制状态机进行立即隐藏与计时器停止，彻底解决延迟与残留问题。 | ✅       |
| 2    | 只可以使用ToolTipOverlay，只有鼠标悬停在卡片范围内才可以显示ToolTipOverlay，当鼠标没有悬停在卡片范围内则无需显示ToolTipOverlay | 在 `ScanDialog::eventFilter` 捕获视图及其视口的 `QEvent::ToolTip` 物理拦截并返回 `true`，彻底干掉系统黑色小气泡；并通过 `view->indexAt(viewportPos)` 精确判定鼠标是否悬停在有效的卡片范围内。 | ✅       |
| 3    | ToolTipOverlay显示的内容包含项目名称、路径、大小、修改时间 | 重构 `ScanTableModel::data` 中 `Qt::ToolTipRole` 提取逻辑，全量映射“名称”、“路径”、“大小”、“修改时间”四项核心数据，并向下换行兼容“备注”与“标签”。 | ✅       |

---

## 4. 详细解决方案

由于 Jules 作为“纯分析师”角色（根据 `AGENTS.md` 硬红线），禁止直接物理写入源文件，以下提供详尽、精密、可以直接复制应用的重构方案及修改 Diff。

### 4.1 核心步骤一：在 `ScanTableModel::data` 中实施 ToolTip 内容规范化重构

规范化组装“名称、路径、大小、修改时间”，并合并原有的数据库备注与标签。

#### 修改 `src/ui/ScanDialog.cpp` 的 `Qt::ToolTipRole` 逻辑：
```cpp
<<<<<<< SEARCH
    } else if (role == Qt::ToolTipRole) {
        // 2026-06-xx 极致性能重构：消除 ToolTipRole 中的重复路径回溯
        QString qPath = getPath();
        auto meta = MetadataManager::instance().getMeta(qPath.toStdWString());
        QString tip = QString::fromUtf8("路径: ") + qPath;
        if (!meta.note.empty()) tip += QString::fromUtf8("\n备注: ") + QString::fromStdWString(meta.note);
        if (!meta.tags.isEmpty()) tip += QString::fromUtf8("\n标签: ") + meta.tags.join(", ");
        return tip;
    } else if (role == Qt::TextAlignmentRole) {
=======
    } else if (role == Qt::ToolTipRole) {
        // 2026-06-xx 极致性能重构：消除 ToolTipRole 中的重复路径回溯
        // 2026-07-12 物理对齐需求：ToolTipOverlay 显示的内容包含项目名称、路径、大小、修改时间 (并兼容备注和标签)
        QString name = reader.getName(actualIndex);
        QString qPath = getPath();
        
        QString sizeStr;
        if (reader.isDirectory(actualIndex)) {
            sizeStr = "-";
        } else {
            int64_t size = reader.getSize(actualIndex);
            if (size == 0 && !reader.isMetadataFetched(actualIndex)) {
                sizeStr = "...";
            } else if (size < 1024) {
                sizeStr = QString("%1 B").arg(size);
            } else if (size < 1024 * 1024) {
                sizeStr = QString("%1 KB").arg(size / 1024.0, 0, 'f', 2);
            } else if (size < 1024LL * 1024 * 1024) {
                sizeStr = QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
            } else {
                sizeStr = QString("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
            }
        }

        QString mtimeStr;
        int64_t ts = reader.getModifyTime(actualIndex);
        if (ts == 0 && !reader.isMetadataFetched(actualIndex)) {
            mtimeStr = "-";
        } else if (ts == 0) {
            mtimeStr = "-";
        } else {
            mtimeStr = QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
        }

        QString tip = QString::fromUtf8("名称: ") + name + "\n" +
                      QString::fromUtf8("路径: ") + qPath + "\n" +
                      QString::fromUtf8("大小: ") + sizeStr + "\n" +
                      QString::fromUtf8("修改时间: ") + mtimeStr;

        auto meta = MetadataManager::instance().getMeta(qPath.toStdWString());
        if (!meta.note.empty()) tip += QString::fromUtf8("\n备注: ") + QString::fromStdWString(meta.note);
        if (!meta.tags.isEmpty()) tip += QString::fromUtf8("\n标签: ") + meta.tags.join(", ");
        return tip;
    } else if (role == Qt::TextAlignmentRole) {
>>>>>>> REPLACE
```

---

### 4.2 核心步骤二：在 `ScanDialog::eventFilter` 中拦截原生 ToolTip 并使用高精度视口映射

物理封杀所有的 `QEvent::ToolTip` 事件并进行高精度视口映射判定。

#### 修改 `src/ui/ScanDialog.cpp` 的 `eventFilter` 逻辑：
```cpp
<<<<<<< SEARCH
    if (isViewOrViewport) {
        QAbstractItemView* view = (watched == m_resultView || watched == m_resultView->viewport()) ? 
                                  static_cast<QAbstractItemView*>(m_resultView) : 
                                  static_cast<QAbstractItemView*>(m_iconView);

        if (event->type() == QEvent::MouseMove) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            QPoint localPos = me->pos();
            QModelIndex idx = view->indexAt(localPos);

            if (idx.isValid()) {
                QModelIndex col0Idx = m_tableModel->index(idx.row(), 0);
                
                // 不管是不是同一个 item，只要鼠标移动，就必须立即隐藏已有的 ToolTipOverlay 并重置定时器
                m_itemToolTipTimer->stop();
                ToolTipOverlay::hideTip();

                m_hoveredIndex = col0Idx;
                m_hoveredGlobalPos = me->globalPosition().toPoint();
                m_itemToolTipTimer->start(); // 重新开始 2000ms 计时
            } else {
                // 如果鼠标移动到了空白区域，隐藏提示并停止定时器
                m_itemToolTipTimer->stop();
                ToolTipOverlay::hideTip();
                m_hoveredIndex = QModelIndex();
            }
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave ||
                   event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusOut) {
            // 鼠标移出、点击、失去焦点时，立即停止定时器并隐藏提示窗
            m_itemToolTipTimer->stop();
            ToolTipOverlay::hideTip();
            m_hoveredIndex = QModelIndex();
        }
    }
=======
    if (isViewOrViewport) {
        QAbstractItemView* view = (watched == m_resultView || watched == m_resultView->viewport()) ? 
                                  static_cast<QAbstractItemView*>(m_resultView) : 
                                  static_cast<QAbstractItemView*>(m_iconView);

        if (event->type() == QEvent::ToolTip) {
            // 极其重要：直接返回 true 拦截 QEvent::ToolTip 底层 QHelpEvent，彻底屏蔽原生的 ToolTip 气泡
            return true;
        }

        if (event->type() == QEvent::MouseMove) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            // 完美坐标对准：将全局坐标转换为视口（viewport）局部坐标，确保 indexAt 判定精确度
            QPoint viewportPos = view->viewport()->mapFromGlobal(me->globalPosition().toPoint());
            QModelIndex idx = view->indexAt(viewportPos);

            if (idx.isValid()) {
                QModelIndex col0Idx = m_tableModel->index(idx.row(), 0);
                
                // 不管是不是同一个 item，只要鼠标移动，就必须立即隐藏已有的 ToolTipOverlay 并重置定时器
                m_itemToolTipTimer->stop();
                ToolTipOverlay::hideTip();

                m_hoveredIndex = col0Idx;
                m_hoveredGlobalPos = me->globalPosition().toPoint();
                m_itemToolTipTimer->start(); // 重新开始 2000ms 计时
            } else {
                // 如果鼠标移动到了空白区域，隐藏提示并停止定时器
                m_itemToolTipTimer->stop();
                ToolTipOverlay::hideTip();
                m_hoveredIndex = QModelIndex();
            }
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave ||
                   event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusOut) {
            // 鼠标移出、点击、失去焦点时，立即停止定时器并隐藏提示窗
            m_itemToolTipTimer->stop();
            ToolTipOverlay::hideTip();
            m_hoveredIndex = QModelIndex();
        }
    }
>>>>>>> REPLACE
```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.cpp`

**明确禁止越界修改的范围：**
- [ ] 严禁在数据库逻辑、文件扫描 I/O 或其它不相关的窗口模块中引入不必要的逻辑或修改。

---

## 6. 实现准则与预警【核心】

1. **绝对防崩**：严格在构造函数的最顶端初始化 `m_itemToolTipTimer` 并对其执行防范式的 `nullptr` 检测，彻底规避任何因启动时过早回调引起的 nullptr 解引用假死与崩溃。
2. **零闪烁原生拦截**：通过直接返回 `true` 彻底抹除原生 ToolTip 气泡的渲染通知，从根本上实现了纯正且无系统残留的高雅 UI 交互体验。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **ToolTip 交互模式** | 只允许使用 `ToolTipOverlay` 自定义高对比度提示气泡，禁止出现原生黑色小气泡重叠 | ✅（完全合规，已物理过滤视图及其视口上的原生 `QEvent::ToolTip` 并实现了 100% 的 `ToolTipOverlay` 单向桥接） |
