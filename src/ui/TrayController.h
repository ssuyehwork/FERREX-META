#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QWidget>

namespace FERREX {

/**
 * @brief 系统托盘控制器
 * 专门管理托盘图标、菜单及其相关的显示/退出动作
 */
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

} // namespace FERREX
