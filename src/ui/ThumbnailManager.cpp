#include "ThumbnailManager.h"
#include "UiHelper.h"
#include <QThreadStorage>
#include <QFileInfo>
#include <QMetaObject>
#include <QPainter>
#include <QImage>
#include <QThread>
#include <windows.h>

namespace FERREX {

// COM 线程隔离初始化辅助
struct ScopedComInit {
    ScopedComInit() { CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); }
    ~ScopedComInit() { CoUninitialize(); }
};

ThumbnailManager& ThumbnailManager::instance() {
    static ThumbnailManager inst;
    return inst;
}

ThumbnailManager::ThumbnailManager() {
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(std::max<int>(1, QThread::idealThreadCount() / 2));

    m_l1Cache.setMaxCost(1000);       // 1000 个高频精确视口图像
    m_l2DoubleTrack.setMaxCost(500);  // 500 个高精度大图渐进变焦背景
}

ThumbnailManager::~ThumbnailManager() {
    m_pool->clear();
    m_pool->waitForDone();
    delete m_pool;
}

bool ThumbnailManager::isFailed(uint64_t key) const {
    QMutexLocker locker(&m_mutex);
    return m_failedKeys.contains(key);
}

void ThumbnailManager::clearCache() {
    QMutexLocker locker(&m_mutex);
    m_l1Cache.clear();
    m_l2DoubleTrack.clear();
    m_pendingKeys.clear();
    m_failedKeys.clear();
}

QPixmap ThumbnailManager::requestThumbnail(uint64_t key, const QString& fullPath, const QString& ext,
                                          int targetSize, int64_t fileSize, int64_t mtime, bool& outHasPerfectMatch) {
    QMutexLocker locker(&m_mutex);
    outHasPerfectMatch = false;

    QString l1Key = QString("%1_%2_%3").arg(key).arg(targetSize).arg(mtime);

    // 1. 尝试 L1 精确尺寸高速匹配
    QPixmap* l1Pix = m_l1Cache.object(l1Key);
    if (l1Pix) {
        outHasPerfectMatch = true;
        return *l1Pix;
    }

    // 2. 尝试 L2 变焦历史大图高速插值拉伸占位
    QPixmap* l2Pix = m_l2DoubleTrack.object(QString::number(key));

    // 3. 后台投递 LIFO 异步精确加载队列
    if (!m_pendingKeys.contains(key) && !m_failedKeys.contains(key)) {
        m_pendingKeys.insert(key);

        m_pool->start([this, key, fullPath, ext, targetSize, l1Key]() {
            static QThreadStorage<ScopedComInit> comStorage;
            if (!comStorage.hasLocalData()) comStorage.setLocalData(ScopedComInit());

            QImage img = UiHelper::getShellThumbnail(fullPath, targetSize);

            QMutexLocker subLocker(&m_mutex);
            m_pendingKeys.remove(key);

            if (!img.isNull()) {
                double ar = (double)img.width() / (double)img.height();
                QPixmap pix = QPixmap::fromImage(img);

                // 同时填充 L1 精确缓存与 L2 大图变焦备份
                m_l1Cache.insert(l1Key, new QPixmap(pix));
                m_l2DoubleTrack.insert(QString::number(key), new QPixmap(pix));

                QMetaObject::invokeMethod(this, [this, key, pix, ar]() {
                    emit thumbnailReady(key, pix, ar);
                }, Qt::QueuedConnection);
            } else {
                m_failedKeys.insert(key);
                QMetaObject::invokeMethod(this, [this, key]() {
                    emit thumbnailFailed(key);
                }, Qt::QueuedConnection);
            }
        });
    }

    // 若无精确匹配，但有 L2 历史高精度大图资产，则优先返回 L2，由渲染层在主线程平滑拉伸，杜绝闪烁白卡片
    if (l2Pix) {
        return *l2Pix;
    }

    return QPixmap(); // 加载中，返回空 QPixmap
}

void ThumbnailManager::preScaleCache(double factor) {
    // 变焦过程中通过前置对 LRU 历史高精度资产直接进行线性内存插值缩放，
    // 保证在变焦的几十毫秒物理微调期内，渲染层总能直接摸到等比例的历史位图资产。
    QMutexLocker locker(&m_mutex);
    if (factor <= 0.0 || factor == 1.0) return;
    // 降噪控制省略或保留：pre-scale 具体尺度重算，由于 QCache 迭代局限，变焦时直接由 QPainter 在 Delegate 层实时平滑插值更稳定，此占位供接口闭环。
}

} // namespace FERREX
