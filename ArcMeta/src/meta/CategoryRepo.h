#pragma once

#include <string>
#include <vector>
#include <QString>
#include <QMap>
#include <atomic>
#include <functional>
#include <map>
#include "sqlite3.h"

namespace ArcMeta {

struct Category {
    int id = 0;
    int parentId = 0;
    std::wstring name;
    std::wstring color;
    std::vector<std::wstring> presetTags;
    int sortOrder = 0;
    bool pinned = false;
    bool encrypted = false;
    std::wstring encryptHint;
    uint64_t physicalFrn = 0;
    std::wstring physicalPath;
};

/**
 * @brief 分类项记录（含路径提示）
 */
struct CategoryItem {
    std::string fileId128;
    std::wstring pathHint;
};

/**
 * @brief 分类持久层
 * 彻底废除数据库，全量转向 SCCH 架构
 */
class CategoryRepo {
public:
    // 2026-06-xx 物理同步：与 CategoryModel.cpp 定义的系统项 ID 保持绝对一致
    static constexpr int TRASH_CATEGORY_ID    = -8;
    static constexpr int UNCATEGORIZED_CAT_ID = -2;

    /**
     * @brief 获取默认分类颜色：深灰色 (#555555)
     */
    static std::wstring getDefaultColor() { return L"#555555"; }

    static bool add(Category& cat);
    static bool update(const Category& cat);
    static Category getById(int id);
    static int findCategoryId(int parentId, const std::wstring& name);
    static int findByFrn(uint64_t frn);
    static bool updatePhysicalMapping(int id, uint64_t frn, const std::wstring& path);
    static bool remove(int id);
    static bool reorder(int parentId, bool ascending);
    static bool reorderAll(bool ascending);
    static std::vector<Category> getAll();
    static std::vector<Category> getRecentlyUsed(int limit);
    static std::vector<std::pair<int, int>> getCounts();
    static int getUniqueItemCount();
    static int getUncategorizedItemCount();
    static QMap<QString, int> getSystemCounts();
    static QStringList getSystemCategoryPaths(const QString& type);

    // 条目关联逻辑
    static bool addItemToCategory(int categoryId, const std::string& fileId128, const std::wstring& pathHint = L"");
    static bool removeItemFromCategory(int categoryId, const std::string& fileId128);
    static bool removeAllCategories(const std::string& fileId128);
    static bool removeAllCategoriesBatch(const std::vector<std::string>& fids);
    static std::vector<int> getItemCategoryIds(const std::string& fid);
    static bool moveToTrashBatch(const std::vector<std::string>& fids);

    static bool restoreFromTrash(const std::string& fileId128);
    static bool restoreFromTrashBatch(const std::vector<std::string>& fids);
    static bool permanentlyDelete(const std::string& fileId128);
    static bool permanentlyDeleteBatch(const std::vector<std::string>& fids);

    static std::vector<CategoryItem> getItemsInCategory(int categoryId);
    static std::vector<CategoryItem> getItemsInCategories(const std::vector<int>& categoryIds);
    static std::vector<CategoryItem> getItemsRecursive(int categoryId);
    static std::vector<int> getSubtreeIds(int categoryId);

    // 废弃接口（保持兼容）
    static std::vector<std::string> getFileIdsInCategory(int categoryId);
    static std::vector<std::string> getFileIdsRecursive(int categoryId);

    static void saveImmediately();

    /**
     * @brief 2026-06-xx 物理修复：在主线程预热缓存管理器，确保定时器线程归属正确
     */
    static void initialize();

    // 增量计数接口 (Part 4)
    static int getTotalFileCount();
    static int getUncategorizedCount();
    static void setTotalFileCount(int count);
    static void setCategorizedCount(int count);
    static void incrementTotalFileCount(int delta);
    static void incrementCategorizedCount(int delta);
    
    /**
     * @brief 强制执行全量计数重计 (物理账本对账)
     */
    static void fullRecount();

    /**
     * @brief 统计指标更新原子化
     * @param key 统计项 Key (total_file_count, categorized_count)
     * @param delta 增量
     */
    static void updatePersistentStat(const std::string& key, int delta);

    /**
     * @brief 批量执行 FID 处理任务
     */
    static bool executeFidBatch(const std::vector<std::string>& fids, std::function<bool(sqlite3*, const std::string&)> action);

    /**
     * @brief 自动同步已分类计数
     */
    static void syncCategorizedCountForFid(const std::string& fid);

    // 2026-06-xx 逻辑收拢：系统指标键名常量
    static constexpr const char* STAT_TOTAL_FILES = "total_file_count";
    static constexpr const char* STAT_CATEGORIZED = "categorized_count";

    static std::atomic<int> s_totalFileCount;
    static std::atomic<int> s_categorizedCount;
};

} // namespace ArcMeta
