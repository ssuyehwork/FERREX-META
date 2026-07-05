#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QtConcurrent>
#include <QThreadPool>
#include <QDir>
#include <QDebug>
#include <QTimer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MetadataManager.h"
#include "../db/Database.h"
#include "../db/ItemRepo.h"
#include "../db/FolderRepo.h"
#include "MetadataDefs.h"
#include "AmMetaJson.h"
#include "AllFrnManager.h"
#include "../mft/MftReader.h"
#include "../db/CategoryRepo.h"

#include <windows.h>
#include <fileapi.h>
#include <winbase.h>
#include <handleapi.h>
#include <winnt.h>
#include <sddl.h>


#include <cstdio>
#include <cwchar>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <shared_mutex>

namespace ArcMeta {

// --- 内部静态工具函数 (必须在类成员函数前定义或前向声明) ---

/**
 * @brief 标准化路径
 */
static std::wstring normalizePath(const std::wstring& path) {
    if (path.empty()) return L"";
    QString qp = QDir::toNativeSeparators(QDir::cleanPath(QString::fromStdWString(path)));
    if (qp.length() == 2 && qp.endsWith(':')) qp += '\\';
    if (qp.length() >= 2 && qp[1] == ':') qp[0] = qp[0].toUpper();
    return qp.toStdWString();
}

/**
 * @brief 获取相对路径
 */
static std::wstring getRelativePath(const std::wstring& fullPath) {
    if (fullPath.length() >= 3 && fullPath[1] == L':' && fullPath[2] == L'\\') return fullPath.substr(2);
    return fullPath;
}

/**
 * @brief 物理锚点 Fallback ID 生成 (按照 Memories.md 铁律)
 * 2026-06-xx 物理加固：废除基于路径的哈希，强制回归 FRN 物理标识。
 */
static std::string generateFallbackFid(const std::wstring& vol, const std::wstring& frn) {
    if (vol.empty() || frn.empty()) return "";
    // 格式: FRN:[Volume]:[FRN]
    return "FRN:" + QString::fromStdWString(vol).toUpper().toStdString() + ":" + QString::fromStdWString(frn).toUpper().toStdString();
}

/**
 * @brief SHA-256 确定性 ID 生成 (仅作为最后防线)
 */
static std::string generateDeterministicSha256Id(const std::wstring& path) {
    if (path.empty()) return "";
    std::wstring nPath = normalizePath(path);
    std::wstring vol = MetadataManager::getVolumeSerialNumber(nPath);
    QByteArray seed = QString::fromStdWString(vol + L":" + nPath).toUtf8();
    QByteArray hash = QCryptographicHash::hash(seed, QCryptographicHash::Sha256);
    // 加上 PATHURL 前缀以示区分，避免与物理 ID 混淆
    return "PATHURL:" + hash.left(16).toHex().toUpper().toStdString();
}

/**
 * @brief SHA-256 确定性伪 FRN 生成
 */
static std::wstring generateDeterministicFrn(const std::wstring& path) {
    if (path.empty()) return L"VIRTUAL_EMPTY";
    QByteArray hash = QCryptographicHash::hash(QString::fromStdWString(path).toUtf8(), QCryptographicHash::Sha256);
    return QString(hash.left(8).toHex().toUpper()).toStdWString();
}

/**
 * @brief 卷序列号到驱动器的映射
 */
static std::unordered_map<std::wstring, std::wstring> getVolumeToDriveMap() {
    std::unordered_map<std::wstring, std::wstring> map;
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            std::wstring drive = std::wstring(1, L'A' + i) + L":\\";
            if (GetDriveTypeW(drive.c_str()) == DRIVE_FIXED) {
                std::wstring serial = MetadataManager::getVolumeSerialNumber(drive);
                if (serial != L"UNKNOWN") map[serial] = drive;
            }
        }
    }
    return map;
}

// --- MetadataManager 实现 ---

MetadataManager& MetadataManager::instance() {
    static MetadataManager inst;
    return inst;
}

