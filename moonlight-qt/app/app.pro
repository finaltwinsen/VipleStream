QT += core quick network quickcontrols2 svg
CONFIG += c++17

# VipleStream §LNK4291：不連 Qt6EntryPoint.lib。Qt 官方 prebuilt 的那支沒帶 -guard:ehcont，
# 跟 globaldefs.pri 開的 CET/EHCont 旗標衝突，每次連結都 LNK4291（整個 exe 唯一沒有
# EH continuation 資料的模組）。WinMain 自帶在 main.cpp 檔尾（照 Qt qtentrypoint_win.cpp）。
win32: CONFIG -= entrypoint

unix:!macx {
    # VipleStream: was `moonlight`. Windows/Linux ship a `viplestream`
    # executable; on macOS the global menu bar shows `VipleStream`.
    TARGET = viplestream
} else {
    # On macOS, this is the name displayed in the global menu bar
    TARGET = VipleStream
}

include(../globaldefs.pri)

# Precompile QML files to avoid writing qmlcache on portable versions.
# Since this binds the app against the Qt runtime version, we will only
# do this for Windows and Mac (when disable-prebuilts is not defined),
# since they always ship with the matching build of the Qt runtime.
!disable-prebuilts {
    win32|macx {
        CONFIG(release, debug|release) {
            CONFIG += qtquickcompiler
        }
    }
}

# Maximize optimization for Release builds to improve render path performance
CONFIG(release, debug|release) {
    # MSVC: Whole Program Optimization (LTO) + intrinsics + fast FP + AVX2
    *-msvc {
        QMAKE_CXXFLAGS_RELEASE += /GL /Oi /fp:fast /arch:AVX2
        QMAKE_CFLAGS_RELEASE   += /GL /Oi /fp:fast /arch:AVX2
        QMAKE_LFLAGS_RELEASE   += /LTCG
    }
    # GCC/Clang (Linux/macOS): LTO + O3
    # §M2a：CONFIG+=disable-lto 只拿掉 LTO（-O3 保留）。給 build-steamframe.sh 的 dev
    # flavor 用：Flatpak 的 viplestream 模組是 dir 來源、每次整個重建，LTO link 不進
    # ccache，在 qemu（aarch64）下會把增量建置時間放大好幾倍。release 不帶這個旗標。
    else {
        !disable-lto {
            QMAKE_CXXFLAGS_RELEASE += -O3 -flto
            QMAKE_CFLAGS_RELEASE   += -O3 -flto
            QMAKE_LFLAGS_RELEASE   += -flto
        } else {
            QMAKE_CXXFLAGS_RELEASE += -O3
            QMAKE_CFLAGS_RELEASE   += -O3
        }
    }
}

TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

win32 {
    contains(QT_ARCH, x86_64) {
        LIBS += -L$$PWD/../libs/windows/lib/x64
        INCLUDEPATH += $$PWD/../libs/windows/include/x64 $$PWD/../libs/windows/include/x64/SDL2
    }
    contains(QT_ARCH, arm64) {
        LIBS += -L$$PWD/../libs/windows/lib/arm64
        INCLUDEPATH += $$PWD/../libs/windows/include/arm64 $$PWD/../libs/windows/include/arm64/SDL2
    }

    INCLUDEPATH += $$PWD/../libs/windows/include $$PWD/../libs/windows/nvofa/include
    # §HID-PROBE：backend/hidprobe.cpp 用 HidD_*（hid.lib）與 CM_Get_Device_Interface_ListW（cfgmgr32.lib）；
    # 兩者都是 Windows SDK 內建 import lib，runtime 的 hid.dll / cfgmgr32.dll 是系統元件，打包清單不用動。
    LIBS += ws2_32.lib winmm.lib dxva2.lib ole32.lib gdi32.lib user32.lib d3d9.lib dwmapi.lib dbghelp.lib hid.lib cfgmgr32.lib shell32.lib
    # VipleStream: DirectML FRUC backend. d3d12.lib and directml.lib
    # both ship with the Windows 10 SDK; the runtime DirectML.dll is
    # part of Windows 10 1903+ (no redist needed on modern systems).
    LIBS += d3d12.lib dxgi.lib directml.lib

    # VipleStream: ONNX Runtime with DirectML EP (x64 only). Headers
    # + static-link lib are committed under libs/windows/onnxruntime
    # (extracted from Microsoft.ML.OnnxRuntime.DirectML NuGet).
    # The runtime onnxruntime.dll gets copied into the release by
    # build_moonlight_package.cmd; it is loaded lazily on first use
    # so systems without it still launch (FRUC falls back to the
    # inline DML graph).
    contains(QT_ARCH, x86_64) {
        INCLUDEPATH += $$PWD/../libs/windows/onnxruntime/build/native/include
        LIBS        += -L$$PWD/../libs/windows/onnxruntime/runtimes/win-x64/native
        LIBS        += onnxruntime.lib
    }

    # VipleStream v1.3.x — NCNN with Vulkan EP for FRUC backend C
    # (cross-vendor RIFE that doesn't depend on ORT's DML EP heuristics).
    # Headers + import lib committed under libs/windows/ncnn (Tencent
    # ncnn-windows-vs2022-shared release, Vulkan-enabled). Runtime
    # ncnn.dll is copied into release by build_moonlight_package.cmd.
    # Loaded lazily on first use so systems without Vulkan loader
    # still launch (FRUC cascade falls through to DirectML / Generic).
    contains(QT_ARCH, x86_64) {
        INCLUDEPATH += $$PWD/../libs/windows/ncnn/build/native/include
        LIBS        += -L$$PWD/../libs/windows/ncnn/runtimes/win-x64/native
        LIBS        += ncnn.lib
        DEFINES     += VIPLESTREAM_HAVE_NCNN
        # Windows ncnn DLL is custom-patched with create_gpu_instance_external()
        # for §J.3.e.1.d external VkDevice handoff to libplacebo.  Linux stock
        # ncnn doesn't have this — falls back to plain create_gpu_instance().
        DEFINES     += VIPLE_NCNN_HAS_EXTERNAL_HANDOFF
        # NCNN handles Vulkan dispatch entirely inside ncnn.dll; no
        # need to link vulkan-1.lib at this layer. We may need it
        # later for shared-texture path (D3D12↔Vulkan handle import).
        #
        # NCNN's mat.h gates on POSIX-style preprocessor macros
        # (__SSE2__, __AVX__) that MSVC doesn't define by default.
        # x86_64 always has SSE2; AVX is opt-in via /arch:AVX.  Define
        # __SSE2__ unconditionally and expose AVX iff /arch:AVX was
        # set so the right intrinsic headers get included.
        DEFINES += __SSE2__=1
    }

    # §J.3.e.2.i.8 Phase 1.6 — NVIDIA Nsight Aftermath SDK (GPU crash dump
    # collection on device-lost / TDR).  Used to diagnose ONLY mode 24-78s
    # device-lost root cause that validation layer can't see (driver-internal
    # GPU fault).  SDK headers + lib + DLL committed under 3rdparty/aftermath_sdk/
    # (free download, NV Developer account required).  Disabled if SDK not
    # present (e.g. CI without dev tools).
    !disable-aftermath:exists($$PWD/../3rdparty/aftermath_sdk/lib/x64/GFSDK_Aftermath_Lib.x64.lib) {
        DEFINES     += VIPLESTREAM_HAVE_AFTERMATH
        INCLUDEPATH += $$PWD/../3rdparty/aftermath_sdk/include
        LIBS        += -L$$PWD/../3rdparty/aftermath_sdk/lib/x64
        LIBS        += GFSDK_Aftermath_Lib.x64.lib
    }
}
macx:!disable-prebuilts {
    INCLUDEPATH += $$PWD/../libs/mac/include $$PWD/../libs/mac/include/SDL2
    LIBS += -L$$PWD/../libs/mac/lib
}

