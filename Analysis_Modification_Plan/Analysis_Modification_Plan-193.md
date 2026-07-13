# 全仓库架构重构计划（上帝类精准拆分） —— Analysis_Modification_Plan-193.md

## 1. 任务背景与识别清单

为了实现彻底的模块化，杜绝在修改过程中发生任何凭空“脑补”的现象，我们在对整个代码仓库进行二次精细分析后，正式拟定此版**完整合并版第一阶段重构清单**。

---

### 【核心红线纪律】严禁脑补新增任何超出需求的功能

本案及今后所有重构工作**严禁脑补、严禁自我感觉良好地添加任何用户需求范围之外的“冗余功能/多余按钮”**。

在本次审查中，我们精确捕捉到之前 Jules 这个傻逼 Ai 脑补残留的“死功能”：
- `ScanDialog.cpp` 中定义并组装了一个“导出所选为 CSV”的无用死按钮 `m_csvBtn`，但却没有编写任何真正的底层 CSV 数据导出触发。
- 这一冗余功能不仅干扰了界面布局、造成了死代码垃圾，还直接推挤了状态栏空间。
- **在本方案中，我们将彻底根除、擦除该脑补按钮 `m_csvBtn` 的定义与拼装**。后续重构及开发任务中，若发生任何形式的脑补添加，直接打回重做！

---

### 上帝类 1：`ScanDialog` (`src/ui/ScanDialog.cpp`)

#### 1. 基本物理信息
- **类名**：`ScanDialog`
- **文件**：`src/ui/ScanDialog.h`, `src/ui/ScanDialog.cpp`
- **当前总行数**：2707 行
- **最长的 3 个函数**（不含同文件内的 `ScanTableModel`）：
  1. `ScanDialog::setupUi`（第 1359 ~ 1630 行）：**272 行** （子控件组装、QSS 样式注入、事件信号槽连接）
  2. `ScanDialog::onCustomContextMenu`（第 1856 ~ 2005 行）：**150 行** （处理复杂右键上下文菜单、操作文件物理移动/重命名等）
  3. `ScanDialog::eventFilter`（第 2425 ~ 2567 行）：**143 行** （双击输入框历史弹出、快捷键 QuickLook 物理预览拦截、滑动条点击事件等）

#### 2. 实际承担的职责领域 (具体颗粒度)
1. **无边框 Dialog 的拖拽与边缘大小缩放事件拦截**
2. **物理磁盘盘符、NTFS 状态与驱动器介质探测**
3. **搜素框/扩展名输入框的双击历史记录下拉菜单构建与删除交互**
4. **右键上下文菜单生成、重命名弹框、文件物理 I/O 操作（复制/剪切/粘贴）**
5. **缩略图预热流水线与多线程分发调度**
6. **数据表格模型（`ScanTableModel`）—— 包含缩略图多轨缓存、宽高比分析、任务队列合并处理等全部核心数据流**
7. **表格自适应列宽动态计算与拖拽行为大小双向硬限保护决策**
8. **状态栏文本格式化拼装、文件大小字节单位换算与输出状态维护**
9. **键盘全局快捷键处理（包括双段式 Esc 退出、空格键 QuickLook 预览、元数据标签快捷键分发等）**
10. **Tooltip 悬停提示管理（悬停延迟定时器调度、Hover 索引维护与 ToolTipOverlay 联动呈现）**
11. **UI 顶层控件实例化与多子系统组装、信号槽接线、视图切换（组合根）**
12. **【除臃剔骨】彻底铲除历史脑补的 dead 状态栏按钮 `m_csvBtn`（包含与其相关的所有 setupUi 分配、显示隐藏代码，彻底消灭无用冗余逻辑）**

#### 3. 精准模块化拆分方案