MetadataManager::MetadataManager(QObject* parent) : QObject(parent) {
    m_batchTimer = new QTimer(this);
    m_batchTimer->setInterval(1500);
    m_batchTimer->setSingleShot(true);
    connect(m_batchTimer, &QTimer::timeout, [this]() {
        std::vector<std::wstring> paths;
        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            paths.assign(m_dirtyPaths.begin(), m_dirtyPaths.end());
            m_dirtyPaths.clear();
        }
        for (const auto& p : paths) persistAsync(p);
    });
}

void MetadataManager::initFromDatabase() {
    std::unordered_map<std::wstring, RuntimeMeta> tempCache;

    // 2026-06-xx 架构修正：从 FERREX_drivers.json 加载磁盘根目录元数据
    QString driversPath = QCoreApplication::applicationDirPath() + "/FERREX_drivers.json";
    QFile dFile(driversPath);
    if (dFile.open(QIODevice::ReadOnly)) {
        QJsonObject root = QJsonDocument::fromJson(dFile.readAll()).object();
        for (auto it = root.begin(); it != root.end(); ++it) {
            std::wstring nPath = normalizePath(it.key().toStdWString());
            QJsonObject m = it.value().toObject();
            RuntimeMeta rm;
            rm.rating = m["rating"].toInt();
            rm.color = m["color"].toString().toStdWString();
            rm.pinned = m["pinned"].toBool();
            rm.note = m["note"].toString().toStdWString();
            QJsonArray tagsArr = m["tags"].toArray();
            for (const auto& t : tagsArr) rm.tags << t.toString();
            tempCache[nPath] = std::move(rm);
        }
    }

    auto volMap = getVolumeToDriveMap();
    QSqlDatabase db = ArcMeta::Database::instance().getThreadDatabase();
    if (!db.isOpen()) return;

    auto loadTable = [&](const char* sql) {
        QSqlQuery query(db);
        query.setForwardOnly(true);
        if (!query.exec(sql)) return;
        while (query.next()) {
            std::wstring dbVol = query.value(0).toString().toStdWString();
            std::wstring dbPath = query.value(1).toString().toStdWString();
            std::wstring finalPath = normalizePath(dbPath);
            if (volMap.count(dbVol)) {
                QString base = QString::fromStdWString(volMap[dbVol]);
                if (base.endsWith('\\')) base.chop(1);
                finalPath = normalizePath((base + QString::fromStdWString(getRelativePath(dbPath))).toStdWString());
            }
            RuntimeMeta meta;
            meta.rating = query.value(2).toInt();
            meta.color = query.value(3).toString().toStdWString();
            QByteArray tagsRaw = query.value(4).toByteArray();
            if (!tagsRaw.isEmpty() && tagsRaw != "[]") {
                QJsonDocument doc = QJsonDocument::fromJson(tagsRaw);
                if (doc.isArray()) { for (const auto& v : doc.array()) meta.tags << v.toString(); }
            }
            meta.pinned = query.value(5).toBool();
            meta.encrypted = query.value(6).toBool();
            meta.note = query.value(7).toString().toStdWString();
            // 2026-06-xx 物理兼容：从数据库加载 palettes (若存在)
            if (query.record().indexOf("palettes") != -1) {
                QByteArray palRaw = query.value("palettes").toByteArray();
                if (!palRaw.isEmpty()) {
                    QJsonDocument doc = QJsonDocument::fromJson(palRaw);
                    if (doc.isArray()) {
                        for (const auto& v : doc.array()) {
                            QJsonObject o = v.toObject();
                            QJsonArray c = o.value("color").toArray();
                            meta.palettes.push_back({QColor(c[0].toInt(), c[1].toInt(), c[2].toInt()), (float)o.value("ratio").toDouble()});
                        }
                    }
                }
            }
            tempCache[finalPath] = std::move(meta);
        }
    };
    loadTable("SELECT volume, path, rating, color, tags, pinned, encrypted, note, palettes FROM items WHERE rating > 0 OR color != '' OR tags != '' OR pinned = 1 OR encrypted = 1 OR note != '' OR palettes != ''");
    loadTable("SELECT volume, path, rating, color, tags, pinned, encrypted, note, palettes FROM folders WHERE rating > 0 OR color != '' OR tags != '' OR pinned = 1 OR encrypted = 1 OR note != '' OR palettes != ''");
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_cache = std::move(tempCache);
    }
    emit metaChanged("__RELOAD_ALL__");
}

