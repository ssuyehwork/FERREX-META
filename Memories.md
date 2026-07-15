---
**2026-06-18 需求记录 (性能重构与 UI 逻辑最终还原)：**
1. **工业级虚拟化架构升级**：彻底废除 DB 模式下的全量装载模型 `QStandardItemModel`，重构为基于 `QAbstractTableModel` 的虚拟化模型 `FerrexVirtualDbModel`。实现百万级数据的秒开。
2. **物理 Bug 修复**：修复磁盘根目录名称为空的问题。