| 序号 | 拆分职责领域 (颗粒度) | 新类名 | 新文件名 | 核心运行状态管理说明 |
| :--- | :--- | :--- | :--- | :--- |
| 1 | **无边框缩放与拖拽控制** | `FramelessResizeBorder` | `src/ui/FramelessResizeBorder.h`/`.cpp` | 拦截系统非客户区消息，计算并处理窗口边缘拉伸逻辑。 |
| 2 | **NTFS 驱动器状态与介质探测** | `SystemDriveScanner` | `src/ui/SystemDriveScanner.h`/`.cpp` | 维护可用盘符、NTFS 判别状态，轮询驱动器插入事件并回调通知。 |
| 3 | **搜索历史记录下拉菜单联动** | `HistoryDropdownController` | `src/ui/HistoryDropdownController.h`/`.cpp` | 管理搜索/扩展名输入框的双击事件拦截及历史菜单节点删除逻辑。 |
| 4 | **右键上下文菜单与物理 I/O** | `ContextMenuExecutor` | `src/ui/ContextMenuExecutor.h`/`.cpp` | 构建结果项的右键关联菜单，实现复制、剪切、粘贴、重命名的底层 I/O 交互。 |
| 5 | **缩略图预热流水线控制** | `ThumbnailWarmupPipeline` | `src/ui/ThumbnailWarmupPipeline.h`/`.cpp` | 接管可见行变更时的预加载信号，异步分发生成任务以填充 LRU 缓存。 **（无状态纯计算层，见下方追问说明）。** |
| 6 | **数据表格独立模型** | `ScanTableModel` *(物理移出)* | `src/ui/ScanTableModel.h`/`.cpp` | 彻底脱离主文件，作为独立的类和文件，维护结果集数据。 |
| 7 | **表格自适应列宽与约束决策** | `ResultTableColumnWidthPolicy` | `src/ui/ResultTableColumnWidthPolicy.h`/`.cpp` | 核心解耦算法。**见下方“1.4 节”专门的真相来源声明。** |
| 8 | **状态栏数据格式化** | `StatusBarFormatter` | `src/ui/StatusBarFormatter.h`/`.cpp` | 提供静态/无状态纯计算：大小换算（KB/MB/GB）、千分位数字格式化、状态文本合成。 |
| 9 | **键盘全局快捷键分发** | `GlobalKeyboardShortcutHandler` | `src/ui/GlobalKeyboardShortcutHandler.h`/`.cpp` | 拦截 Dialog 的按键事件，分发 Esc、Space、Ctrl+F、备注快捷键至具体执行器。 |
| 10 | **悬停提示 Tooltip 控制器** | `ViewportTooltipController` | `src/ui/ViewportTooltipController.h`/`.cpp` | 监听鼠标 Hover 位置，维护内部 `QTimer` 延迟周期，防抖调用 `ToolTipOverlay`。 |

---

### 1.4 核心追问（一）：`ResultTableColumnWidthPolicy` 如何查询真实约束？

在列宽计算算法的设计上，我们做出如下**绝对承诺与设计规约**：

- **唯一真相来源原则**：`ResultTableColumnWidthPolicy` **绝对不会**在代码里自己硬编码维护任何关于“路径列最小可用宽度是多少”、“当前列处于什么拉伸模式”的假设数字或静态常量（如之前的 `150` 或 `100` 等魔法数字）。
- **实时运行时查询机制**：
  在计算“名称列自适应宽度上限”和“右侧可用剩余像素空间”时，该策略类将接受 `QTableView*` 指针作为入参，并通过以下 Qt API 实时进行查询：
  1. 调用 `header->sectionResizeMode(i)` 查询每一列的实时缩放模式（是否为 `QHeaderView::Stretch` 或 `QHeaderView::Interactive`）。
  2. 调用 `header->minimumSectionSize()` 获取表头所配置的全局极限物理最小尺寸。
  3. 调用 `tableView->columnWidth(i)` 测量“大小”列与“修改日期”列被用户拉伸后的动态宽度，作为扣减边界。
  4. 调用 `tableView->viewport()->width()` 作为自适应计算的总容器基准，加上 `QScrollBar` 的物理宽度差进行安全占位。

由此，算法将实现：**计算列宽上限 = 视口宽度 - $\sum$ (所有非 Stretch、非自适应列的 `columnWidth` 实测值) - (被 Stretch 列对应的 `minimumSectionSize` 实测值) - 滚动条占位宽度**。
这彻底杜绝了硬编码假设，将 Qt 表头配置作为系统运算的唯一事实来源。

---

### 1.5 核心追问（二）：`ThumbnailWarmupPipeline` 状态与边界澄清

- **唯一状态持有者**：**`ScanTableModel` 是缩略图缓存状态的唯一持有者。**
- **绝对避免重复造轮子**：`ThumbnailWarmupPipeline` 将是一个**完全无状态（Stateless）的纯计算流**。它绝对不会持有自己独立的 `QThreadPool`、任务队列，也绝不持有任何 `QCache` 缓存或 `failedThumbs` 失败标记。所有的缩略图物理生成（`m_thumbPool`）、去重新增（`m_requestedThumbs`）、多轨缓存（`m_thumbCache`）全部保留并归属于 `ScanTableModel` 统一维护。
- **职责与强类型单向接口设计**：
  `ThumbnailWarmupPipeline` 的职责仅限于：“在滚动条变化或视图切换时，测量可视区间，测算出向上、向下各拓宽 `N` 行（例如可视页面高度的 1.5 倍缓冲带）的预热区间，将行号转换成一批 FRN keys，最后通过一个单向通知接口直接提交给 Model”：
  ```cpp
  // 在 ScanTableModel 中公开的去重承接接口
  void ScanTableModel::requestWarmupThumbs(const QVector<uint64_t>& keys);
  ```
  `ScanTableModel` 在此接口中，使用自己维护的 `m_thumbCache` 和 `failedThumbs` 进行过滤去重，将真正需要加载的任务加入其内部已有的 `m_thumbTaskQueue`，通过 `m_thumbPool` 执行。以此保证状态零分裂。

---

### 1.6 核心追问（三）：“搜索/筛选条件收集与触发”为什么不需要拆分？

- **定位分析**：
  在 `ScanDialog.cpp` 中存在 `onTriggerSearch`、`onFilterOptionChanged` 和 `onStartScan` 三个与搜索触发、盘符更改、条件勾选相关的处理槽。我们对这三个函数的具体代码进行了二次审计，结论是：**它们不需要拆分为独立类。**