void MetadataManager::initFromJsonMode() {
    std::unordered_map<std::wstring, RuntimeMeta> tempCache;
    auto frnsMap = AllFrnManager::getAllFrns();
    
    // 2026-05-29 物理加固：在 JSON 模式初始化期间，利用已有索引回填 SQLite items 表 (Plan-45)
    // 理由：彻底解决用户反馈的“模式切换后计数为 0”的问题，实现全自动对账。
    QSqlDatabase db = ArcMeta::Database::instance().getThreadDatabase();
    bool useDb = db.isOpen();
    if (useDb) db.transaction();

    for (auto it = frnsMap.begin(); it != frnsMap.end(); ++it) {
        QString frnStr = it.key();
        QString lastKnownPath = it.value();
        std::wstring resolvedPath = lastKnownPath.toStdWString();
        
        bool ok = false;
        uint64_t frnVal = frnStr.toULongLong(&ok, 16);
        if (ok) {
            for (size_t d = 0; d < 26; ++d) {
                std::wstring p = MftReader::instance().getPathFast(d, frnVal);
                if (!p.empty()) {
                    if (p.find(L".am_meta.json") != std::wstring::npos) {
                        resolvedPath = QDir::toNativeSeparators(QFileInfo(QString::fromStdWString(p)).absolutePath()).toStdWString();
                    } else {
                        resolvedPath = p;
                    }
                    break;
                }
            }
        }
        
        AmMetaJson amJson(resolvedPath);
        if (amJson.load()) {
            std::wstring vol = getVolumeSerialNumber(resolvedPath);
            std::wstring nResolvedPath = normalizePath(resolvedPath);

            // 1. 加载文件夹本身的元数据
            const auto& f = amJson.folder();
            RuntimeMeta fMeta;
            fMeta.rating = f.rating; fMeta.color = f.color;
            for (const auto& t : f.tags) fMeta.tags << QString::fromStdWString(t);
            fMeta.pinned = f.pinned; fMeta.note = f.note; fMeta.encrypted = f.encrypted;
            fMeta.palettes = f.palettes;
            tempCache[nResolvedPath] = std::move(fMeta);

            if (useDb && !f.fileId128.empty()) {
                FolderRepo::save(vol, nResolvedPath, f);
            }
            
            // 2. 加载子项元数据
            for (const auto& [name, item] : amJson.items()) {
                RuntimeMeta iMeta;
                iMeta.rating = item.rating; iMeta.color = item.color;
                for (const auto& t : item.tags) iMeta.tags << QString::fromStdWString(t);
                iMeta.pinned = item.pinned; iMeta.note = item.note; iMeta.encrypted = item.encrypted;
                iMeta.palettes = item.palettes;
                std::wstring itemPath = resolvedPath + L"\\" + name;
                tempCache[normalizePath(itemPath)] = std::move(iMeta);

                if (useDb && !item.fileId128.empty()) {
                    std::wstring parentDir = QDir::toNativeSeparators(QString::fromStdWString(resolvedPath)).toStdWString();
                    ItemRepo::save(parentDir, name, item);
                }
            }
        }
    }
    if (useDb) db.commit();

    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_cache = std::move(tempCache);
    }
    emit metaChanged("__RELOAD_ALL__");
}

