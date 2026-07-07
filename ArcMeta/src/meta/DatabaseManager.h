#ifndef ARCMETA_DATABASE_MANAGER_H
#define ARCMETA_DATABASE_MANAGER_H

#include <QString>
#include <QObject>
#include "sqlite3.h"
#include <map>
#include <string>
#include <mutex>
#include <functional>
#include <deque>
#include <thread>
#include <condition_variable>
#include <atomic>

struct sqlite3;

namespace ArcMeta {

/**
 * @brief 数据库事务 RAII 守卫
 * 确保即使在逻辑分支提前返回时事务也能安全关闭。
 */
class SqlTransaction {
public:
    explicit SqlTransaction(struct sqlite3* db);
    ~SqlTransaction();

    bool commit();
    void rollback();

private:
    struct sqlite3* m_db;
    bool m_committed = false;
    bool m_isNested = false;
};

class DatabaseManager : public QObject {
    Q_OBJECT
public:
    static DatabaseManager& instance();

    /**
     * @brief 初始化数据库（加载所有挂载驱动器的数据库到内存）
     */
    bool init();

    /**
     * @brief 持久化所有内存库到磁盘
     */
    void flushAll();

    /**
     * @brief 2026-07-xx 按照用户要求 (1.21)：步进式持久化接口
     * @return 如果所有备份已完成，返回 true；否则返回 false。
     */
    bool flushStep();

    /**
     * @brief 显式关闭并释放所有数据库资源 (1.21)
     */
    void shutdown();

    /**
     * @brief 获取指定磁盘卷序列号对应的内存连接
     * @param volumeSerial 磁盘卷序列号（如 A1B2C3D4）
     * @param driveLetter 盘符（如 "D" 或 "D:"），可选。若提供则触发数据库文件名自适应重命名。
     */
    sqlite3* getMemoryDb(const std::wstring& volumeSerial, const QString& driveLetter = "");

    /**
     * @brief 获取全局数据库内存连接
     */
    sqlite3* getGlobalDb();

    /**
     * @brief 获取指定内存连接对应的磁盘连接（仅供异步同步使用）
     */
    sqlite3* getDiskDb(sqlite3* memDb);

    /**
     * @brief 将任务投递到异步 I/O 队列
     */
    void enqueueSyncTask(std::function<void()> task);

    /**
     * @brief 获取当前挂起的同步任务总数 (Atomic)
     */
    int getPendingTasksCount() const { return m_pendingTasksCount.load(); }

    /**
     * @brief 内部接口：增减任务计数 (Plan-131 方案 D)
     */
    void incrementPendingTasks();
    void decrementPendingTasks();

signals:
    /**
     * @brief 异步任务计数变更信号
     */
    void pendingTasksCountChanged(int count);

private:
    DatabaseManager(QObject* parent = nullptr);
    ~DatabaseManager();

    struct DbConnection {
        sqlite3* diskDb = nullptr;
        sqlite3* memDb = nullptr;
        sqlite3_backup* activeBackup = nullptr;
        std::wstring diskPath;
    };

    void startWorkerThread();
    void stopWorkerThread();
    void workerLoop();

    std::deque<std::function<void()>> m_syncQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::thread m_workerThread;
    std::atomic<bool> m_stopWorker{false};
    std::atomic<int> m_pendingTasksCount{0};

    /**
     * @brief 异步任务 RAII 令牌 (Plan-131 方案 D)
     */
    struct SyncTaskToken {
        SyncTaskToken();
        SyncTaskToken(const SyncTaskToken&);
        ~SyncTaskToken();
    };

    std::map<std::wstring, DbConnection> m_driveDbs;
    DbConnection m_globalDb;
    std::mutex m_mutex;

    bool loadDb(const std::wstring& diskPath, DbConnection& conn);
    void saveDb(DbConnection& conn);
    void closeDb(DbConnection& conn);

    QString getAppDir();
    void ensureHidden(const std::wstring& path);
};

} // namespace ArcMeta

#endif // ARCMETA_DATABASE_MANAGER_H
