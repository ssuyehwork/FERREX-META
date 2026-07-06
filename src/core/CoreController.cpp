#include "CoreController.h"
#include "../db/Database.h"
#include "../db/SyncEngine.h"
#include "../db/CategoryRepo.h"
#include "../db/ItemRepo.h"
#include "../meta/MetadataManager.h"
#include "../mft/MftReader.h"
#include <QtConcurrent>
#include <QThreadPool>
#include <QDebug>

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
 * 2026-05-24 按照用户要求：实现纯数据库架构。
 */
void CoreController::startSystem() {
    // 异步链式初始化
    QThreadPool::globalInstance()->start([this]() {
        try {
            qint64 startTime = QDateTime::currentMSecsSinceEpoch();
            qDebug() << "[Core] >>> 开始后台异步初始化链条 (纯数据库架构已就位) <<<";
            
            // 1. 初始化数据库元数据内存镜像 (关键：提供丝滑的 UI 首次渲染)
            // 2026-04-12 修复：使用 QMetaObject::invokeMethod 确保 UI 更新在主线程执行，避免跨线程信号问题
            QMetaObject::invokeMethod(this, [this]() {
                setStatus("正在载入元数据缓存...", true);
            }, Qt::QueuedConnection);
            
            MetadataManager::instance().initFromDatabase();
            qDebug() << "[Core] 数据库元数据缓存加载完成，耗时:" << (QDateTime::currentMSecsSinceEpoch() - startTime) << "ms";

            // 2026-06-xx 按照用户要求：物理剥离 MFT 索引初始化。
            // ".scch" 缓存仅在 ScanDialog 中按需加载，MainWindow 启动时不占用相关内存。
            qDebug() << "[Core] 已跳过 MFT 索引加载 (按需加载模式已启用)";

            // 2. 执行一次增量对账
            // 2026-05-14 架构修正：消除“异步就绪幻觉”，确保对账完成后再宣告就绪
            SyncEngine::instance().runIncrementalSync([this, startTime]() {
                QMetaObject::invokeMethod(this, [this, startTime]() {
                    setStatus("系统就绪", false);
                    qDebug() << "[Core] !!! 纯数据库架构初始化就绪，总耗时:" << (QDateTime::currentMSecsSinceEpoch() - startTime) << "ms";
                    emit initializationFinished();
                }, Qt::QueuedConnection);
            });
        } catch (const std::exception& e) {
            qCritical() << "[Core] 初始化过程中发生异常:" << e.what();
            QMetaObject::invokeMethod(this, [this]() {
                setStatus("初始化失败", false);
            }, Qt::QueuedConnection);
            QMetaObject::invokeMethod(this, &CoreController::initializationFinished, Qt::QueuedConnection);
        } catch (...) {
            qCritical() << "[Core] 初始化过程中发生未知异常";
            QMetaObject::invokeMethod(this, [this]() {
                setStatus("初始化失败", false);
            }, Qt::QueuedConnection);
            QMetaObject::invokeMethod(this, &CoreController::initializationFinished, Qt::QueuedConnection);
        }
    });
}

QStringList CoreController::performSearch(const QString& keyword) {
    if (keyword.isEmpty()) return {};

    QStringList paths;
    if (CategoryRepo::isJsonMode()) {
        // 模式 A: 纯 JSON 内存搜索
        paths = MetadataManager::instance().searchInCache(keyword);
    } else {
        // 模式 B: 经典数据库搜索 (已包含路径+标签+备注检索)
        paths = ItemRepo::searchByKeyword(keyword, "");
    }
    return paths;
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
