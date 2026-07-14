#ifndef FERREX_METADATA_DEFS_H
#define FERREX_METADATA_DEFS_H

#include <string>
#include <vector>
#include <QString>

#include <QColor>

namespace FERREX {

struct PaletteEntry {
    QColor color;
    float ratio;
};

struct FolderMeta {
    std::wstring sortBy = L"name";
    std::wstring sortOrder = L"asc";
    std::string fileId128; 
    std::vector<PaletteEntry> palettes;

    bool isDefault() const {
        return sortBy == L"name" && sortOrder == L"asc" && fileId128.empty() && palettes.empty();
    }
};

struct ItemMeta {
    std::wstring originalName;
    std::wstring volume;
    std::wstring frn;
    std::string fileId128; 
    long long size = 0;
    long long creationTime = 0;   
    long long modificationTime = 0; 
    long long accessTime = 0;     
    std::vector<PaletteEntry> palettes;

    bool hasUserOperations() const {
        return !fileId128.empty() || !palettes.empty();
    }
};

} 

#endif 
