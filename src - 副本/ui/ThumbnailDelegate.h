#pragma once

#include <QStyledItemDelegate>

namespace ArcMeta {

class ThumbnailDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ThumbnailDelegate(QObject* parent = nullptr);

    void setHasThumbnailRole(int role);
    void setRatingRole(int role);
    void setPathRole(int role);
    void setPinnedRole(int role);
    void setManagedRole(int role);
    void setTypeRole(int role);
    void setIsEmptyRole(int role);
    void setColorRole(int role);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override;

private:
    int m_hasThumbnailRole = Qt::UserRole + 1;
    int m_ratingRole = -1;
    int m_pathRole = -1;
    int m_pinnedRole = -1;
    int m_managedRole = -1;
    int m_typeRole = -1;
    int m_isEmptyRole = -1;
    int m_colorRole = -1;

    struct Metrics {
        QRect cardRect;
        QRect textRect;
        QRect banRect;
        int starsStartX;
        int starSize;
        int starSpacing;
        int ratingY;
        int ratingH;

        QRect starRect(int index) const {
            return QRect(starsStartX + index * (starSize + starSpacing), 
                         ratingY + (ratingH - starSize) / 2, 
                         starSize, starSize);
        }
    };
    Metrics calculateMetrics(const QStyleOptionViewItem& option) const;
};

} // namespace ArcMeta
