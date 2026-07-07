#include "IndexedEntry.h"
#include "../meta/MetadataManager.h"
#include <QFileInfo>
#include <QDir>

namespace ArcMeta {

ItemRecord ItemRecord::create(const QString& path) {
    ItemRecord r;
    QString nPath = QDir::toNativeSeparators(path);
    std::wstring wPath = nPath.toStdWString();
    QFileInfo info(nPath);

    // 1. 物理属性采样 (零 I/O 核心)
    std::string fid;
    long long size = 0, ctime = 0, mtime = 0, atime = 0;
    MetadataManager::fetchWinApiMetadataDirect(wPath, fid, nullptr, &size, nullptr, &ctime, &mtime, &atime);

    r.path = nPath;
    r.size = size;
    r.ctime = ctime;
    r.mtime = mtime;
    r.atime = atime;

    // 2. 核心元数据注入 (确保 width/height/palettes 物理对齐)
    auto meta = MetadataManager::instance().getMeta(wPath);
    r.isDir = info.isDir(); // 物理属性优先，确保未索引目录显示正常
    r.rating = meta.rating;
    r.color = QString::fromStdWString(meta.color);
    r.tags = meta.tags;
    r.fileId = meta.fileId128;
    r.pinned = meta.pinned;
    r.encrypted = meta.encrypted;
    r.url = QString::fromStdWString(meta.url);
    r.note = QString::fromStdWString(meta.note);
    r.width = meta.width;
    r.height = meta.height;
    r.isManaged = meta.hasUserOperations();
    for (const auto& pe : meta.palettes) {
        r.palettes.push_back({pe.color, pe.ratio});
    }

    if (r.isDir) {
        QDir sub(nPath);
        r.isEmpty = sub.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty();
    } else {
        r.suffix = info.suffix().toLower();
    }
    return r;
}

} // namespace ArcMeta
