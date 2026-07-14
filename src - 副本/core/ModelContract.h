#pragma once
#include <Qt>

namespace FERREX {

enum CommonRole {
    
    IdRole              = Qt::UserRole + 1,  
    NameRole            = Qt::UserRole + 2,  
    PathRole            = Qt::UserRole + 3,  

    IsLockedRole        = Qt::UserRole + 102, 
    EncryptHintRole     = Qt::UserRole + 104, 
    InDatabaseRole      = Qt::UserRole + 105, 
    IsEmptyRole         = Qt::UserRole + 106, 
    CategoryIdRole      = Qt::UserRole + 107, 

    AspectRatioRole     = Qt::UserRole + 201, 
    HasThumbnailRole    = Qt::UserRole + 202, 
    PalettesRole        = Qt::UserRole + 203, 
    CountRole           = Qt::UserRole + 204  
};

} 
