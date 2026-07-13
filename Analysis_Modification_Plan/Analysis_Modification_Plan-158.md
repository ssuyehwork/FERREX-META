# 实现 Ctrl + 滚轮联动尺寸调节 —— Analysis_Modification_Plan-158.md

## 1. 任务背景
用户希望在文件列表区域（详情视图与图标视图）通过 `Ctrl + 鼠标滚轮` 实现类似 Windows 资源管理器的缩放功能。该功能需直接驱动标题栏现有的“尺寸滑动条”（m_sizeSlider）。

## 2. 问题定位
- **模块**：`src/ui/ScanDialog.cpp`
- **位点**：`ScanDialog::eventFilter` (约第 2015 行起)

## 3. 详细解决方案 (代码级指引)

### 3.1 滚轮事件拦截逻辑
在 `eventFilter` 中对列表视图（m_resultView）和图标视图（m_iconView）增加以下逻辑片段：

```cpp
// 伪代码逻辑指引：
if ((watched == m_resultView || watched == m_iconView) && event->type() == QEvent::Wheel) {
    QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
    
    // 拦截 Ctrl + 滚轮
    if (wheelEvent->modifiers() & Qt::ControlModifier) {
        int delta = wheelEvent->angleDelta().y();
        int step = 10; // 建议步进 10 像素
        
        if (delta > 0) {
            // 向上滚：放大
            m_sizeSlider->setValue(m_sizeSlider->value() + step);
        } else {
            // 向下滚：缩小
            m_sizeSlider->setValue(m_sizeSlider->value() - step);
        }
        
        return true; // 拦截事件，防止触发列表的垂直滚动
    }
}
```

### 3.2 联动原理说明
由于 `m_sizeSlider` 在构造函数中已连接了 `valueChanged` 信号（处理函数见 `src/ui/ScanDialog.cpp` 第 713-722 行），该函数会自动执行：
1. 更新全局配置 `m_config.iconSize`。
2. 物理调整图标视图的 `setTargetRowHeight`。
3. 清理旧尺寸的缩略图缓存 `clearThumbCache`。
4. 触发模型重绘 `updateResults`。
5. 自动执行配置保存 `m_config.save()`。

通过 `m_sizeSlider->setValue()` 进行驱动是实现该交互增强任务逻辑一致性最高、代码改动最小的最优解。

## 4. 修改边界声明【红线】
- **禁止越界修改**：严禁在 `eventFilter` 之外手动编写独立的缩放渲染函数。必须走 `m_sizeSlider` 联动链路。
