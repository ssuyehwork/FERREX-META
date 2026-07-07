#pragma once

#include <QAbstractTableModel>
#include <QVariant>
#include <vector>
#include <QString>
#include <QDateTime>

namespace ArcMeta {

struct InvalidItem {
    QString path;
    QString fileName;
    QString originalPath;
    long long invalidAt;
    QString type;
    long long size;
    bool checked = false;
};

class InvalidDataModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        Check = 0,
        FileName,
        OriginalPath,
        InvalidTime,
        Type,
        Size,
        ColumnCount
    };

    explicit InvalidDataModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_records.size());
    }

    int columnCount(const QModelIndex& = QModelIndex()) const override {
        return ColumnCount;
    }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.row() >= static_cast<int>(m_records.size())) return QVariant();

        const auto& item = m_records[index.row()];
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            switch (index.column()) {
                case FileName: return item.fileName;
                case OriginalPath: return item.originalPath;
                case InvalidTime: return QDateTime::fromMSecsSinceEpoch(item.invalidAt).toString("yyyy-MM-dd HH:mm:ss");
                case Type: return item.type;
                case Size: {
                    if (item.size < 1024) return QString::number(item.size) + " B";
                    if (item.size < 1024 * 1024) return QString::number(item.size / 1024.0, 'f', 1) + " KB";
                    return QString::number(item.size / (1024.0 * 1024.0), 'f', 1) + " MB";
                }
                default: return QVariant();
            }
        } else if (role == Qt::CheckStateRole && index.column() == Check) {
            return item.checked ? Qt::Checked : Qt::Unchecked;
        } else if (role == Qt::TextAlignmentRole) {
            if (index.column() == Check) return QVariant::fromValue(Qt::AlignCenter);
            return QVariant::fromValue(Qt::Alignment(Qt::AlignLeft | Qt::AlignVCenter));
        }
        return QVariant();
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override {
        if (index.isValid() && role == Qt::CheckStateRole && index.column() == Check) {
            m_records[index.row()].checked = (value.toInt() == Qt::Checked);
            emit dataChanged(index, index, {Qt::CheckStateRole});
            return true;
        }
        return false;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            switch (section) {
                case Check: return "";
                case FileName: return "文件名";
                case OriginalPath: return "原始路径";
                case InvalidTime: return "失效时间";
                case Type: return "类型";
                case Size: return "大小";
            }
        }
        return QVariant();
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (index.column() == Check) f |= Qt::ItemIsUserCheckable;
        return f;
    }

    void setRecords(const std::vector<InvalidItem>& records) {
        beginResetModel();
        m_records = records;
        endResetModel();
    }

    void setAllChecked(bool checked) {
        if (m_records.empty()) return;
        for (auto& item : m_records) item.checked = checked;
        emit dataChanged(index(0, Check), index(static_cast<int>(m_records.size()) - 1, Check), {Qt::CheckStateRole});
    }

    QStringList getCheckedPaths() const {
        QStringList paths;
        for (const auto& item : m_records) {
            if (item.checked) paths << item.path;
        }
        return paths;
    }

private:
    std::vector<InvalidItem> m_records;
};

} // namespace ArcMeta
