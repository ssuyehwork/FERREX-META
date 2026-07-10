# 视图模式文件名至多折行两行与自适应限制 —— Analysis_Modification_Plan-163.md

## 1. 任务背景
为了确保软件界面在任何缩放级别下都保持整洁、规范的排版，不同模式下的项目名称需被约束为**至多折行两行**。
* **图标网格模式**：目前其 `textRect` 高度固定为 `36` 像素，已天然限制为至多两行 [1]。
* **列表模式**：随着 `Ctrl + 滚轮` 将行高放大（如 64px 至 256px），长文件名可能会发生无限折行。需要引入动态几何约束，在行高偏小时保持单行显示，行高充裕时允许至多折行两行，并在第二行中间自动进行 `...` 缩略 [1]。

## 2. 问题定位
- **模块**：`src/ui/ScanDialog.cpp`
- **位点**：`ListThumbnailDelegate::paint` 虚函数内的文字绘制段落（约第 135 - 165 行） [1]。

## 3. 详细解决方案 (代码级指引)

定位到 `ScanDialog.cpp` 内 `ListThumbnailDelegate::paint` 函数的 **第 5 步（绘制右侧文字）**，将其中的文本区域计算与绘制逻辑重构为以下**多轨自适应算法**：

```cpp
        // 5. 绘制右侧文字 (文件名)
        QString name = index.data(Qt::DisplayRole).toString();
        // 原项目设定：第0列选中的项为白色，未选中的项为蓝色 [1]
        QColor textColor = isSelected ? QColor("#FFFFFF") : QColor("#3498db");

        painter->setPen(textColor);
        painter->setFont(option.font);

        // --- 物理限制算法：至多显示两行并根据行高自适应折行 ---
        int lineHeight = option.fontMetrics.height(); // 获取当前单行高度 [1]
        int maxTextHeight = lineHeight * 2;          // 限制文字绘制区域最高为双倍行高 [1]

        // 精确对齐计算：使 2 行高的文字区域在整行内部保持垂直居中 [1]
        int textTop = option.rect.top() + (option.rect.height() - maxTextHeight) / 2;
        if (textTop < option.rect.top()) textTop = option.rect.top(); // 越界防护

        QRect textRect = option.rect;
        textRect.setLeft(squareRect.right() + 10);
        textRect.setTop(textTop);
        textRect.setHeight(maxTextHeight);

        // 临界高度判定：如果整行高度较小（例如默认的 32px 或 40px），双倍高度会导致文字被上下截断 [1]
        // 因此若行高不足以容纳 2.5 行文字，强制退化为纯单行缩略模式 [1]
        bool isSingleLineMode = (option.rect.height() < lineHeight * 2.5);

        QString elidedText;
        int flags = Qt::AlignLeft | Qt::AlignVCenter;

        if (isSingleLineMode) {
            // 单行模式：仅在 1 倍宽度内缩略，不启用折行 [1]
            elidedText = option.fontMetrics.elidedText(name, Qt::ElideMiddle, textRect.width() - 10);
        } else {
            // 双行模式：允许在 2 倍宽度内缩略，并启用 TextWordWrap 自动换行 [1]
            elidedText = option.fontMetrics.elidedText(name, Qt::ElideMiddle, (textRect.width() - 10) * 2);
            flags |= Qt::TextWordWrap;
        }

        painter->drawText(textRect, flags, elidedText);

        painter->restore();
```

---

## 4. 修改边界声明【红线】
- **严禁干涉单行退化模式下的折行**：在 `isSingleLineMode`（单行退化模式）为 `true` 时，绝对不可在 `flags` 中混入 `Qt::TextWordWrap` [1]。否则在较窄的行高下，文字会因强行折行而只显示出上半截，造成视觉残缺。
- **宽度计算偏移校准**：文本省略计算中的有效宽度应统一传入 `textRect.width() - 10` 以防文字过于贴近单元格边缘。
