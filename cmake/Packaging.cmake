# CPack packages for Linux (DEB) and macOS (DMG). Windows uses the WiX MSI built by
# scripts/package-windows.bat (installer/windows/deskflower.wxs).
# NOTE: the DEB and DMG configurations have not been built yet (see README).

set(CPACK_PACKAGE_NAME "deskflower")
set(CPACK_PACKAGE_VENDOR "DeskFlower")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Shared keyboard, mouse and clipboard across computers on the LAN")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "DeskFlower")

if(APPLE)
    # Drag-and-drop disk image with the app bundles (agent and session helper) and bin/.
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "DeskFlower")
elseif(UNIX)
    # Self-contained install in /opt/deskflower (Qt is deployed next to the executables).
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/deskflower")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    # Maintainer scripts get 0755 even when the sources come from a file system without the x bit.
    set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${PROJECT_SOURCE_DIR}/packaging/linux/postinst;${PROJECT_SOURCE_DIR}/packaging/linux/prerm")

    # Files outside the prefix: user service, menu entry and autostart of the agent (tray).
    install(FILES packaging/linux/deskflower.service DESTINATION /usr/lib/systemd/user)
    install(FILES packaging/linux/deskflower.desktop DESTINATION /usr/share/applications)
    install(FILES packaging/linux/deskflower-autostart.desktop DESTINATION /etc/xdg/autostart
            RENAME deskflower.desktop)
    # Icon theme entries named by the .desktop files (Icon=deskflower).
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES ${DESKFLOWER_ICONS_DIR}/png/deskflower-${size}.png
                DESTINATION /usr/share/icons/hicolor/${size}x${size}/apps RENAME deskflower.png)
    endforeach()
    install(FILES ${DESKFLOWER_ICONS_DIR}/deskflower.svg DESTINATION /usr/share/icons/hicolor/scalable/apps)
endif()

include(CPack)
