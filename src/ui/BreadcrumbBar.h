#pragma once

#include <QWidget>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStringList>

namespace ArcMeta {

/**
 * @brief 面包屑导航栏部件
 * 将路径拆分为层级按钮，支持点击跳转
 */
class BreadcrumbBar : public QWidget {
    Q_OBJECT

public:
    explicit BreadcrumbBar(QWidget* parent = nullptr);
    ~BreadcrumbBar() override = default;

    /**
     * @brief 设置当前显示路径并刷新按钮
     */
    void setPath(const QString& path);

signals:
    /**
     * @brief 用户点击某个层级按钮时发出
     * @param path 该层级对应的完整物理路径
     */
    void pathClicked(const QString& path);

    /**
     * @brief 当用户点击空白区域时发出，用于告知外部切换到编辑模式
     */
    void blankAreaClicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void clearButtons();
    void addLevel(const QString& name, const QString& fullPath);

    QHBoxLayout* m_layout = nullptr;
    QString m_currentPath;
};

} // namespace ArcMeta
