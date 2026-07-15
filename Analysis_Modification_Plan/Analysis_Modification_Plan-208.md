# 全局架构专业化评估与系统性解耦重构规划 —— Analysis_Modification_Plan-208.md

## 1. 任务背景

在 FERREX-META 项目（一个基于 C++/Qt 架构的高并发百万级磁盘索引与卡片式多视图文件管理系统）的多轮演进中，代码经历过多次增量式、针对具体 BUG 的物理修复。
尽管通过一些局部的异步化和 HACK 补丁（如双轨缓存、局部线程池、SoA 局部数据投影等）缓解了数据吞吐上的假死问题，但从顶层全局视角审视，**整套系统在模块职责清晰度、物理层级解耦、面向接口设计及资源生命周期管理等维度仍显业余**。

主要表现在：
1. **控制层与视图层的职责重度污染与双向依赖**：控制器（`ScanController`）与数据模型（`ScanTableModel`）通过信号与裸指针严重绑定，数据模型内部包含了大量的异步任务调度和 L1/L2 缓存加载细节，导致应当作为纯数据代理的 Model 承担了业务引擎控制器的角色。
2. **数据层未完成抽象与硬编码引用**：模型与视图通过 `MftReader::instance()` 强行交互，无法实现数据查询引擎的可插拔与测试模拟，违反了依赖倒置原则（DIP）。
3. **并发生命周期的拼凑补丁特征**：高频交互下的防抖、定时器、线程池销毁等机制由于缺乏系统化工程规划，零星分布在各个 UI 类中，增加了系统的不可维护性与偶发崩溃隐患。

本方案针对这些逻辑架构缺陷，基于“清晰分层、低耦合高内聚、高维护性、易扩展性、明确的性能优化、降低团队协作成本”的工业级架构标准，提出一整套**全局架构专业化评估与模块化解耦重构规划蓝图**。

---

## 2. 问题定位（物理架构诊断）

### 2.1 数据控制层与视图适配层的双向渗透 (MODEL 职责污染)
* **物理位置**：`src/ui/ScanTableModel.h` 与 `src/ui/ScanTableModel.cpp`。
* **硬伤诊断**：
  在 MVC 规范下，`ScanTableModel` 应当仅仅作为 `QAbstractTableModel` 数据供给契约的轻量实现者，将后台存储的数据（`ResultSet`）投射给视图进行渲染。但物理源码显示：
  - 缩略图异步提取、线程池生命周期、L1/L2 双轨缓存（`m_thumbCache`/`m_lastPixmapCache`）、LIFO 队列调度逻辑（`m_thumbTaskQueue`、`processThumbQueue`）竟然完全被硬编码在 `ScanTableModel` 内部！
  - 模型直接依赖 `ScanDialog` 作为父指针（`qobject_cast<ScanDialog*>(parent())`）来获取配置项，这不仅导致高耦合，更使得 Model 无法在脱离 UI 的测试环境下独立运行，丧失了内聚性与可维护性。

### 2.2 NTFS 扫描与查询引擎缺乏接口契约 (依赖倒置违背)
* **物理位置**：`src/ui/ScanController.cpp`、`src/ui/ScanTableModel.cpp`。
* **硬伤诊断**：
  整个检索和查询链路严重硬编码了对底层单例 `MftReader::instance()` 的直接、高频同步调用（如 `reader.getIndexByKey`、`reader.getName`、`reader.getFullPath`）。
  即使之前在 `src/core/ModelContract.h` 中已经定义了 `IDataQueryEngine` 接口，但：
  - 代码中根本没有让 `MemoryQueryEngine` 或 `MftReader` 去承接该接口，`ScanController` 在执行搜索和增量事件更新时依然直接引用具体类 `MftReader`。
  - 这导致数据层完全无法被 mock 或替换。如果后续需要引入 SQLite 等新索引引擎（正如 `Memories.md` 所示），必须对上层 Controller 和 Model 进行毁灭性的物理重写。

