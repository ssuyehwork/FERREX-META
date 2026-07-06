#pragma once
#include <QSettings>
#include <QVariant>
#include <QString>
#include <QMutex>

namespace FERREX {

/**
 * @brief 工业级配置管理单例 (AppConfig)
 * 物理隔离 QSettings，解决身份分裂问题，统一组织与应用名称。
 */
class AppConfig {
public:
    static AppConfig& instance() {
        static AppConfig inst;
        return inst;
    }

    void setValue(const QString& key, const QVariant& value) {
        QMutexLocker lock(&m_mutex);
        m_settings.setValue(key, value);
    }

    QVariant getValue(const QString& key, const QVariant& defaultValue = QVariant()) const {
        QMutexLocker lock(&m_mutex);
        return m_settings.value(key, defaultValue);
    }

    void sync() {
        QMutexLocker lock(&m_mutex);
        m_settings.sync();
    }

private:
    AppConfig() : m_settings("FERREX团队", "FERREX") {}
    ~AppConfig() = default;
    AppConfig(const AppConfig&) = delete;
    AppConfig& operator=(const AppConfig&) = delete;

    mutable QSettings m_settings;
    mutable QMutex m_mutex;
};

} // namespace FERREX
