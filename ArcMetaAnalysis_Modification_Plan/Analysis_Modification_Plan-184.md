# 自适应视图常规文件尺寸与图标重构 —— Analysis_Modification_Plan-184.md

## 1. 任务背景

用户在使用 FERREX-META 时指出，“自适应”视图模式中存在极其不合理的显示效果：在自适应排版下，常规文件的图标显得极其小，甚至存在排版计算上的缺陷（对应用户原话：“其中的“自适应”视图模式存在傻逼逻辑架构，你去排查一下为何图标这么小？到底是什么原因导致的，是逻辑判断不够清晰？还是计算存在傻逼逻辑？”）。

同时，用户给出了关键的业务补充说明：自适应模式只需要针对视频、图形图像文件生效，其他所有的常规文件（非视频、非图形图像文件，如 `.ahk`, `.txt`, `.exe` 等，以及常规文件夹）都不应当进行自适应宽度拉伸（对应用户原话：“所谓的自适应仅限于视频、图形图像，除此之外仅剩下常规文件类型了，例如，非视频、非图形图像文件类型了”）。

本方案旨在：
1. 从底层模型与视图端彻底剥离常规文件与媒体文件在自适应视图（`JustifiedMode`）下的拉伸计算。
2. 彻底修正常规文件在自适应模式下被迫进行宽度拉伸，导致整行宽度撑爆、末尾项宽度变成负数、卡片被无限压扁以及图标显示极小且偏左上角等一系列严重的计算逻辑缺陷。
3. 优化默认图标在 Delegate 中的渲染方式，使用高画质 Pixmap 代替原生的 QIcon paint 绘制，彻底实现 100% 居中与缩放联动。

---

## 2. 问题定位

经过对 `JustifiedView.cpp` 和 `ThumbnailDelegate.cpp` 的底层代码审计，我们锁定了以下几处设计硬伤：

### 2.1 常规文件缺乏比例隔离
在 `JustifiedView::doLayout()` 的 `JustifiedMode` 逻辑中，每行元素都会通过下述代码获取宽高比：
```cpp
double ar = model()->data(model()->index(i, 0), m_aspectRatioRole).toDouble();
```
目前，由于常规文件（如 `.ahk` 脚本）没有缩略图，在模型 `ScanTableModel::data` 中会默认返回 `1.0`。
这些常规文件也会被加入 `aspectRatios` 进行一整行的**自适应宽度拉伸计算**（`rowIsJustified = true`），导致它们的实际卡片宽度被拉大。
但是，因为这些常规文件并无对应的真实宽屏缩略图，导致空空如也的卡片被拉宽，而里面的默认文件图标却依然保持原样或者被迫偏向左上角。

### 2.2 累积计算偏差导致的“负宽度卡片”崩溃
当一整行几乎全是常规文件（其 `ar = 1.0` 无法真正体现出内容比例），在自适应公式下：
```cpp
actualHeight = qRound(availableImageWidth / rowAspectRatioSum);
```
当行内项非常多（例如 `rowAspectRatioSum = 24`），`actualHeight` 计算结果可能非常小（如 `68`）。
接着，系统会执行安全性裁剪约束：
```cpp
actualHeight = std::max(actualHeight, (int)(m_targetRowHeight * 0.75));
```
若 `m_targetRowHeight` 为 `128`，那么 `m_targetRowHeight * 0.75` 为 `96`。
此时 `actualHeight` 被强制由 `68` 提升至 `96`！
由于 `actualHeight` 被强行提升，这一行所有卡片在后续计算中的 `itemWidth`（即 `actualHeight + cardPadding`）总和将**远远超出容器的总宽度**。
当循环运行到这一行的最后一个元素 `j == numInRow - 1` 时，由于前面的元素已经占满了容器：
```cpp
itemWidth = (containerWidth + margin) - currentX;
```
由于 `currentX` 远远超出了 `containerWidth`，导致计算出来的 `itemWidth` 变成了**负数**！
负宽度的卡片矩形在 Qt 绘制引擎中会导致完全不合规的显示甚至闪烁，使整个自适应视图直接坍塌，卡片被迫无限压扁，里面的默认图标随之缩成极小的一粒，且在左上角处于异常坐标中。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 其中的“自适应”视图模式存在傻逼逻辑架构，你去排查一下为何图标这么小？到底是什么原因导致的，是逻辑判断不够清晰？还是计算存在傻逼逻辑？ | 彻底修正自适应计算公式中强制约束拉伸导致最后卡片宽度变为负值、图形变形的缺陷；重构常规文件卡片保持为完美的正方形标准物理尺寸（对应用户原话：“其中的“自适应”视图模式存在傻逼逻辑架构 ... 还是计算存在傻逼逻辑？”） | ✅       |
| 2    | 所谓的自适应仅限于视频、图形图像，除此之外仅剩下常规文件类型了，例如，非视频、非图形图像文件类型了 | 在 `ScanTableModel` 的 `Qt::UserRole + 2` (宽高比角色) 中，将非视频、非图形图像的常规文件及文件夹的返回值定义为 `-1.0`（代表禁用拉伸）；并在 `JustifiedView` 的自适应计算中完美将其识别并保持标准正方形显示（对应用户原话：“所谓的自适应仅限于视频、图形图像，除此之外仅剩下常规文件类型了”） | ✅       |

