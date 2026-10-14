qt_add_executable(deskflower-service
    main.cpp
    servicecontroller.h servicecontroller.cpp
    sessionlauncher.h sessionlauncher.cpp
    platform/platform.h
    platform/platform_common.cpp
)

if(WIN32)
    target_sources(deskflower-service PRIVATE platform/platform_win.cpp)
    target_link_libraries(deskflower-service PRIVATE advapi32 wtsapi32 userenv)
else()
    target_sources(deskflower-service PRIVATE platform/platform_unix.cpp)
endif()

set_target_properties(deskflower-service PROPERTIES WIN32_EXECUTABLE FALSE)
deskflower_add_app_icon(deskflower-service)
target_include_directories(deskflower-service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(deskflower-service PRIVATE deskflower_core deskflower_web)

# Self-contained installation: cmake --install <build> --prefix <dir> (Qt libraries via windeployqt).
install(TARGETS deskflower-service
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_app_script(
    TARGET deskflower-service
    OUTPUT_SCRIPT deploy_script
    DEPLOY_TOOL_OPTIONS ${DESKFLOWER_DEPLOY_OPTIONS}
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