unix:if(!macx|disable-prebuilts) {
    CONFIG += link_pkgconfig
    PKGCONFIG += openssl sdl2 SDL2_ttf

    # We have our own optimized libopus.a for Steam Link
    if(!config_SL|disable-prebuilts) {
        PKGCONFIG += opus
    }

    !disable-ffmpeg {
        packagesExist(libavcodec) {
            PKGCONFIG += libavcodec libavutil libswscale
            CONFIG += ffmpeg

            !disable-libva {
                packagesExist(libva) {
                    !disable-x11 {
                        packagesExist(libva-x11) {
                            CONFIG += libva-x11
                        }
                    }
                    !disable-wayland {
                        packagesExist(libva-wayland) {
                            CONFIG += libva-wayland
                        }
                    }
                    !disable-libdrm {
                        packagesExist(libva-drm) {
                            CONFIG += libva-drm
                        }
                    }
                    CONFIG += libva
                }
            }

            !disable-libvdpau {
                packagesExist(vdpau) {
                    CONFIG += libvdpau
                }
            }

            !disable-mmal {
                packagesExist(mmal) {
                    PKGCONFIG += mmal
                    CONFIG += mmal
                }
            }

            !disable-libdrm {
                packagesExist(libdrm) {
                    PKGCONFIG += libdrm
                    CONFIG += libdrm
                }
            }

            # Disabled by default due to reliability issues. See #1314.
            # CUDA interop is superseded by VDPAU and Vulkan Video.
            enable-cuda {
                packagesExist(ffnvcodec) {
                    PKGCONFIG += ffnvcodec
                    CONFIG += cuda
                }
            }

            !disable-libplacebo {
                packagesExist(libplacebo) {
                    PKGCONFIG += libplacebo
                    CONFIG += libplacebo
                }
            }

            # VipleStream §K.1 — ncnn (Vulkan EP) for PlVkRenderer's RIFE Phase B
            # + Generic FRUC override.  Linux 補幀主力是 VkFrucRenderer (vkfruc.cpp，
            # 無 ncnn 依賴)，但 plvk.cpp 有 149 處 ncnn 引用編譯依賴.  最小改動路：
            # 裝 ncnn from source 進 /usr/local (見 scripts/wsl_build_moonlight.sh)，
            # source 完全不動.  AppImage 多 ~9 MB；RIFE Phase B 在 Linux 上技術上
            # 可用，但 stream pipeline 預設仍走 VkFrucRenderer.
            #
            # §M2a（F8）：安裝位置改由 NCNN_PREFIX 指定（預設 /usr/local，與舊行為相同）。
            # build-appimage-native.sh 傳 $HOME/.local/ncnn、Flatpak 傳 /app；以前腳本有傳
            # 但這裡根本沒讀，是靠 builder 上 /usr/local 的 symlink 才建得起來。
            isEmpty(NCNN_PREFIX): NCNN_PREFIX = /usr/local
            !disable-ncnn:exists($$NCNN_PREFIX/include/ncnn/mat.h) {
                message(NCNN found at $$NCNN_PREFIX — RIFE Phase B available)
                DEFINES     += VIPLESTREAM_HAVE_NCNN
                INCLUDEPATH += $$NCNN_PREFIX/include
                LIBS        += -L$$NCNN_PREFIX/lib -lncnn
            }

            # plvk.cpp／vkfruc.cpp／rife_native_vk.cpp 無條件 include 並呼叫 ncnn：有
            # libplacebo 而沒有 ncnn 時，與其讓編譯噴一長串 include 錯誤，不如在 qmake 階段講清楚。
            libplacebo:!contains(DEFINES, VIPLESTREAM_HAVE_NCNN) {
                error("libplacebo was found but ncnn was not: $$NCNN_PREFIX/include/ncnn/mat.h is missing or CONFIG+=disable-ncnn is set. The Vulkan renderers plvk/vkfruc/rife_native_vk require ncnn - install it and pass NCNN_PREFIX=<install prefix> to qmake, or build with CONFIG+=disable-libplacebo.")
            }
        }

        !disable-wayland {
            packagesExist(wayland-client) {
                CONFIG += wayland
                PKGCONFIG += wayland-client
            }
        }

        !disable-x11 {
            packagesExist(x11) {
                DEFINES += HAS_X11
                PKGCONFIG += x11
            }
        }
    }
}
win32 {
    LIBS += -llibssl -llibcrypto -lSDL2 -lSDL2_ttf -lavcodec -lavutil -lswscale -lopus -ldxgi -ld3d11 -llibplacebo
    CONFIG += ffmpeg libplacebo
}
win32:!winrt {
    CONFIG += discord-rpc
}
macx {
    !disable-prebuilts {
        LIBS += -lssl.3 -lcrypto.3 -lavcodec.62 -lavutil.60 -lswscale.9 -lopus -lSDL2 -lSDL2_ttf
        CONFIG += discord-rpc
    }

    LIBS += -lobjc -framework VideoToolbox -framework AVFoundation -framework CoreVideo -framework CoreGraphics -framework CoreMedia -framework AppKit -framework Metal -framework QuartzCore
    CONFIG += ffmpeg
}

