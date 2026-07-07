#pragma once

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QCheckBox>
#include <QTextEdit>
#include <QLineEdit>
#include <QFrame>
#include <QStyle>
#include <vector>
#include <string>

namespace ArcMeta {

/**
 * @brief ElasticEdit: 弹性高度编辑框，内容自动撑开高度
 * 2026-06-xx 工业级重构：基类切换为 QTextEdit 以获得精确的像素级渲染高度反馈
 */
class ElasticEdit : public QTextEdit {
    Q_OBJECT
public:
    explicit ElasticEdit(QWidget* parent = nullptr);
    void adjustHeight();
signals:
    void returnPressed(); // 统一信号接口
protected:
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
};

/**
 * @brief Tag Pill 圆角标签组件 (22px height, 11px radius)
 */
class TagPill : public QWidget {
    Q_OBJECT
public:
    explicit TagPill(const QString& text, QWidget* parent = nullptr);
    void setData(const QString& text);
signals:
    void deleteRequested(const QString& text);
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    QString m_text;
    QLabel* m_label = nullptr;
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
 * @brief ColorPill: 用于流式展示的单个颜色块 (16x16px, 4px 圆角)
 */
class ColorPill : public QWidget {
    Q_OBJECT
public:
    explicit ColorPill(const QColor& color, float ratio, QWidget* parent = nullptr);
    void setData(const QColor& color, float ratio);
signals:
    void colorSelected(const QColor& color);
    void requestSetAsPrimary(const QColor& color);
protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
private:
    QColor m_color;
    float m_ratio;
    bool m_hovered = false;
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
     * @brief 设置当前选中的路径列表，用于多选批量操作
     */
    void setSelectedPaths(const QStringList& paths) { m_selectedPaths = paths; }

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

    /**
     * @brief 标签变更信号 (支持批量更新)
     */
    void tagsChanged(const QStringList& tags);

public:
    /**
     * @brief 设置星级显示
     */
    void setRating(int rating);
    void setColor(const std::wstring& color);
    void setPinned(bool pinned);
    void setTags(const QStringList& tags);
    void setNote(const std::wstring& note);
    void setURL(const std::wstring& url);
    void setCategory(const QString& category);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void initUi();
    void adjustFlowHeights();
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
    
    ElasticEdit* m_nameEdit = nullptr;
    QLabel* lblType = nullptr, *lblSize = nullptr;
    QLabel* lblCtime = nullptr, *lblMtime = nullptr, *lblAtime = nullptr;
    ElasticEdit* m_pathEdit = nullptr;
    QLabel* lblEncrypted = nullptr;
    
    QWidget* m_paletteBox = nullptr;
    FlowLayout* m_paletteFlowLayout = nullptr;
    
    QWidget* m_tagBox = nullptr;
    QWidget* m_tagContainer = nullptr;
    FlowLayout* m_tagFlowLayout = nullptr;
    ElasticEdit* m_tagEdit = nullptr;
    
    ElasticEdit* m_noteEdit = nullptr;
    ElasticEdit* m_linkEdit = nullptr;
    
    ElasticEdit* m_categoryEdit = nullptr;

    QStringList m_selectedPaths;

    // 2026-06-xx 性能优化：控件复用池
    QList<TagPill*> m_tagPool;
    QList<ColorPill*> m_colorPool;
    QTimer* m_adjustTimer = nullptr;

private slots:
    void onTagAdded();
    void onTagDeleted(const QString& text);
    void setAsPrimaryColor(const QColor& color);
};

} // namespace ArcMeta
