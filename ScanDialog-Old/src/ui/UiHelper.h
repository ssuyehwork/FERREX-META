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
            "QMenu { background-color: #2D2D2D; color: #EEE; border: 1px solid #444; padding: 4px; border-radius: 8px; }"
            "QMenu::item { padding: 6px 25px 6px 10px; border-radius: 4px; font-size: 12px; }"
            "QMenu::item:selected { background-color: #3E3E42; color: white; }"
            "QMenu::separator { height: 1px; background: #444; margin: 4px 8px; }"
            "QMenu::right-arrow { image: url(data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0ibm9uZSIgc3Ryb2tlPSIjRUVFRUVFIiBzdHJva2Utd2lkdGg9IjIiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCI+PHBvbHlsaW5lIHBvaW50cz0iOSAxOCAxNSAxMiA5IDYiPjwvcG9seWxpbmU+PC9zdmc+); width: 12px; height: 12px; right: 8px; }"
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
        QImage targetImg = getShellThumbnail(targetFile, 256);
        if (targetImg.isNull()) targetImg.load(targetFile);
        if (targetImg.isNull()) return {};

        QImage sampled = targetImg.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        
        struct BucketInfo { 
            long long rSum = 0, gSum = 0, bSum = 0; 
            double rankWeight = 0.0;
            int count = 0; 
        };
        QMap<QRgb, BucketInfo> bucketStats;
        int totalPixels = 0;

        for (int row = 0; row < sampled.height(); ++row) {
            for (int col = 0; col < sampled.width(); ++col) {
                QRgb rgb = sampled.pixel(col, row);
                if (qAlpha(rgb) < 128) continue;

                int r = qRed(rgb), g = qGreen(rgb), b = qBlue(rgb);
                QColor color(r, g, b);
                int h, s, l; color.getHsl(&h, &s, &l);
                double sat = s / 255.0, lig = l / 255.0;

                // 计算排序权重：鲜艳色、中性亮度色得分高
                double vibrancy = sat * (1.0 - std::abs(lig - 0.5) * 2.0);
                double weight = 0.2 + 10.0 * std::pow(vibrancy, 2.0);

                // 极白/极黑不做过滤，但权重设低
                if ((lig > 0.97 && sat < 0.05) || (lig < 0.03)) {
                    weight = 0.01; 
                }

                // 5-bit 量化提高色彩区分度
                QRgb rgbKey = qRgb(r & 0xF8, g & 0xF8, b & 0xF8);
                auto& stat = bucketStats[rgbKey];
                stat.rSum += r; stat.gSum += g; stat.bSum += b;
                stat.rankWeight += weight;
                stat.count++;
                totalPixels++;
            }
        }
        if (totalPixels == 0) return {};

        struct FinalBucket { QColor avgColor; double rankWeight; int count; };
        QList<FinalBucket> buckets;
        for (auto it = bucketStats.begin(); it != bucketStats.end(); ++it) {
            const auto& s = it.value();
            buckets.append({ QColor((int)(s.rSum / s.count), (int)(s.gSum / s.count), (int)(s.bSum / s.count)), s.rankWeight, s.count });
        }

        // 相似色合并
        QList<FinalBucket> merged;
        for (const auto& b : buckets) {
            bool found = false;
            int h1, s1, l1; b.avgColor.getHsl(&h1, &s1, &l1);
            for (auto& m : merged) {
                int h2, s2, l2; m.avgColor.getHsl(&h2, &s2, &l2);
                int dh = std::abs(h1 - h2); if (dh > 180) dh = 360 - dh;
                int ds = std::abs(s1 - s2), dl = std::abs(l1 - l2);
                if (dh < 15 && ds < 20 && dl < 15) { // 智能平衡阈值
                    int total = m.count + b.count;
                    m.avgColor = QColor((m.avgColor.red()*m.count + b.avgColor.red()*b.count)/total, (m.avgColor.green()*m.count + b.avgColor.green()*b.count)/total, (m.avgColor.blue()*m.count + b.avgColor.blue()*b.count)/total);
                    m.rankWeight += b.rankWeight; m.count = total;
                    found = true; break;
                }
            }
            if (!found) merged.append(b);
        }

        // 智能选色：动态显著性排序 + 空间排斥
        QVector<QPair<QColor, float>> result;
        struct Candidate { QColor color; double score; int count; int h, s, l; };
        QList<Candidate> candidates;
        for (const auto& m : merged) {
            int h, s, l; m.avgColor.getHsl(&h, &s, &l);
            candidates.append({ m.avgColor, m.rankWeight, m.count, h, s, l });
        }

        while (result.size() < 10 && !candidates.isEmpty()) {
            int bestIdx = -1; double maxScore = -1e9;
            for (int i = 0; i < candidates.size(); ++i) {
                const auto& c = candidates[i];
                double score = c.score;
                for (const auto& r : result) {
                    int h2, s2, l2; r.first.getHsl(&h2, &s2, &l2);
                    if (c.s < 25 && s2 < 25) { if (std::abs(c.l - l2) < 40) score *= 0.1; }
                    else {
                        int dh = std::abs(c.h - h2); if (dh > 180) dh = 360 - dh;
                        if (dh < 40) score *= (dh / 40.0);
                        if (dh < 20 && std::abs(c.l - l2) < 30) score *= 0.2;
                    }
                }
                if (score > maxScore) { maxScore = score; bestIdx = i; }
            }
            if (bestIdx != -1 && maxScore > 0) {
                result.append({ candidates[bestIdx].color, (float)candidates[bestIdx].count / totalPixels });
                candidates.removeAt(bestIdx);
            } else break;
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
        // 2026-06-xx 物理修复：在 hashKey 中加入 v14 标识，强制失效旧缓存
        QString hashKey = QString("%1_%2_%3_%4_v14").arg(path).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch()).arg(size);
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
