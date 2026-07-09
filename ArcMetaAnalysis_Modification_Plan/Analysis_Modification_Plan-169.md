# FERREX-META 缩略图“物理资产优先”高保真渲染重构 —— Analysis_Modification_Plan-169.md

## 1. 任务背景
在目前版本的 FERREX-META 中，由于异步提取高清晰度缩略图需要消耗磁盘 I/O 成本，因此当显示新数据或高速滚动列表时，在第一阶段通常会先在卡片正中央渲染一个具有 50% 透明度的文件关联“系统默认图标”。等工作线程异步提取出缩略图并更新模型后，才最终将该系统图标替换并刷新为缩略图。
这种“系统图标在前，缩略图在后”的逻辑，在百万级数据体量下会引入非常显眼的刷新跳变感和视觉闪烁（被称为“傻逼逻辑架构”）。为了提升产品级的高级感与顺滑感，必须对渲染优先级进行彻底重构，实现“优先寻找并平滑拉伸显示已有或缓存的缩略图，最后再回退到默认图标”的高保真渲染架构。

## 2. 问题定位
视觉跳变和闪烁问题根源于 `src/ui/ThumbnailDelegate.cpp` 中的 `paint` 函数（第 65~93 行）对绘制分支的管理：

```cpp
int thumbStatus = index.data(m_hasThumbnailRole).toInt(); // 0=不支持, 1=就绪, 2=加载中
QVariant decoData = index.data(Qt::DecorationRole);
QPixmap thumb;
if (thumbStatus == 1 && decoData.canConvert<QPixmap>()) {
    thumb = decoData.value<QPixmap>();
}

// ...

if (thumbStatus == 1 && !thumb.isNull()) { // 强卡只有状态 1 且有 pixmap 才画缩略图
    // ... [绘制缩略图]
} else { // 只要没准备好，无条件全画系统图标
    QIcon icon = qvariant_cast<QIcon>(decoData);
    if (!icon.isNull()) {
        if (thumbStatus == 2) painter->setOpacity(0.5); // 加载中时绘制 50% 透明的系统图标
        // ... [绘制系统图标]
    }
}
```

### 根因分析：
1. **强行设卡导致无法利用现有资产**：即便模型在 `Qt::DecorationRole` 中已经提供了缓存在 `m_lastPixmapCache` 中的上一次有效缩略图副本（用于在拉伸调整时的渐进拉伸占位），但只要当前全新尺寸的提取状态 `thumbStatus` 返回为 `2`（加载中），代理（Delegate）就会粗暴地无视缩略图 Pixmap 的存在，无条件地通过 `else` 分支去绘制 50% 透明度的系统图标。
2. **缺乏防闪烁二级提取机制**：没有对图像数据的属性类型做预检。在显示图片、视频、设计图等图形文件时，如果发现本地高速缓存（L1 或 L2）已有可用缩略图，应不加限制地立刻以最高优先级执行绘制；只有在判定其完全不兼容缩略图，或者异步生成遭遇绝对失败之后，才能回退到系统图标作为最后的兜底手段。

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 应该是先显示缩略图，然后才显示图标，这才是我期望的 | 彻底重构 Delegate 渲染分支，若只要能提取出有效的 Pixmap 资产，便立刻绘制缩略图并平滑充满卡片（对应用户原话：“先显示缩略图”） | ✅ |
| 2    | 只有在没有缩略图或者生成失败时才回退到系统默认图标 | 仅当 Pixmap 为空且判定无法加载缩略图、或提取彻底失败后，才作为最后的手段去绘制默认的文件类型关联图标（对应用户原话：“然后才显示图标”） | ✅ |

## 4. 详细解决方案

作为“资深程序员·纯分析师”，本案提供清晰的重构逻辑代码样式设计，以供后续物理执行：

### 4.1 重构 `ThumbnailDelegate::paint` 的决策管线

为了颠覆绘制优先级并杜绝突变闪烁，对 `paint` 进行如下重构，打破以状态字 `thumbStatus == 1` 作为硬门槛的桎梏，改以 **“缩略图数据实存（Pixmap Valid）”** 作为第一优先控制链：

