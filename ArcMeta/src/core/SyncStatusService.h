#ifndef ARCMETA_SYNC_STATUS_SERVICE_H
#define ARCMETA_SYNC_STATUS_SERVICE_H

#include <QObject>
#include <QTimer>
#include <atomic>

namespace ArcMeta {

/**
 * @brief 同步状态服务 (解耦试点)
 * 负责从底层同步引擎接收高频信号，并按 UI 刷新率进行节流分发。
 */
class SyncStatusService : public QObject {
    Q_OBJECT
public:
    static SyncStatusService& instance();

    /**
     * @brief 是否正在同步中 (线程安全)
     */
    bool isSyncing() const { return m_pendingCount.load() > 0; }

    /**
     * @brief 获取待处理任务数 (线程安全)
     */
    int pendingCount() const { return m_pendingCount.load(); }

signals:
    /**
     * @brief 节流后的状态更新信号 (主线程触发)
     */
    void statusUpdated(bool syncing, int count);

private:
    SyncStatusService();
    ~SyncStatusService() override = default;

    std::atomic<int> m_pendingCount{0};
    QTimer* m_throttleTimer = nullptr;

    void updateState(int count);
};

} // namespace ArcMeta

#endif // ARCMETA_SYNC_STATUS_SERVICE_H
