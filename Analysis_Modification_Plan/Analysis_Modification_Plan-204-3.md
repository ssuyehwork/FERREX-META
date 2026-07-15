# 全局架构专业化评估与解耦规划 —— Analysis_Modification_Plan-204.md

## 1. 任务背景
在对 FERREX-META 项目（即基于 C++/Qt 的高并发磁盘索引与卡片式视图管理系统）的物理源码进行深度审计后，发现当前在 `src/` 重构版中，由于早期缺乏严密的顶层分层规划，塞入了大量为绕过 Bug 而临时打上的“业余拼凑补丁”（临时解决方案）。这些代码在工业级千万文件、海量缩略图、滚轮快速变焦的负载下，留下了极大的并发、内存及析构安全隐患。

为向开发人员提供一份**绝对零歧义、零脑补空间、可由程序 or AI 直接执行精准 SEARCH / REPLACE 替换**的工程落地级重构规范，本方案对项目物理源码实施定点解耦重组规划。

---

## 2. 问题定位（物理源码诊断）

### 2.1 构造逻辑严重漏装与运行期动态 HACK 补丁
* **物理位置**：`src/ui/ScanTableModel.cpp` 构造函数与 `data()` 函数体第 170-173 行。
* **硬伤诊断**：
  `m_lastPixmapCache` 在 `ScanTableModel` 构造函数中未进行容量上限限制，反而是在高频渲染的 `ScanTableModel::data()` 内动态拦截配置，造成每次渲染一个图标都要做动态判断，设计极其业余。

### 2.2 线程生命周期野蛮规避与全局线程池滥用
* **物理位置**：`src/ui/ScanTableModel.cpp` 析构函数第 112-120 行。
* **硬伤诊断**：
  为绕过高频缩放中未执行完的线程死等问题，极其粗暴地采用全局线程池 `QThreadPool::globalInstance()` 进行局部线程池 `m_thumbPool` 的延迟异步销毁。在快速退出的高并发场景下极易触发段错误或野指针调用。

### 2.3 物理边界重度污染（模型层与高开销引擎锁、递归路径耦合）
* **物理位置**：`src/ui/ScanTableModel.cpp` 中的 `data()`。
* **硬伤诊断**：
  `data()` 在处理 `DisplayRole` 或 `ToolTipRole` 时，频繁通过 `reader.getFullPath(actualIndex)` 递归追溯节点，并疯狂申请 `m_dataLock` 锁，造成主线程与后台线程严重互锁卡顿。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 清晰分层：前端、业务逻辑、数据层职责分明 | 4.1 数据投影重组与 TableModel 适配 | ✅       |
| 2    | 低耦合高内聚：模块独立，接口明确 | 4.2 消除运行期动态 HACK 精准替换指令 | ✅       |
| 3    | 可维护性强：新成员能快速理解和修改 | 4.3 物理线程池安全析构重构替换指令 | ✅       |
| 4    | 可扩展性好：容易增加新功能 or 替换模块 | 4.4 抽象隔离数据引擎与视图表现层 | ✅       |
| 5    | 性能优化明确：调用链简洁，资源利用高效 | 4.5 零闪烁平滑变焦刷新控制 | ✅       |
| 6    | 团队协作顺畅：架构直观，沟通成本低 | 4.6 清理 Memories.md 垃圾规则，统一规范 | ✅       |

---

## 4. 详细解决方案 (精准 SEARCH / REPLACE 物理指令集)

### 4.1 数据投影解耦 (物理边界清晰分层)

重构 `src/ui/ScanController.h` 中的 `ResultSet` 数据结构，将前台渲染高开销数据彻底 SoA 投影隔离：

#### [物理重构指令 1] 针对 `src/ui/ScanController.h`
```cpp
<<<<<<< SEARCH
struct RenderMeta {
    QColor color;
    explicit RenderMeta(const QColor& c = QColor()) : color(c) {}
};

struct ResultSet {
    std::vector<uint64_t> keys;
    std::unordered_map<uint64_t, int> keyToPos;
    std::unordered_map<uint64_t, RenderMeta> metadata;
};
=======
struct ResultSet {
    std::vector<uint64_t> keys;
    std::unordered_map<uint64_t, int> keyToPos;
    
    // 工业级 SoA 数据投影：将路径、大小等在后台线程利用引擎短暂读锁一次性装配完毕
    // 彻底切断主线程 TableModel::data() 运行时对 MftReader 的高开销锁竞争与递归寻址
    std::vector<QString> cachedNames;
    std::vector<QString> cachedPaths;
    std::vector<int64_t> cachedSizes;
    std::vector<int64_t> cachedMtimes;
    std::vector<bool> isDirFlags;
};
>>>>>>> REPLACE
```

