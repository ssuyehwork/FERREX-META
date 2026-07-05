#pragma once

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QFrame>
#include <QStyle>
#include <vector>
#include <string>

namespace ArcMeta {

/**
 * @brief Tag Pill 圆角标签组件 (22px height, 11px radius)
 */
class TagPill : public QWidget {
    Q_OBJECT
public:
    explicit TagPill(const QString& text, QWidget* parent = nullptr);
signals:
    void deleteRequested(const QString& text);
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    QString m_text;
    QPushButton* m_closeBtn = nullptr;
};

/**
 * @brief 流式布局容器 (用于展示标签)
 */
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget *parent, int margin = -1, int hSpacing = -1, int vSpacing = -1);
    ~FlowLayout();
    void addItem(QLayoutItem *item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int) const override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QLayoutItem *takeAt(int index) override;
private:
    int doLayout(const QRect &rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;
    QList<QLayoutItem *> itemList;
    int m_hSpace;
    int m_vSpace;
};

/**
 * @brief 自定义星级打分器 (20x20px stars, 4px spacing)
 */
class StarRatingWidget : public QWidget {
    Q_OBJECT
public:
    explicit StarRatingWidget(QWidget* parent = nullptr);
    void setRating(int rating);
    int rating() const { return m_rating; }
signals:
    void ratingChanged(int rating);
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
private:
    int m_rating = 0;
};

/**
 * @brief 自定义颜色选择器 (18x18px dots, 6px spacing)
 */
class ColorPickerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ColorPickerWidget(QWidget* parent = nullptr);
    void setColor(const std::wstring& colorName);
    std::wstring color() const { return m_currentColor; }
signals:
    void colorChanged(const std::wstring& colorName);
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
private:
    std::wstring m_currentColor = L"";
    struct ColorEntry {
        std::wstring name;
        QColor value;
    };
    std::vector<ColorEntry> m_colors;
};

/**
 * @brief 元数据面板（面板五）
 */
class MetaPanel : public QFrame {
    Q_OBJECT
public:
    explicit MetaPanel(QWidget* parent = nullptr);
    ~MetaPanel() override = default;


    void updateInfo(const QString& name, const QString& type, const QString& size,
                    const QString& ctime, const QString& mtime, const QString& atime,
                    const QString& path, bool encrypted);

    /**
     * @brief 设置变长色板显示
     */
    void setPalettes(const QVector<QPair<QColor, float>>& palette);
    
signals:
    /**
     * @brief 元数据面板向上通知的信号
     * @param rating -1 表示未变，0..5 有效
     * @param color L"__NO_CHANGE__" 表示未变
     */
    void metadataChanged(int rating, const std::wstring& color);

    /**
     * @brief 根据颜色搜索项目
     */
    void searchByColor(const QColor& color);

public:
    /**
     * @brief 设置星级显示
     */
    void setRating(int rating);
    void setColor(const std::wstring& color);
    void setPinned(bool pinned);
    void setTags(const QStringList& tags);
    void setNote(const std::wstring& note);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void initUi();
    void addInfoRow(const QString& label, QLabel*& valueLabel);
    QFrame* createSeparator();
    
    /**
     * @brief 2026-04-12 物理还原：创建一个带图标、标题和边框的“小方盒”容器
     */
    QWidget* createSectionBox(const QString& iconName, const QString& title, QWidget* content);

    QVBoxLayout* m_mainLayout = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_container = nullptr;
    QVBoxLayout* m_containerLayout = nullptr;
    QLabel* lblName = nullptr, *lblType = nullptr, *lblSize = nullptr;
    QLabel* lblCtime = nullptr, *lblMtime = nullptr, *lblAtime = nullptr;
    QLabel* lblPath = nullptr, *lblEncrypted = nullptr;
    QWidget* m_paletteContainer = nullptr;
    QHBoxLayout* m_paletteLayout = nullptr;
    QWidget* m_tagContainer = nullptr;
    FlowLayout* m_tagFlowLayout = nullptr;
    QLineEdit* m_tagEdit = nullptr;
    QPlainTextEdit* m_noteEdit = nullptr;

private slots:
    void onTagAdded();
    void onTagDeleted(const QString& text);
};

} // namespace ArcMeta
