#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ScanDialog.h"
#include "IScanResultView.h"
#include "ListResultView.h"
#include "JustifiedResultView.h"
#include "GridResultView.h"
#include <QDataStream>
#include "../core/CacheManager.h"
#include <QPainter>
#include <QTimer>
#include <QIcon>
#include "../mft/MftReader.h"
#include "UiHelper.h"
#include "../util/ShellHelper.h"
#include "../meta/MetadataManager.h"
#include <QFileInfo>
#include <QCheckBox>
#include <QFrame>
#include <QProgressBar>
#include <QFuture>
#include <QFutureWatcher>
#include <QCloseEvent>
#include <QWheelEvent>
#include <QLineEdit>
#include <QTextEdit>
#include <QTableView>
#include <QAbstractTableModel>
#include <QSvgRenderer>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QWindow>
#include <QStyle>
#include <QDateTime>
#include <algorithm>
#include <execution>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QClipboard>
#include <QShortcut>
#include <QApplication>
#include <QProcess>
#include <QMessageBox>
#include <QInputDialog>
#include <QPointer>
#include <QThreadStorage>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrent>
#include <QDir>
#include <QReadLocker>
#include <QWriteLocker>
#include <numeric>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <ntddstor.h>

#include "ScanController.h"
#include "JustifiedView.h"
#include "ThumbnailDelegate.h"
#include "QuickLookWindow.h"
#include "ResizeEventFilter.h"
#include "ToolTipOverlay.h"
#include "HoverEventFilter.h"
#include <memory>
#include <algorithm>
#include <QWidgetAction>

namespace FERREX {

// ============================================================================
// [历史重构] 专门用于下来面板中展示历史选项和右侧删除按钮的自定义微控件
// ============================================================================
class HistoryItemWidget : public QWidget {
public:
    HistoryItemWidget(const QString& text, bool isQuery, QMenu* parentMenu, ScanDialog* dialog, QWidget* parent = nullptr)
        : QWidget(parent), m_text(text), m_isQuery(isQuery), m_parentMenu(parentMenu), m_dialog(dialog)
    {
        // 开启整行悬停高亮样式
        this->setStyleSheet(
            "HistoryItemWidget { background-color: transparent; } "
            "HistoryItemWidget:hover { background-color: #2A2A2A; }"
        );

        // 采用极致扁平与紧凑的布局结构，为窄下拉框（如 120px 的后缀框）腾出完美空间
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 1, 4, 1);
        layout->setSpacing(4);

        // A. 左侧：条目文本按钮（扁平设计，点击后将内容填入主搜索框，并执行搜索）
        auto* btnText = new QPushButton(m_text, this);
        btnText->setCursor(Qt::PointingHandCursor);
        btnText->setStyleSheet(
            "QPushButton { background: transparent; border: none; color: #CCCCCC; text-align: left; font-size: 12px; padding: 4px 0; } "
            "QPushButton:hover { color: #FFFFFF; }"
        );
        connect(btnText, &QPushButton::clicked, this, &HistoryItemWidget::onSelectTriggered);
        layout->addWidget(btnText, 1); // 占据所有的剩余伸缩空间

        // B. 右侧：精致的 “×” 单项删除按钮 (对应用户原话：“下来面板的每一个选项右侧都应该有一个“×”，这样的话用户就可以轻松移除某个选项了”)
        auto* btnDelete = new QPushButton("×", this);
        btnDelete->setFixedSize(18, 18);
        btnDelete->setCursor(Qt::PointingHandCursor);
        btnDelete->setToolTip("移除该历史记录");
        btnDelete->setStyleSheet(
            "QPushButton { background: transparent; border: none; color: #888888; font-size: 14px; font-weight: bold; border-radius: 3px; line-height: 18px; } "
            "QPushButton:hover { color: #FFFFFF; background-color: #E81123; }" // 悬停显红
        );
        connect(btnDelete, &QPushButton::clicked, this, &HistoryItemWidget::onDeleteTriggered);
        layout->addWidget(btnDelete);
    }

private:
    void onSelectTriggered() {
        if (m_dialog) {
            m_dialog->setHistoryText(m_text, m_isQuery);
        }
        if (m_parentMenu) {
            m_parentMenu->close(); // 选中后关闭下拉菜单
        }
    }

    void onDeleteTriggered() {
        if (m_dialog) {
            m_dialog->removeHistoryItem(m_text, m_isQuery);
        }
        if (m_parentMenu) {
            m_parentMenu->close(); // 删除后立即关闭旧菜单，外界会重新唤醒最新菜单以完成无缝重绘
            // 2026-07-11 优化：使用异步 QTimer 触发重新打开，使当前 QMenu::exec 循环能够完全平稳退出，避免嵌套事件循环风险
            // 为避免 QMenu 析构导致 itemWidget (this) 被销毁从而引发 Use-After-Free 崩溃，
            // 此处必须按值捕获 isQuery 和 dialog，而不是捕获 this 指针。
            bool isQuery = m_isQuery;
            ScanDialog* dialog = m_dialog;
            if (dialog) {
                QTimer::singleShot(50, dialog, [dialog, isQuery]() {
                    dialog->reopenHistoryMenu(isQuery);
                });
            }
        }
    }

    QString m_text;
    bool m_isQuery;
    QMenu* m_parentMenu;
    ScanDialog* m_dialog;
};

} // namespace FERREX

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef run
#undef run
#endif


namespace FERREX {

// 2026-07-11 物理移植自 ArcMeta (Plan-179)：出厂默认配置
static const QSet<QString> DEFAULT_BLACKLIST = {
    // 系统 / 可执行 / 压缩
    "exe", "dll", "sys", "bin", "dat", "lib", "obj", "msi", "com",
    "zip", "rar", "7z", "iso", "tar", "gz", "bz2", "dmg", "pkg",
    // 音频类（由默认播放器处理）
    "mp3", "wav", "wma", "flac", "aac", "ogg", "m4a", "ape", "opus", "aiff", "amr",
    // 视频类（由默认播放器处理）
    "mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "3gp", "ts", "rmvb", "rm", "vob"
};

static const QSet<QString> DEFAULT_WHITELIST = {
    // 图像类
    "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "eps", "pdf", "svg",
    // 纯文本 / 标记语言
    "txt", "md", "markdown", "rst", "log", "nfo", "tex", "latex", "diff", "patch",
    "csv", "tsv", "html", "htm", "xhtml", "xml", "xsl", "xslt", "svg", "xaml",
    "json", "json5", "toml", "yaml", "yml", "ini", "conf", "cfg", "properties", "env",
    // C / C++ 系
    "c", "h", "cpp", "cc", "cxx", "c++", "hpp", "hh", "hxx", "h++", "inl", "ipp",
    // C# / .NET
    "cs", "csx", "vb", "vbs", "vba",
    // Java / Kotlin / Scala / Groovy
    "java", "kt", "kts", "scala", "sc", "groovy", "gradle",
    // Web 前端
    "js", "mjs", "cjs", "jsx", "ts", "tsx", "vue", "svelte",
    "css", "scss", "sass", "less", "styl",
    "php", "php3", "php4", "php5", "phtml",
    // 脚本语言
    "py", "pyw", "pyi",          // Python
    "rb", "rbw", "rake",          // Ruby
    "pl", "pm", "pod", "t",       // Perl / Raku
    "lua",                         // Lua
    "tcl", "tk",                   // TCL
    "r", "rmd",                    // R
    "m",                           // MATLAB / Objective-C
    "jl",                          // Julia
    // 系统 / 自动化脚本
    "sh", "bash", "zsh", "fish", "ksh", "csh",   // Shell
    "bat", "cmd",                                   // Windows Batch
    "ps1", "psm1", "psd1", "ps1xml",              // PowerShell
    "ahk", "ahk2",                                 // AutoHotkey
    "au3",                                         // AutoIt
    "nsi", "nsh",                                  // NSIS
    "iss",                                         // Inno Setup
    "reg",                                         // Windows Registry
    // 系统 / 编译工具
    "cmake", "make", "mk", "makefile",
    "dockerfile",
    // 数据库
    "sql", "mysql", "pgsql", "plsql",
    // 函数式 / 其他语言
    "hs", "lhs",                   // Haskell
    "erl", "hrl",                  // Erlang
    "clj", "cljs", "cljc",         // Clojure
    "lisp", "el", "scm", "ss",     // Lisp / Scheme
    "f", "for", "f90", "f95", "f03", // Fortran
    "d",                            // D
    "pas", "pp", "inc",            // Pascal / Delphi
    "swift",                        // Swift
    "go",                           // Go
    "rs",                           // Rust
    "dart",                         // Dart
    "zig",                          // Zig
    "nim",                          // Nim
    "cr",                           // Crystal
    "ex", "exs",                    // Elixir
    "coffee",                       // CoffeeScript
    "as",                           // ActionScript
    "ada", "adb", "ads",           // Ada
    "asm", "s", "nasm",            // Assembly
    "v", "sv", "svh",              // Verilog / SystemVerilog
    "vhd", "vhdl",                 // VHDL
    "pro",                          // Prolog
    "sas",                          // SAS
    "matlab",                       // MATLAB (alt)
    "cob", "cbl",                  // COBOL
    "bas", "frm", "cls",           // VB Classic / FreeBasic
    "asp", "aspx",                 // ASP
    "jsp",                          // JSP
};

// 2026-07-11 物理移植自 ArcMeta (Plan-179)：前置文件类型准入哨兵
// 按下空格键时，先通过此函数判断文件是否支持预览，不支持则直接 return，绝不弹出预览窗
static bool isPathPreviewable(const QString& path, const ScanConfig& config) {
    QFileInfo info(path);
    if (info.isDir()) return false; // 文件夹直接拦截

    QString ext = info.suffix().toLower();
    if (config.previewBlacklist.contains(ext)) return false;
    return config.previewWhitelist.contains(ext);
}



// --- ScanTableModel Implementation ---

ScanTableModel::ScanTableModel(ScanController* controller, QObject* parent) 
    : QAbstractTableModel(parent), m_controller(controller) 
{
    m_currentResultSet = std::make_shared<ResultSet>();

    // 建立隔离的缩略图任务专用线程池，避免与主后台任务竞争资源
    m_thumbPool = new QThreadPool(this);
    
    // 2026-06-xx 任务三：磁盘类型感知的线程调度
    // 默认保守策略：若无法获取配置或存在 HDD，则使用串行模式 (1) 保护寻道性能
    bool allSSD = true;
    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent);
    if (dlg) {
        // 2026-06-xx 物理修复：使用 ShellHelper::isSolidStateDrive 统一探测
        for (const QString& d : dlg->m_config.activeDrives) {
            if (!ShellHelper::isSolidStateDrive(d)) {
                allSSD = false;
                break;
            }
        }
    } else {
        allSSD = false; // 未知环境下保守处理
    }

    if (allSSD) {
        // 对于 SSD，设置并发上限为理想线程数的一半，平衡系统负载
        m_thumbPool->setMaxThreadCount(std::max<int>(1, QThread::idealThreadCount() / 2));
    } else {
        // 对于 HDD，保持串行以减少寻道开销
        m_thumbPool->setMaxThreadCount(1);
    }

    m_thumbCache.setMaxCost(500); // 限制缩略图内存占用
    m_throttleTimer = new QTimer(this);
    m_throttleTimer->setInterval(100); 

    m_metadataTimer = new QTimer(this);
    m_metadataTimer->setInterval(150); // 150ms 视口防抖
    m_metadataTimer->setSingleShot(true);
    connect(m_metadataTimer, &QTimer::timeout, this, [this]() {
        if (m_visibleTop < 0 || m_visibleBottom < 0) return;
        
        // 2026-06-xx 物理修复：视口扫描异步化。
        // 理由：虽然 requestMetadata 是异步的，但其内部会触发 MftReader 的读写锁申请。
        // 在 220万数据下，如果 UI 线程密集触发 lock 申请，会造成明显的微卡顿甚至假死。
        auto snap = m_currentResultSet;
        int top = m_visibleTop;
        int bottom = m_visibleBottom;

        (void)QtConcurrent::run([snap, top, bottom]() {
            auto& reader = MftReader::instance();
            for (int i = top; i <= bottom; ++i) {
                if (i >= (int)snap->keys.size()) break;
                uint64_t key = snap->keys[i];
                int idx = reader.getIndexByKey(key);
                if (idx != -1 && !reader.isMetadataFetched(idx)) {
                    const_cast<MftReader&>(reader).requestMetadata(idx);
                }
            }
        });
    });

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setInterval(20); // 20ms 任务归并窗口
    m_thumbTimer->setSingleShot(true);
    connect(m_thumbTimer, &QTimer::timeout, this, &ScanTableModel::processThumbQueue);

    connect(m_throttleTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingRows.isEmpty()) return;
        QList<int> rows = m_pendingRows.values();
        std::sort(rows.begin(), rows.end());
        m_pendingRows.clear();

        for (int i = 0; i < rows.size(); ) {
            int startRow = rows[i];
            int endRow = rows[i];
            int j = i + 1;
            while (j < rows.size() && rows[j] == endRow + 1) {
                endRow = rows[j];
                j++;
            }
            
            // 2026-06-xx 物理加固：在发射 dataChanged 前强制核对行号边界，防止越界触发断言
            if (startRow >= 0 && endRow < m_displayCount) {
                emit dataChanged(index(startRow, 0), index(endRow, 3));
            }
            i = j;
        }
    });

    // 2026-06-xx 架构重构：切换至 Controller 驱动的原子快照更新 (使用信号携带的快照，绝对安全)
    connect(m_controller, &ScanController::resultsSwapped, this, [this](std::shared_ptr<ResultSet> newSet) {
        updateResults(newSet);
    });
}
ScanTableModel::~ScanTableModel() {
    if (m_thumbPool) {
        m_thumbPool->waitForDone();
    }
}

int ScanTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_displayCount;
}

int ScanTableModel::columnCount(const QModelIndex& /*parent*/) const { return 4; }