SOURCES += \
    backend/nvaddress.cpp \
    backend/nvapp.cpp \
    cli/pair.cpp \
    main.cpp \
    backend/computerseeker.cpp \
    backend/relaylookup.cpp \
    backend/relaytcptunnel.cpp \
    backend/relayudptunnel.cpp \
    backend/identitymanager.cpp \
    backend/nvcomputer.cpp \
    backend/nvhttp.cpp \
    backend/nvpairingmanager.cpp \
    backend/computermanager.cpp \
    backend/boxartmanager.cpp \
    backend/richpresencemanager.cpp \
    cli/commandlineparser.cpp \
    cli/frucoffline.cpp \
    cli/listapps.cpp \
    cli/quitstream.cpp \
    cli/startstream.cpp \
    settings/compatfetcher.cpp \
    settings/mappingfetcher.cpp \
    settings/streamingpreferences.cpp \
    streaming/input/abstouch.cpp \
    streaming/input/gamepad.cpp \
    streaming/input/input.cpp \
    streaming/input/keyboard.cpp \
    streaming/input/mouse.cpp \
    streaming/input/reltouch.cpp \
    streaming/input/sc_hid.cpp \
    streaming/session.cpp \
    streaming/audio/audio.cpp \
    streaming/audio/renderers/sdlaud.cpp \
    gui/computermodel.cpp \
    gui/appmodel.cpp \
    streaming/bandwidth.cpp \
    streaming/streamutils.cpp \
    backend/autoupdatechecker.cpp \
    backend/updater.cpp \
    path.cpp \
    settings/mappingmanager.cpp \
    gui/sdlgamepadkeynavigation.cpp \
    streaming/video/overlaymanager.cpp \
    streaming/transfer/filetransferclient.cpp \
    streaming/vr/vrlaunchparams.cpp \
    streaming/vr/vrsynthetic.cpp \
    streaming/vr/vrtracking.cpp \
    streaming/vr/vrframemeta.cpp \
    backend/systemproperties.cpp \
    backend/hidprobe.cpp \
    wm.cpp

# VipleStream 2.0 §SF-PROBE／§SF-ENV（M2a）— Steam Frame 探測動作與執行環境摘要。
# 一律編譯：非 Linux／沒有 FFmpeg／沒有 OpenXR 的建置由各檔自己回 stub（結束碼 10）。
# bitstreamdump.cpp 也放這裡：StreamCommandLineParser 的 --dump-bitstream 在每個平台
# 都會呼叫 BitstreamDump::setPath()，實際寫檔只在 ffmpeg.cpp 呼叫。
SOURCES += \
    backend/sfenv.cpp \
    cli/probeutil.cpp \
    cli/v4l2probe.cpp \
    cli/decodebench.cpp \
    cli/xrprobe.cpp \
    streaming/video/bitstreamdump.cpp \
    streaming/video/v4l2/v4l2caps.cpp \
    streaming/xr/xrruntimejson.cpp

HEADERS += \
    backend/sfenv.h \
    cli/probeutil.h \
    cli/v4l2probe.h \
    cli/decodebench.h \
    cli/xrprobe.h \
    streaming/video/bitstreamdump.h \
    streaming/video/v4l2/v4l2caps.h \
    streaming/xr/xrruntimejson.h

