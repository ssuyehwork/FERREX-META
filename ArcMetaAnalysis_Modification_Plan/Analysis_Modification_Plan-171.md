# 网格模式末行元素对齐与间距一致性重构 —— Analysis_Modification_Plan-171.md

## 1. 任务背景
在 FERREX-META 视图组件的“网格排版 (等高等宽)”（GridMode，对应用户上传的图片 `image.png` 中右侧 [右侧] 红色箭头所指向的下拉菜单项）模式下，如果最后一行 [最后一行] 元素未填满满排数目，该行的列间距会偏大，导致该行元素被强行分散拉伸，无法与上方 [上方] 满排的元素在垂直列线 [垂直列线] 上严格对齐，视觉排版呈现混乱（对应图片 `image.png` 中底部 [底部] 红色实线矩形框内的排版现象）。本方案旨在物理重构 Grid Mode 的几何排版计算模型，确保网格模式下的所有卡片无论在哪一行均能实现跨行垂直列线对齐与恒定间距。

## 2. 问题定位
* **物理病灶所在模块**：`src/ui/JustifiedView.cpp`
* **关键函数**：`JustifiedView::doLayout()`
* **问题成因**：
  在 `doLayout()` 函数的 `GridMode` 逻辑段中：
  ```cpp
  int i = 0;
  while (i < count) {
      int numInRow = (containerWidth + spacing) / (itemWidth + spacing);
      if (numInRow <= 0) numInRow = 1;
      numInRow = std::min(numInRow, count - i); // [问题点 1] 末行元素数量在此被强行截断为 count - i

      // 两端对齐等间距分布
      int actualSpacing = 0;
      if (numInRow > 1) {
          // [问题点 2] 若末行元素数量变小，此处计算出的实际间距 actualSpacing 会远远大于满排时的间距
          actualSpacing = (containerWidth - (numInRow * itemWidth)) / (numInRow - 1);
      }

      int currentX = margin;
      for (int j = 0; j < numInRow; ++j) {
          int itemIdx = i + j;
          m_geometries[itemIdx] = { QRect(currentX, currentY, itemWidth, itemHeight), itemIdx };
          currentX += itemWidth + actualSpacing; // [问题点 3] 强行使当前行元素按重新计算出的、过宽的 actualSpacing 渲染
      }
      currentY += itemHeight + spacing;
      i += numInRow;
  }
  ```
  上述逻辑混淆了“两端对齐 (Justify)”与“网格对齐 (Grid)”的数学模型。网格排版应当基于**最大满行容量数下的固定间距**来布局所有元素，保证最后一行元素（无论有多少个）依然按照上方 [上方] 行列对齐的物理坐标定位，其右侧 [右侧] 未填满的空间则保持自然空白。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 用户确认在“网格排版 (等高等宽)”模式下修复末行对齐与间距 | 废除末行 `actualSpacing` 的重算机制，使其共享满排间距 | ✅ 一致 |
| 2    | 未满排的末行元素必须保持与满排时完全一致的垂直列线对齐 | 采用基于最大满行数的固定间距算法来物理约束所有行，使末行向左端 [左侧] 自然靠拢 | ✅ 一致 |

## 4. 详细解决方案

在 `src/ui/JustifiedView.cpp` 的 `doLayout()` 函数中，对 `GridMode` 的布局逻辑进行如下重构：

### 网格排版恒定间距算法设计
1. **预先计算最大满行容纳数**：
   不应在循环内部对当前行执行 `numInRow` 截断，而应先在进入循环前根据容器总宽度 `containerWidth` 和标准间距 `spacing` 计算出标准满行情况下最多能放多少个元素（设为 `maxNumInRow`）：
   ```cpp
   int maxNumInRow = (containerWidth + spacing) / (itemWidth + spacing);
   if (maxNumInRow <= 0) maxNumInRow = 1;
   ```
2. **确定标准物理间距**：
   如果标准满行容纳数 `maxNumInRow` 大于 1，则所有行的标准列间距 `standardSpacing` 应该是恒定的：
   ```cpp
   int standardSpacing = spacing;
   if (maxNumInRow > 1) {
       standardSpacing = (containerWidth - (maxNumInRow * itemWidth)) / (maxNumInRow - 1);
   }
   ```
   *注：这样既保持了网格在满行状态下两端完美贴边的紧凑对齐，又锁定了恒定不变的列间距物理常数！*

3. **按标准坐标矩阵循环排版**：
   在后续按行排布的 `while` 循环中：
   * 当前行的实际元素数量 `numInRow` 依然为 `std::min(maxNumInRow, count - i)`。
   * 排版当前行元素时，每个元素 `j` 的 X 轴坐标不使用重新计算的间距，而是使用完全一致的 `itemWidth + standardSpacing` 作为偏移量累加：
     ```cpp
     int currentX = margin;
     for (int j = 0; j < numInRow; ++j) {
         int itemIdx = i + j;
         m_geometries[itemIdx] = { QRect(currentX, currentY, itemWidth, itemHeight), itemIdx };
         currentX += itemWidth + standardSpacing;
     }
     ```
   * 如此一来，最后一行 [最后一行] 的少数量元素将完美地在左侧 [左侧] 的第 1、第 2... 列物理列线上严格垂直对齐，右侧 [右侧] 留下自然留白，彻底解决排版错乱、间距过宽、左右强行拉伸的愚蠢 bug。

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] `src/ui/JustifiedView.cpp`：仅修改 `doLayout()` 中 `m_layoutMode == GridMode` 这一逻辑分支的代码。

**明确禁止越界修改的范围：**
- [ ] 严禁修改 `JustifiedView::doLayout()` 中 `else` 分支（即 `JustifiedMode`）的自适应宽高排版计算。
- [ ] 严禁修改 `src/ui/ThumbnailDelegate.cpp` 中任何非布局相关的渲染逻辑。

## 6. 实现准则与预警【核心】
* **头文件依赖**：无需新增任何头文件，直接复用已有的 `<algorithm>`。
* **潜在高频重画抖动预防**：`doLayout()` 属于 UI 排版的核心函数，在主界面尺寸拖动、缩放滑块调节等场景下会高频触发。由于方案在进入循环前便将间距固定化，消除了对每一行进行动态除法运算的复杂度，计算开销从原先的 $O(count)$ 次除法降到了 $O(1)$ 次，百万级视图更新时的 CPU 负荷得到了进一步优化。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| GridMode 布局 | 需支持 GridMode 并通过 widget 属性 "gridMode" 解耦感知 | ✅ 符合。本方案不破坏任何既有属性或解耦标准，只修正底层 doLayout 布局坐标计算。 |

## 8. 待确认事项（可选）
无。用户已对排版对齐方案达成完全共识。
