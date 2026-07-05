#pragma once

#include <QWidget>
#include <QColor>
#include <QImage>
#include <QLineEdit>
#include <QSlider>

namespace ArcMeta {

// --- SV (饱和度/亮度) 拾取区域 ---
class SvPicker : public QWidget {
    Q_OBJECT
public:
    explicit SvPicker(QWidget* parent = nullptr);
    void setHue(int h);
    void setSv(int s, int v);
signals:
    void svChanged(int s, int v);
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
private:
    void updateFromPos(const QPoint& pos);
    int m_h = 0, m_s = 255, m_v = 255;
    QImage m_bgCache;
    bool m_dirty = true;
};

// --- 色相 (Hue) 滑块 ---
class HueSlider : public QWidget {
    Q_OBJECT
public:
    explicit HueSlider(QWidget* parent = nullptr);
    void setHue(int h);
signals:
    void hueChanged(int h);
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
private:
    void updateFromPos(int y);
    int m_h = 0;
};

// --- 主面板 ---
class ColorPicker : public QWidget {
    Q_OBJECT
public:
    explicit ColorPicker(QWidget* parent = nullptr);
    QColor currentColor() const { return m_color; }
    // 2026-05-17 按照用户要求：新增 currentTolerance() 以暴露准确度滑条当前值
    int    currentTolerance() const;
    void setCurrentColor(const QColor& c);

signals:
    // 2026-05-17 按照用户要求：信号增加 tolerance 参数，让接收方直接获取准确度值
    void colorSelected(const QColor& c, int tolerance);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void updatePreview();
    void syncHsvFromColor();
    void updateColorFromHsv();

    QColor m_color = Qt::red;
    int m_h = 0, m_s = 255, m_v = 255;

    SvPicker*  m_svPicker;
    HueSlider* m_hueSlider;
    QWidget*   m_previewBlock;
    QLineEdit* m_hexEdit;
    QSlider*   m_toleranceSlider = nullptr; // 2026-05-17 按照用户要求：准确度（容差）滑条
};

} // namespace ArcMeta
