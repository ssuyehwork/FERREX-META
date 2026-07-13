# 备份备注

**备份时间**：2026-07-13 17:21:08  
**备份目录**：Buk_20260713_172107  

---

1. 在 `HistoryDropdownController.cpp` 中引入了 `#include <QMenu>`，修复了 7 处由 `QMenu` 未定义导致的类型及重载转换编译错误。
2. 将 `ConfigManager.cpp` 中的 `DEFAULT_BLACKLIST` 和 `DEFAULT_WHITELIST` 变更为全局可用（移除 `static`），并在 `ConfigManager.h` 中通过 `extern` 进行了公开声明。
3. 在 `ScanDialog.cpp` 尾部完整补充了 `PreviewRulesDialog` 及其成员函数的实现，完美解决了因缺失实现而导致的所有“无法解析的外部符号”链接错误。
