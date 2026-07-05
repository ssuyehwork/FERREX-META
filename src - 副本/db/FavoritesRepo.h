#pragma once

#include "Database.h"
#include <string>
#include <vector>

namespace ArcMeta {

struct Favorite {
    std::wstring path;
    std::wstring type;
    std::wstring name;
    int sortOrder = 0;
};

/**
 * @brief 收藏夹持久层
 */
class FavoritesRepo {
public:
    static bool add(const Favorite& fav);
    static bool remove(const std::wstring& path);
    static std::vector<Favorite> getAll();
};

} // namespace ArcMeta
