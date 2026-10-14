qt_add_library(deskflower_core STATIC
    buildinfo.h buildinfo.cpp
    clipboardsync.h clipboardsync.cpp
    clock.h
    filedrag.h filedrag.cpp
    filetransfer.h filetransfer.cpp
    cpace.h cpace.cpp
    crypto.h crypto.cpp
    inputcontroller.h inputcontroller.cpp
    ipc.h ipc.cpp
    keymap.h keymap.cpp
    logging.h logging.cpp
    node.h node.cpp
    noise.h noise.cpp
    pairing.h pairing.cpp
    reliable.h reliable.cpp
    remoteview.h remoteview.cpp
    router.h router.cpp
    securestream.h securestream.cpp
    session.h session.cpp
    settings.h settings.cpp
    store.h store.cpp
    update.h update.cpp
    updater.h updater.cpp
    workspace.h workspace.cpp
    wire.h
)

# Version and update signing key, from the top-level CMakeLists.txt.
configure_file(buildconfig.h.in ${CMAKE_CURRENT_BINARY_DIR}/buildconfig.h @ONLY)

target_include_directories(deskflower_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(deskflower_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(deskflower_core
    PUBLIC Qt6::Core Qt6::Network
    PRIVATE df::sodium)
