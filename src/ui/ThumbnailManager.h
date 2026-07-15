#pragma once

#include <QObject>
#include <QPixmap>
#include <QCache>
#include <QSet>
#include <QThreadPool>
#include <QMutex>
#include <QString>

namespace FERREX {

class ThumbnailManager : public QObject {
    Q_OBJECT
public:
    static ThumbnailManager& instance() {
        static ThumbnailManager inst;
        return inst;
    }

    // 请求缩略图物理资产（如果是多媒体文件）
    // 返回：若缓存命中则同步返回可用 Pixmap，未命中则立即返回空，并在工作线程池内安排 LIFO 异步任务加载
    QPixmap requestThumbnail(uint64_t key, const QString& fullPath, const QString& ext, int size, int64_t fileSize, int64_t mtime);

    // 平滑调节高速变焦相关控制
    void preScaleCache(double factor); // 按比例无损缩放双轨 LRU 缓存以作为后续拉伸源
    void clearCache();

    QThreadPool* getThreadPool() const { return m_pool; }

signals:
    // 异步加载成功后，通过信号精准通知观察者进行 UI 重绘
    void thumbnailReady(uint64_t key, const QPixmap& pixmap, double aspectRatio);
    void thumbnailFailed(uint64_t key);

private:
    ThumbnailManager();
    ~ThumbnailManager() override;

    QThreadPool* m_pool = nullptr;
    mutable QMutex m_cacheMutex;
    QCache<QString, QPixmap> m_l1Cache;       // L1 精确匹配缓存 (Key: key_size_mtime)
    QCache<QString, QPixmap> m_l2DoubleTrack;  // L2 变焦兜底双轨缓存 (Key: key)
    QSet<uint64_t> m_pendingKeys;
    QSet<uint64_t> m_failedKeys;
};

} // namespace FERREX
