#pragma once

#include <QObject>

namespace FERREX {

class ScanDialog;

class ThumbnailWarmupPipeline : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailWarmupPipeline(ScanDialog* dialog);
    void triggerWarmup();
};

} // namespace FERREX
