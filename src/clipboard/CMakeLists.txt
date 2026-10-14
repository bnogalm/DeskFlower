qt_add_library(deskflower_clipboard STATIC
    clipboardbridge.h clipboardbridge.cpp
)

target_include_directories(deskflower_clipboard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(deskflower_clipboard PUBLIC Qt6::Core Qt6::Gui)
if(WIN32)
    target_sources(deskflower_clipboard PRIVATE virtualfiles.h virtualfiles_win.cpp)
    target_link_libraries(deskflower_clipboard PRIVATE ole32 shell32 shlwapi)
endif()
