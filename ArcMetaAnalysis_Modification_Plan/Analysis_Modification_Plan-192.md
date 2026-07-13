# 列表视图“修改日期”列可见性与拖拉保护设计 —— Analysis_Modification_Plan-192.md

## 1. 任务背景
用户反馈在“列表”视图中（对应用户原话：“关于“列表”视图”），“修改日期”列会因为列宽不受控或窗口缩小等原因超出 UI 范围（对应用户原话：“修改日期列不可超出UI范围”），进而导致“修改日期”列在视觉上不可见（对应用户原话：“避免修改日期列不可见”）。此外，用户补充了针对手动拖拽列宽时的最新硬限约束：手动调节“大小”列与“修改日期”列的宽度时，需要分别做最小宽度硬件保护（对应用户原话：“用户手动调整“大小”和“修改日期”列的宽度时，大小列最小宽度限制为80像素，不得小于80像素，修改日期列不可小于90像素”）。

## 2. 问题定位
经过对代码库的检索分析，问题定位于：
- **目标文件 1**：`src/ui/ScanDialog.cpp` 中的 `calculateNameColumnMinimumWidth()` 函数、`setup` 时绑定的 `sectionResized` 信号槽。
- **目标文件 2**：`src/ui/ScanDialog.h` 类声明。
- **根因分析**：
  1. **无自适应拓宽上限**：当数据集内文件名极长时，`calculateNameColumnMinimumWidth()` 自适应宽度过大。这会极度挤压“路径”、“大小”和“修改日期”列，使右侧的“修改日期”列（Column 3）超出 UI 范围而被截断。
  2. **拖拽逻辑无双向硬限保护**：目前的 `sectionResized` 只限制了“名称”列（Column 0）的下限（`newSize < minWidth`），但对 Column 0 的最大拉伸上限，以及 Column 2 (“大小”)、Column 3 (“修改日期”) 的用户拖拉缩减行为均无最小尺寸阈值硬限拦截，因此用户容易把列拽得过小导致无法阅读（对应用户原话：“大小列最小宽度限制为80像素，不得小于80像素，修改日期列不可小于90像素”）。
  3. **缺乏窗口改变尺寸联动**：主窗口改变大小（如边缘拖拉）时没有主动刷新名称列宽度上限，导致小窗口下 Column 3 被无情推挤出视野。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 关于“列表”视图，修改日期列不可超出UI范围，避免修改日期列不可见 | 4.1, 4.3 设计自适应宽度动态安全上限，重写 `resizeEvent` 事件触发，防止“修改日期”列超出 UI 范围。 | ✅ |
| 2    | 用户手动调整“大小”和“修改日期”列的宽度时，大小列最小宽度限制为80像素，不得小于80像素，修改日期列不可小于90像素 | 4.2 重构列表表头的 `sectionResized` 信号响应逻辑，对 Column 2、Column 3 施加手动拖拽强制下限。 | ✅ |

## 4. 详细解决方案

### 4.1 重构列宽上限计算
在 `src/ui/ScanDialog.cpp` 中重构 `ScanDialog::calculateNameColumnMinimumWidth()`：

**修改前：**
```cpp
int ScanDialog::calculateNameColumnMinimumWidth() const {
    if (!m_tableModel || !m_listResultView) return 260;
    auto* resultTableView = m_listResultView->getBaseView();
    if (!resultTableView) return 260;

    int rowHeight = m_config.iconSize;
    int cardWidth = rowHeight - 6; // 还原 ListThumbnailDelegate 内部计算的侧边长 [1]
    int basePadding = 6 + 10 + 10; // 左侧内边距(6px) + 间隙(10px) + 右侧文字边缘安全保护(10px) [1]

    // 获取 QTableView 当前使用的字体度量器
    QFontMetrics fm = resultTableView->fontMetrics();
    int maxTextWidth = 100; // 给文字留出基础的 100 像素安全区域

    // 平衡性能与精度：仅遍历当前结果集中的前 1000 个项目，防止大集合（如 200万数据）在主线程产生卡顿
    auto snapshot = m_controller->snapshot();
    if (!snapshot) return 260;
    int count = std::min<int>(1000, (int)snapshot->keys.size());
    auto& reader = MftReader::instance();

    for (int i = 0; i < count; ++i) {
        uint64_t key = snapshot->keys[i];
        int actualIdx = reader.getIndexByKey(key);
        if (actualIdx != -1) {
            QString name = reader.getName(actualIdx);
            // 使用 horizontalAdvance 精确测算该文件名在当前字体下的物理像素宽度 [1]
            int textWidth = fm.horizontalAdvance(name);
            if (textWidth > maxTextWidth) {
                maxTextWidth = textWidth;
            }
        }
    }

    // 精确返回：正方形卡片宽度 + 最长文件名像素宽 + 间距补偿 [1]
    return cardWidth + maxTextWidth + basePadding;
}
```

