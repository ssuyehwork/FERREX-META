#include "AutoImportManager.h"
#include "../mft/MftReader.h"
#include "../meta/MetadataManager.h"
#include "../meta/DatabaseManager.h"
#include "../meta/CategoryRepo.h"
#include "AppConfig.h"
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QFileInfo>
#include <QFile>
#include <QTimer>
#include <QtConcurrent>
#include <QFuture>
#include <QMessageBox> // 引入弹出框支持
#include <functional>
#include <cwchar>
#include <map>
#include <cstdint>
#include <atomic>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ArcMeta {

static std::recursive_mutex s_dbAccessMutex;

// 线程安全的全局调试弹窗助手：将子线程的请求通过队列化事件投递至 GUI 主线程
static void safeShowMessageBox(const QString& title, const QString& text) {
    QMetaObject::invokeMethod(QCoreApplication::instance(), [title, text]() {
        QMessageBox::information(nullptr, title, text);
    }, Qt::QueuedConnection);
}

AutoImportManager& AutoImportManager::instance() {
    static AutoImportManager inst;
    return inst;
}

AutoImportManager::AutoImportManager(QObject* parent) : QObject(parent) {
    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setInterval(3000); 
    m_debounceTimer->setSingleShot(true);
    connect(m_debounceTimer, &QTimer::timeout, this, &AutoImportManager::processImportQueue);
}

AutoImportManager::~AutoImportManager() {
    stopListening();
}

void AutoImportManager::startListening() {
    if (m_isListening) return;

    // [Plan-131 方案 B] 预热 FRN 缓存
    m_managedFrnCache.clear();
    const auto drives = QDir::drives();
    QString debugPrewarmLog = "==== 托管库预热缓存调试器 ====\n";

    for (const QFileInfo& d : drives) {
        std::wstring volSerial = MetadataManager::getVolumeSerialNumber(d.absolutePath().toStdWString());
        std::wstring managedPath = MetadataManager::getManagedLibraryPath(volSerial, d.absolutePath());
        debugPrewarmLog += QString("盘符: %1 | 托管库物理路径: %2\n")
            .arg(d.absolutePath())
            .arg(QString::fromStdWString(managedPath));

        if (!managedPath.empty()) {
            std::string fid; std::wstring frnStr;
            if (MetadataManager::fetchWinApiMetadataDirect(managedPath, fid, &frnStr)) {
                try {
                    uint64_t frn = std::stoull(frnStr, nullptr, 16);
                    m_managedFrnCache.insert(frn);
                    debugPrewarmLog += QString("  ↳ 成功获取 FRN: %1 (已加入内存缓存)\n").arg(QString::fromStdWString(frnStr));
                    qDebug() << "[AutoImport] [Plan-131] 已缓存托管库根 FRN:" << QString::fromStdWString(frnStr) << "->" << QString::fromStdWString(managedPath);
                } catch (...) {
                    debugPrewarmLog += QString("  ↳ 异常：解析 FRN 字符串失败 (%1)\n").arg(QString::fromStdWString(frnStr));
                    qWarning() << "[AutoImport] 解析托管库根 FRN 失败:" << QString::fromStdWString(frnStr);
                }
            } else {
                debugPrewarmLog += "  ↳ 异常：fetchWinApiMetadataDirect 获取物理信息失败\n";
            }
        } else {
            debugPrewarmLog += "  ↳ 异常：未检测到有效托管路径配置\n";
        }
    }

    debugPrewarmLog += QString("\n缓存加载结束，当前已缓存 FRN 总量: %1").arg(m_managedFrnCache.size());
    // 弹出预热调试结果，排查是否由于获取不到托管库根节点 FRN 导致一切事件被过滤
    safeShowMessageBox("调试：托管库预热探测器", debugPrewarmLog);

    connect(&MftReader::instance(), &MftReader::entryAdded, this, &AutoImportManager::onEntryAdded, Qt::QueuedConnection);
    connect(&MftReader::instance(), &MftReader::entryUpdated, this, &AutoImportManager::onEntryUpdated, Qt::QueuedConnection);
    connect(&MftReader::instance(), &MftReader::entryRemoved, this, &AutoImportManager::onEntryRemoved, Qt::QueuedConnection);
    m_isListening = true;
}