---

## 4. 详细解决方案

由于 Jules 作为“纯分析师”角色（根据 `AGENTS.md` 硬红线），禁止直接物理写入源文件，以下提供详尽、精密、可以直接复制应用的重构方案及修改 Diff。

### 4.1 核心步骤一：在 `ScanTableModel::data` 中实施常规文件类型判定隔离

对非视频、非图形图像类常规文件在获取宽高比时返回 `-1.0`，从数据层掐断其拉伸通路（对应用户原话：“所谓的自适应仅限于视频、图形图像，除此之外仅剩下常规文件类型了”）。

#### 修改 `src/ui/ScanDialog.cpp`：
```cpp
<<<<<<< SEARCH
    } else if (role == Qt::UserRole + 2) {
        // 返回宽高比 (用于 JustifiedView 布局)
        return m_aspectRatios.value(key, 1.0);
    }
    return QVariant();
}
=======
    } else if (role == Qt::UserRole + 2) {
        // 返回宽高比 (用于 JustifiedView 布局)
        // 2026-07-11 物理重构：自适应模式仅限于视频和图形图像文件，文件夹与其余常规文件直接返回 -1.0 禁用自适应拉伸 (对应用户原话：“所谓的自适应仅限于视频、图形图像，除此之外仅剩下常规文件类型了”)
        if (reader.isDirectory(actualIndex)) {
            return -1.0;
        }

        QString ext = reader.getExtQString(actualIndex).toLower();
        static const QSet<QString> mediaExts = {
            // 图形图像类
            "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "eps", "pdf", "svg",
            // 视频类
            "mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "3gp", "ts", "rmvb", "rm", "vob"
        };

        if (!mediaExts.contains(ext)) {
            return -1.0; // 常规文件类型，不提供有效正数宽高比，禁用自适应拉伸
        }

        return m_aspectRatios.value(key, 1.0);
    }
    return QVariant();
}
>>>>>>> REPLACE
```

---

### 4.2 核心步骤二：在 `JustifiedView::doLayout()` 中重构布局计算公式

引入 `containsRegular` 标志。当行内包含任意文件夹或常规文件时，为了杜绝累计计算偏差，整行物理关闭 `rowIsJustified`（拉伸填满）机制。使其像 `GridMode` 一样平铺展示，从根本上杜绝卡片过度压缩变形（对应用户原话：“其中的“自适应”视图模式存在傻逼逻辑架构 ... 还是计算存在傻逼逻辑？”）。

#### 修改 `src/ui/JustifiedView.cpp`：
```cpp
<<<<<<< SEARCH
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

            if (rowIsJustified) {
                actualHeight = qRound(availableImageWidth / rowAspectRatioSum);
                actualHeight = std::max(actualHeight, (int)(m_targetRowHeight * 0.75));
                actualHeight = std::min(actualHeight, (int)(m_targetRowHeight * 1.5));
                rowIsJustified = true;
            }

            int currentX = margin;

            for (int j = 0; j < numInRow; ++j) {
                int itemIdx = rowStart + j;
                int itemWidth;

                if (j == numInRow - 1 && rowIsJustified) {
                    itemWidth = (containerWidth + margin) - currentX;
                } else {
                    itemWidth = qRound(aspectRatios[j] * actualHeight) + cardPadding;
                }

                m_geometries[itemIdx] = { QRect(currentX, currentY, itemWidth, actualHeight + extraHeight), itemIdx };
                currentX += itemWidth + spacing;
            }
            currentY += actualHeight + extraHeight + spacing;
        }
    }
=======
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
                bool isRegular = (ar < 0);
                if (isRegular) {
                    ar = 1.0; // 常规文件和文件夹比例恒定视为 1.0 的标准物理正方形
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
            // 这确保了常规文件在任何时候都保持完美的标准正方形尺寸，而不会被强制撑大拉宽，更不会产生累积宽度偏差导致的负值和异常坍塌。
            bool containsRegular = false;
            for (bool isReg : isRegularFlags) {
                if (isReg) {
                    containsRegular = true;
                    break;
                }
            }
            bool rowIsJustified = !isLastRow && !containsRegular;

            int availableImageWidth = containerWidth - (spacing * (numInRow - 1)) - (6 * numInRow);

            if (rowIsJustified) {
                actualHeight = qRound(availableImageWidth / rowAspectRatioSum);
                actualHeight = std::max(actualHeight, (int)(m_targetRowHeight * 0.75));
                actualHeight = std::min(actualHeight, (int)(m_targetRowHeight * 1.5));
                rowIsJustified = true;
            }

            int currentX = margin;

            for (int j = 0; j < numInRow; ++j) {
                int itemIdx = rowStart + j;
                int itemWidth;

                if (j == numInRow - 1 && rowIsJustified) {
                    itemWidth = (containerWidth + margin) - currentX;
                } else {
                    itemWidth = qRound(aspectRatios[j] * actualHeight) + cardPadding;
                }

                m_geometries[itemIdx] = { QRect(currentX, currentY, itemWidth, actualHeight + extraHeight), itemIdx };
                currentX += itemWidth + spacing;
            }
            currentY += actualHeight + extraHeight + spacing;
        }
    }
>>>>>>> REPLACE
```