# 上面幾個檔在 Linux 會 dlopen（Vulkan loader、OpenXR runtime 的診斷）。glibc 2.34 起
# dlopen 已併入 libc，-ldl 只是保險：舊 glibc 需要它，新 glibc 的 libdl 是空殼、無害。
linux: LIBS += -ldl

HEADERS += \
    SDL_compat.h \
    backend/nvaddress.h \
    backend/nvapp.h \
    cli/pair.h \
    settings/compatfetcher.h \
    settings/mappingfetcher.h \
    utils.h \
    backend/computerseeker.h \
    backend/relaylookup.h \
    backend/relaytcptunnel.h \
    backend/relayudptunnel.h \
    backend/identitymanager.h \
    backend/nvcomputer.h \
    backend/nvhttp.h \
    backend/nvpairingmanager.h \
    backend/computermanager.h \
    backend/boxartmanager.h \
    backend/richpresencemanager.h \
    cli/commandlineparser.h \
    cli/frucoffline.h \
    cli/listapps.h \
    cli/quitstream.h \
    cli/startstream.h \
    settings/streamingpreferences.h \
    streaming/input/input.h \
    streaming/session.h \
    streaming/audio/renderers/renderer.h \
    streaming/audio/renderers/sdl.h \
    gui/computermodel.h \
    gui/appmodel.h \
    streaming/video/decoder.h \
    streaming/bandwidth.h \
    streaming/streamutils.h \
    backend/autoupdatechecker.h \
    backend/updater.h \
    backend/updateassetrules.h \
    path.h \
    settings/mappingmanager.h \
    gui/sdlgamepadkeynavigation.h \
    streaming/video/overlaymanager.h \
    streaming/transfer/filetransferclient.h \
    streaming/vr/vrlaunchparams.h \
    streaming/vr/vrsynthetic.h \
    streaming/vr/vrtracking.h \
    streaming/vr/vrframemeta.h \
    backend/systemproperties.h \
    backend/hidprobe.h

