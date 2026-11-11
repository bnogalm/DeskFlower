# deskflower_add_test(name [SOURCES files...] [PARTS parts...]): SOURCES default to <name>.cpp;
# with PARTS, one ctest entry <name>_<part> per part (passed as the first argument), so that they run
# in parallel and report apart.
function(deskflower_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PARTS" ${ARGN})
    if(NOT ARG_SOURCES)
        set(ARG_SOURCES ${name}.cpp)
    endif()
    qt_add_executable(${name} ${ARG_SOURCES})
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(${name} PRIVATE deskflower_core df::sodium Qt6::Test)
    set(_tests)
    if(ARG_PARTS)
        foreach(_part IN LISTS ARG_PARTS)
            add_test(NAME ${name}_${_part} COMMAND ${name} ${_part})
            list(APPEND _tests ${name}_${_part})
        endforeach()
    else()
        add_test(NAME ${name} COMMAND ${name})
        set(_tests ${name})
    endif()
    # Loopback only: no firewall prompt for each new test binary (see Node, DESKFLOWER_BIND_ADDRESS).
    set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT "DESKFLOWER_BIND_ADDRESS=127.0.0.1")
    if(WIN32)
        # Let the test binaries find the Qt DLLs without deploying them.
        get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qtbin "${_qmake}" DIRECTORY)
        set_tests_properties(${_tests} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin}")
        add_dependencies(${name} deskflower_openssl_runtime)
    endif()
endfunction()

deskflower_add_test(tst_crypto)
deskflower_add_test(tst_reliable)
deskflower_add_test(tst_node)
deskflower_add_test(tst_workspace)
deskflower_add_test(tst_router)
deskflower_add_test(tst_input)
deskflower_add_test(tst_clipboard)
deskflower_add_test(tst_agentwatch)
if(WIN32)
    deskflower_add_test(tst_sealedclipboard)
    target_link_libraries(tst_sealedclipboard PRIVATE deskflower_clipboard ole32 shell32 user32)
    # A paste served while its files arrive: the streams, their apartment, the paste session, and
    # another process reading them (local COM only: no clipboard, shell, input or network).
    deskflower_add_test(tst_virtualstream)
    target_link_libraries(tst_virtualstream PRIVATE deskflower_clipboard ole32 shell32 shlwapi user32)
endif()
deskflower_add_test(tst_transfer)
deskflower_add_test(tst_keymap)
# The helper's keyboard injections (keyinjection.h, pure); the keyboard diagnostics only when built.
deskflower_add_test(tst_keyinjection)
if(DESKFLOWER_KEY_DIAGNOSTICS)
    deskflower_add_test(tst_keydiag)
endif()
deskflower_add_test(tst_update)
deskflower_add_test(tst_web)
target_link_libraries(tst_web PRIVATE deskflower_web Qt6::Gui)
deskflower_add_test(tst_phoneuser)
target_link_libraries(tst_phoneuser PRIVATE deskflower_web Qt6::Gui)
deskflower_add_test(tst_view)
target_link_libraries(tst_view PRIVATE deskflower_view)
deskflower_add_test(tst_filedrag)
deskflower_add_test(tst_fsutil)
deskflower_add_test(tst_platform)
target_link_libraries(tst_platform PRIVATE deskflower_platform)
# LABELS desktop: cases that touch the signed-in user's desktop or profile (the clipboard, the shell,
# a dialog, folders in the home folder). They skip unless DESKFLOWER_DESKTOP_TESTS=1, which only CI,
# VMs and containers set (docs/TESTING.md, "Desktop tests").
set_tests_properties(tst_platform PROPERTIES LABELS desktop)
deskflower_add_test(tst_logging)
# The agent's alert for paired devices of another protocol (its model types alone).
deskflower_add_test(tst_protocolalert SOURCES tst_protocolalert.cpp ${PROJECT_SOURCE_DIR}/src/agent/modeltypes.h
                    ${PROJECT_SOURCE_DIR}/src/agent/modeltypes.cpp)
target_include_directories(tst_protocolalert PRIVATE ${PROJECT_SOURCE_DIR}/src/agent)
target_link_libraries(tst_protocolalert PRIVATE Qt6::Qml)
if(WIN32)
    # The drag's virtual files against the shell's own drop target (no mouse, but the real shell:
    # those cases skip unless DESKFLOWER_DESKTOP_TESTS=1).
    deskflower_add_test(tst_dragwin)
    set_tests_properties(tst_dragwin PROPERTIES LABELS desktop)
    target_link_libraries(tst_dragwin PRIVATE deskflower_clipboard ole32 shell32 shlwapi)
    # The agent's clipboard objects pasted through the shell (Windows only).
    deskflower_add_test(tst_clipboardbridge)
    target_link_libraries(tst_clipboardbridge PRIVATE deskflower_clipboard Qt6::Gui ole32 shell32 user32)
    set_tests_properties(tst_clipboardbridge PROPERTIES LABELS desktop)
