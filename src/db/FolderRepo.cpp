#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "FolderRepo.h"

namespace ArcMeta {

bool FolderRepo::save(const std::wstring& volume, const std::wstring& path, const FolderMeta& meta) {
    // 2026-03-xx 修复：通过 getThreadDatabase 获取当前线程专属连接，确保在后台同步任务中正确持久化。
    QSqlDatabase db = ArcMeta::Database::instance().getThreadDatabase();
    QSqlQuery q(db);
    
    q.prepare("INSERT OR REPLACE INTO folders (volume, path, rating, color, tags, pinned, note, sort_by, sort_order, encrypted, encrypt_salt, encrypt_iv, encrypt_verify_hash, file_id_128, palettes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    q.addBindValue(QString::fromStdWString(volume));
    q.addBindValue(QString::fromStdWString(path));
    q.addBindValue(meta.rating);
    q.addBindValue(QString::fromStdWString(meta.color));
    
    QJsonArray tagsArr;
    for (const auto& t : meta.tags) tagsArr.append(QString::fromStdWString(t));
    q.addBindValue(QJsonDocument(tagsArr).toJson(QJsonDocument::Compact));
    
    q.addBindValue(meta.pinned ? 1 : 0);
    q.addBindValue(QString::fromStdWString(meta.note));
    q.addBindValue(QString::fromStdWString(meta.sortBy));
    q.addBindValue(QString::fromStdWString(meta.sortOrder));
    q.addBindValue(meta.encrypted ? 1 : 0);
    q.addBindValue(QString::fromStdString(meta.encryptSalt));
    q.addBindValue(QString::fromStdString(meta.encryptIv));
    q.addBindValue(QString::fromStdString(meta.encryptVerifyHash));
    q.addBindValue(QString::fromStdString(meta.fileId128));
    
    QJsonArray palArr;
    for (const auto& p : meta.palettes) {
        QJsonObject pObj;
        QJsonArray cArr;
        cArr.append(p.color.red()); cArr.append(p.color.green()); cArr.append(p.color.blue());
        pObj.insert("color", cArr);
        pObj.insert("ratio", (double)p.ratio);
        palArr.append(pObj);
    }
    q.addBindValue(QJsonDocument(palArr).toJson(QJsonDocument::Compact));

    return q.exec();
}

bool FolderRepo::get(const std::wstring& volume, const std::wstring& path, FolderMeta& meta) {
    QSqlDatabase db = ArcMeta::Database::instance().getThreadDatabase();
    QSqlQuery q(db);
    q.prepare("SELECT rating, color, tags, pinned, note, sort_by, sort_order, encrypted, encrypt_salt, encrypt_iv, encrypt_verify_hash, file_id_128, palettes FROM folders WHERE volume = ? AND path = ?");
    q.addBindValue(QString::fromStdWString(volume));
    q.addBindValue(QString::fromStdWString(path));
    
    if (q.exec() && q.next()) {
        meta.rating = q.value(0).toInt();
        meta.color = q.value(1).toString().toStdWString();
        
        QJsonDocument doc = QJsonDocument::fromJson(q.value(2).toByteArray());
        meta.tags.clear();
        if (doc.isArray()) {
            for (const auto& v : doc.array()) meta.tags.push_back(v.toString().toStdWString());
        }

        meta.pinned = q.value(3).toInt() != 0;
        meta.note = q.value(4).toString().toStdWString();
        meta.sortBy = q.value(5).toString().toStdWString();
        meta.sortOrder = q.value(6).toString().toStdWString();
        meta.encrypted = q.value(7).toInt() != 0;
        meta.encryptSalt = q.value(8).toString().toStdString();
        meta.encryptIv = q.value(9).toString().toStdString();
        meta.encryptVerifyHash = q.value(10).toString().toStdString();
        meta.fileId128 = q.value(11).toString().toStdString();
        
        QJsonDocument palDoc = QJsonDocument::fromJson(q.value(12).toByteArray());
        meta.palettes.clear();
        if (palDoc.isArray()) {
            for (const auto& v : palDoc.array()) {
                QJsonObject pObj = v.toObject();
                QJsonArray cArr = pObj.value("color").toArray();
                if (cArr.size() >= 3) {
                    meta.palettes.push_back({QColor(cArr[0].toInt(), cArr[1].toInt(), cArr[2].toInt()), (float)pObj.value("ratio").toDouble()});
                }
            }
        }
        return true;
    }
    return false;
}

bool FolderRepo::remove(const std::wstring& volume, const std::wstring& path) {
    QSqlDatabase db = ArcMeta::Database::instance().getThreadDatabase();
    QSqlQuery q(db);
    q.prepare("DELETE FROM folders WHERE volume = ? AND path = ?");
    q.addBindValue(QString::fromStdWString(volume));
    q.addBindValue(QString::fromStdWString(path));
    return q.exec();
}

} // namespace ArcMeta
