cmake_minimum_required(VERSION 3.16)
project(earthbound C CXX)
set(CMAKE_C_STANDARD 11)

# Emit compile_commands.json so clangd/IDEs resolve include paths and types.
# A root-level symlink (../../compile_commands.json) points at this file.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(EB_VIEWPORT_WIDTH 256 CACHE STRING "Viewport width in pixels")
set(EB_VIEWPORT_HEIGHT 224 CACHE STRING "Viewport height in pixels")

# The desktop build is warning-clean: warnings fail it (-DEB_WERROR=OFF to relax).
option(EB_WERROR "Treat compiler warnings as errors" ON)

set(EB_RUNTIME_VIEWPORT ON CACHE BOOL "Choose the picture size at run time")

# One save per file (save001.srm ...): 99 slots, each its own 8 KB SRAM image
set(EB_SAVE_COUNT 99 CACHE STRING "Number of save slots")

set(EB_FILE_BROWSER ON CACHE BOOL "Build the save backup/restore file browser")

# Input settings menu (remappable keys, gamepad choice, deadzone); needs the platform_input_* settings API
set(EB_INPUT_SETTINGS ON CACHE BOOL "Build the input settings menu")

# Smooth camera hooks (runtime option --smooth-camera / EB_SMOOTH_CAMERA=1)
set(EB_SMOOTH_CAMERA ON CACHE BOOL "Build the smooth-camera presentation hooks")

# Build game library from src/
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../src
                 ${CMAKE_CURRENT_BINARY_DIR}/game_lib)

# Find SDL2
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

# Platform executable. Use GLOB_RECURSE (not plain GLOB): a non-recursive
# `file(GLOB ... CONFIGURE_DEPENDS ...)` silently returns empty on some
# filesystems (notably the Docker Desktop virtiofs mount used by the i386 build
# in port/unix/docker), which drops every platform/*.c object and breaks the
# link. GLOB_RECURSE + CONFIGURE_DEPENDS works there and matches src/CMakeLists.txt.
# platform/ has no subdirectories, so this is equivalent to a flat glob.
file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c")
add_executable(earthbound-legacy main.c ${PLATFORM_SOURCES})

target_link_libraries(earthbound-legacy earthbound_game)

if(TARGET SDL2::SDL2)
    target_link_libraries(earthbound-legacy SDL2::SDL2)
else()
    target_include_directories(earthbound-legacy PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(earthbound-legacy ${SDL2_LIBRARIES})
    target_link_directories(earthbound-legacy PRIVATE ${SDL2_LIBRARY_DIRS})
endif()

# Platform code needs SDL2 headers and game library headers (inherited via PUBLIC)
target_include_directories(earthbound-legacy PRIVATE
    ${SDL2_INCLUDE_DIRS}
)

eb_target_warnings(earthbound-legacy)

# Sanitizer support
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

set(_SANITIZERS "")
if(ENABLE_ASAN)
    list(APPEND _SANITIZERS "address")
endif()
if(ENABLE_UBSAN)
    list(APPEND _SANITIZERS "undefined")
endif()

if(_SANITIZERS)
    list(JOIN _SANITIZERS "," _SANITIZER_FLAGS)
    target_compile_options(earthbound-legacy PRIVATE -fsanitize=${_SANITIZER_FLAGS} -fno-omit-frame-pointer)
    target_link_options(earthbound-legacy PRIVATE -fsanitize=${_SANITIZER_FLAGS})
endif()

# The SDL host above is the legacy one (earthbound-legacy); the Qt host is the default.
# ---- The default host (earthbound): Qt window, Vulkan / OpenGL / CPU drawing, Qt audio sink ----
# Same game library and the same SDL-backed input, files, saves and timer; only the window, the
# picture and the sound are Qt's (see port/qt/). EB_DRAWING=vulkan|opengl|cpu picks the drawing.
option(EB_QT_HOST "Build the Qt host (earthbound) when Qt6 is available" ON)
if(EB_QT_HOST)
    find_package(Qt6 QUIET COMPONENTS Widgets Multimedia ShaderTools GuiPrivate)
    if(Qt6_FOUND)
        set(QT_NO_PRIVATE_MODULE_WARNING ON)   # QRhi is semi-public since Qt 6.6
        set(CMAKE_AUTOMOC OFF)
        set(QT_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/../qt/qt_video.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../qt/qt_picture.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../qt/qt_audio.cpp)
        set(QT_PLATFORM_SOURCES ${PLATFORM_SOURCES})
        list(FILTER QT_PLATFORM_SOURCES EXCLUDE REGEX "sdl2_(video|audio)\\.c$")
        add_executable(earthbound main.c ${QT_PLATFORM_SOURCES} ${QT_SOURCES})
        qt_add_shaders(earthbound "eb_shaders" PREFIX "/eb/shaders"
                       BASE "${CMAKE_CURRENT_SOURCE_DIR}/../qt/shaders"
                       FILES "${CMAKE_CURRENT_SOURCE_DIR}/../qt/shaders/picture.vert"
                             "${CMAKE_CURRENT_SOURCE_DIR}/../qt/shaders/picture.frag")
        target_compile_definitions(earthbound PRIVATE EB_QT_HOST=1)
        target_include_directories(earthbound PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../qt ${SDL2_INCLUDE_DIRS})
        target_link_libraries(earthbound PRIVATE earthbound_game Qt6::Widgets Qt6::Multimedia Qt6::GuiPrivate)
        if(TARGET SDL2::SDL2)
            target_link_libraries(earthbound PRIVATE SDL2::SDL2)
        else()
            target_link_libraries(earthbound PRIVATE ${SDL2_LIBRARIES})
            target_link_directories(earthbound PRIVATE ${SDL2_LIBRARY_DIRS})
        endif()
        set_target_properties(earthbound PROPERTIES CXX_STANDARD 17)
    else()
        message(STATUS "Qt6 (Widgets, Multimedia, ShaderTools) not found: the SDL build is the only host")
    endif()
endif()
# Without the Qt host the SDL build is the game, under the plain name.
if(NOT TARGET earthbound)
    set_target_properties(earthbound-legacy PROPERTIES OUTPUT_NAME earthbound)
endif()