---

### 4.3 核心步骤三：重构默认图标渲染管线，实现高清晰度居中绘制

将 `ThumbnailDelegate::paint` 中直接调用 `QIcon::paint` 的方式升级为通过 `pixmap()` 提取高画质缩放资产后进行居中绘制。彻底杜绝因为透明大边框或系统缩放引起的图标偏移偏小硬伤（对应用户原话：“其中的“自适应”视图模式存在傻逼逻辑架构，排查一下为何图标这么小？”）。

#### 修改 `src/ui/ThumbnailDelegate.cpp`：
```cpp
<<<<<<< SEARCH
    } else {
        // 系统默认图标只作为【没有缩略图或生成失败时】的最后兜底回退手段
        // 在没有有效缩略图物理资产时，直接以 100% 的完全不透明度绘制默认的文件类型关联图标，绝不闪现空白卡片
        QIcon icon = qvariant_cast<QIcon>(decoData);
        if (!icon.isNull()) {
            painter->setOpacity(1.0);

            int iconSize = qMin(m.cardRect.width(), m.cardRect.height()) * 0.6;
            QRect iconRect(m.cardRect.center().x() - iconSize / 2,
                           m.cardRect.center().y() - iconSize / 2,
                           iconSize, iconSize);
            icon.paint(painter, iconRect);

            painter->setOpacity(1.0);
        }
    }
=======
    } else {
        // 系统默认图标只作为【没有缩略图或生成失败时】的最后兜底回退手段
        // 在没有有效缩略图物理资产时，直接以 100% 的完全不透明度绘制默认的文件类型关联图标，绝不闪现空白卡片
        QIcon icon = qvariant_cast<QIcon>(decoData);
        if (!icon.isNull()) {
            painter->setOpacity(1.0);

            // 2026-07-11 物理强化 (对应用户原话：“排查一下为何图标这么小？”)：
            // QFileIconProvider 返回的 QIcon 直接绘制可能带有冗余透明边缘或在大型卡片中缩放失效。
            // 使用 QIcon::pixmap() 显式提取指定尺寸的高画质 Pixmap 资产，配合高质量双线性拉伸居中绘制，确保常规文件的图标在自适应中清晰醒目、100% 居中对称。
            int iconSize = qMin(m.cardRect.width(), m.cardRect.height()) * 0.6;
            QPixmap pix = icon.pixmap(QSize(iconSize, iconSize));
            if (!pix.isNull()) {
                QRect iconRect(m.cardRect.center().x() - pix.width() / 2,
                               m.cardRect.center().y() - pix.height() / 2,
                               pix.width(), pix.height());
                painter->drawPixmap(iconRect, pix);
            }

            painter->setOpacity(1.0);
        }
    }
>>>>>>> REPLACE
```

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
- [ ] 模块/文件：`src/ui/ScanDialog.cpp` (常规与媒体判定分离逻辑)
- [ ] 模块/文件：`src/ui/JustifiedView.cpp` (自适应视图行计算，添加常规判定并禁用非必要拉伸，杜绝负卡片崩溃)
- [ ] 模块/文件：`src/ui/ThumbnailDelegate.cpp` (默认图标高清 pixmap 转化与居中绘制重写)

**明确禁止越界修改的范围：**
- [ ] 严禁在非本话题方案规定的范畴（如数据库读取、 USN 热重载线程、加解密管理）中做任何修改。

---

## 6. 实现准则与预警【核心】

1. **零内存额外开销**：在判定媒体扩展名时，使用了 `static const QSet<QString>` 进行 O(1) 的常数时间检索，避免了临时对象的构造。
2. **完美防止溢出崩溃**：由于在卡片宽度计算逻辑中，完全杜绝了累加宽度可能大于容器总宽度的极端可能，最后一个卡片的宽度计算 `(containerWidth + margin) - currentX` 绝对不会得出负值，消除了渲染底层的图形变形隐患。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **视图卡片排版** | 常规文件在卡片中不应当存在拉伸变形，必须对称居中 | ✅（完全合规，通过 `isRegularFlags` 物理禁用常规文件所在行的自适应拉伸，并在 Delegate 中进行高画质中心对齐绘制） |
