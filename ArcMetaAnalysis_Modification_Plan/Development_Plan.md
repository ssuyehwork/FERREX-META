# Development Plan - FERREX-META

## 需求记录与审计建议

### 1. 性能瓶颈分析 (Plan-154)
- **目标**：解决 FERREX-META 在百万级数据下的搜索假死问题。
- **核心病因**：
    1. 读写锁竞争：写锁优先导致 UI 读请求被后台搜索长时间阻塞。
    2. 任务溢出：搜索任务不可中途取消，导致线程池被旧任务塞满。
    3. 哈希构建成本：ResultSet 中 200 万项的 `unordered_map` 构建耗时过长。
    4. 内存抖动：循环内频繁构造 QString 对象。
- **建议方案**：实施无锁快照搜索与零分配筛选。

### 2. 交互逻辑规约 (Plan-154 补充)
- **左键逻辑**：盘符按钮左键仅用于 FilterState 切换，严禁触发数据库加载。
- **右键逻辑**：提供显式的“加载数据”菜单，引导用户按需管理内存。
- **粘贴屏蔽**：彻底移除粘贴功能，改为状态栏提示。

### 3. 数据加载策略 (Plan-155)
- **目标**：废除分页加载机制，实现搜索结果全量映射。
- **核心要求**：rowCount 始终返回真实结果数，移除 fetchMore 逻辑。
- **风险规避**：必须依赖异步投影排序与无锁化渲染以减轻 UI 线程计算压力。

### 4. 预览功能集成 (Plan-157)
- **目标**：复刻 ArcMeta 的“原始优雅版”QuickLook 空格键预览。
- **UI 规约**：
    - 必须为**居中悬浮窗**（非全屏），圆角 12px，样式对标截图。
    - 预览透明图像必须显示**棋盘格背景**（#2B2B2B / #333333 瓦片）。
    - 集成 `ToolTipOverlay` 提示引擎，在切换/打分时显示巨大的半透明图标反馈。
- **文本对标 Notepad++**：
    - 实现 GBK/UTF-8 编码自适应探测。
    - 二进制安全拦截（遇 0x00 字节停止渲染或提示）。
    - 仅读取头部 128KB 保证百万级文件下的瞬间响应。

### 5. 视图排版交互重构 (Plan-159)
- **目标**：彻底移除菜单中冗余离散的图标尺寸级别选项（超大/大/中图标），重构视图控制，使其精简为“自适应、网格、列表”三大纯排版模式。
- **核心要求**：
    - 图标大小由标记为①的滑块独占控制，移除标记为② of the menus items.
    - 重组菜单结构，将“自适应”、“网格”、“列表”设为平级单选切换。
    - 确保配置同步（`viewMode`, `layoutMode`）及对 `m_sizeSlider` 事件处理、滑动调节功能的完整兼容。

### 6. ScanDialog 主界面无边框窗口边缘检测与拉伸缩放支持 (Plan-175)
- **目标**：参考 ArcMeta 架构，解决 ScanDialog 主界面由于子控件事件拦截导致无法边缘拖拽调整大小以及未显示双向箭头光标的问题。
- **核心要求**：
    - 移植并实现全局应用级事件过滤器 `ResizeEventFilter`，通过 `QCoreApplication::instance()->installEventFilter(m_resizeFilter)` 机制，彻底绕过 ScanDialog 内部所有子控件（如输入框、按钮、表格视图等）对鼠标移动事件的拦截遮挡，实现全局、高灵敏度的边缘悬停与双向光标（SizeHorCursor/SizeVerCursor/SizeFDiagCursor/SizeBDiagCursor）实时刷新（对应用户原话：“当鼠标移动到窗口边缘时也没有出现双向箭头”）。
    - 在 `ScanDialog` 类中，重写 `mousePressEvent`、`mouseMoveEvent`、`mouseReleaseEvent` 鼠标事件处理虚函数，加入 DPI 自适应的检测阈值 `getResizeDirection` 与光标刷新 `updateCursorShape` 函数。
    - 在 `ScanDialog` 事件响应中集成完美的拖拽包围盒几何拉伸计算（对应用户原话：“导致无法通过拖拉方式调整窗口大小”），同时在拉伸计算中对最小尺寸限制（`setMinimumSize`）进行严格遵循与集成。

### 7. 全局滚动条宽度样式优化 (Plan-174)
- **目标**：解决滚动条过窄导致鼠标拖拽和操作困难的问题。
- **核心要求**：
    - 优化 `ScanDialog.cpp` 的全局 QSS 样式。将垂直滚动条（`QScrollBar:vertical`）的宽度（width）从默认 of 4px 调整至 7px（对应用户原话：“将其宽度调整为7像素”），以此解决操作吃力的问题（对应用户原话：“滚动条的宽度过于太窄了，导致操作吃力”）。
    - 将水平滚动条（`QScrollBar:horizontal`）的高度（height）同步从 4px 调整至 7px。
    - 将滚动条拖拽手柄（`QScrollBar::handle`）的圆角半径（`border-radius`）从原先的 2px 调整为 3px，以实现视觉与可用性的平衡。

