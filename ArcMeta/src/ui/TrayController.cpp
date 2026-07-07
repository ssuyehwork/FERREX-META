#include "TrayController.h"
#include <QApplication>
#include <QIcon>
#include <QDebug>
#include <QProgressDialog>
#include "../mft/MftReader.h"
#include "../meta/DatabaseManager.h"
#include "BatchProgressDialog.h"

namespace ArcMeta {

TrayController::TrayController(QMainWindow* mainWindow)
    : QObject(mainWindow), m_mainWindow(mainWindow) {
    m_trayIcon = new QSystemTrayIcon(this);
    
    // 2026-04-14 物理加固：锁定图标来源为 Qt 资源系统中的标准 ico
    m_trayIcon->setIcon(QIcon(":/app_icon.ico"));
    m_trayIcon->setToolTip("ArcMeta");

    m_trayMenu = new QMenu(mainWindow);
    m_trayMenu->setStyleSheet(
        "QMenu { background-color: #2D2D2D; color: #EEE; border: 1px solid #444; padding: 4px; border-radius: 8px; }"
        "QMenu::item { padding: 6px 25px 6px 10px; border-radius: 4px; font-size: 12px; }"
        "QMenu::item:selected { background-color: #3E3E42; color: white; }"
    );

    QAction* showAction = m_trayMenu->addAction("显示主界面");
    m_trayMenu->addSeparator();
    QAction* quitAction = m_trayMenu->addAction("退出 ArcMeta");

    connect(showAction, &QAction::triggered, this, &TrayController::onShowMainWindow);
    connect(quitAction, &QAction::triggered, this, &TrayController::onQuitApp);

    m_trayIcon->setContextMenu(m_trayMenu);

    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &TrayController::onTrayActivated);
}

TrayController::~TrayController() {
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
}

void TrayController::show() {
    m_trayIcon->show();
}

void TrayController::hide() {
    m_trayIcon->hide();
}

void TrayController::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        if (m_mainWindow->isVisible()) {
            m_mainWindow->hide();
        } else {
            onShowMainWindow();
        }
    }
}

void TrayController::onShowMainWindow() {
    m_mainWindow->showNormal();
    m_mainWindow->activateWindow();
}

void TrayController::onQuitApp() {
    // 2026-07-xx 按照用户要求 (Plan-119)：秒退出架构实现
    if (m_trayIcon) m_trayIcon->hide();

    // 1. 强制中断所有后台扫描任务
    MftReader::instance().clear(); 

    // 2. 调用 DatabaseManager 停用异步同步队列并释放句柄
    // 数据已在运行期通过增量任务实时落地，此处仅执行排空队列动作，通常为毫秒级。
    DatabaseManager::instance().shutdown();

    qDebug() << "[Exit] 增量数据同步完成，物理占用已释放。程序实现秒退出。";
    QApplication::quit();
}

} // namespace ArcMeta
