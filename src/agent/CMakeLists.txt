qt_add_executable(deskflower-agent WIN32 MACOSX_BUNDLE
    main.cpp
    backend.h backend.cpp
    thumbnailprovider.h
    qrimageprovider.h qrimageprovider.cpp
)

qt_add_qml_module(deskflower-agent
    URI Deskflower
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/DevicesPage.qml
        qml/ScreensPage.qml
        qml/FilesPage.qml
        qml/PeerRow.qml
        qml/PinDisplayDialog.qml
        qml/PinEntryDialog.qml
        qml/ViewRequestDialog.qml
        qml/AboutDialog.qml
        qml/AdvancedSettingsWindow.qml
        qml/Bouquet.qml
)

# License texts for the About window (the same files are installed next to the programs).
file(GLOB _license_texts CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/licenses/*)
qt_add_resources(deskflower-agent "licenses"
    PREFIX "/deskflower/licenses"
    BASE ${PROJECT_SOURCE_DIR}
    FILES
        ${PROJECT_SOURCE_DIR}/LICENSE.md
        ${PROJECT_SOURCE_DIR}/COMMERCIAL-LICENSE.md
        ${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md
        ${_license_texts})

# Window and tray icon; the small sizes come from the simplified drawing.
qt_add_resources(deskflower-agent "app_icon"
    PREFIX "/icons"
    BASE ${DESKFLOWER_ICONS_DIR}/png
    FILES
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-16.png
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-20.png
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-24.png
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-32.png
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-40.png
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-48.png
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-64.png
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-128.png
        ${DESKFLOWER_ICONS_DIR}/png/deskflower-256.png
)
deskflower_add_app_icon(deskflower-agent)

if(WIN32)
    target_sources(deskflower-agent PRIVATE filedragagent.h filedragagent_win.cpp)
    target_link_libraries(deskflower-agent PRIVATE ole32 shell32 shlwapi advapi32)
else()
    target_sources(deskflower-agent PRIVATE filedragagent.h filedragagent_other.cpp)
endif()

target_link_libraries(deskflower-agent PRIVATE
    deskflower_core
    deskflower_clipboard
    qrcodegen
    Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2)

# Self-contained installation: cmake --install <build> --prefix <dir> (Qt libraries via windeployqt).
install(TARGETS deskflower-agent
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_qml_app_script(
    TARGET deskflower-agent
    OUTPUT_SCRIPT deploy_script
    DEPLOY_TOOL_OPTIONS ${DESKFLOWER_DEPLOY_OPTIONS}
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
