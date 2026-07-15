#include "ThumbnailManager.h"
#include "UiHelper.h"
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QThreadStorage>
#include <QtConcurrent/QtConcurrent>

namespace FERREX {

ThumbnailManager::ThumbnailManager() {
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(std::max(2, QThread::idealThreadCount() / 2)); // 保证流畅度
    m_l1Cache.setMaxCost(500);
    m_l2DoubleTrack.setMaxCost(500);
}

ThumbnailManager::~ThumbnailManager() {
    m_pool->waitForDone();
}

QPixmap ThumbnailManager::requestThumbnail(uint64_t key, const QString& fullPath, const QString& ext, int size, int64_t fileSize, int64_t mtime) {
    QMutexLocker locker(&m_cacheMutex);
    QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(mtime);

    // L1 精确命中
    if (m_l1Cache.contains(cacheKey)) {
        return *m_l1Cache.object(cacheKey);
    }

    // 失败过则返回空，退化为默认
    if (m_failedKeys.contains(key)) {
        return QPixmap();
    }

    // 已经在提取中，返回 L2 兜底或者空
    if (m_pendingKeys.contains(key)) {
        if (m_l2DoubleTrack.contains(QString::number(key))) {
            return *m_l2DoubleTrack.object(QString::number(key));
        }
        return QPixmap();
    }

    m_pendingKeys.insert(key);

    // 异步在线程池中加载
    QtConcurrent::run(m_pool, [this, key, fullPath, ext, size, cacheKey]() {
        static QThreadStorage<ScopedComInit> comStorage;
        if (!comStorage.hasLocalData()) {
            comStorage.setLocalData(ScopedComInit());
        }

        QImage img;
        if (ext == "svg") {
            QSvgRenderer renderer(fullPath);
            if (renderer.isValid()) {
                img = QImage(QSize(size, size), QImage::Format_ARGB32);
                img.fill(Qt::transparent);
                QPainter painter(&img);
                renderer.render(&painter);
                painter.end();
            }
        } else {
            img = FERREX::UiHelper::getShellThumbnail(fullPath, size);
        }

        if (!img.isNull()) {
            double ar = (double)img.width() / (double)img.height();
            QPixmap pix = QPixmap::fromImage(img);
            if (!pix.isNull()) {
                QMutexLocker locker(&m_cacheMutex);
                m_l1Cache.insert(cacheKey, new QPixmap(pix));
                m_l2DoubleTrack.insert(QString::number(key), new QPixmap(pix));
                m_pendingKeys.remove(key);
                locker.unlock();

                emit thumbnailReady(key, pix, ar);
            } else {
                QMutexLocker locker(&m_cacheMutex);
                m_failedKeys.insert(key);
                m_pendingKeys.remove(key);
                locker.unlock();

                emit thumbnailFailed(key);
            }
        } else {
            QMutexLocker locker(&m_cacheMutex);
            m_failedKeys.insert(key);
            m_pendingKeys.remove(key);
            locker.unlock();

            emit thumbnailFailed(key);
        }
    });

    if (m_l2DoubleTrack.contains(QString::number(key))) {
        return *m_l2DoubleTrack.object(QString::number(key));
    }

    return QPixmap();
}

void ThumbnailManager::preScaleCache(double factor) {
    // 按需预拉伸逻辑 (此处由 QCache 逻辑维护或空置)
}

void ThumbnailManager::clearCache() {
    QMutexLocker locker(&m_cacheMutex);
    m_l1Cache.clear();
    m_l2DoubleTrack.clear();
    m_pendingKeys.clear();
    m_failedKeys.clear();
}

} // namespace FERREX
