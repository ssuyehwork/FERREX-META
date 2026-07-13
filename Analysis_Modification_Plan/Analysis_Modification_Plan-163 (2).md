# 缩略图渐进式占位与平滑缩放消除闪烁功能方案 —— Analysis_Modification_Plan-163.md

## 1. 任务背景
当用户在 FERREX 搜索结果界面通过 `Ctrl + 滚轮` 高频调节图标尺寸或刷新数据时，原本的加载渲染流水线存在显著的“视觉闪烁感”（闪烁默认文件图标，之后才延迟变为缩略图）。这是因为每次缩放或刷新都会将缩略图高速缓存物理清空。当视图重新对可见项发起渲染索取时，在后台异步线程池计算出新尺寸的图画并返回给主线程之前，由于主线程不能产生任何同步等待阻塞，因而立刻回退并返回了通用的系统类型图标。

为给用户提供最顶级、最丝滑、无白色或彩色文件图标闪烁的缩放过渡，本方案将在 `ScanTableModel` 引入 **模糊尺寸匹配（物理渐进式占位）** 与 **非物理销毁型高速缓存双轨架构**，让旧尺寸的缩略图在过渡期内作为临时占位拉伸显示，从而打造极佳的图形软件交互体感。

## 2. 问题定位
* **根因 1：高频全量擦除**
  在 `m_sizeSlider` 触发的槽函数内，调用了 `m_tableModel->clearThumbCache()`。这粗暴地销毁了之前所有尺寸的图画内存，导致连拉伸旧图作为临时占位的基本物料也不复存在。
* **根因 2：单一尺寸精确匹配链条**
  在 `ScanTableModel::data` 处理 `Qt::DecorationRole` 时，仅通过当前尺寸 `thumbSize` 构建的 `cacheKey`（例如 `FRN_size_mtime`）在 `m_thumbCache` 中进行精确查询。一旦未命中，直接退出并调用了 `reader.getCachedIcon(...)` 默认图标。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 我要的是先显示缩略图，然后才显示图标，也就是先判断缩略图，之后才判断图标，这样的方式可行不？ | 在获取装饰角色时，优先精准尺寸判断 -> 模糊尺寸(历史拉伸)判断 -> 实在没有才回退到系统图标。此方式 100% 可行且能带来顶级丝滑效果。 | ✅ |

## 4. 详细解决方案

### 第一步：在 `src/ui/ScanDialog.h` 中升级高速缓存为双轨架构
在 `ScanTableModel` 内部新增一个轻量级的双轨高速 LRU 缓存 `m_lastPixmapCache`。该缓存的 Key **不包含尺寸 (size)**，仅绑定文件唯一标识（FRN `key`），永远只保存该文件“上一次渲染成功的最新的 Pixmap”副本。

同时修改 `clearThumbCache()` 的清空规则。当执行 `Ctrl + 滚轮` 调节尺寸时，我们应当保留 `m_lastPixmapCache` 历史资产，以充当平滑过渡的拉伸源；只有当用户重置扫描、更换驱动器或更换目录时，才物理清空全部缓存。

**修正对比片段 1 (高速缓存声明扩展)：**
```cpp
// src/ui/ScanDialog.h

<<<<<<< SEARCH
    mutable QCache<QString, QPixmap> m_thumbCache;
    mutable QSet<uint64_t> m_requestedThumbs;
    mutable QMap<uint64_t, double> m_aspectRatios; // 存储宽高比
=======
    mutable QCache<QString, QPixmap> m_thumbCache;
    mutable QCache<QString, QPixmap> m_lastPixmapCache; // 2026-07-xx 渐进式占位双轨缓存 (Key 为 QString::number(key))
    mutable QSet<uint64_t> m_requestedThumbs;
    mutable QMap<uint64_t, double> m_aspectRatios; // 存储宽高比
>>>>>>> REPLACE
```

