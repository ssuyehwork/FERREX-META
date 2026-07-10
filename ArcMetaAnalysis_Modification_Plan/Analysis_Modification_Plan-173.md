# 网格自适应视图下文件名不超过2行重构 —— Analysis_Modification_Plan-173.md

## 1. 任务背景
在自适应（JustifiedMode）与网格（GridMode，对应用户上传的图片 `image.png` 中红圈及箭头指向部分）排版模式下，项目名称存在溢出显示为 3 行的问题（对应用户原话：“在前几任对话中，我曾要求显示项目的名称不可超过2行，但实际却显示为3行，所以需要扩大范围排查，到底是什么原因？”）。本方案旨在全面排查并重构文件名渲染与换行机制，通过引入精准的 QTextLayout 双行排版切割技术（对应用户原话：“方案A”），从根本上阻断第 3 行及以上的文本显示，保证文件名在任何拉伸或缩放场景下均严格限制在最多 2 行。

## 2. 问题定位
* **关键源文件**：`src/ui/ThumbnailDelegate.cpp`
* **关键函数**：`ThumbnailDelegate::paint`
* **问题成因**：
  1. **字符测量乘数猜测机制缺陷**：
     在 `ThumbnailDelegate::paint` 绘制文件名（对应用户原话：“显示项目的名称”）时，采用了以下代码：
     ```cpp
     option.fontMetrics.elidedText(displayName, Qt::ElideMiddle, m.textRect.width() * 2)
     ```
     `fontMetrics.elidedText` 测量的是单行非换行文本的物理像素宽度。这里传入 `width() * 2`，意图是获取最多能显示两行长度的文字。
  2. **零宽空格折行失控**：
     为了在文件名较长时能够顺利换行，系统将下划线、句点替换成了带有零宽空格的字符（如 `_\u200B` 和 `.\u200B`）。当返回的“2倍宽度单行文字”由于零宽空格在 `drawText(..., Qt::TextWordWrap)` 的自动换行逻辑作用下绘制时，如果由于单词切割导致了 3 次折行，就会渲染出 3 行（对应图片中红圈标出的 `justify-co...start.svg` 的 3 行显示）。
  3. **文字区域高度冗余**：
     文字渲染区域 `m.textRect` 的高度硬编码为了 `36px`（对应 PointSize 8 字体下单行约 11~12px，足以挤下 3 行），并且由于 `drawText` 本身不具有物理行数限制，因此只要没有发生垂直裁剪，第 3 行便会堂而皇之地显示在卡片下方（对应用户原话：“但实际却显示为3行”）。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 显示项目的名称不可超过2行，但实际却显示为3行 | 引入 `QTextLayout` 对文件名进行双行排版控制，严格物理限制最多显示两行 | ✅ 一致 |
| 2    | 方案A | 采用 QTextLayout 进行精准的“两行物理修剪”和“第二行末尾省略”的算法重构（方案 A） | ✅ 一致 |

## 4. 详细解决方案

在 `src/ui/ThumbnailDelegate.cpp` 中，废除原有的 `fontMetrics.elidedText(displayName, Qt::ElideMiddle, m.textRect.width() * 2)` 乘数猜测法绘制逻辑，改用基于 `QTextLayout` 的高精度双行切割绘制算法。

### 核心计算流程：
1. **构建并初始化 `QTextLayout`**：
   将转换后的 `displayName` 传入 `QTextLayout`，设置其字体与 `option.font` 对齐。
2. **启用软换行规则**：
   通过 `QTextOption` 设置其换行模式为 `QTextOption::WrapAtWordBoundaryOrAnywhere`。
3. **行排版循环切分**：
   在 `m.textRect.width()` 的物理宽度约束下对文本进行折行排版。
4. **双行裁剪与主动省略算法**：
   - 若排版出的行数 **不超过 2 行**（对应用户原话：“不可超过2行”）：直接使用 `painter->drawText` 逐行渲染各行文本。
   - 若排版出的行数 **超过 2 行**：
     - 第一行正常保留并绘制。
     - 获取第二行原始文本在整体字符串中的物理起始偏移，从该偏移起截取剩余的所有文本（包含可能原本属于第三行、第四行的全部长尾内容）。
     - 将截取出的长尾文本，在第二行的物理宽度（`m.textRect.width()`）下，利用 `option.fontMetrics.elidedText(..., Qt::ElideMiddle, ...)` 进行强制的物理省略（Elide），生成带有省略号的第二行文本并绘制。
     - 强行停止排版，舍弃并阻断后续任何行的物理绘制，从而将总行数绝对锁死在两行。