**修改后：**
```cpp
int ScanDialog::calculateNameColumnMinimumWidth() const {
    if (!m_tableModel || !m_listResultView) return 260;
    auto* resultTableView = m_listResultView->getBaseView();
    if (!resultTableView) return 260;

    int rowHeight = m_config.iconSize;
    int cardWidth = rowHeight - 6; // 还原 ListThumbnailDelegate 内部计算的侧边长 [1]
    int basePadding = 6 + 10 + 10; // 左侧内边距(6px) + 间隙(10px) + 右侧文字边缘安全保护(10px) [1]

    // 获取 QTableView 当前使用的字体度量器
    QFontMetrics fm = resultTableView->fontMetrics();
    int maxTextWidth = 100; // 给文字留出基础的 100 像素安全区域

    // 平衡性能与精度：仅遍历当前结果集中的前 1000 个项目，防止大集合（如 200万数据）在主线程产生卡顿
    auto snapshot = m_controller->snapshot();
    if (!snapshot) return 260;
    int count = std::min<int>(1000, (int)snapshot->keys.size());
    auto& reader = MftReader::instance();

    for (int i = 0; i < count; ++i) {
        uint64_t key = snapshot->keys[i];
        int actualIdx = reader.getIndexByKey(key);
        if (actualIdx != -1) {
            QString name = reader.getName(actualIdx);
            // 使用 horizontalAdvance 精确测算该文件名在当前字体下的物理像素宽度 [1]
            int textWidth = fm.horizontalAdvance(name);
            if (textWidth > maxTextWidth) {
                maxTextWidth = textWidth;
            }
        }
    }

    int calculatedWidth = cardWidth + maxTextWidth + basePadding;

    // 为防“修改日期”列超出 UI 范围而不可见（对应用户原话：“修改日期列不可超出UI范围，避免修改日期列不可见”）：
    // 必须要为右侧列预留安全像素宽度。
    // - 路径列：预留至少 150 像素。
    // - 大小列：预留当前实际大小（或至少 80 像素最小硬限）
    // - 修改日期列：预留当前实际大小（或至少 90 像素最小硬限）
    int viewportWidth = resultTableView->viewport()->width();
    if (viewportWidth <= 0) {
        viewportWidth = resultTableView->width();
    }

    if (viewportWidth > 0) {
        int reservedWidth = 150 + qMax(80, resultTableView->columnWidth(2)) + qMax(90, resultTableView->columnWidth(3));
        int maxAllowedWidth = viewportWidth - reservedWidth;
        if (maxAllowedWidth < 200) {
            maxAllowedWidth = 200; // 维持名称列最低 200 像素的基本显示
        }
        if (calculatedWidth > maxAllowedWidth) {
            calculatedWidth = maxAllowedWidth;
        }
    }

    return calculatedWidth;
}
```

### 4.2 重构 `sectionResized` 拖拽行为硬限拦截逻辑
在 `src/ui/ScanDialog.cpp` 初始化 `m_listResultView` 时，将原本单一限制 Column 0 下限的连接改为对 Column 0、2、3 进行双向或下限全面拦截：

**修改前：**
```cpp
    // Apply the header drag and width auto-restoration constraint on resultTableView [1]
    if (resultTableView) {
        connect(resultTableView->horizontalHeader(), &QHeaderView::sectionResized, this, [this, resultTableView](int logicalIndex, int /*oldSize*/, int newSize) {
            if (logicalIndex == 0 && m_tableModel) {
                int minWidth = calculateNameColumnMinimumWidth();
                if (newSize < minWidth) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(0, minWidth);
                    resultTableView->horizontalHeader()->blockSignals(false);
                }
            }
        });
    }
```