QVariant ScanTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return QVariant();
    int row = index.row();
    if (row < 0 || row >= (int)m_currentResultSet->keys.size()) return QVariant();
    
    uint64_t key = m_currentResultSet->keys[row];
    auto& reader = MftReader::instance();
    int actualIndex = reader.getIndexByKey(key);
    if (actualIndex == -1) return QVariant(); // 文件可能已被删除

    // 2026-06-xx 极致性能重构：行内计算缓存。
    // 理由：getFullPath() 是极其昂贵的递归操作且包含读锁，
    // 在一次 data() 调用中（或者同一行的多列渲染中）必须消除重复计算。
    thread_local static int lastRow = -1;
    thread_local static uint64_t lastKey = 0;
    thread_local static QString cachedPath;
    
    auto getPath = [&]() {
        if (lastRow == row && lastKey == key && !cachedPath.isEmpty()) return cachedPath;
        lastRow = row; lastKey = key;
        cachedPath = reader.getFullPath(actualIndex);
        return cachedPath;
    };
    
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case 0: return reader.getName(actualIndex);
            case 1: return getPath();
            case 2: {
                if (reader.isDirectory(actualIndex)) return "-";
                int64_t size = reader.getSize(actualIndex);
                if (size == 0 && !reader.isMetadataFetched(actualIndex)) {
                    return "...";
                }
                if (size < 1024) return QString("%1 B").arg(size);
                if (size < 1024 * 1024) return QString("%1 KB").arg(size / 1024.0, 0, 'f', 2);
                if (size < 1024LL * 1024 * 1024) return QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
                return QString("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
            }
            case 3: {
                int64_t ts = reader.getModifyTime(actualIndex);
                if (ts == 0 && !reader.isMetadataFetched(actualIndex)) {
                    return "-";
                }
                if (ts == 0) return "-";
                return QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
            }
        }
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        // 2026-06-xx 性能优化：对接 MftReader 预拆分的扩展名字段，消除 UI 层重复解析
        QString ext = reader.getExtQString(actualIndex);
        
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        if (thumbExts.contains(ext) && !reader.isDirectory(actualIndex)) {
            // 2026-06-xx 极致性能优化：使用 CompositeKey + Size + Mtime 构建 O(1) 的原子 CacheKey
            int64_t size = reader.getSize(actualIndex);
            int64_t mtime = reader.getModifyTime(actualIndex);
            QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(mtime);

            // 限制双轨缓存的最大容量，防止极端高频缩放累积内存
            if (m_lastPixmapCache.maxCost() == 0) {
                m_lastPixmapCache.setMaxCost(200); // 默认限制 200 项可见卡片 LRU 备份
            }

            // 1. 精确尺寸缓存匹配
            QPixmap* cached = m_thumbCache.object(cacheKey);
            if (cached) return *cached;

            // 2.【核心改进：先判断历史缩略图并做平滑拉伸】
            QPixmap* lastCached = m_lastPixmapCache.object(QString::number(key));
            if (lastCached) {
                // 后台静默生成符合全新精确尺寸的高画质大图
                if (!m_requestedThumbs.contains(key)) {
                    m_requestedThumbs.insert(key);
                    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                    int thumbSize = dlg ? dlg->m_config.iconSize : 64; // 不再对列表视图强行截断 24px，使其跟随滚轮联动缩放 [1]
                    m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                    if (!m_thumbTimer->isActive()) m_thumbTimer->start();
                }

                // 物理资产优先：直接返回原始的历史 Pixmap 资产，由 Delegate 进行后续 Cover/Contain 平滑拉伸，绝不闪现系统默认图标
                return *lastCached;
            }

            // C. 失败兜底阻断器：如果已经被标记为彻底提取失败，则可以穿透放行，退回到最下方的系统默认图标展示。
            if (m_failedThumbs.contains(key)) {
                return reader.getCachedIcon(ext, false);
            }

            // D. 加载期强制阻断方案：此时缩略图在加载队列中尚未产生。为了杜绝默认图标的插足闪跃，模型层强制返回“符合规范的空 QVariant()”。
            if (!m_requestedThumbs.contains(key)) {
                m_requestedThumbs.insert(key);
                ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
                int thumbSize = dlg ? dlg->m_config.iconSize : 64; // 不再对列表视图强行截断 24px，使其跟随滚轮联动缩放 [1]
                
                m_thumbTaskQueue.append({key, thumbSize, ext, cacheKey});
                if (!m_thumbTimer->isActive()) m_thumbTimer->start();
            }

            return QVariant(); // 【核心物理阻断点】向视图提供空数据，掐断默认图标的透传通路！
        }
        
        // 常规不支持缩略图的后缀（如 txt, exe），直接放行，回退到系统默认图标
        return reader.getCachedIcon(ext, reader.isDirectory(actualIndex));
    } else if (role == Qt::ForegroundRole) {
        // 2026-06-xx 极致性能重构：优先从结果集的预取元数据中获取颜色，消除磁盘 IO 风险
        auto it = m_currentResultSet->metadata.find(key);
        if (it != m_currentResultSet->metadata.end()) {
            return it->second.color;
        }

        // 2026-06-xx 兜底逻辑：若未预取，则计算路径查询，由于 getPath 带有行内缓存，性能依然可控
        QString qPath = getPath();
        auto meta = MetadataManager::instance().getMeta(qPath.toStdWString());
        if (!meta.color.empty()) {
            QColor tagC = UiHelper::parseColorName(QString::fromStdWString(meta.color));
            if (tagC.isValid()) return tagC;
        }
        // 2026-06-xx 按照用户要求：名称列（第0列）强制显示为蓝色
        if (index.column() == 0 || reader.isDirectory(actualIndex)) return QColor("#3498db");
    } else if (role == Qt::ToolTipRole) {
        // 2026-06-xx 极致性能重构：消除 ToolTipRole 中的重复路径回溯
        // 2026-07-12 物理对齐需求：ToolTipOverlay 显示的内容包含项目名称、路径、大小、修改时间 (并兼容备注和标签)
        QString name = reader.getName(actualIndex);
        QString qPath = getPath();
        
        QString sizeStr;
        if (reader.isDirectory(actualIndex)) {
            sizeStr = "-";
        } else {
            int64_t size = reader.getSize(actualIndex);
            if (size == 0 && !reader.isMetadataFetched(actualIndex)) {
                sizeStr = "...";
            } else if (size < 1024) {
                sizeStr = QString("%1 B").arg(size);
            } else if (size < 1024 * 1024) {
                sizeStr = QString("%1 KB").arg(size / 1024.0, 0, 'f', 2);
            } else if (size < 1024LL * 1024 * 1024) {
                sizeStr = QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 2);
            } else {
                sizeStr = QString("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
            }
        }

        QString mtimeStr;
        int64_t ts = reader.getModifyTime(actualIndex);
        if (ts == 0 && !reader.isMetadataFetched(actualIndex)) {
            mtimeStr = "-";
        } else if (ts == 0) {
            mtimeStr = "-";
        } else {
            mtimeStr = QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm");
        }

        QString tip = QString::fromUtf8("名称: ") + name + "\n" +
                      QString::fromUtf8("路径: ") + qPath + "\n" +
                      QString::fromUtf8("大小: ") + sizeStr + "\n" +
                      QString::fromUtf8("修改时间: ") + mtimeStr;

        return tip;
    } else if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
            case 0: case 1: return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
            case 2: case 3: return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
    } else if (role == Qt::UserRole) {
        return key;
    } else if (role == Qt::UserRole + 1) {
        // 返回缩略图物理资产状态：0=未就绪/不支持, 1=有可用缩略图 (用于 Delegate 实施“缩略图第一优先、系统图标靠后兜底”绘制)
        // 对接 MftReader 预拆分字段
        QString ext = reader.getExtQString(actualIndex);
        static const QSet<QString> thumbExts = {"psd", "ai", "eps", "jpg", "jpeg", "png", "webp", "svg"};
        
        if (!thumbExts.contains(ext) || reader.isDirectory(actualIndex)) return 0;

        int64_t size = reader.getSize(actualIndex);
        int64_t mtime = reader.getModifyTime(actualIndex);
        QString cacheKey = QString("%1_%2_%3").arg(key).arg(size).arg(mtime);
        
        // 只要 L1 精确匹配命中，或者 L2 历史备份可用，即视为存在可用物理缩略图资产并返回线索 1，彻底删除任何多余的过渡加载状态。
        if (m_thumbCache.contains(cacheKey) || m_lastPixmapCache.contains(QString::number(key))) {
            return 1;
        }
        return 0;
    } else if (role == Qt::UserRole + 2) {
        // 返回宽高比 (用于 JustifiedView 布局)
        // 2026-07-11 物理重构：自适应模式仅限于视频和图形图像文件，文件夹与其余常规文件直接返回 -1.0 禁用自适应拉伸 (对应用户原话：“所谓的自适应仅限于视频、图形图像，除此之外仅剩下常规文件类型了”)
        if (reader.isDirectory(actualIndex)) {
            return -1.0;
        }

        QString ext = reader.getExtQString(actualIndex).toLower();
        static const QSet<QString> mediaExts = {
            // 图形图像类
            "jpg", "jpeg", "png", "bmp", "webp", "gif", "ico", "psd", "ai", "eps", "pdf", "svg",
            // 视频类
            "mp4", "m4v", "mov", "avi", "mkv", "wmv", "flv", "webm", "3gp", "ts", "rmvb", "rm", "vob"
        };

        if (!mediaExts.contains(ext)) {
            return -1.0; // 常规文件类型，不提供有效正数宽高比，禁用自适应拉伸
        }

        return m_aspectRatios.value(key, 1.0);
    }
    return QVariant();
}

Qt::ItemFlags ScanTableModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() == 0) {
        // 2026-05-16 物理对标：仅名称列允许行内编辑
        f |= Qt::ItemIsEditable;
    }
    return f;
}

bool ScanTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || role != Qt::EditRole || index.column() != 0) return false;
    
    int row = index.row();
    if (row < 0 || row >= (int)m_currentResultSet->keys.size()) return false;
    
    uint64_t key = m_currentResultSet->keys[row];
    auto& reader = MftReader::instance();
    int actualIndex = reader.getIndexByKey(key);
    if (actualIndex == -1) return false;
    
    QString oldName = reader.getName(actualIndex);
    QString newName = value.toString().trimmed();
    if (newName.isEmpty() || newName == oldName) return false;
    
    QString oldPath = reader.getFullPath(actualIndex);
    QFileInfo fi(oldPath);
    QString newPath = fi.absolutePath() + QLatin1String("/") + newName;
    
    if (QFile::rename(oldPath, newPath)) {
        // 2026-05-16 交互加固：物理重命名后，USN 监听器会捕获事件并自动更新模型。
        // 我们在此处不需要手动修改内存池，等待系统级同步最为稳健。
        return true;
    } else {
        QMessageBox::warning(nullptr, "重命名失败", "无法重命名文件，请检查文件是否被占用或是否有权限。");
        return false;
    }
}

QVariant ScanTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
            case 0: return "名称";
            case 1: return "路径";
            case 2: return "大小";
            case 3: return "修改日期";
        }
    }
    return QVariant();
}

void ScanTableModel::updateResults(std::shared_ptr<ResultSet> nextSet) {
    auto baseSet = nextSet ? nextSet : m_controller->snapshot();
    auto newSet = std::make_shared<ResultSet>();
    newSet->metadata = baseSet->metadata;
    newSet->keyToPos = baseSet->keyToPos;

    ScanDialog* dlg = qobject_cast<ScanDialog*>(parent());
    bool isMediaView = false;
    if (dlg) {
        // viewMode == 1 代表自适应与网格的多媒体画廊视图，viewMode == 0 代表全文件列表视图
        isMediaView = (dlg->m_config.viewMode == 1);
    }

    if (isMediaView) {
        // 自适应与网格模式必须遵循其媒体画廊视图本身的展示使命与渲染承载力，在源头只保留视频与图像
        auto& reader = MftReader::instance();
        static const QSet<QString> mediaExts = {
            "jpg", "jpeg", "png", "bmp", "gif", "webp", "svg", "psd", "ai", "eps",
            "mp4", "mkv", "avi", "mov", "flv", "rmvb", "wmv", "webm"
        };

        newSet->keys.reserve(baseSet->keys.size() / 2);
        for (uint64_t key : baseSet->keys) {
            int actualIndex = reader.getIndexByKey(key);
            if (actualIndex == -1) continue;

            // 剔除所有文件夹以及不属于画廊美学展示范围的常规普通非媒体文件
            if (reader.isDirectory(actualIndex)) continue;

            QString ext = reader.getExtQString(actualIndex).toLower();
            if (mediaExts.contains(ext)) {
                newSet->keys.push_back(key);
            }
        }

        // 重建过滤后结果集的 O(1) 反向索引映射
        newSet->keyToPos.clear();
        for (size_t i = 0; i < newSet->keys.size(); ++i) {
            newSet->keyToPos[newSet->keys[i]] = i;
        }
    } else {
        // 列表模式：保留全量普通文件、文件夹及多媒体过滤数据
        newSet->keys = baseSet->keys;
    }

    int oldSize = (int)m_currentResultSet->keys.size();
    int newSize = (int)newSet->keys.size();

    // 2026-06-xx 极致性能重构：Diffing 局部刷新。
    // 物理铁律：在 emit 信号之前必须确保 m_currentResultSet 已更新，
    // 且信号范围必须与数据量绝对对齐，否则 TableView 内部索引越界会导致程序无响应（假死）。
    
    // 如果变动巨大或初始加载，或者模式切换导致的数据量落差，回退到 Reset 模式
    if (oldSize == 0 || std::abs(newSize - oldSize) > 500 || isMediaView != (oldSize != (int)baseSet->keys.size())) {
        beginResetModel();
        m_currentResultSet = newSet;
        m_displayCount = newSize; 
        m_requestedThumbs.clear();
        m_failedThumbs.clear(); // 2026-07-xx 重置时也必须清理失败跟踪，避免由于路径变动或磁盘更新造成不可恢复的阻断
        m_pendingRows.clear(); // 2026-06-xx 任务修复：重置时必须清空待刷新行，防止索引失效
        endResetModel();
        return;
    }

    if (newSize > oldSize) {
        beginInsertRows(QModelIndex(), oldSize, newSize - 1);
        m_currentResultSet = newSet;
        m_displayCount = newSize; 
        endInsertRows();
    } else if (newSize < oldSize) {
        beginRemoveRows(QModelIndex(), newSize, oldSize - 1);
        m_currentResultSet = newSet;
        m_displayCount = newSize;
        endRemoveRows();
    } else if (newSize > 0) {
        m_currentResultSet = newSet;
        emit dataChanged(index(0, 0), index(newSize - 1, 3));
    } else {
        m_currentResultSet = newSet;
    }
}

