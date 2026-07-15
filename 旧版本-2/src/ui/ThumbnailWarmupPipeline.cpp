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
        pool->start([weakThis]() {
            if (!weakThis) return;

            static QThreadStorage<ScopedComInit> comStorage;
            if (!comStorage.hasLocalData()) {
                comStorage.setLocalData(ScopedComInit());
            }

            int total = MftReader::instance().totalCount();
            if (total > 0) {
                for (int i = 0; i < std::min(total, 5000); ++i) {
                    if (!MftReader::instance().isDirectory(i)) {
                        QString ext = MftReader::instance().getExtQString(i);
                        if (UiHelper::isGraphicsFile(ext)) {
                            QString dummyPath = MftReader::instance().getFullPath(i);
                            if (!dummyPath.isEmpty()) {
                                UiHelper::getShellThumbnail(dummyPath, 64);
                            }
                            break;
                        }
                    }
                }
            }
        });
    }
}

} // namespace FERREX
