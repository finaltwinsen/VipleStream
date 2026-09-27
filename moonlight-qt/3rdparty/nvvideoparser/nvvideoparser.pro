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
# NvVideoParser 子目錄也加進 INCLUDEPATH，因為部分 #include 用 <cpudetect.h>
# (angle brackets) 而不是 "NvVideoParser/cpudetect.h" — 上游 build system
# 把 NvVideoParser/include 當作獨立的 include root.
INCLUDEPATH += \
    $$PWD/include \
    $$PWD/include/NvVideoParser \
    $$PWD/include/vkvideo_parser \
    $$PWD/include/VkCodecUtils

# Windows 才加內附的 Vulkan 標頭（兩個 include path 都要）：
#   libs/windows/include      → vk_video/* (codec std, 跨架構共用)
#   libs/windows/include/x64  → vulkan/*, libavcodec/*, etc. (arch-specific)
# §M2a（F8）：Linux／Flatpak 一律用系統（SDK）的 Vulkan 標頭，和 app 本體同一份；
# 以前 Linux 也加這兩條，static lib 與 app 會用到不同版本的 Vulkan 標頭（ODR 風險）。
win32 {
    INCLUDEPATH += \
        $$PWD/../../libs/windows/include \
        $$PWD/../../libs/windows/include/x64
}

# §J.3.e.2.i.8 — Phase 1 (H.265) + Phase 2 (H.264) + Phase 3 (AV1).  VP9 still
# stripped (source not imported).  H264 is now always compiled (Phase 2 import,
# v1.3.273); the switch case in VulkanVideoDecoder.cpp's CreateVulkanVideoDecodeParser
# uses #ifndef VIPLESTREAM_NVPARSER_NO_H264 → un-define removes the gate.
DEFINES += VIPLESTREAM_NVPARSER_NO_VP9

SRC = $$PWD/src
INC = $$PWD/include

# §J.3.e.2.i.8 silently drop parser-internal logging (option D 決策).
# nvVideoParser 用 #define logging macros，這裡覆寫成 noop.
DEFINES += NV_VIDEO_PARSER_NO_LOG

SOURCES += \
    $$SRC/VulkanH264Parser.cpp         \
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
#
# §M2a-R4：gcc（*-g++*）不再加全域 -m 旗標。以前 -mavx512* 套在整個 lib 的每個 TU 上，
# gcc 可以在一般 TU（含標頭 inline 函式）自動產生 AVX-512 指令，在沒有 AVX-512 的 CPU
# （例如 builder 的 Ryzen）上走 Vulkan-video parser 就可能 SIGILL。改成只有三個 SIMD TU
# 在檔頭用 `#pragma GCC target(...)` 各自開啟需要的指令集（NextStartCode{SSSE3,AVX2,AVX512}.cpp）；
# 執行期仍由 cpudetect.cpp 依 CPUID 選路。其他 gcc 相容編譯器（clang 等）維持舊行為：
# 全域開啟三種 SIMD 變體要的指令集。
*-msvc {
    QMAKE_CXXFLAGS += /arch:AVX2
} else:equals(QT_ARCH, x86_64):!*-g++* {
    QMAKE_CXXFLAGS += -mssse3 -mavx -mavx2 -mfma \
                      -mavx512f -mavx512bw -mavx512dq -mavx512vl
}

# §M2a（F8）：上游只匯入了 x86 的 SIMD 實作（NEON／SVE 版沒有匯入），非 x86 架構
# （Flatpak aarch64）不定義這個，VulkanVideoDecoder.cpp 會去呼叫不存在的
# ParseByteStreamNEON/SVE → undefined reference。定義後一律走 C 版 next_start_code。
# SOURCES 不用動：三個 SIMD TU 在非 x86_64 上整檔被 #if 排除，是空的 TU。
!equals(QT_ARCH, x86_64):!equals(QT_ARCH, i386) {
    DEFINES += DISABLE_VK_VIDEO_PARSER_SIMD_OPTIMIZATIONS
}

HEADERS += \
    $$INC/NvVideoParser/ByteStreamParser.h            \
    $$INC/NvVideoParser/VulkanAV1Decoder.h            \
    $$INC/NvVideoParser/VulkanH264Decoder.h           \
    $$INC/NvVideoParser/VulkanH265Decoder.h           \
    $$INC/NvVideoParser/VulkanH26xDecoder.h           \
    $$INC/NvVideoParser/VulkanVideoDecoder.h          \
    $$INC/NvVideoParser/cpudetect.h                   \
    $$INC/NvVideoParser/nvVulkanVideoParser.h         \
    $$INC/NvVideoParser/nvVulkanVideoUtils.h          \
    $$INC/NvVideoParser/nvVulkanh264ScalingList.h     \
    $$INC/NvVideoParser/nvVulkanh265ScalingList.h     \
    $$INC/VkCodecUtils/VkVideoRefCountBase.h          \
    $$INC/VkCodecUtils/VulkanBitstreamBuffer.h        \
    $$INC/vkvideo_parser/PictureBufferBase.h          \
    $$INC/vkvideo_parser/StdVideoPictureParametersSet.h \
    $$INC/vkvideo_parser/VulkanVideoParser.h          \
    $$INC/vkvideo_parser/VulkanVideoParserIf.h        \
    $$INC/vkvideo_parser/VulkanVideoParserParams.h    \
    $$INC/vulkan_interfaces.h