bool ScanTableModel::canFetchMore(const QModelIndex& parent) const {
    Q_UNUSED(parent);
    return false;
}

void ScanTableModel::fetchMore(const QModelIndex& parent) {
    Q_UNUSED(parent);
}

void ScanTableModel::setVisibleRange(int top, int bottom) {
    m_visibleTop = top;
    m_visibleBottom = bottom;
    m_metadataTimer->start();
}

void ScanTableModel::forceFetchAll() {
    int total = (int)m_currentResultSet->keys.size();
    if (m_displayCount >= total) return;
    
    beginInsertRows(QModelIndex(), m_displayCount, total - 1);
    m_displayCount = total;
    endInsertRows();
}

void ScanTableModel::processThumbQueue() {
    if (m_thumbTaskQueue.isEmpty()) return;

    // 2026-06-xx 任务 4.3：LIFO 优先级调度。
    // 理由：用户通常关注滚动停止后的可视区域，后加入队列的请求往往更具时效性。
    auto currentTasks = std::move(m_thumbTaskQueue);
    std::reverse(currentTasks.begin(), currentTasks.end());

    // 使用独立线程池异步执行缩略图提取，不使用全局线程池以防饥饿
    for (const auto& t : currentTasks) {
        m_thumbPool->start([this, t]() {
            // 确保工作线程已初始化 COM 环境
            static QThreadStorage<ScopedComInit> comStorage;
            if (!comStorage.hasLocalData()) {
                comStorage.setLocalData(ScopedComInit());
            }

            auto& reader = MftReader::instance();
            int actualIdx = reader.getIndexByKey(t.key);
            if (actualIdx == -1) return;

            QString fullPath = reader.getFullPath(actualIdx);
            if (fullPath.isEmpty()) return;

            QImage img;
            if (t.ext == "svg") {
                QSvgRenderer renderer(fullPath);
                if (renderer.isValid()) {
                    img = QImage(QSize(t.size, t.size), QImage::Format_ARGB32);
                    img.fill(Qt::transparent);
                    QPainter painter(&img);
                    renderer.render(&painter);
                    painter.end();
                }
            } else {
                img = FERREX::UiHelper::getShellThumbnail(fullPath, t.size);
            }

            if (!img.isNull()) {
                double ar = (double)img.width() / (double)img.height();
                // 切回主线程登记单条结果
                QMetaObject::invokeMethod(this, [this, key = t.key, cacheKey = t.cacheKey, img, ar]() {
                    QPixmap pix = QPixmap::fromImage(img);
                    if (!pix.isNull()) {
                        m_thumbCache.insert(cacheKey, new QPixmap(pix));
                        m_lastPixmapCache.insert(QString::number(key), new QPixmap(pix)); // 实时注册副本，作为下一次调节时的渐进拉伸源
                    }
                    m_aspectRatios[key] = ar;

                    auto snapshot = m_controller->snapshot();
                    auto itPos = snapshot->keyToPos.find(key);
                    if (itPos != snapshot->keyToPos.end() && itPos->second < m_displayCount) {
                        m_pendingRows.insert(itPos->second);
                        if (!m_throttleTimer->isActive()) m_throttleTimer->start();
                    }
                }, Qt::QueuedConnection);
            } else {
                // 【核心改进】：获取失败，记录进失败名单，并强制刷新，退化使用默认图标兜底
                QMetaObject::invokeMethod(this, [this, key = t.key]() {
                    m_failedThumbs.insert(key);
                    auto snapshot = m_controller->snapshot();
                    auto itPos = snapshot->keyToPos.find(key);
                    if (itPos != snapshot->keyToPos.end() && itPos->second < m_displayCount) {
                        m_pendingRows.insert(itPos->second);
                        if (!m_throttleTimer->isActive()) m_throttleTimer->start();
                    }
                }, Qt::QueuedConnection);
            }
        });
    }
}

void ScanTableModel::sort(int column, Qt::SortOrder order) {
    // 2026-06-xx 逻辑剥离：Model 不再拥有排序权，仅向 Controller 发起异步请求
    m_controller->sort(column, static_cast<int>(order));
}

Qt::DropActions ScanTableModel::supportedDragActions() const {
    return Qt::CopyAction;
}

QMimeData* ScanTableModel::mimeData(const QModelIndexList& indexes) const {
    QMimeData* data = new QMimeData();
    QList<QUrl> urls;
    QSet<int> seen;
    for (const QModelIndex& idx : indexes) {
        if (idx.column() != 0) continue;
        int row = idx.row();
        if (row < 0 || row >= (int)m_currentResultSet->keys.size()) continue;
        uint64_t key = m_currentResultSet->keys[row];
        int actualIdx = MftReader::instance().getIndexByKey(key);
        if (actualIdx == -1 || seen.contains(actualIdx)) continue;
        seen.insert(actualIdx);
        QString path = MftReader::instance().getFullPath(actualIdx);
        if (!path.isEmpty()) urls << QUrl::fromLocalFile(path);
    }
    data->setUrls(urls);
    return data;
}

// --- ScanDialog Implementation ---

