# common compile definitions
# this file will also load platform specific definitions

if(NOT MSVC)
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Wall -Wno-sign-compare)
endif()
# Wall - enable all warnings
# Werror - treat warnings as errors
# Wno-maybe-uninitialized/Wno-uninitialized - disable warnings for maybe uninitialized variables
# Wno-sign-compare - disable warnings for signed/unsigned comparisons
# Wno-restrict - disable warnings for memory overlap
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC specific compile options

    # GCC 12 and higher will complain about maybe-uninitialized
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
        list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-maybe-uninitialized)

        # Disable the bogus warning that may prevent compilation (only for GCC 12).
        # See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105651.
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
            list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-restrict)
        endif()
    endif()

    # GCC 15 will complain about uninitialized variables in some cases (Simple-Web-Server)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-uninitialized)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang specific compile options

    # Clang doesn't actually complain about this this, so disabling for now
    # list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-uninitialized)
endif()
if(BUILD_WERROR)
    list(APPEND SUNSHINE_COMPILE_OPTIONS -Werror)
endif()

# setup assets directory
if(NOT SUNSHINE_ASSETS_DIR)
    set(SUNSHINE_ASSETS_DIR "assets")
endif()

# platform specific compile definitions
if(WIN32)
    include(${CMAKE_MODULE_PATH}/compile_definitions/windows.cmake)
elseif(UNIX)
    include(${CMAKE_MODULE_PATH}/compile_definitions/unix.cmake)

    if(APPLE)
        include(${CMAKE_MODULE_PATH}/compile_definitions/macos.cmake)
    else()
        include(${CMAKE_MODULE_PATH}/compile_definitions/linux.cmake)
    endif()
endif()

