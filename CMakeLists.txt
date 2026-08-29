# TinyFramework — CMake build (CUSTOM_MINI self-written renderer mode)
cmake_minimum_required(VERSION 3.16)
project(TinyFramework C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ENABLE_CDP        "Embed the Chrome DevTools Protocol server" ON)
option(ENABLE_DIAG       "Embed the runtime diagnostics/profiler"     ON)
option(TINY_NATIVE       "Build against the OS WebView (NATIVE mode)" OFF)
option(MINI_JIT          "Build QuickJS from source with the baseline JIT (JS_JIT)" ON)
option(MINI_JIT_CODEGEN  "Enable SLJIT native codegen (implies MINI_JIT)" ON)
option(MINI_TLS          "Enable HTTPS via vendored mbedtls" ON)
option(GUI_APP           "Build as Windows GUI subsystem (-mwindows)" ON)

# 自定义 GLFW 安装路径（若不在系统路径中可直接通过 -DGLFW_ROOT=... 覆盖）
set(GLFW_ROOT "D:/glfw-3.4.bin.WIN64" CACHE PATH "Path to prebuilt GLFW SDK")

# ---- Global definitions & flags -----------------------------------
add_compile_definitions(_GNU_SOURCE)

if(TINY_NATIVE)
    set(MODE_FLAGS "")
else()
    set(MODE_FLAGS -DENABLE_CUSTOM_MINI_ENGINE)
endif()

# 编码与编译器特性配置
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---- vendored dependencies: QuickJS -------------------------------
set(QJS_SRC
        libs/quickjs/quickjs.c
        libs/quickjs/libregexp.c
        libs/quickjs/libunicode.c
        libs/quickjs/dtoa.c
)

add_library(quickjs_objs OBJECT ${QJS_SRC})
target_include_directories(quickjs_objs PRIVATE libs/quickjs)
target_compile_definitions(quickjs_objs PRIVATE
        _GNU_SOURCE
        CONFIG_VERSION="2024-01-13"
)

if(MINI_JIT)
    target_compile_definitions(quickjs_objs PRIVATE JS_JIT)
    if(MINI_JIT_CODEGEN)
        target_compile_definitions(quickjs_objs PRIVATE JS_JIT_CODEGEN)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/sljit/sljit_src")
            target_include_directories(quickjs_objs PRIVATE libs/sljit/sljit_src)
        endif()
    endif()
endif()

# ---- vendored dependencies: MbedTLS -------------------------------
# 绕过 mbedtls 自带 CMakeLists 缺失 submodule 的问题，直接基于源码打包静态库
if(MINI_TLS)
    file(GLOB MBEDTLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/mbedtls/library/*.c")
    if(MBEDTLS_SOURCES)
        add_library(mbedtls_static STATIC ${MBEDTLS_SOURCES})
        target_include_directories(mbedtls_static PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/libs/mbedtls/include
                ${CMAKE_CURRENT_SOURCE_DIR}/libs/mbedtls/library
        )
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(mbedtls_static PRIVATE -Os)
        endif()
    else()
        message(WARNING "MbedTLS source files not found in libs/mbedtls/library")
    endif()
endif()

# ---- Sources -------------------------------------------------------
set(CORE_SRC
        src/main.c
        src/mini_renderer.c
        src/mini_gradient.c
        src/mini_css.c
        src/mini_dom.c
        src/mini_events.c
        src/mini_js_bridge.c
        src/mini_vfs_decrypt.c
        src/mini_png.c
        src/mini_html5.c
        src/mini_net.c
        src/mini_log.c
        src/mini_crash.c
        src/mini_cookies.c
        src/mini_httpcache.c
        src/mini_policy.c
        src/mini_websocket.c
        src/mini_h2.c
        src/mini_h3.c
        src/mini_eventloop.c
        src/mini_devtools.c
        src/mini_webgl_ext.c
        src/mini_bidi.c
        src/mini_shaping.c
        src/mini_audio.c
        src/mini_native.c
        src/mini_worker.c
        src/mini_window.c
        src/mini_ipc.c
        src/mini_protocol.c
)

# Windows 图标/版本资源文件编译
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc")
    list(APPEND CORE_SRC src/app.rc)
endif()

if(ENABLE_CDP)
    list(APPEND CORE_SRC src/mini_cdp_server.c src/mini_cdp_domains.c)
endif()

if(ENABLE_DIAG)
    list(APPEND CORE_SRC src/mini_diag.c)
endif()

# ---- The App Executable -------------------------------------------
add_executable(tiny_app ${CORE_SRC} $<TARGET_OBJECTS:quickjs_objs>)
target_compile_definitions(tiny_app PRIVATE ${MODE_FLAGS})

# Include 路径配置
target_include_directories(tiny_app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/quickjs
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/stb
        $<$<BOOL:${MINI_TLS}>:${CMAKE_CURRENT_SOURCE_DIR}/libs/mbedtls/include>
        $<$<BOOL:${MINI_JIT_CODEGEN}>:${CMAKE_CURRENT_SOURCE_DIR}/libs/sljit/sljit_src>
)

# 附加 GLFW SDK 搜索路径
if(EXISTS "${GLFW_ROOT}/include")
    target_include_directories(tiny_app PRIVATE "${GLFW_ROOT}/include")
endif()
if(EXISTS "${GLFW_ROOT}/lib-mingw-w64")
    target_link_directories(tiny_app PRIVATE "${GLFW_ROOT}/lib-mingw-w64")
endif()

# ---- Link Libraries & Flags ---------------------------------------
# Windows GUI 模式
if(WIN32 AND GUI_APP)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(tiny_app PRIVATE -mwindows)
    elseif(MSVC)
        target_link_options(tiny_app PRIVATE /SUBSYSTEM:WINDOWS)
    endif()
endif()

# 基础图形及系统库
if(WIN32)
    target_link_libraries(tiny_app PRIVATE
            glfw3dll
            opengl32
            gdi32
            user32
            imm32
            ws2_32
            bcrypt
            dbghelp
            winmm
            crypt32
    )
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_libraries(tiny_app PRIVATE m)
    endif()
else()
    target_link_libraries(tiny_app PRIVATE glfw GL m pthread)
endif()

# MbedTLS 库链接
if(MINI_TLS)
    target_compile_definitions(tiny_app PRIVATE MINI_TLS)
    if(TARGET mbedtls_static)
        target_link_libraries(tiny_app PRIVATE mbedtls_static)
    endif()
endif()

# ---- Self-tests ---------------------------------------------------
add_executable(vfs_selftest src/mini_vfs_decrypt.c)
target_compile_definitions(vfs_selftest PRIVATE VFS_SELFTEST)
target_include_directories(vfs_selftest PRIVATE include)

add_executable(cdp_selftest src/mini_cdp_server.c)
target_compile_definitions(cdp_selftest PRIVATE CDP_SELFTEST)
target_include_directories(cdp_selftest PRIVATE include)
if(WIN32)
    target_link_libraries(cdp_selftest PRIVATE ws2_32)
endif()

enable_testing()
add_test(NAME vfs_selftest COMMAND vfs_selftest)
add_test(NAME cdp_selftest COMMAND cdp_selftest)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure)