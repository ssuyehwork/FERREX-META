#pragma once

#include "Database.h"
#include <string>
#include <vector>

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
};

/**
 * @brief 分类持久层
 */
class CategoryRepo {
public:
    static void setJsonMode(bool enabled);
    static bool isJsonMode();
    static bool syncDatabaseAndJson();

private:
    static bool m_isJsonMode;

public:
    static bool add(Category& cat);
    static bool update(const Category& cat);
    static bool remove(int id);
    static bool reorder(int parentId, bool ascending);
    static bool reorderAll(bool ascending);
    static std::vector<Category> getAll();
    static std::vector<Category> getRecentlyUsed(int limit);
    static std::vector<std::pair<int, int>> getCounts();
    static int getUniqueItemCount();
    static int getUncategorizedItemCount();
    static QMap<QString, int> getSystemCounts();

    // 条目关联逻辑
    static bool addItemToCategory(int categoryId, const std::string& fileId128);
    static bool removeItemFromCategory(int categoryId, const std::string& fileId128);
    static std::vector<std::string> getFileIdsInCategory(int categoryId);
    static std::vector<std::string> getFileIdsRecursive(int categoryId);
};

} // namespace ArcMeta
