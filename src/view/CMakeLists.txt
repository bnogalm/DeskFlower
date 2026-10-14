qt_add_library(deskflower_view STATIC
    framecodec.h framecodec.cpp
)
target_include_directories(deskflower_view PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(deskflower_view PUBLIC Qt6::Core Qt6::Gui)
