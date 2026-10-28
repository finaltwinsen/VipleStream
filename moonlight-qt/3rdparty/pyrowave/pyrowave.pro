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

include($$PWD/pyrowave.pri)
