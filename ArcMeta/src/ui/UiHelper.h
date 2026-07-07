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
#include <QMutex>
#include <QMutexLocker>
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

    static QMutex& iconMutex() {
        static QMutex mutex;
        return mutex;
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
        // 2026-xx-xx 按照 Plan-114：扩展视频格式识别 (对应用户要求：“视频文件也要显示缩略图”)
        static const QStringList graphicsExts = {
            "png", "jpg", "jpeg", "bmp", "gif", "webp", "ico", "tiff", "tif",
            "psd", "psb", "ai", "eps", "pdf", "svg", "cdr",
            "sketch", "xd", "fig", "dwg", "dxf", "heic", "raw",
            "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm"
        };
        return graphicsExts.contains(ext.toLower());
    }

    static bool isStandardImage(const QString& ext) {
        // 2026-11-14 按照 Plan-109：区分标准图像与专业图形格式，以便在预览时选择最优渲染链路
        static const QStringList standardExts = {
            "png", "jpg", "jpeg", "bmp", "gif", "webp", "ico"
        };
        return standardExts.contains(ext.toLower());
    }

    static QIcon getIcon(const QString& key, const QColor& color, int size = 18) {
        QIcon icon;
        QPixmap pix = getPixmap(key, QSize(size, size), color);
        if (!pix.isNull()) icon.addPixmap(pix);
        return icon;
    }

    static QMutex& fileIconMutex() {
        static QMutex mutex;
        return mutex;
    }

    static QIcon getFileIcon(const QString& filePath, int size = 18, const QColor& overrideColor = QColor()) {
        Q_UNUSED(overrideColor);
        Q_UNUSED(size);
        
        QFileInfo info(filePath);
        // 2026-06-xx 架构修正：磁盘根目录图标应独立缓存，防止其覆盖通用文件夹图标
        QString key = info.isDir() ? (info.isRoot() ? filePath : "folder") : info.suffix().toLower();
        if (key.length() > 128) key = "unknown";
        
        static QMap<QString, QIcon> s_fileIconCache;

        {
            QMutexLocker locker(&fileIconMutex());
            if (s_fileIconCache.contains(key)) {
                return s_fileIconCache[key];
            }
        }

        // 2026-06-xx 性能警告：QFileIconProvider 在 Windows 下涉及 Shell API，
        // 必须确保其操作的线程安全性。虽然 QIcon 引用计数安全，但 QMap 容器操作非线程安全。
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
        
        QMutexLocker locker(&fileIconMutex());
        s_fileIconCache[key] = icon;
        return icon;
    }

    static QPixmap getPixmap(const QString& key, const QSize& size, const QColor& color) {
        QString cKey = QString("%1_%2_%3_%4").arg(key).arg(size.width()).arg(size.height()).arg(color.rgba());
        
        {
            QMutexLocker locker(&iconMutex());
            if (iconPixmapCache().contains(cKey)) return iconPixmapCache()[cKey];
        }

        // 2026-06-xx 物理加固：在锁外进行耗时的 SVG 渲染，减少锁竞争
        QPixmap rendered = renderIcon(key, size, color);
        if (rendered.isNull()) return rendered;

        QMutexLocker locker(&iconMutex());
        iconPixmapCache().insert(cKey, rendered);
        return rendered;
    }

    static void applyMenuStyle(QWidget* menu) {
        if (!menu) return;
        // 2026-06-xx 物理修复：开启透明背景属性，消除圆角外的直角溢出
        menu->setAttribute(Qt::WA_TranslucentBackground);
        menu->setWindowFlag(Qt::FramelessWindowHint);

        // 2026-07-xx 物理级加固：改用实心三角形 (menu_triangle) 并通过临时物理文件保障渲染
        // 理由：部分环境下 Data URL 可能因长度或特殊字符导致 QSS 加载失败，物理文件最稳健。
        // 2026-07-xx 交互纠偏：将 subcontrol-origin 改回 padding 并增加 8px 右边距，使指示符靠边站。
        QString arrowPath = getSvgTempFilePath("menu_triangle", QColor("#CCCCCC"));

        menu->setStyleSheet(QString(
            "QMenu { background-color: #2D2D2D; color: #EEE; border: 1px solid #444; padding: 4px; border-radius: 8px; }"
            "QMenu::item { padding: 6px 25px 6px 10px; border-radius: 4px; font-size: 12px; }"
            "QMenu::item:selected { background-color: #3E3E42; color: white; }"
            "QMenu::separator { height: 1px; background: #444; margin: 4px 8px; }"
            "QMenu::right-arrow { "
            "  image: url(%1); "
            "  subcontrol-origin: padding; "
            "  subcontrol-position: center right; "
            "  right: 8px; "
            "}"
        ).arg(arrowPath));
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

    struct LabColor { double l, a, b; };

    /**
     * @brief 将 RGB 颜色转换为 CIELAB 空间
     * 2026-06-xx 物理级 1:1 实现：采用 D65 标准光源及 sRGB 伽马校正
     */
    static LabColor rgbToLab(const QColor& color) {
        double r = color.red() / 255.0;
        double g = color.green() / 255.0;
        double b = color.blue() / 255.0;

        r = (r > 0.04045) ? std::pow((r + 0.055) / 1.055, 2.4) : r / 12.92;
        g = (g > 0.04045) ? std::pow((g + 0.055) / 1.055, 2.4) : g / 12.92;
        b = (b > 0.04045) ? std::pow((b + 0.055) / 1.055, 2.4) : b / 12.92;

        r *= 100.0; g *= 100.0; b *= 100.0;

        // D65 转换矩阵
        double x = r * 0.4124 + g * 0.3576 + b * 0.1805;
        double y = r * 0.2126 + g * 0.7152 + b * 0.0722;
        double z = r * 0.0193 + g * 0.1192 + b * 0.9505;

        x /= 95.047;
        y /= 100.000;
        z /= 108.883;

        auto f = [](double t) {
            return (t > 0.008856) ? std::pow(t, 1.0/3.0) : (7.787 * t) + (16.0/116.0);
        };

        double L = (116.0 * f(y)) - 16.0;
        double A = 500.0 * (f(x) - f(y));
        double B = 200.0 * (f(y) - f(z));

        return {L, A, B};
    }

    /**
     * @brief 计算两个颜色的感知距离 (CIE76 Delta E)
     * 2026-06-xx 工业标准：由于 LAB 空间在感知上是均匀的，欧氏距离即代表视觉差异
     */
    static double calculateDeltaE(const QColor& c1, const QColor& c2) {
        if (!c1.isValid() || !c2.isValid()) return 1000.0;
        LabColor l1 = rgbToLab(c1);
        LabColor l2 = rgbToLab(c2);
        return std::sqrt(std::pow(l1.l - l2.l, 2) + std::pow(l1.a - l2.a, 2) + std::pow(l1.b - l2.b, 2));
    }


    /**
     * @brief 获取用于分析的图像 (SVG 强制渲染，位图走 Shell 缩略图)
     */
    static QImage getImageForAnalysis(const QString& path, int size = 256) {
        QFileInfo fi(path);
        if (fi.suffix().toLower() == "svg") {
            // SVG 必须用矢量渲染器直接栅格化，禁止走 Shell 缩略图以防止获取到彩色应用图标
            QSvgRenderer renderer(path);
            if (renderer.isValid()) {
                QImage img(size, size, QImage::Format_ARGB32);
                img.fill(Qt::transparent);
                QPainter painter(&img);
                renderer.render(&painter);
                return img;
            }
        }
        
        QImage img = getShellThumbnail(path, size);
        // 2026-07-xx 按照建议：Shell 失败后回退到 Qt 原生加载
        if (img.isNull()) img.load(path);
        return img;
    }

    /**
     * @brief 从图像中提取调色盘 (工业级优化版)
     * 2026-06-xx 架构重构：彻底弃用外部工具链 (ImageMagick/Ghostscript)，全面转向原生 Shell 引擎
     */
    static QVector<QPair<QColor, float>> extractPalette(const QString& targetFile) {
        QImage targetImg = getImageForAnalysis(targetFile, 256);
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

                // 空间感知权重：给图像中心区域更高权重，避开边角角标区
                double centerX = sampled.width() / 2.0;
                double centerY = sampled.height() / 2.0;
                double maxDist = std::sqrt(centerX * centerX + centerY * centerY);
                double dist = std::sqrt(std::pow(col - centerX, 2) + std::pow(row - centerY, 2));
                double spatialWeight = 1.0 + (1.0 - dist / maxDist) * 0.5;

                // 计算排序权重：鲜艳色、中性亮度色得分高
                double vibrancy = sat * (1.0 - std::abs(lig - 0.5) * 2.0);
                // 降低 vibrancy 幂次，使权重增长更平滑
                double weight = (0.5 + 4.0 * std::pow(vibrancy, 1.5)) * spatialWeight;

                // 2026-07-xx 按照 Plan-28：感知显著性加权模型优化
                if (lig > 0.95 && sat < 0.05) {
                    // 强力惩罚近似白色背景
                    weight = 0.001;
                } else if (lig < 0.15) {
                    // 提升暗部基准权重 (从 0.5 提升至 2.0)，确保黑色背景在统计中具有话语权
                    weight = 2.0 * spatialWeight;
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
            for (auto& m : merged) {
                // 2026-07-xx 按照 Plan-28：引入 DeltaE 感知合并，收紧合并阈值，确保灰阶细节
                double de = calculateDeltaE(b.avgColor, m.avgColor);
                if (de < 10.0) { // 感知极度相近则强制合并
                    int total = m.count + b.count;
                    m.avgColor = QColor(
                        (int)(m.avgColor.red() * m.count + b.avgColor.red() * b.count) / total,
                        (int)(m.avgColor.green() * m.count + b.avgColor.green() * b.count) / total,
                        (int)(m.avgColor.blue() * m.count + b.avgColor.blue() * b.count) / total
                    );
                    m.rankWeight += b.rankWeight; m.count = total;
                    found = true; break;
                }
            }
            if (!found) merged.append(b);
        }

        // 智能选色：动态显著性排序 + 空间排斥
        QVector<QPair<QColor, float>> result;
        struct Candidate { QColor color; double score; int count; };
        QList<Candidate> candidates;
        for (const auto& m : merged) {
            candidates.append({ m.avgColor, m.rankWeight, m.count });
        }

        while (result.size() < 10 && !candidates.isEmpty()) {
            int bestIdx = -1; double maxScore = -1e9;
            for (int i = 0; i < candidates.size(); ++i) {
                const auto& c = candidates[i];
                double score = c.score;
                
                // 2026-07-xx 按照 Plan-28：引入增强型 DeltaE 空间排斥逻辑，杜绝相似色霸屏，对齐精度
                for (const auto& r : result) {
                    double de = calculateDeltaE(c.color, r.first);
                    if (de < 20.0) {
                        score *= 0.01; // 极度排斥感知相近色
                    } else if (de < 45.0) {
                        score *= (de / 45.0) * 0.5; // 中度排斥
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
                // 移除 SIIGBF_THUMBNAILONLY，允许 Shell 在缓存失效时强制提取真实内容
                hr = pFactory->GetImage(nativeSize, SIIGBF_RESIZETOFIT, &hBitmap);
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
