QT -= core gui

TARGET = pyrowave
TEMPLATE = lib

CONFIG += staticlib
CONFIG += warn_off

include(../../globaldefs.pri)

CONFIG += c++17

DEFINES += VK_NO_PROTOTYPES
win32 {
    DEFINES += VK_USE_PLATFORM_WIN32_KHR NOMINMAX _CRT_SECURE_NO_WARNINGS
}

# Granite 的 SIMD（math/simd.hpp）在 x86 要求 __SSE3__；GCC／Clang 的 x86_64 預設只到 SSE2。
# 同 CMakeLists.txt 的 -msse3。MSVC 走 intrin.h、ARM 走 NEON，不受影響。
!win32 {
    contains(QT_ARCH, x86_64)|contains(QT_ARCH, i386) {
        QMAKE_CFLAGS += -msse3
        QMAKE_CXXFLAGS += -msse3
    }
}

include($$PWD/pyrowave.pri)
