TEMPLATE = subdirs
SUBDIRS = \
    moonlight-common-c \
    qmdnsengine \
    app \
    h264bitstream

# §J.3.e.2.i.8 — H.265 native VK_KHR_video_decode parser (Apache 2.0)
nvvideoparser.subdir = 3rdparty/nvvideoparser
SUBDIRS += nvvideoparser

# Build the dependencies in parallel before the final app
app.depends = qmdnsengine moonlight-common-c h264bitstream nvvideoparser
win32:!winrt {
    SUBDIRS += AntiHooking
    app.depends += AntiHooking
}

# Support debug and release builds from command line for CI
CONFIG += debug_and_release

# Run our compile tests
load(configure)
qtCompileTest(SL)
qtCompileTest(EGL)
