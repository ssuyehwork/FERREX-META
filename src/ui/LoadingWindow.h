#ifndef LOADINGWINDOW_H
#define LOADINGWINDOW_H

#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <QSvgRenderer>
#include <memory>

namespace ArcMeta {

/**
 * @brief 全量扫描期间显示的加载动画窗口
 * 2026-04-12 按照用户要求：在启动时显示动画进度，待初始化完成后关闭
 */
class LoadingWindow : public QWidget {
    Q_OBJECT

public:
    explicit LoadingWindow(QWidget* parent = nullptr);
    ~LoadingWindow() override = default;

signals:
    /**
     * @brief 窗口关闭完成时发射
     */
    void finished();

public slots:
    /**
     * @brief 更新进度文本（连接到 CoreController::statusTextChanged）
     */
    void updateStatus(const QString& text);

    /**
     * @brief 关闭加载窗口（连接到 CoreController::initializationFinished）
     */
    void onInitializationFinished();

protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onAnimationTimeout();

private:
    QLabel* m_statusLabel = nullptr;
    QTimer* m_animationTimer = nullptr;
    int m_rotationAngle = 0;
    std::unique_ptr<QSvgRenderer> m_svgRenderer;
};

} // namespace ArcMeta

#endif // LOADINGWINDOW_H