ScanDialog::ScanDialog(QWidget* parent)
    : FramelessDialog("FERREX-META", parent), m_config(ConfigManager::instance().getConfig())
{
    // 安全启动保障：将 m_itemToolTipTimer 初始化提升到构造函数最顶端
    m_itemToolTipTimer = new QTimer(this);
    m_itemToolTipTimer->setSingleShot(true);
    m_itemToolTipTimer->setInterval(2000); // 2000毫秒（2秒）延时
    connect(m_itemToolTipTimer, &QTimer::timeout, this, [this]() {
        if (m_hoveredIndex.isValid() && m_tableModel) {
            QString tipText = m_tableModel->data(m_hoveredIndex, Qt::ToolTipRole).toString();
            if (!tipText.isEmpty()) {
                ToolTipOverlay::instance()->showText(m_hoveredGlobalPos, tipText, 0);
            }
        }
    });

    // 2026-07-10 参考 ArcMeta 重构：在 UI 构造的最前期创建并注册全局事件过滤器
    m_resizeFilter = new ResizeEventFilter(this);
    QCoreApplication::instance()->installEventFilter(m_resizeFilter);

    if (!UiHelper::isRunAsAdmin()) {
        QMessageBox::critical(nullptr, "权限不足", "访问 MFT/USN 需要管理员权限。\n请右键以管理员身份运行程序。");
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }

    m_config.load();
    resize(1000, 700);
    setMinimumSize(800, 500);

    m_titleStatusLabel = new QLabel("READY - 0");
    // 按照用户要求：间距严格对齐规范。 margin-left: 1px (配合 layout spacing 4px = 5px)
    m_titleStatusLabel->setStyleSheet("background: transparent; color: #46B478; font-size: 10px; font-weight: bold; margin-left: 1px;");

    if (m_titleLabel && m_pinBtn && m_pinBtn->parentWidget() && m_pinBtn->parentWidget()->layout()) {
        m_titleLabel->hide(); 
        auto* titleLayout = qobject_cast<QHBoxLayout*>(m_pinBtn->parentWidget()->layout());
        if (titleLayout) {
            // 按照用户要求：容器规范高度 34px，布局间距严格锁定 4px
            m_pinBtn->parentWidget()->setFixedHeight(34);
            titleLayout->setSpacing(4);
            titleLayout->setContentsMargins(12, 0, 8, 0);

            QLabel* logoLabel = new QLabel();
            logoLabel->setFixedSize(16, 16);
            logoLabel->setPixmap(UiHelper::getIcon("ferrex", QColor("#FF8C00"), 16).pixmap(16, 16));
            // 物理修正：显式清除所有边距以确保基准对齐
            logoLabel->setStyleSheet("background: transparent; margin: 0px; padding: 0px;"); 
            titleLayout->insertWidget(0, logoLabel);
            
            QLabel* brandLabel = new QLabel("FERREX-META");
            brandLabel->setObjectName("TitleBrandLabel");
            // 物理修正：将 margin-left 设为 0px。配合 Layout Spacing 4px 达到 4px 左右的极紧凑视觉
            brandLabel->setStyleSheet("background: transparent; color: #FF8C00; font-size: 14px; font-weight: bold; letter-spacing: 1.5px; margin-left: 0px; padding: 0px;");
            titleLayout->insertWidget(1, brandLabel);
            
            titleLayout->insertWidget(2, m_titleStatusLabel);

            // 按照截图要求调整布局：
            // [Logo/标题/状态] -> [Stretch] -> [滑动条③] -> [视图按钮②] -> [窗口控制按钮①]
            
            // 找到窗口控制按钮（m_pinBtn 等）在 layout 中的起始索引。
            // 在 FramelessDialog 中，它们是依次 addWidget 的。
            // 这里 titleLayout 是从 FramelessDialog 继承而来的，m_pinBtn 应该已经在里面。
            
            titleLayout->insertStretch(titleLayout->indexOf(m_pinBtn));

            // 构造全局气泡 Hover 事件过滤器
            auto* hoverFilter = new HoverEventFilter(this);

            // ① 视图切换按钮 (标记 2)
            QPushButton* viewBtn = new QPushButton(); 
            viewBtn->setFixedSize(20, 20); // 严格锁定 20x20
            viewBtn->setIcon(UiHelper::getIcon("grid", QColor("#CCCCCC"), 16)); // 严格锁定图标 16x16
            viewBtn->setIconSize(QSize(16, 16));
            viewBtn->setCursor(Qt::PointingHandCursor); 
            viewBtn->setToolTip(""); // 物理禁止原生 ToolTip，防范系统黑块
            viewBtn->setProperty("tooltipText", "排列方式"); // 完美对接高级属性
            viewBtn->installEventFilter(hoverFilter); // 接管悬停管线
            viewBtn->setStyleSheet( 
                "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 0; }" 
                "QPushButton:hover { background: rgba(255, 255, 255, 0.1); }" 
                "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }" 
            ); 
            connect(viewBtn, &QPushButton::clicked, this, [this, viewBtn]() { 
                QMenu* menu = new QMenu(this); 
                menu->setStyleSheet( 
                    "QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; border-radius: 6px; }" 
                    "QMenu::item { padding: 6px 24px; }" 
                    "QMenu::item:selected { background: #2A2A2A; color: #FFF; }" 
                    "QMenu::item:checked { color: #FF8C00; }" 
                ); 

                // 自适应模式（对应用户原话：“自适应”） 
                QAction* jModeAct = menu->addAction("自适应"); 
                jModeAct->setCheckable(true); 
                jModeAct->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 0); 

                // 网格模式（对应用户原话：“网格”） 
                QAction* gModeAct = menu->addAction("网格"); 
                gModeAct->setCheckable(true); 
                gModeAct->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 1); 

                // 列表模式（对应用户原话：“列表”） 
                QAction* listModeAct = menu->addAction("列表"); 
                listModeAct->setCheckable(true); 
                listModeAct->setChecked(m_config.viewMode == 0); 

                // 通过排他性的 Action 组进行物理互斥 
                QActionGroup* modeGrp = new QActionGroup(menu); 
                modeGrp->addAction(jModeAct); 
                modeGrp->addAction(gModeAct); 
                modeGrp->addAction(listModeAct); 

                // 各项单选槽连接，使选择彻底正交 
                connect(jModeAct, &QAction::triggered, this, [this]() { 
                    switchToView(1, 0);
                }); 
                connect(gModeAct, &QAction::triggered, this, [this]() { 
                    switchToView(1, 1);
                }); 
                connect(listModeAct, &QAction::triggered, this, [this]() { 
                    switchToView(0, 0);
                }); 

                menu->exec(viewBtn->mapToGlobal(QPoint(0, viewBtn->height() + 2))); 
            }); 

            // ② 尺寸滑动条 (标记 3)
            m_sizeSlider = new QSlider(Qt::Horizontal); 
            m_sizeSlider->setRange(32, 256); 
            m_sizeSlider->setValue(m_config.iconSize > 0 ? m_config.iconSize : 64); 
            m_sizeSlider->setFixedSize(110, 20); // 高度调整为 20px，避免覆盖/截断
            m_sizeSlider->setCursor(Qt::PointingHandCursor); 
            m_sizeSlider->installEventFilter(this);
            // 间距计算：margin-right 1px + spacing 4px = 5px (精准对标视图按钮)
            m_sizeSlider->setStyleSheet( 
                "QSlider { background: transparent; margin-right: 1px; }"
                "QSlider::groove:horizontal { height: 3px; background: #3F3F3F; border-radius: 2px; }" 
                "QSlider::sub-page:horizontal { background: #FF8C00; border-radius: 2px; }" 
                "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0; " 
                "  background: #FF8C00; border-radius: 6px; }" 
            ); 
            connect(m_sizeSlider, &QSlider::valueChanged, this, [this](int v) { 
                m_config.iconSize = v; 
                if (m_currentActiveView) {
                    m_currentActiveView->setIconSize(v);
                }
                
                // 在列表模式下调节尺寸时，自适应计算并拓宽名称列 [1]
                if (m_config.viewMode == 0 && m_listResultView) {
                    auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
                    if (resultTableView) {
                        int minWidth = calculateNameColumnMinimumWidth();
                        resultTableView->setColumnWidth(0, minWidth);
                    }
                }

                m_tableModel->clearThumbCache(true); 
                m_tableModel->updateResults(); 
                m_config.save(); 
            }); 
            
            QPushButton* rulesBtn = new QPushButton();
            rulesBtn->setFixedSize(20, 20);
            rulesBtn->setIcon(UiHelper::getIcon("settings", QColor("#CCCCCC"), 16));
            rulesBtn->setIconSize(QSize(16, 16));
            rulesBtn->setCursor(Qt::PointingHandCursor);
            rulesBtn->setToolTip(""); // 彻底切除原生阻塞黑色气泡 (对应用户原话：“其他按钮也没有采用Tooltip ... Tooltip都显示了什么？完全看不到”)
            rulesBtn->setProperty("tooltipText", "预览配置"); // 统一改用自定义属性气泡驱动 (对应用户原话：“去参考ArcMeta版本来实现ToolTipOverlay”)
            rulesBtn->installEventFilter(hoverFilter); // 接管悬停管线
            rulesBtn->setStyleSheet(
                "QPushButton { background: transparent; border: none; border-radius: 4px; padding: 0; }"
                "QPushButton:hover { background: rgba(255, 255, 255, 0.1); }"
                "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
            );
            connect(rulesBtn, &QPushButton::clicked, this, [this]() {
                // 使用非模态弹出，禁止 exec()
                // 为解决关闭子窗口时 ScanDialog 表格失焦无法点选的 bug，在堆上创建并将 parent 设为 nullptr 使其完全独立
                // 同时采用 QPointer 防止多开以及销毁时保护
                static QPointer<PreviewRulesDialog> activeDlg;
                if (activeDlg) {
                    activeDlg->raise();
                    activeDlg->activateWindow();
                    return;
                }
                auto* dlg = new PreviewRulesDialog(m_config, nullptr);
                activeDlg = dlg;
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                connect(dlg, &QDialog::accepted, this, [this]() {
                    m_config.save();
                });
                // 当主窗口被销毁时同步销毁非模态子窗口，避免 parent=nullptr 导致的残留崩溃
                connect(this, &QObject::destroyed, dlg, &QObject::deleteLater);
                dlg->show();
            });
            
            titleLayout->insertWidget(titleLayout->indexOf(m_pinBtn), viewBtn);
            titleLayout->insertWidget(titleLayout->indexOf(viewBtn), rulesBtn);
            titleLayout->insertWidget(titleLayout->indexOf(rulesBtn), m_sizeSlider);

            // 更新现有控制按钮样式以对标规范
            for (auto* btn : {m_pinBtn, m_minBtn, m_maxBtn}) {
                if (!btn) continue;
                btn->setFixedSize(20, 20);
                btn->setIconSize(QSize(16, 16));
                btn->setToolTip(""); // 原生禁绝
                
                // 完美赋予自定义 ToolTipOverlay 支持
                if (btn == m_pinBtn) {
                    btn->setProperty("tooltipText", "置顶");
                    btn->installEventFilter(hoverFilter);
                    btn->setStyleSheet(
                        "QPushButton { background: transparent; border: none; border-radius: 4px; } "
                        "QPushButton:hover { background: rgba(255, 255, 255, 0.1); } "
                        "QPushButton:checked { background: rgba(255, 85, 28, 0.2); }"
                    );
                } else {
                    if (btn == m_minBtn) {
                        btn->setProperty("tooltipText", "最小化");
                        btn->installEventFilter(hoverFilter);
                    } else if (btn == m_maxBtn) {
                        btn->setProperty("tooltipText", "最大化");
                        btn->installEventFilter(hoverFilter);
                    }
                    btn->setStyleSheet(
                        "QPushButton { background: transparent; border: none; border-radius: 4px; } "
                        "QPushButton:hover { background: rgba(255, 255, 255, 0.1); } "
                        "QPushButton:pressed { background: rgba(255, 255, 255, 0.2); }"
                    );
                }
            }
            if (m_closeBtn) {
                m_closeBtn->setFixedSize(20, 20);
                m_closeBtn->setIconSize(QSize(16, 16));
                m_closeBtn->setToolTip("");
                m_closeBtn->setStyleSheet(
                    "QPushButton { background-color: #E81123; border: none; border-radius: 4px; } "
                    "QPushButton:hover { background-color: #E81123; } "
                    "QPushButton:pressed { background-color: #A50000; }"
                );
            }
        } else {
            m_titleStatusLabel->hide(); 
        }
    } else {
        m_titleStatusLabel->hide();
    }

    setupUi();

    // --- 2026-06-xx 架构级 QSS：实现样式沙箱与物理隔离 ---
    // 2026-06-xx 物理修正：将标题栏品牌标签样式移入此处，确保优先级并严格锁定 1px 边距 (+4px Spacing = 5px Gap)
    this->setStyleSheet(this->styleSheet() + R"(
        #TitleBrandLabel {
            background: transparent; 
            color: #FF8C00; 
            font-size: 14px; 
            font-weight: bold; 
            letter-spacing: 1.5px; 
            /* 物理负边距补偿：由 -2px 增加至 -4px，确保产生非常明显的紧凑效果 */
            margin-left: -4px; 
            padding: 0px;
        }

        #DialogContainer {
            background-color: #1E1E1E;
            border: 1px solid #333333;
            border-radius: 6px;
        }

        QWidget#SearchContainer, QWidget#DriveContainer { 
            background: transparent; border: none; 
        }

        QStackedWidget#ViewStack {
            background-color: #1E1E1E;
            border: none;
        }
        
        #mainSearchEdit, #extSearchEdit { 
            background: #2D2D2D; 
            border: 1px solid #FF8C00; 
            border-radius: 6px; 
            color: #EEE; 
            font-size: 14px; 
            padding: 0 10px;
            outline: none;
        }

        /* 显式定义伪类，保持橙色边框 */
        #mainSearchEdit:focus, #extSearchEdit:focus { border: 1px solid #FF8C00 !important; }
        #mainSearchEdit:hover, #extSearchEdit:hover { border: 1px solid #FF8C00; }
        
        #mainSearchEdit::placeholder, #extSearchEdit::placeholder {
            color: rgba(238, 238, 238, 0.3);
        }

        /* 搜索按钮：独立物理实体，拥有完整圆角 */
        QPushButton#searchIconButton { 
            background: #FF8C00; 
            border: 1px solid #FF8C00;
            border-radius: 6px; 
            color: #000;
            font-weight: bold;
            padding: 0 15px;
        } 
        QPushButton#searchIconButton:hover { background: #FFA500; } 
        QPushButton#searchIconButton:pressed { background: #CC6600; }

        /* 盘符按钮：使用属性选择器 */
        QPushButton[isActive="true"] {
            background: rgba(255, 140, 0, 30); 
            color: #FF8C00; 
            border: 1px solid #FF8C00; 
            padding: 0 10px; 
            font-size: 12px; 
            font-weight: bold;
            border-radius: 4px;
        }
        QPushButton[isActive="false"] {
            background: #111519; 
            color: #7A8F9E; 
            border: 1px solid #252E37; 
            padding: 0 10px; 
            font-size: 12px;
            border-radius: 4px;
        }

        QProgressBar#ScanProgressBar { background: transparent; border: none; } 
        QProgressBar#ScanProgressBar::chunk { background: #FF8C00; }

        QCheckBox { color: #AAA; }

        /* 全局滚动条美化 */
        QScrollBar:vertical {
            border: none;
            background: transparent;
            width: 7px;
            margin: 0px;
        }
        QScrollBar::handle:vertical {
            background: #333333;
            min-height: 20px;
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover {
            background: #444444;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: none;
        }

        QScrollBar:horizontal {
            border: none;
            background: transparent;
            height: 7px;
            margin: 0px;
        }
        QScrollBar::handle:horizontal {
            background: #333333;
            min-width: 20px;
            border-radius: 3px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #444444;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
            background: none;
        }
    )");

    // --- 2026-05-16 持久化恢复：根据配置恢复视图、尺寸与排序状态 ---
    switchToView(m_config.viewMode, m_config.layoutMode);
    
    // 恢复排序状态 (同时作用于模型和表头视觉)
    if (m_listResultView) {
        auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
        if (resultTableView) {
            resultTableView->horizontalHeader()->setSortIndicator(m_config.sortColumn, static_cast<Qt::SortOrder>(m_config.sortOrder));
        }
    }
    m_tableModel->sort(m_config.sortColumn, static_cast<Qt::SortOrder>(m_config.sortOrder));

    // 2026-06-xx 物理对标：监听引擎加载信号，实现“更新数据中...”的体感同步
    connect(&MftReader::instance(), &MftReader::driveLoaded, this, [this](const QString& drive, int count, int total) {
        updateStatus(QString("正在加载快照 %1 (%2)...").arg(drive).arg(formatNumber(count)), true, total);
    });

    // 2026-05-28 核心补丁：监听引擎增量信号，实现标题栏计数实时更新
    connect(&MftReader::instance(), &MftReader::entriesChangedBatch, this, [this]() { updateStatus("就绪"); });

    // 2026-05-16 物理重载：断开基类 Qt 置顶逻辑，改用 Win32 原生 SetWindowPos 以实现无损切换
    if (m_pinBtn) {
        disconnect(m_pinBtn, &QPushButton::toggled, nullptr, nullptr);
        connect(m_pinBtn, &QPushButton::toggled, this, [this](bool checked) {
            m_pinBtn->setIcon(UiHelper::getIcon(checked ? "pin_vertical" : "pin_tilted", 
                                                checked ? QColor("#FF551C") : QColor("#CCCCCC"), 18));
            HWND hwnd = reinterpret_cast<HWND>(winId());
            if (checked) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            } else {
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            }
        });
    }

    QTimer::singleShot(100, this, [this]() {
        updateStatus("按需初始化中...");
        QPointer<ScanDialog> weakThis(this);
        (void)(QtConcurrent::run)([weakThis]() {
            if (!weakThis) return;
            // 2026-07-07 架构重构：按需加载。启动时仅加载默认盘符。
            bool anyLoaded = false;
            QStringList toLoad;
            for (const QString& d : weakThis->m_config.defaultDrives) toLoad << d;
            
            // 兜底策略：如果没设默认盘，则尝试加载 C: 盘作为可用状态
            if (toLoad.isEmpty()) toLoad << "C:";

            QStringList toScan;
            for (const QString& d : toLoad) {
                if (MftReader::instance().loadDriveFromCache(d)) anyLoaded = true;
                else toScan << d;
            }

            if (!toScan.isEmpty()) {
                QMetaObject::invokeMethod(weakThis.data(), [weakThis, toScan]() {
                    if (weakThis) {
                        for (const QString& d : toScan) weakThis->onStartScan(d);
                    }
                });
            }

            QMetaObject::invokeMethod(weakThis.data(), [weakThis, anyLoaded]() {
                if (!weakThis) return;
                weakThis->updateStatus("就绪");
                weakThis->m_controller->setSearchText("");
                weakThis->refreshDriveList(true); // 后台探测硬件
                if (weakThis->m_config.autoDisplay) weakThis->onFilterOptionChanged();
                
                // 2026-07-07 物理修复：调用封装后的预热函数 (Analysis_Modification_Plan-154.md)
                weakThis->triggerWarmup();
            });
        });
    });

    // 1. 初始化持久 Action 并绑定核心业务槽函数
    m_actJMode = new QAction("自适应(A)", this);
    m_actJMode->setCheckable(true);
    connect(m_actJMode, &QAction::triggered, this, [this]() {
        switchToView(1, 0);
    });

    m_actGMode = new QAction("网格(G)", this);
    m_actGMode->setCheckable(true);
    connect(m_actGMode, &QAction::triggered, this, [this]() {
        switchToView(1, 1);
    });

    m_actListMode = new QAction("列表(L)", this);
    m_actListMode->setCheckable(true);
    connect(m_actListMode, &QAction::triggered, this, [this]() {
        switchToView(0, 0);
    });

    // 2. 通过 QActionGroup 保持物理单选互斥
    QActionGroup* modeGrp = new QActionGroup(this);
    modeGrp->addAction(m_actJMode);
    modeGrp->addAction(m_actGMode);
    modeGrp->addAction(m_actListMode);
}

ScanDialog::~ScanDialog() {
    if (m_resizeFilter) {
        QCoreApplication::instance()->removeEventFilter(m_resizeFilter);
    }
}


void ScanDialog::switchToView(int viewMode, int layoutMode) {
    m_config.viewMode = viewMode;
    m_config.layoutMode = layoutMode;

    if (viewMode == 0) {
        m_currentActiveView = m_listResultView;
    } else if (layoutMode == 0) {
        m_currentActiveView = m_justifiedResultView;
    } else {
        m_currentActiveView = m_gridResultView;
    }

    if (m_currentActiveView) {
        m_viewStack->setCurrentWidget(m_currentActiveView->getWidget());
        m_currentActiveView->setIconSize(m_config.iconSize);
        m_currentActiveView->refreshLayout();
    }

    m_tableModel->updateResults();
    m_config.save();
}

void ScanDialog::closeEvent(QCloseEvent* event) {
    // Plan-136: 在 closeEvent 中拦截关闭信号，若未点击“彻底退出”，则仅执行 hide()
    // 对应用户原话：“保留托盘图标支持”
    hide();
    event->ignore();
}

void ScanDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(m_contentArea);
    // 2026-06-xx 按照建议：将 mainLayout 的 spacing 设置为 10，给组件之间留出物理切割的空隙
    // 按照用户要求：底部边距设为 0，确保状态栏紧贴底边且高度严格受控
    mainLayout->setContentsMargins(10, 10, 10, 0);
    mainLayout->setSpacing(10);

    auto* driveScroll = new QScrollArea();
    driveScroll->setFixedHeight(45);
    driveScroll->setWidgetResizable(true);
    driveScroll->setFrameShape(QFrame::NoFrame);
    driveScroll->setStyleSheet("background: #252526; border: 1px solid #333; border-radius: 4px;");

    m_driveContainer = new QWidget();
    m_driveContainer->setObjectName("DriveContainer");
    m_driveContainer->setAttribute(Qt::WA_StyledBackground, true);
    m_driveLayout = new QHBoxLayout(m_driveContainer);
    // 2026-06-xx 按照建议：盘符起始坐标向右偏移 5 像素 (5 -> 10)
    m_driveLayout->setContentsMargins(10, 0, 5, 0);
    m_driveLayout->setSpacing(10);
    driveScroll->setWidget(m_driveContainer);

    auto* topControl = new QHBoxLayout();
    // 物理修正：恢复容器边距为 0，防止整个框发生偏移
    topControl->setContentsMargins(0, 0, 0, 0);
    topControl->addWidget(driveScroll, 1);
    mainLayout->addLayout(topControl);

    // B. 搜索选项行 (迁移至盘符与搜索框之间)
    auto* optionRow = new QHBoxLayout();
    optionRow->setContentsMargins(0, 0, 0, 0);
    optionRow->setSpacing(15);
    
    m_checkRegex = new QCheckBox("正则");
    m_checkCase = new QCheckBox("大小写");
    m_checkHidden = new QCheckBox("隐藏");
    m_checkSystem = new QCheckBox("系统");
    m_checkDollar = new QCheckBox("显示$");
    m_checkAuto = new QCheckBox("自动显示");

    m_checkRegex->setChecked(m_config.useRegex);
    m_checkCase->setChecked(m_config.caseSensitive);
    m_checkHidden->setChecked(m_config.includeHidden);
    m_checkSystem->setChecked(m_config.includeSystem);
    m_checkDollar->setChecked(m_config.includeDollar);
    m_checkAuto->setChecked(m_config.autoDisplay);

    for (auto* cb : {m_checkRegex, m_checkCase, m_checkHidden, m_checkSystem, m_checkDollar, m_checkAuto}) {
        connect(cb, &QCheckBox::toggled, this, &ScanDialog::onFilterOptionChanged);
        optionRow->addWidget(cb);
    }
    optionRow->addStretch();
    mainLayout->addLayout(optionRow);

    auto* searchContainer = new QWidget();
    searchContainer->setObjectName("SearchContainer");
    searchContainer->setAttribute(Qt::WA_StyledBackground, true);
    auto* searchVLayout = new QVBoxLayout(searchContainer);
    searchVLayout->setContentsMargins(0, 0, 0, 0);
    searchVLayout->setSpacing(10); // 增加呼吸感

    auto* searchRow = new QHBoxLayout();
    searchRow->setContentsMargins(0, 0, 0, 0); 
    searchRow->setSpacing(10); 

    // A. 物理拆分搜索栏组件：恢复“分开”的视觉风格，确保组件间有明显间距
    m_searchEdit = new QLineEdit();
    m_searchEdit->setObjectName("mainSearchEdit");
    m_searchEdit->setPlaceholderText("输入文件名 / 关键词...");
    m_searchEdit->setFixedHeight(36);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_controller->setSearchText(text);
        if (text.isEmpty()) {
            m_controller->triggerSearch(true);
        } else {
            m_controller->triggerSearch(false);
        }
    });
    connect(m_searchEdit, &QLineEdit::editingFinished, this, [this]() { m_config.save(); });
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_searchEdit, 1);

    m_extEdit = new QLineEdit();
    m_extEdit->setObjectName("extSearchEdit");
    m_extEdit->setPlaceholderText("后缀");
    m_extEdit->setFixedWidth(120); 
    m_extEdit->setFixedHeight(36);
    m_extEdit->setClearButtonEnabled(true);
    m_extEdit->installEventFilter(this);
    connect(m_extEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        // 2026-06-xx 性能优化：仅同步过滤状态，使用防抖触发搜索，避免输入后缀时发生假死
        ScanFilterState state;
        state.useRegex = m_checkRegex->isChecked();
        state.caseSensitive = m_checkCase->isChecked();
        state.includeHidden = m_checkHidden->isChecked();
        state.includeSystem = m_checkSystem->isChecked();
        state.includeDollar = m_checkDollar->isChecked();
        state.autoDisplay = m_checkAuto->isChecked();
        QString extText = m_extEdit->text().toLower();
        if (!extText.isEmpty()) state.extensionList = extText.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
        
        m_controller->setFilterState(state);
        m_controller->triggerSearch(false); 
    });
    connect(m_extEdit, &QLineEdit::editingFinished, this, [this]() { m_config.save(); });
    connect(m_extEdit, &QLineEdit::returnPressed, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_extEdit);

    m_searchBtn = new QPushButton("搜索");
    m_searchBtn->setObjectName("searchIconButton");
    m_searchBtn->setFixedWidth(80);
    m_searchBtn->setFixedHeight(36); 
    m_searchBtn->setCursor(Qt::PointingHandCursor);
    m_searchBtn->setIcon(UiHelper::getIcon("search", QColor("#000000"), 18));
    m_searchBtn->setIconSize(QSize(18, 18));
    connect(m_searchBtn, &QPushButton::clicked, this, &ScanDialog::onTriggerSearch);
    searchRow->addWidget(m_searchBtn);

    searchVLayout->addLayout(searchRow);

    m_progressBar = new QProgressBar();
    m_progressBar->setObjectName("ScanProgressBar");
    m_progressBar->setFixedHeight(2);
    m_progressBar->setTextVisible(false);
    m_progressBar->hide();
    searchVLayout->addWidget(m_progressBar);

    mainLayout->addWidget(searchContainer);

    m_controller = new ScanController(this);
    m_tableModel = new ScanTableModel(m_controller, this);

    m_listResultView = new ListResultView(this);
    m_justifiedResultView = new JustifiedResultView(this);
    m_gridResultView = new GridResultView(this);

    m_listResultView->setModel(m_tableModel);
    m_justifiedResultView->setModel(m_tableModel);
    m_gridResultView->setModel(m_tableModel);

    auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());

    // Apply the header drag and width auto-restoration constraint on resultTableView [1]
    if (resultTableView) {
        connect(resultTableView->horizontalHeader(), &QHeaderView::sectionResized, this, [this, resultTableView](int logicalIndex, int /*oldSize*/, int newSize) {
            if (logicalIndex == 0 && m_tableModel) {
                int minWidth = calculateNameColumnMinimumWidth();
                if (newSize < minWidth) {
                    resultTableView->horizontalHeader()->blockSignals(true);
                    resultTableView->setColumnWidth(0, minWidth);
                    resultTableView->horizontalHeader()->blockSignals(false);
                }
            }
        });
    }

    m_viewStack = new QStackedWidget();
    m_viewStack->setObjectName("ViewStack");
    m_viewStack->addWidget(m_listResultView->getWidget());
    m_viewStack->addWidget(m_justifiedResultView->getWidget());
    m_viewStack->addWidget(m_gridResultView->getWidget());

    for (auto* resView : {m_listResultView, m_justifiedResultView, m_gridResultView}) {
        QAbstractItemView* base = resView->getBaseView();

        base->installEventFilter(this);
        base->viewport()->installEventFilter(this);
        base->viewport()->setMouseTracking(true);

        connect(base->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
            refreshVisibleMetadataRange();
        });

        connect(base, &QAbstractItemView::doubleClicked, this, &ScanDialog::onItemDoubleClicked);
        connect(base, &QAbstractItemView::customContextMenuRequested, this, &ScanDialog::onCustomContextMenu);
        connect(base->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ScanDialog::onSelectionChanged);
    }

    m_quickLook = new QuickLookWindow(this);
    connect(m_quickLook, &QuickLookWindow::prevRequested, this, [this]() {
        auto* view = m_currentActiveView->getBaseView();
        int row = view->currentIndex().row();
        if (row > 0) {
            QModelIndex nextIdx = m_tableModel->index(row - 1, 0);
            view->setCurrentIndex(nextIdx);
            QString path = m_tableModel->data(m_tableModel->index(row - 1, 1)).toString();
            m_quickLook->preview(path);
        }
    });
    connect(m_quickLook, &QuickLookWindow::nextRequested, this, [this]() {
        auto* view = m_currentActiveView->getBaseView();
        int row = view->currentIndex().row();
        if (row < m_tableModel->rowCount() - 1) {
            QModelIndex nextIdx = m_tableModel->index(row + 1, 0);
            view->setCurrentIndex(nextIdx);
            QString path = m_tableModel->data(m_tableModel->index(row + 1, 1)).toString();
            m_quickLook->preview(path);
        }
    });

    mainLayout->addWidget(m_viewStack);

    auto* statusContainer = new QWidget();
    statusContainer->setObjectName("StatusContainer");
    statusContainer->setFixedHeight(20);
    statusContainer->setStyleSheet("QWidget#StatusContainer { background: transparent; border: none; }");
    auto* statusBar = new QHBoxLayout(statusContainer);
    // 2026-06-xx 按照用户要求：显式设置垂直居中对齐，并向上偏移 10px (通过底部边距实现)
    statusBar->setAlignment(Qt::AlignVCenter);
    statusBar->setContentsMargins(16, 0, 16, 10);
    statusBar->setSpacing(0);

    m_statLabelMain = new QLabel("");
    m_statLabelMain->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_statLabelMain);

    m_statLabelTime = new QLabel("");
    m_statLabelTime->setStyleSheet("color: #7A8F9E; font-size: 10px; margin-left: 12px;");
    statusBar->addWidget(m_statLabelTime);

    m_selectionLabel = new QLabel("");
    m_selectionLabel->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_selectionLabel);

    m_csvBtn = new QPushButton("导出所选为 CSV");
    m_csvBtn->setFlat(true);
    m_csvBtn->setCursor(Qt::PointingHandCursor);
    m_csvBtn->setStyleSheet("QPushButton { color: #FF8C00; font-size: 10px; border: none; padding: 0 0 0 8px; text-decoration: none; } QPushButton:hover { text-decoration: underline; }");
    m_csvBtn->hide();
    statusBar->addWidget(m_csvBtn);

    statusBar->addStretch();

    m_statLabelMemory = new QLabel("");
    m_statLabelMemory->setStyleSheet("color: #7A8F9E; font-size: 10px;");
    statusBar->addWidget(m_statLabelMemory);

    mainLayout->addWidget(statusContainer);

    connect(m_controller, &ScanController::searchFinished, this, [this](int count, int64_t elapsedMs) {
        Q_UNUSED(count);
        m_lastSearchMs = elapsedMs;
        m_tableModel->updateResults();
        updateStatus("就绪");
        refreshVisibleMetadataRange();
        
        // 在列表激活状态下，检索完成后自动撑开列宽 [1]
        if (m_config.viewMode == 0 && m_listResultView) {
            auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
            if (resultTableView) {
                resultTableView->setColumnWidth(0, calculateNameColumnMinimumWidth());
            }
        }
    });

    connect(m_controller, &ScanController::resultsSwapped, this, [this]() {
        updateStatus("就绪");
        refreshVisibleMetadataRange();

        // 在列表激活状态下，检索完成后自动撑开列宽 [1]
        if (m_config.viewMode == 0 && m_listResultView) {
            auto* resultTableView = qobject_cast<QTableView*>(m_listResultView->getBaseView());
            if (resultTableView) {
                resultTableView->setColumnWidth(0, calculateNameColumnMinimumWidth());
            }
        }
    });

    showDriveLoading();
}

