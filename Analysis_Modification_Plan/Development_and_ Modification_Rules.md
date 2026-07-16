为了让 `FERREX-META` 项目的架构重构能够真正落地，并防止后续开发由于人员变动或紧急需求重新陷入“缝缝补补、职责不清”的泥潭模式，以下为您梳理一份**通用的工业级开发守则与技术规约**。

本守则从**架构边界、并发与锁、内存与资源、极致性能、异常安全**五个维度制订了严格的开发红线（分为“必须”、“应当”与“严禁”），以此作为团队研发与代码审查（Code Review）的客观标准。

---

### 一、 架构边界与依赖控制规约（Architectural Boundaries）

| 规则级别 | 具体守则内容 | 技术原理解析 |
| :--- | :--- | :--- |
| **严禁** | 基础设施层（如 `Core`、`Mft`）直接或间接引入任何 GUI 依赖（如 `QIcon`、`QColor`、`QPainter`）。 | 保持底层核心逻辑与表示层物理隔离。一旦引入 GUI，会导致核心引擎无法进行独立单元测试，且容易引发跨线程的 GUI 渲染崩溃。 |
| **严禁** | 表示层（`UI`）绕过 ViewModel/Service 直接持有 `Core` 或 `Parser` 的实例指针。 | 跨层直接调用会使分层架构形同虚设。UI 会直接承担业务状态机计算，使系统退化回紧耦合状态。 |
| **必须** | 任何数据从 Domain Service 流向 UI 线程时，都必须转化为**只读、自包含且不可变的快照投影（Immutable POD Projection）**。 | CQRS（读写分离）的核心。UI 线程只负责“绘制静态投影”，数据不含有任何底层引擎的活动悬空状态，从而切断数据不一致性。 |
| **应当** | 对系统关键行为（如 MFT 扫描、USN 解析、快照读写）定义抽象接口层，面向接口编程。 | 确保在未来需要将 MFT 替换为 Windows Indexing Service 或其他文件系统引擎时，只需更换 Service 实现，无需更改任何界面代码。 |

---

### 二、 并发、多线程与零锁规约（Concurrency & Lock-Free）

```
[GUI 主线程] ────► 必须零 I/O ────► 必须零复杂计算 ────► 锁持有时间必须 < 100μs
                                                                  │
                                                                  ▼
                                                      [只读 Immutable 结果集投影]
```

1. **GUI 线程黄金法则（The Golden Rule of UI）**：
   * **严禁** 在 GUI 主线程进行任何形式的阻塞式 I/O（如 `QFile::exists`、读取快照、`GetFileAttributes`）或高开销算法（如千万级元素遍历、正则匹配）。
   * **必须** 保证 GUI 线程在任何生命周期下，申请并持有任意互斥锁（`QMutex` / `std::mutex` / `QReadWriteLock`）的持续时间 **$< 100\mu s$**。

2. **双缓冲更新交换规约（Double-Buffered Swap Rule）**：
   * **必须** 在后台线程完成数据筛选、排序和物理属性预取，并生成静态 `ResultProjection`。
   * **必须** 在交换新旧结果集指针时，利用 Qt 的事件队列投递信号（`Qt::QueuedConnection`）或使用轻量级的原子读写屏障，将写锁的申请限制在微秒级：
     ```cpp
     // 推荐的无锁/极简锁指针交换示例
     std::shared_ptr<const ResultSet> snap = std::atomic_load(&m_activeResultSet);
     // UI 线程随后只引用 snap 进行无阻碍绘制，即便后台正在进行新的检索或 compact 整理
     ```

3. **硬件资源感知分配（Hardware-Aware Affinity）**：
   * **必须** 动态探测宿主物理介质：
     * **机械硬盘 (HDD)**：强制限制所有后台提取器（缩略图、色板、File ID）的最高并发数为 **1**。
     * **固态硬盘 (SSD)**：允许并发数为 $\min(4, \text{CPU物理核心数})$。
   * **应当** 为耗时 I/O 任务（磁盘读写）与高计算任务（正则、色板合并）分配独立的线程池，避免计算密集型任务饿死 I/O 读写队列。

---

### 三、 内存、资源与生命周期管理机制（Memory & Resource Management）

