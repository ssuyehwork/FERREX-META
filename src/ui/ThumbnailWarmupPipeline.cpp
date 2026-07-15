#include "ThumbnailWarmupPipeline.h"
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include "UiHelper.h"
#include "ThumbnailManager.h"
#include "../mft/MftReader.h"
#include <QPointer>
#include <QThreadStorage>

using namespace FERREX;

namespace FERREX {

ThumbnailWarmupPipeline::ThumbnailWarmupPipeline(ScanDialog* dialog)
    : QObject(dialog) {}

void ThumbnailWarmupPipeline::triggerWarmup() {
    ScanDialog* dialog = qobject_cast<ScanDialog*>(parent());
    if (!dialog || !dialog->m_tableModel) return;

    QPointer<ScanDialog> weakThis(dialog);
    auto* pool = ThumbnailManager::instance().getPool();
    int maxThreads = pool->maxThreadCount();

    int total = MftReader::instance().totalCount();
    if (total <= 0) return;

    int step = std::max<int>(1, total / maxThreads);

    for (int t = 0; t < maxThreads; ++t) {
        int startRange = t * step;
        int endRange = (t == maxThreads - 1) ? total : (t + 1) * step;

        pool->start([weakThis, startRange, endRange]() {
            if (!weakThis) return;

            static QThreadStorage<ScopedComInit> comStorage;
            if (!comStorage.hasLocalData()) {
                comStorage.setLocalData(ScopedComInit());
            }

            // 每个线程在分配的独立区间内进行预热
            for (int i = startRange; i < std::min(endRange, startRange + 2500); ++i) {
                if (!MftReader::instance().isDirectory(i)) {
                    QString ext = MftReader::instance().getExtQString(i);
                    if (UiHelper::isGraphicsFile(ext)) {
                        QString dummyPath = MftReader::instance().getFullPath(i);
                        if (!dummyPath.isEmpty()) {
                            UiHelper::getShellThumbnail(dummyPath, 64);
                        }
                    }
                }
            }
        });
    }
}

} // namespace FERREX