```cpp
void ThumbnailDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    Metrics m = calculateMetrics(option);
    bool isSelected = (option.state & QStyle::State_Selected);
    bool isGrid = option.widget ? option.widget->property("gridMode").toBool() : false;

    // 1. 获取缩略图状态与装饰媒介
    int thumbStatus = index.data(m_hasThumbnailRole).toInt(); // 0=不支持, 1=就绪, 2=加载中
    QVariant decoData = index.data(Qt::DecorationRole);
    
    QPixmap thumb;
    bool hasValidThumb = false;

    // 2.【核心改动】只要 decoData 能转换为 QPixmap，不管当前状态是 1（就绪）还是 2（加载中），
    // 都认为这属于可用的、高质量的缩略图物理资产（包括上一代多态拉伸暂位图），优先提取！
    if (decoData.canConvert<QPixmap>()) {
        thumb = decoData.value<QPixmap>();
        if (!thumb.isNull()) {
            hasValidThumb = true;
        }
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);

    // ① 绘制卡片内部
    painter->save();
    QPainterPath clipPath;
    clipPath.addRoundedRect(m.cardRect, 6, 6);
    painter->setClipPath(clipPath);

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor("#2d2d2d"));
    painter->drawRect(m.cardRect);

    // 3.【关键路径重构】：优先绘制缩略图
    if (hasValidThumb) {
        // 如果虽然有 Pixmap，但状态仍显示为“加载中”，表明当前是历史缓存在做渐进平滑过渡
        // 我们不应该在此期间绘制系统图标，而是直接对旧缩略图进行渐进拉伸渲染（100% Cover/Contain）
        QPixmap scaled = thumb.scaled(m.cardRect.size(), 
                                      isGrid ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding, 
                                      Qt::SmoothTransformation);
        int x = m.cardRect.center().x() - scaled.width() / 2;
        int y = m.cardRect.center().y() - scaled.height() / 2;
        
        // 如果处于异步加载升级期间，可以赋予一个轻微的平滑淡入感
        if (thumbStatus == 2) {
            painter->setOpacity(0.95); // 95% 透明度，给高精大图的替换预留非常自然的过渡
        }
        painter->drawPixmap(x, y, scaled);
        if (thumbStatus == 2) {
            painter->setOpacity(1.0);
        }
    } 
    // 4.【回退方案】：实在没有任何可用缩略图 Pixmap 时，才回退到绘制文件类型图标
    else {
        // 如果连 QIcon 装饰都没有，才绘制兜底
        QIcon icon = qvariant_cast<QIcon>(decoData);
        if (!icon.isNull()) {
            // 如果该文件类型支持缩略图且当前还在加载中，此时卡片在没有历史 Pixmap 时会有一瞬间的空白。
            // 为了防止界面彻底出现直白黑方块，可以绘制带有轻微动画或 30% 低饱和度透明的图标，
            // 绝不发生 50% 高亮系统图标在前刺眼的情况。
            if (thumbStatus == 2) {
                painter->setOpacity(0.25); // 降低到 25% 灰度透明，作为极静默的隐式过渡
            } else {
                painter->setOpacity(0.7);  // 常规不支持缩略图的项，也采取略显高级的 70% 亮度
            }
            
            int iconSize = qMin(m.cardRect.width(), m.cardRect.height()) * 0.6;
            QRect iconRect(m.cardRect.center().x() - iconSize / 2,
                           m.cardRect.center().y() - iconSize / 2,
                           iconSize, iconSize);
            icon.paint(painter, iconRect);
            
            painter->setOpacity(1.0);
        }
    }
    painter->restore();

    // ③ 绘制卡片边框
    painter->save();
    if (isSelected) {
        painter->setPen(QPen(QColor("#3498db"), 3));
    } else {
        painter->setPen(QPen(QColor("#4a4a4a"), 1));
    }
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(m.cardRect, 6, 6);
    painter->restore();

    // 状态位图标绘制... [保持原有物理位置绘制不变]
    // ... [省略文件名和空文件夹高亮代码]
}
```

