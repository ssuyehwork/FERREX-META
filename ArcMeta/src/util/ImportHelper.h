#pragma once

#include <QStringList>
#include <QWidget>

namespace ArcMeta {

/**
 * @brief 导入中枢模块 (ImportHelper)
 * 2026-07-xx 按照用户要求 (1.19)：归一化“扫描入库”与“拖拽导入”的行为逻辑。
 */
class ImportHelper {
public:
    /**
     * @brief 执行物理迁移流程
     * 2026-07-xx 按照 Plan-116：收拢为仅物理 Move 动作。
     * @param paths 待迁移的物理路径列表
     * @param targetPhysicalPath 目标物理目录（托管库中的具体文件夹）
     * @param parent UI 父窗口
     */
    static void importPaths(const QStringList& paths, const QString& targetPhysicalPath, QWidget* parent = nullptr);
};

} // namespace ArcMeta
