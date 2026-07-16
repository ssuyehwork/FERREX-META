#include "ThumbnailWarmupPipeline.h"
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include "UiHelper.h"
#include "../mft/MftReader.h"
#include <QPointer>
#include <QThreadStorage>

namespace FERREX {

ThumbnailWarmupPipeline::ThumbnailWarmupPipeline(ScanDialog* dialog)
    : QObject(dialog) {}

void ThumbnailWarmupPipeline::triggerWarmup() {
    ScanDialog* dialog = qobject_cast<ScanDialog*>(parent());
    if (!dialog || !dialog->m_tableModel || !dialog->m_tableModel->getThumbPool()) {
        qWarning() << "[TRACE][triggerWarmup] 退出预热，条件不满足";
        return;
    }

    QPointer<ScanDialog> weakThis(dialog);
    auto* pool = dialog->m_tableModel->getThumbPool();
    int maxThreads = pool->maxThreadCount();

    qInfo() << "[TRACE][triggerWarmup] 物理启动预热流水线. 线程池最大并发数:" << maxThreads;

    for (int t = 0; t < maxThreads; ++t) {
        pool->start([weakThis, t]() {
            if (!weakThis) {
                qInfo() << "[TRACE][triggerWarmup][Thread-" << t << "] 目标 Dialog 已析构，退出线程";
                return;
            }

            qInfo() << "[TRACE][triggerWarmup][Thread-" << t << "] 线程套间初始化...";
            static QThreadStorage<ScopedComInit> comStorage;
            if (!comStorage.hasLocalData()) {
                comStorage.setLocalData(ScopedComInit());
            }

            int total = MftReader::instance().totalCount();
            qInfo() << "[TRACE][triggerWarmup][Thread-" << t << "] 数据库总量:" << total << " 准备扫描大图前5000条...";

            if (total > 0) {
                int warmupCount = 0;
                for (int i = 0; i < std::min(total, 5000); ++i) {
                    if (!MftReader::instance().isDirectory(i)) {
                        QString ext = MftReader::instance().getExtQString(i);
                        if (UiHelper::isGraphicsFile(ext)) {
                            QString dummyPath = MftReader::instance().getFullPath(i);
                            if (!dummyPath.isEmpty()) {
                                qInfo() << "[TRACE][triggerWarmup][Thread-" << t << "] 提取大图:" << dummyPath;
                                QElapsedTimer getTimer;
                                getTimer.start();
                                
                                UiHelper::getShellThumbnail(dummyPath, 64);
                                
                                if (getTimer.elapsed() > 100) {
                                    qWarning() << "[TRACE][triggerWarmup][Thread-" << t << "] GetImage 超时！耗时:" << getTimer.elapsed() << "ms 对于文件:" << dummyPath;
                                }
                                warmupCount++;
                            }
                            break;
                        }
                    }
                }
                qInfo() << "[TRACE][triggerWarmup][Thread-" << t << "] 线程预热结束，处理文件数:" << warmupCount;
            }
        });
    }
}

} // namespace FERREX
