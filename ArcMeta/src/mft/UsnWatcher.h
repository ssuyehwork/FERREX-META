#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma once

#include <string>
#include <atomic>
#include <memory>
#include "MftReader.h"
#include <QString>
#include <QThread>
#include <QList>

namespace ArcMeta {

// 变动记录结构
struct UsnChange {
    enum Type { Created, Deleted, Renamed, Modified };
    Type type;
    uint64_t frn;
    uint64_t parentFrn;
    QString name;
    uint32_t attributes;
    int64_t size;
};

/**
 * @brief 高性能 USN 日志监控器
 * 
 * 实时监控 NTFS 卷的文件变动，支持离线追平逻辑。
 */
class UsnWatcher : public QThread {
    Q_OBJECT
public:
    // 构造函数接收 std::wstring 卷名和初始 USN
    explicit UsnWatcher(const std::wstring& volume, uint64_t startUsn = 0, QObject* parent = nullptr);
    virtual ~UsnWatcher();

    void stop();
    bool isStopped() const { return m_stopRequested.load(); }

protected:
    void run() override;

private:
    // 处理单条 USN 记录并更新 MftReader
    void handleRecord(USN_RECORD_V2* pRecord);

    std::wstring m_volume;
    uint64_t m_lastUsn;
    std::atomic<bool> m_stopRequested;
    HANDLE m_hVolume = INVALID_HANDLE_VALUE;
};

} // namespace ArcMeta
