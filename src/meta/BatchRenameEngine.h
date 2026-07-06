#pragma once

#include <string>
#include <vector>
#include <QString>

namespace ArcMeta {

/**
 * @brief 批量重命名规则组件类型
 */
enum class RenameComponentType {
    Text,           // 固定文本
    Sequence,       // 序列数字
    Date,           // 日期 (yyyyMMdd)
    OriginalName,   // 原始文件名
    Metadata        // 元数据变量 (标签, 星级)
};

struct RenameRule {
    RenameComponentType type;
    QString value;      // 文本值、日期格式或元数据键名
    int start = 1;      // 序列起始
    int step = 1;       // 序列步长
    int padding = 3;    // 补零位数
};

/**
 * @brief 批量重命名引擎
 * 管道模式依次处理文件名，支持预检与冲突检测
 */
class BatchRenameEngine {
public:
    static BatchRenameEngine& instance();

    /**
     * @brief 执行预览计算
     * @param originalPaths 原始文件路径列表
     * @param rules 管道规则列表
     * @return 计算后的新名称列表
     */
    std::vector<std::wstring> preview(const std::vector<std::wstring>& originalPaths, const std::vector<RenameRule>& rules);

    /**
     * @brief 执行物理重命名与元数据迁移
     */
    bool execute(const std::vector<std::wstring>& originalPaths, const std::vector<RenameRule>& rules);

private:
    BatchRenameEngine() = default;
    ~BatchRenameEngine() = default;

    QString processOne(const std::wstring& path, int index, const std::vector<RenameRule>& rules);
};

} // namespace ArcMeta