void AutoImportManager::stopListening() {
    if (!m_isListening) return;
    disconnect(&MftReader::instance(), &MftReader::entryAdded, this, &AutoImportManager::onEntryAdded);
    disconnect(&MftReader::instance(), &MftReader::entryUpdated, this, &AutoImportManager::onEntryUpdated);
    disconnect(&MftReader::instance(), &MftReader::entryRemoved, this, &AutoImportManager::onEntryRemoved);
    m_isListening = false;
}

void AutoImportManager::syncAllManagedLibraries() {
    const auto drives = QDir::drives();
    bool changed = false;
    for (const QFileInfo& d : drives) {
        QString drive = d.absolutePath();
        QString letter = drive.left(1).toUpper();
        
        QDir rootDir(drive);
        QStringList entries = rootDir.entryList({"ArcMeta.Library_*"}, QDir::Dirs | QDir::Hidden);
        
        QString targetName = "ArcMeta.Library_" + letter;
        for (const QString& entry : entries) {
            if (QString::compare(entry, targetName, Qt::CaseInsensitive) == 0) {
                QString managedPath = rootDir.absoluteFilePath(entry);
                qDebug() << "[AutoImport] 启动对账：发现物理托管库，执行同步 ->" << managedPath;
                (void)QtConcurrent::run([this, managedPath]() {
                    handleRecursiveIngestion(QDir::toNativeSeparators(managedPath).toStdWString());
                });
                changed = true;
            }
        }
    }
    if (changed) {
        MetadataManager::instance().notifyFullUIRebuild();
    }
}

void AutoImportManager::onEntryAdded(uint64_t key) {
    (void)QtConcurrent::run([this, key]() {
        // 2026-08-xx 按照 Plan-126：USN 高效过滤 (FRN 链判定)
        if (!isUnderManagedLibrary(key)) return;

        std::lock_guard<std::recursive_mutex> dbLock(s_dbAccessMutex);
        int idx = MftReader::instance().getIndexByKey(key);
        if (idx < 0) return;

        QString qPath = MftReader::instance().getFullPath(idx);
        std::wstring fullPath = qPath.toStdWString();
        uint64_t frn = MftReader::instance().getFrn(idx);

        if (MftReader::instance().isDirectory(idx)) {
            // 1:1 镜像同步：创建逻辑分类
            int existingCat = CategoryRepo::findByFrn(frn);
            if (existingCat == 0) {
                QString fileName = QFileInfo(qPath).fileName();
                std::wstring parentPath = QFileInfo(qPath).absolutePath().toStdWString();
                std::string parentFid;
                std::wstring pFrnStr;
                int parentCatId = 0;
                if (MetadataManager::fetchWinApiMetadataDirect(parentPath, parentFid, &pFrnStr)) {
                    uint64_t pFrn = std::stoull(pFrnStr, nullptr, 16);
                    parentCatId = CategoryRepo::findByFrn(pFrn);
                }

                Category cat;
                cat.parentId = parentCatId;
                cat.name = fileName.toStdWString();
                cat.physicalFrn = frn;
                cat.physicalPath = fullPath;
                cat.color = CategoryRepo::getDefaultColor();
                CategoryRepo::add(cat);
                MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
            }
        } else {
            // 职责收拢：USN 驱动入库
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_pendingPaths.push_back(fullPath);
            }
            QMetaObject::invokeMethod(m_debounceTimer, "start", Qt::QueuedConnection);
        }
    });
}

