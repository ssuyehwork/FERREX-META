# 彻底治理线程/锁竞争、假死与卡顿问题性能重构方案 —— Analysis_Modification_Plan-200.md

## 1. 任务背景
在项目进行模块化与高性能重构后，由于部分并发设计、多线程重排序与磁盘高频写盘逻辑出现了过度脑补的设计（AI 脑补过度），导致了在 220万+ 规模数据集下，程序在滚轮/滑块缩放、程序退出、USN 事件合并、鼠标悬停提示以及自适应排版中，出现了极其明显的线程竞争、读写锁挂起、GUI 线程阻塞假死与图标拉伸变形等一系列严重缺陷。

本方案严格物理对齐具有最高流畅度与最严谨排版机制的 **“旧版本-3”**，旨在完全治理和移除所有的“傻逼逻辑架构”，提供无锁/去锁化快照异步传递、防抖同步写盘、不阻塞析构、以及逻辑 DPI 1:1 无损绘制的超丝滑全新架构方案。

---

## 2. 问题定位

### 2.1 缩放（Zooming）主线程磁盘同步 I/O 阻塞
* **定位**：`src/ui/ScanDialog.cpp` 的 `m_sizeSlider` 的 `valueChanged` 信号槽内，硬编码同步调用了 `m_config.save()`，在 `ScanDialog.cpp:234`、`261`、`891` 等高频事件响应位置。
* **原因**：每次滚轮变焦和拖拽都会触发数百次磁盘同步写入 `FERREX_scan_config.json`，直接卡死 GUI 事件循环。

### 2.2 析构/切换盘符重置时主线程无理 `waitForDone()` 强同步挂起
* **定位**：`src/ui/ScanTableModel.cpp` 析构函数第 `138` 行
* **原因**：硬编码调用 `m_thumbPool->waitForDone()` 迫使主线程阻塞等候子线程池磁盘 IO 结束。

### 2.3 220万数据下 USN 增量多线程排序与 `MftReader.m_dataLock` 激烈锁竞争
* **定位**：`src/ui/ScanController.cpp` 的 `sort()`、`processBatchUpdates()`
* **原因**：过多的排序线程高频申请底层 SoA 数据读写锁，导致 UI 线程调用 `data()` 渲染时因为锁等待被长期挂起，页面顿挫。

### 2.4 ToolTipOverlay 跨非主线程创建 QWidget 导致事件循环冲突与崩溃
* **定位**：`src/ui/ViewportTooltipController.cpp`
* **原因**：在异步任务回调中跨线程调用 `ToolTipOverlay::instance()`，违反了 Qt 只能在 GUI 线程实例化 QWidget 规则，引发死锁或崩溃。

### 2.5 JustifiedMode 对常规文件/文件夹的傻逼拉伸和比例累计偏差
* **定位**：`src/ui/JustifiedView.cpp`（自适应模式计算段）与 `src/ui/ScanTableModel.cpp` 对普通文件的宽高比返回。
* **原因**：当前版丢弃了“旧版本-3”中只对视频、图像自适应拉伸的阻断保护（`rowIsJustified` 在包含常规文件或最末行时强制为 false），强行对所有文件拉伸并填充宽度，导致排版大范围瘫塌，图标失真。

### 2.6 ThumbnailDelegate 图标 High-DPI 拉伸马赛克和圆角撑爆
* **定位**：`src/ui/ThumbnailDelegate.cpp`（物理图标绘制段）
* **原因**：使用 `icon.pixmap()` 并在自实现矩形中拉伸，导致在 125%、150%+ 缩放的高分屏下，小图标失真、带锯齿。物理高画质缩略图没有做 `thumbStatus == 1` 和 `canConvert<QPixmap>()` 的双轨强校验，导致 QIcon 经常被隐式误判为 QPixmap 直接撑爆卡片圆角。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 排查明确存在线程竞争、锁竞争、假死、卡顿等逻辑代码 | 详细分析了高频写盘、无阻塞析构、无锁快照、去锁排序、跨线程安全以及自适应排版阻断的全部卡顿原因并提出治理方法 | ✅ |
| 2    | 任何时候都必须严格遵循AGENTS.md的规则，这是红线绝不可触碰 | 本方案作为纯分析文档，不直接修改任何代码，严格保持纯分析师身份 | ✅ |
| 3    | 单项/单向性流程，禁止自行回溯 | 本方案严格依顺序产出，基于最新对标“旧版本-3”的共识向前推进 | ✅ |
| 4    | 凡是会导致线程竞争、锁竞争、假死、卡顿的逻辑架构都属于是傻逼逻辑架构，必须达到操作丝滑流畅才行 | 采用无锁指针交换、防抖化磁盘写操作、LIFO 队列、逻辑等比原生绘制、完全还原旧版本-3的布局阻断，使操作恢复行云流水的极致顺滑 | ✅ |

