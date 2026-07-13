#include "CoreController.h"
#include "../meta/MetadataManager.h"
#include "../mft/MftReader.h"
#include <QtConcurrent>
#include <QThreadPool>
#include <QDebug>
#include <QDateTime>

namespace FERREX {

CoreController& CoreController::instance() {
    static CoreController inst;
    return inst;
}

CoreController::CoreController(QObject* parent) : QObject(parent) {
}

CoreController::~CoreController() {}

void CoreController::startSystem() {
    QThreadPool::globalInstance()->start([this]() {
        try {
            qint64 startTime = QDateTime::currentMSecsSinceEpoch();
            qInfo() << "[Core] >>> 开始后台异步初始化链条 (轻量化元数据架构) <<<";
            
            QMetaObject::invokeMethod(this, [this]() {
                setStatus("正在载入配置...", true);
            }, Qt::QueuedConnection);
            
            MetadataManager::instance().loadAllMetaAsync();
            
            QMetaObject::invokeMethod(this, [this, startTime]() {
                setStatus("系统就绪", false);
                qInfo() << "[Core] !!! 系统就绪，总耗时:" << (QDateTime::currentMSecsSinceEpoch() - startTime) << "ms";
                emit initializationFinished();
            }, Qt::QueuedConnection);
        } catch (...) {
            qCritical() << "[Core] 初始化过程中发生未知异常";
            QMetaObject::invokeMethod(this, &CoreController::initializationFinished, Qt::QueuedConnection);
        }
    });
}

QStringList CoreController::performSearch(const QString& keyword) {
    if (keyword.isEmpty()) return {};
    // 2026-06-xx 架构重构：全局搜索已由 ScanController + MftReader 承载
    return {}; 
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

} // namespace FERREX
