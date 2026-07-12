#include "MetadataManager.h"
#include "MetadataDefs.h"
#include "AllFrnManager.h"
#include "../mft/MftReader.h"
#include "../ui/UiHelper.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QtConcurrent>
#include <QDir>
#include <QDebug>
#include <QTimer>
#include <QCoreApplication>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <fileapi.h>
#include <winbase.h>
#include <handleapi.h>
#include <winnt.h>

#include <cstdio>
#include <cwchar>
#include <mutex>
#include <shared_mutex>

namespace FERREX {

static std::wstring normalizePath(const std::wstring& path) {
    if (path.empty()) return L"";
    QString qp = QDir::toNativeSeparators(QDir::cleanPath(QString::fromStdWString(path)));
    if (qp.length() == 2 && qp.endsWith(':')) qp += '\\';
    if (qp.length() >= 2 && qp[1] == ':') qp[0] = qp[0].toUpper();
    return qp.toStdWString();
}

MetadataManager& MetadataManager::instance() {
    static MetadataManager inst;
    return inst;
}

MetadataManager::MetadataManager(QObject* parent) : QObject(parent) {
    // 2026-06-xx 极致精简重构：彻底废弃所有外部持久化逻辑，回归纯内存管理。
    // loadAllMetaAsync 现在仅作为架构预留或加载必要的全局驱动器配置。
    loadAllMetaAsync();
}

void MetadataManager::loadAllMetaAsync() {
    (void)QtConcurrent::run([this]() {
        std::unordered_map<std::wstring, RuntimeMeta> tempCache;
        qInfo() << "[Metadata] 启动纯内存元数据架构初始化...";

        // 仅加载全局驱动器配置作为基础视觉状态
        QString driversPath = QCoreApplication::applicationDirPath() + "/FERREX_drivers.json";
        QFile dFile(driversPath);
        if (dFile.open(QIODevice::ReadOnly)) {
            QJsonObject root = QJsonDocument::fromJson(dFile.readAll()).object();
            for (auto it = root.begin(); it != root.end(); ++it) {
                std::wstring nPath = normalizePath(it.key().toStdWString());
                RuntimeMeta rm;
                tempCache[nPath] = std::move(rm);
            }
        }

        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_cache = std::move(tempCache);
        }
        emit metaChanged("__RELOAD_ALL__");
        qInfo() << "[Metadata] 纯内存初始化完成.";
    });
}

RuntimeMeta MetadataManager::getMeta(const std::wstring& path) {
    std::wstring nPath = normalizePath(path);
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_cache.find(nPath);
        if (it != m_cache.end()) return it->second;
    }
    return RuntimeMeta();
}

// 废弃/占位实现 (完全移除所有磁盘 IO 操作)
QStringList MetadataManager::searchInCache(const QString&) { return {}; }
void MetadataManager::setPinned(const std::wstring&, bool) {}
void MetadataManager::setEncrypted(const std::wstring&, bool) {}
void MetadataManager::setPalettes(const std::wstring&, const QVector<QPair<QColor, float>>&) {}
QVector<QColor> MetadataManager::getPalettes(const std::wstring&) { return {}; }
void MetadataManager::renameItem(const std::wstring&, const std::wstring&) {}
void MetadataManager::removeMetadataSync(const std::wstring&) {}
void MetadataManager::syncPhysicalMetadata(const std::wstring&) {}
std::string MetadataManager::getFileIdSync(const std::wstring&) { return ""; }
bool MetadataManager::hasPendingSync() const { return false; }
QStringList MetadataManager::getPendingSyncDirs() { return {}; }
void MetadataManager::removeFidsFromLog(const QStringList&) {}
std::wstring MetadataManager::getVolumeSerialNumber(const std::wstring&) { return L""; }
void MetadataManager::addToSyncLog(const std::wstring&) {}
bool MetadataManager::fetchWinApiMetadataDirect(const std::wstring&, std::string&, std::wstring*, long long*, std::wstring*, long long*, long long*, long long*) { return false; }
void MetadataManager::persistAsync(const std::wstring&) {}
void MetadataManager::debouncePersist(const std::wstring&) {}
void MetadataManager::saveSyncLog() {}

} // namespace FERREX