---

## 4. 详细解决方案

### 4.1 缩放高频事件保存配置防抖（Debounce）物理机制
* **设计细节**（对应用户原话：“配置文件高频保存降噪”）：
  * **剥离 save()**：物理剔除 `m_sizeSlider` 的 `connect` 信号槽和滚轮变焦事件过滤器里的同步 `m_config.save()`，使其在此类密集连续事件中仅执行内存中的 `iconSize` 调节。
  * **延迟保存定时器**：在 `ScanDialog` 中增设一个 `m_configSaveTimer`（单次，间隔为 1000ms）。
  * 当尺寸滑块或滚轮调节触发时，仅更新内存配置并调用 `m_configSaveTimer->start()`。只有在用户停止操作满 1000ms 后，才在后台/或主线程空闲时执行一次 `m_config.save()` 持久化。
  * 在 `closeEvent` 和析构阶段，做一次强兜底保存，从而绝对杜绝高频磁盘 I/O 阻塞。

### 4.2 析构无条件 `waitForDone()` 彻底剥离与安全异步销毁
* **设计细节**（对应用户原话：“析构死等防假死规避”）：
  * **消除硬阻塞**：删除 `ScanTableModel` 析构函数中同步等待 `m_thumbPool->waitForDone()` 的设计。
  * **安全线程销毁**：将 `m_thumbPool` 设为在主线程析构时不进行强行等待，而是通过设置子线程为 `autoDelete` 方式。或者在析构时，通过一个原子变量标志（如 `std::atomic<bool> m_isDestroying{true}`）迅速让目前在跑的所有 `ThumbTask` 线程检测到并立刻 `return` 退出。
  * 从而确保在关闭应用或重置检索时，窗口可以在毫秒级瞬间消失并清理非活动队列，杜绝卡死挂起。

### 4.3 千万级无锁快照（ResultSet Swap）与增量重排序 Throttle
* **设计细节**（对应用户原话：“排查明确存在线程竞争、锁竞争、假死、卡顿等逻辑代码”）：
  * **快照无锁读**：主线程 `data()` 渲染层获取文件名、路径、大小等信息时，只通过 `m_controller->snapshot()` 获取只读指针 `std::shared_ptr<ResultSet>`。通过对该智能指针的极速拷贝完成无锁快照访问，无需在 render 循环中申请读写锁。
  * **后台重排序去锁投影**：排序线程开始时，首先在极短的时间内拷贝 SoA 数据池中的局部指针（`m_string_pool`、`m_sizes` 等 SoA 快照投影），随后完全在线程私有内存中去锁化排序。排序完毕后，使用 `resultsSwapped()` 信号原子级地将全新 `ResultSet` 传递并更新回主线程模型中。
  * **USN 防抖合并（Throttle）**：高频的 USN 数据写入与改变，不直接高频触发 `processBatchUpdates`，而是使用 `m_batchTimer` 合并进行。当事件数超过 2000 时，不进行反复增量排序，而是直接降级进行快速的后台全量重索，并彻底避免多线程对底层 `m_dataLock` 锁的反复强占。

### 4.4 ToolTipOverlay 主线程强制冷启动与跨线程生命周期隔离
* **设计细节**（对应用户原话：“主线程单例强制冷启动”、“跨线程安全缺陷修复”）：
  * **首层主线程实例化**：在 `ScanDialog` 构造函数的核心头部（GUI主线程中），强制显式调用一次 `ToolTipOverlay::instance()`，使其 QWidget 在最安全的 UI 线程冷启动生成，避免任何后台任务在第一次触发提示时由于懒加载在非 GUI 线程实例化 QWidget 崩溃。
  * **异步呼叫规约**：在事件过滤器中，凡是需要刷新或改变 ToolTip 内容的操作，强制使用 `QMetaObject::invokeMethod` 传递或者信号槽，把更新指令排队分发到主线程执行，从根本上隔离跨线程非法 UI 控制。

### 4.5 物理对标“旧版本-3”的自适应 Justified 阻断排版与宽高比保护
* **设计细节**（对应用户原话：“所谓的自适应仅限于视频、图形图像，除此之外仅剩下常规文件类型了”）：
  * **宽高比强规整**：在 `ScanTableModel::data()` 返回自适应比例（`UserRole + 2`）时，对文件夹及所有常规非视频、图形图像后缀类型的文件（不属于 jpg、png、psd、mp4、avi 等后缀），强制无条件返回 `-1.0`。
  * **JustifiedMode 还原旧版本-3 的阻断机制**：
    在 `JustifiedView.cpp` 宽度自适应计算中，完全还原“旧版本-3”的比例与阻断判定：
    * 当 `ar <= 0.01` 时，强制将宽高比等同于 `1.0` 的物理标准正方形。
    * 行拉伸填充判定阻断器：
      ```cpp
      bool containsRegular = false;
      for (bool isReg : isRegularFlags) {
          if (isReg) { containsRegular = true; break; }
      }
      bool rowIsJustified = !isLastRow && !containsRegular;
      ```
    * 只要该行包含常规文件（如 AHK、TXT 等），或者属于最末一行，`rowIsJustified` 立即强制置为 `false`，彻底关闭拉伸填满特性。这确保了常规文件始终在自适应网格中保持完美、对称、不变形的 `1:1` 物理正方形排布，彻底消除了由于比例误差导致的负值与坍塌。