# Platform-specific renderers and decoders
ffmpeg {
    message(FFmpeg decoder selected)

    DEFINES += HAVE_FFMPEG
    SOURCES += \
        streaming/video/ffmpeg.cpp \
        streaming/video/ffmpeg-renderers/genhwaccel.cpp \
        streaming/video/ffmpeg-renderers/sdlvid.cpp \
        streaming/video/ffmpeg-renderers/swframemapper.cpp \
        streaming/video/ffmpeg-renderers/pacer/pacer.cpp

    HEADERS += \
        streaming/video/ffmpeg.h \
        streaming/video/ffmpeg-renderers/renderer.h \
        streaming/video/ffmpeg-renderers/genhwaccel.h \
        streaming/video/ffmpeg-renderers/sdlvid.h \
        streaming/video/ffmpeg-renderers/swframemapper.h \
        streaming/video/ffmpeg-renderers/pacer/pacer.h
}
libva {
    message(VAAPI renderer selected)

    PKGCONFIG += libva
    DEFINES += HAVE_LIBVA
    SOURCES += streaming/video/ffmpeg-renderers/vaapi.cpp \
               streaming/video/ffmpeg-renderers/vaapi_vk_bridge.cpp
    HEADERS += streaming/video/ffmpeg-renderers/vaapi.h \
               streaming/video/ffmpeg-renderers/vaapi_vk_bridge.h
}
libva-x11 {
    message(VAAPI X11 support enabled)

    PKGCONFIG += libva-x11
    DEFINES += HAVE_LIBVA_X11
}
libva-wayland {
    message(VAAPI Wayland support enabled)

    PKGCONFIG += libva-wayland
    DEFINES += HAVE_LIBVA_WAYLAND
}
libva-drm {
    message(VAAPI DRM support enabled)

    PKGCONFIG += libva-drm
    DEFINES += HAVE_LIBVA_DRM
}
libvdpau {
    message(VDPAU renderer selected)

    DEFINES += HAVE_LIBVDPAU
    SOURCES += streaming/video/ffmpeg-renderers/vdpau.cpp
    HEADERS += streaming/video/ffmpeg-renderers/vdpau.h
}
mmal {
    message(MMAL renderer selected)

    DEFINES += HAVE_MMAL
    SOURCES += streaming/video/ffmpeg-renderers/mmal.cpp
    HEADERS += streaming/video/ffmpeg-renderers/mmal.h

    # We suppress EGL usage when MMAL is available because MMAL has
    # significantly better performance than EGL on the Pi. Setting
    # this option allows EGL usage even if built with MMAL support.
    #
    # It is highly recommended to also build with 'gpuslow' to avoid
    # EGL being preferred if direct DRM rendering is available.
    allow-egl-with-mmal {
        message(Allowing EGL usage with MMAL enabled)

        DEFINES += ALLOW_EGL_WITH_MMAL
    }
}
libdrm {
    message(DRM renderer selected)

    DEFINES += HAVE_DRM
    SOURCES += streaming/video/ffmpeg-renderers/drm.cpp
    HEADERS += streaming/video/ffmpeg-renderers/drm.h

    linux {
        !disable-masterhooks {
            message(Master hooks enabled)
            DEFINES += HAVE_DRM_MASTER_HOOKS
            SOURCES += masterhook.c masterhook_internal.c
            LIBS += -ldl -pthread
        }
    }
}
cuda {
    message(CUDA support enabled)

    DEFINES += HAVE_CUDA
    SOURCES += streaming/video/ffmpeg-renderers/cuda.cpp
    HEADERS += streaming/video/ffmpeg-renderers/cuda.h

    # ffnvcodec uses libdl in cuda_load_functions()/cuda_free_functions()
    LIBS += -ldl
}
libplacebo {
    message(Vulkan support enabled via libplacebo)

    DEFINES += HAVE_LIBPLACEBO_VULKAN
    SOURCES += \
        streaming/video/ffmpeg-renderers/plvk.cpp \
        streaming/video/ffmpeg-renderers/plvk_c.c \
        streaming/video/ffmpeg-renderers/vkfruc.cpp \
        streaming/video/ffmpeg-renderers/vkfruc-decode.cpp \
        streaming/video/ffmpeg-renderers/vkfruc-aftermath.cpp \
        streaming/video/ffmpeg-renderers/ncnn_rife_warp.cpp \
        streaming/video/ffmpeg-renderers/rife_native_vk.cpp \
        streaming/video/ffmpeg-renderers/modelfetcher.cpp
    HEADERS += \
        streaming/video/ffmpeg-renderers/plvk.h \
        streaming/video/ffmpeg-renderers/vkfruc.h \
        streaming/video/ffmpeg-renderers/vkfruc-decode.h \
        streaming/video/ffmpeg-renderers/vkfruc-aftermath.h \
        streaming/video/ffmpeg-renderers/ncnn_rife_warp.h \
        streaming/video/ffmpeg-renderers/rife_native_vk.h \
        streaming/video/ffmpeg-renderers/modelfetcher.h

    # VipleStream §K.X — vkfruc.cpp #include "nvOpticalFlowVulkan.h" needs
    # the NVIDIA Optical Flow SDK header on the include path.  win32 block
    # already adds it (line 66), but the libplacebo path also gets compiled
    # on Linux (WSL/native) so add it here for unix builds too.  Header is
    # portable typedef-only (no Win32-specific definitions inside; the
    # only #ifdef _WIN32 in nvOpticalFlowCommon.h is calling-convention
    # NVOFAPI = __stdcall vs empty).  Runtime LoadLibraryA is already
    # gated by Q_OS_WIN32 in vkfruc.cpp::loadNvOfApi (line 1081); Linux
    # path returns false with "not yet wired" warning, env var defaults
    # off so users won't notice.  See memory `reference_linux_build_pipeline.md`.
    unix:!macx: INCLUDEPATH += $$PWD/../libs/windows/nvofa/include

    # VipleStream §K.X — vkfruc.cpp #include "ncnn/stb_image_write.h" for the
    # §B-DUMP diagnostic frame writer.  Windows ships this via the ncnn
    # NuGet/prebuilt under libs/windows/ncnn/build/native/include/ncnn/
    # (already on INCLUDEPATH via the win32 generic libs/windows/include).
    # On Linux the apt/source-built ncnn does NOT install stb_image_write.h
    # to /usr/local/include/ncnn/ (it's a build-dir internal header).  Mirror
    # the Windows-shipped copy onto unix INCLUDEPATH; wsl_sync_to_ext4.sh
    # rsyncs the headers across.  stb_image_write is a single-header library,
    # zero portability concern.
    # On Linux do NOT add libs/windows/ncnn to INCLUDEPATH: the Windows-shipped
    # headers use __declspec(dllimport) (broken under gcc). Use the system
    # libstb-dev + system ncnn instead. Build prereq for vkfruc §B-DUMP's
    # <ncnn/stb_image_write.h> on Linux:
    #   sudo apt-get install libstb-dev
    #   sudo ln -s /usr/include/stb /usr/include/ncnn   # safe — system ncnn
    #                                                     lives in /usr/local
    # On Windows the libs/windows/include path (already added higher up) gives
    # vkfruc the bundled <ncnn/stb_image_write.h> via the ncnn nuget.
}
config_EGL {
    message(EGL renderer selected)

    CONFIG += egl
    DEFINES += HAVE_EGL
    SOURCES += \
        streaming/video/ffmpeg-renderers/eglvid.cpp \
        streaming/video/ffmpeg-renderers/egl_extensions.cpp \
        streaming/video/ffmpeg-renderers/eglimagefactory.cpp
    HEADERS += \
        streaming/video/ffmpeg-renderers/eglvid.h \
        streaming/video/ffmpeg-renderers/eglimagefactory.h
}
config_SL {
    message(Steam Link build configuration selected)

    !disable-prebuilts {
        # Link against our NEON-optimized libopus build
        LIBS += -L$$PWD/../libs/steamlink/lib
        INCLUDEPATH += $$PWD/../libs/steamlink/include
        LIBS += -lopus -larmasm -lNE10
    }

    DEFINES += EMBEDDED_BUILD STEAM_LINK HAVE_SLVIDEO HAVE_SLAUDIO
    LIBS += -lSLVideo -lSLAudio

    SOURCES += \
        streaming/video/slvid.cpp \
        streaming/audio/renderers/slaud.cpp
    HEADERS += \
        streaming/video/slvid.h \
        streaming/audio/renderers/slaud.h
}
win32 {
    HEADERS += streaming/video/ffmpeg-renderers/dxutil.h
}
win32:!winrt {
    message(DXVA2 and D3D11VA renderers selected)

    SOURCES += \
        streaming/video/ffmpeg-renderers/dxva2.cpp \
        streaming/video/ffmpeg-renderers/d3d11va.cpp \
        streaming/video/ffmpeg-renderers/d3d11_vk_bridge.cpp \
        streaming/video/ffmpeg-renderers/nvofruc.cpp \
        streaming/video/ffmpeg-renderers/genericfruc.cpp \
        streaming/video/ffmpeg-renderers/directmlfruc.cpp \
        streaming/video/ffmpeg-renderers/ncnnfruc.cpp \
        streaming/video/ffmpeg-renderers/vulkanvideo.cpp \
        streaming/video/ffmpeg-renderers/pacer/dxvsyncsource.cpp

    HEADERS += \
        streaming/video/ffmpeg-renderers/dxva2.h \
        streaming/video/ffmpeg-renderers/d3d11va.h \
        streaming/video/ffmpeg-renderers/d3d11_vk_bridge.h \
        streaming/video/ffmpeg-renderers/nvofruc.h \
        streaming/video/ffmpeg-renderers/genericfruc.h \
        streaming/video/ffmpeg-renderers/directmlfruc.h \
        streaming/video/ffmpeg-renderers/ncnnfruc.h \
        streaming/video/ffmpeg-renderers/vulkanvideo.h \
        streaming/video/ffmpeg-renderers/ifrucbackend.h \
        streaming/video/ffmpeg-renderers/pacer/dxvsyncsource.h
}
macx {
    message(VideoToolbox renderer selected)

    SOURCES += \
        streaming/video/ffmpeg-renderers/vt_base.mm \
        streaming/video/ffmpeg-renderers/vt_avsamplelayer.mm \
        streaming/video/ffmpeg-renderers/vt_metal.mm

    HEADERS += \
        streaming/video/ffmpeg-renderers/vt.h
}
discord-rpc {
    message(Discord integration enabled)

    LIBS += -ldiscord-rpc
    DEFINES += HAVE_DISCORD
}
embedded {
    message(Embedded build)

    DEFINES += EMBEDDED_BUILD
}
glslow {
    message(GL slow build)

    DEFINES += GL_IS_SLOW
}
vkslow {
    message(Vulkan slow build)

    DEFINES += VULKAN_IS_SLOW
}
gpuslow {
    message(GPU slow build)

    DEFINES += GL_IS_SLOW VULKAN_IS_SLOW
}
wayland {
    message(Wayland extensions enabled)

    DEFINES += HAS_WAYLAND
    SOURCES += streaming/video/ffmpeg-renderers/pacer/waylandvsyncsource.cpp
    HEADERS += streaming/video/ffmpeg-renderers/pacer/waylandvsyncsource.h
}
# VipleStream 2.0 §SF-PROBE（M2a）— OpenXR loader（xr-probe 的 A 段）。
# 刻意 opt-in、不自動偵測：只有 build-steamframe.sh（Flatpak，兩種 arch）帶 CONFIG+=openxr。
# x64 AppImage 不帶，即使 builder 裝了 libopenxr-dev 也不會意外連上 loader。
# Windows 的 OpenXR 留到 M3a。
unix:!macx:openxr {
    message(OpenXR loader enabled)

    PKGCONFIG += openxr
    DEFINES += HAVE_OPENXR
    SOURCES += streaming/xr/xrprobe_instance.cpp
    HEADERS += streaming/xr/xrprobe_instance.h
}