1. **裸指针安全红线（No Raw Pointer Allocation）**：
   * **严禁** 使用 `new` / `delete` 进行显式堆内存生命周期管理。所有动态组件分配必须使用 RAII 容器（如 `std::unique_ptr`、`std::shared_ptr`、`QScopedPointer`）。
   * **必须** 预防跨线程回调中的**对象夭折（Use-After-Free）**风险。后台异步任务（如 `QtConcurrent::run`）在引用任何 UI 控件或适配器（`ViewModel`）时，**必须**使用 `QPointer`（弱引用监视器）或 `std::weak_ptr` 进行安全包裹，并在回调开头进行空值校验。

2. **资源泄露与爆仓阻断（Leak Prevention）**：
   * **必须** 对内存敏感的临时资产（如 Pixmap 缩略图、视口缓存）设置软硬件容量上限上限（`QCache::setMaxCost`），超出阈值时必须强制由系统自动剔除。
   * **严禁** 在 SoA（结构体数组）的增量变更（如 USN 重命名）中仅做追加而不对原废弃字符串进行物理剔除。在废弃空间累计达到 **$10\text{ MB}$** 或失效条目数大于 **$50,000$** 时，系统**必须**通过高优先级后台异步任务自动触发 compact 紧凑化算法，物理归并和释放堆碎片。

---

### 四、 异常安全、错误容忍与日志规范（Exception & Logging）

1. **防御性编程（Defensive Programming）**：
   * **严禁** 在解析 MFT 扇区或文件 ID 时，对未经验证的二进制数据进行强制指针强制类型转换。所有缓冲区边界读取**必须**有严格的断言与 `offsetof` 偏移保护。
   * **必须** 确保所有涉及 Windows API / Shell COM 组件调用的区域，均有严格的异常/错误捕获机制，并退回到安全兜底路径（例如：缩略图生成失败时，必须在 L2 失败追踪容器中记录并直接返回默认系统图标，阻断任务在同一失效文件上死循环重复拉取）。

2. **COM 运行环境保护（Scoped COM Safety）**：
   * **必须** 使用 Scoped 卫兵管理 COM 初始化生命周期：
     ```cpp
     // 严禁直接裸调用 CoInitialize/CoUninitialize
     struct ScopedComInit {
         ScopedComInit() { CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); }
         ~ScopedComInit() { CoUninitialize(); }
     };
     ```

3. **零日志污染与定位排查规范（Zero-Pollution Logging）**：
   * **严禁** 在高频渲染（`paintEvent`）或 SoA 遍历循环中输出调试日志。
   * **必须** 建立统一分级的日志流，并将日志写入路径锁定在用户的 `%APPDATA%`，严禁写入处于 MFT 监控中的工作盘符目录，杜绝因写入调试日志而导致 USN 触发无线自激循环的逻辑漏洞。

---

### 五、 极致性能与 API 一致性守则（High-Performance & Consistency）

```
[UI 渲染热路径]
      │
      ├──► 严禁 QString::fromUtf8 / QString::number 等动态转换 (必须使用预取投影缓存)
      ├──► 严禁 new / delete 动态堆申请 (必须使用固定长度缓冲与预分配空间)
      └──► 严禁 QPixmap 动态缩放 (必须在 L1/L2 缓存落盘时即完成目标像素裁剪)
```

1. **UI 渲染热路径性能红线（Hot Path Constraints）**：
   * 在视图 Delegate 的 `paint` 逻辑或 TableModel 的 `data` 逻辑中：
     * **严禁** 发生任何堆分配。
     * **严禁** 临时执行 `QString::fromUtf8` 或通过 `std::string` 物理拷贝构造。
     * **应当** 使用 `std::wstring_view` 或 `std::string_view` 进行前缀/后缀匹配；字符串拼接必须预先使用 `reserve` 预分配内存，降低堆内存抖动（Heap Thrashing）。

2. **只读零复制规则（Zero-Copy Rule）**：
   * 在 Core 引擎的查询和数据传输中，**严禁**克隆大型数据集实体。任何结果集传递必须采用 C++ 11 右值移动语义 `std::move`，或者直接传递 `std::shared_ptr`，将数据装配和对齐耗时锁死在常数时间 $O(1)$。

3. **API 一致性与 Role 规约（Contract Standard）**：
   * **必须** 物理隔离全应用的 `CommonRole` 定义。
   * 自定义 Model 返回的所有自定义 Role（UserRole 及以上）**必须**在 `ModelContract.h` 进行全局唯一注册和声明，严禁在各组件、各类 Delegate 中私自硬编码 Role 偏移，杜绝潜在的跨视图碰撞与解析失效风险。