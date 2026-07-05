#pragma once

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QPlainTextEdit>

namespace ArcMeta {

/**
 * @brief 专业快速预览窗口
 * 硬件加速图片预览、大文件 Markdown 内存映射极速加载
 */
class QuickLookWindow : public QWidget {
    Q_OBJECT

public:
    static QuickLookWindow& instance();

    /**
     * @brief 预览指定文件
     * @param path 文件路径
     */
    void previewFile(const QString& path);

signals:
    /**
     * @brief 用户按 1-5 键设置星级时发出
     */
    void ratingRequested(int rating);
    void colorRequested(const QString& color);
    void prevRequested();
    void nextRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    QuickLookWindow();
    ~QuickLookWindow() override = default;

    void initUi();
    void renderImage(const QString& path);
    void renderProfessionalImage(const QString& path);
    void renderText(const QString& path);

    QVBoxLayout* m_mainLayout = nullptr;
    
    // 图片预览组件（QGraphicsView 硬件加速）
    QGraphicsView* m_graphicsView = nullptr;
    QGraphicsScene* m_scene = nullptr;
    
    // 文本预览组件 (Markdown / Text)
    QPlainTextEdit* m_textPreview = nullptr;
};

} // namespace ArcMeta