RESOURCES += \
    resources.qrc \
    qml.qrc

TRANSLATIONS += \
    languages/qml_zh_CN.ts \
    languages/qml_de.ts \
    languages/qml_fr.ts \
    languages/qml_nb_NO.ts \
    languages/qml_ru.ts \
    languages/qml_es.ts \
    languages/qml_ja.ts \
    languages/qml_vi.ts \
    languages/qml_th.ts \
    languages/qml_ko.ts \
    languages/qml_hu.ts \
    languages/qml_nl.ts \
    languages/qml_sv.ts \
    languages/qml_tr.ts \
    languages/qml_uk.ts \
    languages/qml_zh_TW.ts \
    languages/qml_el.ts \
    languages/qml_hi.ts \
    languages/qml_it.ts \
    languages/qml_pt.ts \
    languages/qml_pt_BR.ts \
    languages/qml_pl.ts \
    languages/qml_cs.ts \
    languages/qml_he.ts \
    languages/qml_ckb.ts \
    languages/qml_lt.ts \
    languages/qml_et.ts \
    languages/qml_bg.ts \
    languages/qml_eo.ts \
    languages/qml_ta.ts

# Additional import path used to resolve QML modules in Qt Creator's code model
QML_IMPORT_PATH =

# Additional import path used to resolve QML modules just for Qt Quick Designer
QML_DESIGNER_IMPORT_PATH =

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../moonlight-common-c/release/ -lmoonlight-common-c
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../moonlight-common-c/debug/ -lmoonlight-common-c
else:unix: LIBS += -L$$OUT_PWD/../moonlight-common-c/ -lmoonlight-common-c

INCLUDEPATH += $$PWD/../moonlight-common-c/moonlight-common-c/src
DEPENDPATH += $$PWD/../moonlight-common-c/moonlight-common-c/src