include_directories(BEFORE SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nv-codec-headers/include")
file(GLOB NVENC_SOURCES CONFIGURE_DEPENDS "src/nvenc/*.cpp" "src/nvenc/*.h")
list(APPEND PLATFORM_TARGET_FILES ${NVENC_SOURCES})

# VipleStream: MP-QUIC multipath transport (opt-in)
option(VIPLE_MPQUIC "Enable MP-QUIC multipath transport" OFF)
if(VIPLE_MPQUIC)
    add_compile_definitions(VIPLE_MPQUIC=1)
    # picoquic + picotls use #ifdef _WINDOWS to skip POSIX headers.
    # MinGW defines _WIN32 but not _WINDOWS — add it globally so both
    # the library code and Sunshine's own #include <picoquic.h> work.
    if(WIN32 AND NOT MSVC)
        add_compile_definitions(_WINDOWS)
    endif()

    # picoquic submodule
    if(EXISTS "${CMAKE_SOURCE_DIR}/third-party/picoquic/CMakeLists.txt")
        set(PICOQUIC_FETCH_PTLS ON CACHE BOOL "" FORCE)
        # MinGW compat: picotls' fusion engine uses aligned_alloc
        # (C11 but missing from MinGW CRT) and poll() doesn't exist.
        if(WIN32 AND NOT MSVC)
            set(WITH_FUSION OFF CACHE BOOL "" FORCE)
            set(WITH_SELECT ON CACHE BOOL "" FORCE)
        endif()
        add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/picoquic" EXCLUDE_FROM_ALL)

        # picotls guards POSIX headers with #ifndef _WINDOWS but MinGW
        # doesn't define _WINDOWS (only _WIN32). Inject it so the POSIX
        # includes are correctly skipped.
        if(WIN32 AND NOT MSVC)
            # picotls's wincompat.h lives in picotlsvs/picotls/ — not in
            # the default include path. Also need to suppress MinGW/GCC
            # warnings for upstream third-party code we don't control.
            set(_ptls_src "${CMAKE_BINARY_DIR}/_deps/picotls-src")

            # picotls' wincompat.h (MSVC compat shim) redefines ssize_t
            # and struct timezone that MinGW already provides. Write a
            # thin wrapper that skips the conflicting parts.
            set(_compat_dir "${CMAKE_BINARY_DIR}/picotls-mingw-compat")
            file(MAKE_DIRECTORY "${_compat_dir}")
            file(WRITE "${_compat_dir}/wincompat.h" [=[
#ifndef VIPLE_MINGW_WINCOMPAT_H
#define VIPLE_MINGW_WINCOMPAT_H
/* MinGW compat shim for picotls + picoquic.
 * Both libraries ship a wincompat.h that redefines ssize_t, struct timezone,
 * and __attribute__ for MSVC.  MinGW already provides all three, so this shim
 * includes only what's actually needed and adds the wintimeofday() decl that
 * picoquic expects.
 */
#include <stdint.h>
#include <sys/types.h>    /* MinGW ssize_t */
#include <Winsock2.h>
#include <ws2tcpip.h>
#include <malloc.h>
#include <sys/time.h>     /* MinGW struct timezone + gettimeofday */

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif

/* MinGW has real gettimeofday — do NOT remap to wintimeofday.
 * The original wincompat.h does #define gettimeofday wintimeofday for MSVC,
 * but on MinGW that creates an undefined reference at link time. */

#endif /* VIPLE_MINGW_WINCOMPAT_H */
]=])

            foreach(_ptls_tgt picotls-core picotls-openssl picotls-minicrypto)
                if(TARGET ${_ptls_tgt})
                    target_compile_definitions(${_ptls_tgt} PRIVATE _WINDOWS)
                    # Our shim takes priority over picotlsvs/picotls/
                    target_include_directories(${_ptls_tgt} BEFORE PRIVATE
                        "${_compat_dir}"
                        "${_ptls_src}/picotlsvs/picotls")
                    # Suppress warnings + GCC 15 hard errors for
                    # upstream third-party code we don't modify.
                    target_compile_options(${_ptls_tgt} PRIVATE
                        -w
                        -Wno-incompatible-pointer-types
                        -Wno-error=incompatible-pointer-types
                        -Wno-error)
                endif()
            endforeach()
        endif()

        # picoquic also uses #ifdef _WINDOWS for POSIX header guards
        # and has its own wincompat.h with timezone/ssize_t redefs.
        if(WIN32 AND NOT MSVC)
            if(TARGET picoquic-core)
                target_compile_definitions(picoquic-core PRIVATE _WINDOWS)
                target_include_directories(picoquic-core BEFORE PRIVATE
                    "${_compat_dir}")
                target_compile_options(picoquic-core PRIVATE
                    -w -Wno-error
                    -Wno-incompatible-pointer-types
                    -Wno-error=incompatible-pointer-types)
            endif()
        endif()

        include_directories(SYSTEM
            "${CMAKE_SOURCE_DIR}/third-party/picoquic/picoquic"
            "${CMAKE_SOURCE_DIR}/third-party/picoquic/picoquicfirst"
            # picotls headers for minicrypto signing bypass
            # (picoquic marks PTLS_INCLUDE_DIRS PRIVATE, so we need
            #  explicit access for quic_server.cpp to call
            #  ptls_minicrypto_load_private_key / ptls_load_certificates)
            "${CMAKE_BINARY_DIR}/_deps/picotls-src/include")
        list(APPEND PLATFORM_LIBRARIES picoquic-core picotls-minicrypto)
    else()
        message(WARNING "VIPLE_MPQUIC=ON but third-party/picoquic not found. "
                        "Run: git submodule add https://github.com/private-octopus/picoquic.git "
                        "third-party/picoquic && git submodule update --init --recursive")
    endif()

    list(APPEND PLATFORM_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/src/quic_server.h"
        "${CMAKE_SOURCE_DIR}/src/quic_server.cpp")
endif()

set(SUNSHINE_TARGET_FILES
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Input.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Rtsp.h"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/RtspParser.c"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/src/Video.h"
        "${CMAKE_SOURCE_DIR}/third-party/tray/src/tray.h"
        "${CMAKE_SOURCE_DIR}/src/upnp.cpp"
        "${CMAKE_SOURCE_DIR}/src/upnp.h"
        "${CMAKE_SOURCE_DIR}/src/stun.cpp"
        "${CMAKE_SOURCE_DIR}/src/stun.h"
        "${CMAKE_SOURCE_DIR}/src/relay.cpp"
        "${CMAKE_SOURCE_DIR}/src/relay.h"
        "${CMAKE_SOURCE_DIR}/src/udp_tunnel.cpp"
        "${CMAKE_SOURCE_DIR}/src/udp_tunnel.h"
        "${CMAKE_SOURCE_DIR}/src/tunnel_session.cpp"
        "${CMAKE_SOURCE_DIR}/src/tunnel_session.h"
        "${CMAKE_SOURCE_DIR}/src/cbs.cpp"
        "${CMAKE_SOURCE_DIR}/src/utility.h"
        "${CMAKE_SOURCE_DIR}/src/uuid.h"
        "${CMAKE_SOURCE_DIR}/src/config.h"
        "${CMAKE_SOURCE_DIR}/src/config.cpp"
        "${CMAKE_SOURCE_DIR}/src/display_device.h"
        "${CMAKE_SOURCE_DIR}/src/display_device.cpp"
        "${CMAKE_SOURCE_DIR}/src/entry_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/entry_handler.h"
        "${CMAKE_SOURCE_DIR}/src/file_handler.cpp"
        "${CMAKE_SOURCE_DIR}/src/file_handler.h"
        "${CMAKE_SOURCE_DIR}/src/file_transfer.cpp"
        "${CMAKE_SOURCE_DIR}/src/file_transfer.h"
        "${CMAKE_SOURCE_DIR}/src/globals.cpp"
        "${CMAKE_SOURCE_DIR}/src/globals.h"
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/logging.h"
        "${CMAKE_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_SOURCE_DIR}/src/main.h"
        "${CMAKE_SOURCE_DIR}/src/crypto.cpp"
        "${CMAKE_SOURCE_DIR}/src/crypto.h"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/nvhttp.h"
        "${CMAKE_SOURCE_DIR}/src/httpcommon.cpp"
        "${CMAKE_SOURCE_DIR}/src/httpcommon.h"
        "${CMAKE_SOURCE_DIR}/src/confighttp.cpp"
        "${CMAKE_SOURCE_DIR}/src/confighttp.h"
        "${CMAKE_SOURCE_DIR}/src/rtsp.cpp"
        "${CMAKE_SOURCE_DIR}/src/rtsp.h"
        "${CMAKE_SOURCE_DIR}/src/stream.cpp"
        "${CMAKE_SOURCE_DIR}/src/stream.h"
        "${CMAKE_SOURCE_DIR}/src/video.cpp"
        "${CMAKE_SOURCE_DIR}/src/video.h"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.cpp"
        "${CMAKE_SOURCE_DIR}/src/video_colorspace.h"
        "${CMAKE_SOURCE_DIR}/src/input.cpp"
        "${CMAKE_SOURCE_DIR}/src/input.h"
        "${CMAKE_SOURCE_DIR}/src/audio.cpp"
        "${CMAKE_SOURCE_DIR}/src/audio.h"
        "${CMAKE_SOURCE_DIR}/src/platform/common.h"
        "${CMAKE_SOURCE_DIR}/src/process.cpp"
        "${CMAKE_SOURCE_DIR}/src/process.h"
        "${CMAKE_SOURCE_DIR}/src/network.cpp"
        "${CMAKE_SOURCE_DIR}/src/network.h"
        "${CMAKE_SOURCE_DIR}/src/move_by_copy.h"
        "${CMAKE_SOURCE_DIR}/src/system_tray.cpp"
        "${CMAKE_SOURCE_DIR}/src/system_tray.h"
        "${CMAKE_SOURCE_DIR}/src/task_pool.h"
        "${CMAKE_SOURCE_DIR}/src/thread_pool.h"
        "${CMAKE_SOURCE_DIR}/src/thread_safe.h"
        "${CMAKE_SOURCE_DIR}/src/sync.h"
        "${CMAKE_SOURCE_DIR}/src/round_robin.h"
        "${CMAKE_SOURCE_DIR}/src/stat_trackers.h"
        "${CMAKE_SOURCE_DIR}/src/stat_trackers.cpp"
        "${CMAKE_SOURCE_DIR}/src/rswrapper.h"
        "${CMAKE_SOURCE_DIR}/src/rswrapper.c"
        ${PLATFORM_TARGET_FILES})

if(NOT SUNSHINE_ASSETS_DIR_DEF)
    set(SUNSHINE_ASSETS_DIR_DEF "${SUNSHINE_ASSETS_DIR}")
endif()
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ASSETS_DIR="${SUNSHINE_ASSETS_DIR_DEF}")

list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_TRAY=${SUNSHINE_TRAY})

# Publisher metadata
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_NAME="${SUNSHINE_PUBLISHER_NAME}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_WEBSITE="${SUNSHINE_PUBLISHER_WEBSITE}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_PUBLISHER_ISSUE_URL="${SUNSHINE_PUBLISHER_ISSUE_URL}")

include_directories(BEFORE "${CMAKE_SOURCE_DIR}")

include_directories(
        BEFORE
        SYSTEM
        "${CMAKE_SOURCE_DIR}/third-party"
        "${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet/include"
        "${CMAKE_SOURCE_DIR}/third-party/nanors"
        "${CMAKE_SOURCE_DIR}/third-party/nanors/deps/obl"
        ${OPENSSL_INCLUDE_DIR}
        ${Opus_INCLUDE_DIR}
        ${FFMPEG_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}  # has to be the last, or we get runtime error on macOS ffmpeg encoder
)

list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        ${MINIUPNP_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        enet
        libdisplaydevice::display_device
        nlohmann_json::nlohmann_json
        ${Opus_LIBRARY}
        ${FFMPEG_LIBRARIES}
        ${Boost_LIBRARIES}
        ${OPENSSL_LIBRARIES}
        ${PLATFORM_LIBRARIES})
