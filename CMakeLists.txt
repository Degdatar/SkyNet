
cmake_minimum_required(VERSION 3.21)

project(
    SkyNet
    VERSION 1.0.1
    LANGUAGES CXX
)

# ============================================================================
# C++ Configuration
# ============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable LTCG for Release builds
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Use the static MSVC runtime (/MT)
set(
    CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ============================================================================
# OpenSSL
# ============================================================================

find_package(OpenSSL REQUIRED)

# ============================================================================
# CommonLibSSE-NG
# ============================================================================

# Disable CommonLib tests
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
# SkyNet currently ships and is tested as a flat Skyrim SE/AE plugin.  Keep
# CommonLib's VR target off so a normal clone does not require OpenVR headers.
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)

add_subdirectory(extern/CommonLibSSE-NG)

# CommonLibSSE exports its Windows 7 compatibility definitions to consumers.
# httplib 0.57 requires Windows 10, so prevent only those two inherited macros
# from overriding SkyNet's Windows 10 target below.
get_target_property(_commonlibsse_interface_definitions
    CommonLibSSE INTERFACE_COMPILE_DEFINITIONS)
list(REMOVE_ITEM _commonlibsse_interface_definitions
    "WINVER=0x0601"
    "_WIN32_WINNT=0x0601")
set_property(TARGET CommonLibSSE PROPERTY INTERFACE_COMPILE_DEFINITIONS
    "${_commonlibsse_interface_definitions}")

# ============================================================================
# SkyNet Plugin
# ============================================================================

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/webview_browser.cpp
)

# ============================================================================
# Include Directories
# ============================================================================

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/WebView2/build/native/include
    ${OPENSSL_INCLUDE_DIR}
)

# ============================================================================
# Link Libraries
# ============================================================================

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    crypt32
    ole32
    oleaut32
    shlwapi
    gdiplus
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/WebView2/build/native/x64/WebView2LoaderStatic.lib"
)

# ============================================================================
# Compile Definitions
#
# cpp-httplib 0.57 and later require a Windows 10 API target.
# This project uses Windows 10+ APIs exposed by the current Windows SDK.
# ============================================================================

target_compile_definitions(${PROJECT_NAME} PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT

    UNICODE
    _UNICODE

    WIN32_LEAN_AND_MEAN
    NOMINMAX

    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

# ============================================================================
# MSVC Compiler Options
# ============================================================================

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /utf-8
        /wd4127
    )
endif()

# ============================================================================
# Output
# ============================================================================

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "SkyNetBrowser"
    SUFFIX ".dll"
)

# Create a ready-to-install Data folder next to the built DLL.  The browser
# assets must be deployed with the plugin; PrismaUI resolves the view below
# Data/PrismaUI/views/SkyNet/.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/PrismaUI/views/SkyNet"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/PrismaUI/Mods/SkyNet"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/PrismaUI/views/SkyNet"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/SkyNetLogo.jpg"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/SkyNetBrowserRuntimeInstaller.exe"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/MicrosoftEdgeWebView2RuntimeInstallerX64.exe"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/SkyNetBrowserSetup.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/SkyNetLogo.png"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/SkyNetLogo.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/Splash.html"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/Splash.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/Splash.mp4"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/Splash.mp4"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/Home.html"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/Home.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/About.html"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/About.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/CHANGELOG.md"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/Extensions"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/Extensions"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Data/SKSE/Plugins/SkyNet/Extensions"
)
