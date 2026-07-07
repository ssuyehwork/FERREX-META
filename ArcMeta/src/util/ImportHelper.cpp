#include "ImportHelper.h"
#include "../ui/Logger.h"
#include "../ui/BatchProgressDialog.h"
#include "../ui/ToolTipOverlay.h"
#include "../meta/MetadataManager.h"
#include "../meta/CategoryRepo.h"
#include "../meta/DatabaseManager.h"
#include "ShellHelper.h"
#include <QDir>
#include <QFileInfo>
#include <QtConcurrent>
#include <QMetaObject>
#include <QCoreApplication>
#include "FramelessDialog.h"
#include <QMutex>
#include <QFuture>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#include <objbase.h>
#endif

namespace ArcMeta {

void ImportHelper::importPaths(const QStringList& paths, const QString& targetPhysicalPath, QWidget* parent) {
    if (paths.isEmpty()) return;

    BatchProgressDialog* progress = new BatchProgressDialog("正在迁移项目至托管库...", parent);
    progress->show();

    struct ImportContext {
        std::atomic<bool> isCancelled{false};
        QFuture<void> future;
    };
    auto context = std::make_shared<ImportContext>();
    QPointer<BatchProgressDialog> weakProgress(progress);

    QObject::connect(progress, &BatchProgressDialog::rejected, [weakProgress, context, parent]() {
        if (!weakProgress) return;
        if (!FramelessMessageBox::question(parent, "中断迁移", "迁移尚未完成。确定要停止当前迁移任务吗？")) {
            weakProgress->show();
            return;
        }
        context->isCancelled = true;
        if (context->future.isRunning()) context->future.waitForFinished();
        weakProgress->deleteLater();
    });

    context->future = QtConcurrent::run([paths, targetPhysicalPath, weakProgress, context]() {
        // 2026-07-xx 按照 Plan-116：收拢为纯物理移动动作
        // 数据库入库动作完全依靠 USN Journal 异步感知。
        
        int total = paths.size();
        int handled = 0;

        for (const QString& src : paths) {
            if (context->isCancelled) break;
            
            handled++;
            if (weakProgress) {
                QMetaObject::invokeMethod(weakProgress.data(), "updateProgress", Qt::QueuedConnection, 
                                         Q_ARG(int, handled), Q_ARG(int, total), Q_ARG(QString, QFileInfo(src).fileName()));
            }

            // 执行物理移动
            ShellHelper::copyOrMoveItems({src}, targetPhysicalPath, true);
        }

        QMetaObject::invokeMethod(QCoreApplication::instance(), [weakProgress, context, handled]() {
            if (context->isCancelled) return;
            if (weakProgress) {
                weakProgress->accept();
                weakProgress->deleteLater();
            }
            ToolTipOverlay::instance()->showText(QCursor::pos(), 
                QString("已完成 %1 个项目的物理迁移，数据库将随后异步更新").arg(handled), 2000, QColor("#2ecc71"));
        });
    });
}

} // namespace ArcMeta
