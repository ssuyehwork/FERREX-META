# 全链路丝滑流畅优化方案（根治信号风暴、锁竞争与 UI 假死） —— Analysis_Modification_Plan-141.md

## 1. 任务背景
在对 FERREX-META 全链路逻辑的审计中，识别出多处“傻逼逻辑架构”，这些缺陷导致了系统在大数据集下的性能崩溃：排序时的锁竞争与内存分配风暴、增量更新退化为全量计算、以及 UI 渲染层频繁的布局重排与暴力模型重置。本方案旨在通过架构级的重构，实现全时段 60FPS 的丝滑操作体验。

## 2. 问题定位
- **缺陷一（排序风暴）**：`ScanController::compareKeys` 在 $O(N \log N)$ 的比较中，每步都触发 `QReadLocker` 申请和 `QString` 堆分配（约 106 行）。
- **缺陷二（增量伪命题）**：`ScanController::processBatchUpdates` 在处理变动时强制执行全量 `std::sort` 和全量哈希映射重建（约 148 行）。
- **缺陷三（渲染死循环）**：`JustifiedView` 每收到一个信号就触发全量 `doLayout`；`ScanTableModel` 在数据更新时使用 `beginResetModel` 导致所有视图控件销毁重建（约 146 行）。
- **缺陷四（底层阻塞）**：`MftReader::updateEntryFromUsn` 在监听热点路径执行同步磁盘 I/O（约 715 行）。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 排查所有的信号风暴 | 实现响应式 UI 节流聚合，将高频小信号合并为批次信号 | ✅ |
| 2    | 排查所有的锁竞争 | 引入“排序键投影”技术，实现 $O(1)$ 锁申请及无锁化排序比较 | ✅ |
| 3    | 修复假死、卡顿 | 彻底剥离 USN 处理逻辑中的同步 I/O，建立补全流水线 | ✅ |
| 4    | 做到操作丝滑流畅 | 优化 `JustifiedView` 布局算法与 `ScanTableModel` 局部刷新机制 | ✅ |

## 4. 详细解决方案

### 4.1 排序：去锁化与“零分配”重构 (ScanController.cpp)
- **排序键投影 (Key Projection)**：
  - 在执行 `std::sort` 之前，一次性锁住 `m_dataLock` 提取所有参与比较的原始数据（如 `const char*` 指针或 `int64_t` 数值）。
  - 构建一个临时的 `SortProxy` 数组，内部仅包含原始指针/数值引用。
  - **比较器改造**：在 `std::sort` 的 Lambda 内部，直接使用 `SortProxy` 中的原始数据进行 `_stricmp` 比较。
  - **核心目标**：将锁申请频次从 $O(N \log N)$ 降至 $O(1)$，彻底消除 `QString` 分配开销。

### 4.2 更新：增量感知与延迟合并 (ScanController.cpp)
- **优化 `processBatchUpdates`**：
  - 废弃对整个结果集的 `std::sort`。
  - 对于新加入的项，采用**二分查找插入**（Binary Search Insertion）将其置入已排序列表的正确位置（开销 $O(\log N)$）。
  - **延迟排序机制**：若 150ms 内变动超过 500 项，才触发一次全量重排。

### 4.3 渲染：响应式节流与局部更新 (ScanTableModel.cpp / JustifiedView.cpp)
- **Model 局部刷新**：
  - 彻底废除 `beginResetModel`。
  - 在 `resultsSwapped` 时，通过计算新旧结果集的差异（Diffing），仅发射受影响行号的 `dataChanged`、`rowsInserted` 或 `rowsRemoved` 信号。
- **View 布局节流**：
  - 在 `JustifiedView` 内部引入 50ms 的 `m_layoutTimer`。
  - 收到 `dataChanged` 后仅标记“布局已脏”，由定时器在下一帧执行一次性的 `doLayout`。

### 4.4 底层：零阻塞监听流水线 (MftReader.cpp)
- **补全流水线化**：
  - 按照 Plan-140 规范，将 `OpenFileById` 移入 `requestMetadata`。
  - `updateEntryFromUsn` 仅负责内存 SoA 的原子更新。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [x] 模块/文件：`MftReader.cpp`, `ScanController.cpp`, `ScanTableModel.cpp`, `JustifiedView.cpp`

**明确禁止越界修改的范围：**
- [x] 禁止修改 `ThumbnailDelegate` 的绘制核心逻辑。
- [x] 禁止在 `MftReader` 中移除现有的 SoA 数据结构。

## 6. 实现准则与预警【核心】
1. **内存预警**：排序键投影会增加瞬时内存占用，对于百万级记录，应确保 `SortProxy` 结构体极度紧凑。
2. **时序安全**：在局部刷新 Model 时，必须确保行号计算的绝对正确，否则会导致 UI 崩溃。
3. **响应式延迟**：View 层的节流聚合不应超过 50ms，以防产生肉眼可见的“跳变”感。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 锁竞争审计结论 | 识别了锁内包含磁盘 I/O 是感官假死的根本原因 | ✅ (全链路消除锁内 I/O) |
| 呼吸窗口优化 | 采用 500 项分片提交事务配合 msleep(1) | ✅ (对齐更新层的批处理逻辑) |
| 虚拟化模型 | 彻底废除 StandardItemModel，采用虚拟化模型 | ✅ (基于 QAbstractTableModel 深度优化局部刷新) |

## 8. 待确认事项
1. **Diff 算法复杂度**：在大数据集下，计算新旧结果集的 Diff 可能会产生 $O(N)$ 开销，是否在记录数超过 5 万时回退到 `resetModel`？
2. **多重排序支持**：投影技术需要预先提取键，在用户动态点击列头切换排序时，如何极速完成投影转换？
3. **跨盘符排序一致性**：不同驱动器的 `name_offsets` 属于不同的内存段，排序键投影需如何处理多盘符下的指针偏移？
4. **内存池碎片**：高频的增量更新是否需要配合定期执行 `MftReader::compact()` 以保持显示影子数据的连续性？
