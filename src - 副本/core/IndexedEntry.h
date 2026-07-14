#pragma once

#include <QString>

namespace FERREX {

struct IndexedEntry {
    QString name;           
    int64_t size = 0;       
    int64_t modifyTime = 0; 
    int parentIndex = -1;   
    unsigned __int64 parentFrn = 0; 
    bool isDir = false;     
    uint32_t attributes = 0; 
    unsigned __int64 frn = 0; 

    QString suffix() const {
        if (isDir) return QString();
        int pos = name.lastIndexOf('.');
        if (pos == -1) return QString();
        return name.mid(pos + 1).toLower();
    }
};

} 
