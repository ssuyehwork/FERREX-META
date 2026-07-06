#ifndef ARCMETA_METADATA_MANAGER_H
#define ARCMETA_METADATA_MANAGER_H

#include "MetadataDefs.h"
#include <QObject>
#include <QString>
#include <QTimer>
#include <QStringList>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <string>

namespace ArcMeta {

/**
 * @brief 内存元数据镜像结构
 */
struct RuntimeMeta {
    int rating = 0;
    std::wstring color;
    QStringList tags;
    std::wstring note;
    bool pinned = false;
    bool encrypted = false;
    std::vector<PaletteEntry> palettes;

    /**
     * @brief 判定是否有用户操作过的信息，作为“已录入/受控”状态的感应逻辑
     * 2026-06-xx 按照用户要求：只要有任何元数据修改，即视为数据库已录入项
     */
    bool hasUserOperations() const {
        return rating > 0 || !color.empty() || !tags.isEmpty() || !note.empty() || pinned || encrypted;
    }
};

/**
 * @brief 元数据管理器
 */
class MetadataManager : public QObject {
    Q_OBJECT
public:
    static MetadataManager& instance();

    void initFromDatabase();
    void initFromJsonMode();
    RuntimeMeta getMeta(const std::wstring& path);

    /**
     * @brief 2026-06-xx 按照用户要求：在 JSON 内存模式下执行多维搜索
     * @param keyword 关键词
     * @return 匹配的物理路径列表
     */
    QStringList searchInCache(const QString& keyword);

    void setRating(const std::wstring& path, int rating);
    void setColor(const std::wstring& path, const std::wstring& color);
    void setPinned(const std::wstring& path, bool pinned);
    void setTags(const std::wstring& path, const QStringList& tags);
    void setNote(const std::wstring& path, const std::wstring& note);
    void setEncrypted(const std::wstring& path, bool encrypted);
    void setPalettes(const std::wstring& path, const QVector<QPair<QColor, float>>& palettes);
    QVector<QColor> getPalettes(const std::wstring& path);

    void renameItem(const std::wstring& oldPath, const std::wstring& newPath);
    void removeMetadataSync(const std::wstring& path);

    /**
     * @brief 物理同步元数据
     * 2026-06-xx 按照用户要求：支持主动触发物理元数据（File ID 等）的获取与保存
     */
    void syncPhysicalMetadata(const std::wstring& path);

    /**
     * @brief 同步获取文件的 128-bit File ID (或 Fallback ID)
     * 2026-06-15 物理加固：确保在建立分类关联前指纹已就绪
     */
    std::string getFileIdSync(const std::wstring& path);

    /**
     * @brief 检查是否存在待同步的元数据目录
     */
    bool hasPendingSync() const;

    /**
     * @brief 加载待同步清单
     */
    QStringList getPendingSyncDirs();

    /**
     * @brief 从日志中移除指定的 FID 条目
     */
    void removeFidsFromLog(const QStringList& fids);

    /**
     * @brief 获取路径所在磁盘的卷序列号
     */
    static std::wstring getVolumeSerialNumber(const std::wstring& path);

    /**
     * @brief 记录事务日志 Synchronize.json
     */
    void addToSyncLog(const std::wstring& dirPath);

    /**
     * @brief 内部辅助：通过 WinAPI 获取 File ID 和基础元数据
     * 2026-06-xx 物理修复：已升级为公开静态成员，支持跨模块同步入库
     * 2026-06-xx 物理补完：增加 outFrn 参数以获取物理索引，彻底杜绝数据库主键冲突
     */
    static bool fetchWinApiMetadataDirect(const std::wstring& path, std::string& outId128, std::wstring* outFrn = nullptr, long long* outSize = nullptr, std::wstring* outType = nullptr, long long* outCtime = nullptr, long long* outMtime = nullptr, long long* outAtime = nullptr);

signals:
    // 2026-05-27 物理修复：信号参数由 std::wstring 改为 QString
    // 理由：std::wstring 未注册为元类型，导致跨线程发射时（如数据库预热阶段）触发 QueuedConnection 失败从而引起崩溃。
    void metaChanged(const QString& path);

    /**
     * @brief 待同步状态变更信号
     * @param hasPending 是否存在待处理数据
     */
    void pendingSyncChanged(bool hasPending);

private:
    MetadataManager(QObject* parent = nullptr);
    ~MetadataManager() override = default;

    std::unordered_map<std::wstring, RuntimeMeta> m_cache;
    mutable std::shared_mutex m_mutex;
    
    // 2026-05-25 按照用户要求：改用单例计时器与脏路径集，彻底解决计时器风暴
    QTimer* m_batchTimer = nullptr;
    std::unordered_set<std::wstring, std::hash<std::wstring>> m_dirtyPaths;

    void persistAsync(const std::wstring& path);
    void debouncePersist(const std::wstring& path);

    void saveSyncLog();
};

} // namespace ArcMeta

#endif // ARCMETA_METADATA_MANAGER_H
