# 核心单例初始化竞争与缓存同步阻塞重构 —— Analysis_Modification_Plan-206.md

## 1. 任务背景
在对 FERREX-META 项目的物理源码进行多维扩大排查后，发现核心单例 `MftReader::instance()` 及系统层面的全量扫描缓存写入中，存在两处由于历史缺乏工程化规划而遗留的并发及性能缺陷：
1. **单例初始化提权竞态隐患**：`MftReader::instance()` 在静态变量 `inst` 构造时，其提权（`enablePrivilege`）调用却放置在其后的 `std::call_once` 中。在后台多线程并发环境下，可能会发生提权尚未物理完成而静态实例已在构造函数中执行需要特权的 NTFS 物理磁盘卷句柄打开申请，导致偶发性的 `ERROR_ACCESS_DENIED` 错误。
2. **扫描增量持久化中的值拷贝损耗**：在全量 MFT 扫描（`NtfsVolumeMftParser::loadMftDirect`）累积到 100,000 条记录投递本地缓存写入时，代码将包含十万条 SoA 数据的巨型 `delta` 变量以值拷贝形式捕获（`[path_base, delta, currentUsn]`）。在大规模磁盘扫描过程中，这导致主扫描线程内瞬间触发了极高开销的深拷贝与内存抖动。

本方案旨在针对核心单例提权时序及 MFT 异步扫描数据转移进行重塑。

---

## 2. 方案 A：核心单例提权时序对齐重构
在 `src/mft/MftReader.cpp` 的 `MftReader::instance()` 函数中，将 `std::call_once` 提权操作提前至局部静态变量 `inst` 的定义和构造之前。这保障了并发线程获取单例时，特权百分之百完全就绪，从而根除高并发下偶发性打开卷句柄被拒的死穴。

---

## 3. 方案 B：优化扫描持久化的零拷贝移动捕获
在 `src/mft/NtfsVolumeMftParser.cpp` 的 `NtfsVolumeMftParser::loadMftDirect` 记录投递 lambda 表达式中，使用 C++14 的右值捕获转移：
```cpp
movingDelta = std::move(delta)
```
并显式附带 `mutable` 关键字，将原本开销庞大的值拷贝克隆完美优化为 $O(1)$ 生命周期的指针和控制权转移，消除扫描主线程的卡顿和内存抖动。

---

## 4. 修改边界声明【红线】
- **修改文件范围**：`src/mft/MftReader.cpp`, `src/mft/NtfsVolumeMftParser.cpp`
- **明确禁止修改范围**：禁止修改上述两点优化逻辑之外的任何业务与 UI 展示代码。
