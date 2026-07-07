#include "CategoryRepo.h"
#include "DatabaseManager.h"
#include "MetadataManager.h"
#include "sqlite3.h"
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QtConcurrent>
#include <set>
#include <unordered_set>
#include <algorithm>

namespace ArcMeta {

std::atomic<int> CategoryRepo::s_totalFileCount{0};
std::atomic<int> CategoryRepo::s_categorizedCount{0};


void CategoryRepo::initialize() {
    // SQLite 模式下，DatabaseManager::init() 已由 MetadataManager 调用
}

void CategoryRepo::saveImmediately() {
    DatabaseManager::instance().flushAll();
}

std::vector<Category> CategoryRepo::getAll() {
    std::vector<Category> results;
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return results;

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, parent_id, name, color, preset_tags, sort_order, pinned, encrypted, encrypt_hint, physical_frn, physical_path FROM categories WHERE id > 0 ORDER BY sort_order ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Category c;
            c.id = sqlite3_column_int(stmt, 0);
            c.parentId = sqlite3_column_int(stmt, 1);
            const wchar_t* wname = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 2));
            if (wname) c.name = wname;
            const wchar_t* color = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 3));
            if (color) c.color = color;
            const wchar_t* wtags = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 4));
            QString tags = wtags ? QString::fromWCharArray(wtags) : "";
            for (const auto& t : tags.split(",", Qt::SkipEmptyParts)) c.presetTags.push_back(t.toStdWString());
            c.sortOrder = sqlite3_column_int(stmt, 5);
            c.pinned = sqlite3_column_int(stmt, 6) != 0;
            c.encrypted = sqlite3_column_int(stmt, 7) != 0;
            const wchar_t* hint = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 8));
            if (hint) c.encryptHint = hint;
            c.physicalFrn = sqlite3_column_int64(stmt, 9);
            const wchar_t* wpath = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 10));
            if (wpath) c.physicalPath = wpath;
            results.push_back(c);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool CategoryRepo::add(Category& cat) {
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) {
        qDebug() << "[CategoryRepo] add FAILED: No DB connection";
        return false;
    }

    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO categories (parent_id, name, color, preset_tags, sort_order, pinned, encrypted, encrypt_hint, physical_frn, physical_path) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, cat.parentId);
        sqlite3_bind_text16(stmt, 2, cat.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 3, cat.color.c_str(), -1, SQLITE_TRANSIENT);
        
        QStringList tags;
        for (const auto& t : cat.presetTags) tags << QString::fromStdWString(t);
        sqlite3_bind_text16(stmt, 4, tags.join(",").toStdWString().c_str(), -1, SQLITE_TRANSIENT);
        
        sqlite3_bind_int(stmt, 5, cat.sortOrder);
        sqlite3_bind_int(stmt, 6, cat.pinned ? 1 : 0);
        sqlite3_bind_int(stmt, 7, cat.encrypted ? 1 : 0);
        sqlite3_bind_text16(stmt, 8, cat.encryptHint.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 9, cat.physicalFrn);
        sqlite3_bind_text16(stmt, 10, cat.physicalPath.c_str(), -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            cat.id = static_cast<int>(sqlite3_last_insert_rowid(db));
            qDebug() << "[CategoryRepo] add success: Name =" << QString::fromStdWString(cat.name) << "ID =" << cat.id << "Parent =" << cat.parentId;
            sqlite3_finalize(stmt);
            return true;
        } else {
            qDebug() << "[CategoryRepo] add FAILED during step:" << sqlite3_errmsg(db) << "Code:" << rc;
        }
        sqlite3_finalize(stmt);
    } else {
        qDebug() << "[CategoryRepo] add FAILED during prepare:" << sqlite3_errmsg(db) << "Code:" << rc;
    }
    return false;
}

bool CategoryRepo::removeAllCategories(const std::string& fileId128) {
    return removeAllCategoriesBatch({fileId128});
}

bool CategoryRepo::removeAllCategoriesBatch(const std::vector<std::string>& fids) {
    return executeFidBatch(fids, [](sqlite3* db, const std::string& fid) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "DELETE FROM category_items WHERE file_id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, fid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return true;
    });
}

