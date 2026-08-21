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

# ---- mode flag -----------------------------------------------------
if(TINY_NATIVE)
    set(MODE_FLAGS "")
else()
    set(MODE_FLAGS -DENABLE_CUSTOM_MINI_ENGINE)
endif()

# ---- hardening / size flags ---------------------------------------
add_compile_options(
    $<$<C_COMPILER_ID:GNU,Clang>:-Os -flto -ffunction-sections -fdata-sections
                                  -fno-asynchronous-unwind-tables -fno-ident>)
add_link_options($<$<C_COMPILER_ID:GNU,Clang>:-flto -Wl,--gc-sections,--as-needed,--build-id=none>)
add_link_options($<$<C_COMPILER_ID:MSVC>:/OPT:REF /OPT:ICF /LTCG>)

# ---- vendored dependencies: mbedtls --------------------------------
if(MINI_TLS)
    # 禁用 mbedtls 自带的测试和可执行文件，极大加快编译速度
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/mbedtls/CMakeLists.txt")
        add_subdirectory(libs/mbedtls EXCLUDE_FROM_ALL)
    endif()
endif()

# ---- vendored dependencies: quickjs & sljit -----------------------
if(MINI_JIT)
    set(JS_JIT ON CACHE BOOL "Enable QuickJS baseline JIT" FORCE)
    if(MINI_JIT_CODEGEN)
        set(JS_JIT_CODEGEN ON CACHE BOOL "Enable SLJIT codegen" FORCE)
        # 提供 sljit 头文件搜索路径供 quickjs 及主工程引用
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/sljit")
            include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libs/sljit/sljit_src)
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/quickjs/CMakeLists.txt")
        add_subdirectory(libs/quickjs EXCLUDE_FROM_ALL)
    endif()
endif()

# ---- sources -------------------------------------------------------
set(CORE_SRC
    src/main.c
    src/mini_renderer.c
    src/mini_dom.c
    src/mini_html5.c
    src/mini_css.c
    src/mini_bidi.c
    src/mini_shaping.c
    src/mini_eventloop.c
    src/mini_js_bridge.c
    src/mini_vfs_decrypt.c
    src/mini_net.c
    src/mini_png.c
    src/mini_events.c
    src/mini_gradient.c
    src/mini_log.c
    src/mini_crash.c
    src/mini_cookies.c
    src/mini_httpcache.c
    src/mini_policy.c
    src/mini_websocket.c
    src/mini_h2.c
    src/mini_h3.c
    src/mini_devtools.c)

if(ENABLE_CDP)  list(APPEND CORE_SRC src/mini_cdp_server.c src/mini_cdp_domains.c) endif()
if(ENABLE_DIAG) list(APPEND CORE_SRC src/mini_diag.c)       endif()

# ---- the app -------------------------------------------------------
add_executable(tiny_app ${CORE_SRC})
target_compile_definitions(tiny_app PRIVATE ${MODE_FLAGS})

# 包含目录统一配置
target_include_directories(tiny_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    $<$<BOOL:${MINI_JIT}>:${CMAKE_CURRENT_SOURCE_DIR}/libs/quickjs>
    $<$<BOOL:${MINI_JIT_CODEGEN}>:${CMAKE_CURRENT_SOURCE_DIR}/libs/sljit/sljit_src>
    $<$<BOOL:${MINI_TLS}>:${CMAKE_CURRENT_SOURCE_DIR}/libs/mbedtls/include>
)

# 基础图形与平台库链接
target_link_libraries(tiny_app PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:glfw GL m>
    $<$<C_COMPILER_ID:MSVC>:glfw3 opengl32 user32 gdi32>
)

if(WIN32)
    target_link_libraries(tiny_app PRIVATE imm32 ws2_32 crypt32)
endif()

# QuickJS 链接处理
if(MINI_JIT)
    target_compile_definitions(tiny_app PRIVATE JS_JIT)
    if(TARGET qjs)
        target_link_libraries(tiny_app PRIVATE qjs)
    elseif(TARGET quickjs)
        target_link_libraries(tiny_app PRIVATE quickjs)
    endif()
else()
    target_link_libraries(tiny_app PRIVATE quickjs)
endif()

# MbedTLS 链接处理
if(MINI_TLS)
    target_compile_definitions(tiny_app PRIVATE MINI_TLS)
    # 优先匹配 TARGET 别名，确保构建兼容
    if(TARGET mbedtls_static)
        target_link_libraries(tiny_app PRIVATE mbedtls_static mbedx509_static mbedcrypto_static)
    elseif(TARGET mbedtls)
        target_link_libraries(tiny_app PRIVATE mbedtls mbedx509 mbedcrypto)
    endif()
endif()

# ---- self-tests ---------------------------------------------------
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