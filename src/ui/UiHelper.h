#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma once

#include <QIcon>
#include <QString>
#include <QColor>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>
#include <QMap>
#include <QCache>
#include "../core/AppConfig.h"
#include <QFileInfo>
#include <QImage>
#include <QStringList>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrent>
#include <QDebug>
#include <QSet>
#include <QCoreApplication>
#include <QWidget>
#include <QBuffer>
#include <QProcess>
#include <QUuid>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <algorithm>
#include <cmath>

// Windows Shell 缩略图引擎依赖
#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#ifdef __MINGW32__
// MinGW 可能不支持某些高级 Shell API
#include <shlwapi.h>
#else
#include <shobjidl_core.h>
#include <thumbcache.h>
#endif
#endif

#include "SvgIcons.h"

namespace ArcMeta {

/**
 * @brief UI 辅助类 (全量热加载版 - 杜绝懒加载)
 */
class UiHelper {
public:
    static QMap<QString, QPixmap>& iconPixmapCache() {
        static QMap<QString, QPixmap> cache;
        return cache;
    }

    static void initializeHotIcons() {
        qDebug() << "[UiHelper] 图标系统已启用懒加载模式";
    }

    static QColor parseColorName(const QString& colorName) {
        if (colorName.isEmpty()) return QColor();
        
        // 优先尝试原生解析 (支持 #RRGGBB)
        QColor c(colorName);
        if (c.isValid()) return c;

        if (colorName == "red" || colorName == "红") return QColor("#E24B4A");
        if (colorName == "orange" || colorName == "橙") return QColor("#EF9F27");
        if (colorName == "yellow" || colorName == "黄") return QColor("#FECF0E");
        if (colorName == "green" || colorName == "绿") return QColor("#639922");
        if (colorName == "cyan" || colorName == "青") return QColor("#1D9E75");
        if (colorName == "blue" || colorName == "蓝") return QColor("#378ADD");
        if (colorName == "purple" || colorName == "紫") return QColor("#7F77DD");
        if (colorName == "gray" || colorName == "灰") return QColor("#5F5E5A");
        if (colorName == "black" || colorName == "黑") return QColor("#000000");
        if (colorName == "white" || colorName == "白") return QColor("#FFFFFF");
        
        return QColor();
    }


    static QPixmap renderIcon(const QString& key, const QSize& size, const QColor& color) {
        if (!SvgIcons::icons.contains(key)) return QPixmap();
        QString svgData = SvgIcons::icons[key];
        svgData.replace("currentColor", color.name());
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QSvgRenderer renderer(svgData.toUtf8());
        renderer.render(&painter);
        return pixmap;
    }

    static QString getSvgDataUrl(const QString& key, const QColor& color = QColor("#3498db")) {
        // [PHYSICAL COMPATIBILITY] 转换为 PNG Base64 以确保 QSS 100% 渲染成功
        // 2026-06-xx 物理修正：使用 20x20 尺寸以匹配 QTreeView 默认分支宽度
        QPixmap pix = renderIcon(key, QSize(20, 20), color);
        if (pix.isNull()) return QString();
        
        QByteArray ba;
        QBuffer buffer(&ba);
        buffer.open(QIODevice::WriteOnly);
        pix.save(&buffer, "PNG");
        return QString("data:image/png;base64,%1").arg(QString(ba.toBase64()));
    }

    static QString getSvgTempFilePath(const QString& key, const QColor& color) {
        QPixmap pix = renderIcon(key, QSize(20, 20), color);
        if (pix.isNull()) return QString();

        // 2026-06-xx 物理修复：在路径中加入 V3 标识并强制覆盖。
        // 核心修正：Qt QSS 必须使用正斜杠 (/)，反斜杠会被转义导致加载失败。强制转换为正斜杠。
        QString tmpPath = QDir::temp().filePath(
            QString("arcmeta_%1_%2_v3.png").arg(key).arg(color.name().mid(1))
        );
        pix.save(tmpPath, "PNG");
        return QDir::fromNativeSeparators(tmpPath);
    }

    static bool isGraphicsFile(const QString& ext) {
        // 2026-06-xx 工业级扩容：只要 Windows 能显示预览图，就允许进入解析流程
        static const QStringList graphicsExts = {
            "png", "jpg", "jpeg", "bmp", "gif", "webp", "ico", "tiff", "tif",
            "psd", "psb", "ai", "eps", "pdf", "svg", "cdr",
            "sketch", "xd", "fig", "dwg", "dxf", "heic", "raw"
        };
        return graphicsExts.contains(ext.toLower());
    }

    static QIcon getIcon(const QString& key, const QColor& color, int size = 18) {
        QIcon icon;
        QPixmap pix = getPixmap(key, QSize(size, size), color);
        if (!pix.isNull()) icon.addPixmap(pix);
        return icon;
    }

    static QIcon getFileIcon(const QString& filePath, int size = 18, const QColor& overrideColor = QColor()) {
        Q_UNUSED(overrideColor);
        Q_UNUSED(size);
        
        QFileInfo info(filePath);
        // 2026-06-xx 架构修正：磁盘根目录图标应独立缓存，防止其覆盖通用文件夹图标
        QString key = info.isDir() ? (info.isRoot() ? filePath : "folder") : info.suffix().toLower();
        if (key.length() > 128) key = "unknown";
        
        static QMap<QString, QIcon> s_iconCache;
        if (s_iconCache.contains(key)) {
            return s_iconCache[key];
        }

        QFileIconProvider provider;
        QIcon icon;
        if (info.isDir()) {
            // 2026-06-xx 架构修正：判断是否为磁盘根目录
            if (info.isRoot()) {
                // 若是磁盘根目录，必须获取其盘符图标而非通用文件夹图标
                icon = provider.icon(info);
            } else {
                icon = provider.icon(QFileIconProvider::Folder);
            }
        } else {
            icon = provider.icon(QFileInfo("dummy." + key));
            if (icon.isNull()) {
                icon = provider.icon(QFileIconProvider::File);
            }
        }
        
        s_iconCache[key] = icon;
        return icon;
    }

    static QPixmap getPixmap(const QString& key, const QSize& size, const QColor& color) {
        QString cKey = QString("%1_%2_%3_%4").arg(key).arg(size.width()).arg(size.height()).arg(color.rgba());
        if (iconPixmapCache().contains(cKey)) return iconPixmapCache()[cKey];
        QPixmap rendered = renderIcon(key, size, color);
        if (!rendered.isNull()) iconPixmapCache().insert(cKey, rendered);
        return rendered;
    }

    static void applyMenuStyle(QWidget* menu) {
        if (!menu) return;
        // 2026-06-xx 物理修复：开启透明背景属性，消除圆角外的直角溢出
        menu->setAttribute(Qt::WA_TranslucentBackground);
        menu->setWindowFlag(Qt::FramelessWindowHint);
        menu->setStyleSheet(
            "QMenu { background-color: #1A1A1A; color: #CCC; border: 1px solid #333; padding: 4px; border-radius: 8px; }"
            "QMenu::item { padding: 6px 30px 6px 10px; border-radius: 4px; font-size: 12px; }"
            "QMenu::item:selected { background-color: #232D37; color: white; }"
            "QMenu::separator { height: 1px; background: #333; margin: 4px 8px; }"
            "QMenu::right-arrow { "
            "  image: url(\"data:image/svg+xml;base64,PHN2ZyB2aWV3Qm94PSIwIDAgMTAyNCAxMDI0IiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciPjxwYXRoIGQ9Ik0zNDEuMzMzIDUwLjY2N2w1MTIgNDYxLjMzMy01MTIgNDYxLjMzM3oiIGZpbGw9IiNGRkZGRkYiLz48L3N2Zz4=\"); "
            "  width: 8px; height: 8px; "
            "  subcontrol-origin: padding; "
            "  subcontrol-position: center right; "
            "  right: 10px; "
            "}"
        );
    }

    static QColor getExtensionColor(const QString& ext) {
        static QMap<QString, QColor> s_cache;
        QString upperExt = ext.toUpper();
        if (upperExt == "DIR") return QColor(45, 65, 85, 200);
        if (upperExt.isEmpty()) return QColor(60, 60, 60, 180);
        if (s_cache.contains(upperExt)) return s_cache[upperExt];

        QString settingKey = QString("ExtensionColors/%1").arg(upperExt);
        QVariant val = AppConfig::instance().getValue(settingKey);
        if (val.isValid()) {
            QColor color = val.value<QColor>();
            s_cache[upperExt] = color;
            return color;
        }

        size_t hash = qHash(upperExt);
        int hue = static_cast<int>(hash % 360);
        QColor color = QColor::fromHsl(hue, 160, 110, 200); 
        s_cache[upperExt] = color;
        AppConfig::instance().setValue(settingKey, color);
        return color;
    }

