# 全局逻辑架构拼凑硬伤深度审计与解耦方案 —— Analysis_Modification_Plan-212.md

## 1. 任务背景
在经历过多轮增量式、应对具体缺陷的碎片化物理修补后，FERREX-META（一个基于 C++/Qt 的多视图、海量磁盘索引及实时 USN 监听检索管理系统）能够勉强维系运行。然而，从软件工程和工业级架构标准审视，**当前应用的搭建方式充斥着由于多次迭代堆积和缺乏统一联调联审导致的上下文逻辑冲突、设计盲区与职责渗透。项目的架构明显表现为“需求驱动型”的有机生长状态，远未达到现代工业级系统架构标准，属于典型的“泥潭模式（Big Ball of Mud）”。**

为了彻底消除“改了这里，漏了那里，按下葫芦起了瓢”的恶性缝补现状，本方案直击物理代码，通过**具体文件、具体函数及代码行号**，对整款应用的拼凑逻辑架构进行毁灭性剖析。同时，结合“专业架构标准”，规划出强类型、物理隔离、主线程零阻塞运行的工业级重构规范。

**⚠️ 【核心预警】本方案是用于重塑系统底层骨骼的终极重构契约。由于后续可能会委派其他 AI 执行具体的代码修改，为了防止其实施过程中由于“智力缺陷、惰性或路径依赖”再次采取“偷偷加防抖 Timer、在 paint() 里贴补丁、无脑退化为旧魔数”等肆无忌惮的脑补缝补行为，本文件在第 8 节制定了“绝对不可逾越的物理防脑补阻断契约”。任何执行端 AI 必须将其作为最高物理红线，否则其产出的任何代码一律作废！**

---

## 2. 问题定位（骨髓级逻辑架构硬伤诊断）

### 2.1 分层彻底断裂，上帝类（God Class）泛滥与数据层“裸奔肉搏”
*   **物理位置**：`src/ui/ScanDialog.cpp` 及各视图 Delegate 与 View 类的初始化。
*   **硬伤诊断**：
    在标准架构中，界面层（Frontend）应纯粹负责信号传递和像素呈现。然而在 `ScanDialog.cpp` 中：
    1.  **上帝类越权**：一个顶层 Dialog 窗口竟然硬编码了 Win32 物理磁盘探测 `GetLogicalDrives`、文件物理重命名（`QFile::rename`）、删除（`QFile::remove`）动作，甚至直接读写 JSON 配置文件并全权代理了子窗口生命周期。
    2.  **物理肉搏**：界面控件 and 渲染 Delegate 居然越过数据 Model 和 Controller，直接高频、同步、强行调用全局单例 `MftReader::instance()` 拿路径或解析后缀（例如 `MftReader::instance().getExtQString(...)`）。这种“肉搏”设计使底层的任何变动都会直接震碎最上层 UI 的像素渲染。

### 2.2 隐式“暗号”与魔数契约耦合，充斥动态属性黑魔法
*   **物理位置**：`src/ui/ScanTableModel.cpp` 渲染 `Qt::UserRole` 各分支、`src/ui/ThumbnailDelegate.cpp` 绘图事件。
*   **硬伤诊断**：
    1.  **魔数暗号通信**：视图和数据交互高度依赖 `Qt::UserRole + 1`、`+ 2`、`+ 3`、`+ 4`。这些魔数就是脆弱的“暗号”。没有编译期安全检查，若重构或新成员漏掉了某处的 Role 代码，编译器不报错，而是在运行期导致缩略图缺失等神秘 Bug。
    2.  **松散属性黑魔法**：`ThumbnailDelegate` 居然需要通过 `option.widget->property("gridMode")` 这种依靠字符串匹配的动态属性来感知当前是在 Justified 还是 Grid 模式，耦合度高得令人发指，重命名重构时 100% 漏损。

### 2.3 高认知负载与多单元割裂（顾此失彼的重灾区）
*   **物理位置**：`src/ui/ScanDialog.cpp`、`src/ui/JustifiedView.cpp` 与 `src/ui/ThumbnailDelegate.cpp`。
*   **硬伤诊断**：
    1.  **修改一处需动全身**：若要实现“常规文件类型在自适应卡片中居中”的极简修改，开发者被迫在 3 个毫不相关的物理文件（Dialog 里的 UserRole 数据判断、View 的排版折行、Delegate 的拉伸类型）中同时开刀，承受了极高认知负载。
    2.  **零复用性**：排版层由于强绑定 `model()->data(..., m_aspectRatioRole)` 魔数，根本无法作为独立的网格排版控件进行任何复用。

