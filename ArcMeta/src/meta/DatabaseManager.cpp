#include "DatabaseManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>
#include <windows.h>
#include "MetadataManager.h"

namespace ArcMeta {

SqlTransaction::SqlTransaction(struct sqlite3* db) : m_db(db) {
    if (m_db) {
        // 2026-07-xx 物理修复 (1.22)：通过检测 autocommit 状态支持伪嵌套事务。
        // 如果 autocommit 为 0，说明已经处于外部事务中。
        m_isNested = (sqlite3_get_autocommit(m_db) == 0);
        
        if (!m_isNested) {
            // 2026-06-xx 物理加固：内置针对 SQLITE_BUSY 的重试机制
            int retry = 0;
            int rc;
            while ((rc = sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr)) == SQLITE_BUSY && retry++ < 5) {
                Sleep(50);
            }
            if (rc != SQLITE_OK) {
                qWarning() << "[DB] 事务开启失败:" << sqlite3_errmsg(m_db);
            }
        }
    }
}

SqlTransaction::~SqlTransaction() {
    if (m_db && !m_committed && !m_isNested) {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

bool SqlTransaction::commit() {
    if (m_db && !m_committed) {
        if (m_isNested) {
            m_committed = true;
            return true;
        }
        if (sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) {
            m_committed = true;
            return true;
        }
    }
    return false;
}

void SqlTransaction::rollback() {
    if (m_db && !m_committed) {
        if (!m_isNested) {
            sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        m_committed = true; // Mark as "processed" to prevent dtor rollback
    }
}

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager inst;
    return inst;
}

DatabaseManager::SyncTaskToken::SyncTaskToken() {
    DatabaseManager::instance().incrementPendingTasks();
}

DatabaseManager::SyncTaskToken::SyncTaskToken(const SyncTaskToken&) {
    DatabaseManager::instance().incrementPendingTasks();
}

DatabaseManager::SyncTaskToken::~SyncTaskToken() {
    DatabaseManager::instance().decrementPendingTasks();
}

DatabaseManager::DatabaseManager(QObject* parent) : QObject(parent) {
    startWorkerThread();
}

DatabaseManager::~DatabaseManager() {
    stopWorkerThread();
    flushAll();
    for (auto& pair : m_driveDbs) {
        closeDb(pair.second);
    }
    closeDb(m_globalDb);
}

QString DatabaseManager::getAppDir() {
    return QCoreApplication::applicationDirPath();
}

void DatabaseManager::ensureHidden(const std::wstring& path) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
}

bool DatabaseManager::loadDb(const std::wstring& diskPath, DbConnection& conn) {
    std::string utf8Path = QString::fromStdWString(diskPath).toUtf8().toStdString();
    qDebug() << "[DB] [Plan-130] 直连磁盘数据库模式开启 ->" << QString::fromStdString(utf8Path);
    
    // 1. [Plan-130] 秒退架构：彻底废弃 :memory: 中转，直连磁盘数据库
    if (sqlite3_open_v2(utf8Path.c_str(), &conn.diskDb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        qDebug() << "[DB] Failed to open disk DB:" << QString::fromStdString(utf8Path);
        return false;
    }
    conn.memDb = conn.diskDb; // memDb 句柄现在指向磁盘 DB 句柄，保持上层接口兼容
    ensureHidden(diskPath);

    // 2. 配置高性能 WAL 模式
    sqlite3_exec(conn.diskDb, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(conn.diskDb, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);

    // 初始化表结构 (Schema)
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS metadata (
            file_id TEXT PRIMARY KEY,
            path TEXT NOT NULL,
            is_folder INTEGER DEFAULT 0,
            rating INTEGER DEFAULT 0,
            color TEXT,
            tags TEXT,
            note TEXT,
            url TEXT,
            ctime INTEGER,
            mtime INTEGER,
            atime INTEGER,
            file_size INTEGER,
            palettes BLOB,
            is_trash INTEGER DEFAULT 0,
            original_path TEXT,
            is_invalid INTEGER DEFAULT 0,
            width INTEGER DEFAULT 0,
            height INTEGER DEFAULT 0,
            ingestion_status INTEGER DEFAULT -1
        );
        CREATE INDEX IF NOT EXISTS idx_path ON metadata(path);

        -- 分类定义表
        CREATE TABLE IF NOT EXISTS categories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            parent_id INTEGER DEFAULT 0,
            name TEXT NOT NULL,
            color TEXT,
            preset_tags TEXT,
            sort_order INTEGER DEFAULT 0,
            pinned INTEGER DEFAULT 0,
            encrypted INTEGER DEFAULT 0,
            encrypt_hint TEXT,
            physical_frn INTEGER DEFAULT 0,
            physical_path TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_categories_frn ON categories(physical_frn);

        -- 分类与项目关联表
        CREATE TABLE IF NOT EXISTS category_items (
            category_id INTEGER,
            file_id TEXT,
            path_hint TEXT,
            added_at REAL,
            PRIMARY KEY (category_id, file_id)
        );

        -- 系统统计表
        CREATE TABLE IF NOT EXISTS system_stats (
            key TEXT PRIMARY KEY,
            value INTEGER DEFAULT 0
        );

        -- 标签组表
        CREATE TABLE IF NOT EXISTS tag_groups (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            color TEXT,
            sort_order INTEGER DEFAULT 0
        );

        -- 标签与标签组关联表
        CREATE TABLE IF NOT EXISTS tag_group_items (
            group_id INTEGER,
            tag_name TEXT,
            PRIMARY KEY (group_id, tag_name)
        );
    )";
    char* errMsg = nullptr;
    sqlite3_exec(conn.memDb, schema, nullptr, nullptr, &errMsg);
    if (errMsg) {
        qDebug() << "[DB] Schema error:" << errMsg;
        sqlite3_free(errMsg);
    } else {
        // 2026-06-xx 按照用户要求：清理任何误入 categories 表的系统保留 ID。
        // 系统分类 ID (-1, -2) 仅作为桶位标记存在于逻辑层，绝不可作为 UI 节点存储在 categories 表中。
        const char* cleanup = "DELETE FROM categories WHERE id <= 0;";
        sqlite3_exec(conn.memDb, cleanup, nullptr, nullptr, nullptr);
    }

    // 2026-07-xx 物理加固：自动迁移旧版本数据库字段 (Plan-29)
    sqlite3_stmt* checkStmt;
    bool hasInvalidColumn = false;
    bool hasWidthColumn = false;
    bool hasHeightColumn = false;
    bool hasIngestionStatusColumn = false;

    if (sqlite3_prepare_v2(conn.memDb, "PRAGMA table_info(metadata)", -1, &checkStmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(checkStmt) == SQLITE_ROW) {
            const char* colName = reinterpret_cast<const char*>(sqlite3_column_text(checkStmt, 1));
            if (colName) {
                std::string name(colName);
                if (name == "is_invalid") hasInvalidColumn = true;
                if (name == "width") hasWidthColumn = true;
                if (name == "height") hasHeightColumn = true;
                if (name == "ingestion_status") hasIngestionStatusColumn = true;
            }
        }
        sqlite3_finalize(checkStmt);
    }

    if (!hasInvalidColumn) {
        qDebug() << "[DB] 检测到旧版数据库，正在添加 is_invalid 字段...";
        sqlite3_exec(conn.memDb, "ALTER TABLE metadata ADD COLUMN is_invalid INTEGER DEFAULT 0", nullptr, nullptr, nullptr);
    }
    if (!hasWidthColumn) {
        qDebug() << "[DB] 检测到旧版数据库，正在添加 width 字段...";
        sqlite3_exec(conn.memDb, "ALTER TABLE metadata ADD COLUMN width INTEGER DEFAULT 0", nullptr, nullptr, nullptr);
    }
    if (!hasHeightColumn) {
        qDebug() << "[DB] 检测到旧版数据库，正在添加 height 字段...";
        sqlite3_exec(conn.memDb, "ALTER TABLE metadata ADD COLUMN height INTEGER DEFAULT 0", nullptr, nullptr, nullptr);
    }
    if (!hasIngestionStatusColumn) {
        qDebug() << "[DB] 检测到旧版数据库，正在添加 ingestion_status 字段...";
        sqlite3_exec(conn.memDb, "ALTER TABLE metadata ADD COLUMN ingestion_status INTEGER DEFAULT -1", nullptr, nullptr, nullptr);
    }

    // 2026-08-xx 物理同步扩展：迁移 categories 表字段
    sqlite3_stmt* catCheckStmt;
    bool hasFrnColumn = false;
    bool hasPhysicalPathColumn = false;
    if (sqlite3_prepare_v2(conn.memDb, "PRAGMA table_info(categories)", -1, &catCheckStmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(catCheckStmt) == SQLITE_ROW) {
            const char* colName = reinterpret_cast<const char*>(sqlite3_column_text(catCheckStmt, 1));
            if (colName) {
                std::string name(colName);
                if (name == "physical_frn") hasFrnColumn = true;
                if (name == "physical_path") hasPhysicalPathColumn = true;
            }
        }
        sqlite3_finalize(catCheckStmt);
    }

    if (!hasFrnColumn) {
        sqlite3_exec(conn.memDb, "ALTER TABLE categories ADD COLUMN physical_frn INTEGER DEFAULT 0", nullptr, nullptr, nullptr);
    }
    if (!hasPhysicalPathColumn) {
        sqlite3_exec(conn.memDb, "ALTER TABLE categories ADD COLUMN physical_path TEXT", nullptr, nullptr, nullptr);
    }

    // 2026-08-xx 索引优化
    sqlite3_exec(conn.memDb, "CREATE INDEX IF NOT EXISTS idx_categories_frn ON categories(physical_frn);", nullptr, nullptr, nullptr);

    conn.diskPath = diskPath;
    return true;
}

void DatabaseManager::saveDb(DbConnection& conn) {
    // [Plan-130] 秒退架构：废弃备份逻辑
    Q_UNUSED(conn);
}

void DatabaseManager::closeDb(DbConnection& conn) {
    // [Plan-130] 磁盘直连模式：仅关闭单一句柄
    if (conn.diskDb) {
        sqlite3_close_v2(conn.diskDb);
    }
    conn.memDb = nullptr;
    conn.diskDb = nullptr;
}

bool DatabaseManager::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    QString metaDir = getAppDir() + "/.arcmeta";
    QDir().mkpath(metaDir);
    ensureHidden(metaDir.toStdWString());

    // 加载全局库
    std::wstring globalPath = (metaDir + "/global.db").toStdWString();
    loadDb(globalPath, m_globalDb);

    // 为每个驱动器加载数据库
    // 注意：此处实际应遍历当前在线的驱动器，这里先简化逻辑
    // 实际运行时，MetadataManager 会按需通过 getMemoryDb 触发加载或由 init 调用
    return true;
}

void DatabaseManager::flushAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    saveDb(m_globalDb);
    for (auto& pair : m_driveDbs) {
        saveDb(pair.second);
    }
}

