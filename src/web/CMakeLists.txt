qt_add_library(deskflower_web STATIC
    certificate.h certificate.cpp
    httpserver.h httpserver.cpp
    phoneserver.h phoneserver.cpp
)

qt_add_resources(deskflower_web "phone_assets"
    PREFIX "/deskflower/web"
    BASE assets
    FILES
        assets/index.html
        assets/locked.html
        assets/app.js
        assets/app.css
)

target_include_directories(deskflower_web PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(deskflower_web
    PUBLIC Qt6::Core Qt6::Network
    PRIVATE Qt6::Gui OpenSSL::Crypto)

# Runtime: Qt's OpenSSL TLS backend and our certificate code need the OpenSSL DLLs next to the
# executables, both in the build tree and in installations.
if(WIN32)
    target_link_libraries(deskflower_web PRIVATE iphlpapi)
    get_filename_component(_openssl_bin "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE)
    file(GLOB DESKFLOWER_OPENSSL_DLLS "${_openssl_bin}/libcrypto-3*.dll" "${_openssl_bin}/libssl-3*.dll")
    set(DESKFLOWER_OPENSSL_DLLS ${DESKFLOWER_OPENSSL_DLLS} CACHE INTERNAL "")
    add_custom_target(deskflower_openssl_runtime ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DESKFLOWER_OPENSSL_DLLS} "${CMAKE_BINARY_DIR}"
        COMMENT "Copying OpenSSL runtime")
    add_dependencies(deskflower_web deskflower_openssl_runtime)
    install(FILES ${DESKFLOWER_OPENSSL_DLLS} DESTINATION ${CMAKE_INSTALL_BINDIR})
    # windeployqt only ships the OpenSSL TLS backend when OpenSSL is on its PATH; the phone server
    # needs it (server-side TLS with our EC key).
    install(FILES "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/tls/qopensslbackend$<$<CONFIG:Debug>:d>.dll"
            DESTINATION plugins/tls)
endif()
