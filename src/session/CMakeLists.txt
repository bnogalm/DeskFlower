qt_add_executable(deskflower-session WIN32 MACOSX_BUNDLE main.cpp)

# What the helper is made of; on Linux in a library (deskflower_session_linux, below) that the
# tests drive too.
set(_session_sources
    inputbackend.h stubinputbackend.h
    localscreens.h localscreens.cpp
    sessionclient.h sessionclient.cpp
    screengrabber.h
    screenstreamer.h screenstreamer.cpp
    viewerwindow.h viewerwindow.cpp)
if(NOT (UNIX AND NOT APPLE))
    target_sources(deskflower-session PRIVATE ${_session_sources})
endif()

if(WIN32)
    target_sources(deskflower-session PRIVATE inputbackend_win.cpp screengrabber_win.cpp)
    target_link_libraries(deskflower-session PRIVATE user32 advapi32 d3d11 dxgi)
elseif(APPLE)
    # NOTE: not built yet on macOS (see docs/DESIGN.md).
    target_sources(deskflower-session PRIVATE inputbackend_mac.cpp screengrabber_mac.mm)
    set_source_files_properties(screengrabber_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # ScreenCaptureKit is weakly linked: macOS before 12.3 falls back to QScreen::grabWindow.
    # Carbon: IsSecureEventInputEnabled.
    target_link_libraries(deskflower-session PRIVATE
        "-framework ApplicationServices" "-framework CoreFoundation" "-framework CoreGraphics"
        "-framework Carbon" "-framework Foundation" "-framework CoreMedia" "-framework CoreVideo"
        "-weak_framework ScreenCaptureKit")
    # Background helper: no Dock icon; a stable identifier for the privacy permissions.
    set_target_properties(deskflower-session PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER com.deskflower.session
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/cmake/session-Info.plist.in)
elseif(UNIX)
    find_package(X11 REQUIRED)
    if(NOT X11_Xi_FOUND OR NOT X11_Xtst_FOUND OR NOT X11_Xrandr_FOUND OR NOT X11_Xfixes_FOUND OR NOT X11_XShm_FOUND)
        message(FATAL_ERROR "X11 support needs libXi, libXtst, libXrandr, libXfixes and libXext "
                            "(apt: libxi-dev libxtst-dev libxrandr-dev libxfixes-dev libxext-dev)")
    endif()
    # The Linux backends, in a library of their own so that the tests can drive them
    # (tests/tst_x11.cpp under Xvfb, tests/tst_portal.cpp against a fake portal).
    qt_add_library(deskflower_session_linux STATIC
        ${_session_sources}
        linuxbackends.h inputbackend_linux.cpp inputbackend_x11.cpp screengrabber_linux.cpp)
    target_include_directories(deskflower_session_linux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(deskflower_session_linux
        PUBLIC deskflower_core deskflower_view deskflower_clipboard Qt6::Gui
        PRIVATE X11::X11 X11::Xi X11::Xtst X11::Xrandr X11::Xfixes X11::Xext)

    # Wayland: the xdg-desktop-portal remote desktop session over Qt D-Bus (clipboard, screens,
    # keyboard and mouse), libei for the input itself and PipeWire for the screens.
    find_package(Qt6 COMPONENTS DBus)
    find_package(PkgConfig REQUIRED)
    if(TARGET Qt6::DBus)
        target_sources(deskflower_session_linux PRIVATE
            portalsession.h portalsession.cpp portalclipboard.h portalclipboard.cpp)
        target_compile_definitions(deskflower_session_linux PUBLIC DESKFLOWER_HAVE_PORTAL)
        target_link_libraries(deskflower_session_linux PUBLIC Qt6::DBus)

        pkg_check_modules(LIBEI IMPORTED_TARGET libei-1.0>=1.0)
        if(LIBEI_FOUND)
            target_sources(deskflower_session_linux PRIVATE inputbackend_wayland.cpp)
            target_compile_definitions(deskflower_session_linux PUBLIC DESKFLOWER_HAVE_LIBEI)
            target_link_libraries(deskflower_session_linux PRIVATE PkgConfig::LIBEI)
        else()
            message(WARNING "libei not found (apt: libei-dev): no keyboard/mouse sharing on Wayland")
        endif()

        pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
        if(PIPEWIRE_FOUND)
            target_sources(deskflower_session_linux PRIVATE pipewirecapture.h pipewirecapture.cpp)
            target_compile_definitions(deskflower_session_linux PUBLIC DESKFLOWER_HAVE_PIPEWIRE)
            target_link_libraries(deskflower_session_linux PRIVATE PkgConfig::PIPEWIRE)
        else()
            message(WARNING "PipeWire not found (apt: libpipewire-0.3-dev): no remote view or thumbnails on Wayland")
        endif()
    else()
        message(WARNING "Qt D-Bus not found: no clipboard, screens or keyboard/mouse sharing on Wayland")
    endif()

    target_link_libraries(deskflower-session PRIVATE deskflower_session_linux X11::X11)
else()
    target_sources(deskflower-session PRIVATE inputbackend_stub.cpp screengrabber_other.cpp)
endif()

target_link_libraries(deskflower-session PRIVATE deskflower_core deskflower_view deskflower_clipboard Qt6::Gui)
deskflower_add_app_icon(deskflower-session)

# Self-contained installation: cmake --install <build> --prefix <dir> (Qt libraries via windeployqt).
# macOS: a bundle of its own (privacy permissions are granted per bundle) inside DeskFlower.app
# (deployed with it, cmake/MacBundle.cmake).
if(APPLE)
    install(TARGETS deskflower-session BUNDLE DESTINATION DeskFlower.app/Contents/Helpers)
else()
    install(TARGETS deskflower-session
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    qt_generate_deploy_app_script(
        TARGET deskflower-session
        OUTPUT_SCRIPT deploy_script
        DEPLOY_TOOL_OPTIONS ${DESKFLOWER_DEPLOY_OPTIONS}
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${deploy_script})
endif()