# VipleStream §Q: link picoquic + picotls when MP-QUIC is enabled.
# Build via build-tools\build_picoquic_client.cmd（build_moonlight.cmd 會先呼叫；一律 Release）。
# 2026-09-19 教訓：這個 build 目錄曾被手動用預設 Debug（/MDd /Od）建出來 → 連結時 LNK4098
# （MSVCRTD 與 MSVCRT 衝突），而且 client 的 MP-QUIC 整條路徑跑的是未最佳化碼。
# Order matters: picoquic-core first, then picotls-* (picoquic depends on picotls).
#
# §M2a（F8）：兩個目錄都可從 qmake 命令列覆寫（PICOQUIC_BUILD=… PICOTLS_LIBDIR=…），
# 預設值與舊版相同 → Windows／AppImage 行為不變。Flatpak 在 sandbox 內另建 picoquic
# （build-viplestream.sh 傳自己的 build 目錄）。picotls-fusion 只在 x86_64 且編譯器支援
# AES-NI 時才會產生（aarch64 沒有，picoquic 會自動定義 PTLS_WITHOUT_FUSION），所以依
# 實際檔案存在與否決定要不要連；前後順序維持 openssl → core → fusion → minicrypto。
contains(DEFINES, VIPLE_MPQUIC) {
    isEmpty(PICOQUIC_BUILD): PICOQUIC_BUILD = $$PWD/../../Sunshine/third-party/picoquic/build
    isEmpty(PICOTLS_LIBDIR): PICOTLS_LIBDIR = $$PICOQUIC_BUILD/_deps/picotls-build
    LIBS += -L$$PICOQUIC_BUILD -lpicoquic-core
    LIBS += -L$$PICOTLS_LIBDIR -lpicotls-openssl -lpicotls-core
    exists($$PICOTLS_LIBDIR/libpicotls-fusion.a)|exists($$PICOTLS_LIBDIR/picotls-fusion.lib) {
        LIBS += -lpicotls-fusion
    }
    LIBS += -lpicotls-minicrypto
    unix:!macx: LIBS += -lssl -lcrypto
    win32 {
        VCPKG_LIB = $$(VCPKG_ROOT)/installed/x64-windows/lib
        LIBS += -L$$VCPKG_LIB -llibssl -llibcrypto
        LIBS += -lws2_32 -lbcrypt -lcrypt32 -ladvapi32
        INCLUDEPATH += $$(VCPKG_ROOT)/installed/x64-windows/include
    }
}

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../qmdnsengine/release/ -lqmdnsengine
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../qmdnsengine/debug/ -lqmdnsengine
else:unix: LIBS += -L$$OUT_PWD/../qmdnsengine/ -lqmdnsengine

INCLUDEPATH += $$PWD/../qmdnsengine/qmdnsengine/src/include $$PWD/../qmdnsengine
DEPENDPATH += $$PWD/../qmdnsengine/qmdnsengine/src/include $$PWD/../qmdnsengine

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../h264bitstream/release/ -lh264bitstream
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../h264bitstream/debug/ -lh264bitstream
else:unix: LIBS += -L$$OUT_PWD/../h264bitstream/ -lh264bitstream

INCLUDEPATH += $$PWD/../h264bitstream/h264bitstream
DEPENDPATH += $$PWD/../h264bitstream/h264bitstream

# §J.3.e.2.i.8 — H.265 native VK_KHR_video_decode parser (Apache 2.0)
win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../3rdparty/nvvideoparser/release/ -lnvvideoparser
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../3rdparty/nvvideoparser/debug/ -lnvvideoparser
else:unix: LIBS += -L$$OUT_PWD/../3rdparty/nvvideoparser/ -lnvvideoparser

# 4 條 INCLUDEPATH match nvvideoparser.pro 的設定 — 上游 #include 用
# bare angle-bracket (e.g. <cpudetect.h>) 跟 quoted (e.g. "VulkanVideoParserIf.h")
# 都需要對應 sub-dir 在 INCLUDEPATH.
INCLUDEPATH += \
    $$PWD/../3rdparty/nvvideoparser/include \
    $$PWD/../3rdparty/nvvideoparser/include/NvVideoParser \
    $$PWD/../3rdparty/nvvideoparser/include/vkvideo_parser \
    $$PWD/../3rdparty/nvvideoparser/include/VkCodecUtils
DEPENDPATH += $$PWD/../3rdparty/nvvideoparser/include

!winrt {
    win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../AntiHooking/release/ -lAntiHooking
    else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../AntiHooking/debug/ -lAntiHooking

    INCLUDEPATH += $$PWD/../AntiHooking
    DEPENDPATH += $$PWD/../AntiHooking
}

