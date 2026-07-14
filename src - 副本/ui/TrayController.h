#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QWidget>

namespace FERREX {

class TrayController : public QObject {
    Q_OBJECT

public:
    explicit TrayController(QWidget* mainWindow);
    ~TrayController() override;

    void show();
    void hide();

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void onShowMainWindow();
    void onQuitApp();

private:
    QWidget* m_mainWindow;
    QSystemTrayIcon* m_trayIcon;
    QMenu* m_trayMenu;
};

} 
