#pragma once
#include "ActionCommand.h"
#include "../meta/MetadataManager.h"
#include "../meta/CategoryRepo.h"
#include "../meta/DatabaseManager.h"
#include "sqlite3.h"
#include "../util/ShellHelper.h"
#include <QString>
#include <QVariant>
#include <QFileInfo>
#include <QDir>
#include <string>
#include <vector>
#include <utility>

namespace ArcMeta {

/**
 * @brief 重命名指令
 */
class RenameCommand : public ActionCommand {
public:
    RenameCommand(const QString& oldPath, const QString& newPath)
        : m_oldPath(oldPath), m_newPath(newPath) {}

    void execute() override {
        // 第一次 execute 由外部触发物理操作后调用 pushCommand，
        // 或者在 redo 时被调用。
    }

    void undo() override {
        ShellHelper::renameItem(m_newPath, m_oldPath);
    }

    void redo() override {
        ShellHelper::renameItem(m_oldPath, m_newPath);
    }

    QString description() const override { return "重命名"; }

    bool affectsPath(const QString& path) const override {
        return m_oldPath == path || m_newPath == path;
    }

private:
    QString m_oldPath;
    QString m_newPath;
};

/**
 * @brief 移动/剪切指令
 */
class MoveCommand : public ActionCommand {
public:
    MoveCommand(const QStringList& sourcePaths, const QString& oldDir, const QString& newDir)
        : m_oldDir(oldDir), m_newDir(newDir) {
        for (const QString& p : sourcePaths) {
            m_fileNames << QFileInfo(p).fileName();
        }
    }

    void execute() override {}

    void undo() override {
        QStringList currentPaths;
        for (const QString& name : m_fileNames) {
            currentPaths << QDir(m_newDir).filePath(name);
        }
        ShellHelper::copyOrMoveItems(currentPaths, m_oldDir, true);
    }

    void redo() override {
        QStringList currentPaths;
        for (const QString& name : m_fileNames) {
            currentPaths << QDir(m_oldDir).filePath(name);
        }
        ShellHelper::copyOrMoveItems(currentPaths, m_newDir, true);
    }

    QString description() const override { return "移动文件"; }

    bool affectsPath(const QString& path) const override {
        if (path.startsWith(m_oldDir) || path.startsWith(m_newDir)) return true;
        for (const QString& name : m_fileNames) {
            if (QDir(m_oldDir).filePath(name) == path || QDir(m_newDir).filePath(name) == path) return true;
        }
        return false;
    }

private:
    QStringList m_fileNames;
    QString m_oldDir;
    QString m_newDir;
};

/**
 * @brief 元数据变更指令 (星级、颜色)
 */
class MetadataCommand : public ActionCommand {
public:
    enum Type { Rating, Color };
    MetadataCommand(const QString& path, Type type, const QVariant& oldVal, const QVariant& newVal)
        : m_path(path), m_type(type), m_oldVal(oldVal), m_newVal(newVal) {}

    void execute() override {}

    void undo() override {
        applyValue(m_oldVal);
    }

    void redo() override {
        applyValue(m_newVal);
    }

    QString description() const override { return m_type == Rating ? "更改星级" : "更改颜色"; }

    bool affectsPath(const QString& path) const override {
        return m_path == path;
    }

private:
    void applyValue(const QVariant& val) {
        if (m_type == Rating) {
            MetadataManager::instance().setRating(m_path.toStdWString(), val.toInt());
        } else {
            MetadataManager::instance().setColor(m_path.toStdWString(), val.toString().toStdWString());
        }
    }

    QString m_path;
    Type m_type;
    QVariant m_oldVal;
    QVariant m_newVal;
};

/**
 * @brief 分类关联指令
 */
class CategorizeCommand : public ActionCommand {
public:
    CategorizeCommand(const QString& path, const std::string& fid, int categoryId, bool isAdd)
        : m_path(path), m_fid(fid), m_categoryId(categoryId), m_isAdd(isAdd) {}

    void execute() override {}

