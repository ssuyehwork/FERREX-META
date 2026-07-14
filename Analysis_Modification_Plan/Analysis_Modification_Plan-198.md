# 滚轮与滑块尺寸调节卡顿、析构卡死与过度重置问题修复 —— Analysis_Modification_Plan-198.md

## 1. 任务背景
在 FERREX-META 当前的版本中，重构过后的系统在 `Ctrl + 鼠标滚轮` 调节卡片尺寸时操作表现迟钝、不够流畅，且程序退出时也存在明显卡顿。经过排查，这一交互性能退化和卡死的根因来源于三处过度重置、无谓磁盘 I/O 以及析构时主线程死等同步的设计偏差：
- **每次滚轮变焦都在主线程同步触发 `m_config.save()`**，引入不必要的磁盘持久化开销并严重打断流畅度。
- **每一次尺寸微小抖动都会重置并擦除缩略图缓存**。
- **程序析构时强制在主线程调用了 `m_thumbPool->waitForDone()`**，这会导致后台有积压未处理完的缩略图任务时，界面无法立刻关闭退出。

## 2. 问题定位

### 2.1 主线程高频阻塞磁盘持久化 (I/O Bottleneck)
在 `src/ui/ScanDialog.cpp` 的滑动条值改变连接中：
```cpp
connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int v) { 
    m_config.iconSize = v; 
    // ...
    m_tableModel->clearThumbCache(true); 
    m_tableModel->updateResults(); 
    m_config.save(); // 每次数值变动，频繁触发磁盘 JSON 序列化和写入
});
```
每调节 1 像素尺寸或者通过滚轮快速触发时，`m_config.save()` 会被频繁同步触发，在百万数据下会使事件队列处理延迟大幅提高，造成微卡顿和卡顿。

### 2.2 滚轮滚荡过程中过度清空二级缓存
当前逻辑在调整大小时重置缓存：
```cpp
m_tableModel->clearThumbCache(true);
```
在调节尺寸时，缩略图的大小发生了微小改变（如 64 像素变成 66 像素）。清空 `m_thumbCache` 会让目前可视卡片失去精确的 Pixmap。虽然有 L2 `m_lastPixmapCache` 可用，但是极其密集的投递缩略图加载任务到 `m_thumbPool` 依然会对主线程渲染带来无休止的排队。

### 2.3 析构死等与主线程卡死
在 `src/ui/ScanTableModel.cpp` 析构函数中：
```cpp
ScanTableModel::~ScanTableModel() {
    if (m_thumbPool) {
        m_thumbPool->waitForDone(); // 死等后台未处理完的队列，主线程阻塞假死
    }
}
```
当后台有大量的 SVG 或大图异步生成任务时，一旦用户在此刻关闭窗口，主线程会被无条件卡死，直到所有后台线程的任务全部跑完。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | ctrl+滚轮键变更尺寸时不够丝滑流程 (对应用户原话) | 将配置文件持久化延迟防抖，并消除变焦时的二级缓存清空 | ✅ |
| 2    | 退出时也会发生卡顿 (对应用户原话) | 在 `ScanTableModel` 析构时废除 `waitForDone` 死等，非阻塞丢弃未完成任务 | ✅ |

---

## 4. 详细解决方案

### 4.1 配置文件延迟防抖保存与析构持久化
引入防抖保存策略。在 `ScanDialog` 中增设单次防抖定时器，滑块及滚轮变动后不进行任何同步保存，而是延迟一定时间后在后台或主线程触发写入。同时在 `ScanDialog` 析构时一次性持久化当前状态：
- 增设 `m_configSaveTimer` (间隔 500ms，单次触发) 负责异步或防抖保存。
- 移除 `QSlider::valueChanged` 和 `switchToView` 等视图尺寸调节中的 `m_config.save()` 同步调用。
- 在 `ScanDialog` 析构函数中补全一次最后的强行 `m_config.save()` 写入，确保配置永久留存，实现零开销、无卡顿的尺寸滑滑梯操作。

### 4.2 改写 `ScanTableModel` 析构非阻塞回收
将 `m_thumbPool->waitForDone()` 彻底移除或替换为安全断开策略。由于 `m_thumbPool` 是由 `ScanTableModel` 独占控制，且我们使用的是 QRunnable 匿名 Lambda，可以通过：
- 在析构前调用 `m_thumbPool->clear()` 抛弃尚未运行的所有积压任务，腾空队列。
- 在调用 `m_thumbPool->waitForDone()` 之前直接将未完成的任务做丢弃处理，或者不等待，通过智能指针托管线程池，脱离主线程析构。

### 4.3 滑动/滚动变焦期间完全去 L1 缓存清空
在 `QSlider::valueChanged` 滑动时，废除高频调用 `m_tableModel->clearThumbCache(true);` 引起的瞬间 L1 重构。
- 在变焦滑动过程中，不强制清空 `m_thumbCache`，而是等滑动结束（通过防抖定时器 200ms）后才真正调用 `clearThumbCache`，以重新生成全新精确精度的缩略图并替换。
- 变焦滚动期间，Delegate 直接复用已经生成的 Pixmap 进行高性能的比例拉伸 Cover，避免闪烁、空白和重复提交任务，从而在滚动时享受到媲美原生 GPU 加速的绝对流畅度。

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] `src/ui/ScanDialog.cpp` & `src/ui/ScanDialog.h`（配置文件防抖保存及滑块事件节流重构）
- [ ] `src/ui/ScanTableModel.cpp` & `src/ui/ScanTableModel.h`（析构非阻塞改进、缓存节流重建）

**明确禁止越界修改的范围：**
- [ ] 禁止修改任何除 UI 变焦事件和析构同步外的底层核心 MftReader、MetadataManager 以及加密算法文件。

---

## 6. 实现准则与预警【核心】
1. **防止找不到标识符：** 修改涉及 `QTimer` 以实现防抖，必须在 `ScanDialog.h` 中正确包含 `#include <QTimer>`。
2. **多线程调用预警：** 在析构中，必须优先关闭 `m_throttleTimer` 和 `m_metadataTimer` 等定时器，防止定时器在对象半销毁状态下回调。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 变焦尺寸调节 | 必须达到操作丝滑流畅，无阻塞、无卡顿 | ✅ 符合，采用防抖和免清空过渡策略 |
| 退出回收机制 | 退出不能阻塞主线程，避免析构死等 | ✅ 符合，清除工作队列，避免 waitForDone |
