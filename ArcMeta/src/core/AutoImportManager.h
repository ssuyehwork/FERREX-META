#pragma once
#include <QObject>
#include <QTimer>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_set>
#include <cstdint>

namespace ArcMeta {

/**
 * @brief 2026-07-xx 按照 Plan-67/68：NTFS 托管文件夹自动入库管理器
 */
class AutoImportManager : public QObject {
    Q_OBJECT
public:
    static AutoImportManager& instance();

    // 启动/停止监听
    void startListening();
    void stopListening();

    /**
     * @brief 2026-08-xx 物理同步：扫描所有盘符，补全物理存在但逻辑缺失的托管库根分类
     */
    void syncAllManagedLibraries();

    // 2026-07-xx 按照 Plan-119：记录与获取最近访问文件夹
    static void recordRecentVisitedFolder(const std::wstring& path);
    static QStringList getRecentVisitedFolders(const std::wstring& volSerial);

    /**
     * @brief 2026-07-xx 按照 Plan-118：获取磁盘对应的托管库物理绝对路径
     * @param pathOrVolSerial 路径或卷序列号
     */
    static std::wstring getManagedLibraryPath(const std::wstring& pathOrVolSerial);

private slots:
    // 订阅 MftReader 发现的新增条目
    void onEntryAdded(uint64_t key);
    // 2026-07-xx 按照 Plan-120：订阅 USN 触发的更新条目
    void onEntryUpdated(uint64_t key);
    // 2026-08-xx 按照 Plan-126：处理 USN 移除信号
    void onEntryRemoved(uint64_t key);
    // 去抖超时，合并写入数据库
    void processImportQueue();

private:
    AutoImportManager(QObject* parent = nullptr);
    ~AutoImportManager() override;

    bool checkAndGetManagedPath(const std::wstring& path, std::wstring& outManagedFolder);
    void handleRecursiveIngestion(const std::wstring& rootPath);

    /**
     * @brief 2026-08-xx 按照 Plan-126：基于 FRN 链的高效托管路径过滤
     */
    bool isUnderManagedLibrary(uint64_t key);

    QTimer* m_debounceTimer = nullptr;
    std::vector<std::wstring> m_pendingPaths;
    std::mutex m_queueMutex;
    bool m_isListening = false;

    // 2026-08-xx [Plan-131 方案 B]：托管库根目录 FRN 缓存
    std::unordered_set<uint64_t> m_managedFrnCache;
};

} // namespace ArcMeta
