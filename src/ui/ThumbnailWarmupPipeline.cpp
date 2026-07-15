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
    if (!dialog || !dialog->m_tableModel || !dialog->m_tableModel->getThumbPool()) return;

    QPointer<ScanDialog> weakThis(dialog);
    auto* pool = dialog->m_tableModel->getThumbPool();
    int maxThreads = pool->maxThreadCount();

    for (int t = 0; t < maxThreads; ++t) {
        pool->start([weakThis, t, maxThreads]() {
            if (!weakThis) return;

            static QThreadStorage<ScopedComInit> comStorage;
            if (!comStorage.hasLocalData()) {
                comStorage.setLocalData(ScopedComInit());
            }

            int total = MftReader::instance().totalCount();
            if (total > 0) {
                int maxWarm = std::min(total, 5000);
                int rangeSize = maxWarm / maxThreads;
                if (rangeSize <= 0) rangeSize = maxWarm;

                // 为每一个并发工作线程分配专属的区间
                int startIdx = t * rangeSize;
                int endIdx = (t == maxThreads - 1) ? maxWarm : (t + 1) * rangeSize;

                for (int i = startIdx; i < endIdx; ++i) {
                    if (!MftReader::instance().isDirectory(i)) {
                        QString ext = MftReader::instance().getExtQString(i);
                        if (UiHelper::isGraphicsFile(ext)) {
                            QString dummyPath = MftReader::instance().getFullPath(i);
                            if (!dummyPath.isEmpty()) {
                                UiHelper::getShellThumbnail(dummyPath, 64);
                            }
                            break; // 只提取本区间的第一个文件即宣告完成
                        }
                    }
                }
            }
        });
    }
}

} // namespace FERREX
