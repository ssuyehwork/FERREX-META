#pragma once

#include <QObject>
#include <QString>
#include <memory>

namespace FERREX {

class CoreController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isIndexing READ isIndexing NOTIFY isIndexingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    static CoreController& instance();

    void startSystem();

    bool isIndexing() const { return m_isIndexing; }
    QString statusText() const { return m_statusText; }

    QStringList performSearch(const QString& keyword);

signals:
    void isIndexingChanged(bool indexing);
    void statusTextChanged(const QString& text);
    void initializationFinished();

private:
    CoreController(QObject* parent = nullptr);
    ~CoreController() override;

    void setStatus(const QString& text, bool indexing);

    bool m_isIndexing = false;
    QString m_statusText = "就绪";
};

} 
