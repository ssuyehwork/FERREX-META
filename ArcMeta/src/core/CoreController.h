#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <atomic>

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
     * @brief 统一搜索接口 (2026-07-xx 按照 Plan-57 升级为异步模式)
     * @param keyword 关键词
     * @param scopeSource 范围来源 ("category" 或 "nav")
     * @param categoryId 分类 ID (当 scopeSource 为 "category" 时有效)
     * @param parentPath 物理路径 (当 scopeSource 为 "nav" 时有效)
     */
    void performSearch(const QString& keyword, const QString& scopeSource = "", int categoryId = 0, const QString& parentPath = "");

    /**
     * @brief 中止当前正在进行的搜索任务
     */
    void abortSearch();

    /**
     * @brief 响应硬件变更 (Plan-131 方案 E)
     */
    void handleDeviceChange(unsigned long wParam, unsigned long long lParam);

signals:
    /**
     * @brief 搜索结果流式返回
     * @param results 新发现的路径列表
     * @param isIncremental 是否为增量结果
     */
    void searchResultsAvailable(const QStringList& results, bool isIncremental);
    void searchStarted();
    void searchFinished(int totalFound);

    void isIndexingChanged(bool indexing);
    void statusTextChanged(const QString& text);
    void initializationFinished();

private:
    CoreController(QObject* parent = nullptr);
    ~CoreController() override;

    void setStatus(const QString& text, bool indexing);

    bool m_isIndexing = false;
    QString m_statusText = "就绪";
    
    // 2026-07-xx 按照 Plan-57：搜索状态管理
    std::atomic<bool> m_isSearchAborted{false};
    std::atomic<bool> m_isSearching{false};
    std::atomic<int> m_currentSearchId{0}; // 物理搜索 ID：用于识别并中止过期的异步扫描任务
};

} // namespace ArcMeta
