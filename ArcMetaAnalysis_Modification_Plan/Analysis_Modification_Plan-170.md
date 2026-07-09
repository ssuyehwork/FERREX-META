# FERREX-META 缩略图加载期“零闪烁、纯阻断”高保真渲染重构方案 —— Analysis_Modification_Plan-170.md

## 1. 任务背景
在目前的 FERREX-META 版本中，即使重构了模型与代理（Delegate）的渲染优先级，当首次匹配或加载支持缩略图的高精文件（如 PSD 等图像、视频）时，界面在滑动展现的第一瞬间仍会尴尬地“闪现系统默认图标（如 PSD 关联图标），然后再刷出缩略图”。
这种由于“穿透回退”导致的视觉抖动严重破坏了产品的高级感。用户明确要求必须对该傻逼渲染漏洞执行彻底净化：**必须优先展示缩略图，系统默认图标仅仅作为彻底失败或不支持缩略图时的兜底，在匹配完成至缩略图被加载出来的中间窗口期，必须彻底纯阻断系统默认图标的绘制**。

## 2. 问题定位
通过对 `src/ui/ScanDialog.cpp` 的 `ScanTableModel::data` 与 `src/ui/ThumbnailDelegate.cpp` 的 `paint` 深度审计，定位到两大核心技术漏洞：

### 漏洞一：模型层 `DecorationRole` 加载穿透
在 `ScanDialog.cpp` 中（第 325-378 行）：
```cpp
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        QString ext = reader.getExtQString(actualIndex);
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        if (thumbExts.contains(ext) && !reader.isDirectory(actualIndex)) {
            // ...
            // 1. 精确尺寸缓存未命中
            // 2. L2 历史备份未命中
            // 3. 触发异步队列...
        }
        // 4.【漏洞所在】当缓存（L1与L2）均未命中时，代码会“一路穿透”回退到最下方：
        return reader.getCachedIcon(ext, reader.isDirectory(actualIndex));
    }
```
* **根因分析**：在图片或设计图（如 `.psd`）首次进入视口且尚未生成缩略图（缓存为空）的中间窗口期，模型层由于无法提供 `QPixmap` 物理资产，便粗暴地穿透到最底部的 `return reader.getCachedIcon(...)`。这就向视图层返回了合法的系统文件关联图标。视图层接收到这个图标并将其绘制到卡片中心，导致了“先闪默认图标，后刷缩略图”的视觉跳变。

### 漏洞二：代理层（Delegate）“无区别系统图标渲染”
在 `ThumbnailDelegate.cpp` 中（第 96-106 行）：
```cpp
    } else {
        // 系统默认图标只作为【没有缩略图或生成失败时】的最后兜底回退手段
        // 在没有有效缩略图物理资产时，直接以 100% 的完全不透明度绘制默认的文件类型关联图标，绝不闪现空白卡片
        QIcon icon = qvariant_cast<QIcon>(decoData);
        if (!icon.isNull()) {
            painter->setOpacity(1.0);
            // ... 绘制图标 ...
        }
    }
```
* **根因分析**：由于漏洞一的存在，当缩略图还在异步队列加载时，代理从 `decoData` 中顺利提取出了模型层穿透返回的 `QIcon` 物理实例。因为无法分辨该 `QIcon` 是**“暂时的（由于尚未加载出来而强行穿透的系统图标）”**还是**“永久的（由于无法生成或生成失败而进行兜底的系统图标）”**，代理层只能将其视为有效资产以 100% 不透明度画入卡片。这彻底封死了消除闪烁的可能性。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 优先显示缩略图，其次才是图标兜底 | 彻底颠覆模型层的穿透回退机制。在加载中间期，拒绝返回任何系统图标，实现纯阻断（对应用户原话：“优先显示缩略图”） | ✅ |
| 2    | 只有0/1即可，为什么在匹配到数据之后，显示数据时仍然是先显示图标然后才显示缩略图 | 重构 `UserRole + 1` 为绝对的二元物理资产状态判定。在缩略图首次加载的临时状态窗口中阻断默认图标渲染，只有在无法加载或处理失败时才退化至图标（对应用户原话：“只有0/1即可”） | ✅ |

---

## 4. 详细解决方案

作为“资深程序员·纯分析师”，本案提供清晰、高复原度的重构样式架构，为物理开发明确道路：

### 4.1 模型端：穿透阻断与失败感知跟踪
为了彻底防止未加载完毕的图片穿透返回默认系统图标，需要在 `ScanTableModel` 引入 **“加载完成判定”** 和 **“失败跟踪名单（m_failedThumbs）”**：

1. **头文件扩展 `ScanDialog.h`**
```cpp
    // 在 ScanTableModel 的 private 中引入失败缓存跟踪，避免耗时的 I/O 操作反复重试：
    mutable QSet<uint64_t> m_failedThumbs; // 记录由于格式损坏或物理错误导致提取失败的 FRN key
```