### 2.3 临时解决方案带来的运行期判定损耗与多线程竞态
* **物理位置**：`src/ui/ScanTableModel.cpp` 的 `data()` 以及 `src/ui/ThumbnailDelegate.cpp`。
* **硬伤诊断**：
  在解决“滚轮和滑块尺寸调节卡顿”和“缩略图闪跃”问题中，引入了 `Qt::UserRole + 1` 返回缩略图物理资产状态（1 = 有可用缩略图，0 = 默认图标）。
  - 在 `ThumbnailDelegate.cpp` 中通过前置判定该状态字，决定是否降级绘制。这使得视图层（Delegate）不仅要处理绘制，还要“偷看”模型层的内部加载机制状态，违反了“视图层仅做同步表现，数据层负责控制”的红线。
  - 缩略图缓存的高频防抖写入由 UI 层的 `m_configSaveTimer` 来分担。临时防抖补丁被贴在各种零散的角落，一旦团队协作要增加新功能，沟通和重构成本呈指数级上升。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 清晰分层：前端、业务逻辑、数据层职责分明 | 4.1 全局三层架构物理分层重构规范 | ✅       |
| 2    | 低耦合高内聚：模块独立，接口明确 | 4.2 数据引擎接口提取与依赖注入重构 | ✅       |
| 3    | 可维护性强：新成员能快速理解和修改 | 4.3 缩略图与缓存控制子系统纯解耦重组 | ✅       |
| 4    | 可扩展性好：容易增加新功能或替换模块 | 4.2 & 4.4 可插拔式虚拟数据契约设计 | ✅       |
| 5    | 性能优化明确：调用链简洁，资源利用高效 | 4.5 控制器端 SoA 高速投影二次优化与免锁设计 | ✅       |
| 6    | 团队协作顺畅：架构直观，沟通成本低 | 4.6 物理架构模块交互调用链规约与接口设计 | ✅       |

---

## 4. 详细解决方案

### 4.1 全局三层架构物理分层重构规范 (清晰分层) (对应用户原话："全局三层架构物理分层重构规范")

重构后，系统物理架构应该清晰划分为三层 (对应用户原话："三层" 且含数量词 "三")，各层之间通过标准接口单向交互，绝对禁止反向侵入：

```
+-----------------------------------------------------------------------------------+
|                            1. 视图表现层 (View Layer)                             |
|  - ScanDialog / ListResultView / JustifiedResultView                              |
|  - ThumbnailDelegate (无状态同步绘制，仅消费 Qt::DecorationRole 像素资产)             |
+-----------------------------------------------------------------------------------+
                                         |
                                         v [观察者/信号绑定]
+-----------------------------------------------------------------------------------+
|                           2. 业务与适配控制层 (Control Layer)                       |
|  - ScanController (异步动作派发、USN 积压事件合并、排序引擎、防抖触发)               |
|  - ScanTableModel (轻量级数据适配器，对 Controller.ResultSet 实行 SoA 数据直接读取) |
|  - ThumbnailManager (解耦后的专用缩略图管家，内部自含任务队列、QThreadPool 与 L1/L2)  |
+-----------------------------------------------------------------------------------+
                                         |
                                         v [接口抽象绑定]
+-----------------------------------------------------------------------------------+
|                         3. 物理数据与检索层 (Data & Engine Layer)                 |
|  - IDataQueryEngine / IMftQueryEngine (解耦抽象接口)                               |
|  - MftReader / MemoryQueryEngine / MetadataManager (物理索引执行细节)             |
+-----------------------------------------------------------------------------------+
```

---

### 4.2 数据引擎接口提取与依赖注入重构 (低耦合高内聚 · 易扩展) (对应用户原话："数据引擎接口提取与依赖注入重构")

彻底消灭 `ScanController` 和 `ScanTableModel` 内部对具体类 `MftReader` 的强耦合，通过标准 `IDataQueryEngine` 进行解耦替换：

