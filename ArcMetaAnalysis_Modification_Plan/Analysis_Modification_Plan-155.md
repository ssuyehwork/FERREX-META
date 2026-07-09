# 废除分页机制实现搜索结果全量加载 —— Analysis_Modification_Plan-155.md

## 1. 任务背景
当前 FERREX-META 的 `ScanTableModel` 采用了 100 行分段加载（Pagination/fetchMore）机制。用户明确要求废除此机制，实现“有多少加载多少”的全量显示模式，以提升百万级数据下的全局感知力。

## 2. 问题定位
- **模块**：`ScanTableModel` (位于 `src/ui/ScanDialog.cpp`)
- **函数**：`updateResults`, `canFetchMore`, `fetchMore`
- **根因**：现有代码在 `updateResults` 中人为将 `m_displayCount` 裁剪至 100，并依赖 `fetchMore` 逐步膨胀。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 我要的是全量加载 有多少加多少 (对应用户原话) | 废除 m_displayCount 的 100 行限制，rowCount 始终返回 ResultSet 总数 | ✅ |
| 2    | 实现搜索结果有多少加多少 (对应用户原话) | 移除 fetchMore 分页逻辑 | ✅ |

## 4. 详细解决方案

### 4.1 修改 ScanTableModel::updateResults
在执行 `beginResetModel` 后，不再使用 `std::min` 裁剪大小，而是直接赋值：
```cpp
// 修改前
m_displayCount = (std::min<int>)(newSize, 100);

// 修改后
m_displayCount = newSize;
```

### 4.2 废除 fetchMore 逻辑
- 将 `canFetchMore` 始终返回 `false`。
- 将 `fetchMore` 设为空函数，防止冗余的行插入信号触发。

### 4.3 渲染性能压榨 (视图投影复用)
由于全量显示会导致 TableView 滚动条瞬间感知到数百万行，滚动时的渲染压力剧增。必须确保 `ScanTableModel::data` 中已实现的行内缓存（`thread_local static`）逻辑保持稳健。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` 中的 `ScanTableModel` 类实现。

**明确禁止越界修改的范围：**
- [ ] 禁止修改 `MftReader.cpp` 中的底层扫描与搜索逻辑。
- [ ] 禁止修改 `ScanController.cpp` 中的异步搜索与排序流程。

## 6. 实现准则与预警【核心】
1. **头文件依赖**：方案依赖 `<algorithm>` 和 `ScanDialog.h`，当前代码已包含。
2. **UI 假死预警**：在 220 万量级下，`m_displayCount` 从 0 直接跳变至 2,000,000 会导致 `TableView` 的 `updateGeometries()` 被调用。如果此时主线程负载过高，可能出现 1-2 秒的“白屏”或“无响应”状态。
3. **滚动条抖动**：全量加载后，滚动条滑块会变得极小，建议用户在后续方案中考虑增加“虚拟滚动优化”或自定义滚动条步长。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 虚拟化架构 | 重构为基于 QAbstractTableModel 的虚拟化模型 | ✅ (本方案在虚拟化模型基础上实现全量映射) |
| 百万级数据秒开 | 实现百万级数据的秒开 | ✅ (数据映射为 O(1)，卡顿仅存在于 Qt 内部布局计算) |

## 8. 待确认事项
- **无**：用户已明确放弃“保留滚动位置”需求，本方案将维持 Qt 默认的滚动行为（即全量重置后回到顶部）。
