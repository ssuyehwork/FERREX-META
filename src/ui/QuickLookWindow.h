#pragma once

#include <QWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QPropertyAnimation>

namespace FERREX {

class QuickLookWindow : public QWidget {
    Q_OBJECT
public:
    explicit QuickLookWindow(QWidget* parent = nullptr);
    ~QuickLookWindow() override;

    void preview(const QString& filePath);
    void closePreview();

signals:
    void prevRequested();
    void nextRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void renderImage(const QString& path);
    void renderText(const QString& path);
    
    QString detectEncoding(const QByteArray& data);
    bool isBinary(const QByteArray& data);

    QLabel* m_imageLabel = nullptr;
    QPlainTextEdit* m_textEdit = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_infoLabel = nullptr;
    QWidget* m_container = nullptr;
    
    QString m_currentPath;
};

} // namespace FERREX