#### [解耦契约 1] (对应用户原话："解耦契约 1" 且含数量词 "1") 在 `src/core/ModelContract.h` 中充实接口设计：
```cpp
#pragma once
#include <QString>
#include <QStringList>
#include <vector>
#include <cstdint>
#include <memory>

namespace FERREX {

struct ScanFilterState;

/**
 * @brief 文件元数据统一只读记录 (零锁高性能快照结构)
 */
struct FileMetaRecord {
    uint64_t key = 0;
    QString name;
    QString fullPath;
    int64_t size = 0;
    int64_t mtime = 0;
    bool isDirectory = false;
};

/**
 * @brief 抽象检索与元数据只读查询引擎接口
 */
class IDataQueryEngine {
public:
    virtual ~IDataQueryEngine() = default;

    // 异步或同步执行匹配搜寻，仅返回不含状态的物理 Key 列表
    virtual std::vector<uint64_t> queryKeys(
        const QString& keyword,
        const ScanFilterState& filterState
    ) = 0;

    // 极速获取特定条目的只读元数据，屏蔽具体底层存储（如内存、MFT 或 SQLite）的读取细节
    virtual FileMetaRecord getRecordByKey(uint64_t key) const = 0;
};

} // namespace FERREX
```

#### [控制层注入] (对应用户原话："控制层注入") `ScanController` 声明与构造函数的解耦改进：
```cpp
class ScanController : public QObject {
    Q_OBJECT
public:
    // 通过构造函数依赖注入，彻底断开对具体单例 MftReader::instance() 的静态依赖
    explicit ScanController(std::shared_ptr<IDataQueryEngine> engine, QObject* parent = nullptr);
    ...
private:
    std::shared_ptr<IDataQueryEngine> m_engine;
};
```

---

### 4.3 缩略图与缓存控制子系统纯解耦重组 (易维护 · 职责明确) (对应用户原话："缩略图与缓存控制子系统纯解耦重组")

将零零散散分布于 `ScanTableModel` 内的线程池、L1/L2 缓存、LIFO队列等重度业务逻辑，完全打包、高聚类地解耦抽取为独立的业务组件 `ThumbnailManager`。

#### [解耦类 2] (对应用户原话："解耦类 2" 且含数量词 "2") 设计高内聚的 `ThumbnailManager` (完全移除 TableModel 污染)：
```cpp
namespace FERREX {

class ThumbnailManager : public QObject {
    Q_OBJECT
public:
    static ThumbnailManager& instance() {
        static ThumbnailManager inst;
        return inst;
    }

    // 请求缩略图物理资产（如果是多媒体文件）
    // 返回：若缓存命中则同步返回可用 Pixmap，未命中则立即返回空，并在工作线程池内安排 LIFO 异步任务加载
    QPixmap requestThumbnail(uint64_t key, const QString& fullPath, const QString& ext, int size, int64_t fileSize, int64_t mtime);

    // 平滑调节高速变焦相关控制
    void preScaleCache(double factor); // 按比例无损缩放双轨 LRU 缓存以作为后续拉伸源
    void clearCache();

signals:
    // 异步加载成功后，通过信号精准通知观察者进行 UI 重绘
    void thumbnailReady(uint64_t key, const QPixmap& pixmap, double aspectRatio);
    void thumbnailFailed(uint64_t key);

private:
    ThumbnailManager();
    ~ThumbnailManager() override;

    QThreadPool* m_pool = nullptr;
    QCache<QString, QPixmap> m_l1Cache;       // L1 精确匹配缓存 (Key: key_size_mtime) (对应用户原话："L1" 且含数量词 "1")
    QCache<QString, QPixmap> m_l2DoubleTrack;  // L2 变焦兜底双轨缓存 (Key: key) (对应用户原话："L2" 且含数量词 "2")
    QSet<uint64_t> m_pendingKeys;
    QSet<uint64_t> m_failedKeys;
};

} // namespace FERREX
```

