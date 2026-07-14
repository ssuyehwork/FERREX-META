#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <windows.h>
#include <winioctl.h>
#include <QString>
#include <QThread>
#include <QList>

namespace FERREX {

struct UsnChange {
    enum Type { Created, Deleted, Renamed, Modified };
    Type type;
    uint64_t frn;
    uint64_t parentFrn;
    QString name;
    uint32_t attributes;
    int64_t size;
};

class UsnWatcher : public QThread {
    Q_OBJECT
public:
    
    explicit UsnWatcher(const std::wstring& volume, uint64_t startUsn = 0, QObject* parent = nullptr);
    virtual ~UsnWatcher();

    void stop();

protected:
    void run() override;

private:
    
    void handleRecord(USN_RECORD_V2* pRecord);

    std::wstring m_volume;
    uint64_t m_lastUsn;
    std::atomic<bool> m_stopRequested;
    HANDLE m_hVolume = INVALID_HANDLE_VALUE;
};

} 
