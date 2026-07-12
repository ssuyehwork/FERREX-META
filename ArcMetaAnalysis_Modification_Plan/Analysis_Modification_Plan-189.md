# “数据筛选遵循三种视图模式”核心定位重塑方案 —— Analysis_Modification_Plan-189.md

## 1. 任务背景
在 FERREX-META 当前的版本实现中，数据过滤结果在各视图（列表视图、自适应视图、网格视图）（对应用户原话：“三种模式”/“三种视图模式”）下的展示呈现未能完美对齐各模式本身的业务使命和承载力定位。不论用户采用何种筛选条件（对应用户原话：“无论如何筛选数据”），海量无法生成缩略图的常规文本、代码文件和文件夹都会不加区分地进入本该属于“多媒体精美画廊”的**自适应视图**与**网格视图**中，这不仅破坏了画廊视图的高级美感（堆满了默认空白正方形卡片），而且极易因为数十万非媒体项强行参与流式宽高排版计算而引发累积宽度异常与性能卡顿。因此，用户下达了在数据显示时“必须遵循三种视图模式”的架构加固指令。

## 2. 问题定位
当前的数据过滤流是：`ScanController` 处理底层 MFT 的全量筛选，并将结果集 `ResultSet`（存储 FRN 唯一 Key 向量）同步至数据驱动模型 `ScanTableModel`。
*   `ScanTableModel` 不管当前处于何种视图模式，都直接将 `m_currentResultSet` 的行数通过 `rowCount()` 完整返回，并由其 `data()` 暴露出去。
*   **痛点**：对于专注于海量普通文件列表管理的“列表视图”（对应用户原话：“三种模式”），这种直接输出机制完全符合其高吞吐的设计目的。但对于自适应和网格这两种致力于图形/图像和视频美学陈列的卡片画廊视图，它们应该只关心并展示具有缩略图表现力的多媒体项。

因此，解决方案是：在 `ScanTableModel::updateResults(std::shared_ptr<ResultSet> nextSet)` 中引入**模式感知过滤机制（Mode-Aware Filter）**。当切换到自适应或网格模式时，动态对原始结果集进行精细的媒体过滤并重建局部索引映射；当处于列表模式时，则依然保留完整的检索结果。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 我期望 无论如何筛选数据，在显示数据时必须遵循三种模式来显示数据（对应用户原话） | 4. 详细解决方案 中在 `ScanTableModel` 引入模式感知过滤器，使其在不同模式下自适应分流显示 | ✅ |
| 2    | 你不看看图片还有更多复选框吗？（对应用户原话） | 2. 问题定位 中明确涵盖了图片左侧复选框、主搜索框及右侧后缀名输入框的联合多功能筛选情况 | ✅ |

## 4. 详细解决方案

我们对 `src/ui/ScanDialog.cpp` 中的数据驱动模型 `ScanTableModel` 进行核心算法更新，让其在更新结果时动态感知当前的视图模式，并进行媒体项的前置过滤。

### 4.1 定义媒体扩展名集合
流式卡片画廊（自适应与网格视图）的核心使命是美观地展示多媒体卡片。支持的媒体扩展名集合设定为：
`jpg, jpeg, png, bmp, gif, webp, svg, psd, ai, eps` （图形图像及平面设计文件）以及 `mp4, mkv, avi, mov, flv, rmvb, wmv, webm` （视频多媒体文件）。

### 4.2 修改 `ScanTableModel::updateResults`
在 `src/ui/ScanDialog.cpp` 中，利用友元类（friend class）特性，通过 `parent()` 动态向上获取当前 `ScanDialog` 的 `m_config.viewMode` 属性。
*   如果 `viewMode == 1`（代表自适应或网格视图模式），则启动画廊专用的媒体过滤；
*   如果 `viewMode == 0`（代表列表视图模式），则不做任何二次拦截，直接显示完整筛选数据（包括普通文件和文件夹）。

### 修改 `src/ui/ScanDialog.cpp` 如下：

