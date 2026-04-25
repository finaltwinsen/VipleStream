# windows specific compile definitions

add_compile_definitions(SUNSHINE_PLATFORM="windows")

# Require Windows 10 APIs (WaitOnAddress, etc.)
# NOMINMAX: prevent windows.h min/max macros from conflicting with std::numeric_limits
# D3D11_NO_HELPERS: suppress C++ operator== definitions in d3d11.h that cause
#   extern "C" linkage conflicts when d3d11.h is pulled in by FFmpeg headers
if(MSVC)
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX D3D11_NO_HELPERS)
endif()

enable_language(RC)
if(NOT MSVC)
    set(CMAKE_RC_COMPILER windres)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    # gcc complains about misleading indentation in some mingw includes
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-misleading-indentation)
endif()

# Disable warnings for Windows ARM64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-dll-attribute-on-redeclaration)  # Boost
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-unknown-warning-option)  # ViGEmClient
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-unused-variable)  # Boost
endif()

# see gcc bug 98723
add_definitions(-DUSE_BOOST_REGEX)

# curl
add_definitions(-DCURL_STATICLIB)
include_directories(SYSTEM ${CURL_STATIC_INCLUDE_DIRS})
link_directories(${CURL_STATIC_LIBRARY_DIRS})

# miniupnpc
add_definitions(-DMINIUPNP_STATICLIB)

# extra tools/binaries for audio/display devices
add_subdirectory(tools)  # todo - this is temporary, only tools for Windows are needed, for now

# nvidia
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvapi")
file(GLOB NVPREFS_FILES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/third-party/nvapi/*.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/nvprefs/*.h")

# vigem
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include")

# sunshine icon
if(NOT DEFINED SUNSHINE_ICON_PATH)
    set(SUNSHINE_ICON_PATH "${CMAKE_SOURCE_DIR}/sunshine.ico")
endif()

# Create a separate object library for the RC file with minimal includes
add_library(sunshine_rc_object OBJECT "${CMAKE_SOURCE_DIR}/src/platform/windows/windows.rc")

# Set minimal properties for RC compilation - only what's needed for the resource file
# Otherwise compilation can fail due to "line too long" errors
set_target_properties(sunshine_rc_object PROPERTIES
    COMPILE_DEFINITIONS "PROJECT_ICON_PATH=${SUNSHINE_ICON_PATH};PROJECT_NAME=${PROJECT_NAME};PROJECT_VENDOR=${SUNSHINE_PUBLISHER_NAME};PROJECT_VERSION=${PROJECT_VERSION};PROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};PROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR};PROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH};RC_VERSION_BUILD=${RC_VERSION_BUILD};RC_VERSION_REVISION=${RC_VERSION_REVISION}"  # cmake-lint: disable=C0301
    INCLUDE_DIRECTORIES ""
)

# ViGEmBus version
set(VIGEMBUS_PACKAGED_V "1.21.442")
set(VIGEMBUS_PACKAGED_V_2 "${VIGEMBUS_PACKAGED_V}.0")
list(APPEND SUNSHINE_DEFINITIONS VIGEMBUS_PACKAGED_VERSION="${VIGEMBUS_PACKAGED_V_2}")

set(PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/platform/windows/publish.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/misc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_base.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_vram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_ram.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/display_wgc.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/steam_scanner.h"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/steam_scanner.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/vdf_parser.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/src/ViGEmClient.cpp"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Client.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Common.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/Util.h"
        "${CMAKE_SOURCE_DIR}/third-party/ViGEmClient/include/ViGEm/km/BusShared.h"
        ${NVPREFS_FILES})

if(NOT MSVC)
    set(OPENSSL_LIBRARIES
            libssl.a
            libcrypto.a)
endif()

if(MSVC)
    list(PREPEND PLATFORM_LIBRARIES
            ${CURL_STATIC_LIBRARIES}
            avrt
            d3d11
            D3DCompiler
            dwmapi
            dxgi
            iphlpapi
            ksuser
            ${MINHOOK_LIBRARY}
            ntdll
            setupapi
            shlwapi
            synchronization.lib
            userenv
            ws2_32
            wsock32
    )
else()
    list(PREPEND PLATFORM_LIBRARIES
            ${CURL_STATIC_LIBRARIES}
            avrt
            d3d11
            D3DCompiler
            dwmapi
            dxgi
            iphlpapi
            ksuser
            libssp.a
            libstdc++.a
            libwinpthread.a
            minhook::minhook
            ntdll
            setupapi
            shlwapi
            synchronization.lib
            userenv
            ws2_32
            wsock32
    )
endif()

if(SUNSHINE_ENABLE_TRAY)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/third-party/tray/src/tray_windows.c")
endif()

# ── viple-splash.exe ──────────────────────────────────────────────────────────
# VipleStream H Phase 2.3: tiny Win32 GUI exe that spawns in the user
# session (via Sunshine's detached-command path) to cover the desktop
# with a black topmost window while a Steam game loads. Keeps the
# "launching game" transition from leaking the user's desktop to the
# streaming client for 5-15 seconds.
#
# Built as a standalone executable — no link against Sunshine or its
# boost/ssl deps. Install next to sunshine.exe so Sunshine can locate it
# at runtime via its own GetModuleFileName(NULL) -> dirname path.
add_executable(viple_splash WIN32
        "${CMAKE_SOURCE_DIR}/src/tools/viple_splash.cpp"
)
if(MSVC)
    target_link_libraries(viple_splash PRIVATE user32 gdi32 shcore kernel32)
else()
    # MinGW with `-static` (inherited from SUNSHINE exe linker flags) will
    # skip implicit libs — spell out every DLL the code actually touches:
    #   user32  — CreateWindowEx / ShowWindow / EnumWindows / FillRect /
    #             GetSystemMetrics / LoadCursor / RegisterClassEx / ...
    #   gdi32   — GetStockObject (BLACK_BRUSH)
    #   shcore  — SetProcessDpiAwarenessContext
    #   kernel32— GetTickCount64 / Sleep (via std::this_thread / std::chrono)
    # mingwthrd / mingw32 / msvcrt / pthread are all auto-pulled by the
    # C++ standard-library link line; std::thread / std::chrono compile
    # cleanly against them.
    target_link_libraries(viple_splash PRIVATE user32 gdi32 shcore kernel32)

    # Our entry point is wWinMain (wide-char) — MinGW's default crtexewin.c
    # looks for WinMain unless we pass -municode at both compile and link
    # time. Without this we get:
    #   undefined reference to `WinMain'
    # at the link stage even though wWinMain is defined.
    target_compile_options(viple_splash PRIVATE -municode)
    target_link_options(viple_splash PRIVATE -municode)
endif()
set_target_properties(viple_splash PROPERTIES
        OUTPUT_NAME "viple-splash"
)
