# “自适应”视图模式行为完全还原为旧版本-1 —— Analysis_Modification_Plan-188.md

## 1. 任务背景
在项目迭代与功能重构过程中，当前版本对“自适应”视图模式（Justified Mode）的排版逻辑进行了调整。新增了对于视频、图形图像等媒体文件的特定自适应拉伸限制，并对常规文件（如 AHK、TXT 等文本/代码、文件夹等非媒体文件）实施了强制固定正方形尺寸排布和整行拉伸填满限制机制。由于该处的重构未能契合用户对纯粹“自适应”视图行为的最初追求，并且当前的改动机制相对繁杂，容易在边缘情况或混合布局下造成崩溃或严重的样式不协调。因此，用户下达了将“自适应”视图模式严格按照“旧版本-1”中原生、优美的逻辑形态进行恢复的修改指令。

## 2. 问题定位
当前问题主要源自 `src/ui/JustifiedView.cpp` 中 `JustifiedView::doLayout()` 方法。
具体问题代码位于针对 `JustifiedMode` 的 `else` 逻辑分支中，当前逻辑增加了如下的限制和阻断代码：
1. **多余的常规文件判定（`isRegular`）**：引入了 `isRegularFlags` 与 `isRegular` 阈值判断（`ar <= 0.01`），并将这些常规文件的比例全部固定成 `1.0` 的标准物理正方形。
2. **多余的整行拉伸过滤（`containsRegular`）**：在行布局高度计算前进行物理防错校验，如果行内存在任意常规非自适应文件（`containsRegular`），整行便直接关闭 `rowIsJustified`（拉伸填满特性）。这直接破坏了所有类型的文件自适应横向排满视口的视觉特性。

上述两项限制造成了当前的排版效果货不对板。而这些新增变量与流程在“旧版本-1”中是不存在的。因此，我们需要将整个布局循环完全还原为旧版本-1中纯粹通过 `ar <= 0` 判定且只在 `!isLastRow` 时触发 `rowIsJustified` 的流畅模式。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 由于在修改代码的时候，当前版本被严重破坏了，所以需要严格按照旧版本-1的方式来修复（对应用户原话） | 4. 详细解决方案 中将 `JustifiedView::doLayout()` 的 `JustifiedMode` 逻辑段还原至“旧版本-1”的原生写法 | ✅ |
| 2    | 不在本次范围内的是任何对源代码的物理修改，仅产出分析与方案文档（对应我的理解） | 方案仅作为分析与方案文档落盘并提供精准伪代码（对应 5. 修改边界声明【红线】） | ✅ |

## 4. 详细解决方案

我们应当对 `src/ui/JustifiedView.cpp` 中的 `JustifiedView::doLayout()` 逻辑进行针对性还原。以下是需要更新的重点修改内容：

### 修改 `src/ui/JustifiedView.cpp` 如下：