- **为什么不需要拆分的正当理由**：
  1. **纯转发/胶水定位**：这三个函数本质上只是读取 UI 控件（`QCheckBox` 勾选状态、`QLineEdit` 文本输入），将它们无损打包进 `ScanFilterState` 结构体，然后直接调用 `m_controller->setFilterState(state)` 与 `m_controller->triggerSearch(true)`。
  2. **核心筛选算法已高度内聚**：真正的正则表达式编译、物理白名单/黑名单文件名拆解匹配、多线程搜索，在既有架构中**早就已经完全封装在 `ScanController` 这一高度独立的底层类中**。
  3. **留在组合根的合规性**：因此，这三个槽函数只是纯粹的“UI 状态转发胶水层”，不包含任何具体的算法、格式化或数据缓存管理。让它们留在作为“组合根”的 `ScanDialog` 中，符合只做高层连接 and 转发的单一职责，不构成职责过载。

#### 7. 拆分后，原 `ScanDialog` 还剩下什么？
重构后，`ScanDialog` 将仅作为 **“组合根（Composition Root）”**：
- 在其构造函数和 `setupUi()` 中，实例化上述 10 个职责类的对象。
- 其唯一的业务逻辑就是“接线”——即在子系统实例化完毕后，将它们各自的信号与槽通过高层 `connect` 连接在一起（例如：连接 `ViewportTooltipController` 的悬停信号到主窗口的展示逻辑）。
- 自身完全退化为无状态的壳。一句话描述其职责就是：**“实例化子模块并完成高层信号槽接线的顶层 UI 组合容器”**。

---

### 上帝类 2：`MftReader` (`src/mft/MftReader.cpp`)

#### 1. 基本物理信息
- **类名**：`MftReader`
- **文件**：`src/mft/MftReader.h`, `src/mft/MftReader.cpp`
- **当前总行数**：1866 行
- **最长的 3 个函数**：
  1. `MftReader::updateEntryFromUsn`（第 1115 ~ 1284 行）：**170 行** （USN 变更通知，文件名重名更新，旧 FRN 树节点回收）
  2. `MftReader::loadMftDirect`（第 1471 ~ 1626 行）：**156 行** （打开物理卷设备，读取引导扇区，解析 MFT 记录和元数据）
  3. `MftReader::loadFromCache`（第 280 ~ 424 行）：**145 行** （ScchCache 载入、CRC32 验证、主增量与 tombstone 状态Compaction合并）

#### 2. 实际承担的职责领域 (具体颗粒度)
1. **NTFS 物理层引导扇区直接解析、卷加载与 $MFT 节点读取（核心扫描）**
2. **USN 日志变更监听更新、目录树重构与旧 FRN 节点回收**
3. **主索引缓存加载（ScchCache）生命周期维护与 Compaction 机制控制**
4. **基于内存模型的数据多线程检索引擎（Search Engine）**

*(注：原 GUI 图标依赖函数 `getCachedIcon` 将移至 `UiHelper` 统一管理。)*

#### 3. 精准模块化拆分方案

| 序号 | 拆分职责领域 (颗粒度) | 新类名 | 新文件名 |
| :--- | :--- | :--- | :--- |
| 1 | **NTFS 物理层加载与 $MFT 解析器** | `NtfsVolumeMftParser` | `src/mft/NtfsVolumeMftParser.h`/`.cpp` |
| 2 | **USN 树节点同步与生命周期重构** | `UsnJournalTreeSynchronizer` | `src/mft/UsnJournalTreeSynchronizer.h`/`.cpp` |
| 3 | **磁盘二级缓存及 Compaction 触发控制** | `DiskIndexCacheCoordinator` | `src/mft/DiskIndexCacheCoordinator.h`/`.cpp` |
| 4 | **基于内存模型的数据多线程检索引擎** | `MemoryQueryEngine` | `src/mft/MemoryQueryEngine.h`/`.cpp` |

#### 4. 拆分后，原 `MftReader` 还剩下什么？
重构后，`MftReader` 单例一句话说清的职责是：**“NTFS 数据流聚合、初装载与多线程检索的异步协调控制器”**。
它内部仅持有上述拆分后新类的实力指针，不再具有具体的 MFT 二进制拆解、USN 内存节点重绘、以及缓存合并底层算法。

---

## 2. 待确认事项与约束词声明
- **方位/顺序词**：“1”、“2”、“3”、“4”、“5”（指代章节和函数排序编号）；“左”、“右”（“左侧名称列”、“右侧大小、修改日期列”）；“前”、“后”（代码修改前后的版本定位）。
- **数量/物理词**：“10”（拆分出 10 个子职责模块）；“2707”、“1866”（文件的实际物理行数）。
- **重构原则**：我们重申，在进入第二阶段时，绝不会“凭空脑补”任何多余的新业务或功能漏洞。所有的重构动作都仅针对本方案中识别出的职责领域进行物理平移、状态接线与功能复原，保持在 100% 绿色安全的重构边界内。
