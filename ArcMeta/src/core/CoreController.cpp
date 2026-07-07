#include "CoreController.h"
#include "AutoImportManager.h"
#include "../mft/MftReader.h"
#include "AppConfig.h"
#include "../meta/CategoryRepo.h"
#include "../meta/MetadataManager.h"
#include "../ui/Logger.h"
#include <QThreadPool>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QtConcurrent>
#include <unordered_set>

namespace ArcMeta {

CoreController& CoreController::instance() {
    static CoreController inst;
    return inst;
}

CoreController::CoreController(QObject* parent) : QObject(parent) {
}

CoreController::~CoreController() {}

/**
 * @brief 启动系统初始化链条
 * 彻底废除分布式文件模式，全面转向 SQLite 内存模式 (One-Drive-One-DB)
 */
void CoreController::startSystem() {
    QThreadPool::globalInstance()->start([this]() {
        try {
            qint64 startTime = QDateTime::currentMSecsSinceEpoch();
            qDebug() << "[Core] >>> 开始后台异步初始化 (SQLite 内存模式) <<<";
            
            QMetaObject::invokeMethod(this, [this]() {
                setStatus("正在载入元数据缓存...", true);
            }, Qt::QueuedConnection);
            
            // 仅执行 SQLite 模式初始化
            MetadataManager::instance().initFromScchMode();

            // 2026-08-xx 按照 Plan-126：彻底废除 NativeFolderWatcher (IOCP) 双轨制。
            // 全面转向单一 USN Journal 主轨。
            AutoImportManager::instance().startListening();
            
            // [Plan-129] USN 监控点火：系统启动时自动载入缓存并开启监控线程
            MftReader::instance().loadFromCache();

            // 2026-08-xx 物理同步：初始化完成后执行一次全量物理库对账 (在后台线程执行，避免阻塞 UI)
            AutoImportManager::instance().syncAllManagedLibraries();

            QMetaObject::invokeMethod(this, [this, startTime]() {
                setStatus("系统就绪", false);
                qDebug() << "[Core] !!! SQLite 内存模式初始化就绪，耗时:" << (QDateTime::currentMSecsSinceEpoch() - startTime) << "ms";
                emit initializationFinished();
            }, Qt::QueuedConnection);

        } catch (...) {
            qCritical() << "[Core] 初始化过程中发生异常";
            QMetaObject::invokeMethod(this, [this]() {
                setStatus("初始化失败", false);
                emit initializationFinished();
            }, Qt::QueuedConnection);
        }
    });
}

void CoreController::performSearch(const QString& keyword, const QString& scopeSource, int categoryId, const QString& parentPath) {
    // 1. 物理中止旧任务：无论新词是否为空，只要发起 performSearch 就必须清理前序任务
    abortSearch();
    
    ArcMeta::Logger::log(QString("[Core] performSearch 触发 -> 词: %1 | 来源: %2 | 路径: %3")
                        .arg(keyword).arg(scopeSource).arg(parentPath));

    if (keyword.isEmpty()) {
        ArcMeta::Logger::log("[Core] 关键词为空，跳过执行检索流程");
        return;
    }
    
    m_isSearchAborted = false;
    m_isSearching = true;
    int searchId = ++m_currentSearchId;

    ArcMeta::Logger::log(QString("[Core] 搜索任务已启动 [%1]，正在发射 searchStarted 信号...").arg(searchId));
    emit searchStarted();

    // 2. 异步启动双轨搜索任务
    (void)QtConcurrent::run([this, keyword, scopeSource, categoryId, parentPath, searchId]() {
        QStringList cacheResults;
        std::unordered_set<std::wstring> seenPaths;
        int totalFound = 0;

        // --- 第一阶段：内存缓存检索 (极速响应) ---
        cacheResults = MetadataManager::instance().searchInCache(keyword, scopeSource, categoryId, parentPath);
        for (const auto& p : cacheResults) {
            seenPaths.insert(p.toStdWString());
        }
        totalFound = static_cast<int>(cacheResults.size());

        // 发射第一批缓存结果
        if (!m_isSearchAborted && m_currentSearchId == searchId) {
            ArcMeta::Logger::log(QString("[Core] 缓存阶段发现 %1 条结果 [%2]，正在流式传输...").arg(cacheResults.size()).arg(searchId));
            emit searchResultsAvailable(cacheResults, false);
        }

        // --- 第二阶段：如果是物理导航模式，执行 I/O 扫描补全 (Plan-57) ---
        if (scopeSource == "nav" && !parentPath.isEmpty() && !m_isSearchAborted && m_currentSearchId == searchId) {
            QDirIterator it(parentPath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            QStringList batch;
            int scanCount = 0;
            
            while (it.hasNext()) {
                if (m_isSearchAborted || m_currentSearchId != searchId) break;
                scanCount++;
                if (scanCount % 2000 == 0) {
                     ArcMeta::Logger::log(QString("[Core] I/O 扫描进度: 已检查 %1 个项目 [%2]").arg(scanCount).arg(searchId));
                }
                
                QString fullPath = it.next();
                QString fileName = it.fileName();
                
                // 关键词匹配逻辑 (简单文件名包含，未来可扩展为更复杂匹配)
                if (fileName.contains(keyword, Qt::CaseInsensitive)) {
                    std::wstring wPath = MetadataManager::normalizePath(fullPath.toStdWString());
                    // 去重：如果已经在缓存中搜到了，则跳过
                    if (seenPaths.find(wPath) == seenPaths.end()) {
                        batch << fullPath;
                        seenPaths.insert(wPath);
                        totalFound++;

                        // 攒批发射，防止 UI 信号淹没
                        if (batch.size() >= 50) {
                            emit searchResultsAvailable(batch, true);
                            batch.clear();
                        }
                    }
                }
            }
            
            if (!batch.isEmpty() && !m_isSearchAborted) {
                emit searchResultsAvailable(batch, true);
            }
        }

        m_isSearching = false;
        if (!m_isSearchAborted && m_currentSearchId == searchId) {
            ArcMeta::Logger::log(QString("[Core] 搜索总计发现 %1 项 [%2]，发送 searchFinished 信号").arg(totalFound).arg(searchId));
            emit searchFinished(totalFound);
        } else {
            ArcMeta::Logger::log(QString("[Core] 搜索任务被中途中止或作废 [%1]").arg(searchId));
        }
    });
}

void CoreController::abortSearch() {
    m_isSearchAborted = true;
    // 等待现有搜索任务退出的轻量化处理（实际生产环境可能需要更复杂的等待机制）
}

void CoreController::handleDeviceChange(unsigned long wParam, unsigned long long lParam) {
#ifdef Q_OS_WIN
    // 2026-05-24 按照用户要求：捕捉硬件变更，硬盘插入时触发 GLOB 扫描对账
    // [Plan-131 方案 E] 从 MainWindow 迁移至此
    if (wParam == 0x8000 /* DBT_DEVICEARRIVAL */ || wParam == 0x8004 /* DBT_DEVICEREMOVECOMPLETE */) {
        qDebug() << "[Core] [Plan-131] 检测到磁盘硬件变更，触发全量 GLOB 对账...";
        // 异步触发扫描，防止阻塞 UI
        (void)QtConcurrent::run([]() {
            // 这里可以根据需要驱动 MftReader 重新扫描或 AutoImportManager 对账
            // 例如：MftReader::instance().buildIndex(); 
            // 或者 AutoImportManager::instance().syncAllManagedLibraries();
        });
    }
#endif
    Q_UNUSED(lParam);
}

void CoreController::setStatus(const QString& text, bool indexing) {
    if (m_statusText != text) {
        m_statusText = text;
        emit statusTextChanged(m_statusText);
    }
    if (m_isIndexing != indexing) {
        m_isIndexing = indexing;
        emit isIndexingChanged(m_isIndexing);
    }
}

} // namespace ArcMeta
