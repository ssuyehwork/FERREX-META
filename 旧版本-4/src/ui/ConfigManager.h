#pragma once

#include <QString>
#include <QStringList>
#include <QSet>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>

namespace FERREX {

extern const QSet<QString> DEFAULT_BLACKLIST;
extern const QSet<QString> DEFAULT_WHITELIST;

struct ScanConfig {
    QSet<QString> activeDrives;
    QSet<QString> defaultDrives;
    QStringList queryHistory;
    QStringList extHistory;
    
    QSet<QString> previewBlacklist;
    QSet<QString> previewWhitelist;
    
    int viewMode = 0;   // 0: Details, 1: Icons
    int iconSize = 128; // 256, 128, 64
    int layoutMode = 0; // 0: JustifiedMode, 1: GridMode
    int sortColumn = 0; 
    int sortOrder = 0;  // 0: Asc, 1: Desc

    bool useRegex = true;
    bool caseSensitive = false;
    bool includeHidden = false;
    bool includeSystem = false;
    bool includeDollar = false;
    bool autoDisplay = false;

    void load();
    void save();
};

class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager inst;
        return inst;
    }

    ScanConfig& getConfig() { return m_config; }
    void load() { m_config.load(); }
    void save() { m_config.save(); }

private:
    ConfigManager() { load(); }
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    ScanConfig m_config;
};

} // namespace FERREX