std::vector<int> CategoryRepo::getItemCategoryIds(const std::string& fid) {
    std::vector<int> ids;
    if (fid.empty()) return ids;
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return ids;

    sqlite3_stmt* stmt;
    const char* sql = "SELECT category_id FROM category_items WHERE file_id = ? AND category_id > 0";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, fid.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ids.push_back(sqlite3_column_int(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return ids;
}

bool CategoryRepo::moveToTrashBatch(const std::vector<std::string>& fids) {
    return executeFidBatch(fids, [](sqlite3* db, const std::string& fid) {
        // 1. Remove all existing category associations
        sqlite3_stmt* delStmt;
        if (sqlite3_prepare_v2(db, "DELETE FROM category_items WHERE file_id = ?", -1, &delStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(delStmt, 1, fid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(delStmt);
            sqlite3_finalize(delStmt);
        }
        // 2. Insert into trash bucket
        std::wstring path = MetadataManager::instance().getPathByFid(fid);
        sqlite3_stmt* insStmt;
        if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO category_items (category_id, file_id, path_hint, added_at) VALUES (?, ?, ?, ?)",
            -1, &insStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(insStmt, 1, TRASH_CATEGORY_ID);
            sqlite3_bind_text(insStmt, 2, fid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text16(insStmt, 3, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(insStmt, 4, static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
            sqlite3_step(insStmt);
            sqlite3_finalize(insStmt);
        }
        // 3. Update is_trash flag
        if (!path.empty()) {
            MetadataManager::instance().setTrash(path, true);
        }
        return true;
    });
}

bool CategoryRepo::restoreFromTrashBatch(const std::vector<std::string>& fids) {
    return executeFidBatch(fids, [](sqlite3* db, const std::string& fid) {
        // 1. Remove from trash bucket
        sqlite3_stmt* delStmt;
        if (sqlite3_prepare_v2(db,
            "DELETE FROM category_items WHERE category_id = ? AND file_id = ?",
            -1, &delStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(delStmt, 1, TRASH_CATEGORY_ID);
            sqlite3_bind_text(delStmt, 2, fid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(delStmt);
            sqlite3_finalize(delStmt);
        }
        // 2. Add to "未分类" bucket
        std::wstring path = MetadataManager::instance().getPathByFid(fid);
        sqlite3_stmt* insStmt;
        if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO category_items (category_id, file_id, path_hint, added_at) VALUES (?, ?, ?, ?)",
            -1, &insStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(insStmt, 1, UNCATEGORIZED_CAT_ID);
            sqlite3_bind_text(insStmt, 2, fid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text16(insStmt, 3, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(insStmt, 4, static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
            sqlite3_step(insStmt);
            sqlite3_finalize(insStmt);
        }
        // 3. Clear is_trash flag in metadata cache + persist
        if (!path.empty()) {
            MetadataManager::instance().setTrash(path, false);
        }
        
        // 2026-06-xx 物理对账：恢复后触发全量统计重建
        MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
        return true;
    });
}

bool CategoryRepo::restoreFromTrash(const std::string& fid) {
    return restoreFromTrashBatch({fid});
}

bool CategoryRepo::permanentlyDeleteBatch(const std::vector<std::string>& fids) {
    // Collect paths before removing from cache
    std::vector<std::wstring> paths;
    for (const auto& fid : fids) {
        std::wstring path = MetadataManager::instance().getPathByFid(fid);
        if (!path.empty()) paths.push_back(path);
    }

    bool ok = executeFidBatch(fids, [](sqlite3* db, const std::string& fid) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "DELETE FROM category_items WHERE file_id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, fid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return true;
    });

    // 2. Remove from metadata table + in-memory cache (per-volume DBs handled inside removeMetadataSync)
    int removedCount = 0;
    for (const auto& path : paths) {
        MetadataManager::instance().removeMetadataSync(path);
        removedCount++;
    }

    // 3. Update "全部数据" count — permanent delete is the only operation that reduces it
    if (removedCount > 0) {
        incrementTotalFileCount(-removedCount);
    }

    return ok;
}

bool CategoryRepo::permanentlyDelete(const std::string& fid) {
    return permanentlyDeleteBatch({fid});
}

Category CategoryRepo::getById(int id) {
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    Category c;
    if (!db) return c;

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, parent_id, name, color, preset_tags, sort_order, pinned, encrypted, encrypt_hint, physical_frn, physical_path FROM categories WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            c.id = sqlite3_column_int(stmt, 0);
            c.parentId = sqlite3_column_int(stmt, 1);
            const wchar_t* wname = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 2));
            if (wname) c.name = wname;
            const wchar_t* color = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 3));
            if (color) c.color = color;
            const wchar_t* wtags = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 4));
            QString tags = wtags ? QString::fromWCharArray(wtags) : "";
            for (const auto& t : tags.split(",", Qt::SkipEmptyParts)) c.presetTags.push_back(t.toStdWString());
            c.sortOrder = sqlite3_column_int(stmt, 5);
            c.pinned = sqlite3_column_int(stmt, 6) != 0;
            c.encrypted = sqlite3_column_int(stmt, 7) != 0;
            const wchar_t* hint = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 8));
            if (hint) c.encryptHint = hint;
            c.physicalFrn = sqlite3_column_int64(stmt, 9);
            const wchar_t* wpath = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 10));
            if (wpath) c.physicalPath = wpath;
        }
        sqlite3_finalize(stmt);
    }
    return c;
}

