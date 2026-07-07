#ifndef FRAMELESSFILEDIALOG_H
#define FRAMELESSFILEDIALOG_H

#include "FramelessDialog.h"
#include <QFileSystemModel>
#include <QListView>
#include <QTreeView>
#include <QListWidget>
#include <QComboBox>
#include <QLineEdit>

namespace ArcMeta {

class FramelessFileDialog : public FramelessDialog {
    Q_OBJECT
public:
    enum FileMode { ExistingFile, Directory, AnyFile };

    explicit FramelessFileDialog(const QString& title, const QString& dir = "", 
                                 FileMode mode = ExistingFile, const QString& filter = "", 
                                 QWidget* parent = nullptr);

    static QString getExistingDirectory(QWidget* parent, const QString& caption = "", const QString& dir = "");
    static QString getOpenFileName(QWidget* parent, const QString& caption = "", const QString& dir = "", const QString& filter = "");
    static QString getSaveFileName(QWidget* parent, const QString& caption = "", const QString& dir = "", const QString& filter = "");

    QString selectedPath() const { return m_selectedPath; }

private slots:
    void onSidebarClicked(QListWidgetItem* item);
    void onFileViewDoubleClicked(const QModelIndex& index);
    void onSelectionChanged(const QModelIndex& current, const QModelIndex& previous);
    void onAccept();
    void onPathEntered();

private:
    void setupSidebar();
    void updatePath(const QString& path);

    QFileSystemModel* m_model;
    QListView* m_listView;
    QListWidget* m_sidebar;
    QLineEdit* m_pathEdit;
    QLineEdit* m_fileEdit;
    QComboBox* m_filterCombo;
    QPushButton* m_okBtn;

    FileMode m_mode;
    QString m_selectedPath;
};

} // namespace ArcMeta

#endif // FRAMELESSFILEDIALOG_H
