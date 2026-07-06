#pragma once

#include "Database.h"
#include "../meta/MetadataDefs.h"
#include <string>
#include <vector>

namespace ArcMeta {

/**
 * @brief 文件夹元数据持久层
 */
class FolderRepo {
public:
    /**
     * @brief 保存或更新文件夹元数据
     */
    static bool save(const std::wstring& volume, const std::wstring& path, const FolderMeta& meta);

    /**
     * @brief 获取文件夹元数据
     */
    static bool get(const std::wstring& volume, const std::wstring& path, FolderMeta& meta);

    /**
     * @brief 删除文件夹记录
     */
    static bool remove(const std::wstring& volume, const std::wstring& path);
};

} // namespace ArcMeta
