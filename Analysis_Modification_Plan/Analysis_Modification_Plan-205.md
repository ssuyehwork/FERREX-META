# 高并发磁盘检索引擎与 I/O 过滤架构重构 —— Analysis_Modification_Plan-205.md

## 1. 任务背景
在 FERREX-META 处理千万级海量文件的实时磁盘索引与高并发搜索场景下，极高频的并发读取（如极速打字搜索变焦）与底层的 USN 日志变化拦截模块面临极其严峻的 CPU 互锁空转及性能抖动瓶颈：
1. **高并发读锁排队瓶颈**：在 `MemoryQueryEngine::search` 中，重型读锁被错误置于 `QtConcurrent::blockingMap` 多线程大循环闭包内部。导致在极速变焦或高并发查询时，数十个物理线程在每次迭代中频繁申请/释放读锁，引发了严重的伪随机线程互锁竞争与长耗时排队死等，无法实现真正的多线程并行无锁检索。
2. **USN 拦截模糊检索空转瓶颈**：在磁盘 USN 高频文件变更拦截器 `UsnWatcher::handleRecord` 中，原逻辑采用 `std::transform` 进行宽字符小写转换，且频繁使用 `.find()` 进行繁重的全模糊检索。在高频写 I/O（如编译中或数据库写入）场景下，全模糊遍历严重拖累了拦截速度，导致 CPU 无效空转，难以保障高性能过滤。

本任务旨在实施高并发磁盘检索引擎的并发锁结构重塑，并完成 USN 拦截器的无拷贝前尾缀秒级过滤优化，彻底斩断这两个底层高并发性能隐患。

## 2. 方案 A：高并发检索读锁外置（MemoryQueryEngine.cpp）
在 `src/mft/MemoryQueryEngine.cpp` 中，将重型读锁 `reader->m_dataLock` 从多线程并行闭包 `QtConcurrent::blockingMap` 的内部彻底提取出来，一次性置于其外部，从而实现子线程在并发映射计算期间的零锁、无争抢自由检索：
- **修改模块**：`src/mft/MemoryQueryEngine.cpp`
- **实现逻辑**：
  在 `QtConcurrent::blockingMap` 启动前获取共享读锁：
  ```cpp
  QReadLocker locker(&reader->m_dataLock);
  ```
  在 Map 闭包内，移除任何读锁申请，各工作线程安全、无锁并发读取已有共享数据块。

## 3. 方案 B：无拷贝静态前/尾缀秒级过滤（UsnWatcher.cpp）
在 `src/mft/UsnWatcher.cpp` 中，建立无拷贝、高效率的静态前/尾缀快速匹配逻辑，完全平替原有的低效 `.find()` 模糊遍历检索：
- **修改模块**：`src/mft/UsnWatcher.cpp`
- **实现逻辑**：
  引入静态不常变的忽略后缀列表 `static_ignored_suffixes` 与前缀列表 `static_ignored_prefixes`。使用 `compare` 函数直接在大容量宽字符内存尾部和头部进行无拷贝比对，规避了全字符串无意义扫描。

---

## 4. 修改边界声明【红线】
- **允许修改文件**：`src/mft/MemoryQueryEngine.cpp`, `src/mft/UsnWatcher.cpp`
- **明确禁止修改范围**：严禁修改除了上述重构点外的任何数据库持久化、UI 展示或非高并发搜索核心逻辑。
