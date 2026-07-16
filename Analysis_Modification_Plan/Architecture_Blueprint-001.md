# FERREX-META 桌面端工业级架构设计与防错开发规范 —— Architecture_Blueprint-001.md

## 1. 架构改良目标
1. **强类型显式契约**：彻底清除 `Qt::UserRole + N` 等魔数，禁止使用 Qt 动态属性（Dynamic Property）黑魔法传递核心排版逻辑。
2. **物理层级严格隔离**：前端渲染层（View/Delegate）必须“哑巴化”（不含任何业务/file系统逻辑判定），数据层（Model/MftReader）负责将所有原始数据加工好并提供强类型接口。
3. **主线程零阻塞运行**：高频绘图事件中零内存分配，绝对禁止在 UI 线程同步执行任何涉及锁竞争或 I/O 的级联路径拼接动作。

---

## 2. 三层工业级架构职责划分（MVC/MVP 变种）

```
+-----------------------------------------------------------------+
|                       View Layer (Frontend)                     |
|  [ScanDialog] <---> [GridResultView] / [TableView]              |
|                             |                                   |
|                             +---> [ThumbnailDelegate]             |
+-----------------------------|-----------------------------------+
                              | (仅通过强类型 Payload 结构体进行安全数据交互)
+-----------------------------v-----------------------------------+
|                        Controller Layer                         |
|  [ScanController]  (处理检索、排序、选区逻辑，不参与任何 UI 细节)      |
+-----------------------------^-----------------------------------+
                              | (解耦)
+-----------------------------v-----------------------------------+
|                     Data & Repository Layer                     |
|  [ScanTableModel] <---> [CacheManager] <---> [MftRepository]    |
|                     (对 MftReader 核心单例进行接口隔离封装)        |
+-----------------------------------------------------------------+
```

### 2.1 Frontend Layer（前端渲染层）
* **代表类**：`ScanDialog`、`JustifiedView`、`TableView`、`ThumbnailDelegate`、`ListThumbnailDelegate`。
* **职责限制**：
  * **仅负责绘制**。只负责将接收到的数据、图标和颜色画在屏幕上。
  * **禁止包含业务白名单**：委派内绝对不准出现针对file后缀名（如判断是否为图像/视频）的白名单过滤逻辑（`mediaExts` 应从 Delegate 彻底剥离，交由数据层完成）。
  * **禁止直接调用 MFT 引擎**：Delegate 与 View 严禁直接通过 `MftReader::instance()` 提取file名或绝对路径。

### 2.2 Controller Layer（逻辑控制层）
* **代表类**：`ScanController`。
* **职责限制**：
  * 接收前端信号（如搜索编辑、过滤开关触发），在后台线程驱动检索，计算出唯一的原子结果集快照（`ResultSet`），并派发给 Model 层。不参与任何界面布局和像素绘制。

### 2.3 Data Layer（数据模型层）
* **代表类**：`ScanTableModel`。
* **职责限制**：
  * **统一数据加工**：负责将 `MftReader` 中的底层字节数据进行归纳、解析、缓存。它应该在将数据交给 UI 前，把file后缀过滤、常规file判定、DPI 放大折行、大图状态码、父目录缓存链彻底打包处理好，以结构体形式打包返回。

---

## 3. 核心防错开发红线（不容逾越的契约）

### 【红线一】魔数与隐式 Role 彻底禁令（强类型通信规约）
* **硬性规定**：禁止在 View、Delegate 与 Model 之间通过 `index.data(Qt::UserRole + N)` 读写分散的、未定义的魔数角色。
* **工业级解法**：在数据交互层统一定义一个强类型数据承载结构体 `FerrexItemPayload`（需通过 `Q_DECLARE_METATYPE` 进行元类型注册）：

