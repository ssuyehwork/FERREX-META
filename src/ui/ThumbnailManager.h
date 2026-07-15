#pragma once
#include <QObject>
#include <QPixmap>
#include <QImage>
#include <QCache>
#include <QSet>
#include <QMutex>
#include <QThreadPool>

namespace FERREX {

struct ScopedComInit {
    ScopedComInit();
    ~ScopedComInit();
};

class ThumbnailManager : public QObject {
    Q_OBJECT
public:
    static ThumbnailManager& instance();

    /**
     * @brief 请求缩略图物理资产（如果是多媒体文件）
     * @param key 节点唯一 Key (FRN)
     * @param fullPath 文件绝对全路径
     * @param ext 后缀名 (例如 png)
     * @param targetSize 请求的精确逻辑目标像素尺寸
     * @param fileSize 物理大小 (用于缓存版本效期校验)
     * @param mtime 修改时间 (用于缓存版本效期校验)
     * @param outHasPerfectMatch 出参：标识是否命中 L1 精确尺寸缓存
     * @return 最终可供渲染的 QPixmap。若有历史资产则返回 L2 拉伸占位，否则返回空
     */
    QPixmap requestThumbnail(uint64_t key, const QString& fullPath, const QString& ext,
                             int targetSize, int64_t fileSize, int64_t mtime, bool& outHasPerfectMatch);

    /**
     * @brief 滑动窗口更新时，快速预制比例无损插值拉伸源，阻断卡片白屏闪跃
     */
    void preScaleCache(double factor);

    void clearCache();
    bool isFailed(uint64_t key) const;
    QThreadPool* getPool() const { return m_pool; }

signals:
    // 只在主线程发送和转换 QImage 到 QPixmap，绝对符合 GUI 安全性
    void thumbnailReady(uint64_t key, const QImage& image, double aspectRatio, const QString& l1Key);
    void thumbnailFailed(uint64_t key);

private slots:
    void onThumbnailReady(uint64_t key, const QImage& image, double aspectRatio, const QString& l1Key);

private:
    ThumbnailManager();
    ~ThumbnailManager() override;

    QThreadPool* m_pool = nullptr;
    mutable QMutex m_mutex;

    QCache<QString, QPixmap> m_l1Cache;       // L1 精确匹配尺寸缓存 (Key: key_size_mtime)
    QCache<QString, QPixmap> m_l2DoubleTrack;  // L2 变焦历史大图占位备份 (Key: key)

    QSet<uint64_t> m_pendingKeys;
    QSet<uint64_t> m_failedKeys;
};

} // namespace FERREX
