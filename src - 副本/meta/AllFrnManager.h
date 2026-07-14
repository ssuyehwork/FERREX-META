#pragma once

#include <QString>
#include <QMap>
#include <string>

namespace FERREX {

class AllFrnManager {
public:

    static void registerFrn(const std::wstring& frn, const std::wstring& path);

    static QMap<QString, QString> getAllFrns();
};

} 
