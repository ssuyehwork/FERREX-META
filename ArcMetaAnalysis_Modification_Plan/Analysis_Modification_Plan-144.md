# 扩展名 SoA 原子化存储与零解析搜索方案 —— Analysis_Modification_Plan-144.md

## 1. 任务背景
在数百万级数据（如截图所示的 89.6 万项）的高频筛选场景下，现有的 `matchEntry()` 筛选逻辑由于依赖实时字符串解析（`rfind('.')` / `substr()`）导致界面出现明显的“假死”现象。为了彻底消除搜索链路中的 CPU 重复开销，需要将扩展名的拆分动作前移至数据入库阶段，并实现基于字段偏移的原子化比较。

## 2. 问题定位
- **瓶颈函数**：`src/mft/MftReader.cpp` 中的 `MftReader::matchEntry()` 及 `MftReader::search()`。
- **根因分析**：
  1. **高频解析开销**：每次点击过滤项或输入关键词时，模型都会对百万级条目执行 `rfind` 查找点号，并在主线程产生海量临时的 `QByteArray` 对象。
  2. **搜索架构缺陷**：SoA 结构虽然优化了属性读取，但文件名后缀这种“高频谓词”却处于非结构化状态，违背了高性能数据处理原则。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 在扫描/加载阶段就完成拆分 | 修改 `loadMftDirect`, `updateEntryFromUsn`, `loadFromCache` | ✅ |
| 2    | 新增 `m_ext_offsets` 数组 | 在 SoA 结构中新增平行偏移数组 | ✅ |
| 3    | 扩展名统一转小写且不含点 | `splitNameAndExt` 实现标准化转换 | ✅ |
| 4    | matchEntry 零解析比较 | 移除筛选路径中的所有字符串解析函数 | ✅ |
| 5    | 不改动磁盘格式 | 持久化层维持现状，仅重构内存映射逻辑 | ✅ |

## 4. 详细解决方案

### 4.1 核心拆分引擎实现 (MftReader.cpp)
在 `MftReader.cpp` 匿名命名空间或作为私有静态函数实现：
```cpp
static void splitNameAndExt(const std::string& fullName, std::string& outExt) {
    outExt.clear();
    // 1. 定位最后一个 "."
    size_t lastDot = fullName.find_last_of('.');
    // 2. 边界检查：若无 "." 或 "." 处于首位（如 .gitignore），ext 设为空
    if (lastDot != std::string::npos && lastDot > 0) {
        outExt = fullName.substr(lastDot + 1);
        // 3. 统一转换为小写 (筛选加速)
        std::transform(outExt.begin(), outExt.end(), outExt.begin(), ::tolower);
    }
}
```

### 4.2 SoA 结构扩展 (MftReader.h)
- 在 `MftReader` 私有成员中增加：`std::vector<uint32_t> m_ext_offsets;`
- 在 `clearInternal()` 中同步执行 `m_ext_offsets.clear();`

### 4.3 零解析搜索重构 (matchEntry / search)
- **search 预处理**：在并行循环开始前，将 `extensionList` 预处理为：
  - 统一小写。
  - 移除前导 "."。
- **matchEntry 改造**：
```cpp
// 改造前 (含解析)
// size_t nameLen = strlen(p); ... _stricmp(p + nameLen - exUtf8.size(), exUtf8.constData())

// 改造后 (零解析，纯寻址)
const char* ext = reinterpret_cast<const char*>(m_string_pool.data() + m_ext_offsets[i]);
for (const QString& ex : processedExtensionList) {
    if (_stricmp(ext, ex.toUtf8().constData()) == 0) {
        return true;
    }
}
```

### 4.4 数据的全量/增量准入 (三处核心修改点)
1. **loadMftDirect / mergeDriveResult**：在 `mergeDriveResult` 循环中，调用 `splitNameAndExt`，将结果压入 `m_string_pool` 并记录 `m_ext_offsets`。
2. **loadFromCache**：从 `.bin` 读取 `pkg.name` 后，执行一次性拆分并存入 SoA。
3. **updateEntryFromUsn**：重命名或新建文件时，重新计算 `ext` 并覆盖/追加 SoA。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] `src/mft/MftReader.h`：结构声明改动。
- [ ] `src/mft/MftReader.cpp`：拆分算法实现与筛选逻辑重构。

**明确禁止越界修改的范围：**
- [ ] `src/mft/ScchCache.cpp`：禁止修改磁盘持久化字段。
- [ ] `src/ui/ScanTableModel.cpp`：禁止修改 UI 显示逻辑。

## 6. 实现准则与预警【核心】
- **内存对齐**：`m_ext_offsets` 必须与 `m_frns` 等数组保持 1:1 的容量对齐，确保 `compact()` 时不丢失索引。
- **隐藏文件注记**：方案已明确 `.bashrc` 等文件的扩展名为“空字符串”，这在 `matchEntry` 匹配时必须得到正确体现。
- **碎片整理**：`MftReader::compact()` 必须同步处理 `m_ext_offsets` 的迁移。

## 7. Memories.md 合规检查 (角色准则内化版)
- **并发性能**：`search()` 依然维持并行处理，但循环体开销由于零解析化而大幅降低。
- **UI 考古**：SoA 扩展名存储模式沿用现有的 `m_name_offsets` 成熟方案。

## 8. 待确认事项
- **多重扩展名处理**：本方案采用“取最后一段”原则。例如 `config.tar.gz` 会存储为 `gz`。若用户未来需要支持多重扩展名筛选，需在 `splitNameAndExt` 中引入白名单或特殊逻辑。
- **隐藏文件界定**：以 `.` 开头的文件（如 `.env`）目前被定义为“无后缀”文件。
- **内存预估**：每百万条数据将产生约 4MB 的 `m_ext_offsets` 额外开销，以及字符串池中平均 4-6 字节/条目的扩展名存储开销，整体内存增长量预估在 8-10MB 左右。