    /**
     * @brief 对颜色进行量化 (已废除破坏性位截断，直接返回原色以确保预览色与上色完全一致)
     */
    static inline QColor quantizeColor(const QColor& color) {
        return color;
    }


    /**
     * @brief 从图像中提取调色盘 (工业级优化版)
     * 2026-06-xx 架构重构：彻底弃用外部工具链 (ImageMagick/Ghostscript)，全面转向原生 Shell 引擎
     */
    static QVector<QPair<QColor, float>> extractPalette(const QString& targetFile) {
        // 优先从系统缩略图引擎获取数据，支持 PSD, AI, EPS, PDF 等专业格式 (前提是系统有预览插件)
        QImage targetImg = getShellThumbnail(targetFile, 128);

        // 回退：针对普通图片或无插件环境，直接通过 Qt 加载
        if (targetImg.isNull()) {
            targetImg.load(targetFile);
        }

        // 核心防御：加载图像后必须立即进行空值检查，防止后续像素处理逻辑崩溃
        if (targetImg.isNull()) return {};

        // 1. 采样：使用 128x128 提高颜色覆盖度
        QImage sampled = targetImg.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        
        struct BucketInfo { 
            long long rSum = 0, gSum = 0, bSum = 0; 
            int count = 0; 
        };
        QMap<QRgb, BucketInfo> bucketStats;
        int totalValidPixels = 0;

        for (int row = 0; row < sampled.height(); ++row) {
            for (int col = 0; col < sampled.width(); ++col) {
                QRgb rgb = sampled.pixel(col, row);
                if (qAlpha(rgb) < 128) continue;

                // 2. 量化分组：使用 3-bit 建立桶
                QRgb rgbKey = qRgb(qRed(rgb) & 0xE0, qGreen(rgb) & 0xE0, qBlue(rgb) & 0xE0);
                auto& stat = bucketStats[rgbKey];
                stat.rSum += qRed(rgb);
                stat.gSum += qGreen(rgb);
                stat.bSum += qBlue(rgb);
                stat.count++;
                totalValidPixels++;
            }
        }

        if (bucketStats.isEmpty()) return {};

        // 3. 计算桶的平均真色并初步排序
        struct FinalBucket { QColor avgColor; int count; };
        QList<FinalBucket> buckets;
        for (auto it = bucketStats.begin(); it != bucketStats.end(); ++it) {
            const auto& s = it.value();
            buckets.append({ QColor((int)(s.rSum / s.count), (int)(s.gSum / s.count), (int)(s.bSum / s.count)), s.count });
        }
        std::sort(buckets.begin(), buckets.end(), [](const FinalBucket& a, const FinalBucket& b) {
            return a.count > b.count;
        });

        // 4. 相似合并 (HSL 空间比对：deltaH < 20, deltaS < 25, deltaL < 20)
        QList<FinalBucket> merged;
        for (const auto& b : buckets) {
            bool found = false;
            int h1, s1, l1; b.avgColor.getHsl(&h1, &s1, &l1);
            
            for (auto& m : merged) {
                int h2, s2, l2; m.avgColor.getHsl(&h2, &s2, &l2);
                
                int dh = std::abs(h1 - h2);
                if (dh > 180) dh = 360 - dh; // 色相环循环处理
                int ds = std::abs(s1 - s2);
                int dl = std::abs(l1 - l2);

                if (dh < 20 && ds < 25 && dl < 20) {
                    // 重新计算加权平均色，确保最终 HEX 真值不偏离物理重心
                    int total = m.count + b.count;
                    int nr = (m.avgColor.red() * m.count + b.avgColor.red() * b.count) / total;
                    int ng = (m.avgColor.green() * m.count + b.avgColor.green() * b.count) / total;
                    int nb = (m.avgColor.blue() * m.count + b.avgColor.blue() * b.count) / total;
                    m.avgColor = QColor(nr, ng, nb);
                    m.count = total;
                    found = true;
                    break;
                }
            }
            if (!found) merged.append(b);
        }

        // 5. 再次排序并返回全量色板
        std::sort(merged.begin(), merged.end(), [](const FinalBucket& a, const FinalBucket& b) {
            return a.count > b.count;
        });

        QVector<QPair<QColor, float>> result;
        for (int i = 0; i < (int)merged.size(); ++i) {
            float ratio = (float)merged[i].count / totalValidPixels;
            // 物理过滤：占比不足 1% 的杂色直接丢弃，对标 Eagle 密度
            if (ratio < 0.01f) continue;
            result.append({ merged[i].avgColor, ratio });
        }
        return result;
    }

