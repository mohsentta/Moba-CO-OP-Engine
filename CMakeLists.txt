cmake_minimum_required(VERSION 3.25)

project(lolview
    VERSION 0.2.0
    DESCRIPTION "Standalone League of Legends model/effect viewer and sandbox"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LOLVIEW_FETCH_DEPS "Fetch zstd and zlib at configure time" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

# ---------------------------------------------------------------------------
# Payload decompression: zstd (most chunks) and zlib (gzip chunks).
# Both are permissive-licensed C libraries, fetched and built as static libs so
# the project has no system dependencies. -DLOLVIEW_FETCH_DEPS=OFF to use
# pre-installed ones via find_package.
# ---------------------------------------------------------------------------
if(LOLVIEW_FETCH_DEPS)
    include(FetchContent)

    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG        v1.5.6
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  build/cmake)
    FetchContent_MakeAvailable(zstd)

    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG        v1.3.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(zlib)

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.4.18
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(SDL3)

    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.8
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(imgui)
endif()

set(LOLVIEW_RENDER_DEPS "")
if(TARGET SDL3::SDL3)
    list(APPEND LOLVIEW_RENDER_DEPS SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    list(APPEND LOLVIEW_RENDER_DEPS SDL3::SDL3-static)
endif()
if(WIN32)
    list(APPEND LOLVIEW_RENDER_DEPS opengl32)
endif()

set(LOLVIEW_ZSTD_TARGET "")
foreach(candidate libzstd_static zstd_static zstd::libzstd_static zstd)
    if(TARGET ${candidate})
        set(LOLVIEW_ZSTD_TARGET ${candidate})
        break()
    endif()
endforeach()

set(LOLVIEW_ZLIB_TARGET "")
foreach(candidate zlibstatic zlib ZLIB::ZLIB)
    if(TARGET ${candidate})
        set(LOLVIEW_ZLIB_TARGET ${candidate})
        break()
    endif()
endforeach()

if(NOT LOLVIEW_ZSTD_TARGET OR NOT LOLVIEW_ZLIB_TARGET)
    message(FATAL_ERROR
        "zstd/zlib targets not found. Configure with -DLOLVIEW_FETCH_DEPS=ON or "
        "install them and provide the targets.")
endif()

message(STATUS "lolview: zstd -> ${LOLVIEW_ZSTD_TARGET}, zlib -> ${LOLVIEW_ZLIB_TARGET}")

# ---------------------------------------------------------------------------
# Core: format parsing, decompression and the asset-source guardrail. No
# graphics dependencies, so it builds and tests anywhere a compiler exists.
# ---------------------------------------------------------------------------
add_library(lolview_core STATIC
    src/net/net.cpp
    src/core/walkable.cpp
    src/core/asset_source.cpp
    src/core/xxhash64.cpp
    src/formats/bcdec_impl.cpp
    src/formats/bin.cpp
    src/formats/dds.cpp
    src/formats/decompress.cpp
    src/formats/ground.cpp
    src/formats/mapgeo.cpp
    src/formats/anm.cpp
    src/formats/anm_compressed.cpp
    src/formats/pose.cpp
    src/formats/skl.cpp
    src/formats/skn.cpp
    src/formats/tex.cpp
    src/formats/vfx.cpp
    src/formats/vfx_def.cpp
    src/formats/wad.cpp)

target_include_directories(lolview_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if (WIN32)
    target_link_libraries(lolview_core PUBLIC ws2_32)
endif()
target_include_directories(lolview_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(lolview_core PUBLIC LOLVIEW_VERSION="${PROJECT_VERSION}")
target_link_libraries(lolview_core PUBLIC ${LOLVIEW_ZSTD_TARGET} ${LOLVIEW_ZLIB_TARGET})

add_executable(lolview src/main.cpp)
target_link_libraries(lolview PRIVATE lolview_core)

# ---------------------------------------------------------------------------
# Renderer: SDL3 window/context + OpenGL 4.6 core, offscreen PNG output today
# and the interactive viewer later.
# ---------------------------------------------------------------------------
option(LOLVIEW_BUILD_VIEWER "Build the SDL3 + OpenGL renderer" ON)

if(LOLVIEW_BUILD_VIEWER)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC ${LOLVIEW_RENDER_DEPS})

    add_library(lolview_render STATIC
        src/render/gl.cpp
        src/render/renderer.cpp
        src/render/stb_write.cpp
        src/render/texture_upload.cpp)
    target_include_directories(lolview_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(lolview_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_link_libraries(lolview_render PUBLIC lolview_core PRIVATE ${LOLVIEW_RENDER_DEPS})
    target_link_libraries(lolview PRIVATE lolview_render)
    target_compile_definitions(lolview PRIVATE LOLVIEW_HAS_VIEWER=1)

    add_executable(lolview_sandbox src/app/sandbox.cpp)
    target_link_libraries(lolview_sandbox PRIVATE lolview_render imgui ${LOLVIEW_RENDER_DEPS})
    target_compile_definitions(lolview_sandbox PRIVATE LOLVIEW_VERSION="${PROJECT_VERSION}")
endif()

enable_testing()
add_executable(lolview_tests tests/test_main.cpp)
target_link_libraries(lolview_tests PRIVATE lolview_core)
add_test(NAME core_tests COMMAND lolview_tests)

add_executable(mapgeo_probe src/tools/mapgeo_probe.cpp)
target_link_libraries(mapgeo_probe PRIVATE lolview_core)

# ---- game host ------------------------------------------------------------
# The server owns the world; the game mode is a Lua script. Lua is fetched so it
# is not vendored into the tree.
option(LOLVIEW_LUA "Build the Lua-driven game host" ON)
if (LOLVIEW_LUA)
    include(FetchContent)
    set(LUA_URL "https://www.lua.org/ftp/lua-5.4.7.tar.gz")
    FetchContent_Declare(lua URL ${LUA_URL} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(lua)

    file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/src/*.c)
    list(REMOVE_ITEM LUA_SOURCES
        ${lua_SOURCE_DIR}/src/lua.c
        ${lua_SOURCE_DIR}/src/luac.c
        ${lua_SOURCE_DIR}/src/onelua.c)
    add_library(lua_static STATIC ${LUA_SOURCES})
    target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR}/src)
    target_compile_definitions(lua_static PUBLIC LUA_USE_WINDOWS)
    if (WIN32)
        target_link_libraries(lua_static PUBLIC ws2_32)
    endif()

    add_executable(lolview_server src/server/main.cpp)
    target_link_libraries(lolview_server PRIVATE lolview_core lua_static)
endif()
