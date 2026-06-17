// VipleStream §FRUC-VALIDATE — 離線 FRUC 品質量測（dev-only）實作。
// 設計見 frucoffline.h 與 docs 計畫。流程：
//   1. 設 VIPLE_VKFRUC_DUMP_DIR（必須在 new VkFrucRenderer 之前——dump dir
//      會自動觸發 m_SwMode + FRUC + DUAL + dump，見 vkfruc.cpp:263）。
//   2. 建隱藏 SDL Vulkan window → new VkFrucRenderer(0) → initialize()。
//   3. 讀 PNG 序列 → sws_scale RGB→NV12（BT.601 limited，對齊 renderer 預設）
//      → renderFrame()。real/interp 幀由 §B-DUMP 寫成 BMP。
//   4. flush dump、teardown。

#include "frucoffline.h"
#include "commandlineparser.h"

#include <QByteArray>
#include <QDir>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <SDL.h>

#include <cstdio>

#ifdef HAVE_LIBPLACEBO_VULKAN
#include "streaming/video/decoder.h"                       // PDECODER_PARAMETERS + VIDEO_FORMAT_*
#include "settings/streamingpreferences.h"                 // VideoDecoderSelection
#include "streaming/video/ffmpeg-renderers/vkfruc.h"       // VkFrucRenderer

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

