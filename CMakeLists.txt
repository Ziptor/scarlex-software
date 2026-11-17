cmake_minimum_required(VERSION 3.20)
project(rob_run_tool LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(SCARLEX_DIAGNOSTICS "Build developer diagnostics and verbose logging" OFF)
option(SCARLEX_PUBLIC_RELEASE "Build the single-file server-backed launcher" OFF)
if (SCARLEX_PUBLIC_RELEASE AND SCARLEX_DIAGNOSTICS)
    message(FATAL_ERROR "Public releases must have SCARLEX_DIAGNOSTICS=OFF")
endif()
add_compile_definitions(SCARLEX_DIAGNOSTICS=$<BOOL:${SCARLEX_DIAGNOSTICS}>
    SCARLEX_PUBLIC_RELEASE=$<BOOL:${SCARLEX_PUBLIC_RELEASE}>)
set(SCARLEX_VERSION "1.2.0" CACHE STRING "Scarlex release version")
set(SCARLEX_LOADER_VERSION "${SCARLEX_VERSION}" CACHE STRING "Scarlex launcher version")
if (NOT SCARLEX_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "SCARLEX_VERSION must be X.Y.Z")
endif()
if (NOT SCARLEX_LOADER_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "SCARLEX_LOADER_VERSION must be X.Y.Z")
endif()
string(REPLACE "." "," SCARLEX_VERSION_RC "${SCARLEX_VERSION}")
configure_file(licensing/payload_version.rc.in payload_version.rc @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mesh_overlay.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mesh_overlay.hlsl" MESH_OVERLAY_HLSL)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mesh_overlay_shader.h"
    "#pragma once\ninline constexpr char mesh_overlay_shader[] = R\"HLSL(${MESH_OVERLAY_HLSL})HLSL\";\n")

# Ищем папку ImGui (на случай, если она всё ещё imgui-1.91.9b)
file(GLOB IMGUI_DIRS "third_party/imgui*")
list(GET IMGUI_DIRS 0 IMGUI_DIR)

# Собираем все cpp файлы из корня и папки src
file(GLOB ROOT_SOURCES CONFIGURE_DEPENDS "*.cpp")
file(GLOB SRC_SOURCES "src/*.cpp")

add_executable(rob_run_tool 
    ${ROOT_SOURCES} 
    ${SRC_SOURCES} 
    menu_assets.rc
    "${CMAKE_CURRENT_BINARY_DIR}/payload_version.rc"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)
if (WIN32 AND NOT SCARLEX_DIAGNOSTICS)
    set_target_properties(rob_run_tool PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

add_custom_command(TARGET rob_run_tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/icons" "$<TARGET_FILE_DIR:rob_run_tool>/icons"
    COMMENT "Copying weapon icons beside the executable"
)

target_include_directories(rob_run_tool PRIVATE 
    . 
    include
    src
    diagnostics/include
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    third_party/json
)

target_link_libraries(rob_run_tool PRIVATE
    d3d11 dxgi dcomp dwmapi user32 gdi32 winmm d3dcompiler windowscodecs ole32
    winhttp bcrypt crypt32 iphlpapi
)

add_library(scarlex_ed25519 STATIC
    third_party/monocypher/monocypher.c
    third_party/monocypher/monocypher-ed25519.c
)
target_include_directories(scarlex_ed25519 PUBLIC third_party/monocypher)
target_link_libraries(rob_run_tool PRIVATE scarlex_ed25519)
target_compile_definitions(rob_run_tool PRIVATE SCARLEX_VERSION="${SCARLEX_VERSION}")
target_sources(rob_run_tool PRIVATE
    licensing/ticket_validator.cpp
    loader/game_watcher.cpp
    loader/hardware_id.cpp
    loader/http_client.cpp
    loader/logger.cpp
)

if (MSVC)
    target_compile_options(rob_run_tool PRIVATE /W4 /permissive- /utf-8)
endif()

if (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(rob_run_visibility SHARED visibility/NativePhysics.cpp)
    target_include_directories(rob_run_visibility PRIVATE diagnostics/include)
    target_compile_options(rob_run_visibility PRIVATE /W4 /permissive- /utf-8)
endif()

if (MSVC)
    include(FetchContent)
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(scarlex_webview2
        URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/microsoft.web.webview2.1.0.4314.35.nupkg
        URL_HASH SHA256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4
        DOWNLOAD_NAME Microsoft.Web.WebView2.zip
    )
    FetchContent_MakeAvailable(scarlex_webview2)
    set(webview2_native "${scarlex_webview2_SOURCE_DIR}/build/native")
    if (CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$")
        set(webview2_arch arm64)
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(webview2_arch x64)
    else()
        set(webview2_arch x86)
    endif()
    add_executable(scarlex_loader WIN32
        loader/main_loader.cpp
        loader/loader_ui.cpp
        loader/webview/pc_status.cpp
        loader/game_watcher.cpp
        loader/logger.cpp
        loader/diagnostic_sender.cpp
        loader/security_check.cpp
        loader/hardware_id.cpp
        loader/http_client.cpp
        loader/update_checker.cpp
        loader/payload_downloader.cpp
        loader/runtime_downloader.cpp
    )
    string(REPLACE "." "," SCARLEX_LOADER_VERSION_RC "${SCARLEX_LOADER_VERSION}")
    configure_file(loader/loader_version.rc.in loader_version.rc @ONLY)
    target_sources(scarlex_loader PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/loader_version.rc")
    target_include_directories(scarlex_loader PRIVATE . third_party/json "${webview2_native}/include")
    target_compile_definitions(scarlex_loader PRIVATE SCARLEX_VERSION="${SCARLEX_VERSION}" SCARLEX_LOADER_VERSION="${SCARLEX_LOADER_VERSION}" UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(scarlex_loader PRIVATE /W4 /permissive- /utf-8)
    if (SCARLEX_PUBLIC_RELEASE)
        target_link_options(scarlex_loader PRIVATE "/MANIFESTUAC:level='requireAdministrator'")
    endif()
    target_link_libraries(scarlex_loader PRIVATE scarlex_ed25519 winhttp bcrypt crypt32 version iphlpapi advapi32 psapi
        ole32 oleaut32 user32 gdi32 dwmapi shlwapi wbemuuid
        "${webview2_native}/${webview2_arch}/WebView2LoaderStatic.lib")
    if (NOT SCARLEX_PUBLIC_RELEASE)
      add_custom_target(scarlex_ui
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:scarlex_loader>/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/menu_logo.png" "$<TARGET_FILE_DIR:scarlex_loader>/ui/logo.png"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/menu_inter.ttf" "$<TARGET_FILE_DIR:scarlex_loader>/ui/inter.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/Inter-OFL.txt" "$<TARGET_FILE_DIR:scarlex_loader>/ui/Inter-OFL.txt"
        VERBATIM
    )
      add_dependencies(scarlex_loader scarlex_ui)
    endif()
endif()