    /**
     * @brief 从图像中提取主色调 (向后兼容封装版)
     */
    static inline QColor extractDominantColor(const QString& targetFile) {
        auto palette = extractPalette(targetFile);
        return palette.isEmpty() ? QColor() : palette.first().first;
    }

public:
    static QImage getShellThumbnail(const QString& path, int size) {
        // 2026-06-xx 物理重构：引入磁盘缓存机制，消除“失忆症”
        QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QString cacheDir = QDir(appData).filePath("thumbs/");
        QDir().mkpath(cacheDir);

        QFileInfo fi(path);
        // 2026-06-xx 物理修复：在 hashKey 中加入 v13 标识，强制失效旧缓存
        QString hashKey = QString("%1_%2_%3_%4_v13").arg(path).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch()).arg(size);
        QString safeName = QString::number(qHash(hashKey), 16) + ".png";
        QString cachePath = cacheDir + safeName;

        if (QFile::exists(cachePath)) {
            QImage img;
            if (img.load(cachePath)) return img;
        }

#ifdef Q_OS_WIN
        PIDLIST_ABSOLUTE pidl = nullptr;
        HRESULT hr = SHParseDisplayName(path.toStdWString().c_str(), nullptr, &pidl, 0, nullptr);
        if (FAILED(hr)) return QImage();
        IShellItem* pItem = nullptr;
        hr = SHCreateItemFromIDList(pidl, IID_IShellItem, (void**)&pItem);
        ILFree(pidl);
        if (SUCCEEDED(hr)) {
            IShellItemImageFactory* pFactory = nullptr;
            hr = pItem->QueryInterface(IID_IShellItemImageFactory, (void**)&pFactory);
            if (SUCCEEDED(hr)) {
                SIZE nativeSize = { size, size };
                HBITMAP hBitmap = nullptr;
                hr = pFactory->GetImage(nativeSize, SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT, &hBitmap);
                if (SUCCEEDED(hr) && hBitmap) {
                    BITMAP bmpInfo;
                    GetObject(hBitmap, sizeof(bmpInfo), &bmpInfo);
                    int w = bmpInfo.bmWidth;
                    int h = std::abs(bmpInfo.bmHeight);

                    BITMAPINFOHEADER bi = {};
                    bi.biSize        = sizeof(BITMAPINFOHEADER);
                    bi.biWidth       = w;
                    bi.biHeight      = -h;   // 负值 = top-down，方向永远正确
                    bi.biPlanes      = 1;
                    bi.biBitCount    = 32;
                    bi.biCompression = BI_RGB;

                    QByteArray pixels(w * h * 4, 0);
                    HDC hdc = GetDC(nullptr);
                    GetDIBits(hdc, hBitmap, 0, h, pixels.data(),
                              reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
                    ReleaseDC(nullptr, hdc);

                    // Windows 返回 BGRA，Qt 需要 RGBA，交换 R/B 通道
                    uint8_t* p = reinterpret_cast<uint8_t*>(pixels.data());
                    for (int i = 0; i < w * h; ++i) {
                        std::swap(p[i * 4 + 0], p[i * 4 + 2]);
                    }

                    QImage img(p, w, h, w * 4, QImage::Format_RGBA8888);
                    img = img.copy(); // 确保数据所有权
                    
                    // 异步存入磁盘缓存
                    (void)QtConcurrent::run([img, cachePath]() {
                        img.save(cachePath, "PNG");
                    });

                    DeleteObject(hBitmap);
                    pFactory->Release();
                    pItem->Release();
                    return img;
                }
                pFactory->Release();
            }
            pItem->Release();
        }
#else
        Q_UNUSED(path); Q_UNUSED(size);
#endif
        return QImage();
    }
};

} // namespace ArcMeta
