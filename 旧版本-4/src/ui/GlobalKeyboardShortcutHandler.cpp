#include "GlobalKeyboardShortcutHandler.h"
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include "IScanResultView.h"
#include "QuickLookWindow.h"
#include <QFileInfo>
#include <QApplication>

namespace FERREX {

static bool isPathPreviewable(const QString& path, const ScanConfig& config) {
    QFileInfo info(path);
    if (info.isDir()) return false;

    QString ext = info.suffix().toLower();
    if (config.previewBlacklist.contains(ext)) return false;
    return config.previewWhitelist.contains(ext);
}

GlobalKeyboardShortcutHandler::GlobalKeyboardShortcutHandler(ScanDialog* dialog)
    : QObject(dialog) {}

bool GlobalKeyboardShortcutHandler::handleKeyPress(QKeyEvent* event) {
    ScanDialog* dialog = qobject_cast<ScanDialog*>(parent());
    if (!dialog) return false;

    if (event->key() == Qt::Key_Escape) {
        bool searchNotEmpty = dialog->m_searchEdit && !dialog->m_searchEdit->text().isEmpty();
        bool extNotEmpty = dialog->m_extEdit && !dialog->m_extEdit->text().isEmpty();

        if (searchNotEmpty || extNotEmpty) {
            if (dialog->m_searchEdit) dialog->m_searchEdit->clear();
            if (dialog->m_extEdit) dialog->m_extEdit->clear();
            event->accept();
            return true;
        }
        
        dialog->reject();
        event->accept();
        return true;
    }

    if (event->key() == Qt::Key_Space) {
        if (!dialog->m_currentActiveView) return false;
        auto* view = dialog->m_currentActiveView->getBaseView();
        QModelIndex idx = view->currentIndex();
        if (idx.isValid()) {
            if (dialog->m_quickLook->isVisible()) {
                dialog->m_quickLook->closePreview();
            } else {
                QString path = dialog->m_tableModel->data(dialog->m_tableModel->index(idx.row(), 1)).toString();
                if (!isPathPreviewable(path, dialog->m_config)) return true;
                dialog->m_quickLook->preview(path);
            }
        }
        return true;
    }

    if (event->key() == Qt::Key_F2) {
        dialog->onRenameTriggered();
        return true;
    }

    if (event->key() == Qt::Key_F5) {
        dialog->onTriggerSearch();
        return true;
    }

    if (event->key() == Qt::Key_A && event->modifiers() == Qt::ControlModifier) { 
        dialog->selectAllResults();
        return true; 
    }

    if (event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier) {
        dialog->onCopyTriggered(false);
        return true;
    }

    if (event->key() == Qt::Key_X && event->modifiers() == Qt::ControlModifier) {
        dialog->onCopyTriggered(true);
        return true;
    }

    if (event->key() == Qt::Key_V && event->modifiers() == Qt::ControlModifier) {
        dialog->updateStatus("当前视图不支持粘贴");
        return true;
    }

    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if ((dialog->m_searchEdit && dialog->m_searchEdit->hasFocus()) || 
            (dialog->m_extEdit && dialog->m_extEdit->hasFocus())) {
            dialog->onTriggerSearch();
        } else {
            if (!dialog->m_currentActiveView) return false;
            auto view = dialog->m_currentActiveView->getBaseView();
            auto index = view->currentIndex();
            if (index.isValid()) dialog->onItemDoubleClicked(index);
        }
        return true;
    }

    return false;
}

} // namespace FERREX
