#
# Loads FFmpeg pre-compiled binaries from GitHub releases or a user-specified path
#
include_guard(GLOBAL)

# ffmpeg pre-compiled binaries
if(NOT DEFINED FFMPEG_PREPARED_BINARIES)
    # Set platform-specific libraries
    if(WIN32)
        if(MSVC)
            # vpl.lib (Intel oneVPL) is not bundled with Windows SDK; omit it for MSVC builds.
            # FFmpeg loads the oneVPL dispatcher at runtime via LoadLibrary when available.
            set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid)
        else()
            set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
        endif()
    elseif(FREEBSD)
        # numa is not available on FreeBSD
        set(FFMPEG_PLATFORM_LIBRARIES va va-drm va-x11 X11)
    elseif(UNIX AND NOT APPLE)
        set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
    endif()

    # Determine download location
    set(FFMPEG_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps")

    # VipleStream：third-party/build-deps 是直接 vendor 的檔案（不是 git submodule），下面的 tag 查詢一定落空、
    # 退回「latest」release；latest 會隨上游換版（2026-07-24 起 nv-codec-headers 升到 13.1，nvenc_base.cpp 的
    # 版本檢查只收 12.0／13.0，且 13.1 會抬高 NVIDIA 驅動下限）。Linux 固定在最後一版 13.0 的 release，
    # 建置才可重現（2026-11-03 CachyOS 容器乾淨建置時踩到；linux-builder 是靠舊的快取）。
    # 換版時先確認 ffnvcodec/nvEncodeAPI.h 的版本並同步 nvenc_base.cpp 的檢查。
    if(UNIX AND NOT APPLE AND NOT DEFINED VIPLE_FFMPEG_BUILD_DEPS_TAG)
        set(VIPLE_FFMPEG_BUILD_DEPS_TAG "v2026.713.132551")
    endif()

    # Fetch tags for the build-deps submodule so tag lookups work in CI shallow clones
    execute_process(
        COMMAND git -C "${CMAKE_SOURCE_DIR}/third-party/build-deps" fetch --tags --depth=1
        OUTPUT_QUIET
        ERROR_QUIET
    )

    # Get the current commit/tag from the build-deps submodule
    execute_process(
        COMMAND git -C "${CMAKE_SOURCE_DIR}/third-party/build-deps" describe --tags --exact-match
        OUTPUT_VARIABLE FFMPEG_RELEASE_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # If no exact tag match, try to get the commit hash and look for a tag
    if(NOT FFMPEG_RELEASE_TAG)
        execute_process(
            COMMAND git -C "${CMAKE_SOURCE_DIR}/third-party/build-deps" rev-parse HEAD
            OUTPUT_VARIABLE BUILD_DEPS_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        # Try to find a tag that points to this commit
        execute_process(
            COMMAND git -C "${CMAKE_SOURCE_DIR}/third-party/build-deps" tag --points-at ${BUILD_DEPS_COMMIT}
            OUTPUT_VARIABLE FFMPEG_RELEASE_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()

    if(VIPLE_FFMPEG_BUILD_DEPS_TAG)
        set(FFMPEG_RELEASE_TAG "${VIPLE_FFMPEG_BUILD_DEPS_TAG}")
    endif()

    # Set GitHub release URL
    set(FFMPEG_GITHUB_REPO "LizardByte/build-deps")
    if(FFMPEG_RELEASE_TAG)
        set(FFMPEG_RELEASE_URL "https://github.com/${FFMPEG_GITHUB_REPO}/releases/download/${FFMPEG_RELEASE_TAG}")
        set(FFMPEG_VERSION_DIR "${FFMPEG_DOWNLOAD_DIR}/ffmpeg-${FFMPEG_RELEASE_TAG}")
        message(STATUS "Using FFmpeg from build-deps tag: ${FFMPEG_RELEASE_TAG}")
    else()
        set(FFMPEG_RELEASE_URL "https://github.com/${FFMPEG_GITHUB_REPO}/releases/latest/download")
        set(FFMPEG_VERSION_DIR "${FFMPEG_DOWNLOAD_DIR}/ffmpeg-latest")
        message(STATUS "Using FFmpeg from latest build-deps release")
    endif()

    # Set extraction directory and prepared binaries path
    set(FFMPEG_EXTRACT_DIR "${FFMPEG_DOWNLOAD_DIR}")
    set(FFMPEG_PREPARED_BINARIES "${FFMPEG_EXTRACT_DIR}/ffmpeg")

    # Set the archive filename based on architecture
    set(FFMPEG_ARCHIVE_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-ffmpeg.tar.gz")
    set(FFMPEG_ARCHIVE_PATH "${FFMPEG_VERSION_DIR}/${FFMPEG_ARCHIVE_NAME}")
    set(FFMPEG_DOWNLOAD_URL "${FFMPEG_RELEASE_URL}/${FFMPEG_ARCHIVE_NAME}")

    # Check if already downloaded and extracted
    if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a")
        # Check if we need to download the archive
        if(NOT EXISTS "${FFMPEG_ARCHIVE_PATH}")
            message(STATUS "Downloading FFmpeg binaries from ${FFMPEG_DOWNLOAD_URL}")

            # Download the archive
            file(DOWNLOAD
                "${FFMPEG_DOWNLOAD_URL}"
                "${FFMPEG_ARCHIVE_PATH}"
                SHOW_PROGRESS
                STATUS FFMPEG_DOWNLOAD_STATUS
                TIMEOUT 300
            )

            # Check download status
            list(GET FFMPEG_DOWNLOAD_STATUS 0 FFMPEG_DOWNLOAD_STATUS_CODE)
            list(GET FFMPEG_DOWNLOAD_STATUS 1 FFMPEG_DOWNLOAD_STATUS_MESSAGE)

            if(NOT FFMPEG_DOWNLOAD_STATUS_CODE EQUAL 0)
                message(FATAL_ERROR "Failed to download FFmpeg binaries: ${FFMPEG_DOWNLOAD_STATUS_MESSAGE}")
            endif()
        else()
            message(STATUS "Using cached FFmpeg archive at ${FFMPEG_ARCHIVE_PATH}")
        endif()

        # Extract the archive
        message(STATUS "Extracting FFmpeg binaries to ${FFMPEG_EXTRACT_DIR}")
        file(ARCHIVE_EXTRACT  # cmake-lint: disable=E1126
            INPUT "${FFMPEG_ARCHIVE_PATH}"
            DESTINATION "${FFMPEG_EXTRACT_DIR}"
        )

        # Verify extraction
        if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a")
            message(FATAL_ERROR "FFmpeg extraction failed or unexpected directory structure")
        endif()

        message(STATUS "FFmpeg binaries successfully downloaded and extracted")
    else()
        message(STATUS "Using existing FFmpeg binaries at ${FFMPEG_PREPARED_BINARIES}")
    endif()

    # Set FFmpeg libraries
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        set(HDR10_PLUS_LIBRARY "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()

    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a"
        ${HDR10_PLUS_LIBRARY}
        ${FFMPEG_PLATFORM_LIBRARIES}
    )
else()
    # User provided FFMPEG_PREPARED_BINARIES path
    message(STATUS "Using user-specified FFmpeg binaries at ${FFMPEG_PREPARED_BINARIES}")

    # Set platform-specific libraries
    if(NOT DEFINED FFMPEG_PLATFORM_LIBRARIES)
        if(WIN32)
            if(MSVC)
            # vpl.lib (Intel oneVPL) is not bundled with Windows SDK; omit it for MSVC builds.
            # FFmpeg loads the oneVPL dispatcher at runtime via LoadLibrary when available.
            set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid)
        else()
            set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
        endif()
        elseif(FREEBSD)
            set(FFMPEG_PLATFORM_LIBRARIES va va-drm va-x11 X11)
        elseif(UNIX AND NOT APPLE)
            set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
        endif()
    endif()

    # Set base FFmpeg libraries (always required)
    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
    )

    # Add optional libraries if they exist (e.g., from prebuilt packages)
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a")
    endif()
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()

    # Add platform libraries
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_PLATFORM_LIBRARIES})
endif()

set(FFMPEG_INCLUDE_DIRS "${FFMPEG_PREPARED_BINARIES}/include")