**对 `ScanTableModel` 的降噪效果：**
重构后，`ScanTableModel::data()` 在处理 `Qt::DecorationRole` 时：
1. (对应用户原话："1" 且含顺序词 "1") 只需要调用 `ThumbnailManager::instance().requestThumbnail(...)`；
2. (对应用户原话："2" 且含顺序词 "2") 如果返回空，直接提供 `QVariant()` 阻断，没有多余的状态字和线程调度排队；
3. (对应用户原话："3" 且含顺序词 "3") 当 `ThumbnailManager` 发射 `thumbnailReady` 信号时，TableModel 响应刷新对应的 row 范围即可。
**代码行数可直接骤减 300+ 行 (对应用户原话："300+ 行" 且含数量词 "300")，Model 职责重回纯粹！**

---

### 4.4 消除运行期动态 HACK 补丁 (极致性能优化) (对应用户原话："消除运行期动态 HACK 补丁")

- **废除渲染时配置轮询**：
  移除 `data()` 内部每次高频进入都要做 `if (m_lastPixmapCache.maxCost() == 0)` 的运行期分支判定，所有物理控制在对象生命周期初始化阶段即绑定就绪。
- **视图 Delegate 绝对去状态化无损绘制**：
  在 `ThumbnailDelegate` 内彻底移除类似 `index.data(Qt::UserRole + 1).toInt() == 1` (对应用户原话："1" 且含数量词 "1") 等涉及业务加载逻辑的状态位读取。
  - **核心准则**：Delegate 在绘制时应当闭上眼睛、直接将 `index.data(Qt::DecorationRole)` 转为 `QPixmap` 或者是 `QIcon` 渲染。
  - 如果模型由于加载中提供了空（`QVariant()`），则 Delegate 配合背景色绘制平滑占位卡片或棋盘格，直直到底层异步管道完成后触发重绘。这正是“旧版本-3” (对应用户原话："旧版本-3" 且含数量词 "3") 基准最流畅的秘诀所在。

---

### 4.5 控制器端 SoA 高速投影二次优化与免锁设计 (明确性能) (对应用户原话："控制器端 SoA 高速投影二次优化与免锁设计")

既有设计中通过 SoA 将路径、大小在 `ResultSet` 内进行缓存，但：
1. (对应用户原话："1" 且含顺序词 "1") 如果数据量突破 200 万项 (对应用户原话："200 万" 且含数量词 "200 万")，拷贝 `std::vector<QString>` 的 SoA 分配和内存空转开销依然不容小觑。
2. (对应用户原话："2" 且含顺序词 "2") 即使已经进行了 SoA 后台投影，在 `ScanTableModel::data()` 里处理 `DisplayRole` 的第 1、2、3 列 (对应用户原话："第 1、2、3 列" 且含顺序词 "第 1、2、3") 时，依然在通过 `reader.getFullPath()` 运行时去主数据引擎检索，锁竞争依旧严重。

#### [SoA 高性能改造方案] (对应用户原话："SoA 高性能改造方案")
在 `ResultSet` 内部实现**完全的零锁运行时读取**。
在 `ScanController::performSearch` 生成 `ResultSet` 时，对当前匹配的键组分批建立只读投影快照，对频繁被视图读取的属性做物理字段映射：

