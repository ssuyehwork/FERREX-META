#include "SyncStatusService.h"
#include "../meta/DatabaseManager.h"
#include <QDebug>

namespace ArcMeta {

SyncStatusService& SyncStatusService::instance() {
    static SyncStatusService inst;
    return inst;
}

SyncStatusService::SyncStatusService() {
    m_throttleTimer = new QTimer(this);
    m_throttleTimer->setInterval(200); // 200ms 高性能节流窗口
    m_throttleTimer->setSingleShot(true);

    connect(m_throttleTimer, &QTimer::timeout, [this]() {
        emit statusUpdated(isSyncing(), pendingCount());
    });

    // 订阅底层原始信号
    connect(&DatabaseManager::instance(), &DatabaseManager::pendingTasksCountChanged, this, [this](int count) {
        updateState(count);
    }, Qt::QueuedConnection);

    // 初始化同步计数
    m_pendingCount.store(DatabaseManager::instance().getPendingTasksCount());
}

void SyncStatusService::updateState(int count) {
    m_pendingCount.store(count);
    
    // 如果计时器未运行，启动计时器进行节流
    if (!m_throttleTimer->isActive()) {
        m_throttleTimer->start();
    }
}

} // namespace ArcMeta