void AutoImportManager::onEntryUpdated(uint64_t key) {
    (void)QtConcurrent::run([this, key]() {
        // 2026-08-xx 按照 Plan-128：操作溯源判定
        bool isInternal = MetadataManager::instance().isInternalOperating();
        bool isUnderLibrary = isUnderManagedLibrary(key);

        if (!isUnderLibrary) {
            // [信号审计]：项移出了托管库
            int idx = MftReader::instance().getIndexByKey(key);
            if (idx >= 0) {
                uint64_t frn = MftReader::instance().getFrn(idx);
                size_t dIdx = static_cast<size_t>(key >> 48);
                auto drives = MftReader::instance().getDriveList();
                
                if (dIdx < drives.size()) {
                    std::wstring volSerial = MetadataManager::getVolumeSerialNumber(drives[dIdx]);
                    wchar_t frnBuf[17]; swprintf(frnBuf, 17, L"%016llX", frn);
                    std::string fid = MetadataManager::generateFallbackFid(volSerial, frnBuf);
                    std::wstring oldPath = MetadataManager::instance().getPathByFid(fid);

                    // 物理红线：必须确保该项此前在托管库内（即存在元数据记录）才执行后续逻辑
                    if (!oldPath.empty() && MetadataManager::isInsideManagedLibrary(oldPath)) {
                        if (!isInternal) {
                            // 第三方移动出库：标记失效
                            if (MftReader::instance().isDirectory(idx)) {
                                MetadataManager::instance().setInvalidRecursive(oldPath, true);
                            } else {
                                MetadataManager::instance().setInvalid(oldPath, true);
                            }
                        } else {
                            // 2026-08-xx 按照 Plan-128：内部操作移出托管库 -> 执行硬删除
                            qDebug() << "[AutoImport] 内部操作移出托管库：执行硬删除 ->" << QString::fromStdWString(oldPath);
                            MetadataManager::instance().removeMetadataSync(oldPath);
                        }
                    }
                }
            }
            return;
        }

        std::lock_guard<std::recursive_mutex> dbLock(s_dbAccessMutex);
        int idx = MftReader::instance().getIndexByKey(key);
        if (idx < 0) return;

        QString qPath = MftReader::instance().getFullPath(idx);
        std::wstring fullPath = qPath.toStdWString();
        uint64_t frn = MftReader::instance().getFrn(idx);

        if (MftReader::instance().isDirectory(idx)) {
            // 1:1 镜像同步：重命名或位移
            int catId = CategoryRepo::findByFrn(frn);
            if (catId > 0) {
                Category cat = CategoryRepo::getById(catId);
                QString newName = QFileInfo(qPath).fileName();
                
                // 物理父目录 FRN 校验
                std::wstring parentPath = QFileInfo(qPath).absolutePath().toStdWString();
                std::string pfid; std::wstring pfrnStr;
                int newParentId = 0;
                if (MetadataManager::fetchWinApiMetadataDirect(parentPath, pfid, &pfrnStr)) {
                    newParentId = CategoryRepo::findByFrn(std::stoull(pfrnStr, nullptr, 16));
                }

                if (QString::fromStdWString(cat.name) != newName || cat.parentId != newParentId) {
                    qDebug() << "[Mirror] 物理同步逻辑分类 ->" << newName << "Parent:" << newParentId;
                    cat.name = newName.toStdWString();
                    cat.parentId = newParentId;
                    cat.physicalPath = fullPath;
                    CategoryRepo::update(cat);
                    MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
                }
            } else {
                // [Plan-130] 补全实时入库分流：处理首次从库外移入的文件夹
                qDebug() << "[AutoImport] 检测到新文件夹移入库内，触发递归入库 ->" << qPath;
                handleRecursiveIngestion(fullPath);
            }
        } else {
            // 文件更新：重新注册元数据
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_pendingPaths.push_back(fullPath);
            }
            QMetaObject::invokeMethod(m_debounceTimer, "start", Qt::QueuedConnection);
        }
    });
}