endif()
# Crash diagnostics: the crashes happen in a helper process next to the test.
qt_add_executable(crashprobe crashprobe.cpp)
set_target_properties(crashprobe PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(crashprobe PRIVATE deskflower_core)
deskflower_add_test(tst_crash)
add_dependencies(tst_crash crashprobe)

# Many-small-files transfer benchmark: a program, not a test (no add_test: ctest never runs it), and
# it does nothing unless DESKFLOWER_BENCH=1. Data from tests/bench/make_tree.py; see bench_transfer.cpp.
qt_add_executable(bench_transfer bench_transfer.cpp)
set_target_properties(bench_transfer PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(bench_transfer PRIVATE deskflower_core df::sodium Qt6::Test)
if(WIN32)
    add_dependencies(bench_transfer deskflower_openssl_runtime)
endif()

# Streaming paste benchmark (Windows): another process reads the agent's virtual files as Explorer
# would. Not a test either (no add_test), and it does nothing unless DESKFLOWER_BENCH=1.
if(WIN32)
    qt_add_executable(bench_paste bench_paste.cpp)
    set_target_properties(bench_paste PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(bench_paste PRIVATE deskflower_clipboard ole32 shell32 shlwapi user32)
endif()

# End-to-end: real deskflower-service processes driven over their IPC as the agent and the session
# helper (tests/e2e). Parts: the command line, two devices on loopback, the same behind a NAT.
deskflower_add_test(tst_e2e SOURCES e2e/tst_e2e.cpp e2e/e2eharness.h PARTS cli direct nat)
target_include_directories(tst_e2e PRIVATE e2e)
target_compile_definitions(tst_e2e PRIVATE DF_SERVICE_EXE="$<TARGET_FILE:deskflower-service>")
add_dependencies(tst_e2e deskflower-service)
set_tests_properties(tst_e2e_cli PROPERTIES LABELS desktop)

if(UNIX)
    # The service's SessionLauncher, with the test program itself as the session helper.
    deskflower_add_test(tst_sessionlauncher)
    target_sources(tst_sessionlauncher PRIVATE ${PROJECT_SOURCE_DIR}/src/service/sessionlauncher.cpp)
    target_include_directories(tst_sessionlauncher PRIVATE ${PROJECT_SOURCE_DIR}/src/service)
    target_link_libraries(tst_sessionlauncher PRIVATE deskflower_platform)
endif()

if(UNIX AND NOT APPLE)
    # Linux integration tests: X11 under Xvfb (the session helper's backends, the clipboard as
    # other clients see it, the agent's drag and drop), and the Wayland portal session against a
    # fake portal on the session bus. They skip themselves without a display or a bus, and unless
    # DESKFLOWER_PRIVATE_SESSION=1 (tests/docker, the CI) says both are the test run's own: they
    # grab the pointer and keyboard, inject input, write the clipboard and claim the portal's name.
    find_package(X11 REQUIRED)
    deskflower_add_test(tst_x11)
    target_link_libraries(tst_x11 PRIVATE deskflower_session_linux deskflower_dragagent Qt6::Widgets
                          X11::X11 X11::Xi X11::Xtst)
    set_tests_properties(tst_x11 PROPERTIES LABELS desktop)
    find_package(Qt6 COMPONENTS DBus)
    if(TARGET Qt6::DBus)
        deskflower_add_test(tst_portal)
        target_link_libraries(tst_portal PRIVATE deskflower_session_linux Qt6::DBus)
        set_tests_properties(tst_portal PROPERTIES LABELS desktop)
    endif()
endif()

# The service's controller over its local IPC (its sources: it lives in the service executable).
deskflower_add_test(tst_servicecontroller)
target_sources(tst_servicecontroller PRIVATE ${PROJECT_SOURCE_DIR}/src/service/servicecontroller.cpp)
target_include_directories(tst_servicecontroller PRIVATE ${PROJECT_SOURCE_DIR}/src/service)
target_link_libraries(tst_servicecontroller PRIVATE deskflower_platform deskflower_web)
if(APPLE)
    # The real pasteboard (cocoa platform, not the offscreen one the other tests may use): skipped
    # unless DESKFLOWER_DESKTOP_TESTS=1 (CI), never on a developer's own clipboard by default.
    set_source_files_properties(tst_macclipboard.cpp PROPERTIES LANGUAGE OBJCXX) # reads NSPasteboard
    deskflower_add_test(tst_macclipboard)
    target_link_libraries(tst_macclipboard PRIVATE deskflower_clipboard Qt6::Gui "-framework AppKit")
    set_property(TEST tst_macclipboard APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
    set_tests_properties(tst_macclipboard PROPERTIES LABELS desktop)
endif()