### 核心伪代码设计：
```cpp
// 替换原有 drawText 渲染流程
painter->save();
QString name = index.data(Qt::DisplayRole).toString();
painter->setPen(isSelected ? QColor("#3498db") : QColor("#EEEEEE"));

if (m_managedRole != -1 && !isSelected && !index.data(m_managedRole).toBool()) {
    painter->setPen(QColor(238, 238, 238, 120));
}

QFont textFont = painter->font();
textFont.setPointSize(8);
painter->setFont(textFont);

QString displayName = name;
displayName.replace("_", "_\u200B");
displayName.replace(".", ".\u200B");

// 采用方案 A：使用 QTextLayout 进行精准的双行物理修剪与第二行末尾省略
QTextLayout textLayout(displayName, painter->font());
QTextOption textOption;
textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
textOption.setAlignment(Qt::AlignCenter);
textLayout.setTextOption(textOption);

textLayout.beginLayout();
int lineCount = 0;
int textWidth = m.textRect.width() - 8; // 两侧留出 4px 安全边距 (对应 adjusted(4, 0, -4, 0))
int currentY = m.textRect.top();
int fontHeight = option.fontMetrics.height();

// 存储切分出的各行
struct RenderLine {
    QString text;
    int y;
};
QList<RenderLine> linesToRender;

while (true) {
    QTextLine line = textLayout.createLine();
    if (!line.isValid()) {
        break;
    }
    line.setLineWidth(textWidth);
    lineCount++;

    if (lineCount == 1) {
        // 第一行完整保留
        int start = line.textStart();
        int len = line.textLength();
        linesToRender.append({displayName.mid(start, len), currentY});
        currentY += fontHeight;
    } else if (lineCount == 2) {
        // 关键路径：检查是否存在第三行
        QTextLine nextLine = textLayout.createLine();
        if (nextLine.isValid()) {
            // 确实存在第三行或更多，第二行必须承接全部剩余的长尾内容，并做 ElideMiddle 裁剪
            int start = line.textStart();
            QString remainingText = displayName.mid(start);
            // 物理省略
            QString elidedRemaining = option.fontMetrics.elidedText(remainingText, Qt::ElideMiddle, textWidth);
            linesToRender.append({elidedRemaining, currentY});
        } else {
            // 没有第三行，第二行正常显示
            int start = line.textStart();
            int len = line.textLength();
            linesToRender.append({displayName.mid(start, len), currentY});
        }
        break; // 绝对阻断第三行，退出排版循环
    }
}
textLayout.endLayout();

// 物理渲染
for (const auto& rLine : linesToRender) {
    QRect lineRect(m.textRect.left() + 4, rLine.y, textWidth, fontHeight);
    painter->drawText(lineRect, Qt::AlignCenter, rLine.text);
}

painter->restore();
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] `src/ui/ThumbnailDelegate.cpp`：物理重构 `paint` 函数中的文件名渲染段，使用双行 `QTextLayout` 替代原有的 `drawText` 与 `width() * 2`。

**明确禁止越界修改的范围：**
- [ ] 禁止修改除 `ThumbnailDelegate.cpp` 以外的任何界面文件、核心控制逻辑或多线程扫描器。
- [ ] 严禁修改 `JustifiedView.cpp` 中已经对齐的卡片间距和 `GridMode` 算法逻辑。

## 6. 实现准则与预警【核心】
1. **依赖头文件**：必须引入 `<QTextLayout>` 与 `<QTextOption>` 头文件，确保其物理类型在 `ThumbnailDelegate.cpp` 中定义。
2. **文字水平居中对齐**：使用 `textOption.setAlignment(Qt::AlignCenter)` 以及在 `drawText` 绘制时保持 `Qt::AlignCenter`，确保文件名在卡片中央完美水平居中。
3. **安全间距处理**：输入框和绘制文字的安全边距物理宽度应保持 `m.textRect.width() - 8`，从而与输入重命名时的 `editorGeometry` 保持高精度对齐，避免重命名编辑框发生视觉像素跳变。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| UI考古原则 | 样式、命名、渲染风格需与现有视图高度一致，避免引入未知样式 | ✅ 符合，完美契合现有 `textRect` 像素布局 |
| 呼吸窗口与性能 | 在百万级渲染方案（Plan-150）中，应尽可能规避同步计算损耗 | ✅ 符合，本双行排版仅对当前视口可见元素执行，计算复杂度为 O(1)，无假死风险 |

## 8. 待确认事项
* 无。采用 QTextLayout 物理阻断是解决自动换行不受控最彻底、最优雅的工业级方案。