void AutoImportManager::onEntryRemoved(uint64_t key) {
    (void)QtConcurrent::run([this, key]() {
        // 2026-08-xx 按照 Plan-128：操作溯源判定
        bool isInternal = MetadataManager::instance().isInternalOperating();

        // 由于 MftReader 已经删除了索引，无法通过 MftReader 获取路径，需直接操作数据库。
        // key 的低 48 位即为 FRN
        uint64_t frn = key & 0x0000FFFFFFFFFFFFull;
        
        std::lock_guard<std::recursive_mutex> dbLock(s_dbAccessMutex);
        
        // 1. 检查是否为镜像分类
        int catId = CategoryRepo::findByFrn(frn);
        if (catId > 0) {
            if (isInternal) {
                qDebug() << "[Mirror] 内部删除：同步移除镜像分类 ID:" << catId;
                CategoryRepo::remove(catId);
            } else {
                qDebug() << "[Mirror] 第三方删除：递归标记分类下项目失效";
                // 获取分类对应的物理路径并执行递归失效
                Category cat = CategoryRepo::getById(catId);
                if (!cat.physicalPath.empty()) {
                    MetadataManager::instance().setInvalidRecursive(cat.physicalPath, true);
                }
                // 注意：第三方删除时，分类本身也应被逻辑隐藏或标记
                CategoryRepo::remove(catId); 
            }
            MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
        }

        // 2. 文件项审计
        // 2026-08-xx 按照 Plan-128：物理删除审计。由于 MFT 记录已消失，通过所有在线卷的 FID 缓存找回路径。
        auto drives = MftReader::instance().getDriveList();
        for (const auto& volPath : drives) {
            std::wstring volSerial = MetadataManager::getVolumeSerialNumber(volPath);
            
            wchar_t frnBuf[17];
            swprintf(frnBuf, 17, L"%016llX", frn);
            std::string fid = MetadataManager::generateFallbackFid(volSerial, frnBuf);

            std::wstring path = MetadataManager::instance().getPathByFid(fid);
            if (!path.empty()) {
                // 物理红线：仅针对原先位于托管库内的项执行审计分流
                if (MetadataManager::isInsideManagedLibrary(path)) {
                    if (isInternal) {
                        qDebug() << "[AutoImport] 内部操作删除：执行硬删除 ->" << QString::fromStdWString(path);
                        MetadataManager::instance().removeMetadataSync(path);
                    } else {
                        qDebug() << "[AutoImport] 第三方外部删除：标记失效 ->" << QString::fromStdWString(path);
                        MetadataManager::instance().setInvalid(path, true);
                    }
                }
            }
        }
        MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
    });
}

void AutoImportManager::recordRecentVisitedFolder(const std::wstring& path) {
    if (path.empty()) return;
    std::wstring managedFolder;
    if (instance().checkAndGetManagedPath(path, managedFolder)) return;

    std::wstring volSerial = MetadataManager::getVolumeSerialNumber(path);
    if (volSerial.empty()) return;

    QString key = QString("RecentVisited/Volume_%1").arg(QString::fromStdWString(volSerial));
    QStringList list = AppConfig::instance().getValue(key, QStringList()).toStringList();

    QString qPath = QString::fromStdWString(MetadataManager::normalizePath(path));
    list.removeAll(qPath);
    list.prepend(qPath);
    while (list.size() > 14) list.removeLast();

    AppConfig::instance().setValue(key, list);
}

QStringList AutoImportManager::getRecentVisitedFolders(const std::wstring& volSerial) {
    if (volSerial.empty()) return QStringList();
    QString key = QString("RecentVisited/Volume_%1").arg(QString::fromStdWString(volSerial));
    return AppConfig::instance().getValue(key, QStringList()).toStringList();
}

bool AutoImportManager::checkAndGetManagedPath(const std::wstring& path, std::wstring& outManagedFolder) {
    std::wstring managedAbs = getManagedLibraryPath(path);
    if (managedAbs.empty()) return false;

    if (path.size() >= managedAbs.size() && _wcsnicmp(path.c_str(), managedAbs.c_str(), managedAbs.size()) == 0) {
        outManagedFolder = managedAbs;
        return true;
    }
    return false;
}

std::wstring AutoImportManager::getManagedLibraryPath(const std::wstring& pathOrVolSerial) {
    if (pathOrVolSerial.empty()) return L"";

    std::wstring volSerial = pathOrVolSerial;
    if (volSerial.find(L":") != std::wstring::npos || volSerial.find(L"\\") != std::wstring::npos) {
        volSerial = MetadataManager::getVolumeSerialNumber(pathOrVolSerial);
    }
    if (volSerial.empty() || volSerial == L"UNKNOWN") return L"";

    QString drive;
    const auto drives = QDir::drives();
    for (const QFileInfo& d : drives) {
        if (MetadataManager::getVolumeSerialNumber(d.absolutePath().toStdWString()) == volSerial) {
            drive = d.absolutePath();
            break;
        }
    }
    if (drive.isEmpty()) return L"";

    QString key = QString("ManagedFolder/Volume_%1").arg(QString::fromStdWString(volSerial));
    QString relPath = AppConfig::instance().getValue(key, "").toString();

    if (relPath.isEmpty()) {
        relPath = "ArcMeta.Library_" + drive.left(1).toUpper();
        bool exists = QDir(drive + relPath).exists(); 
        if (!exists) return L"";
    }

    std::wstring result = MetadataManager::normalizePath((drive.toStdWString() + relPath.toStdWString()));
    return result;
}

