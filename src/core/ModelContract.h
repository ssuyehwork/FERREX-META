#pragma once
#include <Qt>
#include <QString>
#include <QStringList>
#include <vector>
#include <cstdint>
#include <memory>

namespace FERREX {

struct ScanFilterState {
    QStringList extensionList;
    bool useRegex = false;
    bool caseSensitive = false;
    bool includeHidden = true;
    bool includeSystem = true;
    bool includeDollar = true;
    bool autoDisplay = false;

    bool isEmpty() const {
        return extensionList.isEmpty() && !useRegex && !caseSensitive && includeHidden && includeSystem && includeDollar && !autoDisplay;
    }
};

/**
 * @brief 文件元数据统一只读记录 (零锁高性能快照结构)
 */
struct FileMetaRecord {
    uint64_t key = 0;
    QString name;
    QString fullPath;
    int64_t size = 0;
    int64_t mtime = 0;
    bool isDirectory = false;
};

/**
 * @brief 抽象检索与元数据只读查询引擎接口
 */
class IDataQueryEngine {
public:
    virtual ~IDataQueryEngine() = default;

    // 异步或同步执行匹配搜寻，仅返回不含状态的物理 Key 列表
    virtual std::vector<uint64_t> queryKeys(
        const QString& keyword,
        const ScanFilterState& filterState
    ) = 0;

    // 极速获取特定条目的只读元数据，屏蔽具体底层存储（如内存、MFT 或 SQLite）的读取细节
    virtual FileMetaRecord getRecordByKey(uint64_t key) const = 0;
};

/**
 * @brief 工业级模型契约 (ModelContract)
 * 物理统一全应用 Role 定义，彻底解决跨组件 Role 冲突问题。
 */
enum CommonRole {
    // 基础角色 (UserRole + 0..100)
    IdRole              = Qt::UserRole + 1,  // 数据库 ID (分类 ID 等)
    NameRole            = Qt::UserRole + 2,  // 原始名称
    PathRole            = Qt::UserRole + 3,  // 物理路径
    
    // 状态角色 (UserRole + 101..200)
    IsLockedRole        = Qt::UserRole + 102, // 锁定/置顶状态 (列表显示)
    EncryptHintRole     = Qt::UserRole + 104, // 加密提示
    InDatabaseRole      = Qt::UserRole + 105, // 是否已录入数据库
    IsEmptyRole         = Qt::UserRole + 106, // 是否为空目录
    CategoryIdRole      = Qt::UserRole + 107, // 所属分类 ID
    
    // UI/渲染角色 (UserRole + 201..300)
    AspectRatioRole     = Qt::UserRole + 201, // 图像宽高比
    HasThumbnailRole    = Qt::UserRole + 202, // 是否拥有物理缩略图
    PalettesRole        = Qt::UserRole + 203, // 物理色板数据
    CountRole           = Qt::UserRole + 204  // 子项数量
};

} // namespace FERREX
