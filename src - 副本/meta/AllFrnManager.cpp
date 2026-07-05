#include "AllFrnManager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <shared_mutex>
#include <QDebug>

namespace ArcMeta {

static std::shared_mutex s_frnMutex;

void AllFrnManager::registerFrn(const std::wstring& frn, const std::wstring& path) {
    if (frn.empty() || path.empty()) return;

    QString qFrn = QString::fromStdWString(frn);
    QString qPath = QString::fromStdWString(path);

    std::unique_lock<std::shared_mutex> lock(s_frnMutex);
    
    QJsonObject root;
    QFile file("All_FRN_am_meta.json");
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            root = doc.object();
        }
        file.close();
    }

    QJsonObject frnsObj = root["frns"].toObject();
    // 物理防冲突：如果已登记，且路径相同，则无需重复写盘
    if (frnsObj.contains(qFrn) && frnsObj[qFrn].toString() == qPath) {
        return; 
    }

    frnsObj[qFrn] = qPath;
    root["frns"] = frnsObj;

    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
        qDebug() << "[AllFrnManager] 成功登记 FRN:" << qFrn << "路径:" << qPath;
    } else {
        qWarning() << "[AllFrnManager] 无法写入 All_FRN_am_meta.json";
    }
}

QMap<QString, QString> AllFrnManager::getAllFrns() {
    QMap<QString, QString> result;
    std::shared_lock<std::shared_mutex> lock(s_frnMutex);

    QFile file("All_FRN_am_meta.json");
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            QJsonObject frnsObj = root["frns"].toObject();
            for (auto it = frnsObj.begin(); it != frnsObj.end(); ++it) {
                result[it.key()] = it.value().toString();
            }
        }
        file.close();
    }
    return result;
}

} // namespace ArcMeta
