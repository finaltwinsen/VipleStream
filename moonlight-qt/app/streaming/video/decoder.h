#pragma once

#include <Limelight.h>
#include "SDL_compat.h"
#include "settings/streamingpreferences.h"

#define SDL_CODE_FRAME_READY 0

#define MAX_SLICES 4

typedef struct _VIDEO_STATS {
    uint32_t receivedFrames;
    uint32_t decodedFrames;
    uint32_t renderedFrames;
    uint32_t totalFrames;
    uint32_t networkDroppedFrames;
    uint32_t pacerDroppedFrames;
    // §LOGHYG-NET10（2026-11-27）：host 沒有附擷取時戳的幀。host 把「畫面沒有更新、重送上一張」
    // 編成新幀時不帶 frame_timestamp（Sunshine stream.cpp 的 frame_is_dupe），header 的
    // frame_processing_latency 因此是 0；這是 client 端唯一能分辨「靜態畫面重複幀」與
    // 「主機真的跟不上」的訊號。只有在這個 session 看過 host 回報非 0 延遲時才拿它當重複幀
    // （見 FFmpegVideoDecoder::m_HostLatReported）。
    uint32_t hostRepeatFrames;
    uint16_t minHostProcessingLatency;         // low-res from RTP
    uint16_t maxHostProcessingLatency;         // low-res from RTP
    uint32_t totalHostProcessingLatency;       // low-res from RTP
    uint32_t framesWithHostProcessingLatency;  // low-res from RTP
    uint64_t totalReassemblyTimeUs;            // high-res (1us)
    uint64_t totalDecodeTimeUs;                // high-res (1us)
    uint64_t totalPacerTimeUs;                 // high-res (1us)
    uint64_t totalRenderTimeUs;                // high-res (1us)
    uint32_t lastRtt;                          // low-res from enet (1ms)
    uint32_t lastRttVariance;                  // low-res from enet (1ms)
    double totalFps;                           // high-res
    double receivedFps;                        // high-res
    double decodedFps;                         // high-res
    double renderedFps;                        // high-res
    double videoMegabitsPerSec;                // current video bitrate in Mbps, not including FEC overhead
    uint64_t measurementStartUs;               // microseconds

    // VipleStream: FRUC frame interpolation counters
    uint32_t frucInterpolatedFrames;           // interpolated frames produced by FRUC
} VIDEO_STATS, *PVIDEO_STATS;

typedef struct _DECODER_PARAMETERS {
    SDL_Window* window;
    StreamingPreferences::VideoDecoderSelection vds;

    int videoFormat;
    int width;
    int height;
    int frameRate;
    bool enableVsync;
    bool enableFramePacing;
    bool testOnly;
    // §VR M3a X2（尾端，預設值讓既有逐欄位設定的呼叫端不受影響）：XR 虛擬螢幕的 XrContext，
    // 非 null 時 FFmpegVideoDecoder 先試 XrRenderer frontend；testFrameOnly＝這個實例只解測試幀
    // （separate test decoder，C20），不可把幀送進 XR mailbox。
    class XrContext* xr = nullptr;
    bool testFrameOnly = false;
} DECODER_PARAMETERS, *PDECODER_PARAMETERS;

#define WINDOW_STATE_CHANGE_SIZE 0x01
#define WINDOW_STATE_CHANGE_DISPLAY 0x02

typedef struct _WINDOW_STATE_CHANGE_INFO {
    SDL_Window* window;
    uint32_t stateChangeFlags;

    // Populated if WINDOW_STATE_CHANGE_SIZE is set
    int width;
    int height;

    // Populated if WINDOW_STATE_CHANGE_DISPLAY is set
    int displayIndex;
} WINDOW_STATE_CHANGE_INFO, *PWINDOW_STATE_CHANGE_INFO;

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() {}
    virtual bool initialize(PDECODER_PARAMETERS params) = 0;
    virtual bool isHardwareAccelerated() = 0;
    virtual bool isAlwaysFullScreen() = 0;
    virtual bool isHdrSupported() = 0;
    virtual int getDecoderCapabilities() = 0;
    virtual int getDecoderColorspace() = 0;
    virtual int getDecoderColorRange() = 0;
    virtual QSize getDecoderMaxResolution() = 0;
    virtual int submitDecodeUnit(PDECODE_UNIT du) = 0;
    virtual void renderFrameOnMainThread() = 0;
    virtual void setHdrMode(bool enabled) = 0;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) = 0;
};
