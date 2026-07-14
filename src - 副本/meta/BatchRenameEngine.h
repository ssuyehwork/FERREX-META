#pragma once

#include <string>
#include <vector>
#include <QString>

namespace FERREX {

enum class RenameComponentType {
    Text,           
    Sequence,       
    Date,           
    OriginalName,   
    Metadata        
};

struct RenameRule {
    RenameComponentType type;
    QString value;      
    int start = 1;      
    int step = 1;       
    int padding = 3;    
};

class BatchRenameEngine {
public:
    static BatchRenameEngine& instance();

    std::vector<std::wstring> preview(const std::vector<std::wstring>& originalPaths, const std::vector<RenameRule>& rules);

    bool execute(const std::vector<std::wstring>& originalPaths, const std::vector<RenameRule>& rules);

private:
    BatchRenameEngine() = default;
    ~BatchRenameEngine() = default;

    QString processOne(const std::wstring& path, int index, const std::vector<RenameRule>& rules);
};

} 
