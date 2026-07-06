#pragma once

#include <string>
#include <vector>
#include <map>
#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include "MetadataDefs.h"

namespace ArcMeta {

/**
 * @brief 处理 .am_meta.json 的读写类
 * 2026-06-xx 按照用户要求：从 ArcMeta-1 恢复离散元数据管理逻辑，并适配 SHA-256 与物理 FRN 协议
 */
class AmMetaJson {
public:
    /**
     * @param folderPath 目标文件夹的完整路径（不含文件名）
     */
    explicit AmMetaJson(const std::wstring& folderPath);

    /**
     * @brief 加载 .am_meta.json 文件
     */
    bool load();

    /**
     * @brief 安全保存到 .am_meta.json 文件
     */
    bool save() const;

    // 数据访问接口
    FolderMeta& folder() { return m_folder; }
    const FolderMeta& folder() const { return m_folder; }

    std::map<std::wstring, ItemMeta>& items() { return m_items; }
    const std::map<std::wstring, ItemMeta>& items() const { return m_items; }

    /**
     * @brief 移除指定文件名的元数据条目
     */
    void remove(const std::wstring& fileName) { m_items.erase(fileName); }

    /**
     * @brief 静态辅助方法：重命名元数据条目
     */
    static bool renameItem(const QString& folderPath, const QString& oldName, const QString& newName);

private:
    std::wstring m_folderPath;
    std::wstring m_filePath;
    
    FolderMeta m_folder;
    std::map<std::wstring, ItemMeta> m_items;

    // 内部转换辅助
    static QJsonObject folderToEntry(const FolderMeta& meta);
    static FolderMeta entryToFolder(const QJsonObject& obj);
    static QJsonObject itemToEntry(const ItemMeta& meta);
    static ItemMeta entryToItem(const QJsonObject& obj);

    static QString toQString(const std::wstring& ws) { return QString::fromStdWString(ws); }
    static std::wstring toStdWString(const QString& qs) { return qs.toStdWString(); }
};

} // namespace ArcMeta
