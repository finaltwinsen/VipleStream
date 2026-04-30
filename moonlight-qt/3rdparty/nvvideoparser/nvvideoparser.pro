# §J.3.e.2.i.8 — H.265 bitstream parser library (static).
#
# Source imported from NVIDIA/Khronos vk_video_samples repo (Apache 2.0).
# 跟 h264bitstream 同 build pattern：QT-less static lib, included by app/.

QT       -= core gui

TARGET = nvvideoparser
TEMPLATE = lib

CONFIG += staticlib
CONFIG += warn_off  # 上游 NVIDIA code 用 -Wall 會噴一堆 noise

include(../../globaldefs.pri)

# 需要 C++17 (constexpr if, structured bindings 等)
CONFIG += c++17

# 上游 #include 路徑用 "NvVideoParser/...", "VkCodecUtils/...",
# "vkvideo_parser/..." 子目錄.  保留這個 layout, INCLUDEPATH 加我們的
# include root + Vulkan 頭檔目錄.
#
# 兩個 Vulkan include path 都加：
#   libs/windows/include      → vk_video/* (codec std, 跨架構共用)
#   libs/windows/include/x64  → vulkan/*, libavcodec/*, etc. (arch-specific)
#
# NvVideoParser 子目錄也加進 INCLUDEPATH，因為部分 #include 用 <cpudetect.h>
# (angle brackets) 而不是 "NvVideoParser/cpudetect.h" — 上游 build system
# 把 NvVideoParser/include 當作獨立的 include root.
INCLUDEPATH += \
    $$PWD/include \
    $$PWD/include/NvVideoParser \
    $$PWD/include/vkvideo_parser \
    $$PWD/include/VkCodecUtils \
    $$PWD/../../libs/windows/include \
    $$PWD/../../libs/windows/include/x64

# §J.3.e.2.i.8 — Phase 1 (H.265) + Phase 3 (AV1).  H264/VP9 still stripped.
# Old VIPLESTREAM_NVPARSER_H265_ONLY define is removed; the AV1 case is now
# always compiled (we patch the existing #ifndef VIPLESTREAM_NVPARSER_H265_ONLY
# in VulkanVideoDecoder.cpp's switch — un-defining the gate enables AV1+VP9
# but VP9 source isn't imported, so we keep that case behind a separate
# VIPLESTREAM_NVPARSER_NO_VP9 define).  H.264 same — VIPLESTREAM_NVPARSER_NO_H264.
DEFINES += VIPLESTREAM_NVPARSER_NO_H264 VIPLESTREAM_NVPARSER_NO_VP9

SRC = $$PWD/src
INC = $$PWD/include

# §J.3.e.2.i.8 silently drop parser-internal logging (option D 決策).
# nvVideoParser 用 #define logging macros，這裡覆寫成 noop.
DEFINES += NV_VIDEO_PARSER_NO_LOG

SOURCES += \
    $$SRC/VulkanH265Parser.cpp         \
    $$SRC/VulkanAV1Decoder.cpp         \
    $$SRC/VulkanAV1GlobalMotionDec.cpp \
    $$SRC/VulkanVideoDecoder.cpp       \
    $$SRC/cpudetect.cpp                \
    $$SRC/NextStartCodeC.cpp           \
    $$SRC/NextStartCodeAVX2.cpp        \
    $$SRC/NextStartCodeAVX512.cpp      \
    $$SRC/NextStartCodeSSSE3.cpp

# AVX2 / AVX512 / SSSE3 source files require specific compiler intrinsics
# enabled.  /arch:AVX2 covers most; AVX512/SSSE3 use intrinsic headers
# that work without the /arch flag (compiler still emits target-specific
# code via the intrinsics).
QMAKE_CXXFLAGS += /arch:AVX2

HEADERS += \
    $$INC/NvVideoParser/ByteStreamParser.h            \
    $$INC/NvVideoParser/VulkanAV1Decoder.h            \
    $$INC/NvVideoParser/VulkanH265Decoder.h           \
    $$INC/NvVideoParser/VulkanH26xDecoder.h           \
    $$INC/NvVideoParser/VulkanVideoDecoder.h          \
    $$INC/NvVideoParser/cpudetect.h                   \
    $$INC/NvVideoParser/nvVulkanVideoParser.h         \
    $$INC/NvVideoParser/nvVulkanVideoUtils.h          \
    $$INC/NvVideoParser/nvVulkanh265ScalingList.h     \
    $$INC/VkCodecUtils/VkVideoRefCountBase.h          \
    $$INC/VkCodecUtils/VulkanBitstreamBuffer.h        \
    $$INC/vkvideo_parser/PictureBufferBase.h          \
    $$INC/vkvideo_parser/StdVideoPictureParametersSet.h \
    $$INC/vkvideo_parser/VulkanVideoParser.h          \
    $$INC/vkvideo_parser/VulkanVideoParserIf.h        \
    $$INC/vkvideo_parser/VulkanVideoParserParams.h    \
    $$INC/vulkan_interfaces.h