int runFrucOffline(const FrucOfflineCommandLineParser& args)
{
#ifndef HAVE_LIBPLACEBO_VULKAN
    (void)args;
    fprintf(stderr, "[fruc-offline] built without HAVE_LIBPLACEBO_VULKAN — "
                    "VkFrucRenderer unavailable on this build\n");
    return 10;
#else
    const QString inputDir = args.getInputDir();
    const QString dumpDir  = args.getDumpDir();
    const int width  = args.getWidth();
    const int height = args.getHeight();

    // 列出輸入幀（PNG / BMP，依檔名排序）。
    QDir dir(inputDir);
    QStringList frames = dir.entryList(QStringList() << "*.png" << "*.bmp",
                                       QDir::Files, QDir::Name);
    if (frames.isEmpty()) {
        fprintf(stderr, "[fruc-offline] no PNG/BMP frames in %s\n",
                qPrintable(inputDir));
        return 11;
    }
    QDir().mkpath(dumpDir);
    fprintf(stderr, "[fruc-offline] %d input frames, %dx%d -> dump %s\n",
            (int)frames.size(), width, height, qPrintable(dumpDir));

    // ★ 順序鐵律：在 new VkFrucRenderer 之前設 env。ctor 在 m_SwMode 讀
    //   VIPLE_VKFRUC_DUMP_DIR（vkfruc.cpp:263），設了才會走 SW + FRUC + dump。
    qputenv("VIPLE_VKFRUC_DUMP_DIR", dumpDir.toUtf8());
    qputenv("VIPLE_VKFRUC_DUMP_FRAMES", QByteArray::number((int)frames.size() * 2 + 16));
    // vkfruc 只在 delay>0 時才覆寫預設 10000ms（vkfruc.cpp:2877）；設 1ms 近即時開始 dump，
    // 否則離線 feed 迴圈 ~1 秒就結束、遠在 10 秒 delay 前 → 完全 dump 不到。
    qputenv("VIPLE_VKFRUC_DUMP_DELAY_MS", "1");
    // 固定 tier 求 determinism（短路 autotier）。預設 T1 = block-match-Vulkan
    // （無特殊 GPU 需求，第一個驗測切片）。外部可用 VIPLE_VKFRUC_TIER 覆寫。
    if (qEnvironmentVariableIsEmpty("VIPLE_VKFRUC_TIER")) {
        qputenv("VIPLE_VKFRUC_TIER", "1");
    }

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[fruc-offline] SDL_InitSubSystem(VIDEO) failed: %s\n",
                SDL_GetError());
        return 12;
    }

    // 隱藏 Vulkan window——dump 抓的是 present 前的 FRUC compute buffer，
    // 不需要看畫面，但 initialize 需要 surface 才能建 swapchain。
    SDL_Window* window = SDL_CreateWindow(
        "viplestream-fruc-offline",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        width, height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "[fruc-offline] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 13;
    }

    VkFrucRenderer* renderer = new VkFrucRenderer(0);

    DECODER_PARAMETERS params;
    SDL_zero(params);
    params.window           = window;
    params.vds              = StreamingPreferences::VDS_FORCE_HARDWARE;
    params.videoFormat      = VIDEO_FORMAT_H264;  // 只影響內部 buffer 設定，不解碼
    params.width            = width;
    params.height           = height;
    params.frameRate        = 60;
    params.enableVsync      = false;
    params.enableFramePacing = false;
    params.testOnly         = false;

    if (!renderer->initialize(&params)) {
        fprintf(stderr, "[fruc-offline] VkFrucRenderer::initialize failed "
                        "(see [VIPLE-VKFRUC] log above)\n");
        delete renderer;
        SDL_DestroyWindow(window);
        return 14;
    }
    fprintf(stderr, "[fruc-offline] renderer initialized; feeding frames...\n");

    // RGB24 → NV12，BT.601 limited（對齊 IFFmpegRenderer 預設 REC_601 / limited，
    // 讓 dump 端的 YUV→RGB 還原與這裡的 RGB→YUV 對稱，避免色彩偏差吃掉指標）。
    struct SwsContext* sws = sws_getContext(
        width, height, AV_PIX_FMT_RGB24,
        width, height, AV_PIX_FMT_NV12,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        fprintf(stderr, "[fruc-offline] sws_getContext failed\n");
        delete renderer;
        SDL_DestroyWindow(window);
        return 15;
    }
    {
        // 對齊引擎 NV12→RGB 用的 BT.709 narrow（init log: ycbcr sampler model=BT709
        // range=narrow）。601/709 不一致會全幀色偏吃掉數 dB（passthrough PSNR 變低）。
        const int* coef = sws_getCoefficients(SWS_CS_ITU709);
        // srcRange=1(RGB full) → dstRange=0(YUV limited/narrow)。
        sws_setColorspaceDetails(sws, coef, 1, coef, 0, 0, 1 << 16, 1 << 16);
    }

    int fed = 0;
    for (const QString& name : frames) {
        QImage img(dir.filePath(name));
        if (img.isNull()) {
            fprintf(stderr, "[fruc-offline] skip unreadable %s\n", qPrintable(name));
            continue;
        }
        img = img.convertToFormat(QImage::Format_RGB888);
        if (img.width() != width || img.height() != height) {
            img = img.scaled(width, height, Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_RGB888);
        }

        AVFrame* frame = av_frame_alloc();
        frame->format      = AV_PIX_FMT_NV12;
        frame->width       = width;
        frame->height      = height;
        frame->colorspace  = AVCOL_SPC_BT709;      // 對齊引擎 NV12 sampler（BT.709 narrow）
        frame->color_range = AVCOL_RANGE_MPEG;     // limited / narrow
        if (av_frame_get_buffer(frame, 32) < 0) {
            fprintf(stderr, "[fruc-offline] av_frame_get_buffer failed\n");
            av_frame_free(&frame);
            continue;
        }

        const uint8_t* srcSlice[1] = { img.constBits() };
        const int srcStride[1] = { (int)img.bytesPerLine() };
        sws_scale(sws, srcSlice, srcStride, 0, height, frame->data, frame->linesize);

        renderer->renderFrame(frame);
        av_frame_free(&frame);
        fed++;

        // 模擬 ~60fps 間隔，避免 latency throttle / autotier 誤判而 skip chain。
        SDL_Delay(16);
    }

    fprintf(stderr, "[fruc-offline] fed %d frames; flushing dump...\n", fed);
    SDL_Delay(750);  // 給 §B-DUMP writer thread 時間把最後幾幀寫完

    sws_freeContext(sws);
    delete renderer;  // dtor 收掉 writer thread + teardown Vulkan
    SDL_DestroyWindow(window);

    fprintf(stderr, "[fruc-offline] done. dumps in %s\n", qPrintable(dumpDir));
    return 0;
#endif
}
