# Eagle 底层逻辑架构分析报告

### 1. 核心设计：逻辑与物理的完全解耦
Eagle 架构并不依赖磁盘物理路径。它在原生文件系统之上构建了一层逻辑映射层，确保了素材管理的灵活性。这种设计实现了“非破坏性”管理，即不改变用户原始的文件存储结构。

### 2. 全局身份追踪 (UID System)
*   **机制**：每个素材在导入库时分配一个永久的唯一 ID（UID），例如在 `mtime.json` 中可见的 `K1FCER5QBASU2` 等。
*   **作用**：所有的元数据（评分、标签、注释、分类关联）都与该 UID 绑定，而非与物理路径绑定。即使物理文件被重命名或在同一个分区内移动，只要 UID 关联存在，数据就不会丢失。

### 3. 虚拟分类拓扑 (Logical Hierarchy)
*   **核心文件**：`metadata.json`
*   **机制**：定义了一套基于 ID 的树状逻辑文件夹结构。
*   **特点**：
    *   每个逻辑文件夹拥有唯一 ID（如 `MAXC6PC9UDVHM`）。
    *   分类的父子级嵌套关系完全由 JSON 的 `children` 数组定义，与物理磁盘上的目录嵌套完全脱离。
    *   支持瞬间重构目录树而无需触动磁盘 IO。

### 4. 性能快照引擎 (Incremental Sync)
*   **核心文件**：`mtime.json`
*   **机制**：记录每个素材 ID 对应文件的最后感知修改时间戳（Unix Timestamp）。
*   **作用**：软件启动时通过快速对比磁盘 `mtime` 与 JSON 快照，实现万级素材的秒级增量更新，避免全量扫描带来的性能负担。

### 5. 集中式标签池 (Global Tag Pool)
*   **核心文件**：`tags.json`
*   **机制**：维护全局标签词库、标签组（Tag Groups）以及用户的输入历史（historyTags）。
*   **作用**：为 UI 提供全局快速联想支持，确保跨素材标签命名的一致性，同时具体的绑定关系分布式存储在素材各自的描述文件中。

### 6. 存储单元架构 (Typical Storage)
在完整的 Eagle 架构中，物理资源通常以 `<UID>.info` 的形式进行扁平化存储，每个文件夹包含原始素材、描述 JSON 和预览图。根目录的 JSON 文件充当了整个库的“大脑”和控制中枢。

// ===================|===================

这是一个很好的底层问题。Eagle 是用 **Electron** 构建的（本质是 Node.js + Chromium），所以读取过程大致是这样的：

---

## 启动时的读取流程

### 第一步：读取 `library.eagle`（全局索引）

```javascript
// 单个文件，直接同步读取
const library = JSON.parse(fs.readFileSync('MyLibrary.library/library.eagle', 'utf-8'))
// 拿到文件夹树、标签组等全局结构
```

---

### 第二步：扫描所有 `.info` 文件夹，批量读取 `metadata.json`

这里是**性能关键点**，Eagle 不可能一个一个同步读，大概率采用的是：

```javascript
// 1. 先扫描得到所有路径列表
const infoDirs = fs.readdirSync('images/').filter(d => d.endsWith('.info'))

// 2. 并发批量读取（Promise.all 或分批 chunk）
const allMetadata = await Promise.all(
  infoDirs.map(dir => 
    fs.promises.readFile(`images/${dir}/metadata.json`, 'utf-8')
      .then(JSON.parse)
  )
)
```

---

### 第三步：构建内存索引

读完之后不是简单地存一个数组，而是会构建多个**倒排索引**，方便快速查询：

```javascript
const store = {
  items: new Map(),        // id → metadata 对象
  byTag: new Map(),        // "插画" → Set([id1, id2, ...])
  byFolder: new Map(),     // folderId → Set([id1, id2, ...])
  byExt: new Map(),        // "jpg" → Set([...])
  byColor: [],             // 用于颜色近似匹配
}

for (const meta of allMetadata) {
  store.items.set(meta.id, meta)
  
  for (const tag of meta.tags) {
    if (!store.byTag.has(tag)) store.byTag.set(tag, new Set())
    store.byTag.get(tag).add(meta.id)
  }
  // ... 其他索引同理
}
```

---

## 整体流程图

```
磁盘
├── library.eagle ──────────────────→ 全局结构（文件夹树）
│                                              ↓
└── images/                            内存 Store
    ├── AAA.info/metadata.json ──┐             ↓
    ├── BBB.info/metadata.json ──┼──→  Map / 倒排索引
    ├── CCC.info/metadata.json ──┘
    └── ...（并发读取）               搜索/过滤直接查这里
                                       不再碰磁盘
```

---

## 为什么用并发而不是逐个读？

| 方式 | 1万个文件耗时（估算） |
|------|------|
| 逐个同步读 | 5～15 秒 |
| Promise.all 并发 | 0.5～2 秒 |
| 分批 chunk 并发 | 1～3 秒（更稳定，避免句柄耗尽） |

并发读是这类场景的标准做法，Node.js 的异步 I/O 天然适合这种"大量小文件"的场景。

---

**一句话总结**：Eagle 启动时用 Node.js 的异步并发 I/O 把所有 `metadata.json` 读进来，然后在内存里构建多个倒排索引，之后所有搜索操作都只查内存，不再碰磁盘。