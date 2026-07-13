#include "ContextMenuExecutor.h"
#include "ScanDialog.h"
#include "ScanTableModel.h"
#include "ScanController.h"
#include "IScanResultView.h"
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QProcess>
#include <QDir>
#include <QMessageBox>
#include <QFile>
#include <QTableView>
#include <QHeaderView>
#include <windows.h>
#include <shellapi.h>

namespace FERREX {

ContextMenuExecutor::ContextMenuExecutor(ScanDialog* dialog)
    : QObject(dialog) {}

void ContextMenuExecutor::executeContextMenu(const QPoint& pos) {
    ScanDialog* dialog = qobject_cast<ScanDialog*>(parent());
    if (!dialog || !dialog->m_currentActiveView) return;

    QAbstractItemView* activeView = dialog->m_currentActiveView->getBaseView();
    
    QModelIndex indexAtPos = activeView->indexAt(pos);
    QModelIndexList selectedRows;

    if (indexAtPos.isValid()) {
        if (!activeView->selectionModel()->isSelected(indexAtPos)) {
            activeView->selectionModel()->select(indexAtPos, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        
        auto allSelected = activeView->selectionModel()->selectedIndexes();
        for (const auto& idx : allSelected) {
            if (idx.column() == 0) selectedRows.append(idx);
        }
    } else {
        selectedRows.clear();
    }
    
    QMenu menu(dialog);
    menu.setStyleSheet("QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; } QMenu::item:selected { background: #232D37; color: #FFF; }");

    if (!selectedRows.isEmpty()) {
        int count = selectedRows.size();
        menu.addAction(count > 1 ? "批量打开文件" : "打开文件", [dialog, selectedRows]() {
            for (const auto& index : selectedRows) dialog->onItemDoubleClicked(index);
        });
        
        menu.addAction("在“资源管理器”中显示", [dialog, selectedRows]() {
            QString path = dialog->m_tableModel->data(dialog->m_tableModel->index(selectedRows.first().row(), 1)).toString();
            QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(path)});
        });
        
        menu.addSeparator();
        
        menu.addAction(count > 1 ? "批量复制路径" : "复制路径", [dialog, selectedRows]() {
            QStringList paths;
            for (const auto& idx : selectedRows) paths << dialog->m_tableModel->data(dialog->m_tableModel->index(idx.row(), 1)).toString();
            QApplication::clipboard()->setText(paths.join("\n"));
        });
        
        menu.addAction(count > 1 ? "批量复制文件名" : "复制文件名", [dialog, selectedRows]() {
            QStringList names;
            for (const auto& idx : selectedRows) names << dialog->m_tableModel->data(dialog->m_tableModel->index(idx.row(), 0)).toString();
            QApplication::clipboard()->setText(names.join("\n"));
        });

        menu.addSeparator();
        menu.addAction("剪切", dialog, [dialog]() { dialog->onCopyTriggered(true); });
        menu.addAction("复制", dialog, [dialog]() { dialog->onCopyTriggered(false); });
        
        if (count == 1) {
            menu.addAction("重命名", dialog, [dialog]() {
                QTimer::singleShot(0, dialog, &ScanDialog::onRenameTriggered);
            });
        }
        
        menu.addSeparator();
        
        menu.addAction(count > 1 ? "批量删除" : "删除", [dialog, selectedRows]() {
            QString msg = (selectedRows.size() == 1) ? QString("确定要永久删除 %1 吗？").arg(dialog->m_tableModel->data(dialog->m_tableModel->index(selectedRows.first().row(), 0)).toString())
                                                   : QString("确定要永久删除选中的 %1 个项目吗？").arg(selectedRows.size());
            if (QMessageBox::question(dialog, "确认删除", msg) == QMessageBox::Yes) {
                for (const auto& idx : selectedRows) {
                    QString path = dialog->m_tableModel->data(dialog->m_tableModel->index(idx.row(), 1)).toString();
                    QFile::remove(path);
                }
                dialog->m_controller->triggerSearch(true);
            }
        });
        
        menu.addSeparator();
        
        menu.addAction("属性", [dialog, selectedRows]() {
            QString path = dialog->m_tableModel->data(dialog->m_tableModel->index(selectedRows.first().row(), 1)).toString();
            std::wstring wpath = path.toStdWString();
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_INVOKEIDLIST;
            sei.lpVerb = L"properties";
            sei.lpFile = wpath.c_str();
            sei.nShow = SW_SHOW;
            ShellExecuteExW(&sei);
        });

        menu.addSeparator();
    }

    QMenu* viewMenu = menu.addMenu("视图(V)");

    if (dialog->m_actJMode && dialog->m_actGMode && dialog->m_actListMode) {
        dialog->m_actJMode->setChecked(dialog->m_config.viewMode == 1 && dialog->m_config.layoutMode == 0);
        dialog->m_actGMode->setChecked(dialog->m_config.viewMode == 1 && dialog->m_config.layoutMode == 1);
        dialog->m_actListMode->setChecked(dialog->m_config.viewMode == 0);

        viewMenu->addAction(dialog->m_actJMode);
        viewMenu->addAction(dialog->m_actGMode);
        viewMenu->addAction(dialog->m_actListMode);
    }
    
    QMenu* sortMenu = menu.addMenu("排序(S)");
    QStringList sortOptions = {"名称", "路径", "大小", "修改日期"};
    for (int i = 0; i < sortOptions.size(); ++i) {
        QAction* act = sortMenu->addAction(sortOptions[i]);
        connect(act, &QAction::triggered, dialog, [dialog, i]() {
            auto* resultTableView = dialog->m_listResultView ? qobject_cast<QTableView*>(dialog->m_listResultView->getBaseView()) : nullptr;
            if (resultTableView) {
                Qt::SortOrder order = resultTableView->horizontalHeader()->sortIndicatorOrder();
                resultTableView->sortByColumn(i, order);
                dialog->m_config.sortColumn = i;
                dialog->m_config.sortOrder = static_cast<int>(order);
                dialog->m_config.save();
            }
        });
    }
    sortMenu->addSeparator();
    QAction* ascAction = sortMenu->addAction("升序(A)");
    QAction* descAction = sortMenu->addAction("降序(D)");
    connect(ascAction, &QAction::triggered, dialog, [dialog]() { 
        auto* resultTableView = dialog->m_listResultView ? qobject_cast<QTableView*>(dialog->m_listResultView->getBaseView()) : nullptr;
        if (resultTableView) {
            resultTableView->sortByColumn(resultTableView->horizontalHeader()->sortIndicatorSection(), Qt::AscendingOrder); 
            dialog->m_config.sortOrder = 0;
            dialog->m_config.save();
        }
    });
    connect(descAction, &QAction::triggered, dialog, [dialog]() { 
        auto* resultTableView = dialog->m_listResultView ? qobject_cast<QTableView*>(dialog->m_listResultView->getBaseView()) : nullptr;
        if (resultTableView) {
            resultTableView->sortByColumn(resultTableView->horizontalHeader()->sortIndicatorSection(), Qt::DescendingOrder); 
            dialog->m_config.sortOrder = 1;
            dialog->m_config.save();
        }
    });

    QAction* refreshAction = menu.addAction("刷新(R)");
    refreshAction->setShortcut(QKeySequence(Qt::Key_F5));
    connect(refreshAction, &QAction::triggered, dialog, &ScanDialog::onTriggerSearch);

    // 修复：不使用 sender() 查找视图，防止解耦后返回空指针；直接使用已获取并校验过的活动视图 activeView
    menu.exec(activeView->viewport()->mapToGlobal(pos));
}

} // namespace FERREX