**修正对比片段 2 (清除缓存规则细分分流)：**
我们为 `clearThumbCache` 新增一个可选布尔参数 `keepLastCache`（默认为 `false`）。在滚轮缩放引起的更新中，设为 `true` 以保留最后的图像缓冲。

```cpp
// src/ui/ScanDialog.h

<<<<<<< SEARCH
    void clearThumbCache() { 
        m_thumbCache.clear(); 
        m_requestedThumbs.clear(); 
        m_thumbTaskQueue.clear();
    }
=======
    void clearThumbCache(bool keepLastCache = false) { 
        m_thumbCache.clear(); 
        m_requestedThumbs.clear(); 
        m_thumbTaskQueue.clear();
        if (!keepLastCache) {
            m_lastPixmapCache.clear();
        }
    }
>>>>>>> REPLACE
```

### 第二步：在 `src/ui/ScanDialog.cpp` 中微调尺寸滑块的触发信号
将 `Ctrl + 滚轮` 以及滑块拉动时的 `clearThumbCache` 替换为 `clearThumbCache(true)`，以此妥善维护我们的过渡缓冲资源。

**修正对比片段：**
```cpp
// src/ui/ScanDialog.cpp

<<<<<<< SEARCH
            connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int v) { 
                m_config.iconSize = v; 
                m_resultView->verticalHeader()->setDefaultSectionSize(v); 
                m_iconView->setTargetRowHeight(v); 
                m_tableModel->clearThumbCache(); 
                m_tableModel->updateResults(); // 确保触发重新加载并生成新尺寸的缩略图
                m_config.save(); 
            }); 
=======
            connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int v) { 
                m_config.iconSize = v; 
                m_resultView->verticalHeader()->setDefaultSectionSize(v); 
                m_iconView->setTargetRowHeight(v); 
                m_tableModel->clearThumbCache(true); // 保留上一次的历史 Pixmap 资产用作渐进拉伸占位
                m_tableModel->updateResults(); // 确保触发重新加载并生成新尺寸的缩略图
                m_config.save(); 
            }); 
>>>>>>> REPLACE
```

### 第三步：在 `ScanTableModel::data` 中实现“先判断缩略图，后退化到图标”
重构获取 `Qt::DecorationRole` 角色分支的决策顺序树。通过两级缓存匹配（精确尺寸匹配 -> 任何尺寸模糊拉伸占位匹配 -> 系统默认图标）实现完美的视觉无缝过渡。

**修正对比片段：**
```cpp
// src/ui/ScanDialog.cpp

<<<<<<< SEARCH
            QPixmap* cached = m_thumbCache.object(cacheKey);
            if (cached) return *cached;

            if (!m_requestedThumbs.contains(key)) {
                m_requestedThumbs.insert(key);
                ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                int thumbSize = (dlg && dlg->m_viewStack->currentIndex() == 0) ? 24 : (dlg ? dlg->m_config.iconSize : 64);
                
                // 2026-06-xx 极致架构：加入并行批处理队列，废除“单请求单线程”模式
                m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                if (!m_thumbTimer->isActive()) m_thumbTimer->start();
            }
        }
        return reader.getCachedIcon(ext, reader.isDirectory(actualIndex));
=======
            // 限制双轨缓存的最大容量，防止极端高频缩放累积内存
            if (m_lastPixmapCache.maxCost() == 0) {
                m_lastPixmapCache.setMaxCost(200); // 默认限制 200 项可见卡片 LRU 备份
            }

            // 1. 精确尺寸缓存匹配
            QPixmap* cached = m_thumbCache.object(cacheKey);
            if (cached) return *cached;

            // 2.【核心改进：先判断历史缩略图并做平滑拉伸】
            QPixmap* lastCached = m_lastPixmapCache.object(QString::number(key));
            if (lastCached) {
                // 后台静默生成符合全新精确尺寸的高画质大图
                if (!m_requestedThumbs.contains(key)) {
                    m_requestedThumbs.insert(key);
                    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                    int thumbSize = (dlg && dlg->m_viewStack->currentIndex() == 0) ? 24 : (dlg ? dlg->m_config.iconSize : 64);
                    m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                    if (!m_thumbTimer->isActive()) m_thumbTimer->start();
                }

                // 将历史多态尺寸的旧缩略图临时拉伸/缩放到当前目标尺寸，直接返回！
                ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                int thumbSize = (dlg && dlg->m_viewStack->currentIndex() == 0) ? 24 : (dlg ? dlg->m_config.iconSize : 64);
                return lastCached->scaled(QSize(thumbSize, thumbSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }

            // 3. 首次加载（完全没有生成过任何缩略图），触发异步处理并回退
            if (!m_requestedThumbs.contains(key)) {
                m_requestedThumbs.insert(key);
                ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                int thumbSize = (dlg && dlg->m_viewStack->currentIndex() == 0) ? 24 : (dlg ? dlg->m_config.iconSize : 64);
                
                m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                if (!m_thumbTimer->isActive()) m_thumbTimer->start();
            }
        }
        // 4.【最后才判断图标】实在没有任何缩略图记录，才回退到系统默认图标
        return reader.getCachedIcon(ext, reader.isDirectory(actualIndex));
>>>>>>> REPLACE
```