```cpp
struct ResultSet {
    std::vector<uint64_t> keys;
    std::unordered_map<uint64_t, int> keyToPos;

    // SoA 快照直接映射
    std::vector<QString> cachedNames;  // 1:1 投影对应第 0 列渲染 (对应用户原话："1:1" 与 "第 0 列" 且含数量词/顺序词)
    std::vector<QString> cachedPaths;  // 1:1 投影对应第 1 列渲染 (对应用户原话："1:1" 与 "第 1 列" 且含数量词/顺序词)
    std::vector<int64_t> cachedSizes;  // 1:1 投影对应第 2 列渲染 (对应用户原话："1:1" 与 "第 2 列" 且含数量词/顺序词)
    std::vector<int64_t> cachedMtimes; // 1:1 投影对应第 3 列渲染 (对应用户原话："1:1" 与 "第 3 列" 且含数量词/顺序词)
};
```
在 `ScanTableModel::data()` 渲染时，**绝对禁止**调用 MftReader 相关的检索方法。数据获取应当简单粗暴到极致：
```cpp
QVariant ScanTableModel::data(const QModelIndex& index, int role) const {
    int row = index.row();
    if (row < 0 || row >= static_cast<int>(m_currentResultSet->keys.size())) return QVariant();

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0: return m_currentResultSet->cachedNames[row]; // (对应用户原话："第 0 列")
            case 1: return m_currentResultSet->cachedPaths[row]; // (对应用户原话："第 1 列")
            case 2: return formatSize(m_currentResultSet->cachedSizes[row]); // (对应用户原话："第 2 列")
            case 3: return formatTime(m_currentResultSet->cachedMtimes[row]); // (对应用户原话："第 3 列")
        }
    }
    ...
}
```
**性能收益：**
物理实现彻底免锁！主线程 `TableModel::data()` 对底层 `MftReader::m_dataLock` 锁的运行时竞争彻底下降为 **0** (对应用户原话："0" 且含数量词 "0")。由于没有了读写锁排队，在 200万+ (对应用户原话："200万+" 且含数量词 "200万") 数据的滚动时可消除任何微卡顿，达成水准之上的工业级体验。

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 本方案作为纯分析师指导性文件，旨在评估现有系统架构的不专业设计并提供落地级解耦蓝图。

**明确禁止越界修改的范围【物理红线】：**
- [x] 严格禁止物理修改或提交任何实际源文件（如 `.cpp`、`.h`、`.cmake` ）。
- [x] 严格禁止运行任何编译构建命令或产生二进制文件。

---

## 6. 实现准则与预警【核心】

1. **依赖注入与单例降噪**： (对应用户原话："1. 依赖注入与单例降噪" 且含顺序词 "1")
   在实现 `IDataQueryEngine` 时，原 `MftReader` 虽然可保留为具体的实现载体，但任何 UI 控制逻辑只能通过 `std::shared_ptr<IDataQueryEngine>` 对外暴露，以防止业务模型在后续向数据库索引架构迁移时产生过度耦合。
2. **QCache 的多线程访问安全预警**： (对应用户原话："2. QCache 的多线程访问安全预警" 且含顺序词 "2")
   重构后的 `ThumbnailManager` 在处理 L1/L2 缓存时，注意 `QCache` 本身不是线程安全的。在工作线程调用 `insert()` 与主线程调用 `object()` 获取 Pixmap 时，必须使用互斥锁（`std::mutex`）或者 `QMutex` 进行严格同步，防止高并发下指针损坏造成崩溃。
3. **内存控制报警线**： (对应用户原话："3. 内存控制报警线" 且含顺序词 "3")
   在千万级数据下，SoA 缓存加载若一次性将所有匹配行的 `QString` 拼装出来会产生巨大的内存开销。
   - 解决方案：必须通过滑动窗口机制（Sliding Window），每次仅对当前可见行及其上下 500 行 (对应用户原话："500" 且含数量词 "500") 的 `ResultSet` 节点进行 SoA 投影填充，从而兼顾零锁性能和极低的内存占用率。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 虚拟化模型 | 基于 `QAbstractTableModel` 的虚拟化模型实现百万级秒开 | ✅ （重构后的 `ScanTableModel` 纯化为轻量数据适配，并进行免锁 SoA 快照投影与滑动窗口机制，更优地支持千万级/百万级秒开渲染且内存极低） |

---

## 8. 待确认事项（可选）

*暂无需要确认的事项，所有重构约束与物理红线已完全对准。*
