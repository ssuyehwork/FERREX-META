# 列表模式行内编辑框高度统一与居中重构 —— Analysis_Modification_Plan-162.md

## 1. 任务背景
在列表模式下，按下 `F2` 调出的重命名编辑框，其高度会随着行高的放大发生严重的非线性膨胀（可达 120px 甚至 248px），破坏了与网格模式编辑框高度（固定 26px 左右）的一致性。需要将列表模式下的编辑框高度进行物理锁定（28 像素），并在行内执行高精度垂直居中对齐 [1]。

## 2. 问题定位
- **模块**：`src/ui/ScanDialog.cpp`
- **位点**：`ListThumbnailDelegate::updateEditorGeometry` 虚函数重写部分 [1]。

## 3. 详细解决方案 (代码级指引)

在 `ListThumbnailDelegate::updateEditorGeometry` 中，引入垂直居中数学对齐逻辑：

```cpp
void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& /*index*/) const override {
    int padding = 3;
    int side = option.rect.height() - (padding * 2);
    if (side <= 0) side = 16;

    // 1. 精确计算输入框的左侧水平起点 (左边距 6px + 缩略图边长 + 间距 10px) [1]
    int textLeft = option.rect.left() + 6 + side + 10;

    // 2. 物理锁定编辑框高度为 28 像素，杜绝其随滚轮无限膨胀 [1]
    const int targetEditorHeight = 28;

    // 3. 数学垂直居中对齐计算： y_offset = (当前行高 - 目标编辑框高度) / 2 [1]
    int yOffset = (option.rect.height() - targetEditorHeight) / 2;
    int editorTop = option.rect.top() + yOffset;

    // 4. 构建完美垂直居中的 QRect 区域，右侧保留 10 像素安全边距
    int editorWidth = option.rect.width() - (textLeft - option.rect.left()) - 10;
    QRect editorRect(textLeft, editorTop, editorWidth, targetEditorHeight);

    editor->setGeometry(editorRect);
}
```

## 4. 修改边界声明【红线】
- **严禁干涉首列文本的 `paint` 区域**：此对齐逻辑仅限在 `updateEditorGeometry` 中微调重命名编辑框（editor）的几何区域 [1]。绝对禁止去改动 `paint` 函数内 `m.textRect` 的计算，否则会导致常规非编辑状态下的文本发生纵向偏移。
