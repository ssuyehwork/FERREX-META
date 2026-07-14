#include "TrayController.h"
#include <QApplication>
#include <QIcon>
#include <QDebug>
#include "../mft/MftReader.h"

namespace FERREX {

TrayController::TrayController(QWidget* mainWindow)
    : QObject(mainWindow), m_mainWindow(mainWindow) {
    m_trayIcon = new QSystemTrayIcon(this);

    m_trayIcon->setIcon(QIcon(":/app_icon.ico"));
    m_trayIcon->setToolTip("FERREX");

    m_trayMenu = new QMenu(mainWindow);
    m_trayMenu->setStyleSheet(
        "QMenu { background-color: #2D2D2D; color: #EEE; border: 1px solid #444; padding: 4px; border-radius: 8px; }"
        "QMenu::item { padding: 6px 25px 6px 10px; border-radius: 4px; font-size: 12px; }"
        "QMenu::item:selected { background-color: #3E3E42; color: white; }"
    );

    QAction* showAction = m_trayMenu->addAction("显示主界面");
    m_trayMenu->addSeparator();
    QAction* quitAction = m_trayMenu->addAction("退出 FERREX");

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
    
    if (m_trayIcon) m_trayIcon->hide();
    MftReader::instance().clear(); 
    QApplication::quit();
}

} 
