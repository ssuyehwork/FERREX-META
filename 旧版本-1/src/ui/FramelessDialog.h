#ifndef FRAMELESSDIALOG_H
#define FRAMELESSDIALOG_H

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QPoint>
#include <QMouseEvent>
#include <QKeyEvent>

namespace FERREX {

/**
 * @brief 无边框对话框基类，自带标题栏、关闭按钮（扁平化设计）
 * 适配 FERREX 风格，参考旧版 RapidNotes 基因实现
 */
class FramelessDialog : public QDialog {
    Q_OBJECT
public:
    enum ResizeDir {
        DIR_NONE = 0,
        DIR_TOP,
        DIR_BOTTOM,
        DIR_LEFT,
        DIR_RIGHT,
        DIR_TOPLEFT,
        DIR_TOPRIGHT,
        DIR_BOTTOMLEFT,
        DIR_BOTTOMRIGHT
    };

    explicit FramelessDialog(const QString& title, QWidget* parent = nullptr);
    virtual ~FramelessDialog() = default;

    QWidget* getContentArea() const { return m_contentArea; }

protected:
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override; // 追加双击虚函数重写声明
    void changeEvent(QEvent* event) override;               // 追加窗口改变虚函数重写声明
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

    ResizeDir getResizeDir(const QPoint& pos) const;
    void updateCursorShape(ResizeDir dir);

    QWidget* m_contentArea;
    QVBoxLayout* m_mainLayout;
    QVBoxLayout* m_outerLayout;
    QWidget* m_container;
    QLabel* m_titleLabel;
    QPushButton* m_pinBtn;
    QPushButton* m_minBtn;
    QPushButton* m_maxBtn;
    QPushButton* m_closeBtn;

private:
    QPoint m_dragPos;
    bool m_isDragging = false;

    ResizeDir m_resizeDir = DIR_NONE;
    QPoint m_startGlobalPos;
    QRect m_startGeometry;
    static const int PADDING = 6;
};

/**
 * @brief 无边框文本输入对话框
 */
class FramelessInputDialog : public FramelessDialog {
    Q_OBJECT
public:
    explicit FramelessInputDialog(const QString& title, const QString& label, 
                                  const QString& initial = "", QWidget* parent = nullptr);
    QString text() const { return m_edit->text().trimmed(); }

protected:
    void showEvent(QShowEvent* event) override;

private:
    QLineEdit* m_edit;
};

} // namespace FERREX

#endif // FRAMELESSDIALOG_H