RuntimeMeta MetadataManager::getMeta(const std::wstring& path) {
    std::wstring nPath = normalizePath(path);
    
    // 2026-06-xx 物理同步逻辑：优先检查内存缓存（高性能）
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_cache.find(nPath);
        if (it != m_cache.end()) return it->second;
    }

    // 2026-06-xx 核心恢复：若内存未命中，尝试从本地离散 .am_meta.json 加载
    // 理由：实现“目录导航”对本地管理文件的实时感应，即使数据库未同步
    QFileInfo info(QString::fromStdWString(nPath));
    std::wstring parentDir = QDir::toNativeSeparators(info.absolutePath()).toStdWString();
    std::wstring fileName = info.fileName().toStdWString();

    AmMetaJson amJson(parentDir);
    if (amJson.load()) {
        auto& items = amJson.items();
        if (items.count(fileName)) {
            const auto& item = items.at(fileName);
            RuntimeMeta rm;
            rm.rating = item.rating; rm.color = item.color;
            rm.pinned = item.pinned; rm.encrypted = item.encrypted;
            rm.note = item.note;
            rm.palettes = item.palettes;
            for (const auto& t : item.tags) rm.tags << QString::fromStdWString(t);
            
            // 写入缓存并返回
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_cache[nPath] = rm;
            return rm;
        }

        // 如果是文件夹自身
        if (info.isDir()) {
            const auto& folder = amJson.folder();
            if (!folder.isDefault()) {
                RuntimeMeta rm;
                rm.rating = folder.rating; rm.color = folder.color;
                rm.pinned = folder.pinned; rm.note = folder.note;
                rm.palettes = folder.palettes;
                for (const auto& t : folder.tags) rm.tags << QString::fromStdWString(t);
                
                std::unique_lock<std::shared_mutex> lock(m_mutex);
                m_cache[nPath] = rm;
                return rm;
            }
        }
    }

    return RuntimeMeta();
}

void MetadataManager::setRating(const std::wstring& path, int rating) {
    std::wstring nPath = normalizePath(path);
    { std::unique_lock<std::shared_mutex> lock(m_mutex); m_cache[nPath].rating = rating; }
    emit metaChanged(QString::fromStdWString(nPath));
    debouncePersist(nPath);
}

void MetadataManager::setColor(const std::wstring& path, const std::wstring& color) {
    std::wstring nPath = normalizePath(path);
    { std::unique_lock<std::shared_mutex> lock(m_mutex); m_cache[nPath].color = color; }
    emit metaChanged(QString::fromStdWString(nPath));
    debouncePersist(nPath);
}

void MetadataManager::setPinned(const std::wstring& path, bool pinned) {
    std::wstring nPath = normalizePath(path);
    { std::unique_lock<std::shared_mutex> lock(m_mutex); m_cache[nPath].pinned = pinned; }
    emit metaChanged(QString::fromStdWString(nPath));
    debouncePersist(nPath);
}

void MetadataManager::setTags(const std::wstring& path, const QStringList& tags) {
    std::wstring nPath = normalizePath(path);
    { std::unique_lock<std::shared_mutex> lock(m_mutex); m_cache[nPath].tags = tags; }
    emit metaChanged(QString::fromStdWString(nPath));
    debouncePersist(nPath);
}

void MetadataManager::setNote(const std::wstring& path, const std::wstring& note) {
    std::wstring nPath = normalizePath(path);
    { std::unique_lock<std::shared_mutex> lock(m_mutex); m_cache[nPath].note = note; }
    emit metaChanged(QString::fromStdWString(nPath));
    debouncePersist(nPath);
}

void MetadataManager::setEncrypted(const std::wstring& path, bool encrypted) {
    std::wstring nPath = normalizePath(path);
    { std::unique_lock<std::shared_mutex> lock(m_mutex); m_cache[nPath].encrypted = encrypted; }
    emit metaChanged(QString::fromStdWString(nPath));
    debouncePersist(nPath);
}

