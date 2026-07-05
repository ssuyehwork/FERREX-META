#include "Database.h"
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QDir>
#include <QStandardPaths>
#include <QThread>

namespace ArcMeta {

struct Database::Impl {
    QSqlDatabase db;
    std::wstring dbPath;
};

Database& Database::instance() {
    static Database inst;
    return inst;
}

Database::Database() : m_impl(std::make_unique<Impl>()) {}
Database::~Database() = default;

bool Database::init(const std::wstring& dbPath) {
    m_impl->dbPath = dbPath;
    m_impl->db = QSqlDatabase::addDatabase("QSQLITE");
    m_impl->db.setDatabaseName(QString::fromStdWString(dbPath));

    if (!m_impl->db.open()) return false;

    // 关键红线：防死锁配置
    QSqlQuery query(m_impl->db);
    query.exec("PRAGMA journal_mode=WAL;");
    query.exec("PRAGMA synchronous=NORMAL;");
    query.exec("PRAGMA busy_timeout=5000;");

    createTables();
    createIndexes();
    return true;
}

std::wstring Database::getDbPath() const {
    return m_impl->dbPath;
}

QSqlDatabase Database::getThreadDatabase() {
    // 2026-03-xx 性能优化：通过线程本地变量缓存连接名称，减少字符串拼接开销
    static thread_local QString threadConnectionName;
    if (threadConnectionName.isEmpty()) {
        threadConnectionName = QString("conn_%1").arg((quintptr)QThread::currentThreadId());
    }
    
    // 如果该线程已经建立过连接，直接返回现有连接
    if (QSqlDatabase::contains(threadConnectionName)) {
        QSqlDatabase db = QSqlDatabase::database(threadConnectionName);
        if (db.isOpen()) {
            return db;
        }
    }

    // 否则，为新线程建立独立连接
    // 2026-04-12 用户优化：添加异常处理，防止线程数据库连接失败导致程序闪退
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", threadConnectionName);
    db.setDatabaseName(QString::fromStdWString(m_impl->dbPath));
    
    if (!db.open()) {
        qCritical() << "[Database] 线程" << QThread::currentThreadId() << "无法打开数据库:"
                    << db.lastError().text() << "，路径:" << QString::fromStdWString(m_impl->dbPath);
        return db;  // 返回无效的数据库对象，调用方应检查 isOpen()
    }

    // 对新连接同样应用 WAL 优化，防止写入锁死
    QSqlQuery query(db);
    if (!query.exec("PRAGMA journal_mode=WAL;")) {
        qWarning() << "[Database] 设置 WAL 模式失败:" << query.lastError().text();
    }
    query.exec("PRAGMA synchronous=NORMAL;");
    query.exec("PRAGMA busy_timeout=5000;");
    
    qDebug() << "[Database] 线程" << QThread::currentThreadId() << "数据库连接已建立";
    return db;
}

void Database::createTables() {
    QSqlQuery q(m_impl->db);
    q.exec("CREATE TABLE IF NOT EXISTS folders (volume TEXT, path TEXT, rating INTEGER DEFAULT 0, color TEXT DEFAULT '', tags TEXT DEFAULT '', pinned INTEGER DEFAULT 0, note TEXT DEFAULT '', sort_by TEXT DEFAULT 'name', sort_order TEXT DEFAULT 'asc', encrypted INTEGER DEFAULT 0, encrypt_salt TEXT DEFAULT '', encrypt_iv TEXT DEFAULT '', encrypt_verify_hash TEXT DEFAULT '', file_id_128 TEXT DEFAULT '', last_sync REAL, PRIMARY KEY (volume, path))");
    q.exec("CREATE TABLE IF NOT EXISTS items (volume TEXT NOT NULL, frn TEXT NOT NULL, path TEXT, parent_path TEXT, type TEXT, rating INTEGER DEFAULT 0, color TEXT DEFAULT '', tags TEXT DEFAULT '', pinned INTEGER DEFAULT 0, note TEXT DEFAULT '', encrypted INTEGER DEFAULT 0, encrypt_salt TEXT DEFAULT '', encrypt_iv TEXT DEFAULT '', encrypt_verify_hash TEXT DEFAULT '', original_name TEXT DEFAULT '', file_id_128 TEXT DEFAULT '', size INTEGER DEFAULT 0, ctime REAL DEFAULT 0, mtime REAL DEFAULT 0, atime REAL DEFAULT 0, deleted INTEGER DEFAULT 0, PRIMARY KEY (volume, frn))");
    q.exec("CREATE TABLE IF NOT EXISTS tags (tag TEXT PRIMARY KEY, item_count INTEGER DEFAULT 0)");
    q.exec("CREATE TABLE IF NOT EXISTS favorites (path TEXT PRIMARY KEY, type TEXT, name TEXT, sort_order INTEGER DEFAULT 0, added_at REAL)");
    q.exec("CREATE TABLE IF NOT EXISTS categories (id INTEGER PRIMARY KEY AUTOINCREMENT, parent_id INTEGER DEFAULT 0, name TEXT NOT NULL, color TEXT DEFAULT '', preset_tags TEXT DEFAULT '', sort_order INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, encrypted INTEGER DEFAULT 0, encrypt_salt TEXT DEFAULT '', encrypt_iv TEXT DEFAULT '', encrypt_verify_hash TEXT DEFAULT '', encrypt_hint TEXT DEFAULT '', created_at REAL)");
    q.exec("CREATE TABLE IF NOT EXISTS category_items (category_id INTEGER, file_id_128 TEXT, added_at REAL, PRIMARY KEY (category_id, file_id_128))");
    q.exec("CREATE TABLE IF NOT EXISTS sync_state (key TEXT PRIMARY KEY, value TEXT)");
    
    // 2026-04-12 按照用户要求：新增文件夹扫描缓存表，用于增量扫描剪枝
    q.exec("CREATE TABLE IF NOT EXISTS folder_scan_cache ("
           "path TEXT PRIMARY KEY, "
           "mtime INTEGER NOT NULL, "
           "scanned_at INTEGER NOT NULL)");

    // 紧急补丁：由于 CREATE TABLE IF NOT EXISTS 不会修改已存在的表，
    // 显式检查并添加缺失的字段。
    auto addColumnIfNotExist = [&](const QString& table, const QString& column, const QString& type) {
        QSqlQuery check(m_impl->db);
        check.exec(QString("PRAGMA table_info(%1)").arg(table));
        bool exists = false;
        while (check.next()) {
            if (check.value(1).toString() == column) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            QSqlQuery q(m_impl->db);
            q.exec(QString("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, type));
        }
    };

    addColumnIfNotExist("categories", "preset_tags", "TEXT DEFAULT ''");
    addColumnIfNotExist("categories", "sort_order", "INTEGER DEFAULT 0");
    addColumnIfNotExist("categories", "pinned", "INTEGER DEFAULT 0");
    addColumnIfNotExist("categories", "encrypted", "INTEGER DEFAULT 0");
    addColumnIfNotExist("categories", "encrypt_salt", "TEXT DEFAULT ''");
    addColumnIfNotExist("categories", "encrypt_iv", "TEXT DEFAULT ''");
    addColumnIfNotExist("categories", "encrypt_verify_hash", "TEXT DEFAULT ''");
    addColumnIfNotExist("categories", "encrypt_hint", "TEXT DEFAULT ''");

    addColumnIfNotExist("folders", "volume", "TEXT DEFAULT ''");
    addColumnIfNotExist("folders", "encrypted", "INTEGER DEFAULT 0");
    addColumnIfNotExist("folders", "encrypt_salt", "TEXT DEFAULT ''");
    addColumnIfNotExist("folders", "encrypt_iv", "TEXT DEFAULT ''");
    addColumnIfNotExist("folders", "encrypt_verify_hash", "TEXT DEFAULT ''");
    addColumnIfNotExist("folders", "file_id_128", "TEXT DEFAULT ''");
    addColumnIfNotExist("folders", "palettes", "TEXT DEFAULT ''");
    addColumnIfNotExist("items", "file_id_128", "TEXT DEFAULT ''");
    addColumnIfNotExist("items", "size", "INTEGER DEFAULT 0");
    addColumnIfNotExist("items", "palettes", "TEXT DEFAULT ''");

    // 2026-06-xx 物理架构升级：检测并迁移 category_items 表结构
    // 逻辑：如果表里还存在 item_path 字段，说明是旧版结构，执行“核平”式重建（因为旧路径关联已废弃）
    {
        QSqlQuery check(m_impl->db);
        check.exec("PRAGMA table_info(category_items)");
        bool hasItemPath = false;
        while (check.next()) {
            if (check.value(1).toString() == "item_path") {
                hasItemPath = true;
                break;
            }
        }
        if (hasItemPath) {
            qDebug() << "[Database] 检测到旧版 category_items 结构，正在执行 File ID 架构迁移...";
            QSqlQuery drop(m_impl->db);
            drop.exec("DROP TABLE category_items");
            drop.exec("CREATE TABLE category_items (category_id INTEGER, file_id_128 TEXT, added_at REAL, PRIMARY KEY (category_id, file_id_128))");
        }
    }
}

void Database::createIndexes() {
    QSqlQuery q(m_impl->db);
    q.exec("CREATE INDEX IF NOT EXISTS idx_items_path ON items(path)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_items_parent ON items(parent_path)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_items_deleted ON items(deleted)");
    
    // 2026-06-xx 性能加固：按照用户要求，为元数据字段建立物理索引，实现工业级检索性能
    q.exec("CREATE INDEX IF NOT EXISTS idx_items_rating ON items(rating)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_items_color ON items(color)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_items_pinned ON items(pinned)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_items_fid ON items(file_id_128)");

    // 2026-06-xx 性能加固：为 category_items 增加索引以加速 2.4M 数据量的统计逻辑
    q.exec("CREATE INDEX IF NOT EXISTS idx_catitems_cid ON category_items(category_id)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_catitems_fid ON category_items(file_id_128)");
}

qint64 Database::queryFolderCache(const std::wstring& path) {
    QSqlDatabase db = getThreadDatabase();
    QSqlQuery q(db);
    q.prepare("SELECT mtime FROM folder_scan_cache WHERE path = ?");
    q.addBindValue(QString::fromStdWString(path));
    if (q.exec() && q.next()) {
        return q.value(0).toLongLong();
    }
    return -1;
}

void Database::upsertFolderCache(const std::wstring& path, qint64 mtime) {
    QSqlDatabase db = getThreadDatabase();
    QSqlQuery q(db);
    // 2026-04-12 按照用户要求：使用 INSERT ... ON CONFLICT(path) DO UPDATE 语法
    q.prepare("INSERT INTO folder_scan_cache (path, mtime, scanned_at) VALUES (?, ?, ?) "
              "ON CONFLICT(path) DO UPDATE SET mtime = excluded.mtime, scanned_at = excluded.scanned_at");
    q.addBindValue(QString::fromStdWString(path));
    q.addBindValue(mtime);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        qCritical() << "[Database] upsertFolderCache 失败:" << q.lastError().text();
    }
}

} // namespace ArcMeta
