qt_add_library(deskflower_clipboard STATIC
    clipboardbridge.h clipboardbridge.cpp
    clipboardbackend.h clipboardbackend.cpp
)

target_include_directories(deskflower_clipboard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(deskflower_clipboard PUBLIC Qt6::Core Qt6::Gui)
if(WIN32)
    # Virtual files for pastes and drags, and the streams of a paste served while the files arrive
    # (a paste session, its requests to the service, the multithreaded apartment that hosts its
    # streams).
    target_sources(deskflower_clipboard PRIVATE virtualfiles.h virtualfiles_win.cpp
                   pastesession_win.h pastesession_win.cpp pasterequests_win.h pasterequests_win.cpp
                   streamhost_win.h streamhost_win.cpp)
    target_link_libraries(deskflower_clipboard PRIVATE Qt6::Network ole32 shell32 shlwapi)
endif()
if(APPLE)
    # NSPasteboard: change polling and lazily provided items (ARC for the Objective-C++ file).
    target_sources(deskflower_clipboard PRIVATE clipboardbridge_mac.h clipboardbridge_mac.mm)
    set_source_files_properties(clipboardbridge_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(deskflower_clipboard PRIVATE "-framework AppKit")
endif()