---

### 4.2 消除运行期动态 HACK

物理删除 `ScanTableModel::data()` 中的 HACK 分支，并将容量限制统一上提至 `ScanTableModel` 构造函数内集中分配。

#### [物理重构指令 2] 针对 `src/ui/ScanTableModel.cpp` 构造函数
```cpp
<<<<<<< SEARCH
    m_thumbCache.setMaxCost(500); // 限制缩略图内存占用
    m_throttleTimer = new QTimer(this);
=======
    m_thumbCache.setMaxCost(500); // 限制缩略图内存占用
    m_lastPixmapCache.setMaxCost(200); // 消除 data() 中的拦截，统一在此完成初始化分配
    m_throttleTimer = new QTimer(this);
>>>>>>> REPLACE
```

#### [物理重构指令 3] 针对 `src/ui/ScanTableModel.cpp` 的 `data()` 函数内
```cpp
<<<<<<< SEARCH
            // 限制双轨缓存的最大容量，防止极端高频缩放累积内存
            if (m_lastPixmapCache.maxCost() == 0) {
                m_lastPixmapCache.setMaxCost(200); // 默认限制 200 项可见卡片 LRU 备份
            }

            // 1. 精确尺寸缓存匹配
=======
            // 1. 精确尺寸缓存匹配
>>>>>>> REPLACE
```

---

### 4.3 物理线程池安全析构 (杜绝全局池异步滥用崩溃)

规范 `ScanTableModel` 析构函数，通过 `m_isDestroying` 快速原子阻断结合标准的 RAII 局域池销毁，杜绝全局线程池延迟删除漏洞。

#### [物理重构指令 4] 针对 `src/ui/ScanTableModel.cpp` 析构函数
```cpp
<<<<<<< SEARCH
    if (m_thumbPool) {
        m_thumbPool->clear();
        QThreadPool* poolToDestroy = m_thumbPool;
        m_thumbPool = nullptr;
        QThreadPool::globalInstance()->start([poolToDestroy]() {
            delete poolToDestroy;
        });
    }
=======
    if (m_thumbPool) {
        // 1. 快速排空排队任务
        m_thumbPool->clear();
        // 2. 干净同步回收（结合 m_isDestroying 瞬间折返，微秒级退出，绝对安全且无死等）
        m_thumbPool->waitForDone();
        delete m_thumbPool;
        m_thumbPool = nullptr;
    }
>>>>>>> REPLACE
```

---

### 4.4 抽象隔离数据引擎与视图表现层

新增底层查询标准接口 `IDataQueryEngine` 及视图标准接口 `IScanResultView`，在 `src/core/ModelContract.h`（若不存在则创建，用于解耦插拔规范定义）内强制执行：

```cpp
// 写入 / 规范化至 src/core/ModelContract.h
#pragma once
#include <QString>
#include <QStringList>
#include <vector>
#include <cstdint>

namespace FERREX {

struct ScanFilterState;

class IDataQueryEngine {
public:
    virtual ~IDataQueryEngine() = default;
    virtual std::vector<uint64_t> search(
        const QString& text,
        const ScanFilterState& state
    ) = 0;
};

}
```

---

### 4.5 零闪烁平滑变焦刷新控制
在滑块尺寸变化时，取消对任何重置与清空高速二级缓存的调用，完全利用 `m_lastPixmapCache` 的平滑自适应缩放进行占位绘制，规避高频变焦导致的白卡片。

---

### 4.6 清理 Memories.md 历史垃圾规则污染
物理移除 `Memories.md` 中属于其他项目的“星级评分、彩色胶囊、中性灰锁定”等离线残留定义，确保团队在纯净、无任何不相干噪声的上下文下进行零成本高效协作。

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 本方案通过最直观的 C++ 标准 SEARCH/REPLACE 指令，规定了重构时的物理替换边界。

**明确禁止越界修改的范围：**
- [ ] 严格禁止直接修改任何实际 `.cpp`、`.h` 文件及执行软件编译。

---

## 6. 实现准则与预警【核心】

1. **缓存溢出预防**：
   重构 SoA 缓存加载时，当搜索匹配结果突破 50 万项时，必须在后台线程对 `cachedPaths` 采用局部滑动加载策略，限制预装载最大项数为 10 万行，防止占用过多物理内存。
2. **多线程并发安全**：
   在 `processBatchUpdates()` 中进行 USN 重排序合并时，必须在排序前及 Proxy 装配循环内部高频检测 `mySortId != m_currentSortId.load(std::memory_order_relaxed)` 以在收到新搜索或退出命令时迅速返回。