**修改后：**
```cpp
    // Apply the header drag and width auto-restoration constraint on resultTableView [1]
    if (resultTableView) {
        connect(resultTableView->horizontalHeader(), &QHeaderView::sectionResized, this, [this, resultTableView](int logicalIndex, int /*oldSize*/, int newSize) {
            if (!m_tableModel) return;

            if (logicalIndex == 0) {
                int minWidth = calculateNameColumnMinimumWidth();

                // 动态上限拦截，确保不挤死右侧各列空间
                int viewportWidth = resultTableView->viewport()->width();
                if (viewportWidth <= 0) viewportWidth = resultTableView->width();
                int reservedWidth = 150 + qMax(80, resultTableView->columnWidth(2)) + qMax(90, resultTableView->columnWidth(3));
                int maxWidth = viewportWidth - reservedWidth;
                if (maxWidth < minWidth) maxWidth = minWidth;

                if (newSize < minWidth) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(0, minWidth);
                    resultTableView->horizontalHeader()->blockSignals(false);
                } else if (newSize > maxWidth) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(0, maxWidth);
                    resultTableView->horizontalHeader()->blockSignals(false);
                }
            }
            else if (logicalIndex == 2) {
                // 用户手动调整“大小”列（对应用户原话：“用户手动调整“大小”和“修改日期”列的宽度时”）
                // 限制其宽度不得小于 80 像素（对应用户原话：“大小列最小宽度限制为80像素，不得小于80像素”）
                if (newSize < 80) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(2, 80);
                    resultTableView->horizontalHeader()->blockSignals(false);
                }
            }
            else if (logicalIndex == 3) {
                // 用户手动调整“修改日期”列（对应用户原话：“用户手动调整“大小”和“修改日期”列的宽度时”）
                // 限制其宽度不得小于 90 像素（对应用户原话：“修改日期列不可小于90像素”）
                if (newSize < 90) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(3, 90);
                    resultTableView->horizontalHeader()->blockSignals(false);
                }
            }
        });
    }
```

### 4.3 窗口缩放联动自愈
重写 `ScanDialog` 类的大小改变事件虚函数。

**在 `src/ui/ScanDialog.h` 类声明中加入重写：**
```cpp
protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override; // 重载大小变更
    bool eventFilter(QObject* watched, QEvent* event) override;
```

**在 `src/ui/ScanDialog.cpp` 中具体实现：**
```cpp
void ScanDialog::resizeEvent(QResizeEvent* event) {
    FramelessDialog::resizeEvent(event); // 沿用基类处理

    // 如果处于“列表”视图模式下（对应用户原话：“关于“列表”视图”），窗口拉伸调整时自动核算最新上限并调整列宽
    if (m_config.viewMode == 0 && m_listResultView) {
        auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
        if (resultTableView) {
            int minWidth = calculateNameColumnMinimumWidth();
            resultTableView->setColumnWidth(0, minWidth);
        }
    }
}
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.h` 和 `src/ui/ScanDialog.cpp` 中关于列宽限制和 resizeEvent 绑定的逻辑代码。

**明确禁止越界修改的范围：**
- [ ] 严禁修改列表结果视图的底层数据结构模型或其它无关 UI 效果。

## 6. 实现准则与预警【核心】
1. **依赖的头文件预警**：在 `src/ui/ScanDialog.cpp` 中确保加入了 `#include <QResizeEvent>` 的引用，防止类型未定义编译错误。
2. **死循环回弹保护**：在手动调节逻辑强制指定 `setColumnWidth` 前后，必须利用 `blockSignals` 进行包裹断开，阻断信号递归溢出。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 列表视图 | 列表视图展示全部匹配的文件，并保证各列的合理可见性与调节控制约束 | ✅ 符合 |

## 8. 待确认事项与未定义描述声明（含未指定方位词、数量词与顺序词列表）
本方案中使用的以下方位词、数量词与顺序词（未在用户原话中显式指定，故在此罗列声明以确保完全合规）：
- **方位词**：
  - “右侧”（指代右侧的路径、大小、修改日期等列）
  - “下限”、“上限”（指代各列的大小调节限制边界）
  - “前”、“后”（出现于“修改前”、“修改后”的代码版本对比）
- **数量词**：
  - “192”（Analysis_Modification_Plan-192.md 的版本编号）
  - “1”、“2”、“3”、“4”、“5”、“6”、“7”、“8”（本规划文档的各大章节编号）
  - “150”、“200”（为列预留和最低限制的具体像素数值）
  - “0”、“2”、“3”（列索引编号）
  - “双向”（“拖拽行为双向硬限保护”）
- **顺序词**：
  - “第”（出现于“第 8 节”等表述中）
  - “第一”（“目标文件 1”）
  - “第二”（“目标文件 2”）
  - “最低”（“维持名称列最低 200 像素”）