<<<<<<< SEARCH
    } else {
        // JustifiedMode 逻辑：仅对媒体（视频、图形图像）实施自适应宽高拉伸，常规文件保持标准固定正方形排布 (对应用户原话：“所谓的自适应仅限于视频、图形图像，除此之外仅剩下常规文件类型了”)
        int i = 0;
        while (i < count) {
            int rowStart = i;
            double rowAspectRatioSum = 0;
            std::vector<double> aspectRatios;
            std::vector<bool> isRegularFlags; // 记录每个项目是否为常规非自适应文件
 
            while (i < count) {
                double ar = model()->data(model()->index(i, 0), m_aspectRatioRole).toDouble();
                
                // 【核心修复】：物理加固判定。如果 ar < 0（显式常规文件），或者 ar <= 0.01（无效数据/未载入占位符）
                // 则一律视为常规文件，禁止任何自适应宽度拉伸 [1, 2]
                bool isRegular = (ar <= 0.01); 
                if (isRegular) {
                    ar = 1.0; // 常规文件和文件夹比例恒定视为 1.0 的标准物理正方形 [1, 2]
                }
                
                aspectRatios.push_back(ar);
                isRegularFlags.push_back(isRegular);
                rowAspectRatioSum += ar;
                
                int numInRow = (int)aspectRatios.size();
                double estimatedWidth = (rowAspectRatioSum * m_targetRowHeight) + (6 * numInRow) + (spacing * (numInRow - 1));
                if (estimatedWidth > containerWidth) {
                    if (numInRow > 1) {
                        aspectRatios.pop_back();
                        isRegularFlags.pop_back();
                        rowAspectRatioSum -= ar;
                    } else {
                        i++;
                    }
                    break; 
                }
                i++;
            }
 
            int rowEnd = i;
            int numInRow = rowEnd - rowStart;
            if (numInRow <= 0) break;
 
            int actualHeight = m_targetRowHeight;
            bool isLastRow = (i == count);
 
            // 核心物理防错判断 (对应用户原话：“其中的“自适应”视图模式存在傻逼逻辑架构 ... 还是计算存在傻逼逻辑？”)：
            // 只要行内包含任何常规非自适应文件（如 AHK、TXT 等），或者属于最末一行，整行立即关闭拉伸填满特性！
            // 这确保了常规文件在任何时候都保持完美的标准正方形尺寸，而不会被强制撑大拉宽，更不会产生累积宽度偏差导致的负值 and 异常坍塌。
            bool containsRegular = false;
            for (bool isReg : isRegularFlags) {
                if (isReg) {
                    containsRegular = true;
                    break;
                }
            }
            bool rowIsJustified = !isLastRow && !containsRegular;
 
            int availableImageWidth = containerWidth - (spacing * (numInRow - 1)) - (6 * numInRow);
=======
    } else {
        // JustifiedMode 逻辑保持原有自适应宽高
        int i = 0;
        while (i < count) {
            int rowStart = i;
            double rowAspectRatioSum = 0;
            std::vector<double> aspectRatios;
 
            while (i < count) {
                double ar = model()->data(model()->index(i, 0), m_aspectRatioRole).toDouble();
                if (ar <= 0) ar = 1.0;
                
                aspectRatios.push_back(ar);
                rowAspectRatioSum += ar;
                
                int numInRow = (int)aspectRatios.size();
                double estimatedWidth = (rowAspectRatioSum * m_targetRowHeight) + (6 * numInRow) + (spacing * (numInRow - 1));
                if (estimatedWidth > containerWidth) {
                    if (numInRow > 1) {
                        aspectRatios.pop_back();
                        rowAspectRatioSum -= ar;
                    } else {
                        i++;
                    }
                    break; 
                }
                i++;
            }
 
            int rowEnd = i;
            int numInRow = rowEnd - rowStart;
            if (numInRow <= 0) break;
 
            int actualHeight = m_targetRowHeight;
            bool isLastRow = (i == count);
            bool rowIsJustified = !isLastRow;
 
            int availableImageWidth = containerWidth - (spacing * (numInRow - 1)) - (6 * numInRow);
>>>>>>> REPLACE

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/JustifiedView.cpp` (方案设计阶段，物理代码禁止修改)

**明确禁止越界修改的范围：**
- [ ] 物理修改任何 `.cpp` / `.h` / `.cmake` 代码文件均被强制禁绝（Jules 分析师硬红线）。
- [ ] 禁止修改除 `JustifiedView` 自适应（Justified Mode）视图模式之外的其他视图（如列表视图、网格视图）以及其他不相关的业务代码。

## 6. 实现准则与预警【核心】

1. **零功能性代码修改警告**：本方案作为纯分析方案文档归档，任何人若需执行修复，应严格按照第 4 节中的 Diff 段进行精确替换。
2. **头文件依赖**：自适应还原逻辑完全保留原有的数据结构和字段，没有依赖任何新增头文件，因此还原后绝对不会出现“找不到标识符”等编译错误。
3. **性能开销预警**：移除了多余的 `isRegularFlags` 等局部容器构造和循环遍历开销，使得在大规模数据集下的自适应排版性能更加敏捷和轻量化。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 自适应视图模式还原 | 严格按照旧版本-1的方式恢复原生自适应宽高排布，不施加媒体类型限制 | ✅ 符合 |

## 8. 待确认事项（可选）
（无）