    void undo() override {
        if (m_isAdd) {
            CategoryRepo::removeItemFromCategory(m_categoryId, m_fid);
        } else {
            CategoryRepo::addItemToCategory(m_categoryId, m_fid, m_path.toStdWString());
        }
        MetadataManager::instance().notifyCategoryCountChanged();
    }

    void redo() override {
        if (m_isAdd) {
            CategoryRepo::addItemToCategory(m_categoryId, m_fid, m_path.toStdWString());
        } else {
            CategoryRepo::removeItemFromCategory(m_categoryId, m_fid);
        }
        MetadataManager::instance().notifyCategoryCountChanged();
    }

    QString description() const override { return m_isAdd ? "添加分类" : "移除分类"; }

    bool affectsPath(const QString& path) const override {
        return m_path == path;
    }

private:
    QString m_path;
    std::string m_fid;
    int m_categoryId;
    bool m_isAdd;
};

/**
 * @brief 批量取消分类指令 (Plan-83)
 */
class BulkUncategorizeCommand : public ActionCommand {
public:
    BulkUncategorizeCommand(const QString& path, const std::string& fid, const std::vector<int>& oldCatIds)
        : m_path(path), m_fid(fid), m_oldCatIds(oldCatIds) {}

    void execute() override {}

    void undo() override {
        for (int catId : m_oldCatIds) {
            CategoryRepo::addItemToCategory(catId, m_fid, m_path.toStdWString());
        }
        MetadataManager::instance().notifyCategoryCountChanged();
    }

    void redo() override {
        CategoryRepo::removeAllCategories(m_fid);
        MetadataManager::instance().notifyCategoryCountChanged();
    }

    QString description() const override { return "回归未分类"; }

    bool affectsPath(const QString& path) const override {
        return m_path == path;
    }

private:
    QString m_path;
    std::string m_fid;
    std::vector<int> m_oldCatIds;
};

/**
 * @brief 2026-07-xx 按照用户要求 (1.19)：导入任务指令
 * 支持一键撤销整个导入任务产生的分类节点与关联
 */
class ImportCommand : public ActionCommand {
public:
    ImportCommand(const std::vector<int>& createdCatIds, 
                  const std::vector<std::pair<int, std::string>>& createdAssociations,
                  const std::vector<std::wstring>& registeredPaths)
        : m_createdCatIds(createdCatIds), 
          m_createdAssociations(createdAssociations),
          m_registeredPaths(registeredPaths) {}

    void execute() override {}

    void undo() override {
        // 1. 移除分类关联
        for (const auto& assoc : m_createdAssociations) {
            CategoryRepo::removeItemFromCategory(assoc.first, assoc.second);
        }
        // 2. 物理删除创建的分类节点 (逆序删除以处理父子关系)
        for (auto it = m_createdCatIds.rbegin(); it != m_createdCatIds.rend(); ++it) {
            // 此处直接操作 DB 物理删除，不走 CategoryRepo::remove 的移入回收站逻辑
            sqlite3* db = DatabaseManager::instance().getGlobalDb();
            if (db) {
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, "DELETE FROM categories WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmt, 1, *it);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
        }
        // 3. 将新注册的项目标记回非受控状态 (Managed = false)
        for (const auto& wp : m_registeredPaths) {
            MetadataManager::instance().setManaged(wp, false, false);
        }

        // 4. 物理落盘并刷新 UI
        CategoryRepo::saveImmediately();
        MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
    }

    void redo() override {
        // 由于导入涉及物理扫描，REDO 逻辑较为复杂，暂时保留空实现或提示用户重新导入
    }

    QString description() const override { return "撤销导入任务"; }

    bool affectsPath(const QString& path) const override {
        std::wstring wp = QDir::toNativeSeparators(path).toStdWString();
        for (const auto& p : m_registeredPaths) if (p == wp) return true;
        return false;
    }

private:
    std::vector<int> m_createdCatIds;
    std::vector<std::pair<int, std::string>> m_createdAssociations;
    std::vector<std::wstring> m_registeredPaths;
};

} // namespace ArcMeta
