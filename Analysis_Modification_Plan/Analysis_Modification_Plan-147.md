# UI 持续闪烁与显示延迟专项修复方案 —— Analysis_Modification_Plan-147.md

## 1. 任务背景
用户反馈在 221 万数据量下，界面出现剧烈闪烁，且“即打即搜”功能失效，必须点击搜索按钮才有结果。经审计，这是由于渲染管线存在信号共振以及异步任务管理时序冲突导致的。

## 2. 问题定位
- **瓶颈 A：信号共振导致的重复 Reset**
  - `ScanController` 同时发出 `searchFinished` 和 `resultsSwapped` 信号。
  - `ScanDialog` 和 `ScanTableModel` 分别监听这两个信号并各自调用 `updateResults()`。
  - **后果**：单次搜索触发了两次 `beginResetModel()`，造成肉眼可见的视觉闪烁。
- **瓶颈 B：异步任务过度 Cancellation**
  - 在快速输入时，`triggerSearch` 的高频防抖逻辑不断取消前一个任务。由于缺乏“搜索中”的中间态保持，界面在输入期间始终处于“空显示”或“白屏”。
- **瓶颈 C：Diff 算法门槛过低**
  - 现有的 `updateResults` 在变动超过 500 项时即放弃增量刷新，直接全量重置，这在处理百万级列表时是不合理的性能负担。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 数据一直闪烁 | 实施信号归一化，合并冗余刷新 | ✅ |
| 2    | 按下搜索释放才显示 | 修复异步任务时序，支持流畅的即打即搜 | ✅ |
| 3    | 极致性能原则 | 引入“视图锁定”与更智能的 Diffing | ✅ |

## 4. 详细解决方案

### 4.1 信号归一化与原子更新 (ScanController.cpp)
- **重构**：移除 `searchFinished` 信号中对数据处理的依赖，将其降级为纯 UI 状态（更新计时器）。
- **统一管线**：所有的 `ResultSet` 变更一律通过 `resultsSwapped(std::shared_ptr<ResultSet>)` 信号驱动，确保“一个结果集、一次通知、一次重绘”。

### 4.2 智能增量刷新算法 (ScanTableModel.cpp)
- **门槛调整**：将 Diffing 刷新的阈值从 500 项提高至 20,000 项。
- **视图锁定**：在 `updateResults` 过程中，通过记录 `m_displayCount` 避免无谓的 `beginInsertRows` 冲击，仅在实际滚动需要时拉取更多项（Lazy Fetching）。
- **零闪烁更新**：
```cpp
if (isIncrementalUpdate) {
    // 使用 dataChanged 替代 beginResetModel
    emit dataChanged(index(0, 0), index(rowCount() - 1, 3));
}
```

### 4.3 搜索触发逻辑加固 (ScanDialog.cpp)
- **修复即打即搜**：理顺 `onFilterOptionChanged` 与 `m_controller->triggerSearch()` 的调用链。
- **中间态保持**：在搜索进行中，保留旧的结果集显示，仅在状态栏显示“正在搜索...”。严禁在子线程未返回前清空 UI。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] `src/ui/ScanController.cpp`：信号下发逻辑。
- [ ] `src/ui/ScanTableModel.cpp`：刷新算法重构。
- [ ] `src/ui/ScanDialog.cpp`：搜索触发时序调整。

**明确禁止越界修改的范围：**
- [ ] `src/mft/MftReader.cpp`：禁止修改底层的搜索算法。

## 6. 实现准则与预警【核心】
- **锁管理预警**：在 `updateResults` 期间，必须通过 `shared_ptr` 确保旧结果集的生命周期，防止在 Diffing 比较过程中旧结果集被销毁引发崩溃。
- **UI 响应度**：确保在 221 万数据下，即使搜索未结束，列表依然可以滚动查看旧数据（Non-blocking View）。

## 7. Memories.md 合规检查
- **信号节流**：方案符合 `MetadataManager` 建立的信号聚合规约。
- **UI 考古**：增量刷新逻辑对标 `ScanTableModel` 的原始设计初衷。

## 8. 待确认事项
- **Diff 开销**：在 2 万项级别执行 `std::equal` 检查约耗时 1-2ms，对 UI 线程无压力。
