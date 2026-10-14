# Provides the imported target df::sodium.
#  - Windows/MSVC: downloads the official prebuilt static libsodium.
#  - Linux/macOS: uses the system package via pkg-config (apt: libsodium-dev, brew: libsodium).

if(MSVC)
    include(FetchContent)
    FetchContent_Declare(libsodium_msvc
        URL https://download.libsodium.org/libsodium/releases/libsodium-1.0.20-msvc.zip
        URL_HASH SHA256=2ff97f9e3f5b341bdc808e698057bea1ae454f99e29ff6f9b62e14d0eb1b1baa
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(libsodium_msvc)

    set(_sodium_root "${libsodium_msvc_SOURCE_DIR}")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_sodium_arch x64)
    else()
        set(_sodium_arch Win32)
    endif()

    add_library(df_sodium STATIC IMPORTED GLOBAL)
    set_target_properties(df_sodium PROPERTIES
        IMPORTED_LOCATION_RELEASE "${_sodium_root}/${_sodium_arch}/Release/v143/static/libsodium.lib"
        IMPORTED_LOCATION_DEBUG "${_sodium_root}/${_sodium_arch}/Debug/v143/static/libsodium.lib"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
        IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
        INTERFACE_INCLUDE_DIRECTORIES "${_sodium_root}/include"
        INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC")
    add_library(df::sodium ALIAS df_sodium)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
    add_library(df::sodium ALIAS PkgConfig::SODIUM)
endif()