void ScanDialog::showDriveLoading() {
    if (!m_driveLayout) return;

    QLayoutItem* child;
    while ((child = m_driveLayout->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }
    QLabel* loadingLbl = new QLabel("更新数据中...");
    loadingLbl->setStyleSheet("background: transparent; border: none; color: #7A8F9E; font-size: 12px; font-weight: bold; margin-left: 10px;");
    m_driveLayout->addWidget(loadingLbl);
    m_driveLayout->addStretch();
}

void ScanDialog::refreshDriveList(bool forceProbe) {
    if (!forceProbe && !m_cachedDriveInfos.isEmpty()) {
        updateDriveButtonStyles();
        return;
    }

    // 2026-06-xx 按照用户要求：加载盘符数据（.scch）之前，先显示占位提示
    showDriveLoading();

    QPointer<ScanDialog> weakThis(this);
    (void)(QtConcurrent::run)([weakThis]() {
        if (!weakThis) return;
        QVector<DriveInfo> drives;
        DWORD driveMask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (driveMask & (1 << i)) {
                QString letter = QString(QChar('A' + i)) + QLatin1String(":");
                WCHAR volName[MAX_PATH + 1] = {0};
                WCHAR fsName[MAX_PATH + 1] = {0};
                QString driveRoot = letter + QLatin1String("\\");
                BOOL ok = GetVolumeInformationW(reinterpret_cast<const wchar_t*>(driveRoot.utf16()), 
                                              volName, MAX_PATH + 1, NULL, NULL, NULL, 
                                              fsName, MAX_PATH + 1);
                DriveInfo info;
                info.letter = letter;
                info.hasMedia = ok;
                if (ok) {
                    info.label = QString::fromWCharArray(volName);
                    info.isNtfs = QString::fromWCharArray(fsName).contains("NTFS", Qt::CaseInsensitive);
                } else {
                    info.isNtfs = false;
                }

                // 2026-06-xx 极致性能对标与 C 盘加固：
                // 只要探测到 C 盘，无论其文件系统报告如何，强制视为 NTFS 以允许进入 MFT 扫描引擎。
                // 理由：系统盘可能因为权限竞争导致 fsName 获取为空，但物理上必然存在 MFT。
                if (letter == "C:") {
                    info.isNtfs = true;
                }
                
                drives.append(info);
            }
        }

        QMetaObject::invokeMethod(weakThis.data(), [weakThis, drives]() {
            if (!weakThis) return;
            weakThis->m_cachedDriveInfos = drives;
            
            if (weakThis->m_config.activeDrives.isEmpty()) {
                for (const auto& info : drives) {
                    if (info.hasMedia && info.isNtfs) {
                        weakThis->m_config.activeDrives.insert(info.letter);
                    }
                }
            } else {
                if (!weakThis->m_config.activeDrives.contains("C:")) {
                    for (const auto& info : drives) {
                        if (info.letter == "C:") {
                            weakThis->m_config.activeDrives.insert("C:");
                            break;
                        }
                    }
                }
            }
            weakThis->m_config.save();

            QLayoutItem* item;
            while ((item = weakThis->m_driveLayout->takeAt(0)) != nullptr) {
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
            weakThis->m_driveButtonMap.clear();

            for (const auto& info : drives) {
                if (!info.hasMedia) continue; 
                if (!info.isNtfs && info.letter != "C:") continue;

                QString label = info.label.isEmpty() ? "本地磁盘" : info.label;
                QString btnText = QString("%1 (%2)").arg(info.letter).arg(label);
                
                QPushButton* btn = new QPushButton(btnText);
                btn->setCheckable(true);
                btn->setFixedHeight(24);
                weakThis->m_driveButtonMap[info.letter] = btn;
                
                connect(btn, &QPushButton::clicked, weakThis.data(), [weakThis, letter = info.letter]() {
                    if (!weakThis) return;
                    bool isSelected = false;
                    if (weakThis->m_config.activeDrives.contains(letter)) {
                        if (weakThis->m_config.activeDrives.size() > 1) {
                            weakThis->m_config.activeDrives.remove(letter);
                        } else {
                            isSelected = true; // 保持选中
                        }
                    } else {
                        weakThis->m_config.activeDrives.insert(letter);
                        isSelected = true;
                    }
                    
                    weakThis->updateDriveButtonStyles();

                    // 2026-05-14 核心同步：显式同步盘符状态至搜索引擎掩码，防止视图过滤失效
                    QStringList activeList;
                    for (const QString& d : weakThis->m_config.activeDrives) activeList << d;
                    MftReader::instance().updateActiveDrives(activeList);

                    // 2026-07-07 核心修正：左键仅筛选，严禁加载数据库 (Analysis_Modification_Plan-154.md)
                    if (isSelected && !MftReader::instance().isDriveIndexed(letter)) {
                        weakThis->updateStatus("请先通过右键菜单‘加载数据’");
                        weakThis->m_config.activeDrives.remove(letter);
                        weakThis->updateDriveButtonStyles();
                    } else {
                        weakThis->onTriggerSearch();
                    }
                });
                
                btn->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(btn, &QPushButton::customContextMenuRequested, weakThis.data(), [weakThis, letter = info.letter](const QPoint& pos) {
                    if (weakThis) weakThis->onDriveContextMenu(letter, pos);
                });
                
                weakThis->m_driveLayout->addWidget(btn);
            }
            weakThis->m_driveLayout->addStretch();
            weakThis->updateDriveButtonStyles();
        });
    });
}

void ScanDialog::updateDriveButtonStyles() {
    for (auto it = m_driveButtonMap.begin(); it != m_driveButtonMap.end(); ++it) {
        bool isActive = m_config.activeDrives.contains(it.key());
        bool isDefault = m_config.defaultDrives.contains(it.key());
        bool isLoaded = MftReader::instance().isDriveIndexed(it.key());
        
        QPushButton* btn = it.value();
        btn->setProperty("isActive", isActive);
        btn->setProperty("isDefault", isDefault);
        btn->setProperty("isLoaded", isLoaded);
        
        // 触发 QSS 刷新
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
        
        QString label = "";
        for (const auto& info : m_cachedDriveInfos) { if (info.letter == it.key()) { label = info.label; break; } }
        QString statusSuffix = isLoaded ? "" : " [未加载]";
        btn->setText(QString("%1%2 (%3)%4").arg(isDefault ? "★ " : "").arg(it.key()).arg(label.isEmpty() ? "本地磁盘" : label).arg(statusSuffix));
        
        if (!isLoaded) {
            btn->setStyleSheet(btn->styleSheet() + " color: #555; ");
        } else {
            btn->setStyleSheet(btn->styleSheet().remove(" color: #555; "));
        }
    }
}

void ScanDialog::onDriveContextMenu(const QString& drive, const QPoint& /*pos*/) {
    QMenu menu(this);
    menu.setStyleSheet("QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; } QMenu::item:selected { background: #232D37; color: #FFF; }");
    
    bool isDefault = m_config.defaultDrives.contains(drive);
    menu.addAction(isDefault ? "取消默认选项" : "设为默认选项", [this, drive, isDefault]() {
        if (isDefault) m_config.defaultDrives.remove(drive);
        else m_config.defaultDrives.insert(drive);
        m_config.save();
        updateDriveButtonStyles();
    });

    menu.addSeparator();

    bool isLoaded = MftReader::instance().isDriveIndexed(drive);
    auto* loadAct = menu.addAction("加载数据 (快速)", [this, drive]() {
        updateStatus(QString("正在加载 %1...").arg(drive), true);
        QPointer<ScanDialog> weakThis(this);
        (void)QtConcurrent::run([weakThis, drive]() {
            // 尝试加载缓存，若失败则自动触发扫描
            if (!MftReader::instance().loadDriveFromCache(drive)) {
                QMetaObject::invokeMethod(weakThis.data(), [weakThis, drive]() {
                    if (weakThis) weakThis->onStartScan(drive);
                });
            } else {
                QMetaObject::invokeMethod(weakThis.data(), [weakThis]() {
                    if (weakThis) {
                        weakThis->updateStatus("就绪");
                        weakThis->updateDriveButtonStyles();
                        weakThis->onTriggerSearch();
                        weakThis->triggerWarmup();
                    }
                });
            }
        });
    });
    loadAct->setEnabled(!isLoaded);

    auto* scanAct = menu.addAction("立即扫描并索引", [this, drive]() {
        onStartScan(drive);
    });
    scanAct->setEnabled(!isLoaded);

    auto* unloadAct = menu.addAction("卸载数据", [this, drive]() {
        MftReader::instance().unloadDrive(drive);
        updateDriveButtonStyles();
        onTriggerSearch();
    });
    unloadAct->setEnabled(isLoaded);
    
    menu.exec(QCursor::pos());
}

void ScanDialog::onCustomContextMenu(const QPoint& pos) {
    if (!m_currentActiveView) return;
    QAbstractItemView* activeView = m_currentActiveView->getBaseView();
    
    // 2026-05-16 空间感知修正：优先探测点击位置
    QModelIndex indexAtPos = activeView->indexAt(pos);
    QModelIndexList selectedRows;

    if (indexAtPos.isValid()) {
        // 1. 点击在项目上：确保该项被选中，并拉取所有选中项用于构建文件操作菜单
        if (!activeView->selectionModel()->isSelected(indexAtPos)) {
            activeView->selectionModel()->select(indexAtPos, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        
        auto allSelected = activeView->selectionModel()->selectedIndexes();
        for (const auto& idx : allSelected) {
            if (idx.column() == 0) selectedRows.append(idx);
        }
    } else {
        // 2. 点击在空白处：清空用于构建菜单的局部索引列表，确保下方 !selectedRows.isEmpty() 判定失败
        // 注意：这里不清除 view 的真实 selectionModel，仅让菜单表现为“无目标”状态
        selectedRows.clear();
    }
    
    QMenu menu(this);
    menu.setStyleSheet("QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; } QMenu::item:selected { background: #232D37; color: #FFF; }");

    if (!selectedRows.isEmpty()) {
        int count = selectedRows.size();
        menu.addAction(count > 1 ? "批量打开文件" : "打开文件", [this, selectedRows]() {
            for (const auto& index : selectedRows) onItemDoubleClicked(index);
        });
        
        menu.addAction("在“资源管理器”中显示", [this, selectedRows]() {
            QString path = m_tableModel->data(m_tableModel->index(selectedRows.first().row(), 1)).toString();
            QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(path)});
        });
        
        menu.addSeparator();
        
        menu.addAction(count > 1 ? "批量复制路径" : "复制路径", [this, selectedRows]() {
            QStringList paths;
            for (const auto& idx : selectedRows) paths << m_tableModel->data(m_tableModel->index(idx.row(), 1)).toString();
            QApplication::clipboard()->setText(paths.join("\n"));
        });
        
        menu.addAction(count > 1 ? "批量复制文件名" : "复制文件名", [this, selectedRows]() {
            QStringList names;
            for (const auto& idx : selectedRows) names << m_tableModel->data(m_tableModel->index(idx.row(), 0)).toString();
            QApplication::clipboard()->setText(names.join("\n"));
        });

        menu.addSeparator();
        menu.addAction("剪切", this, [this]() { onCopyTriggered(true); });
        menu.addAction("复制", this, [this]() { onCopyTriggered(false); });
        
        if (count == 1) {
            menu.addAction("重命名", this, [this]() {
                QTimer::singleShot(0, this, &ScanDialog::onRenameTriggered);
            });
        }
        
        menu.addSeparator();
        
        menu.addAction(count > 1 ? "批量删除" : "删除", [this, selectedRows]() {
            QString msg = (selectedRows.size() == 1) ? QString("确定要永久删除 %1 吗？").arg(m_tableModel->data(m_tableModel->index(selectedRows.first().row(), 0)).toString())
                                                   : QString("确定要永久删除选中的 %1 个项目吗？").arg(selectedRows.size());
            if (QMessageBox::question(this, "确认删除", msg) == QMessageBox::Yes) {
                for (const auto& idx : selectedRows) {
                    QString path = m_tableModel->data(m_tableModel->index(idx.row(), 1)).toString();
                    QFile::remove(path);
                }
                m_controller->triggerSearch(true);
            }
        });
        
        menu.addSeparator();
        
        menu.addAction("属性", [this, selectedRows]() {
            QString path = m_tableModel->data(m_tableModel->index(selectedRows.first().row(), 1)).toString();
            std::wstring wpath = path.toStdWString();
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_INVOKEIDLIST;
            sei.lpVerb = L"properties";
            sei.lpFile = wpath.c_str();
            sei.nShow = SW_SHOW;
            ShellExecuteExW(&sei);
        });

        menu.addSeparator();
    }

    // --- 2026-05-16 新增：视图、排序、刷新全局功能菜单 ---
    
    QMenu* viewMenu = menu.addMenu("视图(V)");

    // 在菜单弹出前，根据当前真实配置刷新勾选状态 [1]
    if (m_actJMode && m_actGMode && m_actListMode) {
        m_actJMode->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 0);
        m_actGMode->setChecked(m_config.viewMode == 1 && m_config.layoutMode == 1);
        m_actListMode->setChecked(m_config.viewMode == 0);

        // 直接将持久 Action 插入菜单中展现
        viewMenu->addAction(m_actJMode);
        viewMenu->addAction(m_actGMode);
        viewMenu->addAction(m_actListMode);
    }
    
    QMenu* sortMenu = menu.addMenu("排序(S)");
    QStringList sortOptions = {"名称", "路径", "大小", "修改日期"};
    for (int i = 0; i < sortOptions.size(); ++i) {
        QAction* act = sortMenu->addAction(sortOptions[i]);
        connect(act, &QAction::triggered, this, [this, i]() {
            auto* resultTableView = m_listResultView ? qobject_cast<QTableView*>(m_listResultView->getBaseView()) : nullptr;
            if (resultTableView) {
                Qt::SortOrder order = resultTableView->horizontalHeader()->sortIndicatorOrder();
                resultTableView->sortByColumn(i, order);
                m_config.sortColumn = i;
                m_config.sortOrder = static_cast<int>(order);
                m_config.save();
            }
        });
    }
    sortMenu->addSeparator();
    QAction* ascAction = sortMenu->addAction("升序(A)");
    QAction* descAction = sortMenu->addAction("降序(D)");
    connect(ascAction, &QAction::triggered, this, [this]() { 
        auto* resultTableView = m_listResultView ? qobject_cast<QTableView*>(m_listResultView->getBaseView()) : nullptr;
        if (resultTableView) {
            resultTableView->sortByColumn(resultTableView->horizontalHeader()->sortIndicatorSection(), Qt::AscendingOrder);
            m_config.sortOrder = 0;
            m_config.save();
        }
    });
    connect(descAction, &QAction::triggered, this, [this]() { 
        auto* resultTableView = m_listResultView ? qobject_cast<QTableView*>(m_listResultView->getBaseView()) : nullptr;
        if (resultTableView) {
            resultTableView->sortByColumn(resultTableView->horizontalHeader()->sortIndicatorSection(), Qt::DescendingOrder);
            m_config.sortOrder = 1;
            m_config.save();
        }
    });

    QAction* refreshAction = menu.addAction("刷新(R)");
    refreshAction->setShortcut(QKeySequence(Qt::Key_F5));
    connect(refreshAction, &QAction::triggered, this, &ScanDialog::onTriggerSearch);

    QAbstractItemView* view = qobject_cast<QAbstractItemView*>(sender());
    if (view) menu.exec(view->viewport()->mapToGlobal(pos));
}

void ScanDialog::onItemDoubleClicked(const QModelIndex& index) {
    if (!index.isValid()) return;
    
    QString path = m_tableModel->data(m_tableModel->index(index.row(), 1)).toString();
    ShellExecuteW(NULL, L"open", reinterpret_cast<const wchar_t*>(path.utf16()), NULL, NULL, SW_SHOWNORMAL);
}

void ScanDialog::onSelectionChanged() {
    updateStatus("就绪");
}


void ScanDialog::onStartScan(const QString& drive) {
    QStringList selectedDrives;
    if (drive.isEmpty()) {
        for (const auto& d : m_config.activeDrives) selectedDrives << (d + QLatin1String("\\"));
    } else {
        selectedDrives << (drive + QLatin1String("\\"));
    }
    
    if (selectedDrives.isEmpty()) { onTriggerSearch(); return; }
    updateStatus("正在扫描...", true);

    QPointer<ScanDialog> weakThis(this);
    (void)(QtConcurrent::run)([weakThis, selectedDrives]() {
        MftReader::instance().buildIndex(selectedDrives);
        QMetaObject::invokeMethod(weakThis.data(), [weakThis]() {
            if (!weakThis) return;
            weakThis->updateStatus("就绪");
            weakThis->updateDriveButtonStyles();
            weakThis->onTriggerSearch();
            weakThis->triggerWarmup();
        });
    });
}

