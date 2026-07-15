#pragma once
#include <Qt>
#include <QString>
#include <QStringList>
#include <vector>
#include <cstdint>

namespace FERREX {

struct ScanFilterState;

class IDataQueryEngine {
public:
    virtual ~IDataQueryEngine() = default;
    virtual std::vector<uint64_t> search(
        const QString& text,
        const ScanFilterState& state
    ) = 0;
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
