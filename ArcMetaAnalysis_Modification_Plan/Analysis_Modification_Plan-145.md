# 百万级数据扫描瓶颈分析与极致性能优化方案 (修正版) —— Analysis_Modification_Plan-145.md

## 1. 任务背景
针对大规模驱动器扫描效率低（5022 项/21s）及 UI 假死问题。修正版重点解决排序阶段的并发安全红线，并补全内存映射加载细节。

## 2. 问题定位
- **[重大安全隐患] 排序野指针风险**：原方案试图在锁外使用 SoA 池的 `const char*` 指针。由于 USN 写入可能触发 `vector` 重分配，这会导致排序过程中程序崩溃。
- **[性能瓶颈] 字符串分配风暴**：`loadMftDirect` 的堆分配开销及 `serializeRecord` 中冗余的 `vector` 创建。
- **[架构缺失] MMAP 加速未落地**：`ScchCache` 的加载仍依赖低效的系统调用。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 扫描效率极低 | 引入内存映射加载 (MMAP) | ✅ |
| 2    | UI 假死/卡顿 | 实施“投影拷贝排序”，消除锁竞争 | ✅ |
| 3    | 内存并发安全 | 方案 4.1 采用投影拷贝而非原始指针 | ✅ |

## 4. 详细解决方案

### 4.1 安全的“去锁化”排序投影 (ScanController.cpp)
为了确保 `std::sort` 在锁外执行时不崩溃，必须放弃原始指针，改用“投影拷贝”：
```cpp
// 1. 投影阶段（持有读锁）
struct SortProjection {
    uint32_t index;
    std::string key; // 拷贝字符串，确保持续可用
};
std::vector<SortProjection> projections;
{
    QReadLocker lock(&m_dataLock);
    // ... 批量拷贝文件名到 projections ...
}

// 2. 排序阶段（完全释放锁）
std::sort(std::execution::par, projections.begin(), projections.end(), [](const SortProjection& a, const SortProjection& b){
    return _stricmp(a.key.c_str(), b.key.c_str()) < 0;
});

// 3. 应用阶段（原子交换）
{
    QWriteLocker lock(&m_dataLock);
    // 更新 m_sorted_indices ...
}
```

### 4.2 基于 MMAP 的高速加载 (ScchCache.cpp)
废除 `fread` 循环，改用 Win32 内存映射：
```cpp
HANDLE hFile = CreateFileA(bin_path.c_str(), GENERIC_READ, FILE_SHARE_READ, ...);
HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
const uint8_t* pBase = (const uint8_t*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);

// 直接在 pBase 指针上移动，解析 ScchRecord 和文件名
// 消除 seek 和 read 系统调用开销
```

### 4.3 写入链路性能加固
- **流式 CRC**：重构 `serializeRecord`，移除 `temp_for_crc` 向量构造，改为直接对原始内存分块计算 CRC。

## 5. 修改边界声明【红线】
**涉及范围：**
- `src/mft/MftReader.cpp` / `src/mft/ScchCache.cpp` / `src/controller/ScanController.cpp`

**禁止越界：**
- 禁止在排序投影中为了节省内存而持有原始 `m_string_pool` 指针。

## 6. 实现准则与预警
- **内存预警**：投影拷贝会短期占用额外内存（约 100 万项条目需 32MB），但在当前 PC 环境下属于合理交易（以内存换安全与速度）。
- **MMAP 句柄管理**：必须使用 RAII 包装 `UnmapViewOfFile`，确保任何异常路径下均能释放映射。

## 7. Memories.md 合规检查
- **呼吸窗口**：完全释放锁的排序逻辑为 UI 线程提供了 100% 的呼吸空间。
- **并发性能**：投影过程使用 `std::execution::par` 加速。