bool CategoryRepo::update(const Category& cat) {
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return false;

    sqlite3_stmt* stmt;
    const char* sql = "UPDATE categories SET parent_id=?, name=?, color=?, preset_tags=?, sort_order=?, pinned=?, encrypted=?, encrypt_hint=?, physical_frn=?, physical_path=? WHERE id=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, cat.parentId);
        sqlite3_bind_text16(stmt, 2, cat.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 3, cat.color.c_str(), -1, SQLITE_TRANSIENT);
        QStringList tags;
        for (const auto& t : cat.presetTags) tags << QString::fromStdWString(t);
        sqlite3_bind_text16(stmt, 4, tags.join(",").toStdWString().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, cat.sortOrder);
        sqlite3_bind_int(stmt, 6, cat.pinned ? 1 : 0);
        sqlite3_bind_int(stmt, 7, cat.encrypted ? 1 : 0);
        sqlite3_bind_text16(stmt, 8, cat.encryptHint.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 9, cat.physicalFrn);
        sqlite3_bind_text16(stmt, 10, cat.physicalPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 11, cat.id);

        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }
    return false;
}

int CategoryRepo::findByFrn(uint64_t frn) {
    if (frn == 0) return 0;
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return 0;

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id FROM categories WHERE physical_frn = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, frn);
        int id = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return id;
    }
    return 0;
}

bool CategoryRepo::updatePhysicalMapping(int id, uint64_t frn, const std::wstring& path) {
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return false;

    sqlite3_stmt* stmt;
    const char* sql = "UPDATE categories SET physical_frn = ?, physical_path = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, frn);
        sqlite3_bind_text16(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, id);
        bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return ok;
    }
    return false;
}

int CategoryRepo::findCategoryId(int parentId, const std::wstring& name) {
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return 0;

    sqlite3_stmt* stmt;
    const char* sql = "SELECT id FROM categories WHERE parent_id = ? AND name = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, parentId);
        sqlite3_bind_text16(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        int id = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        if (id > 0) {
            qDebug() << "[CategoryRepo] findCategoryId found:" << QString::fromStdWString(name) << "->" << id << "under parent" << parentId;
        }
        return id;
    }
    return 0;
}

