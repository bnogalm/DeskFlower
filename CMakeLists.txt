cmake_minimum_required(VERSION 3.21)

project(Deskflower VERSION 0.1.20 LANGUAGES C CXX)
if(APPLE)
    # The macOS integration (pasteboard, drag and drop, screen capture) is Objective-C++.
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network Gui Widgets Qml Quick QuickControls2 Test)
# Certificate generation for the phone web server (Qt's TLS backend also uses OpenSSL).
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
qt_standard_project_setup(REQUIRES 6.8)
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

include(GNUInstallDirs)
# Read by the packaging scripts.
file(WRITE ${CMAKE_BINARY_DIR}/version.txt "${PROJECT_VERSION}")

# Ed25519 public key (64 hex characters) that update packages served by paired devices must be
# signed with (docs/DESIGN.md, "Signed updates"). Empty: updating from paired devices is disabled. When the
# environment variable of the same name is set (scripts\package-windows.bat does it from the signing
# key), it replaces the cached value.
set(DESKFLOWER_UPDATE_PUBKEY "" CACHE STRING "Hex Ed25519 public key for signed updates (empty: disabled)")
if(NOT "$ENV{DESKFLOWER_UPDATE_PUBKEY}" STREQUAL "")
    string(STRIP "$ENV{DESKFLOWER_UPDATE_PUBKEY}" _pubkey)
    set(DESKFLOWER_UPDATE_PUBKEY "${_pubkey}" CACHE STRING "Hex Ed25519 public key for signed updates (empty: disabled)" FORCE)
endif()
string(TOLOWER "${DESKFLOWER_UPDATE_PUBKEY}" DESKFLOWER_UPDATE_PUBKEY_HEX)
string(LENGTH "${DESKFLOWER_UPDATE_PUBKEY_HEX}" _pubkey_length)
if(NOT _pubkey_length EQUAL 0 AND (NOT _pubkey_length EQUAL 64 OR NOT DESKFLOWER_UPDATE_PUBKEY_HEX MATCHES "^[0-9a-f]+$"))
    message(FATAL_ERROR "DESKFLOWER_UPDATE_PUBKEY must be 64 hexadecimal characters (deskflower-sign keygen)")
endif()
if(_pubkey_length EQUAL 0)
    message(STATUS "Signed updates from paired devices: disabled (DESKFLOWER_UPDATE_PUBKEY is empty)")
else()
    message(STATUS "Signed updates from paired devices: key ${DESKFLOWER_UPDATE_PUBKEY_HEX}")
endif()

# Keyboard diagnostics (docs/DESIGN.md, "Keyboard and mouse"): a troubleshooting tool that records the keys
# going between devices. Off by default: no UI, the agent's and peers' requests are ignored and its
# code is not built. On: DESKFLOWER_KEY_DIAGNOSTICS is defined for every target.
option(DESKFLOWER_KEY_DIAGNOSTICS "Build the keyboard diagnostics (troubleshooting, records keys)" OFF)
if(DESKFLOWER_KEY_DIAGNOSTICS)
    add_compile_definitions(DESKFLOWER_KEY_DIAGNOSTICS)
    message(STATUS "Keyboard diagnostics: built")
endif()

include(cmake/Sodium.cmake)
include(cmake/AppIcon.cmake)

if(MSVC)
    # Third-party headers (<...>) at W0; C4702 (unreachable code) fires inside Qt's QJSEngine templates.
    add_compile_options(/W4 /permissive- /utf-8 /external:anglebrackets /external:W0 /wd4702)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Licensing (THIRD-PARTY-NOTICES.md): Deskflower is not GPL, so it may only ship Qt modules