### 第四步：在异步缩略图生成完毕后同步登记副本
当后台线程池完成精确大图的高清渲染，将新生成的 `QPixmap` 写入精确缓存时，同步向 `m_lastPixmapCache` 覆盖更新此文件的最新形态：

**修正对比片段：**
```cpp
// src/ui/ScanDialog.cpp

<<<<<<< SEARCH
                    QPixmap pix = QPixmap::fromImage(img);
                    if (!pix.isNull()) {
                        m_thumbCache.insert(cacheKey, new QPixmap(pix));
                    }
                    m_aspectRatios[key] = ar;
=======
                    QPixmap pix = QPixmap::fromImage(img);
                    if (!pix.isNull()) {
                        m_thumbCache.insert(cacheKey, new QPixmap(pix));
                        m_lastPixmapCache.insert(QString::number(key), new QPixmap(pix)); // 实时注册副本，作为下一次调节时的渐进拉伸源
                    }
                    m_aspectRatios[key] = ar;
>>>>>>> REPLACE
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 头文件：`src/ui/ScanDialog.h`（新增 `m_lastPixmapCache` 缓存；扩展 `clearThumbCache` 参数逻辑）
- [ ] 源文件：`src/ui/ScanDialog.cpp`（修正滑块槽函数，重构 `data` 的匹配流程树，并在主线程登记中加入副本注册）

**明确禁止越界修改的范围：**
- [ ] 严禁侵入后台 `m_thumbPool` 线程池的非 UI 操作；严禁对 `reader.getCachedIcon` 进行非中性改写。

## 6. 实现准则与预警【核心】
1. **LRU 容量严控**：`m_lastPixmapCache` 采用 LRU (Least Recently Used) 策略淘汰。如果对百万级图片进行连续缩放，若不设定限制则会耗尽内存。因此我们在 `data` 的入口处通过对 `maxCost()` 的常态化检测，强制将其上限限制为 `200`（可见视口卡片一般在 10~50 之间，200 可完全冗余包含上下滚动区域），在保证平滑缩放的前提下将多余内存物理强制释放。
2. **生命周期完美重合**：当用户切换了目标搜索盘符或打开其他新物理路径时，旧路径的缩略图没有作为拉伸源的语义。我们在 `clearThumbCache` 默认参数中设置了 `keepLastCache = false`，从而使新物理扫描启动时，旧缓存能够被 100% 物理清除，杜绝交叉污染。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 缩略图加载性能瓶颈 | 针对缩略图进行 LIFO 栈式优先调度并压榨并发。 | ✅ 符合，完美兼容已有 LIFO 调度并大幅消除缩放时的图标闪烁，交互性能再次跃迁。 |

## 8. 待确认事项（可选）
* 无