unix:!macx: {
    isEmpty(PREFIX) {
        PREFIX = /usr/local
    }
    isEmpty(BINDIR) {
        BINDIR = bin
    }
    isEmpty(DATADIR) {
        DATADIR = share
    }

    target.path = $$PREFIX/$$BINDIR/

    desktop.files = deploy/linux/viplestream.desktop
    desktop.path = $$PREFIX/$$DATADIR/applications/

    # §K.4-DESKTOP-ID：icon theme 內的名稱必須等於 .desktop 的
    # Icon=viplestream，否則 GNOME app grid / dock 查不到圖示（歷史 bug：
    # 這裡只裝原名 moonlight.svg，Icon=viplestream 永遠解析失敗）。qmake
    # INSTALLS 不支援安裝時改名，用 .extra 複製改名；moonlight.svg 原名
    # 續裝一份給舊 .desktop 相容。
    ICON_APPS_DIR = $$PREFIX/$$DATADIR/icons/hicolor/scalable/apps
    icons.files = res/moonlight.svg
    icons.path = $$ICON_APPS_DIR/
    icons.extra = mkdir -p $(INSTALL_ROOT)$$ICON_APPS_DIR && cp -f $$PWD/res/moonlight.svg $(INSTALL_ROOT)$$ICON_APPS_DIR/viplestream.svg
    icons.uninstall = rm -f $(INSTALL_ROOT)$$ICON_APPS_DIR/viplestream.svg

    appstream.files = deploy/linux/viplestream.appdata.xml
    appstream.path = $$PREFIX/$$DATADIR/metainfo/

    # VipleStream v1.4.143 — Linux AppImage 必須裝 rife model 才能跑 RIFE-β
    # 補幀 path. 之前漏掉, runtime Path::getDataFilePath("rife-v4.25-lite/
    # flownet.param") 全部 fallback miss 落到 ":/data/" QRC 路徑, 但 model
    # 也沒進 resources.qrc 所以 parseParam 失敗 → 整個 RIFE-β 不可用 →
    # 退到 block-match path (低一檔的補幀品質).  裝到 /usr/share/VipleStream/
    # 這個位置, 因為 QCoreApplication::setApplicationName("VipleStream") 後
    # QStandardPaths::AppDataLocation 會找這條路徑; AppImage mount 後路徑
    # 對應 $APPDIR/usr/share/VipleStream/rife-v4.25-lite/.
    rife_models.files = rife_models/rife-v4.25-lite
    rife_models.path = $$PREFIX/$$DATADIR/VipleStream/

    INSTALLS += target desktop icons appstream rife_models
}
win32 {
    RC_ICONS = moonlight.ico
    QMAKE_TARGET_COMPANY = VipleStream
    QMAKE_TARGET_DESCRIPTION = VipleStream Game Streaming Client
    QMAKE_TARGET_PRODUCT = VipleStream Client

    CONFIG -= embed_manifest_exe
    QMAKE_LFLAGS += /MANIFEST:embed /MANIFESTINPUT:$${PWD}/Moonlight.exe.manifest
}
macx {
    # Create Info.plist in object dir with the correct version string
    system(cp $$PWD/Info.plist $$OUT_PWD/Info.plist)
    system(sed -i -e 's/VERSION/$$cat(version.txt)/g' $$OUT_PWD/Info.plist)

    QMAKE_INFO_PLIST = $$OUT_PWD/Info.plist

    APP_BUNDLE_RESOURCES.files = moonlight.icns
    APP_BUNDLE_RESOURCES.path = Contents/Resources

    APP_BUNDLE_PLIST.files = $$OUT_PWD/Info.plist
    APP_BUNDLE_PLIST.path = Contents

    QMAKE_BUNDLE_DATA += APP_BUNDLE_RESOURCES APP_BUNDLE_PLIST

    !disable-prebuilts {
        APP_BUNDLE_FRAMEWORKS.files = $$files(../libs/mac/Frameworks/*.framework, true) $$files(../libs/mac/lib/*.dylib, true)
        APP_BUNDLE_FRAMEWORKS.path = Contents/Frameworks

        QMAKE_BUNDLE_DATA += APP_BUNDLE_FRAMEWORKS

        QMAKE_RPATHDIR += @executable_path/../Frameworks
    }
}

VERSION = "$$cat(version.txt)"

# Generate version_string.h from version.txt via QMAKE_SUBSTITUTES.
#
# Two qmake quirks we work around here:
#   1. QMAKE_SUBSTITUTES strips outer "..." that surrounds a $${VAR}
#      placeholder in the template, so naive `"$${VAR}"` ends up as
#      the bare value with no quotes. We pre-bake the quotes into the
#      variable value itself ("\"…\""), then place the variable
#      unsurrounded in the template so qmake leaves it alone.
#   2. The extended-form ".input/.output = ..." silently falls back to
#      writing into PWD on this qmake build. Use the simple form and
#      gitignore the generated file (see .gitignore at repo root).
#
# See version_string.h.in for why this generated header exists at all
# (command-line -D macros do not retrigger nmake recompiles; generated
# headers do).
VERSION_STR_VALUE = "\"$$cat(version.txt)\""
QMAKE_SUBSTITUTES += version_string.h.in

# VipleStream §K.4-DESKTOP-ID（M2a W2）：desktop id 由 qmake 產生 desktop_id.h 注入
# （main.cpp 的 setDesktopFileName 與 SDL Wayland app id 都用它）。預設 viplestream
# （= binary 名 = viplestream.desktop，AppImage／.deb／Windows 行為不變）；Flatpak 的
# build-viplestream.sh 傳 VIPLE_DESKTOP_ID=$FLATPAK_ID（io.github.finaltwinsen.VipleStream），
# 配合 manifest 的 rename-desktop-file 讓 Wayland app_id 對上改名後的 .desktop。
# 做法照上面的 version_string.h：命令列 -D 巨集不會觸發 nmake 重編，產生式 header 會；
# 引號同樣預先放進變數值（QMAKE_SUBSTITUTES 會吃掉樣板裡包住 $${VAR} 的引號）。
# 值會原樣進 C 字串與 Wayland app_id，所以只允許 [A-Za-z0-9._-]，其他一律 error。
isEmpty(VIPLE_DESKTOP_ID): VIPLE_DESKTOP_ID = viplestream
!count(VIPLE_DESKTOP_ID, 1)|!contains(VIPLE_DESKTOP_ID, "[A-Za-z0-9._-]+") {
    error("VIPLE_DESKTOP_ID must be a single token matching [A-Za-z0-9._-]+ - got: $$VIPLE_DESKTOP_ID")
}
VIPLE_DESKTOP_ID_VALUE = "\"$$VIPLE_DESKTOP_ID\""
QMAKE_SUBSTITUTES += desktop_id.h.in