void ScanDialog::onTriggerSearch() {
    QString q = m_searchEdit->text().trimmed();
    QString e = m_extEdit->text().trimmed();
    
    QTimer::singleShot(10, this, [this, q, e]() {
        bool changed = false;
        if (!q.isEmpty() && (m_config.queryHistory.isEmpty() || m_config.queryHistory.first() != q)) {
            m_config.queryHistory.removeAll(q);
            m_config.queryHistory.prepend(q);
            if (m_config.queryHistory.size() > 10) m_config.queryHistory.removeLast();
            changed = true;
        }
        if (!e.isEmpty() && (m_config.extHistory.isEmpty() || m_config.extHistory.first() != e)) {
            m_config.extHistory.removeAll(e);
            m_config.extHistory.prepend(e);
            if (m_config.extHistory.size() > 10) m_config.extHistory.removeLast();
            changed = true;
        }
        if (changed) m_config.save();
    });

    QStringList activeList;
    for (const QString& drive : m_config.activeDrives) activeList << drive;
    MftReader::instance().updateActiveDrives(activeList);

    onFilterOptionChanged();
    m_controller->setSearchText(m_searchEdit->text());
    m_controller->triggerSearch(true); // 按钮点击立即搜索
}

void ScanDialog::onFilterOptionChanged() {
    // 2026-06-xx 性能优化：移除这里的 config.save() 和频繁的驱动器同步。
    // 只有在明确需要重新搜索时才触发。驱动器同步已移至 onStartScan 或 onTriggerSearch。
    
    m_config.useRegex = m_checkRegex->isChecked();
    m_config.caseSensitive = m_checkCase->isChecked();
    m_config.includeHidden = m_checkHidden->isChecked();
    m_config.includeSystem = m_checkSystem->isChecked();
    m_config.includeDollar = m_checkDollar->isChecked();
    m_config.autoDisplay = m_checkAuto->isChecked();

    ScanFilterState state;
    state.useRegex = m_config.useRegex;
    state.caseSensitive = m_config.caseSensitive;
    state.includeHidden = m_config.includeHidden;
    state.includeSystem = m_config.includeSystem;
    state.includeDollar = m_config.includeDollar;
    state.autoDisplay = m_config.autoDisplay;
    QString extText = m_extEdit->text().toLower();
    if (!extText.isEmpty()) state.extensionList = extText.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts);
    
    m_controller->setFilterState(state);
    // 2026-06-xx 物理对标：配置变更（如勾选开关）时触发立即搜索，以响应“自动显示”等开关状态
    m_controller->triggerSearch(true);
}

void ScanDialog::updateStatus(const QString& text, bool scanning, int64_t totalCount) {
    Q_UNUSED(text);
    if (m_titleStatusLabel) {
        // 2026-07-07 物理修正：标题栏仅展示激活盘符的文件总数 (Analysis_Modification_Plan-154.md)
        int64_t total = (totalCount >= 0) ? totalCount : MftReader::instance().activeCount();
        m_titleStatusLabel->setText(QString("%1 - %2").arg(scanning ? "SCANNING" : "READY").arg(formatNumber(total)));
        m_titleStatusLabel->setStyleSheet(scanning ? "color: #FF8C00; font-size: 10px; font-weight: bold;" : "color: #46B478; font-size: 10px; font-weight: bold;");
    }
    
    if (scanning) { m_progressBar->show(); m_progressBar->setRange(0, 0); }
    else { m_progressBar->hide(); updateStatusBar(); }
}

void ScanDialog::updateStatusBar() {
    if (!m_currentActiveView) return;
    auto view = m_currentActiveView->getBaseView();
    auto selectedRows = view->selectionModel()->selectedRows();
    
    int totalMatch = m_controller->resultCount();
    int loadedDrives = 0;
    for (const auto& info : m_cachedDriveInfos) {
        if (MftReader::instance().isDriveIndexed(info.letter)) loadedDrives++;
    }
    // 2026-07-07 物理修正：更新状态栏文案 (Analysis_Modification_Plan-154.md)
    m_statLabelMain->setText(QString("当前仅在 %1 个已加载盘符范围内搜索 (匹配: %2)").arg(loadedDrives).arg(formatNumber(totalMatch)));
    m_statLabelTime->setText(QString("耗时 %1 ms").arg(m_lastSearchMs));

    if (!selectedRows.isEmpty()) {
        m_selectionLabel->show();
        int64_t totalSize = 0;
        auto& reader = MftReader::instance();
        for (const auto& index : selectedRows) {
            uint64_t key = m_tableModel->data(index, Qt::UserRole).toULongLong();
            int actualIdx = reader.getIndexByKey(key);
            if (actualIdx != -1 && !reader.isDirectory(actualIdx)) totalSize += reader.getSize(actualIdx);
        }
        m_selectionLabel->setText(QString(" | 已选择 %1 项 (%2)").arg(selectedRows.size()).arg(formatSize(totalSize)));
        
        if (selectedRows.size() > 1) m_csvBtn->show();
        else m_csvBtn->hide();
    } else {
        m_selectionLabel->hide();
        m_csvBtn->hide();
    }
    
    int64_t dbTotal = MftReader::instance().totalCount();
    double memoryMb = (dbTotal * 184.0) / 1024.0 / 1024.0;
    // 2026-07-07 架构优化：将全局索引总数下放至状态栏辅助信息 (Analysis_Modification_Plan-154.md)
    m_statLabelMemory->setText(QString("索引总量: %1 | 数据占用: %2 MB").arg(formatNumber(dbTotal)).arg(memoryMb, 0, 'f', 1));

}

void ScanDialog::refreshVisibleMetadataRange() {
    if (!m_tableModel || !m_currentActiveView) return;
    QAbstractItemView* view = m_currentActiveView->getBaseView();

    // 自动多态计算可视行
    int top = 0;
    int bottom = m_tableModel->rowCount() - 1;

    QModelIndex topIdx = view->indexAt(QPoint(10, 10));
    QModelIndex bottomIdx = view->indexAt(QPoint(view->viewport()->width() - 10, view->viewport()->height() - 10));

    if (topIdx.isValid()) top = topIdx.row();
    if (bottomIdx.isValid()) bottom = bottomIdx.row();

    m_tableModel->setVisibleRange(top, bottom);
}

int ScanDialog::calculateNameColumnMinimumWidth() const {
    if (!m_tableModel || !m_listResultView) return 260;
    auto* resultTableView = m_listResultView->getBaseView();
    if (!resultTableView) return 260;

    int rowHeight = m_config.iconSize;
    int cardWidth = rowHeight - 6; // 还原 ListThumbnailDelegate 内部计算的侧边长 [1]
    int basePadding = 6 + 10 + 10; // 左侧内边距(6px) + 间隙(10px) + 右侧文字边缘安全保护(10px) [1]

    // 获取 QTableView 当前使用的字体度量器
    QFontMetrics fm = resultTableView->fontMetrics();
    int maxTextWidth = 100; // 给文字留出基础的 100 像素安全区域

    // 平衡性能与精度：仅遍历当前结果集中的前 1000 个项目，防止大集合（如 200万数据）在主线程产生卡顿
    auto snapshot = m_controller->snapshot();
    if (!snapshot) return 260;
    int count = std::min<int>(1000, (int)snapshot->keys.size());
    auto& reader = MftReader::instance();

    for (int i = 0; i < count; ++i) {
        uint64_t key = snapshot->keys[i];
        int actualIdx = reader.getIndexByKey(key);
        if (actualIdx != -1) {
            QString name = reader.getName(actualIdx);
            // 使用 horizontalAdvance 精确测算该文件名在当前字体下的物理像素宽度 [1]
            int textWidth = fm.horizontalAdvance(name);
            if (textWidth > maxTextWidth) {
                maxTextWidth = textWidth;
            }
        }
    }

    // 精确返回：正方形卡片宽度 + 最长文件名像素宽 + 间距补偿 [1]
    return cardWidth + maxTextWidth + basePadding;
}

QString ScanDialog::formatNumber(int64_t n) {
    return QLocale(QLocale::English).toString(n);
}

QString ScanDialog::formatSize(int64_t bytes) {
    if (bytes == 0) return "0 B";
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < units.size() - 1) {
        size /= 1024.0;
        unit++;
    }
    return QString("%1 %2").arg(size, 0, 'f', 2).arg(units[unit]);
}

void ScanDialog::onRenameTriggered() {
    if (!m_currentActiveView) return;
    auto view = m_currentActiveView->getBaseView();
    auto selection = view->selectionModel()->selectedRows();
    if (selection.isEmpty()) return;
    
    // 2026-05-16 交互进化：触发行内编辑
    view->edit(selection.first());
}

void ScanDialog::onCopyTriggered(bool isCut) {
    if (!m_currentActiveView) return;
    auto view = m_currentActiveView->getBaseView();
    auto selectedIndexes = view->selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty()) return;

    QMimeData* mimeData = m_tableModel->mimeData(selectedIndexes);
    if (!mimeData) return;

    // 2026-07-07 物理修复：通过 Preferred DropEffect 区分复制与剪切
    QByteArray effectData;
    QDataStream stream(&effectData, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << (isCut ? (quint32)2 : (quint32)1); // MOVE=2, COPY=1
    mimeData->setData("Preferred DropEffect", effectData);

    QApplication::clipboard()->setMimeData(mimeData);
}

void ScanDialog::setHistoryText(const QString& text, bool isQuery) {
    if (isQuery) {
        m_searchEdit->setText(text);
    } else {
        m_extEdit->setText(text);
    }
    onTriggerSearch();
}

void ScanDialog::removeHistoryItem(const QString& text, bool isQuery) {
    if (isQuery) {
        m_config.queryHistory.removeAll(text);
    } else {
        m_config.extHistory.removeAll(text);
    }
    m_config.save(); // 保存最新的去重单项状态
}

void ScanDialog::reopenHistoryMenu(bool isQuery) {
    // 重新模拟鼠标双击从而完美重新唤醒刷新后的最新下拉面板，提供完美无感动画
    QWidget* target = isQuery ? static_cast<QWidget*>(m_searchEdit) : static_cast<QWidget*>(m_extEdit);
    if (target) {
        QMouseEvent me(QEvent::MouseButtonDblClick, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &me);
    }
}


void ScanDialog::keyPressEvent(QKeyEvent* event) {
    // 2026-07-10 新增：专属 ScanDialog 主窗口的两段式 Esc 处理（对应用户原话：“当在ScanDialog窗口首次按下键时”）
    if (event->key() == Qt::Key_Escape) {
        bool searchNotEmpty = m_searchEdit && !m_searchEdit->text().isEmpty();
        bool extNotEmpty = m_extEdit && !m_extEdit->text().isEmpty();

        // 1. 若至少有一个输入框不为空，则优先执行一键全部重置清空文字（对应用户原话：“应该先清空ScanDialog主窗口所有输入框的文字”）
        if (searchNotEmpty || extNotEmpty) {
            if (m_searchEdit) m_searchEdit->clear();
            if (m_extEdit) m_extEdit->clear();
            event->accept();
            return; // 消费按键，拦截阻止其传播至基类直接关闭窗口
        }
        
        // 2. 若所有输入框均已经处于清空重置状态，则直接关闭 ScanDialog 主窗口（对应用户原话：“所有输入框都已经处于清空文字状态情况下则直接关闭ScanDialog窗口”）
        reject();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Space) {
        if (!m_currentActiveView) return;
        auto* view = m_currentActiveView->getBaseView();
        QModelIndex idx = view->currentIndex();
        if (idx.isValid()) {
            if (m_quickLook->isVisible()) {
                m_quickLook->closePreview();
            } else {
                QString path = m_tableModel->data(m_tableModel->index(idx.row(), 1)).toString();
                // 2026-07-11 物理移植自 ArcMeta (Plan-179)：前置属性准入判断，文件夹/黑名单直接 return 阻断
                if (!isPathPreviewable(path, m_config)) return;
                m_quickLook->preview(path);
            }
        }
        return;
    }
    if (event->key() == Qt::Key_F2) {
        onRenameTriggered();
        return;
    }
    if (event->key() == Qt::Key_F5) {
        onTriggerSearch();
        return;
    }
    if (event->key() == Qt::Key_A && event->modifiers() == Qt::ControlModifier) { 
        // 2026-07-07 物理修复：调用全量选择逻辑，杜绝虚拟化加载导致的“漏选”
        selectAllResults();
        return; 
    }
    if (event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier) {
        onCopyTriggered(false);
        return;
    }
    if (event->key() == Qt::Key_X && event->modifiers() == Qt::ControlModifier) {
        onCopyTriggered(true);
        return;
    }
    if (event->key() == Qt::Key_V && event->modifiers() == Qt::ControlModifier) {
        // 2026-07-07 物理清理：移除粘贴功能，仅保留提示 (Analysis_Modification_Plan-154.md)
        updateStatus("当前视图不支持粘贴");
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_searchEdit->hasFocus() || m_extEdit->hasFocus()) {
            onTriggerSearch();
        } else {
            if (!m_currentActiveView) return;
            auto view = m_currentActiveView->getBaseView();
            auto index = view->currentIndex();
            if (index.isValid()) onItemDoubleClicked(index);
        }
        return;
    }
    handleMetadataShortcut(event);
    FramelessDialog::keyPressEvent(event);
}

void ScanDialog::selectAllResults() {
    // 2026-07-07 核心修复：不依赖 QAbstractItemView::selectAll()，因为它依赖视图内部几何状态（可能因虚拟化而未就绪）
    m_tableModel->forceFetchAll();
    int total = m_tableModel->rowCount();
    if (total <= 0) return;

    if (!m_currentActiveView) return;
    auto view = m_currentActiveView->getBaseView();

    // 直接构建覆盖全量行的 QItemSelection，绕过视图布局依赖
    QItemSelection selection(
        m_tableModel->index(0, 0), 
        m_tableModel->index(total - 1, m_tableModel->columnCount() - 1)
    );
    view->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    
    // 手动触发状态栏更新，因为同步调用可能错过 selectionChanged 信号（取决于具体实现）
    updateStatusBar();
}

// 2026-05-16 快捷键核心处理逻辑：支持评分、置顶、标签等深度管理快捷键
void ScanDialog::handleMetadataShortcut(QKeyEvent* event) {
    // 2026-06-xx 任务：移除深度管理功能，停用对应的快捷键分发逻辑。
    Q_UNUSED(event);
}