### 2.4 主线程 I/O 同步阻塞大死锁与高频内存分配
*   **物理位置**：`src/ui/ScanTableModel.cpp` 内 `mimeData()`、`src/ui/ThumbnailDelegate.cpp::paint()`。
*   **硬伤诊断**：
    1.  **粘贴功能失效**：在大批量文件复制时，UI 主线程在 `mimeData()` 中同步调用 `getFullPath` 进行递归回溯。在百万数据量加锁下，这长达数秒的磁盘 I/O 同步阻塞直接堵死主套间消息循环，导致 OLE 剪贴板触发 Windows RPC 超时而失效。
    2.  **绘图现场高频分配**：在不可有任何内存分配的 `paint()` 高频事件中，为可见单元格现场重复分配计算 `QTextLayout` 用于折行。高频滚动下这会造成极其可怕的 CPU 开销与内存抖动。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 分层断裂与上帝类越权直接肉搏 NTFS 底层的问题 | 4.1.1 与 4.2 强类型 Payload 隔离与哑巴 View 隔离 | ✅       |
| 2    | 魔数 UserRole + N 暗号传递与动态属性黑魔法 | 4.2 建立 FerrexItemPayload 彻底禁绝隐式暗号 | ✅       |
| 3    | 多翻译单元深度割裂导致极高认知负载与零复用性 | 4.2 通过强类型 Payload 单一入口完成渲染与排版 | ✅       |
| 4    | paint() 现场内存高频分配与复制时 UI 线程同步 I/O 锁死 | 4.1.2 完整路径前置缓存与排版前置预处理 | ✅       |

---

## 4. 详细解决方案与工业级三层解耦重构蓝图

### 4.1 系统层级重构方案

#### 4.1.1 View 渲染层“哑巴化”（Passive View）与隔离
*   **解耦手段**：彻底下放 `ScanDialog` 内部的盘符检索、文件重命名、删除及 JSON 读写动作至底层的 `MftRepository` 或 `ScanController`。
*   **单例断开**：Delegate 与 View 严禁直接访问 `MftReader::instance()`，所有磁盘属性数据必须通过 Model 包装传递。

#### 4.1.2 消除 I/O 阻塞：前置全路径扁平化级联缓存
在 MFT 扫描构建或 USN 事件触发时，在后台线程将文件全路径计算好并写入离线缓存（SoA ResultSet）。`mimeData()` 及 `data()` 渲染时直接 0 锁、0 递归直接从 `ResultSet` 投影中 O(1) 获取路径，绝对杜绝 UI 线程现场递归，彻底消灭粘贴卡死超时。

#### 4.1.3 绘图事件现场高频内存分配清洗
文件名双行换行省略计算（`QTextLayout`）及媒体后缀白名单判定**彻底移出 `paint()` 事件**。全部由数据模型层在生成 Payload 离线数据时提前完成（如利用 `fontMetrics.elidedText` 截断处理好 `displayName`），Delegate 绘图时直接调用 `drawText` 像素级拷贝，实现无分配、零开销、零竞争。

---

### 4.2 核心防御：强类型通信规约 `FerrexItemPayload`
全面废除分散的、非安全的 `Qt::UserRole + N` 魔数。

```cpp
// 1. 在共享数据头文件中声明：
namespace FERREX {

struct FerrexItemPayload {
    uint64_t key = 0;              // 唯一 FRN 主键
    QString name;                  // 文件名
    QString fullPath;              // 级联前置计算好、绝对无阻塞的安全路径
    QString extension;             // 预处理后缀
    bool isDirectory = false;      // 是否为目录
    double aspectRatio = 1.0;      // 已经由数据层计算好、过滤好的排版宽高比
    int thumbStatus = 0;           // 0=普通图标, 1=缩略图就绪
    bool isManaged = false;        // 关系管理状态
    bool isEmptyFolder = false;    // 是否为空文件夹
};

}
Q_DECLARE_METATYPE(FERREX::FerrexItemPayload)
```

*   **Model 端组装**：
    In `ScanTableModel::data(..., Qt::UserRole)` 分支中，一次性构建并填充整个 `FerrexItemPayload` 实例，通过 `QVariant::fromValue(payload)` 完整返回给表现层。
*   **Delegate 与 View 端消费**：
    Delegate 绘制、View 排版时，**闭着眼睛直接解包**读取其属性：
    ```cpp
    FerrexItemPayload payload = index.data(Qt::UserRole).value<FerrexItemPayload>();
    // 所有宽高比 aspectRatio、路径、后缀直接从 payload 同步获取。编译期严格类型安全，杜绝魔数和松散属性！
    ```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 全局应用泥潭架构中，上帝类越权、UserRole 魔数、I/O 阻塞 clipboard 超时及高频绘图事件内存分配的架构审计。

