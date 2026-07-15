cmake_minimum_required(VERSION 3.20)
project(FERREX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 修复：优先探测 QT_DIR 环境变量，若无则使用文档红线默认路径
if(DEFINED ENV{QT_DIR})
    set(CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
else()
    set(CMAKE_PREFIX_PATH "C:/Qt/6.10.2/msvc2022_64")
endif()

# 强制 MSVC 编译器及选项 (文档红线: /W4 /O2)
if(MSVC)
    add_compile_options(/W4 /O2 /utf-8)
endif()

# Qt 6 基础组件
# 2026-05-10 修复：添加缺失的Core和Gui组件
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Sql Concurrent)

# 可选组件：Qt Multimedia (多媒体播放能力)
find_package(Qt6 COMPONENTS Multimedia MultimediaWidgets QUIET)

set(EXTRA_LIBS)
if(Qt6Multimedia_FOUND AND Qt6MultimediaWidgets_FOUND)
    add_compile_definitions(FERREX_HAS_MULTIMEDIA)
    list(APPEND EXTRA_LIBS Qt6::Multimedia Qt6::MultimediaWidgets)
    message(STATUS "🎉 检测到 Qt Multimedia 模块，已开启视频/音频原生预览支持！")
else()
    message(WARNING "⚠️ 未检测到 Qt Multimedia 模块，视频/音频预览将采用优雅降级机制显示占位图。")
endif()

# 2026-03-xx 按照用户要求：禁用 Vulkan 探测以消除日志报错，并清理构建路径矛盾
set(QT_NO_VULKAN ON)

# 路径严谨性核查：若检测到 VS 默认的 out 路径，主动输出警告提示
if(CMAKE_BINARY_DIR MATCHES "/out/")
    message(WARNING "⚠️ 检测到非法构建路径 [out]！请在 VS 工具栏切换至 [x64-release] 预设以符合《源码纯净》规范。")
endif()

# 2026-03-xx 按照用户要求：将可执行文件直接输出到指定项目子目录 FERREX 下，解决 \out 文件夹查找不便的问题
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/FERREX")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/FERREX")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/FERREX")

# 自动生成 MOC/UIC/RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 2026-06-xx 按照用户要求：彻底废除递归搜集 (GLOB)，改为显式列出所有有效源文件。
# 此举可确保构建系统在彻底移除 .am_meta.json 模块后不再包含冗余代码，符合《源码纯净》规范。
set(SOURCES
    src/main.cpp
    src/core/CoreController.cpp
    src/core/CoreController.h
    src/core/CacheManager.cpp
    src/core/CacheManager.h
    src/core/IndexedEntry.h
    src/mft/UsnWatcher.cpp
    src/mft/UsnWatcher.h
    src/crypto/EncryptionManager.cpp
    src/crypto/EncryptionManager.h
    src/meta/AllFrnManager.cpp
    src/meta/AllFrnManager.h
    src/meta/BatchRenameEngine.cpp
    src/meta/BatchRenameEngine.h
    src/meta/MetadataDefs.h
    src/meta/MetadataManager.cpp
    src/meta/MetadataManager.h
    src/mft/MftReader.cpp
    src/mft/MftReader.h
    src/mft/NtfsVolumeMftParser.cpp
    src/mft/NtfsVolumeMftParser.h
    src/mft/MemoryQueryEngine.cpp
    src/mft/MemoryQueryEngine.h
    src/mft/ScchCache.cpp
    src/mft/ScchCache.h
    src/mft/UsnJournalTreeSynchronizer.cpp
    src/mft/UsnJournalTreeSynchronizer.h
    src/mft/DiskIndexCacheCoordinator.cpp
    src/mft/DiskIndexCacheCoordinator.h
    src/ui/FramelessDialog.cpp
    src/ui/FramelessDialog.h
    src/ui/JustifiedView.cpp
    src/ui/JustifiedView.h
    src/ui/QuickLookWindow.cpp
    src/ui/QuickLookWindow.h
    src/ui/FramelessResizeBorder.cpp
    src/ui/FramelessResizeBorder.h
    src/ui/ToolTipOverlay.cpp
    src/ui/ToolTipOverlay.h
    src/ui/HoverEventFilter.cpp
    src/ui/HoverEventFilter.h
    src/ui/ThumbnailDelegate.cpp
    src/ui/ThumbnailDelegate.h
    src/ui/TrayController.cpp
    src/ui/TrayController.h
    src/ui/ScanController.cpp
    src/ui/ScanController.h
    src/ui/ScanDialog.cpp
    src/ui/ScanDialog.h
    src/ui/ScanTableModel.cpp
    src/ui/ScanTableModel.h
    src/ui/ResultTableColumnWidthPolicy.cpp
    src/ui/ResultTableColumnWidthPolicy.h
    src/ui/StatusBarFormatter.cpp
    src/ui/StatusBarFormatter.h
    src/ui/SystemDriveScanner.cpp
    src/ui/SystemDriveScanner.h
    src/ui/HistoryDropdownController.cpp
    src/ui/HistoryDropdownController.h
    src/ui/ContextMenuExecutor.cpp
    src/ui/ContextMenuExecutor.h
    src/ui/ThumbnailWarmupPipeline.cpp
    src/ui/ThumbnailWarmupPipeline.h
    src/ui/GlobalKeyboardShortcutHandler.cpp
    src/ui/GlobalKeyboardShortcutHandler.h
    src/ui/ViewportTooltipController.cpp
    src/ui/ViewportTooltipController.h
    src/ui/IScanResultView.h
    src/ui/ListResultView.cpp
    src/ui/ListResultView.h
    src/ui/JustifiedResultView.cpp
    src/ui/JustifiedResultView.h
    src/ui/GridResultView.cpp
    src/ui/GridResultView.h
    src/ui/ConfigManager.cpp
    src/ui/ConfigManager.h
    src/ui/UiHelper.h
    FERREX.rc
    resources/resources.qrc
)

# 可执行文件 (添加 WIN32 属性以消除控制台黑框)
# 修复：移除直接引用的 "FERREX.manifest"，改由 FERREX.rc 引入以杜绝 mt.exe 冲突
add_executable(FERREX WIN32 ${SOURCES})

# 链接库 (文档红线列表: ole32, bcrypt + QtSql)
# 2026-04-14 按照用户要求：恢复 MFT 逻辑，重新引入 ntdll 依赖
# 2026-05-10 修复：添加缺失的Qt6::Core和Qt6::Gui库
target_link_libraries(FERREX PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::Concurrent
    ${EXTRA_LIBS}
    ntdll
    ole32
    bcrypt
)

# 2026-03-xx 按照用户要求及《记忆碎片》规范：
# 为避免 mt.exe 清单合并冲突，通过 FERREX.rc 嵌入清单并禁用链接器的默认清单生成
if(MSVC)
    target_link_options(FERREX PRIVATE "/MANIFEST:NO")
endif()
# Force Refresh CMake: 2026-06-xx 11:30 (Removed AmMetaJson & SyncQueue, switched to DB-only)
