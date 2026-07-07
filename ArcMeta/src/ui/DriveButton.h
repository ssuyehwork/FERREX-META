#pragma once
#include <QPushButton>
#include <QTimer>
#include "StyleLibrary.h"

namespace ArcMeta {

class DriveButton : public QPushButton {
    Q_OBJECT
public:
    enum State {
        Inactive, // 状态A：灰静 (未激活/无托管库)
        Active,   // 状态B：蓝静 (已激活/任务空闲)
        Running,  // 状态C：蓝转 (任务执行中)
        Paused    // 状态D：灰暂 (任务暂停)
    };

    explicit DriveButton(const QString& driveLetter, QWidget* parent = nullptr);
    
    void setState(State state);
    State state() const { return m_state; }
    QString driveLetter() const { return m_driveLetter; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    void updateAnimation();

private:
    QString m_driveLetter;
    State m_state = Inactive;
    int m_rotationAngle = 0;
    QTimer* m_animationTimer = nullptr;
    
    void startRotation();
    void stopRotation();
};

} // namespace ArcMeta