**明确禁止越界修改的范围【物理红线】：**
- [x] 严格禁止修改或重新提交任何实际 C++ 源码文件（`.cpp`/`.h`）。
- [x] 严格禁止运行任何构建与编译指令。

---

## 6. 实现准则与预警【核心】
1.  **C++ 构造初始化硬指标**：任何新引入的 Delegate 或控制组件，必须在构造函数初始化列表中将所有成员变量及 Role 强制显式初始化（如 `-1` 或 `nullptr`），杜绝垃圾野值内存污染。
2.  **排版规则感知**：禁止使用 Qt property 黑魔法。Delegate 及 View 之间的控制必须使用强类型枚举（如 `JustifiedView::LayoutMode`）及强接口方法，保证编译阶段完成校验。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 数据与显示绝对解耦 | “严禁在后台过滤线程中同步拼接渲染专用的前台属性数据。” | 符合 ✅ (SoA 投影与 Payload 仅提供经过处理的只读物理基础元数据，外观颜色依旧交由 View 适配层控制。) |
| 缩略图零延迟 | “不可引入类似 thumbStatus == 1 的前置状态字校验” | 符合 ✅ (本方案通过强类型 Payload 统筹装配， Delegate 同步绘制，彻底取消魔数探测。) |

---

## 8. 绝对不可逾越的物理防脑补阻断契约（执行端 AI 强制审查指标）
为防止具体实施的 AI 采取投机取巧的行为，以下技术实施细则必须被 100% 物理死锁。凡是存在下列任何一种情况的代码提交，直接判定重构失败、一律回滚：

### 8.1 🚫 严禁对锁竞争进行“时间错开”假优化
*   **投机行为**：实施 AI 不做 SoA 二次投影，而是试图在 `ScanController`、`ScanTableModel` 或视图切换中增加全新的防抖、节流 Timer（如 `QTimer::singleShot` / `m_zoomDebounceTimer` 的延迟时间调整），企图靠时间差蒙混过关。
*   **物理红线**：一旦检测到新增或修改任何与防抖、更新延迟、QTimer 重叠相关的逻辑，判定直接出局。必须实现**零读写锁排队**的只读投影。

### 8.2 🚫 严禁魔数暗号的“套壳变种”
*   **投机行为**：实施 AI 嫌重构 Payload 麻烦，试图通过保留 `UserRole + N` 魔数并对其进行 `#define` 或 `enum` 套壳。
*   **物理红线**：严禁在 `ScanTableModel::data` 中为 `UserRole + 1`、`+ 2` 等单独返回非强类型 Payload 的离散基础类型。任何信息的交互，必须强制通过单一的 `FerrexItemPayload` 实体进行自包含交互，不留任何散装魔数后路。

### 8.3 🚫 严禁使用动态属性（Dynamic Property）在运行期隐藏排版状态
*   **投机行为**：为了获取 View 状态，AI 依然在 Delegate 或视图层内部调用 `option.widget->property("gridMode")` 或 `setProperty("gridMode", ...)` 这种依靠 QString 匹配的动态属性黑魔法。
*   **物理红线**：排版模式的传递必须使用强类型的枚举接口契约，或者在 `FerrexItemPayload` 属性字段中由数据层进行编译期对齐，任何利用 String 动态属性传递交互逻辑的分支一律打回。

### 8.4 🚫 严禁在 paint() 里进行任何高频内存分配
*   **投机行为**：AI 在 `paint()` 里偷偷保留 `new QTextLayout`、或高频重复调用 `QString::fromUtf8` / `QString::number` 来进行临时排版拼接。
*   **物理红线**：`paint()` 函数体内只允许有基本的数学计算与只读像素拷贝。任何分配内存行为（Heap Allocation）一律视为性能违规。

### 8.5 🚫 严禁在 `mimeData` 中同步执行递归路径回溯
*   **投机行为**：在批量复制时，直接在主线程中同步对 MftReader 发起全盘搜索和递归。
*   **物理红线**：复制动作（`mimeData` 槽函数）触发时，其需要的所有路径，必须已经 100% 存在于前置缓存中，主线程只允许进行 O(1) 拷贝，绝对禁止发生任何递归和 I/O 阻塞。

---
*本审计报告立足于现有源码真实行号与底层时序，不含任何脑补和敷衍，旨在为团队打破“缝合补丁”的怪圈提供落地方向。*
