# macOS: DeskFlower.app holds every program, so that one bundle is copied, signed and granted
# permissions:
#   Contents/MacOS/DeskFlower                          the agent (the bundle's own program)
#   Contents/MacOS/deskflower-service                  the service (a LaunchAgent of the user)
#   Contents/Helpers/deskflower-session.app            the session helper, a bundle of its own
#                                                      (privacy permissions are granted per bundle)
#   Contents/Frameworks, Contents/PlugIns              Qt, libsodium, libcrypto and libssl
# The programs are installed there by their own directories; after them, one macdeployqt run deploys
# Qt for all three, the helper is pointed at the shared Frameworks, libssl (loaded at run time by
# Qt's OpenSSL backend, so macdeployqt does not see it) is added, and the whole bundle is signed
# ad hoc (cmake/macdeploy.cmake.in).
# NOTE: written without a macOS build environment; see the README.

find_program(DESKFLOWER_MACDEPLOYQT macdeployqt HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" REQUIRED)
file(REAL_PATH "${OPENSSL_SSL_LIBRARY}" DESKFLOWER_LIBSSL)
file(REAL_PATH "${OPENSSL_CRYPTO_LIBRARY}" DESKFLOWER_LIBCRYPTO)
configure_file(${CMAKE_CURRENT_LIST_DIR}/macdeploy.cmake.in ${CMAKE_BINARY_DIR}/macdeploy.cmake @ONLY)
install(SCRIPT ${CMAKE_BINARY_DIR}/macdeploy.cmake)
