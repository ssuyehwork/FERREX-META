#include "BatchRenameEngine.h"
#include "MetadataManager.h"
#include <QFileInfo>
#include <QDateTime>
#include <filesystem>

namespace ArcMeta {

BatchRenameEngine& BatchRenameEngine::instance() {
    static BatchRenameEngine inst;
    return inst;
}

std::vector<std::wstring> BatchRenameEngine::preview(const std::vector<std::wstring>& originalPaths, const std::vector<RenameRule>& rules) {
    std::vector<std::wstring> results;
    for (int i = 0; i < (int)originalPaths.size(); ++i) {
        results.push_back(processOne(originalPaths[i], i, rules).toStdWString());
    }
    return results;
}

QString BatchRenameEngine::processOne(const std::wstring& path, int index, const std::vector<RenameRule>& rules) {
    QFileInfo info(QString::fromStdWString(path));
    QString newName = "";

    for (const auto& rule : rules) {
        switch (rule.type) {
            case RenameComponentType::Text:
                newName += rule.value;
                break;
            case RenameComponentType::Sequence: {
                int val = rule.start + (index * rule.step);
                newName += QString::number(val).rightJustified(rule.padding, '0');
                break;
            }
            case RenameComponentType::Date:
                newName += QDateTime::currentDateTime().toString(rule.value.isEmpty() ? "yyyyMMdd" : rule.value);
                break;
            case RenameComponentType::OriginalName:
                newName += info.baseName();
                break;
            case RenameComponentType::Metadata:
                newName += "[ArcMeta]"; 
                break;
        }
    }

    QString ext = info.suffix();
    if (!ext.isEmpty()) newName += "." + ext;
    return newName;
}

bool BatchRenameEngine::execute(const std::vector<std::wstring>& originalPaths, const std::vector<RenameRule>& rules) {
    auto newNames = preview(originalPaths, rules);
    for (int i = 0; i < (int)originalPaths.size(); ++i) {
        std::filesystem::path oldP(originalPaths[i]);
        std::filesystem::path newP = oldP.parent_path() / newNames[i];
        try {
            std::filesystem::rename(oldP, newP);
            // 2026-05-24 按照用户要求：彻底移除 JSON 逻辑，仅需更新数据库路径索引
            MetadataManager::instance().renameItem(oldP.wstring(), newP.wstring());
        } catch (...) {
            return false;
        }
    }
    return true;
}

} // namespace ArcMeta
