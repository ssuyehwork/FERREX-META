#pragma once
#include <QString>
#include <vector>
#include <cstdint>
#include <QList>
#include <QColor>
#include <utility>

namespace ArcMeta {

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

    // 2026-06-xx 物理对标：注入核心元数据，杜绝 UI 渲染时的同步 I/O
    int rating = 0;
    QString color;
    QStringList tags;
    std::string fileId;
    bool pinned = false;
    bool encrypted = false;
    double registrationProgress = -1.0; // 初始为 -1.0 表示未计算
    QString url;  // 2026-07-xx 支撑筛选：链接
    QString note; // 2026-07-xx 支撑筛选：备注
    int width = 0;
    int height = 0;

    // 2026-06-xx 极致优化：预取物理属性，实现渲染零 I/O
    long long size = 0;
    long long mtime = 0;
    long long ctime = 0;
    long long atime = 0;
    bool isEmpty = false;
    bool isManaged = false; // 预存受控状态
    QString suffix;
    std::vector<std::pair<QColor, float>> palettes; // 烘焙物理色板，消除 filterAcceptsRow 锁争抢
};

/**
 * @brief 工业级索引条目
 * 用于 MFT 扫描与高速缓存
 */
struct IndexedEntry {
    QString name;           // 文件或目录名
    int64_t size = 0;       // 文件大小 (字节)
    int64_t modifyTime = 0; // 修改时间 (Unix 毫秒)
    int parentIndex = -1;   // 父条目索引，-1 表示根目录
    uint64_t parentFrn = 0; // 临时存储父条目的 FRN (用于两阶段绑定)
    bool isDir = false;     // 是否为目录
    uint32_t attributes = 0; // 文件属性 (FILE_ATTRIBUTE_XXX)
    uint64_t frn = 0; // 文件参考号 (File Reference Number)

    /**
     * @brief 获取文件后缀名
     * @return 小写的后缀名，无后缀时返回空字符串
     */
    QString suffix() const {
        if (isDir) return QString();
        int dotIdx = name.lastIndexOf('.');
        if (dotIdx == -1) return QString();
        return name.mid(dotIdx + 1).toLower();
    }
};

} // namespace ArcMeta
