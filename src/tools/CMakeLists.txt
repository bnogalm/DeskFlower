# Release signing tool (not installed): keys, manifests and signatures for signed updates.
qt_add_executable(deskflower-sign sign.cpp)
set_target_properties(deskflower-sign PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(deskflower-sign PRIVATE deskflower_core)
if(WIN32)
    target_link_libraries(deskflower-sign PRIVATE advapi32)
endif()
