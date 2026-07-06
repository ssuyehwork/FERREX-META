cmake_minimum_required(VERSION 3.20)
project(ArcMeta)

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

# 2026-03-xx 按照用户要求：禁用 Vulkan 探测以消除日志报错，并清理构建路径矛盾
set(QT_NO_VULKAN ON)

# 路径严谨性核查：若检测到 VS 默认的 out 路径，主动输出警告提示
if(CMAKE_BINARY_DIR MATCHES "/out/")
    message(WARNING "⚠️ 检测到非法构建路径 [out]！请在 VS 工具栏切换至 [x64-release] 预设以符合《源码纯净》规范。")
endif()

# 2026-03-xx 按照用户要求：将可执行文件直接输出到指定项目子目录 ArcMeta 下，解决 \out 文件夹查找不便的问题
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/ArcMeta")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/ArcMeta")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/ArcMeta")

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
    src/db/CategoryRepo.cpp
    src/db/CategoryRepo.h
    src/db/Database.cpp
    src/db/Database.h
    src/db/FavoritesRepo.cpp
    src/db/FavoritesRepo.h
    src/db/FolderRepo.cpp
    src/db/FolderRepo.h
    src/db/ItemRepo.cpp
    src/db/ItemRepo.h
    src/db/SyncEngine.cpp
    src/db/SyncEngine.h
    src/meta/AmMetaJson.cpp
    src/meta/AmMetaJson.h
    src/meta/AllFrnManager.cpp
    src/meta/AllFrnManager.h
    src/meta/BatchRenameEngine.cpp
    src/meta/BatchRenameEngine.h
    src/meta/MetadataDefs.h
    src/meta/MetadataManager.cpp
    src/meta/MetadataManager.h
    src/mft/MftReader.cpp
    src/mft/MftReader.h
    src/mft/ScchCache.cpp
    src/mft/ScchCache.h
    src/ui/BatchRenameDialog.cpp
    src/ui/BatchRenameDialog.h
    src/ui/BatchRenamePreviewDialog.cpp
    src/ui/BatchRenamePreviewDialog.h
    src/ui/BreadcrumbBar.cpp
    src/ui/BreadcrumbBar.h
    src/ui/CategoryDelegate.h
    src/ui/CategoryLockDialog.cpp
    src/ui/CategoryLockDialog.h
    src/ui/CategoryModel.cpp
    src/ui/CategoryModel.h
    src/ui/CategoryPanel.cpp
    src/ui/CategoryPanel.h
    src/ui/CategorySetPasswordDialog.cpp
    src/ui/CategorySetPasswordDialog.h
    src/ui/ColorPicker.cpp
    src/ui/ColorPicker.h
    src/ui/ContentPanel.cpp
    src/ui/ContentPanel.h
    src/ui/DropJustifiedView.cpp
    src/ui/DropJustifiedView.h
    src/ui/DropListView.cpp
    src/ui/DropListView.h
    src/ui/DropTreeView.cpp
    src/ui/DropTreeView.h
    src/ui/FilterPanel.cpp
    src/ui/FilterPanel.h
    src/ui/FramelessDialog.cpp
    src/ui/FramelessDialog.h
    src/ui/JustifiedView.cpp
    src/ui/JustifiedView.h
    src/ui/ThumbnailDelegate.cpp
    src/ui/ThumbnailDelegate.h
    src/ui/LoadingWindow.cpp
    src/ui/LoadingWindow.h
    src/ui/Logger.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/TrayController.cpp
    src/ui/TrayController.h
    src/ui/HoverEventFilter.cpp
    src/ui/HoverEventFilter.h
    src/ui/ResizeEventFilter.cpp
    src/ui/ResizeEventFilter.h
    src/ui/AddressBar.cpp
    src/ui/AddressBar.h
    src/ui/MetaPanel.cpp
    src/ui/MetaPanel.h
    src/ui/NavPanel.cpp
    src/ui/NavPanel.h
    src/ui/ProgressDialog.h
    src/ui/QuickLookWindow.cpp
    src/ui/QuickLookWindow.h
    src/ui/RuleRow.cpp
    src/ui/RuleRow.h
    src/ui/ScanController.cpp
    src/ui/ScanController.h
    src/ui/ScanDialog.cpp
    src/ui/ScanDialog.h
    src/ui/SearchHistoryPanel.cpp
    src/ui/SearchHistoryPanel.h
    src/ui/ToolTipOverlay.cpp
    src/ui/ToolTipOverlay.h
    src/ui/TreeItemDelegate.h
    src/ui/UiHelper.h
    ArcMeta.rc
    resources/resources.qrc
)

# 可执行文件 (添加 WIN32 属性以消除控制台黑框)
# 修复：移除直接引用的 "ArcMeta.manifest"，改由 ArcMeta.rc 引入以杜绝 mt.exe 冲突
add_executable(ArcMeta WIN32 ${SOURCES})

# 链接库 (文档红线列表: ole32, bcrypt + QtSql)
# 2026-04-14 按照用户要求：恢复 MFT 逻辑，重新引入 ntdll 依赖
# 2026-05-10 修复：添加缺失的Qt6::Core和Qt6::Gui库
target_link_libraries(ArcMeta PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::Sql
    Qt6::Concurrent
    ntdll
    ole32
    bcrypt
)

# 2026-03-xx 按照用户要求及《记忆碎片》规范：
# 为避免 mt.exe 清单合并冲突，通过 ArcMeta.rc 嵌入清单并禁用链接器的默认清单生成
if(MSVC)
    target_link_options(ArcMeta PRIVATE "/MANIFEST:NO")
endif()
# Force Refresh CMake: 2026-06-xx 11:30 (Removed AmMetaJson & SyncQueue, switched to DB-only)