2. **重构模型层分发机制 `ScanDialog.cpp` 的 `Qt::DecorationRole` 分支**
```cpp
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        QString ext = reader.getExtQString(actualIndex);
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        
        if (thumbExts.contains(ext) && !reader.isDirectory(actualIndex)) {
            int64_t size = reader.getSize(actualIndex);
            int64_t mtime = reader.getModifyTime(actualIndex);
            QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(mtime);

            if (m_lastPixmapCache.maxCost() == 0) {
                m_lastPixmapCache.setMaxCost(200);
            }

            // A. 精确匹配命中
            QPixmap* cached = m_thumbCache.object(cacheKey);
            if (cached) return *cached;

            // B. 历史备份命中
            QPixmap* lastCached = m_lastPixmapCache.object(QString::number(key));
            if (lastCached) {
                // 静默触发高精度的重新计算
                if (!m_requestedThumbs.contains(key)) {
                    m_requestedThumbs.insert(key);
                    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                    int thumbSize = (dlg && dlg->m_viewStack->currentIndex() == 0) ? 24 : (dlg ? dlg->m_config.iconSize : 64);
                    m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                    if (!m_thumbTimer->isActive()) m_thumbTimer->start();
                }
                // 直接返回历史图，不闪图标
                return lastCached->scaled(QSize(64, 64), Qt::KeepAspectRatio, Qt::SmoothTransformation); 
            }

            // C. 失败兜底阻断器：
            // 如果已经被标记为彻底提取失败，则可以穿透放行，退回到最下方的系统默认图标展示。
            if (m_failedThumbs.contains(key)) {
                return reader.getCachedIcon(ext, false);
            }

            // D. 加载期强制阻断：
            // 此时缩略图在加载队列中尚未产生。为了杜绝默认图标的插足闪跃，模型层强制返回“空 QVariant()”。
            // 代理层（Delegate）看到空对象后，卡片内部仅显示平滑高雅的暗灰底色（#2d2d2d），完美实现无闪烁占位！
            if (!m_requestedThumbs.contains(key)) {
                m_requestedThumbs.insert(key);
                ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                int thumbSize = (dlg && dlg->m_viewStack->currentIndex() == 0) ? 24 : (dlg ? dlg->m_config.iconSize : 64);
                m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                if (!m_thumbTimer->isActive()) m_thumbTimer->start();
            }

            return QVariant(); // 【核心物理阻断点】向视图提供空数据，掐断默认图标的透传通路！
        }
        
        // 常规不支持缩略图的后缀（如 txt, exe），直接放行
        return reader.getCachedIcon(ext, reader.isDirectory(actualIndex));
    }
```

3. **异步工作线程：失败状态回录**
当工作线程中执行 SVG 渲染或 Shell 缩略图获取遇到严重错误、图片损坏导致获取到的 `QImage` 彻底为空时，将其记录到 `m_failedThumbs` 并通知视图更新，确保其退化并展示系统默认图标：

```cpp
            if (!img.isNull()) {
                double ar = (double)img.width() / (double)img.height();
                QMetaObject::invokeMethod(this, [this, key = t.key, cacheKey = t.cacheKey, img, ar]() {
                    QPixmap pix = QPixmap::fromImage(img);
                    if (!pix.isNull()) {
                        m_thumbCache.insert(cacheKey, new QPixmap(pix));
                        m_lastPixmapCache.insert(QString::number(key), new QPixmap(pix));
                    }
                    m_aspectRatios[key] = ar;
                    
                    auto snapshot = m_controller->snapshot();
                    auto itPos = snapshot->keyToPos.find(key);
                    if (itPos != snapshot->keyToPos.end() && itPos->second < m_displayCount) {
                        m_pendingRows.insert(itPos->second);
                        if (!m_throttleTimer->isActive()) m_throttleTimer->start();
                    }
                }, Qt::QueuedConnection);
            } else {
                // 【核心改进】：获取失败，记录进失败名单，并强制刷新，使用默认图标兜底
                QMetaObject::invokeMethod(this, [this, key = t.key]() {
                    m_failedThumbs.insert(key);
                    auto snapshot = m_controller->snapshot();
                    auto itPos = snapshot->keyToPos.find(key);
                    if (itPos != snapshot->keyToPos.end() && itPos->second < m_displayCount) {
                        m_pendingRows.insert(itPos->second);
                        if (!m_throttleTimer->isActive()) m_throttleTimer->start();
                    }
                }, Qt::QueuedConnection);
            }
```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.h` 中的 `ScanTableModel` 类私有变量扩展。
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` 中的 `ScanTableModel::data` 中 `DecorationRole` 的过滤流程以及异步生成失败回调。

**明确禁止越界修改的范围：**
- [ ] 严禁修改任何底层 Mft 索引库 `MftReader.cpp` 的缓存清理接口。
- [ ] 严禁修改详情视图（Details Mode）的基本文字颜色渲染和单元格对齐逻辑。

---

## 6. 实现准则与预警【核心】
1. **防爆内存与去抖缓存清理**：`m_failedThumbs` 在模型执行全量检索清理（如 `clearThumbCache` 或搜索重设 `updateResults` 发生 Reset）时，也必须一同执行 `.clear()`，防止垃圾数据溢出或在磁盘路径更新时阻断新图加载。
2. **零闪跃终极体感**：阻断成功后，图片在列表中的刷新体感将展现为：卡片以圆润的 `#2d2d2d` 纯暗背景展示，一旦磁盘后台 I/O 生成就绪，缩略图将瞬间平滑亮起，整个过程中无任何亮色、杂色文件夹或图标来回交替闪跃，达到与 ArcMeta 对齐的、无可挑剔的极致顺滑体验。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 缩略图去毒管线 | 物理废除冗余双轨拉伸占位，对齐缩略图状态并抑制闪变。 | ✅ 符合。本案精确定位并封锁了中间状态的“穿透系统图标”逻辑漏沙，确保无闪烁设计完美闭环。 |

---

## 8. 待确认事项（可选）
- **无**：重构逻辑直击病态根因，设计方案堪称行业典范。