void AutoImportManager::processImportQueue() {
    std::vector<std::wstring> pathsToProcess;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        pathsToProcess = std::move(m_pendingPaths);
        m_pendingPaths.clear();
    }

    if (pathsToProcess.empty()) return;

    // 弹窗告知：开始执行入库了
    safeShowMessageBox("调试：USN 批量入库触发器", 
        QString("防抖定时器超时，正在将以下 %1 个文件送入数据库处理...\n例如第一个路径:\n%2")
        .arg(pathsToProcess.size())
        .arg(QString::fromStdWString(pathsToProcess.front())));

    // 2026-08-xx 异步化改造：将耗时的 registerItem 循环移入后台线程
    (void)QtConcurrent::run([this, pathsToProcess]() {
        std::lock_guard<std::recursive_mutex> dbLock(s_dbAccessMutex);
        MetadataManager::instance().setInternalOperating(true);

        std::map<std::wstring, std::vector<std::wstring>> pathsByVol;
        for (const auto& p : pathsToProcess) {
            pathsByVol[MetadataManager::getVolumeSerialNumber(p)].push_back(p);
        }

        for (auto& pair : pathsByVol) {
            const std::wstring& vol = pair.first;
            if (vol.empty()) continue;

            QString letter = "";
            if (!pair.second.empty()) {
                const std::wstring& firstPath = pair.second.front();
                if (firstPath.length() >= 2 && firstPath[1] == L':') {
                    letter = QString::fromWCharArray(&firstPath[0], 1);
                }
            }

            // 此处可能涉及数据库加载/重命名，需在锁保护下执行
            DatabaseManager::instance().getMemoryDb(vol, letter);

            for (const auto& path : pair.second) {
                // registerItem 内部包含图像元数据提取，是 CPU 密集型操作
                MetadataManager::instance().registerItem(path, true);
            }
        }

        MetadataManager::instance().setInternalOperating(false);
        MetadataManager::instance().notifyFullUIRebuild();
    });
}

bool AutoImportManager::isUnderManagedLibrary(uint64_t key) {
    // [Plan-131 方案 B]：基于 FRN 链的高效托管路径过滤 (内存级 $O(log N)$ 比对)
    uint64_t currentFrn = key & 0x0000FFFFFFFFFFFFull;
    int driveIdx = static_cast<int>(key >> 48);

    QString debugTraceLog = QString("==== [调试] 正在溯源项是否在托管库中 ====\n复合键 (Key): %1\n项 FRN: %2\n盘符索引 (DriveIdx): %3\n\n")
        .arg(key)
        .arg(currentFrn)
        .arg(driveIdx);

    // 向上溯源 FRN 链
    uint64_t frn = currentFrn;
    bool isMatched = false;
    int depth = 0;

    while (frn != 0 && depth < 30) {
        bool inCache = m_managedFrnCache.count(frn);
        debugTraceLog += QString("[%1] 当前节点 FRN: %2 -> 预热缓存命中: %3\n")
            .arg(depth)
            .arg(frn)
            .arg(inCache ? "【成功匹配】" : "不匹配");

        if (inCache) {
            isMatched = true;
            break;
        }
        
        // 获取父级 FRN (通过 MftReader 内存索引)
        uint64_t pFrn = MftReader::instance().getParentFrnByFrn(frn, driveIdx);
        debugTraceLog += QString("   ↳ 获取到父 FRN: %1\n").arg(pFrn);

        if (pFrn == 0 || pFrn == frn) {
            debugTraceLog += "   ↳ 父节点为 0 或追溯循环，中断链路\n";
            break;
        }
        frn = pFrn;
        depth++;
    }

    debugTraceLog += QString("\n判定结果: %1").arg(isMatched ? "允许通行 (进入队列并入库)" : "拦截 (丢弃，不执行入库)");

    // 打印到输出控制台
    qDebug().noquote() << debugTraceLog;

    // 限制只弹出前 5 次弹窗，防止文件变动过多时崩溃卡死
    static std::atomic<int> popupCount{0};
    if (popupCount.load() < 5) {
        popupCount.fetch_add(1);
        safeShowMessageBox("调试：USN 路径溯源追踪器", debugTraceLog);
    }

    return isMatched;
}

