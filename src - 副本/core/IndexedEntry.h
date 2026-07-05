#pragma once

#include <QString>

namespace ArcMeta {

/**
 * @brief 紧凑型索引条目
 * 
 * 该结构用于存储文件系统条目的基本信息，
 * 设计为内存高效的结构，支持大规模文件索引。
 */
struct IndexedEntry {
    QString name;           // 文件或目录名
    int64_t size = 0;       // 文件大小 (字节)
    int64_t modifyTime = 0; // 修改时间 (Unix 毫秒)
    int parentIndex = -1;   // 父条目索引，-1 表示根目录
    unsigned __int64 parentFrn = 0; // 临时存储父条目的 FRN (用于两阶段绑定)
    bool isDir = false;     // 是否为目录
    uint32_t attributes = 0; // 文件属性 (FILE_ATTRIBUTE_XXX)
    unsigned __int64 frn = 0; // 文件参考号 (File Reference Number)

    /**
     * @brief 获取文件后缀名
     * @return 小写的后缀名，无后缀时返回空字符串
     */
    QString suffix() const {
        if (isDir) return QString();
        int pos = name.lastIndexOf('.');
        if (pos == -1) return QString();
        return name.mid(pos + 1).toLower();
    }
};

} // namespace ArcMeta
