qt_add_executable(deskflower-agent WIN32 MACOSX_BUNDLE
    main.cpp
    backend.h backend.cpp
    modeltypes.h modeltypes.cpp
    thumbnailprovider.h
    qrimageprovider.h qrimageprovider.cpp
)

set(_agent_qml_files
    qml/Main.qml
    qml/DevicesPage.qml
    qml/ScreensPage.qml
    qml/FilesPage.qml
    qml/PeerRow.qml
    qml/PinDisplayDialog.qml
    qml/PinEntryDialog.qml
    qml/PairModeDialog.qml
    qml/TrustChooser.qml
    qml/TrustLevels.qml
    qml/ViewPlanDialog.qml
    qml/ViewRequestDialog.qml
    qml/AboutDialog.qml
    qml/AdvancedSettingsWindow.qml
    qml/Bouquet.qml
    qml/TransferRow.qml
    qml/TransferWindow.qml
)
# A QML singleton must be marked as such before the module is declared.
set_source_files_properties(qml/TrustLevels.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
# In the resources the files sit next to the module's qmldir (Deskflower/Main.qml, not
# Deskflower/qml/Main.qml): only there do they see the module's C++ value types (peerInfo...).
foreach(_file IN LISTS _agent_qml_files)
    get_filename_component(_name ${_file} NAME)
    set_source_files_properties(${_file} PROPERTIES QT_RESOURCE_ALIAS ${_name})
endforeach()

qt_add_qml_module(deskflower-agent
    URI Deskflower
    VERSION 1.0
    QML_FILES ${_agent_qml_files}
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
    target_link_libraries(deskflower-agent PRIVATE ole32 shell32 shlwapi advapi32 dwmapi)
elseif(UNIX AND NOT APPLE)
    # X11 drag and drop (XDND), in a library of its own so that tests/tst_x11.cpp can drive it.
    find_package(X11 REQUIRED)
    qt_add_library(deskflower_dragagent STATIC filedragagent.h filedragagent_x11.cpp)
    target_include_directories(deskflower_dragagent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(deskflower_dragagent PUBLIC deskflower_clipboard Qt6::Widgets PRIVATE X11::X11 X11::Xtst)
    target_link_libraries(deskflower-agent PRIVATE deskflower_dragagent)
elseif(APPLE)
    target_sources(deskflower-agent PRIVATE filedragagent.h filedragagent_mac.mm)
    set_source_files_properties(filedragagent_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(deskflower-agent PRIVATE
        "-framework AppKit" "-framework ApplicationServices" "-framework UniformTypeIdentifiers")
else()
    target_sources(deskflower-agent PRIVATE filedragagent.h filedragagent_other.cpp)
endif()

target_link_libraries(deskflower-agent PRIVATE
    deskflower_core
    deskflower_clipboard
    qrcodegen
    Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2)

if(APPLE)
    # DeskFlower.app: the agent, with the service (Contents/MacOS) and the session helper
    # (Contents/Helpers) put inside when installed (cmake/MacBundle.cmake).
    set_target_properties(deskflower-agent PROPERTIES
        OUTPUT_NAME DeskFlower
        MACOSX_BUNDLE_GUI_IDENTIFIER com.deskflower.agent
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/cmake/agent-Info.plist.in)
endif()

# Self-contained installation: cmake --install <build> --prefix <dir> (Qt libraries via windeployqt;
# macOS: one macdeployqt run for the whole bundle, cmake/MacBundle.cmake).
install(TARGETS deskflower-agent
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(NOT APPLE)
    qt_generate_deploy_qml_app_script(
        TARGET deskflower-agent
        OUTPUT_SCRIPT deploy_script
        DEPLOY_TOOL_OPTIONS ${DESKFLOWER_DEPLOY_OPTIONS}
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${deploy_script})
endif()