void AutoImportManager::handleRecursiveIngestion(const std::wstring& rootPath) {
    QDir dir(QString::fromStdWString(rootPath));
    if (!dir.exists()) return;

    // 2026-08-xx 异步化改造：整机加锁保护数据库写入，并迁移信号抑制逻辑
    std::lock_guard<std::recursive_mutex> dbLock(s_dbAccessMutex);

    MetadataManager::instance().setInternalOperating(true);
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    
    {
        SqlTransaction trans(db);

        int rootCatId = 0;
        std::string rootFid;
        std::wstring rootFrnStr;
        if (MetadataManager::fetchWinApiMetadataDirect(rootPath, rootFid, &rootFrnStr)) {
            try {
                uint64_t frn = std::stoull(rootFrnStr, nullptr, 16);
                rootCatId = CategoryRepo::findByFrn(frn);
                if (rootCatId == 0) {
                    QFileInfo info(QString::fromStdWString(rootPath));
                    std::wstring parentPath = info.absolutePath().toStdWString();
                    std::string parentFid;
                    std::wstring parentFrnStr;
                    int parentCatId = 0;
                    if (MetadataManager::fetchWinApiMetadataDirect(parentPath, parentFid, &parentFrnStr)) {
                        uint64_t pFrn = std::stoull(parentFrnStr, nullptr, 16);
                        parentCatId = CategoryRepo::findByFrn(pFrn);
                    }

                    Category cat;
                    // 2026-08-xx 物理同步：ArcMeta.Library_* 强制作为顶级分类 (parentId = 0)
                    if (info.fileName().startsWith("ArcMeta.Library_", Qt::CaseInsensitive)) {
                        cat.parentId = 0;
                    } else {
                        cat.parentId = parentCatId;
                    }
                    cat.name = info.fileName().toStdWString();
                    cat.physicalFrn = frn;
                    cat.physicalPath = rootPath;
                    cat.color = CategoryRepo::getDefaultColor();
                    if (CategoryRepo::add(cat)) {
                        rootCatId = cat.id;
                    }
                }
            } catch (...) {}
        }

        if (rootCatId > 0) {
            std::function<void(const QString&, int)> syncDir;
            syncDir = [&](const QString& currentPath, int parentCatId) {
                QDir currentDir(currentPath);
                QFileInfoList list = currentDir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);

                for (const QFileInfo& fi : list) {
                    std::wstring wPath = QDir::toNativeSeparators(fi.absoluteFilePath()).toStdWString();
                    if (fi.isDir()) {
                        int existingId = CategoryRepo::findCategoryId(parentCatId, fi.fileName().toStdWString());
                        if (existingId == 0) {
                            std::string fid;
                            std::wstring frnStr;
                            if (MetadataManager::fetchWinApiMetadataDirect(wPath, fid, &frnStr)) {
                                try {
                                    Category cat;
                                    cat.parentId = parentCatId;
                                    cat.name = fi.fileName().toStdWString();
                                    cat.physicalFrn = std::stoull(frnStr, nullptr, 16);
                                    cat.physicalPath = wPath;
                                    cat.color = CategoryRepo::getDefaultColor();
                                    if (CategoryRepo::add(cat)) {
                                        existingId = cat.id;
                                    }
                                } catch (...) {}
                            }
                        }
                        if (existingId > 0) {
                            syncDir(fi.absoluteFilePath(), existingId);
                        }
                    } else {
                        MetadataManager::instance().registerItem(wPath, true);
                        if (parentCatId > 0) {
                            std::string fid;
                            if (MetadataManager::fetchWinApiMetadataDirect(wPath, fid)) {
                                CategoryRepo::addItemToCategory(parentCatId, fid, wPath);
                            }
                        }
                    }
                }
            };

            syncDir(QString::fromStdWString(rootPath), rootCatId);
        }
        
        trans.commit();
    }

    MetadataManager::instance().setInternalOperating(false);
    MetadataManager::instance().notifyFullUIRebuild();
}

} // namespace ArcMeta