#pragma once

#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <QObject>

namespace ArcMeta {

/**
 * @brief 同步引擎
 * 负责离线变更追平与系统全量扫描逻辑
 */
class SyncEngine : public QObject {
    Q_OBJECT
public:
    static SyncEngine& instance();

    /**
     * @brief 启动增量同步：处理 Synchronize.json 中的待处理目录
     */
    void runIncrementalSync(std::function<void()> onFinished = nullptr);

    /**
     * @brief 检查是否有待处理的同步任务
     */
    bool hasPendingTasks() const;

    /**
     * @brief 启动全量扫描
     * @param drives 待扫描的驱动器列表，若为空则扫描全部固定驱动器
     * @param onProgress 进度回调
     */
    void runFullScan(const std::vector<std::wstring>& drives = {}, 
                    std::function<void(int current, int total, const std::wstring& path)> onProgress = nullptr);

    /**
     * @brief 维护标签聚合表
     */
    void rebuildTagStats();

signals:
    /**
     * @brief 同步状态变更
     * @param running 是否正在执行同步
     */
    void syncStatusChanged(bool running);

private:
    SyncEngine(QObject* parent = nullptr);
    ~SyncEngine() override = default;

    void scanDirectory(const std::filesystem::path& root, std::vector<std::wstring>& metaFiles);
};

} // namespace ArcMeta