bool DatabaseManager::flushStep() {
    // [Plan-130] 秒退架构：彻底废除 flushStep
    return true;
}

void DatabaseManager::shutdown() {
    stopWorkerThread();

    std::lock_guard<std::mutex> lock(m_mutex);
    
    // [Plan-130] 秒退架构：关闭所有磁盘句柄
    for (auto& pair : m_driveDbs) {
        if (pair.second.diskDb) sqlite3_close_v2(pair.second.diskDb);
        pair.second.memDb = nullptr;
        pair.second.diskDb = nullptr;
    }
    if (m_globalDb.diskDb) sqlite3_close_v2(m_globalDb.diskDb);
    m_globalDb.memDb = nullptr;
    m_globalDb.diskDb = nullptr;
}

sqlite3* DatabaseManager::getMemoryDb(const std::wstring& volumeSerial, const QString& driveLetter) {
    std::lock_guard<std::mutex> lock(m_mutex);
    qDebug() << "[DB] getMemoryDb requested for Serial:" << QString::fromStdWString(volumeSerial) << "Letter:" << driveLetter;
    
    QString cleanLetter = "";
    if (!driveLetter.isEmpty()) {
        cleanLetter = driveLetter.at(0).toUpper();
    }

    // 2026-07-xx 按照用户要求：若数据库已加载但盘符发生变化，触发“关闭-重命名-重新打开”
    if (m_driveDbs.find(volumeSerial) != m_driveDbs.end()) {
        if (!cleanLetter.isEmpty()) {
            QString currentDiskPath = QString::fromStdWString(m_driveDbs[volumeSerial].diskPath);
            QString expectedFileName = QString("Arcmeta_%1_%2.db").arg(QString::fromStdWString(volumeSerial).toUpper()).arg(cleanLetter);
            if (!currentDiskPath.endsWith(expectedFileName)) {
                qDebug() << "[DB] 检测到盘符漂移，执行动态迁移:" << currentDiskPath << " -> " << expectedFileName;
                
                DbConnection& conn = m_driveDbs[volumeSerial];
                saveDb(conn); // 先持久化
                
                // 关闭句柄以解除占用
                if (conn.memDb) sqlite3_close_v2(conn.memDb);
                if (conn.diskDb) sqlite3_close_v2(conn.diskDb);
                conn.memDb = nullptr;
                conn.diskDb = nullptr;

                QString metaDir = getAppDir() + "/.arcmeta";
                QString targetPath = metaDir + "/" + expectedFileName;
                qDebug() << "[DB] 准备执行重命名:" << currentDiskPath << "->" << targetPath;
                
                // 如果目标已存在且不是自己，先将其移走（按用户规则重命名为无效）
                if (QFile::exists(targetPath) && targetPath != currentDiskPath) {
                    QString invalidBase = QString("%1/Arcmeta_%2_无效").arg(metaDir).arg(QString::fromStdWString(volumeSerial).toUpper());
                    QString invalidPath = invalidBase + ".db";
                    int counter = 1;
                    while (QFile::exists(invalidPath)) {
                        invalidPath = QString("%1_%2.db").arg(invalidBase).arg(counter++);
                    }
                    qDebug() << "[DB] 目标文件已存在，先将其重命名为无效:" << invalidPath;
                    QFile::rename(targetPath, invalidPath);
                }

                if (QFile::rename(currentDiskPath, targetPath)) {
                    conn.diskPath = targetPath.toStdWString();
                    qDebug() << "[DB] 重命名成功";
                } else {
                    qWarning() << "[DB] 重命名失败";
                }
                
                // 重新加载到内存
                loadDb(conn.diskPath, conn);
            }
        }
        return m_driveDbs[volumeSerial].memDb;
    }

    if (m_driveDbs.find(volumeSerial) == m_driveDbs.end()) {
        QString metaDir = getAppDir() + "/.arcmeta";
        QString serialStr = QString::fromStdWString(volumeSerial).toUpper();

        QString targetFileName = QString("Arcmeta_%1%2.db").arg(serialStr).arg(cleanLetter.isEmpty() ? "" : "_" + cleanLetter);
        QString targetPath = metaDir + "/" + targetFileName;

        // 探测-纠偏逻辑
        if (!QFile::exists(targetPath)) {
            QDir dir(metaDir);
            QStringList filters;
            filters << QString("Arcmeta_%1*.db").arg(serialStr);
            QFileInfoList list = dir.entryInfoList(filters, QDir::Files | QDir::Hidden | QDir::System, QDir::Time);

            if (!list.isEmpty()) {
                // Case A: 有旧文件。选择最近修改的一个作为目标进行重命名。
                QFileInfo bestInfo = list.first();
                if (!cleanLetter.isEmpty()) {
                    if (QFile::rename(bestInfo.absoluteFilePath(), targetPath)) {
                        qDebug() << "[DB] 自动纠偏：重命名数据库" << bestInfo.fileName() << "->" << targetFileName;
                    } else {
                        qWarning() << "[DB] 重命名失败，降级使用原文件加载:" << bestInfo.absoluteFilePath();
                        targetPath = bestInfo.absoluteFilePath();
                    }
                } else {
                    targetPath = bestInfo.absoluteFilePath();
                }

                // 处理冲突的其他旧文件 (Plan-97 补充要求)
                for (int i = 1; i < list.size(); ++i) {
                    QString conflictPath = list.at(i).absoluteFilePath();
                    QString invalidBase = QString("%1/Arcmeta_%2_无效").arg(metaDir).arg(serialStr);
                    QString invalidPath = invalidBase + ".db";
                    int counter = 1;
                    while (QFile::exists(invalidPath)) {
                        invalidPath = QString("%1_%2.db").arg(invalidBase).arg(counter++);
                    }
                    if (QFile::rename(conflictPath, invalidPath)) {
                        qDebug() << "[DB] 冲突处理：将冗余数据库标记为无效" << list.at(i).fileName() << "->" << QFileInfo(invalidPath).fileName();
                    }
                }
            }
        }

        DbConnection conn;
        if (loadDb(targetPath.toStdWString(), conn)) {
            m_driveDbs[volumeSerial] = conn;
        } else {
            return nullptr;
        }
    }
    return m_driveDbs[volumeSerial].memDb;
}

