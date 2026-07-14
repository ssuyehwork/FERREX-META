#ifndef FERREX_METADATA_MANAGER_H
#define FERREX_METADATA_MANAGER_H

#include "MetadataDefs.h"
#include <QObject>
#include <QString>
#include <QTimer>
#include <QStringList>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <string>

namespace FERREX {

struct RuntimeMeta {
    std::wstring color;
    std::vector<PaletteEntry> palettes;

    bool hasUserOperations() const {
        return !color.empty() || !palettes.empty();
    }
};

class MetadataManager : public QObject {
    Q_OBJECT
public:
    static MetadataManager& instance();

    void loadAllMetaAsync();

    RuntimeMeta getMeta(const std::wstring& path);

    QStringList searchInCache(const QString& keyword);

    void setPinned(const std::wstring& path, bool pinned);
    void setEncrypted(const std::wstring& path, bool encrypted);

    void setPalettes(const std::wstring& path, const QVector<QPair<QColor, float>>& palettes);
    QVector<QColor> getPalettes(const std::wstring& path);

    void renameItem(const std::wstring& oldPath, const std::wstring& newPath);
    void removeMetadataSync(const std::wstring& path);

    void syncPhysicalMetadata(const std::wstring& path);

    std::string getFileIdSync(const std::wstring& path);

    bool hasPendingSync() const;

    QStringList getPendingSyncDirs();

    void removeFidsFromLog(const QStringList& fids);

    static std::wstring getVolumeSerialNumber(const std::wstring& path);

    void addToSyncLog(const std::wstring& dirPath);

    static bool fetchWinApiMetadataDirect(const std::wstring& path, std::string& outId128, std::wstring* outFrn = nullptr, long long* outSize = nullptr, std::wstring* outType = nullptr, long long* outCtime = nullptr, long long* outMtime = nullptr, long long* outAtime = nullptr);

signals:

    void metaChanged(const QString& path);

    void pendingSyncChanged(bool hasPending);

private:
    MetadataManager(QObject* parent = nullptr);
    ~MetadataManager() override = default;

    std::unordered_map<std::wstring, RuntimeMeta> m_cache;
    mutable std::shared_mutex m_mutex;

    QTimer* m_batchTimer = nullptr;
    std::unordered_set<std::wstring, std::hash<std::wstring>> m_dirtyPaths;

    void persistAsync(const std::wstring& path);
    void debouncePersist(const std::wstring& path);

    void saveSyncLog();
};

} 

#endif 
