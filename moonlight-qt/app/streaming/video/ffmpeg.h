#pragma once

#include <functional>
#include <QQueue>
#include <set>

#include "../bandwidth.h"
#include "decoder.h"
#include "ffmpeg-renderers/renderer.h"
#include "ffmpeg-renderers/pacer/pacer.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class FFmpegVideoDecoder : public IVideoDecoder {
public:
    FFmpegVideoDecoder(bool testOnly);
    virtual ~FFmpegVideoDecoder() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool isHardwareAccelerated() override;
    virtual bool isAlwaysFullScreen() override;
    virtual bool isHdrSupported() override;
    virtual int getDecoderCapabilities() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual QSize getDecoderMaxResolution() override;
    virtual int submitDecodeUnit(PDECODE_UNIT du) override;
    virtual void renderFrameOnMainThread() override;
    virtual void setHdrMode(bool enabled) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;

    virtual IFFmpegRenderer* getBackendRenderer();

private:
    enum class TestMode {
        // No test frame and prepare for rendering
        NoTesting,

        // Submit only the test frame and do not prepare for rendering
        TestFrameOnly,

        // Submit the test frame and prepare for rendering
        TestFrame
    };

    // §LOGHYG-A5 — initialize() 本體；public initialize() 變薄殼，
    // 負責依「前次是否失敗」決定 av_log level 再轉呼叫這裡。
    bool initializeInternal(PDECODER_PARAMETERS params);

    bool completeInitialization(const AVCodec* decoder,
                                enum AVPixelFormat requiredFormat,
                                PDECODER_PARAMETERS params,
                                TestMode testMode,
                                bool useAlternateFrontend);

    void stringifyVideoStats(VIDEO_STATS& stats, char* output, int length);

    void logVideoStats(VIDEO_STATS& stats, const char* title);

    void addVideoStats(VIDEO_STATS& src, VIDEO_STATS& dst);

    bool createFrontendRenderer(PDECODER_PARAMETERS params, bool useAlternateFrontend);

    static
    bool isDecoderMatchForParams(const AVCodec *decoder, PDECODER_PARAMETERS params);

    static
    bool isZeroCopyFormat(AVPixelFormat format);

    static
    int getAVCodecCapabilities(const AVCodec *codec);

    bool tryInitializeHwAccelDecoder(PDECODER_PARAMETERS params,
                                     int pass,
                                     QSet<const AVCodec*>& terminallyFailedHardwareDecoders);

    bool tryInitializeNonHwAccelDecoder(PDECODER_PARAMETERS params,
                                        bool requireZeroCopyFormat,
                                        QSet<const AVCodec*>& terminallyFailedHardwareDecoders);

    bool tryInitializeRendererForUnknownDecoder(const AVCodec* decoder,
                                                PDECODER_PARAMETERS params,
                                                bool tryHwAccel);

    bool tryInitializeRenderer(const AVCodec* decoder,
                               enum AVPixelFormat requiredFormat,
                               PDECODER_PARAMETERS params,
                               const AVCodecHWConfig* hwConfig,
                               IFFmpegRenderer::InitFailureReason* failureReason,
                               std::function<IFFmpegRenderer*()> createRendererFunc);

    static IFFmpegRenderer* createHwAccelRenderer(const AVCodecHWConfig* hwDecodeCfg, int pass, int videoFormat);

    bool initializeRendererInternal(IFFmpegRenderer* renderer, PDECODER_PARAMETERS params);

    static bool isSeparateTestDecoderRequired(const AVCodec* decoder);

    void reset();

    void writeBuffer(PLENTRY entry, int& offset);

    static
    enum AVPixelFormat ffGetFormat(AVCodecContext* context,
                                   const enum AVPixelFormat* pixFmts);

    void decoderThreadProc();

    static int decoderThreadProcThunk(void* context);

    AVPacket* m_Pkt;
    AVCodecContext* m_VideoDecoderCtx;
    enum AVPixelFormat m_RequiredPixelFormat;
    QByteArray m_DecodeBuffer;
    const AVCodecHWConfig* m_HwDecodeCfg;
    IFFmpegRenderer* m_BackendRenderer;
    IFFmpegRenderer* m_FrontendRenderer;
    int m_ConsecutiveFailedDecodes;
    Pacer* m_Pacer;
    BandwidthTracker m_BwTracker;
    VIDEO_STATS m_ActiveWndVideoStats;
    VIDEO_STATS m_LastWndVideoStats;
    VIDEO_STATS m_GlobalVideoStats;
    std::set<IFFmpegRenderer::RendererType> m_FailedRenderers;

    int m_FramesIn;
    int m_FramesOut;

    int m_LastFrameNumber;
    int m_StreamFps;
    int m_OriginalVideoWidth;
    int m_OriginalVideoHeight;
    int m_VideoFormat;

    // §LOGHYG-NET10 (v1.5.268) — [VIPLE-NET]/[VIPLE-RATIO] 每秒 2 行改為
    // 每 10 秒 1-2 行彙總。每秒統計照算並存入 ring；異常秒（低收/丟包/
    // 慢解碼/hostLat 突波）立即用原格式印出，並在進入異常時回補前 3 秒
    // (pre) 行，保留秒級斜坡診斷力。
    struct NetSecondSample {
        uint32_t receivedFrames;
        uint32_t decodedFrames;
        uint32_t networkDroppedFrames;
        uint32_t totalFrames;
        double decodeMeanMs;
        double hostLatAvgMs;   // < 0 = 該秒無 host latency 回報（印 n/a）
        bool flushed;          // 已印過秒級行（pre-flush 不重印）
    };
    static constexpr int k_NetRingSize = 10;
    NetSecondSample m_NetRing[k_NetRingSize] = {};
    int m_NetRingHead = 0;             // 下一寫入位置
    int m_NetRingCount = 0;            // 有效格數（<= k_NetRingSize）
    int m_NetSecsSinceAggregate = 0;   // 距上次 [VIPLE-NET10] 的非暖機秒數
    uint64_t m_NetWindowSeq = 0;       // stream (re)start 起算的視窗序號
    bool m_NetAnomalyActive = false;   // 連續異常期間每秒照印

    void logNetSecondLine(const NetSecondSample& s,
                          const char* prefix, const char* suffix) const;

    // VipleStream v1.4.152 §R2-β-2 + v1.4.153 §R2-γ-5: recv-ratio adaptive controller.
    // ACTIVE: §LOGHYG-WARN 60 秒滑動視窗警告（見下方 m_LowRecvWindowBits 區塊）。
    // PASSIVE: above80 / below70 / below40 hysteresis counters drive ratio switch.
    // v1.4.170 §R2-θ: metric changed from recv/server_fps to recv/display_Hz so
    // the controller actually targets display refresh alignment (server fps may
    // not equal display Hz — see plan 167-128-2x-1x-wiggly-lantern).  Counters
    // reused, semantics now "recv against display × X%".
    // §LOGHYG-WARN (v1.5.268) — 取代舊的「連續 5 秒 streak + 永久靜音」：
    // 近 60 秒內 recv%<80 的秒數 ≥ 42 才警告、冷卻 300 秒可重警，並依視窗
    // 內 networkDropped 分流 SERVER_LIMITED（伺服器產幀不足）vs 網路型。
    uint64_t m_LowRecvWindowBits = 0;  // 每秒 1 bit：該秒 recv% < 80
    uint32_t m_WarnWndRecv[60] = {};   // 該秒 receivedFrames
    uint32_t m_WarnWndDrop[60] = {};   // 該秒 networkDroppedFrames
    int m_WarnWndIdx = 0;
    int m_WarnWndFill = 0;
    int m_RatioWarnCooldownSec = 0;    // > 0 遞減；歸零才可再警
    int m_RatioWarnOverlayHideSec = 0; // > 0 遞減；歸零時收掉畫面通知
    // SERVER_LIMITED 常見於靜態畫面省流（damage-based 擷取），overlay 每
    // session 最多一次，之後只記 log，避免每次冷卻到期就跳畫面通知。
    bool m_RatioWarnSrvLtdOverlayShown = false;
    // OverlayStatusUpdate 是多方共用的單一 slot；倒數收掉前先比對文字仍是
    // 自己寫的那份，避免誤關別人的常駐通知（如 gamepad mouse mode）。
    char m_RatioWarnLastMsg[512] = {};
    int m_RecvAbove80Seconds = 0;  // recv ≥ 95% of display_Hz → step ratio down
    int m_RecvBelow70Seconds = 0;  // recv < 80% of display_Hz → bump to 2x
    int m_RecvBelow40Seconds = 0;  // recv < 40% of display_Hz → bump to 3x
    // v1.4.170 §R2-θ — cooldown after LiRequestFpsChange.  Server encoder
    // reconfig takes ~100-500ms; during that window received_fps will dip
    // briefly even though the request landed.  We block further ratio
    // transitions for 5s to avoid encoder-reconfig oscillation.
    int m_FpsChangeCooldownSec = 0;
    bool m_NeedsSpsFixup;
    bool m_TestOnly;
    TestMode m_CurrentTestMode;
    SDL_Thread* m_DecoderThread;
    SDL_atomic_t m_DecoderThreadShouldQuit;

    // Data buffers in the queued DU are not valid
    QQueue<DECODE_UNIT> m_FrameInfoQueue;

    static const uint8_t k_H264TestFrame[];
    static const uint8_t k_HEVCMainTestFrame[];
    static const uint8_t k_HEVCMain10TestFrame[];
    static const uint8_t k_AV1Main8TestFrame[];
    static const uint8_t k_AV1Main10TestFrame[];
    static const uint8_t k_h264High_444TestFrame[];
    static const uint8_t k_HEVCRExt8_444TestFrame[];
    static const uint8_t k_HEVCRExt10_444TestFrame[];
    static const uint8_t k_AV1High8_444TestFrame[];
    static const uint8_t k_AV1High10_444TestFrame[];

};
