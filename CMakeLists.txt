cmake_minimum_required(VERSION 3.20)
project(FERREX-META)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 改进：引入自动寻找机制，优先使用 QT_DIR 环境变量，否则尝试动态获取或回退到默认路径
if(DEFINED ENV{QT_DIR})
    set(CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
else()
    find_program(QT_QMAKE_EXECUTABLE NAMES qmake)
    if(QT_QMAKE_EXECUTABLE)
        get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
        get_filename_component(CMAKE_PREFIX_PATH "${QT_BIN_DIR}" DIRECTORY)
    else()
        set(CMAKE_PREFIX_PATH "C:/Qt/6.10.2/msvc2022_64")
    endif()
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

# 路径严谨性核查：兼容 VS 默认的 out/build 目录，仅在检测到源内构建（In-source build）时报错
if("${CMAKE_BINARY_DIR}" STREQUAL "${PROJECT_SOURCE_DIR}")
    message(FATAL_ERROR "⚠️ 禁止源内构建！请使用独立的构建目录（如 out/build/）以符合《源码纯净》规范。")
endif()

# 2026-03-xx 按照用户要求：将可执行文件直接输出到指定项目子目录 FERREX-META 下，解决 \out 文件夹查找不便的问题
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/FERREX-META")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/FERREX-META")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/FERREX-META")

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
    src/ui/FramelessDialog.cpp
    src/ui/FramelessDialog.h
    src/ui/JustifiedView.cpp
    src/ui/JustifiedView.h
    src/ui/ThumbnailDelegate.cpp
    src/ui/ThumbnailDelegate.h
    src/ui/TrayController.cpp
    src/ui/TrayController.h
    src/ui/ScanController.cpp
    src/ui/ScanController.h
    src/ui/ScanDialog.cpp
    src/ui/ScanDialog.h
    src/ui/UiHelper.h
    FERREX-META.rc
    resources/resources.qrc
)

# 可执行文件 (添加 WIN32 属性以消除控制台黑框)
# 修复：移除直接引用的 "FERREX-META.manifest"，改由 FERREX-META.rc 引入以杜绝 mt.exe 冲突
add_executable(FERREX-META WIN32 ${SOURCES})

# 链接库 (文档红线列表: ole32, bcrypt + QtSql)
# 2026-04-14 按照用户要求：恢复 MFT 逻辑，重新引入 ntdll 依赖
# 2026-05-10 修复：添加缺失的Qt6::Core和Qt6::Gui库
target_link_libraries(FERREX-META PRIVATE
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
# 为避免 mt.exe 清单合并冲突，通过 FERREX-META.rc 嵌入清单并禁用链接器的默认清单生成
if(MSVC)
    target_link_options(FERREX-META PRIVATE "/MANIFEST:NO")
endif()
# Force Refresh CMake: 2026-06-xx 11:30 (Removed AmMetaJson & SyncQueue, switched to DB-only)
