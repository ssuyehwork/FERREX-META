# 缩略图管线彻底去毒与磁盘感知性能优化 —— Analysis_Modification_Plan-153.md

## 1. 任务背景
在处理含有大量重型预览文件（如 PSD、RAW、高分辨率图片）的库时，用户反馈界面会出现明显的卡顿甚至假死。经审计，当前的缩略图加载管线存在严重的架构缺陷：滥用了 `QtConcurrent::blockingMap` 导致全局线程池被 I/O 密集型任务占满（线程饥饿），且未针对不同磁盘介质（HDD/SSD）进行并发控制。

## 2. 问题定位
- **核心死锁原因**：`src/ui/ScanDialog.cpp` 中虽使用了 `m_thumbPool`，但内部嵌套的 `QtConcurrent::blockingMap` 会强制请求 `QThreadPool::globalInstance()`。当缩略图请求量巨大时，全局池被耗尽，导致主线程发出的搜索、排序、UI 刷新等高优先级任务在队列中排队，产生“假死”感。
- **并发策略失误**：缩略图生成属于 I/O 与 CPU 混合型任务。对于 HDD，多线程并发会导致磁头剧烈寻道，性能反而下降；对于 SSD，单线程则无法榨干带宽。当前固定并发数无法适配用户物理环境。
- **COM 初始化隐患**：`getShellThumbnail` 依赖 Windows Shell 接口，若工作线程未正确调用 `CoInitializeEx`，会导致接口调用缓慢甚至失败。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 彻底去毒，解决 UI 卡顿 | 移除内层 `blockingMap`，改用专用池调度，消除全局线程饥饿 | ✅ |
| 2    | 磁盘类型感知并发控制 | 实现 `isSolidStateDrive` 检测，HDD 限制并发 1，SSD 4-8 | ✅ |
| 3    | 优化 Shell 缩略图获取 | 在 `getShellThumbnail` 调用处加固 `ScopedComInit` 管理 | ✅ |

## 4. 详细解决方案

### 4.1 物理层：磁盘介质类型探测
在 `src/util/ShellHelper.h` 中新增磁盘特征识别逻辑：
- 使用 `CreateFile` 打开驱动器句柄（如 `\\.\C:`）。
- 发送 `IOCTL_STORAGE_QUERY_PROPERTY` 并检查 `StorageDeviceSeekPenaltyProperty`。
- `Sizeless` 属性为 false 则判定为 SSD。

### 4.2 架构层：专用线程池动态重构
- **取消内层嵌套（修正）**：在 `ScanDialog::onThumbTaskTimer` 中，彻底弃用 `QtConcurrent::blockingMap`（因其强制请求全局池）。改为遍历任务队列，通过 `m_thumbPool->start(runnable)` 手动分发任务，实现 100% 的线程池物理隔离。
- **动态池容量调整**：
  - 在 `ScanDialog` 初始化或扫描开始时，调用 `ShellHelper::isSolidStateDrive` 检测库所在盘符。
  - 若为 HDD：`m_thumbPool->setMaxThreadCount(1)`（防止磁头剧烈寻道导致 I/O 崩溃）。
  - 若为 SSD：`m_thumbPool->setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 4, 8))`。

### 4.3 渲染层：极致性能加固
- **COM 线程存储（优化）**：在工作线程内部，通过 `QThreadStorage<FERREX::ScopedComInit>` 确保每个工作线程仅在生命周期开始时初始化一次 COM 环境，消除每张缩略图重复初始化的 CPU 损耗。
- **LIFO 优先级调度**：根据 `ScanTableModel` 当前滚动位置，对 `m_thumbTaskQueue` 进行反向排序，确保“当前可见项”排在队列最前端优先处理。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] `src/util/ShellHelper.h`: 新增磁盘检测逻辑，引入 `<winioctl.h>`。
- [ ] `src/ui/UiHelper.h`: 优化 `ScopedComInit` 管理逻辑，引入 `<QThreadStorage>`。
- [ ] `src/ui/ScanDialog.cpp`: 重构调度逻辑，引入 `<algorithm>` 与 `<numeric>`。

**明确禁止越界修改的范围：**
- [ ] 严禁修改 MFT 索引结构 `IndexedEntry`。
- [ ] 严禁修改 `MetadataManager` 的数据库持久化队列。

## 6. 实现准则与预警【核心】

1.  **头文件依赖**：
    - `ShellHelper.h`: `<windows.h>`, `<winioctl.h>`。
    - `ScanDialog.cpp`: `<algorithm>` (`std::clamp`, `std::sort`), `<numeric>` (`std::iota`)。
2.  **线程安全**：`m_thumbPool` 的 `setMaxThreadCount` 操作必须在任务分发前执行。
3.  **IOCTL 权限**：`CreateFile` 探测驱动器时使用 `FILE_SHARE_READ | FILE_SHARE_WRITE`，若失败则保守回退至 2 线程模式。
4.  **代码考古**：参考 `MftReader.cpp` 中对 `DeviceIoControl` 的使用规范。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 批量更新性能红线 | 必须使用 QtConcurrent::run 异步执行 | ✅ 符合 |
| 锁竞争审计结论 | 严禁在锁内包含磁盘 I/O | ✅ 符合（缩略图 I/O 已全量移至锁外专用池） |
| 极致性能重构方案 | 通过 matchEntry 零分配优化 | ✅ 符合（不影响现有筛选逻辑） |

## 8. 待确认事项
1.  **权限处理**：若用户以非管理员权限运行，`IOCTL_STORAGE_QUERY_PROPERTY` 可能会失败。是否接受在这种情况下退回到默认 2 线程的保守策略？（目前方案暂定：失败则视为 HDD 或低速 SSD）。