void ScanDialog::triggerWarmup() {
    // 极致体感：流水线异步预热 
    QPointer<ScanDialog> weakThis(this);
     
    // 我们不使用全局 QtConcurrent，而是直接针对 m_thumbPool 进行定向精准预热 
    if (!weakThis || !weakThis->m_tableModel || !weakThis->m_tableModel->m_thumbPool) { 
        return; 
    } 
 
    auto* pool = weakThis->m_tableModel->m_thumbPool; 
    int maxThreads = pool->maxThreadCount(); // 获取当前线程池的最大并发线程数（SSD 模式下可能为 2~4，HDD 模式下为 1） 
 
    // 物理预热：让线程池里的每一个工作线程都至少执行一次初始化 
    for (int t = 0; t < maxThreads; ++t) { 
        pool->start([weakThis]() { 
            if (!weakThis) return; 
 
            // 1. 确保每个线程的 COM 环境基础（ScopedComInit）在这里完成首次物理初始化 
            static QThreadStorage<ScopedComInit> comStorage; 
            if (!comStorage.hasLocalData()) { 
                comStorage.setLocalData(ScopedComInit()); 
            } 
 
            // 2. 提前让该线程触发一次 Shell 引擎调用 
            int total = MftReader::instance().totalCount(); 
            if (total > 0) { 
                for (int i = 0; i < std::min(total, 5000); ++i) { 
                    if (!MftReader::instance().isDirectory(i)) { 
                        QString ext = MftReader::instance().getExtQString(i); 
                        // 仅寻找一个图片类型文件执行首次 getShellThumbnail 模拟调用 
                        if (UiHelper::isGraphicsFile(ext)) { 
                            QString dummyPath = MftReader::instance().getFullPath(i); 
                            if (!dummyPath.isEmpty()) { 
                                // 此时 Shell32、WIC、套间等一次性初始化成本在此工作线程内瞬间被消化掉 
                                UiHelper::getShellThumbnail(dummyPath, 64); 
                            } 
                            break;  
                        } 
                    } 
                } 
            } 
        }); 
    } 
}

bool ScanDialog::eventFilter(QObject* watched, QEvent* event) {
    // 启动安全防御：严格的空指针保障检测
    if (!m_listResultView || !m_justifiedResultView || !m_gridResultView || !m_itemToolTipTimer || !m_tableModel) {
        return FramelessDialog::eventFilter(watched, event);
    }

    bool isViewOrViewport = false;
    QAbstractItemView* view = nullptr;

    for (auto* resView : {m_listResultView, m_justifiedResultView, m_gridResultView}) {
        QAbstractItemView* base = resView->getBaseView();
        if (watched == base || watched == base->viewport()) {
            isViewOrViewport = true;
            view = base;
            break;
        }
    }

    if (isViewOrViewport && view) {

        if (event->type() == QEvent::ToolTip) {
            // 极其重要：直接返回 true 拦截 QEvent::ToolTip 底层 QHelpEvent，彻底屏蔽原生的 ToolTip 气泡
            return true;
        }

        if (event->type() == QEvent::MouseMove) {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            // 完美坐标对准：将全局坐标转换为视口（viewport）局部坐标，确保 indexAt 判定精确度
            QPoint viewportPos = view->viewport()->mapFromGlobal(me->globalPosition().toPoint());
            QModelIndex idx = view->indexAt(viewportPos);

            if (idx.isValid()) {
                QModelIndex col0Idx = m_tableModel->index(idx.row(), 0);
                
                // 不管是不是同一个 item，只要鼠标移动，就必须立即隐藏已有的 ToolTipOverlay 并重置定时器
                m_itemToolTipTimer->stop();
                ToolTipOverlay::hideTip();

                m_hoveredIndex = col0Idx;
                m_hoveredGlobalPos = me->globalPosition().toPoint();
                m_itemToolTipTimer->start(); // 重新开始 2000ms 计时
            } else {
                // 如果鼠标移动到了空白区域，隐藏提示并停止定时器
                m_itemToolTipTimer->stop();
                ToolTipOverlay::hideTip();
                m_hoveredIndex = QModelIndex();
            }
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave ||
                   event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusOut) {
            // 鼠标移出、点击、失去焦点时，立即停止定时器并隐藏提示窗
            m_itemToolTipTimer->stop();
            ToolTipOverlay::hideTip();
            m_hoveredIndex = QModelIndex();
        }
    }

    if (isViewOrViewport && event->type() == QEvent::Wheel) {
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
        if (wheelEvent->modifiers() & Qt::ControlModifier) {
            int deltaY = wheelEvent->angleDelta().y();
            if (deltaY > 0) {
                m_sizeSlider->setValue(m_sizeSlider->value() + 10);
            } else if (deltaY < 0) {
                m_sizeSlider->setValue(m_sizeSlider->value() - 10);
            }
            return true; // 拦截事件，防止 Ctrl + 滚轮导致列表区域意外滚动
        }
    }

    if (isViewOrViewport && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Space) {
            QModelIndex idx = view->currentIndex();
            if (idx.isValid()) {
                if (m_quickLook->isVisible()) {
                    m_quickLook->closePreview();
                } else {
                    QString path = m_tableModel->data(m_tableModel->index(idx.row(), 1)).toString();
                    // 2026-07-11 物理移植自 ArcMeta (Plan-179)：前置属性准入判断，文件夹/黑名单直接 return 阻断
                    if (!isPathPreviewable(path, m_config)) return true;
                    m_quickLook->preview(path);
                }
            }
            return true; // 拦截事件，防止 TableView 处理空格导致滚动
        }
    }

    if (watched == m_sizeSlider && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            int val = QStyle::sliderValueFromPosition(m_sizeSlider->minimum(), m_sizeSlider->maximum(), me->pos().x(), m_sizeSlider->width());
            m_sizeSlider->setValue(val);
            return true;
        }
    }

    if ((watched == m_searchEdit || watched == m_extEdit) && event->type() == QEvent::MouseButtonDblClick) {
        bool isQuery = (watched == m_searchEdit);
        const QStringList& history = isQuery ? m_config.queryHistory : m_config.extHistory;
        
        if (!history.isEmpty()) {
            QMenu menu(this);
            // 升级样式：加入对 QWidgetAction 自定义组件的 QMenu 内部样式修饰，保持 1A1A1A 深色系的高级感
            // 特别添加 "QMenu::item { padding: 0px 0px; background: transparent; }"，彻底干掉 Qt 默认保留的高宽度 Gutter 图标槽，
            // 让我们的自定义组件能够完整占据 120px 宽度，从而完美露出极右侧的“×”删除按钮！
            menu.setStyleSheet(
                "QMenu { background: #1A1A1A; color: #CCC; border: 1px solid #333; border-radius: 6px; padding: 4px 0; }"
                "QMenu::item { padding: 0px 0px; background: transparent; }"
                "QMenu::separator { height: 1px; background: #333; margin: 4px 0; }"
            );

            // 使下拉面板的宽度与输入框的宽度保持一致
            QWidget* editWidget = static_cast<QWidget*>(watched);
            if (editWidget) {
                menu.setFixedWidth(editWidget->width());
            }
            
            // 使用 QWidgetAction 为每一条历史记录嵌入带有“×”的交互式控件 (对应用户原话：“每个选项右侧都应该有一个“×”……轻松移除某个选项”)
            for (const QString& item : history) {
                auto* wa = new QWidgetAction(&menu);
                auto* itemWidget = new HistoryItemWidget(item, isQuery, &menu, this, &menu);
                wa->setDefaultWidget(itemWidget);
                menu.addAction(wa);
            }
            
            menu.addSeparator();
            
            // 保留一键清空机制，同样以 QWidgetAction 来适配高阶菜单视觉
            auto* clearAction = menu.addAction("清空历史记录", [this, isQuery, &menu]() {
                if (isQuery) m_config.queryHistory.clear();
                else m_config.extHistory.clear();
                m_config.save();
                menu.close();
            });
            clearAction->setIcon(UiHelper::getIcon("close", QColor("#FF4444"), 12));
            
            menu.exec(static_cast<QWidget*>(watched)->mapToGlobal(QPoint(0, static_cast<QWidget*>(watched)->height())));
            return true;
        }
    }

    return FramelessDialog::eventFilter(watched, event);
}

// ============================================================================
// PreviewRulesDialog 实现
// ============================================================================
PreviewRulesDialog::PreviewRulesDialog(ScanConfig& config, QWidget* parent)
    : FramelessDialog("预览配置", parent), m_config(config)
{
    resize(600, 480);
    setMinimumSize(500, 400);

    auto* layout = new QVBoxLayout(m_contentArea);
    layout->setContentsMargins(20, 15, 20, 20);
    layout->setSpacing(10);

    // 1. Whitelist label and editor
    auto* lblWhite = new QLabel("文件预览白名单 (放行规则，以中英文逗号分隔):");
    lblWhite->setStyleSheet("color: #EEEEEE; font-size: 12px; font-weight: bold;");
    layout->addWidget(lblWhite);

    m_whitelistEdit = new QTextEdit();
    m_whitelistEdit->setPlaceholderText("例如: jpg, png, txt, cpp, h, py");
    m_whitelistEdit->setStyleSheet(
        "QTextEdit {"
        "  background-color: #2D2D2D; border: 1px solid #444; border-radius: 6px;"
        "  padding: 8px; color: white; selection-background-color: #3498db;"
        "  font-size: 13px; line-height: 1.4;"
        "}"
        "QTextEdit:focus { border: 1px solid #3498db; }"
    );
    layout->addWidget(m_whitelistEdit);

    // 2. Blacklist label and editor
    auto* lblBlack = new QLabel("文件预览黑名单 (拦截规则，以中英文逗号分隔):");
    lblBlack->setStyleSheet("color: #EEEEEE; font-size: 12px; font-weight: bold;");
    layout->addWidget(lblBlack);

    m_blacklistEdit = new QTextEdit();
    m_blacklistEdit->setPlaceholderText("例如: exe, dll, zip, rar, mp4");
    m_blacklistEdit->setStyleSheet(
        "QTextEdit {"
        "  background-color: #2D2D2D; border: 1px solid #444; border-radius: 6px;"
        "  padding: 8px; color: white; selection-background-color: #3498db;"
        "  font-size: 13px; line-height: 1.4;"
        "}"
        "QTextEdit:focus { border: 1px solid #3498db; }"
    );
    layout->addWidget(m_blacklistEdit);

    // Populate data
    QStringList whiteList;
    for (const auto& ext : m_config.previewWhitelist) whiteList.append(ext);
    whiteList.sort();
    m_whitelistEdit->setPlainText(whiteList.join(", "));

    QStringList blackList;
    for (const auto& ext : m_config.previewBlacklist) blackList.append(ext);
    blackList.sort();
    m_blacklistEdit->setPlainText(blackList.join(", "));

    // 3. Buttons row
    auto* btnLayout = new QHBoxLayout();
    
    auto* btnRestore = new QPushButton("恢复默认");
    btnRestore->setFixedSize(100, 32);
    btnRestore->setCursor(Qt::PointingHandCursor);
    btnRestore->setStyleSheet(
        "QPushButton { background-color: transparent; color: #FF8C00; border: 1px solid #FF8C00; border-radius: 4px; } "
        "QPushButton:hover { background-color: rgba(255, 140, 0, 0.1); }"
    );
    connect(btnRestore, &QPushButton::clicked, this, &PreviewRulesDialog::onRestoreDefaults);
    btnLayout->addWidget(btnRestore);

    btnLayout->addStretch();
    
    auto* btnCancel = new QPushButton("取消");
    btnCancel->setFixedSize(80, 32);
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnCancel->setStyleSheet(
        "QPushButton { background-color: transparent; color: #888; border: 1px solid #444; border-radius: 4px; } "
        "QPushButton:hover { color: #EEE; background-color: #333; }"
    );
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(btnCancel);

    auto* btnOk = new QPushButton("确定");
    btnOk->setFixedSize(80, 32);
    btnOk->setCursor(Qt::PointingHandCursor);
    btnOk->setStyleSheet(
        "QPushButton { background-color: #3498db; color: white; border: none; border-radius: 4px; font-weight: bold; } "
        "QPushButton:hover { background-color: #1a7abf; }" 
    );
    connect(btnOk, &QPushButton::clicked, this, &PreviewRulesDialog::onConfirm);
    btnLayout->addWidget(btnOk);

    layout->addLayout(btnLayout);
}

void PreviewRulesDialog::onRestoreDefaults() {
    QStringList whiteList;
    for (const auto& ext : DEFAULT_WHITELIST) whiteList.append(ext);
    whiteList.sort();
    m_whitelistEdit->setPlainText(whiteList.join(", "));

    QStringList blackList;
    for (const auto& ext : DEFAULT_BLACKLIST) blackList.append(ext);
    blackList.sort();
    m_blacklistEdit->setPlainText(blackList.join(", "));
}

void PreviewRulesDialog::onConfirm() {
    auto parseExtensions = [](const QString& text) -> QSet<QString> {
        QSet<QString> set;
        QString temp = text;
        // 1. 将中文逗号、回车、换行全部统一替换为西文逗号（对应用户原话：“支持中英文逗号分割”）
        temp.replace(QString::fromUtf8("，"), ",");
        temp.replace('\n', ',');
        temp.replace('\r', ',');
        
        // 2. 严格按逗号进行物理分割，而不是空格分割（对应用户原话：“采用逗号分割……而不是空格分割”）
        QStringList list = temp.split(',', Qt::SkipEmptyParts);
        for (const QString& item : list) {
            // 去除多余的空格（例如逗号后的空格）
            QString clean = item.trimmed().toLower();
            if (clean.startsWith('.')) {
                clean = clean.mid(1);
            }
            if (!clean.isEmpty()) {
                set.insert(clean);
            }
        }
        return set;
    };

    m_config.previewWhitelist = parseExtensions(m_whitelistEdit->toPlainText());
    m_config.previewBlacklist = parseExtensions(m_blacklistEdit->toPlainText());

    accept();
}

} // namespace FERREX