### 8. 物理清除冗余快捷键绑定 (Plan-176)
- **目标**：彻底、干净地移除 `ScanDialog` 中不再需要的 `Ctrl+Shift+1 - 3` 快捷键注册，简化代码，杜绝底层热键冲突风险。
- **核心要求**：
    - 从 `ScanDialog.cpp` 中彻底物理剔除三个视图 Action（`m_actJMode`, `m_actGMode`, `m_actListMode`）的快捷键显示文本设定（即彻底删除所有的 `setShortcut(QKeySequence(...))` 逻辑）。
    - 物理删除三个底层 `QShortcut` 对象的动态创建、上下文绑定以及对应的槽函数 `trigger` 信号连接，彻底杜绝代码残留。

### 9. ScanDialog / FramelessDialog 无边框最大化与状态恢复重构 (Plan-177)
- **目标**：彻底解决无边框主界面最大化后无法双击还原、无法拖拽还原、以及点击按钮或快捷键状态不一致的问题。
- **核心要求**：
    - **双击标题栏响应**：在 `FramelessDialog` 中重写 `mouseDoubleClickEvent`（对应用户原话：“双击标题栏也恢复不了窗口”）。判断点击若在标题栏且非按钮上，则自动切换最大化与常规（Normal）状态。
    - **最大化拖拽过渡还原**：在 `mouseMoveEvent` 拖动标题栏时，若检测到当前窗口是最大化状态（对应用户原话：“拖动标题栏也无法恢复窗口”），先自动触发 `showNormal()`，并根据常规窗口尺寸计算出当前鼠标按下的新相对位置，实现流畅无缝的“拖拽自动还原且跟随移动”。
    - **窗口状态变更全同步**：重写 `changeEvent` 并捕捉 `QEvent::WindowStateChange` 事件（对应用户原话：“点击恢复按钮有时无法恢复”），精准感知系统级（如 Win+Up/Down 快捷键、Win11 贴靠布局）触发的状态改变，并自动更新最大化/还原按钮的图标样式，杜绝任何状态不同步引起的失效。

### 10. Ctrl+W 全局支持与 Esc 两段式清空关闭机制重构 (Plan-178)
- **目标**：支持任何界面按下 Ctrl+W 关闭窗口，同时将 ScanDialog 的 Esc 逻辑升级为两段式清空后再关闭。
- **核心要求**：
    - **Ctrl+W 全窗口物理通配**：在无边框对话框基类 `FramelessDialog` 的 `keyPressEvent` 与空格预览窗口 `QuickLookWindow` 的 `keyPressEvent` 中追加对 `Ctrl+W` 按键序列的统一拦截（对应用户原话：“我期望整个应用的任何界面都必须支持Ctrl+W关闭窗口”），拦截成功后执行 `reject()` 或 `close()` 关闭窗口，并阻止事件冒泡。
    - **ScanDialog 专属 Esc 两段式分发**：重写 `ScanDialog` 的 `keyPressEvent` 中 `Qt::Key_Escape` 分支的物理处理（对应用户原话：“在ScanDialog窗口首次按下键时”）。在 Esc 触发时，若 `m_searchEdit` 或 `m_extEdit` 两个核心输入框中有任意一个内容非空，则先执行一键全部清空（`clear()`）（对应用户原话：“应该先清空ScanDialog主窗口所有输入框的文字”）；只有当两个输入框本就全空时，再次按下 Esc 才执行物理关闭（`reject()` / `close()`）动作（对应用户原话：“如果首次按下Esc键时，所有输入框都已经处于清空文字状态情况下则直接关闭ScanDialog窗口”）。

### 11. 空格预览顶层准入前置拦截重构 (Plan-179)
- **目标**：改变“先弹出预览窗，加载失败才提示无法预览”的后置逻辑，物理移植 ArcMeta 的前置属性白名单拦截规则。
- **核心要求**：
    - **空格键触发前置校验**：在 `ScanDialog::keyPressEvent` 与 `ScanDialog::eventFilter` 捕获空格键触发预览前，通过静态函数 `isPathPreviewable` 对目标文件路径进行先期探测（对应用户原话：“先判断项目属性”）。
    - **双轨黑白名单过滤**：对文件夹及压缩包/可执行等黑名单类型（`exe`, `dll`, `zip`, `rar`, `7z` 等）执行绝对阻断，仅对受支持的白名单格式（常用图像、音视频、代码文本等）放行呼叫 `m_quickLook->preview(path)`；对于无法预览的项直接拦截并 `Return`，彻底杜绝闪烁和加载抛错（对应用户原话：“无法预览的可直接不用打开预览界面，直接Return即可”）。
