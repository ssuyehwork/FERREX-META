#pragma once

#include "Database.h"
#include "../meta/MetadataDefs.h"
#include <string>
#include <vector>

namespace ArcMeta {

/**
 * @brief 文件条目持久层
 */
class ItemRepo {
public:
    static bool save(const std::wstring& parentPath, const std::wstring& name, const ItemMeta& meta);
    
    /**
     * @brief 2026-05-24 按照用户要求：仅保存/更新磁盘物理属性（Mtime/Ctime等），不触碰用户元数据
     * 2026-06-15 物理加固：补全参数类型至 qint64 以对齐 MSVC 毫秒精度
     */
    static bool saveBasicInfo(const std::wstring& volume, const std::wstring& frn, const std::wstring& path, const std::wstring& parentPath, bool isDir, qint64 mtime, qint64 size, qint64 ctime = 0, const std::string& fileId128 = "");

    static bool removeByFrn(const std::wstring& volume, const std::wstring& frn);
    static bool physicalRemove(const std::wstring& path);
    static bool restoreByPath(const std::wstring& path);
    static bool markAsDeleted(const std::wstring& volume, const std::wstring& frn);
    
    /**
     * @brief 通过 FRN 获取当前数据库记录的路径
     */
    static std::wstring getPathByFrn(const std::wstring& volume, const std::wstring& frn);

    /**
     * @brief 物理更新路径
     */
    static bool updatePath(const std::wstring& volume, const std::wstring& frn, const std::wstring& newPath, const std::wstring& newParentPath);

    /**
     * @brief 2026-04-12 按照用户要求：基于数据库的文件名关键词搜索
     * @param keyword 搜索关键词
     * @param parentPath 局部搜索时指定父路径，为空则全局搜索
     * @return 匹配的文件物理路径列表（最多 300 条）
     */
    static QStringList searchByKeyword(const QString& keyword, const QString& parentPath = "");

    /**
     * @brief 获取未分类的文件路径
     */
    static QStringList getUncategorizedPaths();

    /**
     * @brief 获取未标签的文件路径
     */
    static QStringList getUntaggedPaths();

    /**
     * @brief 轻量级条目记录，用于虚拟化模型索引
     */
    struct ItemRecord {
        QString volume;
        QString frn;
        QString path; 
        bool isDir = false;
        bool isCategory = false;
        int categoryId = 0;
        QString categoryName;
        QString categoryColor;
    };

    /**
     * @brief 2026-06-xx 新增：根据系统项类型获取对应的物理文件路径列表
     */
    static QStringList getPathsBySystemType(const QString& type);

    /**
     * @brief 2026-06-xx 新增：获取轻量级记录列表以支持虚拟化视图
     */
    static std::vector<ItemRecord> getItemRecordsBySystemType(const QString& type);
    static std::vector<ItemRecord> searchRecordsByKeyword(const QString& keyword, const QString& parentPath = "");
    static std::vector<ItemRecord> getRecordsInCategory(int categoryId);
};

} // namespace ArcMeta
