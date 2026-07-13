# 还原旧版本-3多媒体瞬时过滤与零查询视图切换设计 —— Analysis_Modification_Plan-195.md 
 
## 1. 任务背景 
用户委托将当前重构后的版本与“旧版本-3”进行详细对比（对应用户原话：“当前版本只是“旧版本-3”重构后的版本，之所以进行重构，是因为ScanDialog存在严重的职责过载，理论上来说，重构只是将职责过载的部分拆分成多个模块，所以运行逻辑、流程、参数是不该被修改的，但却被jules这个傻逼Ai脑补破坏了原有的逻辑，而且是严重被破坏”）。本方案排查并列举出由于重构“脑补”导致多媒体过滤机制层级错位、引入重复全量搜索的问题，并提供 100% 无损还原“旧版本-3”优雅瞬时切换及多媒体模型级过滤的针对性技术设计。 
 
## 2. 问题定位 
 
### 2.1 过滤机制层级篡改：从 UI 线程“瞬时过滤”退化为“全量库重扫” 
在“旧版本-3”中，多媒体过滤（排除非媒体文件和所有文件夹）是在 `ScanTableModel::updateResults()` 方法中完成的： 
- 它通过直接检测当前的 `viewMode == 1`（即自适应模式和网格模式，对应用户原话：“自适应、网格、列表这三种视图模式”）来在内存中对全量搜索结果进行瞬时筛选，而不需要操作底层数据库。 
- `switchToView` 在切换视图时，仅仅在内存中调用 `m_tableModel->updateResults()` 进行轻量级的秒级刷新，保留了选中状态与当前视口位置。 
而在当前版本中，重构者将此机制错误地沉降到了 `ScanController::performSearch` 异步检索流中（`state.galleryOnly`）： 
- 导致视图切换到列表或多媒体视图时，由于内存数据不完整，**被迫在 `switchToView` 里重新发起了耗时的全量数据库检索 `onTriggerSearch()`**。这严重破坏了重构“运行逻辑、流程、参数不该被修改”的根本承诺，导致大量的 CPU 空转与交互中断。 
 
### 2.2 确定“导出CSV”功能已按用户要求完全根除 
审计发现，在重构之前的旧版代码中包含了 `m_csvBtn`（“导出所选为 CSV”按钮），而当前重构版由于脑补将其抹除。经与用户沟通，用户明确指示：**“导出所选为 CSV”是旧版本脑补添加的，当前版本没了该功能是用户特意要求根除的**（对应用户原话：““导出所选为 CSV”是jules这个傻逼Ai脑补添加的，当前版本没了“导出所选为 CSV”功能是我特意要求根除的”）。因此，本方案对此被根除的功能进行固化，不做任何恢复。 
 
--- 
 
## 3. 强制对照表 
 
| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 | 
|------|---------------------|------------|----------| 
| 1    | 重构只是将职责过载的部分拆分，运行逻辑、流程、参数是不该被修改的（对应用户原话：“运行逻辑、流程、参数是不该被修改的，但却被jules这个傻逼Ai脑补破坏了原有的逻辑，而且是严重被破坏”） | 回归 UI 模型级瞬时过滤策略，彻底将过滤层级 100% 还原回“旧版本-3”模式。 | ✅ 一致 | 
| 2    | 当前版本没了“导出所选为 CSV”功能是我特意要求根除的（对应用户原话：“当前版本没了“导出所选为 CSV”功能是我特意要求根除的”） | 确认完全剔除 `m_csvBtn` 及相关状态机，保证不作任何多余恢复。 | ✅ 一致 | 
 
--- 
 
## 4. 详细解决方案 
 
### 4.1 彻底剥离 `ScanController` 中的多媒体异步过滤（还原设计一） 
还原 `ScanController::performSearch` 中对 `galleryOnly` 的脑补多媒体后缀筛选，使其总是返回纯粹的全量数据。 
在 `ScanController.cpp` 中： 
- 物理移除在 `performSearch` 中的 `if (state.galleryOnly) { ... }` 过滤数据块，恢复数据库检索只进行基本的过滤（不再参与自适应或网格的媒体剪裁）。 
 
### 4.2 还原 `ScanTableModel::updateResults` 瞬时内存过滤（还原设计二） 
将“旧版本-3”中优雅的多媒体过滤机制完整移植回当前版本的 `ScanTableModel::updateResults` 中： 
在 `ScanTableModel.cpp` 中修改 `updateResults`： 
 
