# 列表模式行内编辑体验优化（深色皮肤与智能重命名选择） —— Analysis_Modification_Plan-160.md

## 1. 任务背景
在“列表模式”下，按下 `F2` 或双击文件名触发重命名时，由于缺乏自定义委托控制，系统会使用原生的白底输入框，且默认全选所有文本（包含后缀名），同时布局位置生硬。需对其进行重构，使重命名输入框完美融入暗黑主题，并自动排除后缀名选择，同时对齐左侧正方形缩略图。

## 2. 问题定位
- **模块**：`src/ui/ScanDialog.cpp` 内新定义的 `ListThumbnailDelegate` 委派类。
- **位点**：重写 `ListThumbnailDelegate` 类的 `createEditor`、`updateEditorGeometry` 和 `setEditorData` 三个虚函数。

## 3. 详细解决方案 (代码级指引)

在 `ListThumbnailDelegate` 中重写并追加了以下三个方法，使列表编辑器的体验与图标网格模式（ThumbnailDelegate）保持一致：

```cpp
class ListThumbnailDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // ... (保留上一版 plan-159 中已写好的 paint 绘制逻辑)

    // [优化一] 强制深色沙箱样式：杜绝亮瞎眼的系统默认白底输入框
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (editor) {
            editor->setStyleSheet(
                "background-color: #2D2D2D; color: white; "
                "selection-background-color: #3498db; "
                "border: 1px solid #3498db; border-radius: 4px; padding: 0 4px;"
            );
        }
        return editor;
    }

    // [优化二] 智能除外选择：重命名时仅高亮选择主文件名，保留后缀名免受覆盖
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        QString value = index.model()->data(index, Qt::EditRole).toString();
        QLineEdit* lineEdit = qobject_cast<QLineEdit*>(editor);
        if (lineEdit) {
            lineEdit->setText(value);
            int lastDot = value.lastIndexOf('.');
            if (lastDot > 0) {
                // 物理分割：仅选择 . 之前的字符，保留后缀不被直接覆写！
                lineEdit->setSelection(0, lastDot);
            } else {
                lineEdit->selectAll();
            }
        }
    }

    // [优化三] 精准几何对齐：编辑框左侧自动避开正方形缩略图并留出呼吸间距
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& /*index*/) const override {
        int padding = 3;
        int side = option.rect.height() - (padding * 2);
        if (side <= 0) side = 16;

        // 计算文字区域的起始左边界 (左边界 6px + 正方形边长 + 间隙 10px)
        int textLeft = option.rect.left() + 6 + side + 10;

        QRect textRect = option.rect;
        textRect.setLeft(textLeft);

        // 适当微调编辑框上下边界高度 (上下各收缩 4px 提升精致感)
        editor->setGeometry(textRect.adjusted(1, 4, -4, -4));
    }
};
```

## 4. 修改边界声明【红线】
- **严禁干涉默认事件过滤逻辑**：本优化仅限控制样式与文本选择范围。键盘拦截逻辑必须继续依赖 `ThumbnailDelegate::eventFilter` 中的全局拦截，避免破坏表格的方向键切换行行为。