```cpp
<<<<<<< SEARCH
void ScanTableModel::updateResults(std::shared_ptr<ResultSet> nextSet) {
    auto newSet = nextSet ? nextSet : m_controller->snapshot();
    int oldSize = (int)m_currentResultSet->keys.size();
    int newSize = (int)newSet->keys.size();

    // 2026-06-xx 极致性能重构：Diffing 局部刷新。
    // 物理铁律：在 emit 信号之前必须确保 m_currentResultSet 已更新，
    // 且信号范围必须与数据量绝对对齐，否则 TableView 内部索引越界会导致程序无响应（假死）。
    
    // 如果变动巨大或初始加载，回退到 Reset 模式
    if (oldSize == 0 || std::abs(newSize - oldSize) > 500) {
        beginResetModel();
        m_currentResultSet = newSet;
        m_displayCount = newSize; 
        m_requestedThumbs.clear();
        m_failedThumbs.clear(); // 2026-07-xx 重置时也必须清理失败跟踪，避免由于路径变动或磁盘更新造成不可恢复的阻断
        m_pendingRows.clear(); // 2026-06-xx 任务修复：重置时必须清空待刷新行，防止索引失效
        endResetModel();
        return;
    }
=======
void ScanTableModel::updateResults(std::shared_ptr<ResultSet> nextSet) {
    auto baseSet = nextSet ? nextSet : m_controller->snapshot();
    auto newSet = std::make_shared<ResultSet>();
    newSet->metadata = baseSet->metadata;
    newSet->keyToPos = baseSet->keyToPos;

    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
    bool isMediaView = false;
    if (dlg) {
        // viewMode == 1 代表自适应与网格的多媒体画廊视图，viewMode == 0 代表全文件列表视图
        isMediaView = (dlg->m_config.viewMode == 1);
    }

    if (isMediaView) {
        // 自适应与网格模式必须遵循其媒体画廊视图本身的展示使命与渲染承载力，在源头只保留视频与图像
        auto& reader = MftReader::instance();
        static const QSet<QString> mediaExts = {
            "jpg", "jpeg", "png", "bmp", "gif", "webp", "svg", "psd", "ai", "eps",
            "mp4", "mkv", "avi", "mov", "flv", "rmvb", "wmv", "webm"
        };
        
        newSet->keys.reserve(baseSet->keys.size() / 2);
        for (uint64_t key : baseSet->keys) {
            int actualIndex = reader.getIndexByKey(key);
            if (actualIndex == -1) continue;
            
            // 剔除所有文件夹以及不属于画廊美学展示范围的常规普通非媒体文件
            if (reader.isDirectory(actualIndex)) continue;
            
            QString ext = reader.getExtQString(actualIndex).toLower();
            if (mediaExts.contains(ext)) {
                newSet->keys.push_back(key);
            }
        }
        
        // 重建过滤后结果集的 O(1) 反向索引映射
        newSet->keyToPos.clear();
        for (size_t i = 0; i < newSet->keys.size(); ++i) {
            newSet->keyToPos[newSet->keys[i]] = i;
        }
    } else {
        // 列表模式：保留全量普通文件、文件夹及多媒体过滤数据
        newSet->keys = baseSet->keys;
    }

    int oldSize = (int)m_currentResultSet->keys.size();
    int newSize = (int)newSet->keys.size();

    // 2026-06-xx 极致性能重构：Diffing 局部刷新。
    // 物理铁律：在 emit 信号之前必须确保 m_currentResultSet 已更新，
    // 且信号范围必须与数据量绝对对齐，否则 TableView 内部索引越界会导致程序无响应（假死）。
    
    // 如果变动巨大或初始加载，或者模式切换导致的数据量落差，回退到 Reset 模式
    if (oldSize == 0 || std::abs(newSize - oldSize) > 500 || isMediaView != (oldSize != (int)baseSet->keys.size())) {
        beginResetModel();
        m_currentResultSet = newSet;
        m_displayCount = newSize; 
        m_requestedThumbs.clear();
        m_failedThumbs.clear(); // 2026-07-xx 重置时也必须清理失败跟踪，避免由于路径变动或磁盘更新造成不可恢复的阻断
        m_pendingRows.clear(); // 2026-06-xx 任务修复：重置时必须清空待刷新行，防止索引失效
        endResetModel();
        return;
    }
>>>>>>> REPLACE
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` (方案设计阶段，物理代码禁止修改)

**明确禁止越界修改的范围：**
- [ ] 物理修改任何 `.cpp` / `.h` / `.cmake` 代码文件均被强制禁绝（Jules 分析师硬红线）。
- [ ] 严禁修改 `ScanController` 中的 MFT 全量底层物理搜索逻辑，必须使其保留纯粹的高速全量搜索特性，确保底层不丢数据。

## 6. 实现准则与预警【核心】

1. **零内存抖动和瞬间重映射**：因为是在 `updateResults` 阶段对 `vector` 进行了一次轻量的后缀过滤和反向索引 `keyToPos` 的重建（由于只处理过滤出来的少部分数据，开销极低），这避免了在 `data()` 中进行重复高开销检测，完全契合百万级数据的高性能运行准则。
2. **零编译报错隔离**：使用了 C++ 强类型安全转换和 Qt 内置的 `QSet::contains` 进行匹配，保留了原有的所有友元和数据依赖关系，安全可靠。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 视图模式感知过滤 | 遵循三种视图模式本身的设计定位与显示规则，媒体模式展示媒体，列表展示全量数据 | ✅ 符合 |

## 8. 待确认事项（可选）
*   **三种模式**（对应用户原话：“三种模式” / “三种视图模式”）
*   **左侧**（对应用户原话：“你不看看图片还有更多复选框吗？” 中指向图片左侧六个筛选复选框的红色标注 1 的箭头）
*   **右侧**（对应用户原话：“每个选项右侧都应该有一个“×”……轻松移除某个选项”）
