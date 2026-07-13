#include "ConfigManager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace FERREX {

const QSet<QString> DEFAULT_BLACKLIST = {
    "exe", "dll", "sys", "bin", "dat", "lib", "obj", "msi", "com",
    "zip", "rar", "7z", "iso", "tar", "gz", "bz2", "dmg", "pkg",
    "mp3", "wav", "wma", "flac", "aac", "ogg", "m4a", "ape", "opus", "aiff", "amr",
    "mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "3gp", "ts", "rmvb", "rm", "vob"
};

const QSet<QString> DEFAULT_WHITELIST = {
    "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "eps", "pdf", "svg",
    "txt", "md", "markdown", "rst", "log", "nfo", "tex", "latex", "diff", "patch",
    "csv", "tsv", "html", "htm", "xhtml", "xml", "xsl", "xslt", "xaml",
    "json", "json5", "toml", "yaml", "yml", "ini", "conf", "cfg", "properties", "env",
    "c", "h", "cpp", "cc", "cxx", "c++", "hpp", "hh", "hxx", "h++", "inl", "ipp",
    "cs", "csx", "vb", "vbs", "vba",
    "java", "kt", "kts", "scala", "sc", "groovy", "gradle",
    "js", "mjs", "cjs", "jsx", "ts", "tsx", "vue", "svelte",
    "css", "scss", "sass", "less", "styl",
    "php", "php3", "php4", "php5", "phtml",
    "py", "pyw", "pyi",
    "rb", "rbw", "rake",
    "pl", "pm", "pod", "t",
    "lua",
    "tcl", "tk",
    "r", "rmd",
    "m",
    "jl",
    "sh", "bash", "zsh", "fish", "ksh", "csh",
    "bat", "cmd",
    "ps1", "psm1", "psd1", "ps1xml",
    "ahk", "ahk2",
    "au3",
    "nsi", "nsh",
    "iss",
    "reg",
    "cmake", "make", "mk", "makefile",
    "dockerfile",
    "sql", "mysql", "pgsql", "plsql",
    "hs", "lhs",
    "erl", "hrl",
    "clj", "cljs", "cljc",
    "lisp", "el", "scm", "ss",
    "f", "for", "f90", "f95", "f03",
    "d",
    "pas", "pp", "inc",
    "swift",
    "go",
    "rs",
    "dart",
    "zig",
    "nim",
    "cr",
    "ex", "exs",
    "coffee",
    "as",
    "ada", "adb", "ads",
    "asm", "s", "nasm",
    "v", "sv", "svh",
    "vhd", "vhdl",
    "pro",
    "sas",
    "matlab",
    "cob", "cbl",
    "bas", "frm", "cls",
    "asp", "aspx",
    "jsp"
};

void ScanConfig::load() {
    previewBlacklist = DEFAULT_BLACKLIST;
    previewWhitelist = DEFAULT_WHITELIST;

    QFile file("FERREX_scan_config.json");
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject root = doc.object();
        
        QJsonObject obj = root["ScanDialog"].toObject();
        if (obj.isEmpty()) obj = root;
        
        auto loadSet = [&](const QString& key, QSet<QString>& set) {
            set.clear();
            QJsonArray arr = obj[key].toArray();
            for (const auto& v : arr) set.insert(v.toString());
        };
        
        loadSet("activeDrives", activeDrives);
        loadSet("defaultDrives", defaultDrives);
        
        queryHistory.clear();
        QJsonArray qArr = obj["queryHistory"].toArray();
        for (const auto& v : qArr) {
            QString val = v.toString();
            if (!queryHistory.contains(val)) {
                queryHistory.append(val);
            }
        }
        while (queryHistory.size() > 10) {
            queryHistory.removeLast();
        }
        
        extHistory.clear();
        QJsonArray eArr = obj["extHistory"].toArray();
        for (const auto& v : eArr) {
            QString val = v.toString();
            if (!extHistory.contains(val)) {
                extHistory.append(val);
            }
        }
        while (extHistory.size() > 10) {
            extHistory.removeLast();
        }
        
        if (obj.contains("previewBlacklist")) loadSet("previewBlacklist", previewBlacklist);
        if (obj.contains("previewWhitelist")) loadSet("previewWhitelist", previewWhitelist);

        if (obj.contains("viewMode")) viewMode = obj["viewMode"].toInt();
        if (obj.contains("iconSize")) iconSize = obj["iconSize"].toInt();
        if (obj.contains("layoutMode")) layoutMode = obj["layoutMode"].toInt();
        if (obj.contains("sortColumn")) sortColumn = obj["sortColumn"].toInt();
        if (obj.contains("sortOrder")) sortOrder = obj["sortOrder"].toInt();

        if (obj.contains("useRegex")) useRegex = obj["useRegex"].toBool();
        if (obj.contains("caseSensitive")) caseSensitive = obj["caseSensitive"].toBool();
        if (obj.contains("includeHidden")) includeHidden = obj["includeHidden"].toBool();
        if (obj.contains("includeSystem")) includeSystem = obj["includeSystem"].toBool();
        if (obj.contains("includeDollar")) includeDollar = obj["includeDollar"].toBool();
        if (obj.contains("autoDisplay")) autoDisplay = obj["autoDisplay"].toBool();
    }
}

void ScanConfig::save() {
    QJsonObject root;
    QFile readFile("FERREX_scan_config.json");
    if (readFile.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(readFile.readAll()).object();
        readFile.close();
    }

    QFile file("FERREX_scan_config.json");
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonObject obj;
        auto saveSet = [&](const QString& key, const QSet<QString>& set) {
            QJsonArray arr;
            for (const auto& v : set) arr.append(v);
            obj[key] = arr;
        };
        
        saveSet("activeDrives", activeDrives);
        saveSet("defaultDrives", defaultDrives);
        
        while (queryHistory.size() > 10) {
            queryHistory.removeLast();
        }
        QJsonArray qArr; for (const auto& v : queryHistory) qArr.append(v);
        obj["queryHistory"] = qArr;
        
        while (extHistory.size() > 10) {
            extHistory.removeLast();
        }
        QJsonArray eArr; for (const auto& v : extHistory) eArr.append(v);
        obj["extHistory"] = eArr;
        
        saveSet("previewBlacklist", previewBlacklist);
        saveSet("previewWhitelist", previewWhitelist);

        obj["viewMode"] = viewMode;
        obj["iconSize"] = iconSize;
        obj["layoutMode"] = layoutMode;
        obj["sortColumn"] = sortColumn;
        obj["sortOrder"] = sortOrder;

        obj["useRegex"] = useRegex;
        obj["caseSensitive"] = caseSensitive;
        obj["includeHidden"] = includeHidden;
        obj["includeSystem"] = includeSystem;
        obj["includeDollar"] = includeDollar;
        obj["autoDisplay"] = autoDisplay;
        
        root["ScanDialog"] = obj;
        file.write(QJsonDocument(root).toJson());
    }
}

} // namespace FERREX