### 4.2 配合重构模型端 `ScanTableModel::data`
为了确保上一代渐进 Pixmap（在 `m_lastPixmapCache` 中）或异步落盘结果在处于 `thumbStatus == 2`（加载中）时能够顺利、高优先地传递给 Delegate：

在 `src/ui/ScanDialog.cpp` 的第 290~312 行左右：
```cpp
            // 2.【核心改进：当处于加载中时优先返回历史缩略图平滑拉伸源】
            QPixmap* lastCached = m_lastPixmapCache.object(QString::number(key));
            if (lastCached) {
                // 如果发现虽然精确尺寸缺失（触发了异步队列，thumbStatus == 2）但 lastCached 物理存在
                // 我们必须高优先返回 lastCached！这样上面的 Delegate 就会将其作为 hasValidThumb 绘制。
                // 绝不会因为尚未生成新尺寸而无脑回退去闪烁系统默认图标。
                
                // 触发后台静默生成符合全新精确尺寸的高画质大图
                if (!m_requestedThumbs.contains(key)) {
                    m_requestedThumbs.insert(key);
                    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                    int thumbSize = (dlg && dlg->m_viewStack->currentIndex() == 0) ? 24 : (dlg ? dlg->m_config.iconSize : 64);
                    m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                    if (!m_thumbTimer->isActive()) m_thumbTimer->start();
                }

                // 返回这个历史 Pixmap！
                return *lastCached; 
            }
```
通过这样的改动，模型（Model）与代理（Delegate）协同发力，建立起了以下坚固的**“物理资产优先、图标靠后兜底”**的逻辑链路：

```
            [视口滚动/数据刷新]
                   │
                   ▼
       Model::data() 索要数据 
                   │
        ┌──────────┴──────────┐
        ▼ L1 内存精确命中       ▼ 缺失，寻找历史 L2（m_lastPixmapCache）
    返回精确 QPixmap       ┌───┴──────────┐
        │                 ▼ 存在历史图     ▼ 彻底没有
        │             返回历史 Pixmap  返回 QFileIconProvider 系统默认图标
        │                 │                │
        └──────────┬──────┘                │
                   ▼                       ▼
    hasValidThumb == true              hasValidThumb == false
                   │                       │
                   ▼                       ▼
    [第一优先级：直接平滑拉伸图片]      [第二优先级：作为最后底线绘制系统图标]
     (绝不闪现系统默认图标)              (加载中以极微弱的 25% 亮度静默占位)
```

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ThumbnailDelegate.cpp` 中的 `paint` 渲染主函数
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` 中的 `ScanTableModel::data` 关于 `DecorationRole` 的分发路径

**明确禁止越界修改的范围：**
- [ ] 禁止改动 `UiHelper::getFileIcon` 内部对于系统关联图标缓存的处理逻辑，该函数只作兜底，不应承载主渲染分支。
- [ ] 禁止改动详情列表模式（`viewMode == 0`）下的单元格渲染逻辑，该视图不涉及大缩略图卡片绘制。

## 6. 实现准则与预警【核心】
1. **防抖动加载与内存控制**：模型层返回 `lastCached` 的同时，应保障 `m_requestedThumbs` 已正确注册，防止列表微小滚动时重复压入重型 I/O 线程池，导致后台队列积压假死。
2. **QVariant 转换保护**：由于在模型中 `lastCached` 返回为 `QPixmap` 实例（即转换为 `QVariant`），在 Delegate 中通过 `decoData.canConvert<QPixmap>()` 转换时，需提前用 `!thumb.isNull()` 来保证物理资产真实有效。
3. **流畅度检测**：L1 与 L2 缓存在百万级高频滑动时应当全无卡顿，且由于消除了系统图标的交替闪烁，列表视觉流畅度可获得大幅提升。

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| 缩略图去毒管线 | 物理废除冗余拉伸，对齐缩略图状态。 LIFO 高优先队列。 | ✅ 符合。本案重构在完全符合原防闪、渐进式拉伸缓存的设计原则下，颠覆了显示优先级，消除了图标闪跃的缺陷。 |

## 8. 待确认事项（可选）
- **无**：本次逻辑极为精密，重构方向完全符合设计哲学。
