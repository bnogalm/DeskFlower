qt_add_executable(deskflower-session WIN32 MACOSX_BUNDLE
    main.cpp
    inputbackend.h
    localscreens.h localscreens.cpp
    sessionclient.h sessionclient.cpp
    screengrabber.h
    screenstreamer.h screenstreamer.cpp
    viewerwindow.h viewerwindow.cpp
)

target_sources(deskflower-session PRIVATE stubinputbackend.h)

if(WIN32)
    target_sources(deskflower-session PRIVATE inputbackend_win.cpp screengrabber_win.cpp)
    target_link_libraries(deskflower-session PRIVATE user32 d3d11 dxgi)
elseif(APPLE)
    # NOTE: not built yet on macOS (see README).
    target_sources(deskflower-session PRIVATE inputbackend_mac.cpp screengrabber_other.cpp)
    target_link_libraries(deskflower-session PRIVATE "-framework ApplicationServices" "-framework CoreFoundation")
    # Background helper: no Dock icon; a stable identifier for the privacy permissions.
    set_target_properties(deskflower-session PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER com.deskflower.session
        MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/cmake/session-Info.plist.in)
elseif(UNIX)
    # NOTE: not built yet on Linux (see README).
    find_package(X11 REQUIRED)
    if(NOT X11_Xi_FOUND OR NOT X11_Xtst_FOUND OR NOT X11_Xrandr_FOUND)
        message(FATAL_ERROR "X11 input needs libXi, libXtst and libXrandr (apt: libxi-dev libxtst-dev libxrandr-dev)")
    endif()
    target_sources(deskflower-session PRIVATE linuxbackends.h inputbackend_linux.cpp inputbackend_x11.cpp
                                              screengrabber_other.cpp)
    target_link_libraries(deskflower-session PRIVATE X11::X11 X11::Xi X11::Xtst X11::Xrandr)

    # Wayland: portals (D-Bus) + libei/liboeffis (apt: libei-dev liboeffis-dev).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBEI IMPORTED_TARGET libei-1.0>=1.0 liboeffis-1.0>=1.0)
    find_package(Qt6 COMPONENTS DBus)
    if(LIBEI_FOUND AND TARGET Qt6::DBus)
        target_sources(deskflower-session PRIVATE inputbackend_wayland.cpp)
        target_compile_definitions(deskflower-session PRIVATE DESKFLOWER_HAVE_LIBEI)
        target_link_libraries(deskflower-session PRIVATE PkgConfig::LIBEI Qt6::DBus)
    else()
        message(WARNING "libei/liboeffis or Qt D-Bus not found: no keyboard/mouse sharing on Wayland")
    endif()
else()
    target_sources(deskflower-session PRIVATE inputbackend_stub.cpp screengrabber_other.cpp)
endif()

target_link_libraries(deskflower-session PRIVATE deskflower_core deskflower_view deskflower_clipboard Qt6::Gui)
deskflower_add_app_icon(deskflower-session)

# Self-contained installation: cmake --install <build> --prefix <dir> (Qt libraries via windeployqt).
install(TARGETS deskflower-session
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_app_script(
    TARGET deskflower-session
    OUTPUT_SCRIPT deploy_script
    DEPLOY_TOOL_OPTIONS ${DESKFLOWER_DEPLOY_OPTIONS}
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
