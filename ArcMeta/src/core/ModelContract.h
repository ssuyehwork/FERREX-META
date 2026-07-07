#pragma once
#include <Qt>

namespace ArcMeta {

/**
 * @brief 工业级模型契约 (ModelContract)
 * 物理统一全应用 Role 定义，彻底解决跨组件 Role 冲突问题。
 */
enum CommonRole {
    // 基础角色 (UserRole + 0..100)
    TypeRole            = Qt::UserRole + 0,  // 类型 (file/folder/category/system/bookmark)
    IdRole              = Qt::UserRole + 1,  // 数据库 ID (分类 ID 等)
    NameRole            = Qt::UserRole + 2,  // 原始名称
    PathRole            = Qt::UserRole + 3,  // 物理路径
    ColorRole           = Qt::UserRole + 4,  // 颜色标记 (Hex)
    RatingRole          = Qt::UserRole + 5,  // 星级评级 (0-5)
    TagsRole            = Qt::UserRole + 6,  // 标签列表 (QStringList)
    
    // 状态角色 (UserRole + 101..200)
    PinnedRole          = Qt::UserRole + 101, // 置顶状态 (快速访问镜像)
    IsLockedRole        = Qt::UserRole + 102, // 锁定/置顶状态 (列表显示)
    EncryptedRole       = Qt::UserRole + 103, // 是否加密
    EncryptHintRole     = Qt::UserRole + 104, // 加密提示
    ManagedRole         = Qt::UserRole + 105, // 是否受控 (已在 SCCH 中登记)
    IsEmptyRole         = Qt::UserRole + 106, // 是否为空目录
    CategoryIdRole      = Qt::UserRole + 107, // 所属分类 ID
    RegistrationProgressRole = Qt::UserRole + 205, // 文件夹入库进度 (double, 0.0 ~ 1.0)
    
    // UI/渲染角色 (UserRole + 201..300)
    AspectRatioRole     = Qt::UserRole + 201, // 图像宽高比
    HasThumbnailRole    = Qt::UserRole + 202, // 是否拥有物理缩略图
    PalettesRole        = Qt::UserRole + 203, // 物理色板数据
    CountRole           = Qt::UserRole + 204  // 子项数量
};

} // namespace ArcMeta