### 4.6 物理还原 ThumbnailDelegate 1:1 高画质逻辑 DPI 居中绘制
* **设计细节**（对应用户原话：“图标未能中心完美居中且发生拉伸、变形”）：
  * **恢复 icon.paint**：完全去除自实现的物理 `QPixmap` 物理像素宽高 bounds 计算，彻底还原为“旧版本-3”原生的 DPI 完美自适应居中绘制：
    ```cpp
    int iconSize = qMin(m.cardRect.width(), m.cardRect.height()) * 0.55;
    QRect iconRect(m.cardRect.center().x() - iconSize / 2,
                   m.cardRect.center().y() - iconSize / 2,
                   iconSize, iconSize);
    icon.paint(painter, iconRect);
    ```
    由 Qt 原生的 `icon.paint` 依靠逻辑 DPI 像素在不同高分屏下自适应渲染无损 1:1 图标，杜绝锯齿和拉伸模糊。
  * **双轨强条件校验高画质缩略图**：
    在 `ThumbnailDelegate.cpp` 中提取高画质大图缩略图时，恢复严格的旧版本-3 逻辑：
    ```cpp
    if (thumbStatus == 1 && decoData.canConvert<QPixmap>()) {
        thumb = decoData.value<QPixmap>();
        if (!thumb.isNull()) { hasValidThumb = true; }
    }
    ```
    只有后台真正生成了高分辨率物理大图且可以转换为 Pixmap 时，才进入圆角大图渲染，彻底切断 QIcon 在常规状态下被隐式转换为 Pixmap 导致撑爆卡片圆角的问题。

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] `src/ui/ScanDialog.cpp` & `ScanDialog.h` （防抖 save 改造，ToolTip 实例化及事件分发，缩放信号重组）
- [ ] `src/ui/ScanTableModel.cpp` & `ScanTableModel.h` （安全析构防死等重构，常规文件比例返回 `-1.0`）
- [ ] `src/ui/JustifiedView.cpp` （自适应计算段，完全物理对标“旧版本-3”的 rowIsJustified 阻断功能）
- [ ] `src/ui/ThumbnailDelegate.cpp` （完全物理对标“旧版本-3”的 `icon.paint` 绘制与高画质缩略图双轨强判定条件）
- [ ] `src/ui/ScanController.cpp` （无锁快照快传，USN Throttle 防抖合并及重排序锁释放）

**明确禁止越界修改的范围：**
- [ ] 严禁修改任何磁盘底层 NTFS/MFT 解析实现。
- [ ] 严禁修改加密、压缩等其他无关业务底层逻辑。

---

## 6. 实现准则与预警【核心】
1. **头文件保障**：修改 `ScanDialog.cpp` 与 `ScanTableModel.cpp` 时，必须确保包含 `<QTimer>`、`<QThread>`、`<QThreadStorage>` 与相关的 Qt 并发、原子操作头文件，防止出现由于异步取消而导致的类成员未定义等编译错误。
2. **多线程野指针预警**：在后台线程中异步排序或 USN 合并时，必须通过 `QPointer<ScanDialog>` 或 `weak_ptr` 检测主框架及 Model 的生命周期状态。一旦发现其被销毁，应该立刻取消当前异步任务，彻底隔绝由于对象生命周期错配引发的内存野指针崩溃。
3. **线程亲和力（Thread Affinity）预警**：严格保障所有的 `QWidget` 相关的 ToolTip 刷新方法由 `QMetaObject::invokeMethod(qApp, ...)` 排队回 GUI 线程完成，杜绝跨非主线程进行直接或间接的 UI 操作。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 评级显示逻辑还原 | 移除悬停高亮，固定 6 图标占位，中性灰锁定，与 hover 无关 | ✅ 本方案不涉及此组件，完全保持既有的 Memories 逻辑不被触碰 |
| 工业级虚拟化架构 | 使用基于 `QAbstractTableModel` 的虚拟化模型 `FerrexVirtualDbModel`，秒开百万级数据 | ✅ 本方案严格采用全快照 O(1) 传递和局部刷新，保障千万级下虚拟化模型的超高速 data() 取数效率 |