# available under the LGPLv3. windeployqt also pulls in modules we do not use; the GPLv3-only ones
# (Virtual Keyboard, Lottie, Quick 3D, HTTP Server) must stay out, and so do the QML debugging
# plugins and Qt PDF (not needed). The check at the end of the installation enforces it.
# The options are windeployqt's; on Linux and macOS Qt's deployment only adds what is linked or
# imported, and the same check runs.
set(DESKFLOWER_DEPLOY_OPTIONS)
if(WIN32)
    # windeployqt only knows --no-<module> for the modules this Qt installation has (and rejects
    # the others): a module that is not installed cannot be deployed either, so its option is left
    # out. The module list comes from windeployqt --help.
    set(_df_excluded_modules virtualkeyboard lottie lottievectorimagegeneratorInternal
        lottievectorimagehelpers quick3dutils pdf)
    get_target_property(_df_windeployqt Qt6::windeployqt IMPORTED_LOCATION)
    if(NOT _df_windeployqt)
        get_filename_component(_df_qtbin "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" ABSOLUTE)
        set(_df_windeployqt "${_df_qtbin}/windeployqt.exe")
    endif()
    execute_process(COMMAND "${_df_windeployqt}" --help OUTPUT_VARIABLE _df_deploy_help
                    ERROR_VARIABLE _df_deploy_help RESULT_VARIABLE _df_deploy_rc)
    foreach(_df_module IN LISTS _df_excluded_modules)
        if(_df_deploy_help MATCHES "[ \n]${_df_module}[ \n]")
            list(APPEND DESKFLOWER_DEPLOY_OPTIONS --no-${_df_module})
        endif()
    endforeach()
    # The MSVC runtime DLLs are installed next to the programs (below), not vc_redist.x64.exe.
    list(APPEND DESKFLOWER_DEPLOY_OPTIONS --no-compiler-runtime --skip-plugin-types qmltooling
         --exclude-plugins qtvirtualkeyboardplugin,qlottievectorimage,qpdf)
    message(STATUS "windeployqt options: ${DESKFLOWER_DEPLOY_OPTIONS}")
endif()

if(MSVC)
    # Ship the MSVC runtime next to the executables (the service may run where it is not installed).
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
    include(InstallRequiredSystemLibraries)
    # CMake releases older than the compiler (e.g. CMake 3.30 with Visual Studio 18) do not find its
    # redistributable folder and install nothing: take the CRT DLLs from the newest
    # VC\Redist\MSVC\<version>\x64\Microsoft.VC*.CRT folder ourselves.
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS MATCHES "vcruntime140")
        file(GLOB _df_crt_dirs "$ENV{VCINSTALLDIR}Redist/MSVC/*/x64/Microsoft.VC*.CRT")
        list(SORT _df_crt_dirs COMPARE NATURAL ORDER DESCENDING)
        if(_df_crt_dirs)
            list(GET _df_crt_dirs 0 _df_crt_dir)
        endif()
        if(NOT _df_crt_dir)
            message(FATAL_ERROR "MSVC runtime DLLs not found (VCINSTALLDIR=$ENV{VCINSTALLDIR})")
        endif()
        file(GLOB _df_crt_dlls "${_df_crt_dir}/*.dll")
        message(STATUS "MSVC runtime from ${_df_crt_dir}")
        install(PROGRAMS ${_df_crt_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

add_subdirectory(third_party/qrcodegen)
add_subdirectory(src/core)
add_subdirectory(src/web)
add_subdirectory(src/view)
add_subdirectory(src/clipboard)
add_subdirectory(src/service)
add_subdirectory(src/session)
add_subdirectory(src/agent)
add_subdirectory(src/tools)

# License texts next to the programs (also shown in the agent's About window).
install(FILES LICENSE.md COMMERCIAL-LICENSE.md THIRD-PARTY-NOTICES.md DESTINATION .)
install(DIRECTORY licenses/ DESTINATION licenses)
if(APPLE)
    include(cmake/MacBundle.cmake) # deploys and signs DeskFlower.app (after its programs, before the check)
endif()
# Runs after the deployment of every program: no GPL-only Qt module may end up in the package.
install(CODE [[
    file(GLOB_RECURSE _gpl_only
        "${CMAKE_INSTALL_PREFIX}/*Qt6VirtualKeyboard*" "${CMAKE_INSTALL_PREFIX}/*Qt6Lottie*"
        "${CMAKE_INSTALL_PREFIX}/*Qt6Quick3D*" "${CMAKE_INSTALL_PREFIX}/*Qt6HttpServer*"
        "${CMAKE_INSTALL_PREFIX}/*qtvirtualkeyboardplugin*" "${CMAKE_INSTALL_PREFIX}/*qlottievectorimage*"
        "${CMAKE_INSTALL_PREFIX}/*QtVirtualKeyboard*" "${CMAKE_INSTALL_PREFIX}/*QtLottie*"
        "${CMAKE_INSTALL_PREFIX}/*QtQuick3D*" "${CMAKE_INSTALL_PREFIX}/*QtHttpServer*")
    if(_gpl_only)
        message(FATAL_ERROR "GPL-only Qt components were deployed (incompatible with Deskflower's license): ${_gpl_only}")
    endif()
]])

enable_testing()
add_subdirectory(tests)

include(cmake/Packaging.cmake)