void MetadataManager::setPalettes(const std::wstring& path, const QVector<QPair<QColor, float>>& palettes) {
    std::wstring nPath = normalizePath(path);
    
    // 1. 同步内存缓存
    std::vector<PaletteEntry> entries;
    for (const auto& p : palettes) {
        entries.push_back({p.first, p.second});
    }
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_cache[nPath].palettes = entries;
    }

    // 2. 物理原子更新 .am_meta.json
    QFileInfo info(QString::fromStdWString(nPath));
    std::wstring parentDir = QDir::toNativeSeparators(info.absolutePath()).toStdWString();
    std::wstring fileName = info.fileName().toStdWString();
    
    AmMetaJson amJson(parentDir);
    if (amJson.load()) {
        if (info.isDir()) {
            amJson.folder().palettes = entries;
        } else {
            amJson.items()[fileName].palettes = entries;
        }
        amJson.save();
    }

    emit metaChanged(QString::fromStdWString(nPath));
    debouncePersist(nPath);
}

QVector<QColor> MetadataManager::getPalettes(const std::wstring& path) {
    std::wstring nPath = normalizePath(path);
    QFileInfo info(QString::fromStdWString(nPath));
    std::wstring parentDir = QDir::toNativeSeparators(info.absolutePath()).toStdWString();
    std::wstring fileName = info.fileName().toStdWString();

    AmMetaJson amJson(parentDir);
    if (amJson.load()) {
        std::vector<PaletteEntry> entries;
        if (info.isDir()) {
            entries = amJson.folder().palettes;
        } else {
            auto& items = amJson.items();
            if (items.count(fileName)) {
                entries = items.at(fileName).palettes;
            }
        }

        QVector<QColor> colors;
        for (const auto& e : entries) colors << e.color;
        return colors;
    }
    return {};
}

void MetadataManager::debouncePersist(const std::wstring& nPath) {
    { std::unique_lock<std::shared_mutex> lock(m_mutex); m_dirtyPaths.insert(nPath); }
    QMetaObject::invokeMethod(m_batchTimer, "start", Qt::QueuedConnection);
}

void MetadataManager::renameItem(const std::wstring& oldPath, const std::wstring& newPath) {
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_cache.find(oldPath);
        if (it != m_cache.end()) {
            m_cache[newPath] = std::move(it->second);
            m_cache.erase(it);
        }
    }
    emit metaChanged(QString::fromStdWString(newPath));
}

