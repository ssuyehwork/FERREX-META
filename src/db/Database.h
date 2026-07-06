#pragma once

#include <string>
#include <memory>
#include <QSqlDatabase>

namespace ArcMeta {

/**
 * @brief 数据库管理类 (原生 QtSql 实现，拒绝第三方 SQLiteCpp)
 */
class Database {
public:
    static Database& instance();

    bool init(const std::wstring& dbPath);
    
    /**
     * @brief 获取数据库文件路径，用于工作线程创建独立连接
     */
    std::wstring getDbPath() const;

    /**
     * @brief 线程安全地获取当前线程专属的数据库连接
     * 2026-03-xx 按照用户要求：修复跨线程数据库访问导致的 "database not open" 警告。
     */
    QSqlDatabase getThreadDatabase();

    /**
     * @brief 2026-04-12 按照用户要求：查询文件夹扫描缓存的 mtime
     */
    qint64 queryFolderCache(const std::wstring& path);

    /**
     * @brief 2026-04-12 按照用户要求：更新或插入文件夹扫描缓存
     */
    void upsertFolderCache(const std::wstring& path, qint64 mtime);

private:
    Database();
    ~Database();
    
    void createTables();
    void createIndexes();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ArcMeta