```cpp
// 在 shared 域中声明：
struct FerrexItemPayload {
    uint64_t key = 0;              // 唯一 FRN
    QString name;                  // file名
    QString fullPath;              // 绝对路径（同步安全解析好的路径）
    QString extension;             // 预处理好的小写后缀名
    bool isDirectory = false;      // 是否为文件夹
    double aspectRatio = 1.0;      // 已经由 Model 预先判定好、处理好过滤的宽高比
    int thumbStatus = 0;           // 0=普通图标, 1=缩略图就绪
    bool isManaged = false;        // 是否已录入
    bool isEmptyFolder = false;    // 是否为空文件夹
};
Q_DECLARE_METATYPE(FERREX::FerrexItemPayload)
```
* **实现要求**：
  * **Model 层包装**：在 `ScanTableModel::data(..., Qt::UserRole)` 分支中，统一实例化该结构体，将所有数据字段填充完毕后通过 `QVariant::fromValue(payload)` 一并返回。
  * **渲染层解包**：Delegate 必须直接获取该强类型结构体：
    ```cpp
    FerrexItemPayload payload = index.data(Qt::UserRole).value<FerrexItemPayload>();
    // 之后所有绘制、角标判断均直接读取 payload 字段，编译期严格类型安全，重命名重构 100% 无死角！
    ```

### 【红线二】C++ 成员变量初始化硬性指标
* **硬性规定**：任何新增加的 Delegate、事件过滤器或自定义控件，**必须在类声明或构造函数初始化列表中将所有成员变量、Role 整型显式初始化为安全默认值（如 `-1` 或 `nullptr`）**。
* **技术理由**：杜绝 C++ 因垃圾内存野值导致在不同电脑或编译器上产生神秘的逻辑错乱和随机闪退。

### 【红线三】UI 线程 I/O 阻塞零限额硬性红线
* **硬性规定**：在主线程（尤其是 `paint` 渲染循环和 `mimeData` 复制响应槽）中，**绝对禁止同步执行未缓存的高频级联路径拼接 `MftReader::instance().getFullPath(idx)`**。
* **技术理由**：长达几秒的磁盘 I/O 和读写锁死会直接堵死主套间（Apartment）的消息循环，不仅会导致界面瞬间假死，还会直接触发 Windows OLE 剪贴板的 RPC 超时，造成批量复制粘贴大面积失效。所有的多级回溯必须在数据层提前级联缓存完毕。

### 【红线四】禁止使用 String-based 属性传递排版规则
* **硬性规定**：禁止通过 `option.widget->property("gridMode")` 或 `setProperty("gridMode")` 这种依靠字符串查找的动态属性来控制 Delegate 内部的图片平滑拉伸模式（`KeepAspectRatio`）。
* **解法**：Delegate 应提供强类型接口（如 `setLayoutMode(JustifiedView::LayoutMode)` 或直接在构造时传入），或在 `FerrexItemPayload` 结构体中直接附带渲染模式字段。保证在编译阶段完成类型校验。

---

## 4. 后续开发与重构实施路径

### 阶段一：打通强类型 `FerrexItemPayload` 通道
1. 在头file中声明 `FerrexItemPayload` 结构体并使用 `Q_DECLARE_METATYPE` 注册。
2. 重构 `ScanTableModel::data` 的 `Qt::UserRole` 分支，统一构建并返回此 Payload。
3. 移除多余的 `Qt::UserRole + 1`、`+ 2`、`+ 3` 等分散魔数判定，Delegate 统改从 Payload 提取数据。

### 阶段二：清算并补齐所有 Delegate 成员初始化
1. 检查并强制修改 `ThumbnailDelegate` 及所有派生类的构造函数，显式赋予其 `-1` 初始化，防范内存污染。

### 阶段三：路径与排版前置预处理，UI 绘图高阶减负
1. 将 `mediaExts`（自适应媒体扩展名白名单）判定、`QTextLayout`（file名双行换行省略计算）的耗时逻辑**从 `paint()` 高频事件中彻底剥离**，在 Model 层准备 Payload 数据时提前通过后台线流水线或防抖预处理完成，`paint` 只做最轻量、最快速的像素拷贝。