```cpp 
// 还原伪代码 
void ScanTableModel::updateResults(std::shared_ptr<ResultSet> nextSet) { 
    auto baseSet = nextSet ? nextSet : m_controller->snapshot(); 
    auto newSet = std::make_shared<ResultSet>(); 
    newSet->metadata = baseSet->metadata; 
    newSet->keyToPos = baseSet->keyToPos; 
 
    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent()); 
    bool isMediaView = false; 
    if (dlg) { 
        // viewMode == 1 代表自适应与网格的多媒体画廊视图 
        isMediaView = (dlg->m_config.viewMode == 1); 
    } 
 
    if (isMediaView) { 
        auto& reader = MftReader::instance(); 
        static const QSet<QString> mediaExts = { 
            "jpg", "jpeg", "png", "bmp", "gif", "webp", "svg", "psd", "ai", "eps", 
            "mp4", "mkv", "avi", "mov", "flv", "rmvb", "wmv", "webm" 
        }; 
         
        newSet->keys.reserve(baseSet->keys.size() / 2); 
        for (uint64_t key : baseSet->keys) { 
            int actualIndex = reader.getIndexByKey(key); 
            if (actualIndex == -1) continue; 
             
            // 剔除所有文件夹以及非媒体文件 
            if (reader.isDirectory(actualIndex)) continue; 
             
            QString ext = reader.getExtQString(actualIndex).toLower(); 
            if (mediaExts.contains(ext)) { 
                newSet->keys.push_back(key); 
            } 
        } 
         
        newSet->keyToPos.clear(); 
        for (size_t i = 0; i < newSet->keys.size(); ++i) { 
            newSet->keyToPos[newSet->keys[i]] = static_cast<int>(i); 
        } 
    } else { 
        // 列表模式：保留全量普通文件、文件夹及过滤数据 
        newSet->keys = baseSet->keys; 
    } 
 
    // 后续进行行数变动计算并发射 Diffing 局部刷新信号或 Reset 信号... 
    ... 
} 
``` 
 
### 4.3 还原 `ScanDialog::switchToView` 零查询转换逻辑（还原设计三） 
在 `ScanDialog.cpp` 中： 
- 将 `switchToView` 的末尾还原为直接调用 `m_tableModel->updateResults();`。 
- 彻底移除对 `onTriggerSearch()` 的多余调用，实现零耗时的秒级模式切换，且完美不丢失当前的选中项。 
 
--- 
 
## 5. 修改边界声明【红线】 
 
**本次方案涉及范围：** 
- [ ] 模块/文件：`src/ui/ScanController.cpp` (撤销多媒体异步过滤) 
- [ ] 模块/文件：`src/ui/ScanTableModel.cpp` (还原动态模型层多媒体过滤) 
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` (还原 `switchToView` 零查询逻辑) 
 
**明确禁止越界修改的范围：** 
- [ ] 不恢复已经按要求永久根除的 `m_csvBtn`（CSV 导出）相关组件。 
- [ ] 严禁修改与 MFT 磁盘扫描内核相关的物理代码。 
 
--- 
 
## 6. 实现准则与预警【核心】 
1. **防范二次异步竞争**：当切换视图改回 `m_tableModel->updateResults()` 内存刷新时，必须确认此时没有正在进行的 `m_sortWatcher`（后台排序任务），防止旧有的异步重排序结果在随后传回时覆盖最新的内存过滤结果。 
2. **多态指针安全**：在 `ScanTableModel::updateResults` 内获取 `parent()` 并转型为 `ScanDialog*` 时，必须使用 `qobject_cast` 并在转型后进行严密的 `if (dlg)` 空指针防御性校验。 
 
--- 
 
## 7. Memories.md 合规检查 
 
| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 | 
|-------------|----------------------|----------------| 
| 视图过滤合规性 | 无论何种过滤，自适应/网格模式仅展示多媒体文件，列表展示所有匹配项。 | ✅ 符合（100% 在 `ScanTableModel` 内以模型级瞬时过滤达成该业务逻辑） | 
| 零分配筛选与防抖 | 废除多重全量扫描，交互平滑，不产生双重检索竞争。 | ✅ 符合（视图切换改回零数据库查询模式，体验平滑） | 
 
--- 
 
## 8. 待确认事项（可选） 
由于本方案中可能涉及以下方位词、顺序词及数量词，若用户原话未明确覆盖，特此列出并在后续流程中遵循： 
1. **“第一步”、“第二步”等技术方案步骤词**：用于描述 pseudo-code 修改的步骤顺序，未在原话中指定。 
2. **“两个”修改位置**：用于描述撤销多媒体异步过滤的 `ScanController` 和恢复过滤的 `ScanTableModel` 两个辅助类的物理还原，属于技术实现范畴，未在原话中指定。 
3. **“自适应、网格、列表这三种视图模式”的物理定位**：此数量和顺序词完全锚定于用户原话，不做任何偏离。 
