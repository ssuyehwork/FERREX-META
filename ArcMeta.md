# ArcMeta 性能优化技术白皮书

## 1. 现状背景与瓶颈分析
在对 ArcMeta 代码库进行深度排查后，确认系统在处理大规模文件（>1000个）及频繁交互时存在明显的性能滞后。主要瓶颈集中在 UI 渲染、数据加载、磁盘 IO 以及数据库事务四个维度。

### 1.1 UI 渲染黑洞 (Rendering Bottleneck)
*   **问题定位**：`UiHelper::getPixmap` 缺乏缓存机制，每次绘制图标都在实时解析 SVG 路径并进行颜色替换。
*   **影响**：列表滚动时，CPU 被大量的 XML 解析和光栅化计算占满，导致掉帧。

### 1.2 信号风暴与重算过载 (Data Flow Overload)
*   **问题定位**：`ContentPanel` 异步加载数据时，以高频小批次（100个/次）向主线程投递。
*   **影响**：每批次数据都会触发 `ProxyModel` 的全量排序 ($O(N \log N)$)，在 5000+ 文件下，主线程被连续的排序计算锁死。

### 1.3 冗余写操作 (Redundant IO)
*   **问题定位**：`MetadataManager::prefetchDirectory` 采用“访问即写”逻辑，进入文件夹即重写 `.am_meta.json`。
*   **影响**：产生大量无效磁盘写入，造成背景 IO 噪音并增加数据库队列压力。

### 1.4 数据库写锁竞争 (DB Locking)
*   **问题定位**：`SyncQueue` 使用单一超大事务处理所有同步路径。
*   **影响**：长时间持有写锁，导致主线程的交互式写入（如改星级）在 `busy_timeout` 中等待。

---

## 2. 详细重构解决方案

### 2.1 UI 渲染：引入像素级缓存池
**目标**：将图标渲染耗时降至微秒级。

*   **实现方案**：
    1.  在 `UiHelper` 增加 `static QCache<QString, QPixmap> s_renderCache;`。
    2.  **缓存键设计**：`key_color_width_height`（例：`"star_filled:#EF9F27:16_16"`）。
    3.  **重构逻辑**：
        ```cpp
        QPixmap UiHelper::getPixmap(const QString& key, const QSize& size, const QColor& color) {
            QString cKey = generateCacheKey(key, size, color);
            if (s_renderCache.contains(cKey)) return *s_renderCache.object(cKey);
            
            QPixmap pix = renderFromSvg(key, size, color); // 耗时操作
            s_renderCache.insert(cKey, new QPixmap(pix));
            return pix;
        }
        ```
    4.  **布局预计算**：将 `GridItemDelegate` 中的几何计算（Rects）移至 `zoomLevel` 改变时的单次计算流程中。
    5.  **文字加速**：对文件名使用 `QStaticText` 预布局，减少文本排版开销。

### 2.2 数据流：双重缓冲 60FPS 平滑消费
**目标**：消除加载过程中的“假死”白屏。

*   **实现方案**：
    1.  **生产者（后台线程）**：每 500 个条目或 200ms 投递一次数据块。
    2.  **消费者（主线程）**：
        *   建立 `std::deque<ScanItemData> m_uiPendingQueue` 接收数据。
        *   开启一个 **16ms (60FPS) 定时器**。
        *   定时器触发时，每帧仅向 Model 插入 50 个条目。
    3.  **排序节流**：加载任务期间调用 `m_proxyModel->setDynamicSortFilter(false)`，扫描结束后再执行单次全量排序。

#### 💡 深度技术解析：为什么需要计时器而非纯懒加载？
*   **懒加载的局限性**：懒加载主要解决的是“非可视区域不进行 IO（读取图标、读取内容）”和“内存占用”问题。但在 Qt 的 Model-View 架构中，即使不加载图标，将 10,000 条基础数据（文本、大小等）同步塞入 Model 的动作本身，也会触发上层 ProxyModel 的全量重算和界面重绘指令风暴。
*   **计时器的限流器作用**：计时器的核心意义是**“主线程负载分摊”**。通过规定每 16ms 只处理 50 条更新，我们确保了主线程在每一帧内只花不到 2ms 处理数据，剩下 14ms 的“帧预算”永远预留给用户的鼠标点击、滚动和交互反馈。
*   **二者配合策略**：使用**计时器**确保行条目平滑载入，不锁死 UI；配合**懒加载**确保滚动到视野内才触发 IO 提取图标。两者结合才能实现真正的“零卡顿”。

### 2.3 持久化：内容脏检查与异步合并
**目标**：减少 80% 的物理磁盘写入。

*   **实现方案**：
    1.  **脏检查 (Dirty Check)**：在 `AmMetaJson` 加载时记录内容 Hash。
    2.  **保存拦截**：`save()` 时先计算当前 Hash，若未变动则直接返回，禁止所有磁盘 IO 操作。
    3.  **延迟写入**：`MetadataManager` 的写入操作引入 2 秒 Debounce 时间，合并短时间内的连续修改。

### 2.4 数据库：微事务分片与读写分离
**目标**：保证用户交互写入的绝对优先级。

*   **实现方案**：
    1.  **事务分片**：`SyncQueue::processBatch` 改为每 50 条记录提交一次事务。
    2.  **让步逻辑**：事务提交后强制执行 `QThread::msleep(5)`，让出数据库写锁时间窗。
    3.  **双连接架构**：主线程使用 `ReadOnly` 连接显示数据，后台线程使用专门的写入连接。

---

## 3. 架构优化技术白皮书总结
本方案通过**“缓存渲染、平滑载入、拦截无效 IO、分片事务”**四大核心战术，旨在将 ArcMeta 的操作延迟降低至感知阈值（16ms）以下，确保在万级文件规模下依然保持“零卡顿”的丝滑体验。