bool CategoryRepo::remove(int id) {
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return false;

    // Step 1: Recursively collect all category IDs to delete (existing logic, keep as-is)
    std::vector<int> toDelete = {id};
    size_t i = 0;
    while (i < toDelete.size()) {
        int pid = toDelete[i++];
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT id FROM categories WHERE parent_id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, pid);
            while (sqlite3_step(stmt) == SQLITE_ROW) toDelete.push_back(sqlite3_column_int(stmt, 0));
            sqlite3_finalize(stmt);
        }
    }

    // Step 2: Collect all unique File IDs from those categories (deduplicated)
    std::vector<std::string> fids;
    std::unordered_map<std::string, std::wstring> fidToPath; // fid -> path_hint
    for (int catId : toDelete) {
        auto items = getItemsInCategory(catId);
        for (const auto& item : items) {
            if (fidToPath.find(item.fileId128) == fidToPath.end()) {
                fidToPath[item.fileId128] = item.pathHint;
                fids.push_back(item.fileId128);
            }
        }
    }

    // Step 3: For each File ID — remove all its category associations, then insert one row into trash bucket
    executeFidBatch(fids, [&](sqlite3* innerDb, const std::string& fid) {
        const std::wstring& pathHint = fidToPath[fid];

        // Remove ALL existing category_items rows for this fid
        sqlite3_stmt* delStmt;
        if (sqlite3_prepare_v2(innerDb, "DELETE FROM category_items WHERE file_id = ?", -1, &delStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(delStmt, 1, fid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(delStmt);
            sqlite3_finalize(delStmt);
        }
        // Insert one row into trash bucket
        sqlite3_stmt* insStmt;
        if (sqlite3_prepare_v2(innerDb,
            "INSERT OR REPLACE INTO category_items (category_id, file_id, path_hint, added_at) VALUES (?, ?, ?, ?)",
            -1, &insStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(insStmt, 1, TRASH_CATEGORY_ID);
            sqlite3_bind_text(insStmt, 2, fid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text16(insStmt, 3, pathHint.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(insStmt, 4, static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
            sqlite3_step(insStmt);
            sqlite3_finalize(insStmt);
        }
        // Update in-memory cache: set isTrash = true, then persist
        std::wstring path = MetadataManager::instance().getPathByFid(fid);
        if (path.empty()) path = pathHint;
        if (!path.empty()) {
            MetadataManager::instance().setTrash(path, true);
        }
        return true;
    });

        // 4. Delete only the category rows
    SqlTransaction trans(db);
    for (int delId : toDelete) {
            // 首先清理子项关联，防止幽灵关联
            sqlite3_stmt* itemDelStmt;
            if (sqlite3_prepare_v2(db, "DELETE FROM category_items WHERE category_id = ?", -1, &itemDelStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(itemDelStmt, 1, delId);
                sqlite3_step(itemDelStmt);
                sqlite3_finalize(itemDelStmt);
            }

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "DELETE FROM categories WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, delId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    trans.commit();

    MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
    return true;
}

bool CategoryRepo::reorder(int parentId, bool ascending) {
    auto cats = getAll();
    std::vector<Category*> targets;
    for (auto& c : cats) if (c.parentId == parentId) targets.push_back(&c);
    
    std::sort(targets.begin(), targets.end(), [ascending](Category* a, Category* b) {
        int cmp = a->name.compare(b->name);
        return ascending ? (cmp < 0) : (cmp > 0);
    });

    for (size_t i = 0; i < targets.size(); ++i) {
        targets[i]->sortOrder = static_cast<int>(i);
        update(*targets[i]);
    }
    return true;
}

bool CategoryRepo::reorderAll(bool ascending) {
    auto cats = getAll();
    std::sort(cats.begin(), cats.end(), [ascending](const Category& a, const Category& b) {
        int cmp = a.name.compare(b.name);
        return ascending ? (cmp < 0) : (cmp > 0);
    });

    for (size_t i = 0; i < cats.size(); ++i) {
        cats[i].sortOrder = static_cast<int>(i);
        update(cats[i]);
    }
    return true;
}

bool CategoryRepo::addItemToCategory(int categoryId, const std::string& fileId128, const std::wstring& pathHint) {
    sqlite3* memDb = DatabaseManager::instance().getGlobalDb();
    if (!memDb) return false;

    std::wstring finalPath = MetadataManager::normalizePath(pathHint);
    if (finalPath.empty()) finalPath = MetadataManager::instance().getPathByFid(fileId128);

    const char* sql = "INSERT OR REPLACE INTO category_items (category_id, file_id, path_hint, added_at) VALUES (?, ?, ?, ?)";
    double addedAt = static_cast<double>(QDateTime::currentMSecsSinceEpoch());

    sqlite3_stmt* memStmt;
    if (sqlite3_prepare_v2(memDb, sql, -1, &memStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(memStmt, 1, categoryId);
        sqlite3_bind_text(memStmt, 2, fileId128.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(memStmt, 3, finalPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(memStmt, 4, addedAt);
        
        if (sqlite3_step(memStmt) == SQLITE_DONE) {
            sqlite3_finalize(memStmt);

            // 异步磁盘分发
            DatabaseManager::instance().enqueueSyncTask([categoryId, fileId128, finalPath, addedAt, memDb, sql]() {
                sqlite3* diskDb = DatabaseManager::instance().getDiskDb(memDb);
                if (!diskDb) return;
                sqlite3_stmt* diskStmt;
                if (sqlite3_prepare_v2(diskDb, sql, -1, &diskStmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(diskStmt, 1, categoryId);
                    sqlite3_bind_text(diskStmt, 2, fileId128.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text16(diskStmt, 3, finalPath.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_double(diskStmt, 4, addedAt);
                    sqlite3_step(diskStmt);
                    sqlite3_finalize(diskStmt);
                }
            });

            // 2026-08-xx 按照 Plan-126：废除此处直接调用 registerItem。
            // 归类操作不应直接触发表入库，应由物理位移（如迁移）触发 USN 信号后再由 AutoImportManager 驱动。

            syncCategorizedCountForFid(fileId128);
            MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::CountsOnly);
            return true;
        }
        sqlite3_finalize(memStmt);
    }
    return false;
}

bool CategoryRepo::removeItemFromCategory(int categoryId, const std::string& fileId128) {
    sqlite3* memDb = DatabaseManager::instance().getGlobalDb();
    if (!memDb) return false;

    const char* sql = "DELETE FROM category_items WHERE category_id = ? AND file_id = ?";
    sqlite3_stmt* memStmt;
    if (sqlite3_prepare_v2(memDb, sql, -1, &memStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(memStmt, 1, categoryId);
        sqlite3_bind_text(memStmt, 2, fileId128.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(memStmt) == SQLITE_DONE) {
            sqlite3_finalize(memStmt);

            // 异步磁盘分发
            DatabaseManager::instance().enqueueSyncTask([categoryId, fileId128, memDb, sql]() {
                sqlite3* diskDb = DatabaseManager::instance().getDiskDb(memDb);
                if (!diskDb) return;
                sqlite3_stmt* diskStmt;
                if (sqlite3_prepare_v2(diskDb, sql, -1, &diskStmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(diskStmt, 1, categoryId);
                    sqlite3_bind_text(diskStmt, 2, fileId128.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(diskStmt);
                    sqlite3_finalize(diskStmt);
                }
            });

            syncCategorizedCountForFid(fileId128);
            return true;
        }
        sqlite3_finalize(memStmt);
    }
    return false;
}

std::vector<CategoryItem> CategoryRepo::getItemsInCategory(int categoryId) {
    return getItemsInCategories({categoryId});
}

std::vector<CategoryItem> CategoryRepo::getItemsInCategories(const std::vector<int>& categoryIds) {
    std::vector<CategoryItem> results;
    if (categoryIds.empty()) return results;
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return results;

    QStringList placeholders;
    for (int i = 0; i < categoryIds.size(); ++i) placeholders << "?";
    QString sql = QString("SELECT DISTINCT file_id, path_hint FROM category_items WHERE category_id IN (%1)").arg(placeholders.join(","));

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        for (int i = 0; i < categoryIds.size(); ++i) {
            sqlite3_bind_int(stmt, i + 1, categoryIds[i]);
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back({
                sqlite3_column_text(stmt, 0) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) : "",
                sqlite3_column_text16(stmt, 1) ? reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 1)) : L""
            });
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

std::vector<int> CategoryRepo::getSubtreeIds(int categoryId) {
    std::vector<int> ids = {categoryId};
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return ids;

    size_t i = 0;
    while (i < ids.size()) {
        int pid = ids[i++];
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT id FROM categories WHERE parent_id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, pid);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int childId = sqlite3_column_int(stmt, 0);
                if (std::find(ids.begin(), ids.end(), childId) == ids.end()) {
                    ids.push_back(childId);
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    return ids;
}

std::vector<CategoryItem> CategoryRepo::getItemsRecursive(int categoryId) {
    std::vector<int> ids = getSubtreeIds(categoryId);

    std::map<std::string, std::wstring> resultsMap;
    for (int cid : ids) {
        auto items = getItemsInCategory(cid);
        for (const auto& item : items) resultsMap[item.fileId128] = item.pathHint;
    }

    std::vector<CategoryItem> results;
    for (auto const& [fid, path] : resultsMap) results.push_back({fid, path});
    return results;
}

std::vector<std::string> CategoryRepo::getFileIdsInCategory(int categoryId) {
    auto items = getItemsInCategory(categoryId);
    std::vector<std::string> res;
    for (const auto& i : items) res.push_back(i.fileId128);
    return res;
}

std::vector<std::string> CategoryRepo::getFileIdsRecursive(int categoryId) {
    auto items = getItemsRecursive(categoryId);
    std::vector<std::string> res;
    for (const auto& i : items) res.push_back(i.fileId128);
    return res;
}

std::vector<std::pair<int, int>> CategoryRepo::getCounts() {
    std::vector<std::pair<int, int>> res;
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return res;

    // 1. 一次性加载所有分类关联关系
    std::unordered_map<std::string, std::vector<int>> fidToCats;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT file_id, category_id FROM category_items WHERE category_id > 0", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* fid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            int catId = sqlite3_column_int(stmt, 1);
            if (fid) fidToCats[fid].push_back(catId);
        }
        sqlite3_finalize(stmt);
    }

    // 2. 加载分类层级结构以支持递归计数
    std::map<int, int> catParentMap;
    if (sqlite3_prepare_v2(db, "SELECT id, parent_id FROM categories WHERE id > 0", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            catParentMap[sqlite3_column_int(stmt, 0)] = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    std::map<int, std::unordered_set<std::string>> catToUniqueFids;
    // 3. 遍历内存缓存，按 FID 去重并分发到各分类桶
    // 2026-07-xx 回滚：仅计算直接关联的 FID，取消自动向上递归汇总
    MetadataManager::instance().forEachCachedItem([&](const std::wstring&, const RuntimeMeta& meta) {
        // 2026-07-xx 物理对齐：只要在关联表中且非文件夹/回收站，即计入分类总数
        if (!meta.fileId128.empty() && !meta.isFolder && !meta.isTrash && !meta.isInvalid) {
            auto it = fidToCats.find(meta.fileId128);
            if (it != fidToCats.end()) {
                for (int catId : it->second) {
                    catToUniqueFids[catId].insert(meta.fileId128);
                }
            }
        }
    });

    for (auto const& [id, fids] : catToUniqueFids) {
        res.push_back({id, static_cast<int>(fids.size())});
    }
    return res;
}

int CategoryRepo::getTotalFileCount() {
    return s_totalFileCount.load();
}

int CategoryRepo::getUncategorizedCount() {
    return getSystemCounts()["uncategorized"];
}

void CategoryRepo::setTotalFileCount(int count) {
    s_totalFileCount.store(count);
}

void CategoryRepo::setCategorizedCount(int count) {
    s_categorizedCount.store(count);
}

void CategoryRepo::incrementTotalFileCount(int delta) {
    s_totalFileCount += delta;
    updatePersistentStat(STAT_TOTAL_FILES, delta);
}

void CategoryRepo::incrementCategorizedCount(int delta) {
    s_categorizedCount += delta;
    updatePersistentStat(STAT_CATEGORIZED, delta);
}

void CategoryRepo::updatePersistentStat(const std::string& key, int delta) {
    sqlite3* memDb = DatabaseManager::instance().getGlobalDb();
    if (!memDb) return;

    const char* sql = "INSERT OR REPLACE INTO system_stats (key, value) VALUES (?, "
                      "COALESCE((SELECT value FROM system_stats WHERE key = ?), 0) + ?)";
    
    sqlite3_stmt* memStmt;
    if (sqlite3_prepare_v2(memDb, sql, -1, &memStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(memStmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(memStmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(memStmt, 3, delta);
        if (sqlite3_step(memStmt) == SQLITE_DONE) {
            sqlite3_finalize(memStmt);

            // 异步磁盘分发
            DatabaseManager::instance().enqueueSyncTask([key, delta, memDb, sql]() {
                sqlite3* diskDb = DatabaseManager::instance().getDiskDb(memDb);
                if (!diskDb) return;
                sqlite3_stmt* diskStmt;
                if (sqlite3_prepare_v2(diskDb, sql, -1, &diskStmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(diskStmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(diskStmt, 2, key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(diskStmt, 3, delta);
                    sqlite3_step(diskStmt);
                    sqlite3_finalize(diskStmt);
                }
            });
        } else {
            sqlite3_finalize(memStmt);
        }
    }
}

bool CategoryRepo::executeFidBatch(const std::vector<std::string>& fids, std::function<bool(struct sqlite3*, const std::string&)> action) {
    if (fids.empty()) return true;
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return false;

    SqlTransaction trans(db);
    for (const auto& fid : fids) {
        if (!action(db, fid)) {
            trans.rollback();
            return false;
        }
    }
    bool ok = trans.commit();
    
    // 2026-06-xx 架构优化：批量变动后自动同步已分类计数，实现跨表一致性
    syncCategorizedCountForFid("");
    
    // 批量处理后通知 UI 刷新
    MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::CountsOnly);
    return ok;
}

void CategoryRepo::syncCategorizedCountForFid(const std::string& /*fid*/) {
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return;

    // 2026-06-xx 物理对账：通过全局 DISTINCT 查询重新计算已分类总数
    // 这种做法比手动增减 delta 更稳健，能自动修正因程序崩溃导致的计数偏差
    sqlite3_stmt* stmt;
    // 物理红线：分类计数必须排除回收站 (ID = -8) 和未分类 (ID = -2)
    const char* sql = "SELECT COUNT(DISTINCT file_id) FROM category_items WHERE category_id > 0";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            int oldCount = s_categorizedCount.load();
            s_categorizedCount.store(count);
            
            // 物理持久化：直接更新增量
            if (count != oldCount) {
                updatePersistentStat(STAT_CATEGORIZED, count - oldCount);
            }
        }
        sqlite3_finalize(stmt);
    }
}

void CategoryRepo::fullRecount() {
    // 2026-06-xx 按照用户要求：从数据库加载持久化的计数
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    bool hasSavedCounts = false;
    if (db) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT key, value FROM system_stats", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* keyPtr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (!keyPtr) continue;
                std::string key = keyPtr;
                int val = sqlite3_column_int(stmt, 1);
                if (key == STAT_TOTAL_FILES) {
                    s_totalFileCount.store(val);
                    if (val > 0) hasSavedCounts = true;
                    qDebug() << "[Recount] 从数据库加载历史计数: Total =" << val;
                }
                else if (key == STAT_CATEGORIZED) {
                    s_categorizedCount.store(val);
                    qDebug() << "[Recount] 从数据库加载历史计数: Categorized =" << val;
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    // 2026-06-xx 物理修复：即便数据库中有记录，也在启动时执行一次全量内存核对以校准偏差。
    // 理由：防止因异常退出导致的 persistent count 与内存实际加载量不一致。
    // 2026-07-xx 按照用户要求：始终执行全量盘点，不再根据 hasSavedCounts 判定，确保实时准确性
    if (true) {
        int total = 0;
        int categorized = 0;
        
        // 1. 获取所有已分类的 FID 及其路径提示
        std::unordered_map<std::string, std::wstring> categorizedFidToPath;
        if (db) {
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, "SELECT DISTINCT file_id, path_hint FROM category_items WHERE category_id > 0", -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* fid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const wchar_t* wpath = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 1));
                    if (fid) categorizedFidToPath[fid] = wpath ? wpath : L"";
                }
                sqlite3_finalize(stmt);
            }
        }

        // 2. 补全内存缺失：如果已分类项不在内存缓存中，强制注册
        // 同时对于已经在缓存中的项，确保其标记为 isManaged
        for (const auto& [fid, path] : categorizedFidToPath) {
            if (path.empty()) continue;
            auto meta = MetadataManager::instance().getMeta(path);
            if (meta.fileId128.empty()) {
                // 此处注册是基于数据库记录的“存量找回”，符合 1:1 同步原则
                MetadataManager::instance().registerItem(path, true);
            } else if (!meta.isManaged) {
                // 已在缓存中但未标记为 Managed (多见于归类后即时重启)，执行一次物理补齐
                MetadataManager::instance().setManaged(path, true, false);
            }
        }

        std::vector<std::wstring> managedPaths;
        std::unordered_set<std::string> seenTotal, seenCategorized;

        // 3. 物理盘点
        MetadataManager::instance().forEachCachedItem([&](const std::wstring& path, const RuntimeMeta& meta) {
            if (meta.fileId128.empty()) return;
            bool isCategorized = categorizedFidToPath.count(meta.fileId128);
            
            // 2026-07-xx 物理校准：只要在数据库中有记录（已分配分类或有自定义元数据），就必须标记为 Managed
            if (isCategorized || meta.hasUserOperations()) {
                if (!meta.isManaged) managedPaths.push_back(path);

                // 2026-07-xx 物理加固：采用 FID 去重盘点，确保总计数绝对精准
                if (!meta.isFolder && !meta.isInvalid && !meta.isTrash) {
                    seenTotal.insert(meta.fileId128);
                    if (isCategorized) seenCategorized.insert(meta.fileId128);
                }
            }
        });

        total = (int)seenTotal.size();
        categorized = (int)seenCategorized.size();

        // 2026-07-xx 物理修复：严禁在 forEachCachedItem 内部持有锁时调用 setManaged (会导致死锁)
        for (const auto& p : managedPaths) {
            MetadataManager::instance().setManaged(p, true, false);
        }
        
        qDebug() << "[Recount] 盘点完成。总计:" << total << "已分类:" << categorized;
        s_totalFileCount.store(total);
        s_categorizedCount.store(categorized);
        
        if (db) {
            SqlTransaction trans(db);
            sqlite3_stmt* stmt;
            const char* sql = "INSERT OR REPLACE INTO system_stats (key, value) VALUES (?, ?)";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, STAT_TOTAL_FILES, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, total);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
                sqlite3_bind_text(stmt, 1, STAT_CATEGORIZED, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, categorized);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            trans.commit();
            // 物理落盘，确保初始化结果即刻生效
            DatabaseManager::instance().flushAll();
        }
    }

    // 2026-06-xx 核心逻辑升级：物理有效性对账 (盘点 FRN)
    // 这一步在后台异步执行，验证文件是否被第三方删除。若失效，标记为 is_invalid 而非直接删除。
    // 使用 [db] 显式捕获数据库指针，并增加错误检查
    (void)QtConcurrent::run([db]() {
        if (!db) return;

        std::vector<std::pair<std::wstring, std::string>> itemsToCheck;
        MetadataManager::instance().forEachCachedItem([&](const std::wstring& path, const RuntimeMeta& meta) {
            // 只对未标记失效且非回收站的文件进行物理校验
            if (!meta.isFolder && !meta.isInvalid && !meta.isTrash) {
                itemsToCheck.push_back({path, meta.fileId128});
            }
        });

        int invalidatedCount = 0;
        for (const auto& item : itemsToCheck) {
            std::string currentFid;
            // 通过 WinAPI 直接检查物理文件是否存在且 ID 匹配
            bool exists = MetadataManager::fetchWinApiMetadataDirect(item.first, currentFid);
            if (!exists || currentFid != item.second) {
                // 物理校验失败：文件可能已被第三方删除或移动，标记为失效
                invalidatedCount++;
                MetadataManager::instance().setInvalid(item.first, true);
            }
        }

        if (invalidatedCount > 0) {
            qDebug() << "[Recount] 物理校验发现" << invalidatedCount << "个失效项，已归类至失效数据";
            // 2026-06-xx 物理同步：强制将内存中的 is_invalid 变更刷入磁盘
            DatabaseManager::instance().flushAll();
            MetadataManager::instance().notifyUI(MetadataManager::RefreshLevel::FullRebuild);
        }
    });
}

std::vector<Category> CategoryRepo::getRecentlyUsed(int limit) {
    std::vector<Category> results;
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (!db) return results;

    sqlite3_stmt* stmt;
    const char* sql = "SELECT c.id, c.parent_id, c.name, c.color, c.preset_tags, c.sort_order, c.pinned, c.encrypted, c.encrypt_hint "
                      "FROM categories c JOIN (SELECT category_id, MAX(added_at) as last_added FROM category_items GROUP BY category_id) r "
                      "ON c.id = r.category_id ORDER BY r.last_added DESC LIMIT ?";
                      
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Category c;
            c.id = sqlite3_column_int(stmt, 0);
            c.parentId = sqlite3_column_int(stmt, 1);
            const wchar_t* wname = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 2));
            if (wname) c.name = wname;
            const wchar_t* color = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 3));
            if (color) c.color = color;
            const wchar_t* wtags = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 4));
            QString tags = wtags ? QString::fromWCharArray(wtags) : "";
            for (const auto& t : tags.split(",", Qt::SkipEmptyParts)) c.presetTags.push_back(t.toStdWString());
            c.sortOrder = sqlite3_column_int(stmt, 5);
            c.pinned = sqlite3_column_int(stmt, 6) != 0;
            c.encrypted = sqlite3_column_int(stmt, 7) != 0;
            const wchar_t* hint = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 8));
            if (hint) c.encryptHint = hint;
            results.push_back(c);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int CategoryRepo::getUniqueItemCount() {
    return s_totalFileCount.load();
}

int CategoryRepo::getUncategorizedItemCount() {
    return getSystemCounts()["uncategorized"];
}

QMap<QString, int> CategoryRepo::getSystemCounts() {
    QMap<QString, int> res;
    
    std::unordered_set<std::string> categorizedFids;
    sqlite3* db = DatabaseManager::instance().getGlobalDb();
    if (db) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT DISTINCT file_id FROM category_items WHERE category_id > 0", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* fid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (fid) categorizedFids.insert(fid);
            }
            sqlite3_finalize(stmt);
        }
    }

    // 2026-07-xx 物理加固：使用 Set 进行 FID 去重计数，彻底解决因路径偏移导致的幽灵计数
    std::unordered_set<std::string> seenAll, seenRecent, seenUntagged, seenUncategorized, seenTrash, seenInvalid;
    QSet<QString> uniqueTags;
    double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch());

    MetadataManager::instance().forEachCachedItem([&](const std::wstring&, const RuntimeMeta& meta) {
        if (meta.fileId128.empty()) return;
        if (meta.isFolder) return;

        // 特殊：失效与回收站反映的是物理存在状态，始终计数
        if (meta.isInvalid) {
            seenInvalid.insert(meta.fileId128);
            return;
        }
        if (meta.isTrash) {
            seenTrash.insert(meta.fileId128);
            return;
        }

        // 2026-07-xx 回滚：系统视图计数放宽 Managed 限制，显示所有已扫描到的有效项
        seenAll.insert(meta.fileId128);
        
        if (meta.tags.isEmpty()) {
            seenUntagged.insert(meta.fileId128);
        } else {
            // 2026-07-xx 按照用户要求：统计系统中存在的唯一标签总数，而非带标签的项目数
            for (const QString& t : meta.tags) uniqueTags.insert(t);
        }

        if (meta.atime >= now - 86400000.0) seenRecent.insert(meta.fileId128);
        
        if (categorizedFids.find(meta.fileId128) == categorizedFids.end()) {
            seenUncategorized.insert(meta.fileId128);
        }
    });

    res["all"] = static_cast<int>(seenAll.size());
    res["tags"] = static_cast<int>(uniqueTags.size());
    res["recently_visited"] = static_cast<int>(seenRecent.size());
    res["untagged"] = static_cast<int>(seenUntagged.size());
    res["uncategorized"] = static_cast<int>(seenUncategorized.size());
    res["trash"] = static_cast<int>(seenTrash.size());
    res["invalid_data"] = static_cast<int>(seenInvalid.size());
    return res;
}

QStringList CategoryRepo::getSystemCategoryPaths(const QString& type) {
    QStringList paths;
    std::unordered_set<std::string> categorizedIds;
    if (type == "uncategorized") {
        sqlite3* db = DatabaseManager::instance().getGlobalDb();
        if (db) {
            sqlite3_stmt* stmt;
            // 2026-06-xx 性能优化：查询“未分类”路径时，排除掉已在自定义分类 (ID > 0) 中的文件
            if (sqlite3_prepare_v2(db, "SELECT DISTINCT file_id FROM category_items WHERE category_id > 0", -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* fid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    if (fid) categorizedIds.insert(fid);
                }
                sqlite3_finalize(stmt);
            }
        }
    }

    double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch());
    MetadataManager::instance().forEachCachedItem([&](const std::wstring& path, const RuntimeMeta& meta) {
        // 核心红线：彻底排除文件夹
        if (meta.isFolder) return;
        
        bool match = false;
        std::wstring finalPath = path;

        if (type == "trash") {
            if (meta.isTrash) match = true;
        } else if (type == "invalid_data") {
            // 2026-08-xx 物理修复：失效数据查询不一致 Bug。
            // 当标记为失效时，物理路径可能已在 m_cache 中缺失，改用 originalPath 作为标识找回
            if (meta.isInvalid) {
                match = true;
                if (finalPath.empty() && !meta.originalPath.empty()) {
                    finalPath = meta.originalPath;
                }
            }
        } else {
            // 严禁显示失效数据
            if (meta.isInvalid) return;

            if (type == "all") {
                if (meta.isTrash) return; 
                match = true;
            }
            else {
                if (meta.isTrash) return;

                if (type == "untagged" && meta.tags.isEmpty()) match = true;
                else if (type == "recently_visited" && meta.atime >= now - 86400000.0) match = true;
                else if (type == "uncategorized" && !meta.fileId128.empty() && categorizedIds.find(meta.fileId128) == categorizedIds.end()) match = true;
            }
        }
        
        if (match && !finalPath.empty()) paths << QString::fromStdWString(finalPath);
    });
    return paths;
}

} // namespace ArcMeta