sqlite3* DatabaseManager::getGlobalDb() {
    return m_globalDb.memDb;
}

sqlite3* DatabaseManager::getDiskDb(sqlite3* memDb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_globalDb.memDb == memDb) return m_globalDb.diskDb;
    for (auto& pair : m_driveDbs) {
        if (pair.second.memDb == memDb) return pair.second.diskDb;
    }
    return nullptr;
}

void DatabaseManager::incrementPendingTasks() {
    int count = ++m_pendingTasksCount;
    emit pendingTasksCountChanged(count);
}

void DatabaseManager::decrementPendingTasks() {
    int count = --m_pendingTasksCount;
    emit pendingTasksCountChanged(count);
}

void DatabaseManager::enqueueSyncTask(std::function<void()> task) {
    SyncTaskToken token; 
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_syncQueue.push_back([task, token]() {
            task();
        });
    }
    m_queueCv.notify_one();
}

void DatabaseManager::startWorkerThread() {
    m_stopWorker = false;
    m_workerThread = std::thread(&DatabaseManager::workerLoop, this);
}

void DatabaseManager::stopWorkerThread() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopWorker = true;
    }
    m_queueCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void DatabaseManager::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this] { return m_stopWorker || !m_syncQueue.empty(); });
            if (m_stopWorker && m_syncQueue.empty()) break;
            task = std::move(m_syncQueue.front());
            m_syncQueue.pop_front();
        }
        if (task) {
            task();
        }
    }
}

} // namespace ArcMeta
