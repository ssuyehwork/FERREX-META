#pragma once

#include <QObject>
#include <QString>
#include <memory>

namespace ArcMeta {

/**
 * @brief 核心中控类
 * 负责协调底层服务初始化、管理系统全局状态、并为 UI 提供异步通知接口。
 */
class CoreController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isIndexing READ isIndexing NOTIFY isIndexingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

public:
    static CoreController& instance();

    /**
     * @brief 启动异步初始化序列
     */
    void startSystem();

    bool isIndexing() const { return m_isIndexing; }
    QString statusText() const { return m_statusText; }

    /**
     * @brief 统一搜索接口
     * @param keyword 关键词
     * @return 匹配的文件路径列表
     */
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

} // namespace ArcMeta
