#ifndef ARCMETA_METADATA_DEFS_H
#define ARCMETA_METADATA_DEFS_H

#include <string>
#include <vector>
#include <QString>

#include <QColor>

namespace ArcMeta {

struct PaletteEntry {
    QColor color;
    float ratio;
    PaletteEntry() : ratio(0.0f) {}
    PaletteEntry(const QColor& c, float r) : color(c), ratio(r) {}
};

/**
 * @brief 文件夹级别的元数据
 */
struct FolderMeta {
    std::wstring sortBy;
    std::wstring sortOrder;
    int rating;
    std::wstring color;
    std::vector<std::wstring> tags;
    bool pinned;
    std::wstring note;
    std::wstring url;
    bool encrypted;
    std::string encryptSalt;
    std::string encryptIv;
    std::string encryptVerifyHash;
    std::string fileId128; // 128-bit File ID (Hex string)
    std::vector<PaletteEntry> palettes;

    FolderMeta() 
        : sortBy(L"name")
        , sortOrder(L"asc")
        , rating(0)
        , pinned(false)
        , encrypted(false) 
    {}

    bool isDefault() const {
        return sortBy == L"name" && sortOrder == L"asc" && rating == 0 &&
               color.empty() && tags.empty() && !pinned && note.empty() && url.empty() && !encrypted && fileId128.empty() && palettes.empty();
    }
};

/**
 * @brief 单个条目（文件或子文件夹）的元数据
 */
struct ItemMeta {
    std::wstring type; // "file" | "folder"
    int rating;
    std::wstring color;
    std::vector<std::wstring> tags;
    bool pinned;
    std::wstring note;
    std::wstring url;
    bool encrypted;
    std::string encryptSalt;
    std::string encryptIv;
    std::string encryptVerifyHash;
    std::wstring originalName;
    std::wstring volume;
    std::wstring frn;
    std::string fileId128; // 128-bit File ID (Hex string)
    int ingestionStatus;   // -1: 未知/非托管, 0: 已登记/待处理, 1: 已完成解析
    long long size;
    long long creationTime;   // ctime (毫秒)
    long long modificationTime; // mtime (毫秒)
    long long accessTime;     // atime (毫秒)
    std::vector<PaletteEntry> palettes;

    ItemMeta()
        : type(L"file")
        , rating(0)
        , pinned(false)
        , encrypted(false)
        , ingestionStatus(-1)
        , size(0)
        , creationTime(0)
        , modificationTime(0)
        , accessTime(0)
    {}

    bool hasUserOperations() const {
        return rating > 0 || !color.empty() || !tags.empty() || pinned ||
               !note.empty() || !url.empty() || encrypted || !fileId128.empty() || !palettes.empty();
    }
};

} // namespace ArcMeta

#endif // ARCMETA_METADATA_DEFS_H