void MetadataManager::removeMetadataSync(const std::wstring& path) {
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        // 1. 递归清理内存缓存
        for (auto it = m_cache.begin(); it != m_cache.end(); ) {
            if (it->first == path || it->first.find(path + L"\\") == 0 || it->first.find(path + L"/") == 0) {
                it = m_cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2. 清理离散 JSON 持久化
    QFileInfo info(QString::fromStdWString(path));
    if (info.isDir()) {
        // 若是文件夹，整个单元删除，包括其内部的 .am_meta.json
        QString metaPath = info.absoluteFilePath() + "/.am_meta.json";
        QFile::remove(metaPath);
    } else {
        // 若是文件，从所在目录的 .am_meta.json 中移除条目
        std::wstring dir = info.absolutePath().toStdWString();
        AmMetaJson json(dir);
        json.remove(info.fileName().toStdWString());
        json.save();
    }
}

std::wstring MetadataManager::getVolumeSerialNumber(const std::wstring& path) {
    if (path.length() < 2 || path[1] != L':') return L"UNKNOWN";
    wchar_t root[4] = { path[0], L':', L'\\', L'\0' };
    DWORD serial = 0;
    if (GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        wchar_t buf[16]; swprintf(buf, 16, L"%08X", serial); return buf;
    }
    return L"UNKNOWN";
}

bool MetadataManager::fetchWinApiMetadataDirect(const std::wstring& path, std::string& outId128, std::wstring* outFrn, long long* outSize, std::wstring* outType, long long* outCtime, long long* outMtime, long long* outAtime) {
    // 2026-06-15 物理重构：采用 0 访问权限打开，最大限度避免共享违规。
    HANDLE hFile = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    
    std::wstring vol = getVolumeSerialNumber(path);
    std::wstring frnStr;

    if (hFile == INVALID_HANDLE_VALUE) {
        // 核心补丁：若因锁定无法获取句柄，回退到基于路径的 SHA-256 (PATHURL 前缀)
        if (outFrn) *outFrn = generateDeterministicFrn(path);
        outId128 = generateDeterministicSha256Id(path);
        return false; // 由于未获取到物理身份，返回 false 以引导调用方判定
    }

    bool gotPhysicalId = false;
    // 1. 暂时跳过复杂的物理ID获取，使用路径哈希
    // TODO: 后续修复Windows API兼容性问题
    gotPhysicalId = false;

    // 2. 获取 FRN 及基础属性
    BY_HANDLE_FILE_INFORMATION basicInfo;
    bool gotBasicInfo = false;
    if (GetFileInformationByHandle(hFile, &basicInfo)) {
        wchar_t frnBuf[17];
        unsigned long long fullFrn = (static_cast<unsigned long long>(basicInfo.nFileIndexHigh) << 32) | basicInfo.nFileIndexLow;
        swprintf(frnBuf, 17, L"%016llX", fullFrn);
        frnStr = frnBuf;
        if (outFrn) *outFrn = frnStr;

        // Fallback 逻辑：如果不支持 128-bit ID，回退到 FRN 物理标识 (Memories.md 铁律)
        if (!gotPhysicalId) {
            outId128 = generateFallbackFid(vol, frnStr);
        }

        if (outSize) *outSize = (static_cast<long long>(basicInfo.nFileSizeHigh) << 32) | basicInfo.nFileSizeLow;
        if (outType) *outType = (basicInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L"folder" : L"file";
        auto toMS = [](const FILETIME& ft) {
            ULARGE_INTEGER ull; ull.LowPart = ft.dwLowDateTime; ull.HighPart = ft.dwHighDateTime;
            return (long long)((ull.QuadPart - 116444736000000000ULL) / 10000ULL);
        };
        if (outCtime) *outCtime = toMS(basicInfo.ftCreationTime);
        if (outMtime) *outMtime = toMS(basicInfo.ftLastWriteTime);
        if (outAtime) *outAtime = toMS(basicInfo.ftLastAccessTime);
        gotBasicInfo = true;
    } else {
        if (outFrn) *outFrn = generateDeterministicFrn(path);
        // 如果连基础属性都拿不到，且没拿到物理 ID，降级至路径哈希
        if (!gotPhysicalId) outId128 = generateDeterministicSha256Id(path);
    }

    CloseHandle(hFile);
    // 返回语义：是否获取到了至少一种形式的稳定物理标识（128位ID 或 FRN）
    return gotPhysicalId || gotBasicInfo;
}

void MetadataManager::syncPhysicalMetadata(const std::wstring& path) {
    persistAsync(path);
}

std::string MetadataManager::getFileIdSync(const std::wstring& path) {
    std::string fid;
    // 2026-06-15 按照审计建议：修正覆盖逻辑。只有当物理获取彻底失败且 fid 为空时才降级哈希。
    if (!fetchWinApiMetadataDirect(path, fid, nullptr) && fid.empty()) {
        fid = generateDeterministicSha256Id(path);
    }
    return fid;
}

void MetadataManager::persistAsync(const std::wstring& path) {
    // 2026-06-16 按照用户授权：物理侧挂 JSON 先行 -> FID 日志追踪 -> 同步对账。
    std::wstring nPath = normalizePath(path);
    QFileInfo info(QString::fromStdWString(nPath));
    std::wstring parentDir = QDir::toNativeSeparators(info.absolutePath()).toStdWString();
    std::wstring fileName = info.fileName().toStdWString();
    std::wstring vol = getVolumeSerialNumber(nPath);

    RuntimeMeta rMeta = getMeta(nPath);

    // 1. 同步到内存与数据库（在 JSON 模式下跳过对数据库的保存）
    if (!CategoryRepo::isJsonMode()) {
        if (info.isDir()) {
            FolderMeta fMeta;
            FolderRepo::get(vol, nPath, fMeta);
            fetchWinApiMetadataDirect(nPath, fMeta.fileId128, nullptr);
            fMeta.rating = rMeta.rating; fMeta.color = rMeta.color;
            fMeta.pinned = rMeta.pinned; fMeta.note = rMeta.note;
            fMeta.tags.clear();
            for (const auto& t : rMeta.tags) fMeta.tags.push_back(t.toStdWString());
            fMeta.palettes = rMeta.palettes;
            FolderRepo::save(vol, nPath, fMeta);
        } else {
            ItemMeta iMeta;
            iMeta.volume = vol;
            fetchWinApiMetadataDirect(nPath, iMeta.fileId128, &iMeta.frn, &iMeta.size, &iMeta.type, &iMeta.creationTime, &iMeta.modificationTime, &iMeta.accessTime);
            iMeta.rating = rMeta.rating; iMeta.color = rMeta.color;
            iMeta.pinned = rMeta.pinned; iMeta.note = rMeta.note;
            iMeta.encrypted = rMeta.encrypted;
            iMeta.tags.clear();
            for (const auto& t : rMeta.tags) iMeta.tags.push_back(t.toStdWString());
            iMeta.palettes = rMeta.palettes;
            ItemRepo::save(parentDir, fileName, iMeta);
        }
    }

    // 2. 物理落地：写入 .am_meta.json (物理侧真值先行)
    // 2026-06-xx 架构修正：判断是否为磁盘根目录
    if (info.isDir() && info.isRoot()) {
        // 磁盘根目录元数据应持久化到程序根目录下的 FERREX_drivers.json，防止权限冲突或物理损坏
        QString driversPath = qApp->applicationDirPath() + "/FERREX_drivers.json";
        QFile file(driversPath);
        QJsonObject root;
        if (file.open(QIODevice::ReadOnly)) {
            root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
        }

        QJsonObject driveMeta;
        driveMeta["rating"] = rMeta.rating;
        driveMeta["color"] = QString::fromStdWString(rMeta.color);
        driveMeta["pinned"] = rMeta.pinned;
        driveMeta["note"] = QString::fromStdWString(rMeta.note);
        QJsonArray tagsArr;
        for (const auto& t : rMeta.tags) tagsArr.append(t);
        driveMeta["tags"] = tagsArr;
        
        root[QString::fromStdWString(nPath)] = driveMeta;

        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(root).toJson());
            file.close();
        }
    } else {
        AmMetaJson amJson(parentDir);
        amJson.load();
        if (info.isDir()) {
            FolderMeta& folder = amJson.folder();
            folder.rating = rMeta.rating; folder.color = rMeta.color;
            folder.pinned = rMeta.pinned; folder.note = rMeta.note;
            folder.tags.clear();
            for (const auto& t : rMeta.tags) folder.tags.push_back(t.toStdWString());
            folder.palettes = rMeta.palettes;
        } else {
            ItemMeta& item = amJson.items()[fileName];
            item.rating = rMeta.rating; item.color = rMeta.color;
            item.pinned = rMeta.pinned; item.encrypted = rMeta.encrypted;
            item.note = rMeta.note;
            item.tags.clear();
            for (const auto& t : rMeta.tags) item.tags.push_back(t.toStdWString());
            item.palettes = rMeta.palettes;
        }
        amJson.save();
    }

    // 2.5 提取该 .am_meta.json 文件本身的物理 FRN，安全、自动登记到根目录 All_FRN_am_meta.json 中
    // 按照用户 2026-05-28 最新逻辑：对账锚点应为 .am_meta.json 文件的 FRN
    std::wstring metaPath = parentDir + L"\\.am_meta.json";
    std::wstring fileFrn;
    std::string fileFid;
    if (fetchWinApiMetadataDirect(metaPath, fileFid, &fileFrn)) {
        AllFrnManager::registerFrn(fileFrn, parentDir);
    }

    // 3. 提取侧挂 .am_meta.json 的物理 FID 并记入日志
    std::wstring amJsonPath = parentDir + L"\\.am_meta.json";
    std::string metaFid;
    if (fetchWinApiMetadataDirect(amJsonPath, metaFid, nullptr)) {
        addToSyncLog(QString::fromStdString(metaFid).toStdWString());
    } else {
        // 退而求其次，若没拿到 FID 则记录目录路径（兼容性处理）
        addToSyncLog(parentDir);
    }

    emit metaChanged(QString::fromStdWString(nPath));
}

bool MetadataManager::hasPendingSync() const {
    QString logPath = qApp->applicationDirPath() + "/Synchronize.json";
    return QFile::exists(logPath);
}

QStringList MetadataManager::getPendingSyncDirs() {
    QString logPath = qApp->applicationDirPath() + "/Synchronize.json";
    QFile file(logPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isArray()) return doc.toVariant().toStringList();
    return {};
}

void MetadataManager::removeFidsFromLog(const QStringList& fidsToRemove) {
    if (fidsToRemove.isEmpty()) return;
    
    QString logPath = qApp->applicationDirPath() + "/Synchronize.json";
    if (!QFile::exists(logPath)) return;

    QStringList current;
    {
        QFile file(logPath);
        if (file.open(QIODevice::ReadOnly)) {
            current = QJsonDocument::fromJson(file.readAll()).toVariant().toStringList();
        }
    }

    bool changed = false;
    for (const auto& f : fidsToRemove) {
        if (current.contains(f)) {
            current.removeAll(f);
            changed = true;
        }
    }

    if (changed) {
        if (current.isEmpty()) {
            QFile::remove(logPath);
            emit pendingSyncChanged(false);
        } else {
            QString tmpPath = logPath + ".tmp";
            QFile tmpFile(tmpPath);
            if (tmpFile.open(QIODevice::WriteOnly)) {
                tmpFile.write(QJsonDocument(QJsonArray::fromStringList(current)).toJson());
                tmpFile.close();
                MoveFileExW(tmpPath.toStdWString().c_str(), logPath.toStdWString().c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
    }
}

void MetadataManager::addToSyncLog(const std::wstring& dirPath) {
    QString path = QString::fromStdWString(dirPath);
    QString logPath = qApp->applicationDirPath() + "/Synchronize.json";

    QStringList currentDirs;
    if (QFile::exists(logPath)) {
        QFile file(logPath);
        if (file.open(QIODevice::ReadOnly)) {
            currentDirs = QJsonDocument::fromJson(file.readAll()).toVariant().toStringList();
        }
    }

    if (!currentDirs.contains(path)) {
        currentDirs << path;
        QJsonArray arr = QJsonArray::fromStringList(currentDirs);
        
        // 原子写逻辑
        QString tmpPath = logPath + ".tmp";
        QFile tmpFile(tmpPath);
        if (tmpFile.open(QIODevice::WriteOnly)) {
            tmpFile.write(QJsonDocument(arr).toJson());
            tmpFile.close();
            
            if (MoveFileExW(tmpPath.toStdWString().c_str(), logPath.toStdWString().c_str(), MOVEFILE_REPLACE_EXISTING)) {
                emit pendingSyncChanged(true);
            }
        }
    }
}

void MetadataManager::saveSyncLog() {
    // 逻辑已在 addToSyncLog 中原子化实现
}

QStringList MetadataManager::searchInCache(const QString& keyword) {
    QStringList results;
    if (keyword.isEmpty()) return results;

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
        const std::wstring& path = it->first;
        const RuntimeMeta& meta = it->second;

        QString qPath = QString::fromStdWString(path);
        QString qNote = QString::fromStdWString(meta.note);
        
        bool match = qPath.contains(keyword, Qt::CaseInsensitive) ||
                     qNote.contains(keyword, Qt::CaseInsensitive);
        
        if (!match) {
            for (const QString& tag : meta.tags) {
                if (tag.contains(keyword, Qt::CaseInsensitive)) {
                    match = true;
                    break;
                }
            }
        }

        if (match) {
            results << qPath;
        }
    }
    return results;
}

} // namespace ArcMeta
