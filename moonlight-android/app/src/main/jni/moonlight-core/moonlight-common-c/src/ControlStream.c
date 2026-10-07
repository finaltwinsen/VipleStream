#include "Limelight-internal.h"
#include "VrMultiLink.h"
#include "HolePunch.h"  // VipleStream: for LocalControlPort (NAT-pinhole preservation)

#ifdef VIPLE_MPQUIC
#include "QuicTransport.h"
#endif

// This is a private header, but it just contains some time macros
#include <enet/time.h>

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

// NV control stream packet header for TCP
typedef struct _NVCTL_TCP_PACKET_HEADER {
    unsigned short type;
    unsigned short payloadLength;
} NVCTL_TCP_PACKET_HEADER, *PNVCTL_TCP_PACKET_HEADER;

typedef struct _NVCTL_ENET_PACKET_HEADER_V1 {
    unsigned short type;
} NVCTL_ENET_PACKET_HEADER_V1, *PNVCTL_ENET_PACKET_HEADER_V1;

typedef struct _NVCTL_ENET_PACKET_HEADER_V2 {
    unsigned short type;
    unsigned short payloadLength;
} NVCTL_ENET_PACKET_HEADER_V2, *PNVCTL_ENET_PACKET_HEADER_V2;

#define AES_GCM_TAG_LENGTH 16
typedef struct _NVCTL_ENCRYPTED_PACKET_HEADER {
    unsigned short encryptedHeaderType; // Always LE 0x0001
    unsigned short length; // sizeof(seq) + 16 byte tag + secondary header and data
    unsigned int seq; // Monotonically increasing sequence number (used as IV for AES-GCM)

    // encrypted NVCTL_ENET_PACKET_HEADER_V2 and payload data follow
} NVCTL_ENCRYPTED_PACKET_HEADER, *PNVCTL_ENCRYPTED_PACKET_HEADER;

typedef struct _QUEUED_REFERENCE_FRAME_CONTROL {
    uint32_t startFrame;
    uint32_t endFrame;
    bool invalidate; // true: RFI(startFrame, endFrame); false: LTR_ACK(startFrame)
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_REFERENCE_FRAME_CONTROL, *PQUEUED_REFERENCE_FRAME_CONTROL;

typedef struct _QUEUED_FRAME_FEC_STATUS {
    SS_FRAME_FEC_STATUS fecStatus;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_FRAME_FEC_STATUS, *PQUEUED_FRAME_FEC_STATUS;

typedef struct _QUEUED_ASYNC_CALLBACK {
    int typeIndex;
    union {
        struct {
            uint16_t controllerNumber;
            uint16_t lowFreqRumble;
            uint16_t highFreqRumble;
        } rumble;
        struct {
            uint16_t controllerNumber;
            uint16_t leftTriggerMotor;
            uint16_t rightTriggerMotor;
        } rumbleTriggers;
        struct {
            uint16_t controllerNumber;
            uint16_t reportRateHz;
            uint8_t motionType;
        } setMotionEventState;
        struct {
            uint16_t controllerNumber;
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } setControllerLed;
        struct {
            uint16_t controllerNumber;
            /**
             * 0x04 - Right trigger
             * 0x08 - Left trigger
             */
            uint8_t eventFlags;
            uint8_t typeLeft;
            uint8_t typeRight;
            // arrays of size DS_EFFECT_PAYLOAD_SIZE
            // this is an opaque payload that will be read directly from the joypad and set as is to the client controller
            // if you are curious about the actual data, there's some rationale in
            // https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db
            uint8_t left[DS_EFFECT_PAYLOAD_SIZE];
            uint8_t right[DS_EFFECT_PAYLOAD_SIZE];
        } dsAdaptiveTrigger;
        struct {
            uint8_t reportId;
            uint8_t op;        // §SC-HID Phase 2C: 1=GET 2=SET
            uint8_t seq;
            uint8_t queryLen;
            uint8_t query[64];
        } scHidFeatureRequest;  // §SC-HID feature tunnel
        struct {
            uint8_t len;
            uint8_t buf[VIPLE_VR_MAX_CTRL_PAYLOAD];
        } vrS2C;  // §VR 0x5508：內嵌複製（佇列的每條釋放路徑都只 free 整個項目，不會洩漏）
    } data;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_ASYNC_CALLBACK, *PQUEUED_ASYNC_CALLBACK;

static SOCKET ctlSock = INVALID_SOCKET;
static ENetHost* client;
static ENetPeer* peer;
static PLT_MUTEX enetMutex;
static bool usePeriodicPing;

static PLT_THREAD lossStatsThread;
static PLT_THREAD invalidateRefFramesThread;
static PLT_THREAD requestIdrFrameThread;
static PLT_THREAD controlReceiveThread;
static PLT_THREAD asyncCallbackThread;
static uint32_t lastGoodFrame;
static uint32_t lastSeenFrame;
static bool stopping;
static bool disconnectPending;
static bool encryptedControlStream;
static bool hdrEnabled;
static SS_HDR_METADATA hdrMetadata;

static int intervalGoodFrameCount;
static int intervalTotalFrameCount;
static uint64_t intervalStartTimeMs;
static int lastIntervalLossPercentage;
static int lastConnectionStatusUpdate;
static uint32_t currentEnetSequenceNumber;
static uint64_t firstFrameTimeMs;
#ifdef VIPLE_MPQUIC
static volatile int enetReconnectPending;
// §Q-BYPASS-LOG-RESET v1.5.197 Fix P.2：提升到 file-scope，
// ENet 重連時重置，讓下一次 failover 的 bypass 動作可見。
// §Q-REVIEW 2026-05-29：volatile — 由 sendMessageEnet（lossStats/控制 thread）
// 讀寫、由 reconnect 路徑歸零，跨執行緒。純診斷計數器，volatile 對齊同檔
// enetReconnectPending 的寫法，避免編譯器快取造成計數失準（非嚴格 atomic，
// 但對 log gate 足夠）。
static volatile int bypassLogCount;

// §Q-REMOTE Fix R.3 (v1.5.235)：「ENet 死、QUIC 活」韌性缺口修補。
// 實測（2026-06-12 port 級封 ENet 47999、QUIC 48010 健康）：所有
// §Q-ENET-GRACE / R.2 / Fix K / Fix P 的觸發鏈都閘在 quicIsFailoverActive()，
// QUIC 主路徑健康（無 failover）時 ENet 一斷就 connectionTerminated——
// 健康的 QUIC 通道沒被用來撐住 session。真實世界觸發：port-specific
// NAT/QoS/防火牆對 47999 與 48010 行為不同。
//
// 修法：控制平面的 fallback 條件統一改為「failover 進行中 *或* QUIC
// 傳輸活著」。MPQUIC 未啟用時 quicIsConnected() 恆 false，行為不變。
static bool quicControlFallbackAvailable(void) {
    return quicIsFailoverActive() || quicIsConnected();
}

// §Q-REMOTE Fix R.3：整段 ENet 重連期間為 1（含每次 5 秒連線嘗試中
// peer 短暫非 NULL 的窗口），重連成功才歸零。若沒有這個旗標，IDR
// 恰落在嘗試窗口會經 sendMessageEnet 失敗路徑走 INPUT datagram
// （server 靜默丟棄）——Fix P 陷阱的 5 秒殘餘版本。
static volatile int enetReconnecting;

// §Q-REMOTE Fix R.3：ENet 控制通道目前不可用（reconnect 進行中或
// peer 已銷毀）。peer 無鎖讀取與 lossStatsThread 既有寫法一致
// （指標讀取，做為路由提示足夠）。
static bool enetControlChannelDown(void) {
    return enetReconnectPending || enetReconnecting || peer == NULL;
}

// §M01-C：給 InputStream.c 用，與本檔 quicControlFallbackAvailable() 同義
//（failover 進行中 或 QUIC 傳輸活著）。舊版 InputStream 只看
// quicIsFailoverActive()，Wi-Fi-only 單一路徑的 failoverPromotedSlot 恆為
// -1，任何一次輸入送失敗都會 connectionTerminated + 輸入執行緒 return。
// 兩邊共用同一個判斷，避免條件各寫各的又漂移。
bool isQuicControlFallbackAvailable(void) {
    return quicControlFallbackAvailable();
}

// §M01-C：LiGetEstimatedRttInfo() 的無鎖快照。上游「peer 永不消失」的
// 假設已被 §Q-ENET-RECONNECT 打破（重連會 enet_host_destroy 舊 client），
// 無鎖讀 peer 是 use-after-free 窗口（TODO Q.r16 ③）；但也不能改成加
// enetMutex——呼叫端在 submitDecodeUnit 路徑（ffmpeg.cpp addVideoStats
// 每秒一次），而 controlReceiveThread 的 disconnectPending 分支會持鎖
// service 最長約 1.1 s（Windows SRWLock 不保證公平），等於拿視訊送解碼
// 去賭鎖。改由 controlReceiveThread 在已持鎖的 service 之後寫快照：
// peer 是 CONNECTED 才設 enetRttValid=1；銷毀／斷線前先清 0。
// rtt 與 var 可能來自相鄰兩次更新（撕裂），對統計顯示無害。
static volatile uint32_t enetRttSnap;
static volatile uint32_t enetRttVarSnap;
static volatile int enetRttValid;
#endif

static LINKED_BLOCKING_QUEUE referenceFrameControlQueue;
static LINKED_BLOCKING_QUEUE frameFecStatusQueue;
static LINKED_BLOCKING_QUEUE asyncCallbackQueue;
static PLT_EVENT idrFrameRequiredEvent;

static PPLT_CRYPTO_CONTEXT encryptionCtx;
static PPLT_CRYPTO_CONTEXT decryptionCtx;

#define CONN_IMMEDIATE_POOR_LOSS_RATE 30
#define CONN_CONSECUTIVE_POOR_LOSS_RATE 15
#define CONN_OKAY_LOSS_RATE 5
#define CONN_STATUS_SAMPLE_PERIOD 3000

#define IDX_START_A 0
#define IDX_REQUEST_IDR_FRAME 0
#define IDX_START_B 1
#define IDX_INVALIDATE_REF_FRAMES 2
#define IDX_LOSS_STATS 3
#define IDX_INPUT_DATA 5
#define IDX_RUMBLE_DATA 6
#define IDX_TERMINATION 7
#define IDX_HDR_INFO 8
#define IDX_RUMBLE_TRIGGER_DATA 9
#define IDX_SET_MOTION_EVENT 10
#define IDX_SET_RGB_LED 11
#define IDX_DS_ADAPTIVE_TRIGGERS 12
#define IDX_FPS_CHANGE 13  // VipleStream: dynamic FPS change (client→server)
#define IDX_SC_HID_FEATURE_REQ 14  // VipleStream §SC-HID: feature tunnel request (server→client)
// §VR：0x5508 VR_S2C 的偽 typeIndex。VR ptype 不進 packetTypes 表（不變式 3），
// 這個值只用在 QUEUED_ASYNC_CALLBACK.typeIndex，絕不可拿來索引 packetTypes[]、
// payloadLengths[]、preconstructedPayloads[]。
#define IDX_VIPLE_VR_S2C 0x100

#define CONTROL_STREAM_TIMEOUT_SEC 10
#define CONTROL_STREAM_LINGER_TIMEOUT_SEC 2

static const short packetTypesGen3[] = {
    0x1407, // Request IDR frame
    0x1410, // Start B
    0x1404, // Invalidate reference frames
    0x140c, // Loss Stats
    0x1417, // Frame Stats (unused)
    -1,     // Input data (unused)
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // HDR mode (unused)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
    -1,     // Adaptive triggers (unused)
    -1,     // FPS change (unused)
    -1,     // SC-HID feature request (unused)
};
static const short packetTypesGen4[] = {
    0x0606, // Request IDR frame
    0x0609, // Start B
    0x0604, // Invalidate reference frames
    0x060a, // Loss Stats
    0x0611, // Frame Stats (unused)
    -1,     // Input data (unused)
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // HDR mode (unused)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
    -1,     // Adaptive triggers (unused)
    -1,     // FPS change (unused)
    -1,     // SC-HID feature request (unused)
};
static const short packetTypesGen5[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0207, // Input data
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
    -1,     // HDR mode (unknown)
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
    -1,     // Adaptive triggers (unused)
    -1,     // FPS change (unused)
    -1,     // SC-HID feature request (unused)
};
static const short packetTypesGen7[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0206, // Input data
    0x010b, // Rumble data
    0x0100, // Termination
    0x010e, // HDR mode
    -1,     // Rumble triggers (unused)
    -1,     // Set motion event (unused)
    -1,     // Set RGB LED (unused)
    -1,     // Adaptive triggers (unused)
    -1,     // FPS change (unused)
    -1,     // SC-HID feature request (unused)
};
static const short packetTypesGen7Enc[] = {
    0x0302, // Request IDR frame
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0206, // Input data
    0x010b, // Rumble data
    0x0109, // Termination (extended)
    0x010e, // HDR mode
    0x5500, // Rumble triggers (Sunshine protocol extension)
    0x5501, // Set motion event (Sunshine protocol extension)
    0x5502, // Set RGB LED (Sunshine protocol extension)
    0x5503, // Set Adaptive Triggers (Sunshine protocol extension)
    0x5504, // FPS change (VipleStream protocol extension)
    0x5505, // SC-HID feature tunnel request (VipleStream §SC-HID)
};

static const char requestIdrFrameGen3[] = { 0, 0 };
static const int startBGen3[] = { 0, 0, 0, 0xa };

static const char requestIdrFrameGen4[] = { 0, 0 };
static const char startBGen4[] = { 0 };

static const char startAGen5[] = { 0, 0 };
static const char startBGen5[] = { 0 };

static const char requestIdrFrameGen7Enc[] = { 0, 0 };

static const short payloadLengthsGen3[] = {
    sizeof(requestIdrFrameGen3), // Request IDR frame
    sizeof(startBGen3), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    64, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen4[] = {
    sizeof(requestIdrFrameGen4), // Request IDR frame
    sizeof(startBGen4), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    64, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen5[] = {
    sizeof(startAGen5), // Start A
    sizeof(startBGen5), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    80, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen7[] = {
    sizeof(startAGen5), // Start A
    sizeof(startBGen5), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    80, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen7Enc[] = {
    sizeof(requestIdrFrameGen7Enc), // Request IDR frame
    sizeof(startBGen5), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    80, // Frame Stats
    -1, // Input data
};

static const char* preconstructedPayloadsGen3[] = {
    requestIdrFrameGen3,
    (char*)startBGen3
};
static const char* preconstructedPayloadsGen4[] = {
    requestIdrFrameGen4,
    startBGen4
};
static const char* preconstructedPayloadsGen5[] = {
    startAGen5,
    startBGen5
};
static const char* preconstructedPayloadsGen7[] = {
    startAGen5,
    startBGen5
};
static const char* preconstructedPayloadsGen7Enc[] = {
    requestIdrFrameGen7Enc,
    startBGen5
};

static short* packetTypes;
static short* payloadLengths;
static char**preconstructedPayloads;
static bool supportsIdrFrameRequest;

#define LOSS_REPORT_INTERVAL_MS 50
#define PERIODIC_PING_INTERVAL_MS 100

// §VR 恢復狀態機（定義在 connectionDetectedFrameLoss 之前）
static void vrRecoveryInit(void);
static void vrRecoveryDestroy(void);
static void vrSendThreadFunc(void* context);
static void vrHandleS2CInline(const uint8_t* tlv, int length);
static void configureVrThrottle(ENetPeer* p);

// Initializes the control stream
int initializeControlStream(void) {
    stopping = false;
    PltCreateEvent(&idrFrameRequiredEvent);
    LbqInitializeLinkedBlockingQueue(&referenceFrameControlQueue, 20);
    LbqInitializeLinkedBlockingQueue(&frameFecStatusQueue, 8); // Limits number of frame status reports per periodic ping interval
    LbqInitializeLinkedBlockingQueue(&asyncCallbackQueue, 30);
    PltCreateMutex(&enetMutex);
    vrRecoveryInit();

    encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431);

    if (AppVersionQuad[0] == 3) {
        packetTypes = (short*)packetTypesGen3;
        payloadLengths = (short*)payloadLengthsGen3;
        preconstructedPayloads = (char**)preconstructedPayloadsGen3;
        supportsIdrFrameRequest = true;
    }
    else if (AppVersionQuad[0] == 4) {
        packetTypes = (short*)packetTypesGen4;
        payloadLengths = (short*)payloadLengthsGen4;
        preconstructedPayloads = (char**)preconstructedPayloadsGen4;
        supportsIdrFrameRequest = true;
    }
    else if (AppVersionQuad[0] == 5) {
        packetTypes = (short*)packetTypesGen5;
        payloadLengths = (short*)payloadLengthsGen5;
        preconstructedPayloads = (char**)preconstructedPayloadsGen5;
        supportsIdrFrameRequest = false;
    }
    else {
        if (encryptedControlStream) {
            packetTypes = (short*)packetTypesGen7Enc;
            payloadLengths = (short*)payloadLengthsGen7Enc;
            preconstructedPayloads = (char**)preconstructedPayloadsGen7Enc;
            supportsIdrFrameRequest = true;
        }
        else {
            packetTypes = (short*)packetTypesGen7;
            payloadLengths = (short*)payloadLengthsGen7;
            preconstructedPayloads = (char**)preconstructedPayloadsGen7;
            supportsIdrFrameRequest = false;
        }
    }

    lastGoodFrame = 0;
    lastSeenFrame = 0;
    disconnectPending = false;
    intervalGoodFrameCount = 0;
    intervalTotalFrameCount = 0;
    intervalStartTimeMs = 0;
    lastIntervalLossPercentage = 0;
    lastConnectionStatusUpdate = CONN_STATUS_OKAY;
    firstFrameTimeMs = 0;
    currentEnetSequenceNumber = 0;
#ifdef VIPLE_MPQUIC
    enetReconnectPending = 0;
    enetReconnecting = 0; // §Q-REMOTE Fix R.3
    enetRttValid = 0;     // §M01-C：上一場 session 的 RTT 快照不可沿用
#endif
    usePeriodicPing = APP_VERSION_AT_LEAST(7, 1, 415);
    encryptionCtx = PltCreateCryptoContext();
    decryptionCtx = PltCreateCryptoContext();
    hdrEnabled = false;
    memset(&hdrMetadata, 0, sizeof(hdrMetadata));

    return 0;
}

static void freeBasicLbqList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;
        free(entry->data);
        entry = nextEntry;
    }
}

// Cleans up control stream
void destroyControlStream(void) {
    LC_ASSERT(stopping);
    PltDestroyCryptoContext(encryptionCtx);
    PltDestroyCryptoContext(decryptionCtx);
    PltCloseEvent(&idrFrameRequiredEvent);
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&referenceFrameControlQueue));
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&frameFecStatusQueue));
    freeBasicLbqList(LbqDestroyLinkedBlockingQueue(&asyncCallbackQueue));

    vrRecoveryDestroy();
    PltDeleteMutex(&enetMutex);
}

static void queueFrameInvalidationTuple(uint32_t startFrame, uint32_t endFrame) {
    // §F1-DBG-ASSERT（§FRZ-B3 同類）：上游原本在這裡
    // LC_ASSERT(startFrame <= endFrame)。反轉範圍只可能來自幀號狀態倒帶
    // （例：§FRZ-WATCHDOG 哨兵重置後採納了 failback 殘留的 stale 幀，
    // depacketizer 以舊的 startFrameNumber 回報較小的 frameIndex）。
    // Android 出貨的 native 是 NDK_DEBUG=1 建置、assert 生效，會在抵達
    // requestInvalidateReferenceFrames 的 §FRZ-B3 防衛之前就 SIGABRT；
    // release 版則把反轉 tuple 排進佇列、聚合後才被 §FRZ-B3 攔下改送 IDR。
    // 這裡直接改要 IDR，兩種建置行為一致，也不讓反轉 tuple 污染聚合範圍。
    if (startFrame > endFrame) {
        static uint64_t lastInvalidTupleLogMs;
        uint64_t nowMs = PltGetMillis();
        if (lastInvalidTupleLogMs == 0 || nowMs - lastInvalidTupleLogMs >= 1000) {
            Limelog("[VIPLE-CTRL] Invalid frame loss range (%u to %u) — requesting IDR frame instead\n",
                    startFrame, endFrame);
            lastInvalidTupleLogMs = nowMs;
        }
        LiRequestIdrFrame();
        return;
    }

    if (isReferenceFrameInvalidationEnabled()) {
        PQUEUED_REFERENCE_FRAME_CONTROL qfit;
        qfit = malloc(sizeof(*qfit));
        if (qfit != NULL) {
            *qfit = (QUEUED_REFERENCE_FRAME_CONTROL){
                .startFrame = startFrame,
                .endFrame = endFrame,
                .invalidate = true,
            };
            if (LbqOfferQueueItem(&referenceFrameControlQueue, qfit, &qfit->entry) == LBQ_BOUND_EXCEEDED) {
                // Too many invalidation tuples, so we need an IDR frame now
                Limelog("RFI range list reached maximum size limit\n");
                free(qfit);
                LiRequestIdrFrame();
            }
        }
        else {
            LiRequestIdrFrame();
        }
    }
    else {
        LiRequestIdrFrame();
    }
}

// Request an IDR frame on demand by the decoder
void LiRequestIdrFrame(void) {
    // Any reference frame invalidation requests should be dropped now.
    // We require a full IDR frame to recover.
    freeBasicLbqList(LbqFlushQueueItems(&referenceFrameControlQueue));

    // Request the IDR frame
    PltSetEvent(&idrFrameRequiredEvent);
}

// Forward declaration (defined later in this file)
static bool sendMessageAndForget(short ptype, short paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData);

// VipleStream: Request dynamic FPS change from server
void LiRequestFpsChange(int newFps) {
    if (packetTypes == NULL || packetTypes[IDX_FPS_CHANGE] == -1) {
        Limelog("FPS change not supported on this protocol version\n");
        return;
    }

    uint32_t payload = LE32((uint32_t)newFps);
    if (!sendMessageAndForget(packetTypes[IDX_FPS_CHANGE],
                              sizeof(payload),
                              &payload,
                              CTRL_CHANNEL_GENERIC,
                              ENET_PACKET_FLAG_RELIABLE,
                              false)) {
        Limelog("FPS change request failed: %d\n", (int)LastSocketError());
        return;
    }

    Limelog("FPS change request sent: %d fps\n", newFps);
}

// ── §VR 恢復狀態機（docs/vr_architecture.md §2.3「VR 恢復」）──────────────
//
// recovery=intra 時掉幀不等 IDR：depacketizer 照常把後續幀送進 decoder（畫面
// 局部破圖、由 intra refresh 自癒），這裡送 0x5507/09 LOSS（VR channel
// UNSEQUENCED，一次送 2 份）請 server 開一波 intra refresh。degraded 期間：
//   • 收到 REFRESH_START 且 startFrame > lastLost → 等這一波帶 REFRESH_DONE
//     的那一幀，收到就結束 degraded；
//   • 2×RTT+20 ms 內沒收到 REFRESH_START → 重送 LOSS；
//   • 重送 3 次仍沒回應，或等了 500 ms → 退回 IDR；
//   • 任何晚於 lastLost 的 IDR 幀都結束 degraded。
// 狀態會被 VideoRecv、控制接收、decoder（LiReportVrLoss）、lossStats 四條
// 執行緒存取，一律持 vrRecMutex；送封包一律在鎖外做（不與 enetMutex 交錯）。

#define VR_LOSS_RESEND_MAX        3
#define VR_LOSS_RESEND_BASE_MS    20
#define VR_UNANSWERED_IDR_MS      500
#define VR_WAVE_TIMEOUT_MS        1000
#define VR_IDR_RETRY_MS           1000
#define VR_STATS_LOG_INTERVAL_MS  10000
#define VR_SEND_TICK_MS           2     // VrSend 執行緒的輪詢間隔
#define VR_LOSS_TWIN_DELAY_MS     5     // LOSS 第二份延後送（分開兩個 datagram，避開同一個 burst）
#define VR_TRACKING_FAIL_BACKOFF  3
#define VR_TRACKING_BACKOFF_MS    1000

typedef enum {
    VR_ACTION_NONE = 0,
    VR_ACTION_RESEND_LOSS,
    VR_ACTION_REQUEST_IDR,
} VR_RECOVERY_ACTION;

static PLT_MUTEX vrRecMutex;
static bool vrSessionAtInit;  // initializeControlStream 當下是否為 VR session（final 統計用）
static struct {
    bool degraded;
    bool refreshSeen;         // 已收到涵蓋 lastLost 的 REFRESH_START
    bool idrRequested;        // 已退回 IDR，等 IDR 幀
    uint32_t firstLost;
    uint32_t lastLost;
    uint32_t waveStart;
    uint32_t waveEnd;
    uint64_t degradedSinceMs;
    uint64_t pendingSinceMs;  // 目前這個「還沒得到 REFRESH_START」的請求從何時開始
    uint64_t lastLossSendMs;
    uint64_t refreshSeenMs;
    uint64_t idrRequestedMs;
    int resendCount;
    // 待 VrSend 執行緒送出的 LOSS（視訊與 decoder 執行緒只登記，不碰 enetMutex）
    bool lossSendPending;
    uint8_t lossSendReason;
    bool twinPending;
    uint64_t twinDueMs;
    uint32_t twinFirst, twinLast;
    uint8_t twinReason;
    // 統計（10 秒區間與整場）
    uint32_t lossEvents, lossSends, lossResends, refreshRx, refreshStale;
    uint32_t recovered, idrFallbacks;
    uint32_t rttSamples, rttMaxMs;
    uint64_t rttSumMs;
    uint32_t recoverSamples, recoverMaxMs;
    uint64_t recoverSumMs;
    uint32_t totalLossEvents, totalRecovered, totalIdrFallbacks, totalRefreshRx;
    uint64_t lastStatsLogMs;
} vrRec;
// §VR：LOSS 送出與重送／退回 IDR 的計時由這條執行緒負責（只在 VR session 建立）。
// §M01-C 規定視訊路徑不得碰 enetMutex（controlReceiveThread 的 disconnectPending
// 分支可能持鎖 service 將近 1 秒），而送 LOSS 要加密＋enet_host_service。
static PLT_THREAD vrSendThread;
static bool vrSendThreadStarted;
static volatile int vrTrackingConsecutiveFails;
static volatile uint64_t vrTrackingBackoffUntilMs;
static volatile uint32_t vrTrackingSent, vrTrackingDropped;
#ifdef VIPLE_MPQUIC
// §VR：QUIC keepalive（lossStats 執行緒每 100 ms 一輪，5 輪 = 500 ms 送一次）
#define VR_QUIC_KEEPALIVE_TICKS 5
static int vrQuicKeepaliveTicks;
#endif

static void vrRecoveryInit(void) {
    PltCreateMutex(&vrRecMutex);
    memset(&vrRec, 0, sizeof(vrRec));
    vrSessionAtInit = (VrFlags & VIPLE_VR_SF_ENABLED) != 0;
    vrSendThreadStarted = false;
    vrTrackingConsecutiveFails = 0;
    vrTrackingBackoffUntilMs = 0;
    vrTrackingSent = 0;
    vrTrackingDropped = 0;
#ifdef VIPLE_MPQUIC
    vrQuicKeepaliveTicks = 0;
#endif
}

static void vrRecoveryDestroy(void) {
    if (vrSessionAtInit) {
        Limelog("[VIPLE-VR-LOSS] (final) lossEvents=%u recovered=%u idrFallbacks=%u refreshRx=%u "
                "tracking sent=%u dropped=%u\n",
                vrRec.totalLossEvents, vrRec.totalRecovered, vrRec.totalIdrFallbacks,
                vrRec.totalRefreshRx, vrTrackingSent, vrTrackingDropped);
    }
    PltDeleteMutex(&vrRecMutex);
}

static bool vrIntraRecoveryActive(void) {
    return (VrFlags & VIPLE_VR_SF_ENABLED) && (VrFlags & VIPLE_VR_SF_RECOVERY_INTRA);
}

static uint32_t vrGetEnetRttMs(void) {
    uint32_t rtt = 0;
    PltLockMutex(&enetMutex);
    if (peer != NULL) {
        rtt = peer->roundTripTime;
    }
    PltUnlockMutex(&enetMutex);
    return rtt;
}

// 送一份 LOSS（只能在 VrSend 執行緒或退回路徑呼叫：會持 enetMutex）
static void vrSendLossOnce(uint32_t firstLost, uint32_t lastLost, uint8_t reason) {
    uint8_t tlv[2 + sizeof(VIPLE_VR_TLV_LOSS)];
    VIPLE_VR_TLV_LOSS loss;

    loss.firstLost = firstLost;
    loss.lastLost = lastLost;
    loss.reason = reason;
    tlv[0] = VIPLE_VR_C2S_LOSS;
    tlv[1] = (uint8_t)sizeof(loss);
    memcpy(&tlv[2], &loss, sizeof(loss));

    // §VR-LINK-CTRL：server 同意的話，在每條已確認的連線各送一份，不走綁在 session 位址的 ENet——那條鏈路變弱時
    // 回報會掉或晚到（2026-10-07 頭盔實測：走到適配器收不到的位置後等不到回應，退回 IDR 花了 832 ms）。
    // 兩條路只走一條：server 的去重只認 50 ms 內的同一筆，晚到的那一份會白開一波。沒有可用連線時照舊走 ENet。
    if (vrmlSendCtrl(tlv, (int)sizeof(tlv)) > 0) {
        return;
    }

    sendMessageAndForget(VIPLE_VR_PTYPE_C2S, (short)sizeof(tlv), tlv,
                         VIPLE_VR_CTRL_CHANNEL, ENET_PACKET_FLAG_UNSEQUENCED, false);
}

// 持 vrRecMutex 呼叫：每 10 秒（有動靜時）印一次區間統計
static void vrMaybeLogStatsLocked(uint64_t now) {
    if (vrRec.lastStatsLogMs == 0) {
        vrRec.lastStatsLogMs = now;
        return;
    }
    if (now - vrRec.lastStatsLogMs < VR_STATS_LOG_INTERVAL_MS) {
        return;
    }
    if (vrRec.lossEvents || vrRec.refreshRx || vrRec.idrFallbacks || vrRec.degraded) {
        Limelog("[VIPLE-VR-LOSS] 10s: lossEvents=%u sends=%u resends=%u refreshRx=%u stale=%u "
                "recovered=%u idrFallback=%u lossToRefresh avg=%ums max=%ums recover avg=%ums max=%ums%s\n",
                vrRec.lossEvents, vrRec.lossSends, vrRec.lossResends, vrRec.refreshRx, vrRec.refreshStale,
                vrRec.recovered, vrRec.idrFallbacks,
                vrRec.rttSamples ? (unsigned)(vrRec.rttSumMs / vrRec.rttSamples) : 0, vrRec.rttMaxMs,
                vrRec.recoverSamples ? (unsigned)(vrRec.recoverSumMs / vrRec.recoverSamples) : 0, vrRec.recoverMaxMs,
                vrRec.degraded ? " (still degraded)" : "");
    }
    vrRec.lossEvents = vrRec.lossSends = vrRec.lossResends = vrRec.refreshRx = vrRec.refreshStale = 0;
    vrRec.recovered = vrRec.idrFallbacks = 0;
    vrRec.rttSamples = vrRec.rttMaxMs = 0;
    vrRec.rttSumMs = 0;
    vrRec.recoverSamples = vrRec.recoverMaxMs = 0;
    vrRec.recoverSumMs = 0;
    vrRec.lastStatsLogMs = now;
}

// 持 vrRecMutex 呼叫：決定逾時動作（實際送出由呼叫端在鎖外做）
static VR_RECOVERY_ACTION vrEvaluateTimersLocked(uint64_t now, uint32_t rttMs,
                                                 uint32_t* firstLost, uint32_t* lastLost) {
    if (!vrRec.degraded) {
        return VR_ACTION_NONE;
    }

    if (vrRec.idrRequested) {
        if (now - vrRec.idrRequestedMs >= VR_IDR_RETRY_MS) {
            vrRec.idrRequestedMs = now;
            return VR_ACTION_REQUEST_IDR;
        }
        return VR_ACTION_NONE;
    }

    if (vrRec.refreshSeen) {
        // 已收到 REFRESH_START，但這一波的 REFRESH_DONE 遲遲沒到（那一幀可能也掉了
        // 卻沒被偵測到）：當成沒回應，重新要一波
        if (now - vrRec.refreshSeenMs < VR_WAVE_TIMEOUT_MS) {
            return VR_ACTION_NONE;
        }
        vrRec.refreshSeen = false;
        vrRec.pendingSinceMs = now;
        vrRec.resendCount = 0;
    }

    if (vrRec.resendCount >= VR_LOSS_RESEND_MAX || now - vrRec.pendingSinceMs >= VR_UNANSWERED_IDR_MS) {
        vrRec.idrRequested = true;
        vrRec.idrRequestedMs = now;
        vrRec.idrFallbacks++;
        vrRec.totalIdrFallbacks++;
        return VR_ACTION_REQUEST_IDR;
    }

    if (now - vrRec.lastLossSendMs >= 2 * (uint64_t)rttMs + VR_LOSS_RESEND_BASE_MS) {
        vrRec.resendCount++;
        vrRec.lossResends++;
        vrRec.lastLossSendMs = now;
        *firstLost = vrRec.firstLost;
        *lastLost = vrRec.lastLost;
        return VR_ACTION_RESEND_LOSS;
    }

    return VR_ACTION_NONE;
}

static void vrPerformAction(VR_RECOVERY_ACTION action, uint32_t firstLost, uint32_t lastLost) {
    switch (action) {
    case VR_ACTION_RESEND_LOSS:
        vrSendLossOnce(firstLost, lastLost, VIPLE_VR_LOSS_RESEND);
        break;
    case VR_ACTION_REQUEST_IDR:
        Limelog("[VIPLE-VR-LOSS] no refresh response — falling back to IDR (lost %u..%u)\n",
                firstLost, lastLost);
        LiRequestIdrFrame();
        break;
    default:
        break;
    }
}

// 呼叫端是 VideoRecv（depacketizer）或 decoder 執行緒：只更新狀態、登記待送的 LOSS，
// 實際送出交給 VrSend 執行緒（不碰 enetMutex）。
static void vrRecoveryReportLoss(uint32_t firstLost, uint32_t lastLost, uint8_t reason) {
    uint64_t now = PltGetMillis();
    bool send = false;
    bool sendNow = false;
    uint32_t sendFirst = 0, sendLast = 0;

    if (isBefore32(lastLost, firstLost)) {
        // 反轉範圍只可能來自幀號狀態倒帶（見 queueFrameInvalidationTuple 的 §F1-DBG-ASSERT），
        // 只回報比較晚的那一端
        static uint64_t lastInvalidLogMs;
        if (lastInvalidLogMs == 0 || now - lastInvalidLogMs >= 1000) {
            Limelog("[VIPLE-VR-LOSS] invalid loss range %u..%u — using %u\n", firstLost, lastLost, firstLost);
            lastInvalidLogMs = now;
        }
        lastLost = firstLost;
    }

    PltLockMutex(&vrRecMutex);
    vrRec.lossEvents++;
    vrRec.totalLossEvents++;
    if (!vrRec.degraded) {
        vrRec.degraded = true;
        vrRec.refreshSeen = false;
        vrRec.idrRequested = false;
        vrRec.firstLost = firstLost;
        vrRec.lastLost = lastLost;
        vrRec.degradedSinceMs = now;
        vrRec.pendingSinceMs = now;
        vrRec.resendCount = 0;
        send = true;
    }
    else if (isBefore32(vrRec.lastLost, lastLost)) {
        // 更晚的新掉幀：擴大範圍。如果已經收到的那一波是在新掉幀之前開始的，
        // 它不保證涵蓋新掉幀 → 重新要一波（server 端會重啟 wave）
        vrRec.lastLost = lastLost;
        if (vrRec.refreshSeen && !isBefore32(lastLost, vrRec.waveStart)) {
            vrRec.refreshSeen = false;
            vrRec.pendingSinceMs = now;
            vrRec.resendCount = 0;
        }
        send = !vrRec.idrRequested;
    }
    // 其餘情況：範圍已涵蓋，交給逾時重送
    if (send) {
        vrRec.lastLossSendMs = now;
        vrRec.lossSends++;
        if (vrSendThreadStarted) {
            vrRec.lossSendPending = true;
            vrRec.lossSendReason = reason;
        }
        else {
            // VrSend 執行緒沒起來（建立失敗）：退回同步送，至少功能正確
            sendNow = true;
            sendFirst = vrRec.firstLost;
            sendLast = vrRec.lastLost;
        }
    }
    PltUnlockMutex(&vrRecMutex);

    if (sendNow) {
        vrSendLossOnce(sendFirst, sendLast, reason);
        vrSendLossOnce(sendFirst, sendLast, reason);
    }
}

static void vrRecoveryOnRefreshStart(uint32_t startFrame, uint8_t frameCnt) {
    uint64_t now = PltGetMillis();

    PltLockMutex(&vrRecMutex);
    if (vrRec.degraded && !vrRec.refreshSeen && !vrRec.idrRequested &&
            isBefore32(vrRec.lastLost, startFrame)) {
        uint32_t waitMs = (uint32_t)(now - vrRec.pendingSinceMs);
        vrRec.refreshSeen = true;
        vrRec.waveStart = startFrame;
        vrRec.waveEnd = startFrame + (frameCnt ? frameCnt : 1) - 1;
        vrRec.refreshSeenMs = now;
        vrRec.refreshRx++;
        vrRec.totalRefreshRx++;
        vrRec.rttSamples++;
        vrRec.rttSumMs += waitMs;
        if (waitMs > vrRec.rttMaxMs) {
            vrRec.rttMaxMs = waitMs;
        }
    }
    else {
        // 第二份備援、或對應的是更早的掉幀
        vrRec.refreshStale++;
    }
    PltUnlockMutex(&vrRecMutex);
}

// VideoRecv 執行緒：每組好一幀呼叫一次。只判斷是否恢復，不做任何網路 I/O。
void vrRecoveryOnFrame(uint32_t frameIndex, uint8_t vrFrameFlags, bool isIdr) {
    uint64_t now;

    if (!vrIntraRecoveryActive()) {
        return;
    }

    now = PltGetMillis();

    PltLockMutex(&vrRecMutex);
    if (vrRec.degraded) {
        bool recovered = false;
        if (isIdr && isBefore32(vrRec.lastLost, frameIndex)) {
            recovered = true;
        }
        // 下界用 waveEnd 而不是 waveStart：startFrame 是 server 的估計值（實際可能晚一幀），
        // 被重啟的舊 wave 的最後一幀也帶 REFRESH_DONE，只看 waveStart 會把它誤當成新 wave 完成
        else if (vrRec.refreshSeen && (vrFrameFlags & VIPLE_VR_FF_REFRESH_DONE) &&
                 !isBefore32(frameIndex, vrRec.waveEnd)) {
            recovered = true;
        }

        if (recovered) {
            uint32_t tookMs = (uint32_t)(now - vrRec.degradedSinceMs);
            vrRec.degraded = false;
            vrRec.refreshSeen = false;
            vrRec.idrRequested = false;
            vrRec.recovered++;
            vrRec.totalRecovered++;
            vrRec.recoverSamples++;
            vrRec.recoverSumMs += tookMs;
            if (tookMs > vrRec.recoverMaxMs) {
                vrRec.recoverMaxMs = tookMs;
            }
        }
    }
    vrMaybeLogStatsLocked(now);
    PltUnlockMutex(&vrRecMutex);
}

// VrSend 執行緒的一輪：送出登記的 LOSS（第二份延後 VR_LOSS_TWIN_DELAY_MS），並推進
// 重送／退回 IDR 的逾時。這條執行緒可以持 enetMutex（讀 RTT、送封包）。
static void vrSendTick(void) {
    VR_RECOVERY_ACTION action;
    uint32_t firstLost = 0, lastLost = 0;
    uint32_t rttMs = 0;
    uint64_t now;
    bool sendFirst = false, sendTwin = false;
    uint32_t f1 = 0, l1 = 0, f2 = 0, l2 = 0;
    uint8_t r1 = 0, r2 = 0;

    // 鎖外讀 degraded 只當提示（讀到舊值的代價是這一輪用 RTT=0 算逾時）
    if (vrRec.degraded) {
        // §VR-LINK-CTRL：LOSS 走連線時，重送的計時也用連線的往返時間（ENet 那條變弱時 RTT 會被拉高）
        int linkRttMs = vrmlCtrlRttMs();
        rttMs = linkRttMs >= 0 ? (uint32_t)linkRttMs : vrGetEnetRttMs();
    }
    now = PltGetMillis();

    PltLockMutex(&vrRecMutex);
    if (vrRec.lossSendPending) {
        vrRec.lossSendPending = false;
        sendFirst = true;
        f1 = vrRec.firstLost;
        l1 = vrRec.lastLost;
        r1 = vrRec.lossSendReason;
        vrRec.twinPending = true;
        vrRec.twinDueMs = now + VR_LOSS_TWIN_DELAY_MS;
        vrRec.twinFirst = f1;
        vrRec.twinLast = l1;
        vrRec.twinReason = r1;
    }
    else if (vrRec.twinPending && now >= vrRec.twinDueMs) {
        vrRec.twinPending = false;
        sendTwin = true;
        f2 = vrRec.twinFirst;
        l2 = vrRec.twinLast;
        r2 = vrRec.twinReason;
    }
    action = vrEvaluateTimersLocked(now, rttMs, &firstLost, &lastLost);
    if (action == VR_ACTION_REQUEST_IDR) {
        firstLost = vrRec.firstLost;
        lastLost = vrRec.lastLost;
    }
    vrMaybeLogStatsLocked(now);
    PltUnlockMutex(&vrRecMutex);

    if (sendFirst) {
        vrSendLossOnce(f1, l1, r1);
    }
    if (sendTwin) {
        vrSendLossOnce(f2, l2, r2);
    }
    vrPerformAction(action, firstLost, lastLost);
}

static void vrSendThreadFunc(void* context) {
    while (!PltIsThreadInterrupted(&vrSendThread)) {
        vrSendTick();
        PltSleepMsInterruptible(&vrSendThread, VR_SEND_TICK_MS);
    }
}

// 控制接收執行緒：0x5508 裡的 REFRESH_START 立刻交給恢復狀態機，不等 async
// 執行緒（整則訊息之後仍照常排進 async 佇列給 client）。
static void vrHandleS2CInline(const uint8_t* tlv, int length) {
    int off = 0;
    while (off + 2 <= length) {
        uint8_t type = tlv[off];
        uint8_t len = tlv[off + 1];
        if (off + 2 + len > length) {
            break;  // 截斷的 TLV：丟棄其餘部分
        }
        if (type == VIPLE_VR_S2C_REFRESH_START && len >= sizeof(VIPLE_VR_TLV_REFRESH_START)) {
            VIPLE_VR_TLV_REFRESH_START rs;
            memcpy(&rs, &tlv[off + 2], sizeof(rs));
            vrRecoveryOnRefreshStart(rs.startFrame, rs.frameCnt);
        }
        off += 2 + len;
    }
}

// §F5（M1a）：VR tracking 走 UNSEQUENCED，ENet 預設的 throttle 會依 RTT 變異
// 機率性丟棄 unreliable 封包。VR session 把 deceleration 設 0，throttle 只升不降。
// server 收到 THROTTLE_CONFIGURE 也會套用到它那端的 peer（S→C 的 unsequenced 同樣受益）。
// 呼叫端必須持有 enetMutex，或確定沒有其他執行緒碰 peer。
void vrLinkS2C(const uint8_t* tlv, int length) {
    // 恢復狀態機自己有鎖（vrRecMutex），可以在影像接收執行緒直接呼叫
    vrHandleS2CInline(tlv, length);
}

static void configureVrThrottle(ENetPeer* p) {
    if (p == NULL || !(VrFlags & VIPLE_VR_SF_ENABLED)) {
        return;
    }
    enet_peer_throttle_configure(p, VIPLE_VR_ENET_THROTTLE_INTERVAL,
                                 VIPLE_VR_ENET_THROTTLE_ACCELERATION,
                                 VIPLE_VR_ENET_THROTTLE_DECELERATION);
    Limelog("[VIPLE-VR-SESSION] ENet throttle configured (interval=%d accel=%d decel=%d)\n",
            VIPLE_VR_ENET_THROTTLE_INTERVAL, VIPLE_VR_ENET_THROTTLE_ACCELERATION,
            VIPLE_VR_ENET_THROTTLE_DECELERATION);
}

int LiGetVrFlags(void) {
    return VrFlags;
}

int LiSendVrTracking(const VIPLE_VR_TRACKING* sample) {
    uint64_t now;

    if (!(VrFlags & VIPLE_VR_SF_ENABLED) || sample == NULL) {
        return -1;
    }

    // §VR-MULTILINK：有已確認的連線時，加密後在每條連線各送一份（server 以序號去重）；
    // 還沒建好或全部斷線時回 0，照舊走下面的 ENet。
    if (vrmlSendTracking(sample, (int)sizeof(*sample)) > 0) {
        vrTrackingConsecutiveFails = 0;
        vrTrackingBackoffUntilMs = 0;
        vrTrackingSent++;
        return 0;
    }

    // ENet 與 QUIC 都送不出去時（斷線中）暫停 1 秒，避免 2×Hz 的失敗洗 log
    now = PltGetMillis();
    if (vrTrackingBackoffUntilMs != 0 && now < vrTrackingBackoffUntilMs) {
        vrTrackingDropped++;
        return -2;
    }

    if (!sendMessageAndForget(VIPLE_VR_PTYPE_TRACKING, (short)sizeof(*sample), sample,
                              VIPLE_VR_CTRL_CHANNEL, ENET_PACKET_FLAG_UNSEQUENCED, false)) {
        vrTrackingDropped++;
        if (++vrTrackingConsecutiveFails >= VR_TRACKING_FAIL_BACKOFF) {
            vrTrackingConsecutiveFails = 0;
            vrTrackingBackoffUntilMs = now + VR_TRACKING_BACKOFF_MS;
            Limelog("[VIPLE-VR-POSE] tracking send failing — backing off %d ms (sent=%u dropped=%u)\n",
                    VR_TRACKING_BACKOFF_MS, vrTrackingSent, vrTrackingDropped);
        }
        return -3;
    }

    vrTrackingConsecutiveFails = 0;
    vrTrackingBackoffUntilMs = 0;
    vrTrackingSent++;
    return 0;
}

int LiSendVrMessage(const uint8_t* tlv, int length, bool reliable) {
    if (!(VrFlags & VIPLE_VR_SF_ENABLED) || tlv == NULL ||
            length <= 0 || length > VIPLE_VR_MAX_CTRL_PAYLOAD) {
        return -1;
    }

    // §VR-LINK-CTRL：LATCH（10 Hz 的節拍回授）在 server 同意時走連線，理由同 LOSS。其餘訊息照舊走 ENet。
    if (!reliable && length >= 2 && tlv[0] == VIPLE_VR_C2S_LATCH && 2 + (int)tlv[1] == length &&
            vrmlSendCtrl(tlv, length) > 0) {
        return 0;
    }

    return sendMessageAndForget(VIPLE_VR_PTYPE_C2S, (short)length, tlv,
                                reliable ? CTRL_CHANNEL_GENERIC : VIPLE_VR_CTRL_CHANNEL,
                                reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED,
                                false) ? 0 : -1;
}

void LiReportVrLoss(uint32_t firstLost, uint32_t lastLost, uint8_t reason) {
    if (!(VrFlags & VIPLE_VR_SF_ENABLED)) {
        return;
    }
    if (!(VrFlags & VIPLE_VR_SF_RECOVERY_INTRA)) {
        LiRequestIdrFrame();
        return;
    }
    vrRecoveryReportLoss(firstLost, lastLost, reason);
}

// Invalidate reference frames lost by the network
void connectionDetectedFrameLoss(uint32_t startFrame, uint32_t endFrame) {
    // §VR：recovery=intra 改送 LOSS，不走 RFI／IDR
    if (vrIntraRecoveryActive()) {
        vrRecoveryReportLoss(startFrame, endFrame, VIPLE_VR_LOSS_NETWORK);
        return;
    }
    queueFrameInvalidationTuple(startFrame, endFrame);
}

// When we receive a frame, update the number of our current frame
// and send ACK control message if the frame is LTR
void connectionReceivedCompleteFrame(uint32_t frameIndex, bool frameIsLTR) {
    lastGoodFrame = frameIndex;
    intervalGoodFrameCount++;

    if (frameIsLTR && IS_SUNSHINE() && isReferenceFrameInvalidationEnabled()) {
        // Queue LTR frame ACK control message
        PQUEUED_REFERENCE_FRAME_CONTROL qfit;
        qfit = malloc(sizeof(*qfit));
        if (qfit != NULL) {
            *qfit = (QUEUED_REFERENCE_FRAME_CONTROL){
                .startFrame = frameIndex,
                .invalidate = false,
            };
            if (LbqOfferQueueItem(&referenceFrameControlQueue, qfit, &qfit->entry) == LBQ_BOUND_EXCEEDED) {
                // This shouldn't happen and indicates that something has gone wrong with the queue
                LC_ASSERT(false);
                Limelog("Couldn't queue LTR ACK because the list has reached maximum size limit\n");
                free(qfit);
                LiRequestIdrFrame();
            }
        }
    }
}

void connectionSendFrameFecStatus(PSS_FRAME_FEC_STATUS fecStatus) {
    // This is a Sunshine protocol extension
    if (!IS_SUNSHINE()) {
        return;
    }

    // Queue a frame FEC status message. This is best-effort only.
    PQUEUED_FRAME_FEC_STATUS queuedFecStatus = malloc(sizeof(*queuedFecStatus));
    if (queuedFecStatus != NULL) {
        queuedFecStatus->fecStatus = *fecStatus;
        if (LbqOfferQueueItem(&frameFecStatusQueue, queuedFecStatus, &queuedFecStatus->entry) == LBQ_BOUND_EXCEEDED) {
            free(queuedFecStatus);
        }
    }
}

void connectionSawFrame(uint32_t frameIndex) {
    // §F1-DBG-ASSERT（§FRZ-B1 同類）：上游原本在這裡
    // LC_ASSERT_VT(!isBefore16(frameIndex, lastSeenFrame))——拿 16-bit 迴繞
    // 比對 32-bit 幀號，跟 §FRZ-B1 是同一個錯。§FRZ-WATCHDOG 哨兵重置後
    // RtpVideoQueue 會採納下一個抵達的幀，若那是 failback 殘留的 stale 幀，
    // Android 出貨的 debug native（assert 生效）會在這裡 SIGABRT。
    // 改為 runtime 防衛：倒退的幀號不列入統計、也不改基準（release 版原本
    // 會讓 intervalTotalFrameCount 以無號相減溢位成天文數字）。
    if (lastSeenFrame != 0 && isBefore32(frameIndex, lastSeenFrame)) {
        static uint64_t lastStaleSawLogMs;
        uint64_t nowMs = PltGetMillis();
        if (lastStaleSawLogMs == 0 || nowMs - lastStaleSawLogMs >= 1000) {
            Limelog("[VIPLE-CTRL] connectionSawFrame: stale frame=%u (lastSeen=%u) — ignored\n",
                    frameIndex, lastSeenFrame);
            lastStaleSawLogMs = nowMs;
        }
        return;
    }

    uint64_t now = PltGetMillis();

    // Suppress connection status warnings for the first sampling period
    // to allow the network and host to settle.
    if (lastSeenFrame == 0) {
        lastSeenFrame = frameIndex;
        firstFrameTimeMs = now;
        return;
    }
    else if (now - firstFrameTimeMs < CONN_STATUS_SAMPLE_PERIOD) {
        lastSeenFrame = frameIndex;
        return;
    }

    if (now - intervalStartTimeMs >= CONN_STATUS_SAMPLE_PERIOD) {
        if (intervalTotalFrameCount != 0) {
            // Notify the client of connection status changes based on frame loss rate
            int frameLossPercent = 100 - (intervalGoodFrameCount * 100) / intervalTotalFrameCount;
            if (lastConnectionStatusUpdate != CONN_STATUS_POOR &&
                    (frameLossPercent >= CONN_IMMEDIATE_POOR_LOSS_RATE ||
                     (frameLossPercent >= CONN_CONSECUTIVE_POOR_LOSS_RATE && lastIntervalLossPercentage >= CONN_CONSECUTIVE_POOR_LOSS_RATE))) {
                // We require 2 consecutive intervals above CONN_CONSECUTIVE_POOR_LOSS_RATE or a single
                // interval above CONN_IMMEDIATE_POOR_LOSS_RATE to notify of a poor connection.
                ListenerCallbacks.connectionStatusUpdate(CONN_STATUS_POOR);
                lastConnectionStatusUpdate = CONN_STATUS_POOR;
            }
            else if (frameLossPercent <= CONN_OKAY_LOSS_RATE && lastConnectionStatusUpdate != CONN_STATUS_OKAY) {
                ListenerCallbacks.connectionStatusUpdate(CONN_STATUS_OKAY);
                lastConnectionStatusUpdate = CONN_STATUS_OKAY;
            }

            lastIntervalLossPercentage = frameLossPercent;
        }

        // Reset interval
        intervalStartTimeMs = now;
        intervalGoodFrameCount = intervalTotalFrameCount = 0;
    }

    intervalTotalFrameCount += frameIndex - lastSeenFrame;
    lastSeenFrame = frameIndex;
}

// Reads an NV control stream packet from the TCP connection
static PNVCTL_TCP_PACKET_HEADER readNvctlPacketTcp(void) {
    NVCTL_TCP_PACKET_HEADER staticHeader;
    PNVCTL_TCP_PACKET_HEADER fullPacket;
    SOCK_RET err;

    err = recv(ctlSock, (char*)&staticHeader, sizeof(staticHeader), 0);
    if (err != sizeof(staticHeader)) {
        return NULL;
    }

    staticHeader.type = LE16(staticHeader.type);
    staticHeader.payloadLength = LE16(staticHeader.payloadLength);

    fullPacket = (PNVCTL_TCP_PACKET_HEADER)malloc(staticHeader.payloadLength + sizeof(staticHeader));
    if (fullPacket == NULL) {
        return NULL;
    }

    memcpy(fullPacket, &staticHeader, sizeof(staticHeader));
    if (staticHeader.payloadLength != 0) {
        err = recv(ctlSock, (char*)(fullPacket + 1), staticHeader.payloadLength, 0);
        if (err != staticHeader.payloadLength) {
            free(fullPacket);
            return NULL;
        }
    }

    return fullPacket;
}

static bool encryptControlMessage(PNVCTL_ENCRYPTED_PACKET_HEADER encPacket, PNVCTL_ENET_PACKET_HEADER_V2 packet) {
    unsigned char iv[16] = { 0 };
    int ivSize;
    int encryptedSize = sizeof(*packet) + packet->payloadLength;

    // NB: Setting the IV must happen while encPacket->seq is still in native byte-order!
    if (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) {
        // Populate the IV in little endian byte order
        iv[3] = (unsigned char)(encPacket->seq >> 24);
        iv[2] = (unsigned char)(encPacket->seq >> 16);
        iv[1] = (unsigned char)(encPacket->seq >> 8);
        iv[0] = (unsigned char)(encPacket->seq >> 0);

        // Set high bytes to something unique to ensure no IV collisions
        iv[10] = (unsigned char)'C'; // Client originated
        iv[11] = (unsigned char)'C'; // Control stream

        // Use 12-byte IV which is ideal for AES-GCM
        ivSize = 12;
    }
    else {
        // This is a truncating cast, but it's what Nvidia does, so we have to mimic it.
        iv[0] = (unsigned char)encPacket->seq;

        // Nvidia's old style encryption uses a 16-byte IV
        ivSize = 16;
    }

    encPacket->encryptedHeaderType = LE16(encPacket->encryptedHeaderType);
    encPacket->length = LE16(encPacket->length);
    encPacket->seq = LE32(encPacket->seq);

    packet->type = LE16(packet->type);
    packet->payloadLength = LE16(packet->payloadLength);

    LC_ASSERT(ivSize <= (int)sizeof(iv));
    LC_ASSERT(ivSize == 12 || ivSize == 16);
    return PltEncryptMessage(encryptionCtx, ALGORITHM_AES_GCM, 0,
                             (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                             iv, ivSize,
                             (unsigned char*)(encPacket + 1), AES_GCM_TAG_LENGTH, // Write tag into the space after the encrypted header
                             (unsigned char*)packet, encryptedSize,
                             ((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH, &encryptedSize); // Write ciphertext after the GCM tag
}

// Caller must free() *packet on success!!!
static bool decryptControlMessageToV1(PNVCTL_ENCRYPTED_PACKET_HEADER encPacket, int encPacketLength, PNVCTL_ENET_PACKET_HEADER_V1* packet, int* packetLength) {
    unsigned char iv[16] = { 0 };
    int ivSize;

    *packet = NULL;

    // It must be an encrypted packet to begin with
    LC_ASSERT(encPacket->encryptedHeaderType == 0x0001);

    // Make sure the host isn't lying to us about the packet length
    int expectedEncLength = encPacket->length + sizeof(encPacket->encryptedHeaderType) + sizeof(encPacket->length);
    LC_ASSERT(encPacketLength == expectedEncLength);
    if (encPacketLength < expectedEncLength) {
        Limelog("Length exceeds packet boundary (needed %d, got %d)\n", expectedEncLength, encPacketLength);
        return false;
    }

    // Check length first so we don't underflow
    if (encPacket->length < sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + sizeof(NVCTL_ENET_PACKET_HEADER_V2)) {
        Limelog("Received runt packet (%d). Unable to decrypt.\n", encPacket->length);
        return false;
    }

    if (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2) {
        // Populate the IV in little endian byte order
        iv[3] = (unsigned char)(encPacket->seq >> 24);
        iv[2] = (unsigned char)(encPacket->seq >> 16);
        iv[1] = (unsigned char)(encPacket->seq >> 8);
        iv[0] = (unsigned char)(encPacket->seq >> 0);

        // Set high bytes to something unique to ensure no IV collisions
        iv[10] = (unsigned char)'H'; // Host originated
        iv[11] = (unsigned char)'C'; // Control stream

        // Use 12-byte IV which is ideal for AES-GCM
        ivSize = 12;
    }
    else {
        // This is a truncating cast, but it's what Nvidia does, so we have to mimic it.
        iv[0] = (unsigned char)encPacket->seq;

        // Nvidia's old style encryption uses a 16-byte IV
        ivSize = 16;
    }

    int plaintextLength = encPacket->length - sizeof(encPacket->seq) - AES_GCM_TAG_LENGTH;
    *packet = malloc(plaintextLength);
    if (*packet == NULL) {
        return false;
    }

    LC_ASSERT(ivSize <= (int)sizeof(iv));
    LC_ASSERT(ivSize == 12 || ivSize == 16);
    if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                           (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                           iv, ivSize,
                           (unsigned char*)(encPacket + 1), AES_GCM_TAG_LENGTH, // The tag is located right after the header
                           ((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH, plaintextLength, // The ciphertext is after the tag
                           (unsigned char*)*packet, &plaintextLength)) {
        free(*packet);
        return false;
    }

    // Now we do an in-place V2 to V1 header conversion, so our existing parsing code doesn't have to change.
    // All we need to do is eliminate the new length field in V2 by shifting everything by 2 bytes.
    memmove(((unsigned char*)*packet) + 2, ((unsigned char*)*packet) + 4, plaintextLength - 4);
    *packetLength = plaintextLength - 2;

    return true;
}

static void enetPacketFreeCb(ENetPacket* packet) {
    if (packet->userData) {
        // userData contains a bool that we will set when freed
        *(volatile bool*)packet->userData = true;
    }
}


// Must be called with enetMutex held
static bool isPacketSentWaitingForAck(ENetPacket* packet) {
    ENetOutgoingCommand* outgoingCommand = NULL;
    ENetListIterator currentCommand;

    // Look for our packet on the sent commands list
    for (currentCommand = enet_list_begin(&peer->sentReliableCommands);
         currentCommand != enet_list_end(&peer->sentReliableCommands);
         currentCommand = enet_list_next(currentCommand))
    {
        outgoingCommand = (ENetOutgoingCommand*)currentCommand;
        if (outgoingCommand->packet == packet) {
            return true;
        }
    }

    return false;
}

static bool sendMessageEnet(short ptype, short paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    ENetPacket* enetPacket;
    int err;

    LC_ASSERT(AppVersionQuad[0] >= 5);

    // Only send reliable packets to GFE
    if (!IS_SUNSHINE()) {
        flags = ENET_PACKET_FLAG_RELIABLE;
    }

    if (encryptedControlStream) {
        PNVCTL_ENCRYPTED_PACKET_HEADER encPacket;
        PNVCTL_ENET_PACKET_HEADER_V2 packet;
        char tempBuffer[256];

        // §F2（2.0 M0）：明文用固定 256 B 的 stack buffer 組裝，上游只靠
        // LC_ASSERT 把關——release 建置的 LC_ASSERT 是空巨集，超長 payload
        // 會直接 memcpy 溢位 stack。2.0 的 VR tracking（0x5506）加 V2 header
        // 共 236 B，已逼近上限，所以改成 runtime 檢查：超長就記 log、回傳
        // false、不送出。上限維持上游斷言的「嚴格小於」（payload ≤ 251 B），
        // 與 docs/vr_protocol.md §4.3／§4.5 的「上限 251 B」一致。
        //
        // 檢查刻意放在 enet_packet_create 與 currentEnetSequenceNumber++
        // 之前：拒送時不配置封包、不持 enetMutex、不消耗加密序號（IV），
        // 也不會落到下方的 QUIC datagram fallback（那條送的是已加密的
        // enetPacket->data，同樣受此檢查保護）。paylen 為負值一併擋下，
        // 避免轉成 size_t 後相加迴繞而繞過檢查。
        //
        // 呼叫端語意：false 一律代表「這則訊息沒送出」。現有呼叫端的
        // payload 長度都是編譯期固定且遠小於上限（最大是 input，
        // MAX_INPUT_PACKET_SIZE 128 + 4 B），此分支對它們不可達；2.0 新增的
        // VR 送出 API 必須把 false 當成「丟棄這筆樣本」，不可據此終止連線。
        if (paylen < 0 || sizeof(*packet) + (size_t)paylen >= sizeof(tempBuffer)) {
            // 節流：前 10 次都印，之後每 1000 次印一次（VR tracking 最高
            // 2×顯示 Hz，程式錯誤時不能每包都洗 log）。計數器跨執行緒
            // 無鎖，只影響 log 取樣，無害。
            static unsigned int f2OversizeCount;
            unsigned int count = ++f2OversizeCount;
            if (count <= 10 || count % 1000 == 0) {
                Limelog("[VIPLE-CTRL] §F2 payload too large (ptype=0x%04x len=%d) "
                        "— limit %d, dropped (count=%u)\n",
                        (unsigned int)(unsigned short)ptype, (int)paylen,
                        (int)(sizeof(tempBuffer) - sizeof(*packet) - 1), count);
            }
            return false;
        }

        enetPacket = enet_packet_create(NULL,
                                        sizeof(*encPacket) + AES_GCM_TAG_LENGTH + sizeof(*packet) + paylen,
                                        flags);
        if (enetPacket == NULL) {
            return false;
        }

        // We (ab)use the enetMutex to protect currentEnetSequenceNumber and the cipherContext
        // used inside encryptControlMessage().
        PltLockMutex(&enetMutex);

        encPacket = (PNVCTL_ENCRYPTED_PACKET_HEADER)enetPacket->data;
        encPacket->encryptedHeaderType = 0x0001;
        encPacket->length = sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + sizeof(*packet) + paylen;
        encPacket->seq = currentEnetSequenceNumber++;

        // Construct the plaintext data for encryption
        // §F2：長度已在函式前段做 runtime 檢查（原本只有 LC_ASSERT）。
        packet = (PNVCTL_ENET_PACKET_HEADER_V2)tempBuffer;
        packet->type = ptype;
        packet->payloadLength = paylen;
        memcpy(&packet[1], payload, paylen);

        // Encrypt the data into the final packet (and byteswap for BE machines)
        if (!encryptControlMessage(encPacket, packet)) {
            Limelog("Failed to encrypt control stream message\n");
            enet_packet_destroy(enetPacket);
            PltUnlockMutex(&enetMutex);
            return false;
        }

        // enetMutex still locked here
    }
    else {
        PNVCTL_ENET_PACKET_HEADER_V1 packet;
        enetPacket = enet_packet_create(NULL, sizeof(*packet) + paylen,
                                        flags);
        if (enetPacket == NULL) {
            return false;
        }

        packet = (PNVCTL_ENET_PACKET_HEADER_V1)enetPacket->data;
        packet->type = LE16(ptype);
        memcpy(&packet[1], payload, paylen);

        PltLockMutex(&enetMutex);
    }

#ifdef VIPLE_MPQUIC
    // §Q-ENET-RECONNECT v1.5.183: peer/client 在 reconnect 等待期間
    // 被銷毀。其他 thread（lossStats 每 100ms、asyncCallback）仍會
    // 呼叫 sendMessageEnet，必須在解引用 peer 前檢查。
    //
    // §M01-C：再加上「重連流程進行中（Fix G 的 500 ms 窗口＋整段重連期）
    // 且 QUIC fallback 可用」。Fix G 窗口內 peer 仍非 NULL，但 ENet 已判死：
    //  - 從 socket-error GRACE 進來時 peer 仍是 CONNECTED，可能 packetQueued
    //    後 service 失敗（err<0）→ 下方 Fix K.2 只在 !packetQueued 才走 QUIC
    //    → 回 false → 舊 InputStream 在「沒有 failover」時直接
    //    connectionTerminated；
    //  - 從 disconnect-timeout GRACE 進來時封包會送進 zombie peer，peer
    //    reset 時整批遺失。
    // 兩種都改成直接走 QUIC flow 0x04（本體不變）。這裡已持 enetMutex，
    // 重連路徑持鎖銷毀 peer/client，讀到的旗標與 peer 一致。
    // IDR 不受影響：requestIdrFrame 在 enetControlChannelDown() 時先走
    // QUIC stream #0 的 'I' marker。
    bool enetObjectsNull = (!peer || !client);
    if (enetObjectsNull ||
        ((enetReconnectPending || enetReconnecting) && quicControlFallbackAvailable())) {
        PltUnlockMutex(&enetMutex);
        // §Q-INPUT-QUIC-FALLBACK v1.5.189 Fix K: ENet 不可用時
        // 改走 QUIC datagram。加密已在上方完成，enetPacket->data
        // 包含完整的 NVCTL encrypted header + AES-GCM tag + ciphertext。
        // §Q-REMOTE Fix R.3：放寬到「QUIC 活著」即可（不限 failover）。
        if (quicControlFallbackAvailable()) {
            // Fix L.2 v1.5.193: diagnostic log（one-shot，避免每封包都印）
            static int nullGuardLogCount = 0;
            if (nullGuardLogCount < 3) {
                Limelog("[VIPLE-MPQUIC] §Q-INPUT-QUIC-FALLBACK: %s, sending "
                        "via QUIC datagram (count=%d)\n",
                        enetObjectsNull ? "peer/client NULL"
                                        : "ENet reconnect in progress (§M01-C)",
                        ++nullGuardLogCount);
            }
            int ret = quicSendDatagram(QUIC_FLOW_INPUT,
                                        enetPacket->data,
                                        (int)enetPacket->dataLength);
            enet_packet_destroy(enetPacket);
            if (ret == 0) {
                return true;
            }
            // QUIC 也失敗 → fall through 到 return false
            Limelog("[VIPLE-MPQUIC] §Q-INPUT-QUIC-FALLBACK: QUIC send also "
                    "failed (ret=%d)\n", ret);
            return false;
        }
        enet_packet_destroy(enetPacket);
        return false;
    }
#endif

#ifdef VIPLE_MPQUIC
    // §Q-FAILOVER-BYPASS v1.5.192 Fix L：在 QUIC failover 期間，
    // ENet 可能處於 zombie 狀態（peer/client 非 NULL、enet_peer_send
    // 不報錯，但封包被 OS 靜默丟棄，永遠到不了 server）。
    // 直接走 QUIC datagram，不浪費時間嘗試 zombie ENet。
    // 涵蓋 IDR request、input、所有 control message。
    //
    // Fix M v1.5.194: 加入 quicIsPrimaryPathUnhealthy() 二次驗證。
    // DYN-STANDBY-ALIVE 可能在 primary 短暫 stale 時假性觸發 failover
    // （v1.5.193 regression），此時 primary path 實際上仍健康
    // （consecutiveTimeouts==0, active==true）→ 不應繞過 ENet。
    if (quicIsFailoverActive() && quicIsPrimaryPathUnhealthy()) {
        PltUnlockMutex(&enetMutex);
        // Fix L.2 v1.5.193: diagnostic log（one-shot，避免每封包都印）
        // bypassLogCount 已提升到 file-scope（Fix P.2），ENet 重連時重置
        if (bypassLogCount < 3) {
            Limelog("[VIPLE-MPQUIC] §Q-FAILOVER-BYPASS: Fix L active — "
                    "ENet zombie bypass, sending via QUIC datagram "
                    "(count=%d)\n", ++bypassLogCount);
        }
        int ret = quicSendDatagram(QUIC_FLOW_INPUT,
                                    enetPacket->data,
                                    (int)enetPacket->dataLength);
        enet_packet_destroy(enetPacket);
        if (ret != 0) {
            Limelog("[VIPLE-MPQUIC] §Q-FAILOVER-BYPASS: QUIC send failed "
                    "(ret=%d)\n", ret);
        }
        return (ret == 0);
    }
#endif

    volatile bool packetFreed = false;

    // Set a callback to use to let us know if the packet has been freed.
    // Freeing can only happen when the packet is acked or send fails.
    enetPacket->userData = (void*)&packetFreed;
    enetPacket->freeCallback = enetPacketFreeCb;

    // Always use channel 0 for GFE and if the requested channel exceeds
    // the peer's supported channel count.
    if (!IS_SUNSHINE() || channelId >= peer->channelCount) {
        channelId = 0;
    }

    // Queue the packet to be sent
    err = enet_peer_send(peer, channelId, enetPacket);
    bool packetQueued = (err == 0);

    // If there is no more data coming soon, send the packet now
    if (!moreData && packetQueued) {
        err = enet_host_service(client, NULL, 0);

        // Wait until the packet is actually sent to provide backpressure on senders
        if (flags & ENET_PACKET_FLAG_RELIABLE) {
            // Don't wait longer than 10 milliseconds to avoid blocking callers for too long
            for (int i = 0; err >= 0 && i < 10; i++) {
                // Break on disconnected, acked/freed, or sent (pending ack).
                if (peer->state != ENET_PEER_STATE_CONNECTED || packetFreed || isPacketSentWaitingForAck(enetPacket)) {
                    break;
                }

                // Release the lock before sleeping to allow another thread to send/receive
                PltUnlockMutex(&enetMutex);
                PltSleepMs(1);
                PltLockMutex(&enetMutex);

                // §Q-ENET-RECONNECT-GUARD (review)：放鎖睡眠期間
                // §Q-ENET-RECONNECT 路徑可能在持鎖下 enet_peer_reset(peer)
                // + enet_host_destroy(client) 並設 NULL。重取鎖後若資源已
                // 被銷毀，enet_host_service(NULL) 會立即 NULL 解參考、
                // 讀 peer->state 也是 NULL deref。入口 null-guard 只檢查一次、
                // 覆蓋不到迴圈內（對照 flushInputOnControlStream 的 Fix J）。
                if (!peer || !client) {
                    err = -1;
                    break;
                }

                // Try to send the packet again
                err = enet_host_service(client, NULL, 0);
            }

            if (peer && err >= 0 && peer->state == ENET_PEER_STATE_CONNECTED && !packetFreed && !isPacketSentWaitingForAck(enetPacket)) {
                Limelog("Control message took over 10 ms to send (net latency: %u ms | packet loss: %f%%)\n",
                        peer->roundTripTime, peer->packetLoss / (float)ENET_PEER_PACKET_LOSS_SCALE);
            }
        }
    }

    // Remove the free callback now that the packet was sent
    if (!packetFreed) {
        enetPacket->userData = NULL;
        enetPacket->freeCallback = NULL;
    }

    PltUnlockMutex(&enetMutex);

    if (err < 0) {
        Limelog("Failed to send ENet control packet\n");
#ifdef VIPLE_MPQUIC
        // §Q-INPUT-QUIC-FALLBACK v1.5.191 Fix K.2：enet_peer_send 失敗時
        // 也嘗試 QUIC datagram fallback。Fix K 只在 null guard 攔截，但
        // peer/client 在重連嘗試期間非 NULL（controlReceiveThread 已重建），
        // enet_peer_send 才是實際失敗點。enetMutex 已在上方 unlock。
        // §Q-REMOTE Fix R.3：放寬到「QUIC 活著」即可（不限 failover）。
        if (!packetQueued && quicControlFallbackAvailable()) {
            int ret = quicSendDatagram(QUIC_FLOW_INPUT,
                                        enetPacket->data,
                                        (int)enetPacket->dataLength);
            enet_packet_destroy(enetPacket);
            if (ret == 0) {
                return true;  // 走 QUIC 成功 → inputSendThread 繼續正常跑
            }
            return false;
        }
#endif
        if (!packetQueued) {
            enet_packet_destroy(enetPacket);
        }
        return false;
    }

    return true;
}

static bool sendMessageTcp(short ptype, short paylen, const void* payload) {
    PNVCTL_TCP_PACKET_HEADER packet;
    SOCK_RET err;

    LC_ASSERT(AppVersionQuad[0] < 5);

    packet = malloc(sizeof(*packet) + paylen);
    if (packet == NULL) {
        return false;
    }

    packet->type = LE16(ptype);
    packet->payloadLength = LE16(paylen);
    memcpy(&packet[1], payload, paylen);

    err = send(ctlSock, (char*) packet, sizeof(*packet) + paylen, 0);
    free(packet);

    if (err != (SOCK_RET)(sizeof(*packet) + paylen)) {
        return false;
    }

    return true;
}

static bool sendMessageAndForget(short ptype, short paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    bool ret;

    // Unlike regular sockets, ENet sockets aren't safe to invoke from multiple
    // threads at once. We have to synchronize them with a lock.
    if (AppVersionQuad[0] >= 5) {
        ret = sendMessageEnet(ptype, paylen, payload, channelId, flags, moreData);
    }
    else {
        ret = sendMessageTcp(ptype, paylen, payload);
    }

    return ret;
}

static bool sendMessageAndDiscardReply(short ptype, short paylen, const void* payload, uint8_t channelId, uint32_t flags, bool moreData) {
    if (AppVersionQuad[0] >= 5) {
        if (!sendMessageEnet(ptype, paylen, payload, channelId, flags, moreData)) {
            return false;
        }
    }
    else {
        PNVCTL_TCP_PACKET_HEADER reply;

        if (!sendMessageTcp(ptype, paylen, payload)) {
            return false;
        }

        // Discard the response
        reply = readNvctlPacketTcp();
        if (reply == NULL) {
            return false;
        }

        free(reply);
    }

    return true;
}

// This intercept function drops disconnect events to allow us to process
// pending receives first. It works around what appears to be a bug in ENet
// where pending disconnects can cause loss of unprocessed received data.
static int ignoreDisconnectIntercept(ENetHost* host, ENetEvent* event) {
    if (host->receivedDataLength == sizeof(ENetProtocolHeader) + sizeof(ENetProtocolDisconnect)) {
        ENetProtocolHeader* protoHeader = (ENetProtocolHeader*)host->receivedData;
        ENetProtocolDisconnect* disconnect = (ENetProtocolDisconnect*)(protoHeader + 1);

        if ((disconnect->header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT) {
            Limelog("ENet disconnect event pending\n");
            disconnectPending = true;
            if (event) {
                event->type = ENET_EVENT_TYPE_NONE;
            }
            return 1;
        }
    }

    return 0;
}

static void asyncCallbackThreadFunc(void* context) {
    PQUEUED_ASYNC_CALLBACK queuedCb, nextCb;

    while (LbqWaitForQueueElement(&asyncCallbackQueue, (void**)&queuedCb) == LBQ_SUCCESS) {
        switch (queuedCb->typeIndex) {
        case IDX_RUMBLE_DATA:
            // Look for another rumble packet to batch with
            while (LbqPeekQueueElement(&asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.rumble.controllerNumber != queuedCb->data.rumble.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.rumble(queuedCb->data.rumble.controllerNumber,
                                     queuedCb->data.rumble.lowFreqRumble,
                                     queuedCb->data.rumble.highFreqRumble);
            break;
        case IDX_RUMBLE_TRIGGER_DATA:
            // Look for another rumble triggers packet to batch with
            while (LbqPeekQueueElement(&asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.rumbleTriggers.controllerNumber != queuedCb->data.rumbleTriggers.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.rumbleTriggers(queuedCb->data.rumbleTriggers.controllerNumber,
                                             queuedCb->data.rumbleTriggers.leftTriggerMotor,
                                             queuedCb->data.rumbleTriggers.rightTriggerMotor);
            break;
        case IDX_SET_RGB_LED:
            // Look for another controller LED packet to batch with
            while (LbqPeekQueueElement(&asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS) {
                // Don't batch with the next packet if it is a different type or controller number
                if (nextCb->typeIndex != queuedCb->typeIndex ||
                        nextCb->data.setControllerLed.controllerNumber != queuedCb->data.setControllerLed.controllerNumber) {
                    break;
                }

                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.setControllerLED(queuedCb->data.setControllerLed.controllerNumber,
                                               queuedCb->data.setControllerLed.r,
                                               queuedCb->data.setControllerLed.g,
                                               queuedCb->data.setControllerLed.b);
            break;
        case IDX_HDR_INFO:
            // HDR state is maintained globally, so we just invoke the client callback here.
            // These events are stateless, so we can consume all of them now.
            while (LbqPeekQueueElement(&asyncCallbackQueue, (void**)&nextCb) == LBQ_SUCCESS && nextCb->typeIndex == queuedCb->typeIndex) {
                // This entry is batchable, so pop it off the queue
                if (LbqPollQueueElement(&asyncCallbackQueue, (void**)&nextCb) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the old entry with the new one
                free(queuedCb);
                queuedCb = nextCb;
            }

            ListenerCallbacks.setHdrMode(hdrEnabled);
            break;

        case IDX_SET_MOTION_EVENT:
            // These events are infrequent and cannot be batched
            ListenerCallbacks.setMotionEventState(queuedCb->data.setMotionEventState.controllerNumber,
                                                  queuedCb->data.setMotionEventState.motionType,
                                                  queuedCb->data.setMotionEventState.reportRateHz);
            break;
        case IDX_DS_ADAPTIVE_TRIGGERS:
            ListenerCallbacks.setAdaptiveTriggers(queuedCb->data.dsAdaptiveTrigger.controllerNumber,
                                                  queuedCb->data.dsAdaptiveTrigger.eventFlags,
                                                  queuedCb->data.dsAdaptiveTrigger.typeLeft,
                                                  queuedCb->data.dsAdaptiveTrigger.typeRight,
                                                  queuedCb->data.dsAdaptiveTrigger.left,
                                                  queuedCb->data.dsAdaptiveTrigger.right);
            break;
        case IDX_SC_HID_FEATURE_REQ:
            // §SC-HID: forward to sc_hid.cpp to probe real SC and send response
            if (ListenerCallbacks.scHidFeatureRequest != NULL) {
                ListenerCallbacks.scHidFeatureRequest(queuedCb->data.scHidFeatureRequest.reportId,
                                                      queuedCb->data.scHidFeatureRequest.op,
                                                      queuedCb->data.scHidFeatureRequest.seq,
                                                      queuedCb->data.scHidFeatureRequest.query,
                                                      queuedCb->data.scHidFeatureRequest.queryLen);
            }
            break;
        case IDX_VIPLE_VR_S2C:
            // §VR：fixupMissingCallbacks 保證 vrMessage 非 NULL
            ListenerCallbacks.vrMessage(queuedCb->data.vrS2C.buf, queuedCb->data.vrS2C.len);
            break;
        default:
            // Unhandled packet type from queueAsyncCallback()
            LC_ASSERT(false);
            break;
        }

        free(queuedCb);
    }
}

static bool needsAsyncCallback(unsigned short packetType) {
    return packetType == packetTypes[IDX_RUMBLE_DATA] ||
           packetType == packetTypes[IDX_RUMBLE_TRIGGER_DATA] ||
           packetType == packetTypes[IDX_SET_MOTION_EVENT] ||
           packetType == packetTypes[IDX_SET_RGB_LED] ||
           packetType == packetTypes[IDX_HDR_INFO] ||
           packetType == packetTypes[IDX_DS_ADAPTIVE_TRIGGERS] ||
           (packetTypes[IDX_SC_HID_FEATURE_REQ] != -1 && packetType == packetTypes[IDX_SC_HID_FEATURE_REQ]) ||
           // §VR：只有協商過的 VR session 才收 0x5508；一般 session 照舊忽略
           ((VrFlags & VIPLE_VR_SF_ENABLED) && packetType == (unsigned short)VIPLE_VR_PTYPE_S2C);
}

static void queueAsyncCallback(PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr, int packetLength) {
    BYTE_BUFFER bb;
    PQUEUED_ASYNC_CALLBACK queuedCb;
    int err;

    LC_ASSERT(needsAsyncCallback(ctlHdr->type));

    queuedCb = malloc(sizeof(*queuedCb));
    if (!queuedCb) {
        return;
    }

    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);

    if (ctlHdr->type == packetTypes[IDX_RUMBLE_DATA]) {
        BbAdvanceBuffer(&bb, 4);

        BbGet16(&bb, &queuedCb->data.rumble.controllerNumber);
        BbGet16(&bb, &queuedCb->data.rumble.lowFreqRumble);
        BbGet16(&bb, &queuedCb->data.rumble.highFreqRumble);

        queuedCb->typeIndex = IDX_RUMBLE_DATA;
    }
    else if (ctlHdr->type == packetTypes[IDX_RUMBLE_TRIGGER_DATA]) {
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.controllerNumber);
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.leftTriggerMotor);
        BbGet16(&bb, &queuedCb->data.rumbleTriggers.rightTriggerMotor);

        queuedCb->typeIndex = IDX_RUMBLE_TRIGGER_DATA;
    }
    else if (ctlHdr->type == packetTypes[IDX_SET_MOTION_EVENT]) {
        BbGet16(&bb, &queuedCb->data.setMotionEventState.controllerNumber);
        BbGet16(&bb, &queuedCb->data.setMotionEventState.reportRateHz);
        BbGet8(&bb, &queuedCb->data.setMotionEventState.motionType);

        queuedCb->typeIndex = IDX_SET_MOTION_EVENT;
    }
    else if (ctlHdr->type == packetTypes[IDX_SET_RGB_LED]) {
        BbGet16(&bb, &queuedCb->data.setControllerLed.controllerNumber);
        BbGet8(&bb, &queuedCb->data.setControllerLed.r);
        BbGet8(&bb, &queuedCb->data.setControllerLed.g);
        BbGet8(&bb, &queuedCb->data.setControllerLed.b);

        queuedCb->typeIndex = IDX_SET_RGB_LED;
    }
    else if (ctlHdr->type == packetTypes[IDX_HDR_INFO]) {
        queuedCb->typeIndex = IDX_HDR_INFO;
    }
    else if (ctlHdr->type == packetTypes[IDX_DS_ADAPTIVE_TRIGGERS]){
        BbGet16(&bb, &queuedCb->data.dsAdaptiveTrigger.controllerNumber);
        BbGet8(&bb, &queuedCb->data.dsAdaptiveTrigger.eventFlags);
        BbGet8(&bb, &queuedCb->data.dsAdaptiveTrigger.typeLeft);
        BbGet8(&bb, &queuedCb->data.dsAdaptiveTrigger.typeRight);

        BbGetBytes(&bb, queuedCb->data.dsAdaptiveTrigger.left, DS_EFFECT_PAYLOAD_SIZE);
        BbGetBytes(&bb, queuedCb->data.dsAdaptiveTrigger.right, DS_EFFECT_PAYLOAD_SIZE);
        queuedCb->typeIndex = IDX_DS_ADAPTIVE_TRIGGERS;
    }
    else if (packetTypes[IDX_SC_HID_FEATURE_REQ] != -1 &&
             ctlHdr->type == packetTypes[IDX_SC_HID_FEATURE_REQ]) {
        // §SC-HID Phase 2C: host relays a Steam feature op (op + seq + Steam's query)
        BbGet8(&bb, &queuedCb->data.scHidFeatureRequest.reportId);
        BbGet8(&bb, &queuedCb->data.scHidFeatureRequest.op);
        BbGet8(&bb, &queuedCb->data.scHidFeatureRequest.seq);
        BbGet8(&bb, &queuedCb->data.scHidFeatureRequest.queryLen);
        BbGetBytes(&bb, queuedCb->data.scHidFeatureRequest.query, 64);
        queuedCb->typeIndex = IDX_SC_HID_FEATURE_REQ;
    }
    else if ((VrFlags & VIPLE_VR_SF_ENABLED) && ctlHdr->type == (unsigned short)VIPLE_VR_PTYPE_S2C) {
        int len = packetLength - (int)sizeof(*ctlHdr);
        if (len <= 0 || len > VIPLE_VR_MAX_CTRL_PAYLOAD) {
            static uint64_t lastBadLenLogMs;
            uint64_t nowMs = PltGetMillis();
            if (lastBadLenLogMs == 0 || nowMs - lastBadLenLogMs >= 1000) {
                Limelog("[VIPLE-VR-SESSION] dropping VR_S2C with bad length %d\n", len);
                lastBadLenLogMs = nowMs;
            }
            free(queuedCb);
            return;
        }
        // §VR-MULTILINK：LINK_READY 由 common-c 自己處理（其餘 subtype 照舊交給 app）
        vrmlOnS2C((const unsigned char*)(ctlHdr + 1), len);
        queuedCb->data.vrS2C.len = (uint8_t)len;
        memcpy(queuedCb->data.vrS2C.buf, (const uint8_t*)(ctlHdr + 1), (size_t)len);
        queuedCb->typeIndex = IDX_VIPLE_VR_S2C;
    }
    else {
        // Unhandled packet type from needsAsyncCallback()
        LC_ASSERT(false);
        free(queuedCb);
        return;
    }

    err = LbqOfferQueueItem(&asyncCallbackQueue, queuedCb, &queuedCb->entry);
    if (err != LBQ_SUCCESS) {
        Limelog("Failed to queue async callback: %d\n", err);
        free(queuedCb);
    }
}

#ifdef VIPLE_MPQUIC
// §M01-C：比對兩個位址的 IP（不比 port——探測 socket 每次拿到的 port 都是
// OS 隨機分配的）。family 不同（含全 0 的初值）一律視為不同。
static bool enetReconnectSameIp(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    if (a->ss_family != b->ss_family) {
        return false;
    }
    if (a->ss_family == AF_INET) {
        return ((const struct sockaddr_in*)a)->sin_addr.s_addr ==
               ((const struct sockaddr_in*)b)->sin_addr.s_addr;
    }
#ifdef AF_INET6
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)a;
        const struct sockaddr_in6* b6 = (const struct sockaddr_in6*)b;
        return memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(a6->sin6_addr)) == 0 &&
               a6->sin6_scope_id == b6->sin6_scope_id;
    }
#endif
    return false;
}

// §M01-C：ENet 重連前的路由探測。對 RemoteAddr:ControlPortNumber 做 UDP
// connect()+getsockname()，不綁定、不送封包，只問 OS「此刻會從哪個本機
// 位址去 host」——與 session 起點 Connection.c 決定 LocalAddr
//（getLocalAddressByUdpConnect）同一套語意，所以重連選到的介面就是「此刻
// 做初次連線會選的那張」（有線在就是有線；只剩 Wi-Fi 就是 Wi-Fi；換了 IP
// 就跟著換）。
// 不用 lcEnumNetInterfaces()：Android 那份是 session 起點的 JNI 快照、session
// 中永不更新；Linux 把 docker0／virbr0 等未知介面都分類成 ETHERNET；都不能
// 拿來判斷「網路回來了」。
// 回傳 0 = 有路由（*localOut 為實際本機位址）；非 0 = 沒有路由（socket 錯誤碼，
// 例如 WSAENETUNREACH 10051／ENETUNREACH；-1 = OS 給了未指定位址）。
// 不印 log（重連迴圈每秒呼叫；狀態轉換由呼叫端印）——所以直接用 socket()
// 而不是會印「socket() failed」的 createSocket()。
static int enetReconnectProbeRoute(struct sockaddr_storage* localOut, SOCKADDR_LEN* localLenOut) {
    LC_SOCKADDR target;
    SOCKET s;
    int err;

    s = socket(RemoteAddr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        err = LastSocketFail();
        return err;
    }

    memcpy(&target, &RemoteAddr, AddrLen);
    SET_PORT(&target, ControlPortNumber);
    if (connect(s, (struct sockaddr*)&target, AddrLen) < 0) {
        err = LastSocketFail();
        closeSocket(s);
        return err;
    }

    memset(localOut, 0, sizeof(*localOut));
    *localLenOut = sizeof(*localOut);
    if (getsockname(s, (struct sockaddr*)localOut, localLenOut) < 0) {
        err = LastSocketFail();
        closeSocket(s);
        return err;
    }
    closeSocket(s);

    // 未指定位址（0.0.0.0／::）視同沒有路由
    if (localOut->ss_family == AF_INET) {
        return ((struct sockaddr_in*)localOut)->sin_addr.s_addr != 0 ? 0 : -1;
    }
#ifdef AF_INET6
    if (localOut->ss_family == AF_INET6) {
        const unsigned char* b = (const unsigned char*)&((struct sockaddr_in6*)localOut)->sin6_addr;
        for (size_t i = 0; i < sizeof(struct in6_addr); i++) {
            if (b[i] != 0) {
                return 0;
            }
        }
        return -1;
    }
#endif
    return -1;
}

// §M01-C：連線嘗試失敗後的指數退避（1→2→4→8→10 s）。host 連續拒絕 ≥20 次
//（stale peer 遲遲不清）時上限放寬到 30 s，降低 host「Rejected connection」
// 警告的 log 量。
static uint32_t enetReconnectNextBackoff(uint32_t curMs, int consecRejects) {
    uint32_t capMs = consecRejects >= 20 ? 30000 : 10000;
    uint32_t nextMs = curMs * 2;
    return nextMs > capMs ? capMs : nextMs;
}
#endif

static void controlReceiveThreadFunc(void* context) {
    int err;
#ifdef VIPLE_MPQUIC
    // §M01-C：重連成功 5 s 後在 control thread 持鎖讀一次 peer，印出
    // 「ENet healthy rtt=… inTransit=…」當作 ENet 雙向可用的正面證據
    //（periodic ping 0x0200 是 reliable，有 ack 才會更新 RTT）。0 = 不印。
    uint64_t enetHealthLogAtMs = 0;
    uint64_t enetHealthReconnectedAtMs = 0;
#endif

    // This is only used for ENet
    if (AppVersionQuad[0] < 5) {
        return;
    }

#ifdef VIPLE_MPQUIC
enet_main_loop:
#endif
    while (!PltIsThreadInterrupted(&controlReceiveThread)) {
        ENetEvent event;
        enet_uint32 waitTimeMs;
#ifdef VIPLE_MPQUIC
        bool healthLogNow = false;
        enet_uint32 healthRtt = 0, healthRttVar = 0, healthInTransit = 0;
        int healthState = 0;
#endif

        PltLockMutex(&enetMutex);

        // Poll for new packets and process retransmissions
        err = serviceEnetHost(client, &event, 0);

        // Compute the next time we need to wake up to handle
        // the RTO timer or a ping.
        if (err == 0) {
            if (ENET_TIME_LESS(peer->nextTimeout, client->serviceTime)) {
                // This can happen when we have no unacked reliable messages
                waitTimeMs = 10;
            }
            else {
                // We add 1 ms just to ensure we're unlikely to undershoot the sleep() and have to
                // do a tiny sleep for another iteration before the timeout is ready to be serviced.
                waitTimeMs = ENET_TIME_DIFFERENCE(peer->nextTimeout, client->serviceTime) + 1;
            }

            // Ensure we don't sleep through a ping
            if (peer->lastReceiveTime && peer->lastSendTime) {
                enet_uint32 timeSinceLastRecv = ENET_TIME_DIFFERENCE(client->serviceTime, peer->lastReceiveTime);
                enet_uint32 timeSinceLastSend = ENET_TIME_DIFFERENCE(client->serviceTime, peer->lastSendTime);
                enet_uint32 timeSinceLastComm = MIN(timeSinceLastSend, timeSinceLastRecv);

                if (timeSinceLastComm >= peer->pingInterval) {
                    // Ping is due now for this peer
                    waitTimeMs = 0;
                } else {
                    waitTimeMs = MIN(waitTimeMs, peer->pingInterval - timeSinceLastComm);
                }
            }
            else {
                waitTimeMs = MIN(waitTimeMs, peer->pingInterval);
            }
        }

#ifdef VIPLE_MPQUIC
        // §M01-C：持鎖更新 LiGetEstimatedRttInfo() 的無鎖快照（語意同上游：
        // peer 是 CONNECTED 才算有效）。
        if (peer->state == ENET_PEER_STATE_CONNECTED) {
            enetRttSnap = peer->roundTripTime;
            enetRttVarSnap = peer->roundTripTimeVariance;
            enetRttValid = 1;
        }
        else {
            enetRttValid = 0;
        }
        // §M01-C：重連 5 s 後的一次性健康檢查（值在鎖內取、log 在鎖外印）
        if (enetHealthLogAtMs != 0 && PltGetMillis() >= enetHealthLogAtMs) {
            enetHealthLogAtMs = 0;
            healthLogNow = true;
            healthRtt = peer->roundTripTime;
            healthRttVar = peer->roundTripTimeVariance;
            healthInTransit = peer->reliableDataInTransit;
            healthState = (int)peer->state;
        }
#endif

        PltUnlockMutex(&enetMutex);

#ifdef VIPLE_MPQUIC
        if (healthLogNow) {
            if (healthState == ENET_PEER_STATE_CONNECTED) {
                Limelog("[VIPLE-MPQUIC] §M01-C ENet healthy rtt=%u var=%u inTransit=%u "
                        "(%llu ms after reconnect)\n",
                        healthRtt, healthRttVar, healthInTransit,
                        (unsigned long long)(PltGetMillis() - enetHealthReconnectedAtMs));
            }
            else {
                Limelog("[VIPLE-MPQUIC] §M01-C ENet NOT healthy %llu ms after reconnect "
                        "(peer state=%d rtt=%u inTransit=%u)\n",
                        (unsigned long long)(PltGetMillis() - enetHealthReconnectedAtMs),
                        healthState, healthRtt, healthInTransit);
            }
        }
#endif

        if (err == 0) {
            // Handle a pending disconnect after unsuccessfully polling
            // for new events to handle.
            if (disconnectPending) {
                PltLockMutex(&enetMutex);
                // Wait 100 ms for pending receives after a disconnect and
                // 1 second for the pending disconnect to be processed after
                // removing the intercept callback.
                err = serviceEnetHost(client, &event, client->intercept ? 100 : 1000);
                if (err == 0) {
                    if (client->intercept) {
                        // Now that no pending receive events remain, we can
                        // remove our intercept hook and allow the server's
                        // disconnect to be processed as expected. We will wait
                        // 1 second for this disconnect to be processed before
                        // we tear down the connection anyway.
                        client->intercept = NULL;
                        PltUnlockMutex(&enetMutex);
                        continue;
                    }
                    else {
                        // The 1 second timeout has expired with no disconnect event
                        // retransmission after the first notification. We can only
                        // assume the server died tragically, so go ahead and tear down.
                        PltUnlockMutex(&enetMutex);
                        Limelog("Disconnect event timeout expired\n");
#ifdef VIPLE_MPQUIC
                        // §Q-REMOTE Fix R.3：failover 或 QUIC 健康都壓制
                        if (quicControlFallbackAvailable()) {
                            Limelog("[VIPLE-MPQUIC] §Q-ENET-GRACE: ENet timeout while "
                                    "QUIC transport alive — suppressing connectionTerminated\n");
                            enetReconnectPending = 1;
                            goto enet_reconnect_wait;
                        }
#endif
                        ListenerCallbacks.connectionTerminated(ML_ERROR_CONTROL_STREAM_DISCONNECT);
                        return;
                    }
                }
                else {
                    PltUnlockMutex(&enetMutex);
                }
            }
            else {
                // No events ready - wait for readability or a local RTO timer to expire
                enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
                enet_socket_wait(client->socket, &condition, waitTimeMs);
                continue;
            }
        }

        if (err < 0) {
            // The error from serviceEnetHost() should be propagated via LastSocketError()
            LC_ASSERT(err == -1);

            err = LastSocketFail();
            // §LOG-TEARDOWN 2026-08-07：使用者主動退出時 input/audio 收線會先
            // 弄斷 ENet 等待（Windows 必現 err=10004 WSAEINTR），每次正常退出
            // 都印「connection failed」誤導事後 log 分析。teardown（連線層級
            // ConnectionInterrupted，LiStopConnection 一開始就設起）改印中性
            // 訊息；真錯誤（非 interrupted）維持原樣。
            if (ConnectionInterrupted) {
                Limelog("Control stream stopped (connection teardown)\n");
            }
            else {
                Limelog("Control stream connection failed: %d\n", err);
            }
#ifdef VIPLE_MPQUIC
            // §Q-ENET-GRACE 2026-05-27: ENet socket 綁在 primary interface
            // IP，failover 時 underlying interface 消失，socket 不能遷移所以
            // send/recv 直接失敗（typical WSAEWOULDBLOCK 10035）。
            // 不可彈 "Connection terminated" dialog——QUIC failover 已把
            // video/audio 轉到備援路徑，使用者只損失控制輸入。
            // §Q-REMOTE Fix R.3：failover 或 QUIC 健康都壓制
            // §FRZ-TEARDOWN 2026-07-06：使用者主動退出時 input/audio 收線
            // 會先弄斷 ENet 等待（err=10004，早於 controlReceiveThread 被
            // interrupt），原本誤入 grace/reconnect 流程（印「等介面恢復
            // 120s」的誤導 log）。用連線層級的 ConnectionInterrupted 判斷
            // ——它在 LiStopConnection 一開始就設起（Connection.c），
            // teardown 走原始收尾（connectionTerminated 此時本就 no-op）。
            if (quicControlFallbackAvailable() &&
                !ConnectionInterrupted &&
                !PltIsThreadInterrupted(&controlReceiveThread)) {
                Limelog("[VIPLE-MPQUIC] §Q-ENET-GRACE: control stream "
                        "connection failed (%d) while QUIC transport alive — "
                        "suppressing connectionTerminated\n", err);
                enetReconnectPending = 1;
                goto enet_reconnect_wait;
            }
#endif
            ListenerCallbacks.connectionTerminated(err);
            return;
        }

        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr;
            int packetLength;

            if (event.packet->dataLength < sizeof(*ctlHdr)) {
                Limelog("Discarding runt control packet: %d < %d\n", event.packet->dataLength, (int)sizeof(*ctlHdr));
                enet_packet_destroy(event.packet);
                continue;
            }

            ctlHdr = (PNVCTL_ENET_PACKET_HEADER_V1)event.packet->data;
            ctlHdr->type = LE16(ctlHdr->type);

            if (encryptedControlStream) {
                // V2 headers can be interpreted as V1 headers for the purpose of examining type,
                // so this check is safe.
                if (ctlHdr->type == 0x0001) {
                    PNVCTL_ENCRYPTED_PACKET_HEADER encHdr;

                    if (event.packet->dataLength < sizeof(NVCTL_ENCRYPTED_PACKET_HEADER)) {
                        Limelog("Discarding runt encrypted control packet: %d < %d\n", event.packet->dataLength, (int)sizeof(NVCTL_ENCRYPTED_PACKET_HEADER));
                        enet_packet_destroy(event.packet);
                        continue;
                    }

                    // encryptedHeaderType is already byteswapped by aliasing through ctlHdr above
                    encHdr = (PNVCTL_ENCRYPTED_PACKET_HEADER)event.packet->data;
                    encHdr->length = LE16(encHdr->length);
                    encHdr->seq = LE32(encHdr->seq);

                    ctlHdr = NULL;
                    packetLength = (int)event.packet->dataLength;
                    if (!decryptControlMessageToV1(encHdr, packetLength, &ctlHdr, &packetLength)) {
                        Limelog("Failed to decrypt control packet of size %d\n", event.packet->dataLength);
                        enet_packet_destroy(event.packet);
                        continue;
                    }

                    // We need to byteswap the unsealed header too
                    ctlHdr->type = LE16(ctlHdr->type);
                }
                else {
                    LC_ASSERT_VT(false);
                    Limelog("Discarding unencrypted packet on encrypted control stream: %04x\n", ctlHdr->type);
                    enet_packet_destroy(event.packet);
                    continue;
                }
            }
            else {
                // Take ownership of the packet data directly for the non-encrypted case
                packetLength = (int)event.packet->dataLength;
                event.packet->data = NULL;
            }

            // We're done with the packet struct
            enet_packet_destroy(event.packet);

            // All below codepaths must free ctlHdr!!!

            // Process HDR data immediately to update global HDR enabled state and HDR metadata.
            // The actual client callback will be invoked in the async callback thread.
            if (ctlHdr->type == packetTypes[IDX_HDR_INFO]) {
                BYTE_BUFFER bb;
                uint8_t enableByte;

                BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);

                BbGet8(&bb, &enableByte);
                if (IS_SUNSHINE()) {
                    // Zero the metadata buffer to properly handle older servers if we have to add new fields
                    memset(&hdrMetadata, 0, sizeof(hdrMetadata));

                    // Sunshine sends HDR metadata in this message too
                    for (int i = 0; i < 3; i++) {
                        BbGet16(&bb, &hdrMetadata.displayPrimaries[i].x);
                        BbGet16(&bb, &hdrMetadata.displayPrimaries[i].y);
                    }
                    BbGet16(&bb, &hdrMetadata.whitePoint.x);
                    BbGet16(&bb, &hdrMetadata.whitePoint.y);
                    BbGet16(&bb, &hdrMetadata.maxDisplayLuminance);
                    BbGet16(&bb, &hdrMetadata.minDisplayLuminance);
                    BbGet16(&bb, &hdrMetadata.maxContentLightLevel);
                    BbGet16(&bb, &hdrMetadata.maxFrameAverageLightLevel);
                    BbGet16(&bb, &hdrMetadata.maxFullFrameLuminance);
                }

                hdrEnabled = (enableByte != 0);
            }

            // §VR：REFRESH_START 由控制接收執行緒直接交給恢復狀態機
            if ((VrFlags & VIPLE_VR_SF_ENABLED) && ctlHdr->type == (unsigned short)VIPLE_VR_PTYPE_S2C) {
                vrHandleS2CInline((const uint8_t*)(ctlHdr + 1), packetLength - (int)sizeof(*ctlHdr));
            }

            // Process client callbacks in a separate thread
            if (needsAsyncCallback(ctlHdr->type)) {
                queueAsyncCallback(ctlHdr, packetLength);
            }
            else if (ctlHdr->type == packetTypes[IDX_TERMINATION]) {
                BYTE_BUFFER bb;


                uint32_t terminationErrorCode;

                if (packetLength >= 6) {
                    // This is the extended termination message which contains a full HRESULT
                    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_BIG);
                    BbGet32(&bb, &terminationErrorCode);

                    Limelog("Server notified termination reason: 0x%08x\n", terminationErrorCode);

                    // Normalize the termination error codes for specific values we recognize
                    switch (terminationErrorCode) {
                    case 0x800e9403: // NVST_DISCONN_SERVER_VIDEO_ENCODER_CONVERT_INPUT_FRAME_FAILED
                        terminationErrorCode = ML_ERROR_FRAME_CONVERSION;
                        break;
                    case 0x800e9302: // NVST_DISCONN_SERVER_VFP_PROTECTED_CONTENT
                        terminationErrorCode = ML_ERROR_PROTECTED_CONTENT;
                        break;
                    case 0x80030023: // NVST_DISCONN_SERVER_TERMINATED_CLOSED
                        if (lastSeenFrame != 0) {
                            // Pass error code 0 to notify the client that this was not an error
                            terminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
                        }
                        else {
                            // We never saw a frame, so this is probably an error that caused
                            // NvStreamer to terminate prior to sending any frames.
                            terminationErrorCode = ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
                        }
                        break;
                    default:
                        break;
                    }
                }
                else {
                    uint16_t terminationReason;

                    // This is the short termination message
                    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);
                    BbGet16(&bb, &terminationReason);

                    Limelog("Server notified termination reason: 0x%04x\n", terminationReason);

                    // SERVER_TERMINATED_INTENDED
                    if (terminationReason == 0x0100) {
                        if (lastSeenFrame != 0) {
                            // Pass error code 0 to notify the client that this was not an error
                            terminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
                        }
                        else {
                            // We never saw a frame, so this is probably an error that caused
                            // NvStreamer to terminate prior to sending any frames.
                            terminationErrorCode = ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
                        }
                    }
                    else {
                        // Otherwise pass the reason unmodified
                        terminationErrorCode = terminationReason;
                    }
                }

                // We used to wait for a ENET_EVENT_TYPE_DISCONNECT event, but since
                // GFE 3.20.3.63 we don't get one for 10 seconds after we first get
                // this termination message. The termination message should be reliable
                // enough to end the stream now, rather than waiting for an explicit
                // disconnect. The server will also not acknowledge our disconnect
                // message once it sends this message, so we mark the peer as fully
                // disconnected now to avoid delays waiting for an ack that will
                // never arrive.
                PltLockMutex(&enetMutex);
#ifdef VIPLE_MPQUIC
                enetRttValid = 0; // §M01-C：peer 即將斷線，RTT 快照作廢
#endif
                enet_peer_disconnect_now(peer, 0);
                PltUnlockMutex(&enetMutex);
                ListenerCallbacks.connectionTerminated((int)terminationErrorCode);
                free(ctlHdr);
                return;
            }

            free(ctlHdr);
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
#ifdef VIPLE_MPQUIC
            // §Q-ENET-GRACE: 當 QUIC failover 正在進行中，ENet 斷線不立即
            // 終止串流。QUIC 已將 video/audio 切到備援路徑，串流仍然活著。
            // 控制輸入（滑鼠 / 鍵盤）暫時中斷，直到用戶重新串流。
            // §Q-REMOTE Fix R.3：failover 或 QUIC 健康都壓制
            if (quicControlFallbackAvailable()) {
                Limelog("[VIPLE-MPQUIC] §Q-ENET-GRACE: ENet disconnected while "
                        "QUIC transport alive — suppressing connectionTerminated. "
                        "Stream continues over QUIC (ENet reconnect pending)\n");
                enetReconnectPending = 1;
                goto enet_reconnect_wait;
            }
#endif
            Limelog("Control stream received unexpected disconnect event\n");
            ListenerCallbacks.connectionTerminated(ML_ERROR_CONTROL_STREAM_DISCONNECT);
            return;
        }
    }

#ifdef VIPLE_MPQUIC
enet_reconnect_wait:
    // §Q-ENET-RECONNECT v1.5.182：ENet 斷線後自動重建 ENet 連線。加密
    // sequence number 不重置，重連後 AES-GCM IV 從斷點繼續遞增。
    //
    // §M01-C（v1.5.276）重寫等待條件與重試策略：
    //  - 舊版（v1.5.185 Fix H）等「OS 有 LC_NETIF_TYPE_ETHERNET 介面 UP」，再綁
    //    session 起點的 LocalAddr 重連。這是當初只在有線為主的桌機上測過的
    //    替代條件：Wi-Fi-only 裝置（Pixel 5 的 lcEnumNetInterfaces 是 session
    //    起點的 JNI 快照、只有 wlan0）條件永遠不成立，120 s 後放棄；桌機有線
    //    不回來、或 DHCP 換了 IP，bind 舊 LocalAddr 也永遠失敗。
    //  - 新版每秒問 OS「此刻有沒有路由到 host、會從哪個本機位址出去」
    //    （enetReconnectProbeRoute），有路由就綁那個位址重連。
    //  - 不再 120 s 放棄：QUIC 活著期間輸入走 flow 0x04 datagram（不可靠，
    //    server→client 控制訊息全斷），ENet 能回來就一直試（退避 1→10 s）。
    //    QUIC 死了（連續 ≥2 s !quicIsConnected()）就明確終止 session——串流
    //    開始後 VideoStream 沒有「影像停了」的 watchdog，不能靠它收尾，否則
    //    會留下畫面凍結、沒有對話框的殭屍 session。
    //  - CONNECT 之後在未發布狀態下多聽一段 probation，擋掉 host 還掛著
    //    stale peer 時的「連上又被踢」抖動。
    //  - 已知限制（follow-up，見 docs/TODO.md）：沒有 re-home——ENet 經 Wi-Fi
    //    重連後，有線回來也留在 Wi-Fi，直到下次失效；server 重連時不補送任何
    //    東西（斷線期間切過 HDR，client 的 HDR 狀態會是舊的）；偵測窗口（10 s）
    //    送進 zombie peer 的 reliable 輸入（含 key-up）會遺失。
    if (enetReconnectPending) {
        enetReconnecting = 1; // §Q-REMOTE Fix R.3：覆蓋整段重連期
        // 注意：這裡 *** 不 *** 立即把 enetReconnectPending 清零。
        // 保持 = 1 讓 lossStatsThread 偵測到並退出（Fix F）。
        // 清零在 500ms 等待後、銷毀 peer/client 前才做。

        Limelog("[VIPLE-MPQUIC] §Q-ENET-RECONNECT: waiting for a route to host "
                "(§M01-C: any interface; input continues via QUIC datagram meanwhile)\n");

        // §Q-ENET-RECONNECT v1.5.183 Fix G: 給 lossStatsThread 時間
        // 偵測 enetReconnectPending 並退出（每 100ms 醒一次，500ms 足夠）。
        // 之後才安全銷毀 peer/client。Fix E 的 null guard 是額外防線。
        PltSleepMs(500);

        enetReconnectPending = 0;

        // 銷毀舊 ENet 資源（lossStatsThread 已退出，安全）
        PltLockMutex(&enetMutex);
        enetRttValid = 0; // §M01-C：舊 peer 即將銷毀，RTT 快照作廢
        if (peer) { enet_peer_reset(peer); peer = NULL; }
        if (client) { enet_host_destroy(client); client = NULL; }
        PltUnlockMutex(&enetMutex);

        // 上一次重連排定的健康檢查 log 屬於已銷毀的 peer，取消
        enetHealthLogAtMs = 0;

        uint64_t waitStartMs = PltGetMillis();
        uint64_t nextAttemptMs = waitStartMs;
        uint64_t quicDeadSinceMs = 0;
        uint32_t backoffMs = 1000;      // 連線嘗試失敗後指數退避 1→2→4→8→10 s
        int attempts = 0;
        int rejects = 0;
        int consecRejects = 0;
        // session 起點本來就有路由 → 初值 true，第一次探測失敗就會印 lost
        bool routeUp = true;
        bool longWaitLogged = false;
        struct sockaddr_storage lastLocal;
        memset(&lastLocal, 0, sizeof(lastLocal));

        while (!stopping && !ConnectionInterrupted &&
               !PltIsThreadInterrupted(&controlReceiveThread)) {
            PltSleepMsInterruptible(&controlReceiveThread, 1000);
            if (stopping || ConnectionInterrupted ||
                PltIsThreadInterrupted(&controlReceiveThread)) {
                break;
            }

            uint64_t nowMs = PltGetMillis();

            // (b) QUIC 存活出口：ENet 已斷、QUIC 也不在 ready（picoquic 一旦
            // 離開 ready 就不會回來；ioDead 也算），2 s debounce 後終止 session。
            // ClInternalConnectionTerminated 本身有去重，與 lossStats 的 ping
            // 失敗終止同時發生也無害。
            if (!quicIsConnected()) {
                if (quicDeadSinceMs == 0) {
                    quicDeadSinceMs = nowMs;
                }
                if (nowMs - quicDeadSinceMs >= 2000) {
                    Limelog("[VIPLE-MPQUIC] §M01-C: QUIC transport dead while ENet down "
                            "— terminating (ENet down %llu ms, attempts=%d rejects=%d)\n",
                            (unsigned long long)(nowMs - waitStartMs), attempts, rejects);
                    ListenerCallbacks.connectionTerminated(ML_ERROR_CONTROL_STREAM_DISCONNECT);
                    return;
                }
            }
            else {
                quicDeadSinceMs = 0;
            }

            // (c) 長時間等待：只印一次，不放棄
            if (!longWaitLogged && nowMs - waitStartMs >= 120000) {
                longWaitLogged = true;
                Limelog("[VIPLE-MPQUIC] §M01-C: ENet still down after 120 s "
                        "(attempts=%d rejects=%d) — input via QUIC datagram "
                        "(unreliable), still retrying\n", attempts, rejects);
            }

            // (d) 每秒都探路由（不受退避限制），狀態轉換才印 log
            struct sockaddr_storage probeLocal;
            SOCKADDR_LEN probeLocalLen = 0;
            int probeErr = enetReconnectProbeRoute(&probeLocal, &probeLocalLen);
            bool haveRoute = (probeErr == 0);
            bool routeRegained = false;
            if (haveRoute != routeUp) {
                if (haveRoute) {
                    char addrStr[URLSAFESTRING_LEN];
                    addrToUrlSafeString(&probeLocal, addrStr, sizeof(addrStr));
                    Limelog("[VIPLE-MPQUIC] §M01-C: route to host available "
                            "(local=%s, %llu ms after ENet down)\n",
                            addrStr, (unsigned long long)(nowMs - waitStartMs));
                    routeRegained = true;
                }
                else {
                    Limelog("[VIPLE-MPQUIC] §M01-C: route to host lost "
                            "(err=%d, %llu ms after ENet down)\n",
                            probeErr, (unsigned long long)(nowMs - waitStartMs));
                }
                routeUp = haveRoute;
            }

            // (e) 路由剛回來，或出口位址變了（例：Wi-Fi 關掉時先經行動網路
            // 探到路由、嘗試一路退避；Wi-Fi 回來後位址改變）→ 退避歸零、立刻試
            if (haveRoute && (routeRegained || !enetReconnectSameIp(&probeLocal, &lastLocal))) {
                if (!routeRegained && lastLocal.ss_family != 0) {
                    char oldStr[URLSAFESTRING_LEN], newStr[URLSAFESTRING_LEN];
                    addrToUrlSafeString(&lastLocal, oldStr, sizeof(oldStr));
                    addrToUrlSafeString(&probeLocal, newStr, sizeof(newStr));
                    Limelog("[VIPLE-MPQUIC] §M01-C: route to host now via local=%s "
                            "(was %s) — backoff reset\n", newStr, oldStr);
                }
                backoffMs = 1000;
                nextAttemptMs = nowMs;
                lastLocal = probeLocal;
                consecRejects = 0;
            }

            // (f) 沒路由、QUIC 看起來已死（等上面的出口收尾）、或還在退避 → 不嘗試
            if (!haveRoute || quicDeadSinceMs != 0 || nowMs < nextAttemptMs) {
                continue;
            }

            // (g) 嘗試一次
            attempts++;
            {
                ENetAddress remoteAddress, localAddress;
                ENetEvent reconnEvent;
                // §Q-ENET-LOCAL-PUBLISH-FIX (review batch 2)：連線嘗試期間
                // 不發布全域 client/peer。物件在 CONNECT＋probation 完成前只有
                // 本執行緒看得到，無鎖 service 是安全的；其他執行緒這段期間
                // 看到 peer==NULL，照既有 Fix K / Fix R.2 路徑走 QUIC fallback。
                ENetHost* newClient = NULL;
                ENetPeer* newPeer;
                bool rejected = false;
                bool quicDiedDuringAttempt = false;
                int cErr = 0;
                unsigned short boundPort = 0;
                char localStr[URLSAFESTRING_LEN];

                memset(&reconnEvent, 0, sizeof(reconnEvent));
                addrToUrlSafeString(&probeLocal, localStr, sizeof(localStr));

                // §M01-C：綁「此刻路由選到的」本機位址，而不是 session 起點的
                // LocalAddr。全域 LocalAddr 不改（video/audio RTP socket、QUIC
                // 都不看它）。
                enet_address_set_address(&localAddress, (struct sockaddr*)&probeLocal, probeLocalLen);
                enet_address_set_address(&remoteAddress, (struct sockaddr*)&RemoteAddr, AddrLen);
                enet_address_set_port(&remoteAddress, ControlPortNumber);

                // IP 沒變且 LiHolePunch 留下了 LocalControlPort：先綁回同一個
                // port（保留 NAT 打洞開出的 pinhole，同 startControlStream），
                // 綁不上再改用 OS 分配的 port。
                if (LocalControlPort != 0 && enetReconnectSameIp(&probeLocal, &LocalAddr)) {
                    enet_address_set_port(&localAddress, LocalControlPort);
                    newClient = enet_host_create(RemoteAddr.ss_family, &localAddress,
                                                 1, CTRL_CHANNEL_COUNT, 0, 0);
                    if (newClient) {
                        boundPort = LocalControlPort;
                    }
                }
                if (!newClient) {
                    enet_address_set_port(&localAddress, 0);  // OS 分配新 port
                    newClient = enet_host_create(RemoteAddr.ss_family, &localAddress,
                                                 1, CTRL_CHANNEL_COUNT, 0, 0);
                }
                if (!newClient) {
                    Limelog("[VIPLE-MPQUIC] §Q-ENET-RECONNECT: enet_host_create failed "
                            "(§M01-C attempt=%d local=%s err=%d) — retry in %u ms\n",
                            attempts, localStr, (int)LastSocketError(), backoffMs);
                    nextAttemptMs = PltGetMillis() + backoffMs;
                    backoffMs = enetReconnectNextBackoff(backoffMs, consecRejects);
                    continue;
                }

                newClient->intercept = ignoreDisconnectIntercept;
                enet_socket_set_option(newClient->socket, ENET_SOCKOPT_QOS, 1);

                newPeer = enet_host_connect(newClient, &remoteAddress,
                                            CTRL_CHANNEL_COUNT, ControlConnectData);
                if (!newPeer) {
                    enet_host_destroy(newClient);
                    Limelog("[VIPLE-MPQUIC] §Q-ENET-RECONNECT: enet_host_connect failed "
                            "(§M01-C attempt=%d local=%s) — retry in %u ms\n",
                            attempts, localStr, backoffMs);
                    nextAttemptMs = PltGetMillis() + backoffMs;
                    backoffMs = enetReconnectNextBackoff(backoffMs, consecRejects);
                    continue;
                }

                // 等 CONNECT（共 5 s）。切成 1 s 一段、段與段之間看 QUIC 是否還
                // 活著：QUIC 死掉時不必把這 5 s 等完，讓上面的存活出口及時收尾。
                // 分段呼叫與單次 5000 ms 等價（serviceEnetHost 本來就每 100 ms
                // 呼叫一次 enet_host_service 處理重傳）。
                for (int slice = 0; slice < 5; slice++) {
                    cErr = serviceEnetHost(newClient, &reconnEvent, 1000);
                    if (cErr != 0) {
                        break;
                    }
                    if (!quicIsConnected()) {
                        quicDiedDuringAttempt = true;
                        break;
                    }
                }

                if (cErr > 0 && reconnEvent.type == ENET_EVENT_TYPE_CONNECT) {
                    // 仍在未發布狀態下完成 flush 與 timeout 設定
                    enet_host_flush(newClient);
                    enet_peer_timeout(newPeer, 2, 10000, 10000);

                    // (h) §M01-C probation：server 若還掛著舊 peer（它的 ENet 逾時
                    // 可能比 client 晚，遠端高 RTT 時可達 ~16-30 s），get_session()
                    // 會拒絕並 enet_peer_disconnect_now()——但 client 已先拿到
                    // CONNECT。舊碼會把這個 peer 發布出去、~1.3 s 後才發現被拒
                    //（期間輸入全丟、約 3 s 一輪抖動）。
                    // 在未發布狀態下多聽一小段。event 傳 NULL → ENet 不 dispatch：
                    //  - RECEIVE 留在 dispatch queue，發布後主迴圈第一次
                    //    serviceEnetHost(client, &event, 0) 自然取出，不會掉；
                    //  - server 的 DISCONNECT 若被 ignoreDisconnectIntercept 吃掉
                    //    會設 disconnectPending，否則 peer 轉成 ZOMBIE 或
                    //    ACKNOWLEDGING_DISCONNECT（state 不再是 CONNECTED）。
                    enet_uint32 probationMs = 300 + 2 * newPeer->roundTripTime;
                    if (probationMs > 1500) {
                        probationMs = 1500;
                    }
                    disconnectPending = false;
                    int pErr = serviceEnetHost(newClient, NULL, probationMs);
                    rejected = (pErr < 0) || disconnectPending ||
                               newPeer->state != ENET_PEER_STATE_CONNECTED;

                    if (!rejected) {
                        // (i) 連線完成 → 鎖內一次性發布給其他執行緒
                        PltLockMutex(&enetMutex);
                        client = newClient;
                        peer = newPeer;
                        // §Q-BYPASS-LOG-RESET v1.5.197 Fix P.2：ENet 重連後
                        // 重置 bypass log 計數器，讓下次 failover 的
                        // bypass 動作可見（診斷改善）。
                        bypassLogCount = 0;
                        // §Q-DISCONNECT-PENDING-FIX (review)：清掉重連前殘留的
                        // disconnectPending。若由 server DISCONNECT 觸發重連
                        //（timeout/DISCONNECT 事件路徑進入時旗標已為 true），
                        // 不歸零會讓重連成功後第一個安靜輪次立刻走 disconnect
                        // 分支 → 拆掉剛建好的連線 → 每 ~1s 重連活鎖、輸入脈衝震盪。
                        // pending disconnect 屬於已在上方銷毀的舊 peer；新連線
                        // 若真收到新 DISCONNECT，intercept hook 會重新設 true。
                        disconnectPending = false;
                        // §F5：重連後 peer 是新的，throttle 設定要重新套用
                        //（peer 已發布、enetReconnecting 尚未清除，持 enetMutex）
                        configureVrThrottle(peer);
                        // §Q-REMOTE Fix R.3：發布完成後才打開控制平面的 ENet 路徑
                        //（順序刻意：先讓 peer 非 NULL、再清 reconnecting；無鎖
                        // 讀者不論看到哪種交錯，至少一個條件成立都會安全走 QUIC）。
                        enetReconnecting = 0;
                        PltUnlockMutex(&enetMutex);

                        uint64_t doneMs = PltGetMillis();
                        Limelog("[VIPLE-MPQUIC] §Q-ENET-RECONNECT: ENet reconnected! "
                                "(§M01-C after %llu ms, attempts=%d, rejects=%d, "
                                "local=%s port=%u, seq continues from %u)\n",
                                (unsigned long long)(doneMs - waitStartMs),
                                attempts, rejects, localStr, (unsigned int)boundPort,
                                currentEnetSequenceNumber);
                        enetHealthReconnectedAtMs = doneMs;
                        enetHealthLogAtMs = doneMs + 5000;
                        goto enet_main_loop;  // 回到主事件迴圈
                    }

                    rejects++;
                    consecRejects++;
                    disconnectPending = false;  // 屬於被拒的 newPeer，不可帶進下一輪
                }
                else if (cErr > 0 && reconnEvent.type == ENET_EVENT_TYPE_RECEIVE &&
                         reconnEvent.packet) {
                    // 防禦：理論上 verify-connect 前不會收到 RECEIVE，但若
                    // 發生，packet 不銷毀會洩漏
                    enet_packet_destroy(reconnEvent.packet);
                }

                // (j) 清理區域物件（未發布、僅本執行緒可見，無鎖即可；thread
                // 中斷時 serviceEnetHost 回 -1 也走這裡，無洩漏）。probation
                // 期間留在 dispatch queue 的封包由 enet_peer_reset 一併釋放。
                enet_peer_reset(newPeer);
                enet_host_destroy(newClient);

                if (stopping || ConnectionInterrupted ||
                    PltIsThreadInterrupted(&controlReceiveThread)) {
                    break;  // teardown：不印誤導的失敗 log
                }

                if (quicDiedDuringAttempt && quicDeadSinceMs == 0) {
                    quicDeadSinceMs = PltGetMillis();
                }
                if (!rejected) {
                    consecRejects = 0;  // 逾時／錯誤打斷「連續被拒」
                }

                nextAttemptMs = PltGetMillis() + backoffMs;
                if (rejected) {
                    Limelog("[VIPLE-MPQUIC] §M01-C: host rejected ENet reconnect "
                            "(stale peer?) (attempt=%d rejects=%d consecutive=%d "
                            "local=%s) — retry in %u ms\n",
                            attempts, rejects, consecRejects, localStr, backoffMs);
                }
                else {
                    Limelog("[VIPLE-MPQUIC] §Q-ENET-RECONNECT: connect attempt failed "
                            "(err=%d event=%d%s, §M01-C attempt=%d local=%s) — retry in %u ms\n",
                            cErr, cErr > 0 ? (int)reconnEvent.type : 0,
                            quicDiedDuringAttempt ? ", QUIC not connected" : "",
                            attempts, localStr, backoffMs);
                }
                backoffMs = enetReconnectNextBackoff(backoffMs, consecRejects);
            }
        }
    }
#endif
}

static void lossStatsThreadFunc(void* context) {
    BYTE_BUFFER byteBuffer;

    if (usePeriodicPing) {
        char periodicPingPayload[8];

        BbInitializeWrappedBuffer(&byteBuffer, periodicPingPayload, 0, sizeof(periodicPingPayload), BYTE_ORDER_LITTLE);
        BbPut16(&byteBuffer, 4); // Length of payload
        BbPut32(&byteBuffer, 0); // Timestamp?

        while (!PltIsThreadInterrupted(&lossStatsThread)) {
            // For Sunshine servers, send the more detailed per-frame FEC messages
            if (IS_SUNSHINE()) {
                PQUEUED_FRAME_FEC_STATUS queuedFrameStatus;

#ifdef VIPLE_MPQUIC
                // §Q-ENET-RECONNECT v1.5.183: peer/client 即將被銷毀。
                // 但如果 QUIC 還活著，改走 QUIC 送 FEC status，不退出。
                if (enetReconnectPending && !quicIsConnected()) {
                    Limelog("[VIPLE-MPQUIC] §Q-ENET-RECONNECT: lossStats thread "
                            "exiting (reconnect pending, no QUIC)\n");
                    return;
                }
#endif

                // §Q-REMOTE Fix R.2: 決定送 FEC status 的方式
                // ENet 可用就走 ENet（低延遲）；ENet 不通改走 QUIC stream #0。
                while (LbqPollQueueElement(&frameFecStatusQueue, (void**)&queuedFrameStatus) == LBQ_SUCCESS) {
                    int sent = 0;
#ifdef VIPLE_MPQUIC
                    if (enetReconnectPending || !peer) {
                        // ENet 確定不通 → 直接走 QUIC，不嘗試 ENet
                        if (quicIsConnected()) {
                            unsigned char buf[1 + sizeof(queuedFrameStatus->fecStatus)];
                            buf[0] = 0x55; // 'U' = FEC status marker
                            memcpy(buf + 1, &queuedFrameStatus->fecStatus,
                                   sizeof(queuedFrameStatus->fecStatus));
                            sent = (quicSendStream(buf, sizeof(buf)) == 0) ? 1 : 0;
                            if (sent) {
                                static int quicFecLogCount = 0;
                                if (quicFecLogCount < 3) {
                                    Limelog("[VIPLE-MPQUIC] §Q-REMOTE Fix R.2: FEC "
                                            "status sent via QUIC stream #0 "
                                            "(count=%d)\n", ++quicFecLogCount);
                                }
                            }
                        }
                    } else
#endif
                    {
                        // ENet path (original)
                        sent = sendMessageEnet(SS_FRAME_FEC_PTYPE,
                                     sizeof(queuedFrameStatus->fecStatus),
                                     &queuedFrameStatus->fecStatus,
                                     CTRL_CHANNEL_GENERIC,
                                     ENET_PACKET_FLAG_UNSEQUENCED,
                                     LbqGetItemCount(&frameFecStatusQueue) > 0) ? 1 : 0;
                    }

                    if (!sent) {
                        Limelog("Loss Stats: Sending frame FEC status message failed: %d\n", (int)LastSocketError());
#ifdef VIPLE_MPQUIC
                        if (quicIsConnected()) {
                            // QUIC 還活著 → 下輪再試，不終止 thread
                            free(queuedFrameStatus);
                            break;
                        }
                        if (quicIsFailoverActive()) {
                            free(queuedFrameStatus);
                            return;
                        }
#endif
                        ListenerCallbacks.connectionTerminated(LastSocketFail());
                        free(queuedFrameStatus);
                        return;
                    }

                    free(queuedFrameStatus);
                }
            }

            // §Q-REMOTE Fix R.2: periodic ping — 同樣加 QUIC fallback
            {
                int pingSent = 0;
#ifdef VIPLE_MPQUIC
                if (enetReconnectPending || !peer) {
                    if (quicIsConnected()) {
                        static const unsigned char pingMarker = 0x50; // 'P'
                        pingSent = (quicSendStream(&pingMarker, 1) == 0) ? 1 : 0;
                        static int quicPingLogCount = 0;
                        if (pingSent && quicPingLogCount < 3) {
                            Limelog("[VIPLE-MPQUIC] §Q-REMOTE Fix R.2: periodic "
                                    "ping sent via QUIC stream #0 "
                                    "(count=%d)\n", ++quicPingLogCount);
                        }
                    }
                } else
#endif
                {
                    pingSent = sendMessageAndForget(0x0200,
                                          sizeof(periodicPingPayload),
                                          periodicPingPayload,
                                          CTRL_CHANNEL_GENERIC,
                                          ENET_PACKET_FLAG_RELIABLE,
                                          false) ? 1 : 0;
                }

                if (!pingSent) {
                    Limelog("Loss Stats: Transaction failed: %d\n", (int)LastSocketError());
#ifdef VIPLE_MPQUIC
                    if (quicIsConnected()) {
                        // QUIC still alive — continue loop, don't terminate
                    } else if (quicIsFailoverActive()) {
                        return;
                    } else
#endif
                    {
                        ListenerCallbacks.connectionTerminated(LastSocketFail());
                        return;
                    }
                }
            }

#ifdef VIPLE_MPQUIC
            // §VR：VR session 的視訊走 RTP/UDP，ENet 正常時 QUIC 上完全沒有流量。
            // QuicTransport 的 stall 偵測（任何路徑 3 秒沒收到封包就判 INACTIVE）會因此
            // 誤觸 failover，Fix L 再把控制訊息（含 tracking）全轉去 QUIC（M1a 實測：
            // 主路徑第 9 秒被判死、來回兩次）。每 500 ms 在 QUIC stream #0 送一個 'P'
            // 標記——server 只刷新 pingTimeout，ABR tick 在 server 端已節流到 500 ms
            // 一輪、不會和 ENet ping 重複計步——它的 ACK 讓主路徑維持「有收到東西」。
            // ENet 斷線時上面的分支本來就每 100 ms 送 'P'，這裡不重複送。
            // 一般 session 的 QUIC 上有視訊流量，不送（不變式 5）。
            if ((VrFlags & VIPLE_VR_SF_ENABLED) && !(enetReconnectPending || !peer) && quicIsConnected()) {
                if (++vrQuicKeepaliveTicks >= VR_QUIC_KEEPALIVE_TICKS) {
                    static const unsigned char vrKeepaliveMarker = 0x50; // 'P'
                    vrQuicKeepaliveTicks = 0;
                    quicSendStream(&vrKeepaliveMarker, 1);
                }
            }
#endif

            // Wait a bit
            PltSleepMsInterruptible(&lossStatsThread, PERIODIC_PING_INTERVAL_MS);
        }
    }
    else {
        char* lossStatsPayload;

        // Sunshine should use the newer codepath above
        LC_ASSERT(!IS_SUNSHINE());

        lossStatsPayload = malloc(payloadLengths[IDX_LOSS_STATS]);
        if (lossStatsPayload == NULL) {
            Limelog("Loss Stats: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }

        while (!PltIsThreadInterrupted(&lossStatsThread)) {
            // Construct the payload
            BbInitializeWrappedBuffer(&byteBuffer, lossStatsPayload, 0, payloadLengths[IDX_LOSS_STATS], BYTE_ORDER_LITTLE);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, LOSS_REPORT_INTERVAL_MS);
            BbPut32(&byteBuffer, 1000);
            BbPut64(&byteBuffer, lastGoodFrame);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, 0x14);

            // Send the message (and don't expect a response)
            if (!sendMessageAndForget(packetTypes[IDX_LOSS_STATS],
                                      payloadLengths[IDX_LOSS_STATS],
                                      lossStatsPayload,
                                      CTRL_CHANNEL_GENERIC,
                                      0,
                                      false)) {
                free(lossStatsPayload);
                Limelog("Loss Stats: Transaction failed: %d\n", (int)LastSocketError());
#ifdef VIPLE_MPQUIC
                if (quicControlFallbackAvailable()) { // §Q-REMOTE Fix R.3
                    Limelog("[VIPLE-MPQUIC] §Q-ENET-GRACE: legacy loss stats "
                            "send failed while QUIC transport alive — stopping "
                            "lossStats thread (suppressing connectionTerminated)\n");
                    return;
                }
#endif
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                return;
            }

            // Wait a bit
            PltSleepMsInterruptible(&lossStatsThread, LOSS_REPORT_INTERVAL_MS);
        }

        free(lossStatsPayload);
    }
}

#ifdef VIPLE_MPQUIC
// §FRZ-RFI-LIMIT 2026-07-06：failover 期間 depacketizer 每個 lost frame
// 都會排一筆 RFI/IDR 請求，QUIC 路徑原本無節流（事故實測一秒 100+ 筆
// 洪水，server 端全是同一個意圖「給我 IDR」）。200ms 節流對恢復延遲
// 無感（server 產出 IDR 本身要數十 ms），但把洪水壓掉。marker 對
// server 而言等效，被節流的請求直接視為已送出。
// §FRZ-ESCALATE 2026-08-07：去 static——VideoStream.c 的 watchdog 升級
// 處置也走這條（extern 宣告在該檔）。節流檢查跨執行緒 best-effort：
// control 與 video 接收執行緒可能同時通過 200ms 檢查各送一枚 marker，
// 實際送出有 picoquicMutex 保護、最壞多送一張等效 IDR request，無害。
void quicSendIdrMarkerRateLimited(const char* tag, const char* what) {
    static uint64_t lastMarkerSentMs;
    uint64_t nowMs = PltGetMillis();
    if (lastMarkerSentMs != 0 && nowMs - lastMarkerSentMs < 200) {
        return;
    }
    static const unsigned char idrMarker = 0x49; // 'I'
    if (quicSendStream(&idrMarker, 1) == 0) {
        lastMarkerSentMs = nowMs;
        Limelog("[VIPLE-MPQUIC] %s: %s request sent via QUIC stream\n",
                tag, what);
    } else {
        Limelog("[VIPLE-MPQUIC] %s: QUIC stream send failed — %s request "
                "dropped\n", tag, what);
    }
}
#endif

static void requestIdrFrame(void) {
#ifdef VIPLE_MPQUIC
    // §Q-IDR-QUIC-FIRST v1.5.197 Fix P.1：在 QUIC failover 期間，
    // 直接走 QUIC stream #0 送 IDR request，跳過整條 ENet 路徑。
    //
    // 根因：sendMessageEnet() 的 early bypass（Fix L）把 raw ENet
    // control message 送成 QUIC_FLOW_INPUT datagram → server 的
    // INPUT handler 不認識 IDR → 靜默丟棄。bypass 回傳 true →
    // 上層以為成功 → §Q-IDR-VIA-QUIC fallback 永遠不觸發。
    // §Q-REMOTE Fix R.3：加入「ENet 控制通道不可用 + QUIC 活著」分支。
    // 不能交給 sendMessageEnet 的 null-guard——它會把 IDR 當 INPUT
    // datagram 送（server 靜默丟棄）且回報成功，fallback 永不觸發
    // （Fix P v1.5.197 的根因，勿重演）。
    if ((quicIsFailoverActive() && quicIsPrimaryPathUnhealthy()) ||
        (enetControlChannelDown() && quicIsConnected())) {
        quicSendIdrMarkerRateLimited("§Q-IDR-QUIC-FIRST", "IDR");
        return;
    }
#endif

    // If this server does not have a known IDR frame request
    // message, we'll accomplish the same thing by creating a
    // reference frame invalidation request.
    if (!supportsIdrFrameRequest) {
        int64_t payload[3];

        // Form the payload
        if (lastSeenFrame < 0x20) {
            payload[0] = 0;
            payload[1] = LE64(lastSeenFrame);
        }
        else {
            payload[0] = LE64(lastSeenFrame - 0x20);
            payload[1] = LE64(lastSeenFrame);
        }

        payload[2] = 0;

        // Send the reference frame invalidation request and read the response
        if (!sendMessageAndDiscardReply(packetTypes[IDX_INVALIDATE_REF_FRAMES],
                                        sizeof(payload),
                                        payload,
                                        CTRL_CHANNEL_URGENT,
                                        ENET_PACKET_FLAG_RELIABLE,
                                        false)) {
            Limelog("Request IDR Frame: Transaction failed: %d\n", (int)LastSocketError());
#ifdef VIPLE_MPQUIC
            if (quicControlFallbackAvailable()) { // §Q-REMOTE Fix R.3
                // §Q-IDR-VIA-QUIC v1.5.177：ENet 死了但 QUIC 還活著。
                // 改走 QUIC stream #0 送 IDR request 給 server，
                // 不再靜默丟棄（根因 D 修正）。
                quicSendIdrMarkerRateLimited("§Q-IDR-VIA-QUIC", "IDR");
                return;
            }
#endif
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            return;
        }
    }
    else {
        // Send IDR frame request and read the response
        if (!sendMessageAndDiscardReply(packetTypes[IDX_REQUEST_IDR_FRAME],
                                        payloadLengths[IDX_REQUEST_IDR_FRAME],
                                        preconstructedPayloads[IDX_REQUEST_IDR_FRAME],
                                        CTRL_CHANNEL_URGENT,
                                        ENET_PACKET_FLAG_RELIABLE,
                                        false)) {
            Limelog("Request IDR Frame: Transaction failed: %d\n", (int)LastSocketError());
#ifdef VIPLE_MPQUIC
            if (quicControlFallbackAvailable()) { // §Q-REMOTE Fix R.3
                // §Q-IDR-VIA-QUIC v1.5.177：同上，改走 QUIC stream #0
                quicSendIdrMarkerRateLimited("§Q-IDR-VIA-QUIC", "IDR");
                return;
            }
#endif
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            return;
        }
    }

    Limelog("IDR frame request sent\n");
}

static void requestInvalidateReferenceFrames(uint32_t startFrame, uint32_t endFrame) {
    // §FRZ-B3: 反轉範圍（start > end）只可能來自上游狀態毒化（release 版
    // LC_ASSERT 無效；毒化事故實測送出過 "(369149 to 328438)"）。與 server
    // 端 nvenc 防衛（invalid rfi → IDR）對稱：改送 IDR，不送無效範圍上線。
    // §F1-DBG-ASSERT: 上游原本在此之前 LC_ASSERT(startFrame <= endFrame)。
    // Android 出貨的 native 是 NDK_DEBUG=1 建置、assert 生效，assert 在前時
    // 這個防衛永遠執行不到（直接 SIGABRT）。防衛必須放在最前面，原 assert
    // 在防衛之後恆真，已拿掉。
    if (startFrame > endFrame) {
        Limelog("Invalid RFI range (%u to %u) — requesting IDR frame instead\n",
                startFrame, endFrame);
        requestIdrFrame();
        return;
    }

    LC_ASSERT(isReferenceFrameInvalidationEnabled());

#ifdef VIPLE_MPQUIC
    // §Q-IDR-QUIC-FIRST v1.5.197 Fix P.1：同 requestIdrFrame()，
    // failover 期間直接走 QUIC stream #0，不走 sendMessageEnet()
    // 的 early bypass（會把 RFI 當 INPUT datagram 送，server 不認識）。
    // §Q-REMOTE Fix R.3：同 requestIdrFrame()——ENet 控制通道不可用
    // 且 QUIC 活著時直接走 stream #0，避開 INPUT-datagram 陷阱。
    if ((quicIsFailoverActive() && quicIsPrimaryPathUnhealthy()) ||
        (enetControlChannelDown() && quicIsConnected())) {
        quicSendIdrMarkerRateLimited("§Q-IDR-QUIC-FIRST", "RFI");
        return;
    }
#endif

    SS_RFI_REQUEST payload = {
        .firstFrameIndex = LE32(startFrame),
        .lastFrameIndex = LE32(endFrame),
    };

    // Send the reference frame invalidation request and read the response
    if (!sendMessageAndDiscardReply(packetTypes[IDX_INVALIDATE_REF_FRAMES],
                                    sizeof(payload),
                                    &payload,
                                    CTRL_CHANNEL_URGENT,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Request Invalidate Reference Frames: Transaction failed: %d\n", (int)LastSocketError());
#ifdef VIPLE_MPQUIC
        if (quicControlFallbackAvailable()) { // §Q-REMOTE Fix R.3
            // §Q-IDR-VIA-QUIC v1.5.177：RFI 也走 QUIC fallback。
            // RFI 效果等同 IDR request，server 收到 0x49 就送 IDR。
            quicSendIdrMarkerRateLimited("§Q-IDR-VIA-QUIC", "RFI");
            return;
        }
#endif
        ListenerCallbacks.connectionTerminated(LastSocketFail());
        return;
    }

    // §LOG-RFI-AGG 2026-08-07：RFI 風暴時逐筆列印（事故單場 7,909 筆）淹沒
    // log。只節流 log、不動送出行為：每秒最多一筆，其餘合併為請求數+涵蓋區間。
    {
        static uint64_t lastRfiSentLogMs;
        static uint32_t rfiSentSuppressed, rfiSentFirstFrame, rfiSentLastFrame;
        uint64_t nowMs = PltGetMillis();

        if (lastRfiSentLogMs == 0 || nowMs - lastRfiSentLogMs >= 1000) {
            if (rfiSentSuppressed > 0) {
                Limelog("Invalidate reference frame request sent (%u to %u; +%u more since last log, frames %u..%u)\n",
                        startFrame, endFrame, rfiSentSuppressed, rfiSentFirstFrame, rfiSentLastFrame);
            }
            else {
                Limelog("Invalidate reference frame request sent (%d to %d)\n", startFrame, endFrame);
            }
            lastRfiSentLogMs = nowMs;
            rfiSentSuppressed = 0;
        }
        else {
            if (rfiSentSuppressed == 0) {
                rfiSentFirstFrame = startFrame;
            }
            rfiSentSuppressed++;
            rfiSentLastFrame = endFrame;
        }
    }
}

static void confirmLongtermReferenceFrame(uint32_t frameIndex) {
    LC_ASSERT(isReferenceFrameInvalidationEnabled());

    SS_LTR_FRAME_ACK payload = {
        .frameIndex = LE32(frameIndex),
    };

    // Send LTR frame ACK and don't wait for response
    if (!sendMessageAndForget(SS_LTR_FRAME_ACK_PTYPE,
                              sizeof(payload),
                              &payload,
                              CTRL_CHANNEL_URGENT,
                              ENET_PACKET_FLAG_RELIABLE,
                              false)) {
        Limelog("LTR frame ACK: Transaction failed: %d\n", (int)LastSocketError());
#ifdef VIPLE_MPQUIC
        if (quicControlFallbackAvailable()) { // §Q-REMOTE Fix R.3
            Limelog("[VIPLE-MPQUIC] §Q-ENET-GRACE: LTR ACK send "
                    "failed while QUIC transport alive — ACK dropped "
                    "(suppressing connectionTerminated)\n");
            return;
        }
#endif
        ListenerCallbacks.connectionTerminated(LastSocketFail());
        return;
    }
}

static void referenceFrameControlFunc(void* context) {
    LC_ASSERT(isReferenceFrameInvalidationEnabled());

    while (!PltIsThreadInterrupted(&invalidateRefFramesThread)) {
        PQUEUED_REFERENCE_FRAME_CONTROL qfit;
        uint32_t invalidateStartFrame;
        uint32_t invalidateEndFrame;
        bool invalidate = false;
        bool rangeRegressed = false;  // §F1-DBG-ASSERT

        // Wait for a reference frame control message or a request to shutdown
        if (LbqWaitForQueueElement(&referenceFrameControlQueue, (void**)&qfit) != LBQ_SUCCESS) {
            // Bail if we're stopping
            return;
        }

        do {
            if (qfit->invalidate) {
                if (!invalidate) {
                    invalidateStartFrame = qfit->startFrame;
                    invalidateEndFrame = qfit->endFrame;
                    invalidate = true;
                }
                else {
                    // Aggregate all lost frames into one range
                    //
                    // §F1-DBG-ASSERT: 上游原本 LC_ASSERT(qfit->endFrame >= invalidateEndFrame)。
                    // 後到 tuple 的 end 比已聚合的 end 小，代表 depacketizer 的
                    // 幀號狀態倒帶過（§FRZ-WATCHDOG 採納 stale 幀、完整幀又把
                    // startFrameNumber 拉回舊值），聚合出的範圍已不可信。Android
                    // 出貨的 debug native 會在 assert SIGABRT；改為標記，整批
                    // 改送 IDR（與 §FRZ-B3 相同的復原手段）。
                    if (qfit->endFrame < invalidateEndFrame) {
                        rangeRegressed = true;
                    }
                    invalidateEndFrame = qfit->endFrame;
                }
            }
            else {
                // Send LTR frame ACK
                confirmLongtermReferenceFrame(qfit->startFrame);
            }
            free(qfit);
        } while (LbqPollQueueElement(&referenceFrameControlQueue, (void**)&qfit) == LBQ_SUCCESS);

        if (invalidate) {
            if (rangeRegressed) {
                Limelog("[VIPLE-CTRL] Non-monotonic RFI ranges (start %u, last end %u) — requesting IDR frame instead\n",
                        invalidateStartFrame, invalidateEndFrame);
                requestIdrFrame();
            }
            else {
                // Send the reference frame invalidation request
                requestInvalidateReferenceFrames(invalidateStartFrame, invalidateEndFrame);
            }
        }
    }
}

static void requestIdrFrameFunc(void* context) {
    while (!PltIsThreadInterrupted(&requestIdrFrameThread)) {
        PltWaitForEvent(&idrFrameRequiredEvent);
        PltClearEvent(&idrFrameRequiredEvent);

        if (stopping) {
            // Bail if we're stopping
            return;
        }

        // Any pending RFI requests and LTR frame ACK messages are now redundant
        freeBasicLbqList(LbqFlushQueueItems(&referenceFrameControlQueue));

        // Request the IDR frame
        requestIdrFrame();
    }
}

// Stops the control stream
int stopControlStream(void) {
#ifdef VIPLE_MPQUIC
    enetRttValid = 0; // §M01-C：peer 即將斷線／銷毀，RTT 快照作廢
#endif
    stopping = true;
    LbqSignalQueueShutdown(&referenceFrameControlQueue);
    LbqSignalQueueShutdown(&frameFecStatusQueue);
    LbqSignalQueueDrain(&asyncCallbackQueue);
    PltSetEvent(&idrFrameRequiredEvent);

    // This must be set to stop in a timely manner
    LC_ASSERT(ConnectionInterrupted);

    if (ctlSock != INVALID_SOCKET) {
        shutdownTcpSocket(ctlSock);
    }

    PltInterruptThread(&lossStatsThread);
    PltInterruptThread(&requestIdrFrameThread);
    PltInterruptThread(&controlReceiveThread);
    PltInterruptThread(&asyncCallbackThread);
    if (vrSendThreadStarted) {
        PltInterruptThread(&vrSendThread);
    }

    PltJoinThread(&lossStatsThread);
    PltJoinThread(&requestIdrFrameThread);
    PltJoinThread(&controlReceiveThread);
    PltJoinThread(&asyncCallbackThread);
    // §VR：一定要在下面銷毀 peer/client 之前 join（它會送 LOSS）；是否 join 看
    // vrSendThreadStarted（§M01-E 的教訓：不能漏 join）
    if (vrSendThreadStarted) {
        PltJoinThread(&vrSendThread);
        vrSendThreadStarted = false;
    }

    // We will only have an RFI thread if RFI is enabled
    if (isReferenceFrameInvalidationEnabled()) {
        PltInterruptThread(&invalidateRefFramesThread);
        PltJoinThread(&invalidateRefFramesThread);
    }

    if (peer != NULL) {
        // Gracefully disconnect to ensure the remote host receives all of our final
        // outbound traffic, including any key up events that might be sent.
        gracefullyDisconnectEnetPeer(client, peer, CONTROL_STREAM_LINGER_TIMEOUT_SEC * 1000);
        peer = NULL;
    }
    if (client != NULL) {
        enet_host_destroy(client);
        client = NULL;
    }
#ifdef VIPLE_MPQUIC
    // §M01-C：開頭清過之後，controlReceiveThread 在被 join 前的最後一輪
    // 可能又把快照設回有效；peer 已銷毀，這裡再清一次。
    enetRttValid = 0;
#endif

    if (ctlSock != INVALID_SOCKET) {
        closeSocket(ctlSock);
        ctlSock = INVALID_SOCKET;
    }

    return 0;
}

// Called by the input stream to send a packet for Gen 5+ servers
int sendInputPacketOnControlStream(unsigned char* data, int length, uint8_t channelId, uint32_t flags, bool moreData) {
    LC_ASSERT(AppVersionQuad[0] >= 5);

    // Send the input data (no reply expected)
    if (sendMessageAndForget(packetTypes[IDX_INPUT_DATA], length, data, channelId, flags, moreData) == 0) {
        return -1;
    }

    return 0;
}

// Called by the input stream to flush queued packets before a batching wait
void flushInputOnControlStream(void) {
    if (AppVersionQuad[0] >= 5) {
        PltLockMutex(&enetMutex);
        // v1.5.186 Fix J: reconnect wait 銷毀 client 後，
        // inputSendThread 的 mouse batching 可能呼叫 flush。
        if (client) {
            enet_host_flush(client);
        }
        PltUnlockMutex(&enetMutex);
    }
}

#ifdef VIPLE_MPQUIC
// v1.5.186 Fix I: inputSendThread 用來等待 ENet 重連的查詢函數。
// 回傳 true 表示 peer 和 client 都已重建。
bool isEnetConnected(void) {
    bool ret;
    PltLockMutex(&enetMutex);
    ret = (peer != NULL && client != NULL);
    PltUnlockMutex(&enetMutex);
    return ret;
}
#endif

bool isControlDataInTransit(void) {
    bool ret = false;

    PltLockMutex(&enetMutex);
    if (peer != NULL && peer->state == ENET_PEER_STATE_CONNECTED) {
        if (peer->reliableDataInTransit != 0) {
            ret = true;
        }
    }
    PltUnlockMutex(&enetMutex);

    return ret;
}

bool LiGetEstimatedRttInfo(uint32_t* estimatedRtt, uint32_t* estimatedRttVariance) {
#ifdef VIPLE_MPQUIC
    // §M01-C：只讀 controlReceiveThread 寫的快照，完全不碰 peer。
    // 上游下方註解「peer 永不消失」的假設已被 §Q-ENET-RECONNECT 打破
    //（重連會 enet_host_destroy 舊 client）；也刻意不加 enetMutex——呼叫端
    // 在 submitDecodeUnit 路徑上（見 enetRttSnap 的說明）。
    if (!enetRttValid) {
        return false;
    }
    if (estimatedRtt != NULL) {
        *estimatedRtt = enetRttSnap;
    }
    if (estimatedRttVariance != NULL) {
        *estimatedRttVariance = enetRttVarSnap;
    }
    return true;
#else
    bool ret = false;

    // We do not acquire enetMutex here because we're just reading metrics
    // and observing a torn write every once in a while is totally fine.
    // The peer pointer points to memory reserved inside the client object,
    // so it's guaranteed that it will never go away underneath us.
    if (peer != NULL && peer->state == ENET_PEER_STATE_CONNECTED) {
        if (estimatedRtt != NULL) {
            *estimatedRtt = peer->roundTripTime;
        }

        if (estimatedRttVariance != NULL) {
            *estimatedRttVariance = peer->roundTripTimeVariance;
        }

        ret = true;
    }

    return ret;
#endif
}

// Starts the control stream
int startControlStream(void) {
    int err;

    if (AppVersionQuad[0] >= 5) {
        ENetAddress remoteAddress, localAddress;
        ENetEvent event;

        LC_ASSERT(ControlPortNumber != 0);

        enet_address_set_address(&localAddress, (struct sockaddr *)&LocalAddr, AddrLen);
#ifdef __3DS__
        // binding to wildcard port is broken on the 3DS, so we need to define a port manually
        enet_address_set_port(&localAddress, htons(n3ds_udp_port++));
#else
        // VipleStream: if LiHolePunch left us a local port, reuse it so the
        // NAT pinhole opened by the punch covers the ENet control stream.
        // Falls back to wildcard (0) when no punch was performed.
        enet_address_set_port(&localAddress, LocalControlPort);
#endif

        enet_address_set_address(&remoteAddress, (struct sockaddr *)&RemoteAddr, AddrLen);
        enet_address_set_port(&remoteAddress, ControlPortNumber);

        // Create a client
        client = enet_host_create(RemoteAddr.ss_family,
                                  LocalAddr.ss_family != 0 ? &localAddress : NULL,
                                  1, CTRL_CHANNEL_COUNT, 0, 0);
        if (client == NULL) {
            stopping = true;
            return -1;
        }

        client->intercept = ignoreDisconnectIntercept;

        // Enable high priority QoS marking on control stream traffic
        //
        // NB: It is important to do this before connecting because there's logic in the connect
        // retransmission code to detect QoS-intolerant routes and disable QoS marking for those.
        enet_socket_set_option (client->socket, ENET_SOCKOPT_QOS, 1);

        // Connect to the host
        peer = enet_host_connect(client, &remoteAddress, CTRL_CHANNEL_COUNT, ControlConnectData);
        if (peer == NULL) {
            stopping = true;
            enet_host_destroy(client);
            client = NULL;
            return -1;
        }

        // Wait for the connect to complete
        err = serviceEnetHost(client, &event, CONTROL_STREAM_TIMEOUT_SEC * 1000);
        if (err <= 0 || event.type != ENET_EVENT_TYPE_CONNECT) {
            if (err < 0) {
                Limelog("Failed to establish ENet connection on UDP port %u: error %d\n", ControlPortNumber, LastSocketFail());
            }
            else if (err == 0) {
                Limelog("Failed to establish ENet connection on UDP port %u: timed out\n", ControlPortNumber);
            }
            else {
                Limelog("Failed to establish ENet connection on UDP port %u: unexpected event %d (error: %d)\n", ControlPortNumber, (int)event.type, LastSocketError());
            }

            stopping = true;
            enet_peer_reset(peer);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;

            if (err == 0) {
                return ETIMEDOUT;
            }
            else if (err > 0 && event.type != ENET_EVENT_TYPE_CONNECT && LastSocketError() == 0) {
                // If we got an unexpected event type and have no other error to return, return the event type
                LC_ASSERT(event.type != ENET_EVENT_TYPE_NONE);
                return event.type != ENET_EVENT_TYPE_NONE ? (int)event.type : LastSocketFail();
            }
            else {
                return LastSocketFail();
            }
        }

        // Ensure the connect verify ACK is sent immediately
        enet_host_flush(client);

#ifdef __3DS__
        // Set the peer timeout to 1 minute and limit backoff to 2x RTT
        // The 3DS can take a bit longer to set up when starting fresh
        enet_peer_timeout(peer, 2, 60000, 60000);
#else
        // Set the peer timeout to 10 seconds and limit backoff to 2x RTT
        enet_peer_timeout(peer, 2, 10000, 10000);
#endif

#ifdef VIPLE_MPQUIC
        // §M01-C：連上當下先填一次 RTT 快照（上游此時就能回 true；之後由
        // controlReceiveThread 主迴圈持續更新）。controlReceiveThread 尚未
        // 建立，這裡是唯一碰 peer 的執行緒。
        enetRttSnap = peer->roundTripTime;
        enetRttVarSnap = peer->roundTripTimeVariance;
        enetRttValid = 1;
#endif

        // §F5：VR session 的 throttle 設定（controlReceiveThread 尚未建立，這裡是
        // 唯一碰 peer 的執行緒；命令會隨 START A 一起 flush 出去）
        configureVrThrottle(peer);
    }
    else {
        // NB: Do NOT use ControlPortNumber here. 47995 is correct for these old versions.
        LC_ASSERT(ControlPortNumber == 0);
        ctlSock = connectTcpSocket(&RemoteAddr, AddrLen,
            47995, CONTROL_STREAM_TIMEOUT_SEC);
        if (ctlSock == INVALID_SOCKET) {
            stopping = true;
            return LastSocketFail();
        }

        enableNoDelay(ctlSock);
    }

    err = PltCreateThread("ControlRecv", controlReceiveThreadFunc, NULL, &controlReceiveThread);
    if (err != 0) {
        stopping = true;
        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    // Send START A
    if (!sendMessageAndDiscardReply(packetTypes[IDX_START_A],
                                    payloadLengths[IDX_START_A],
                                    preconstructedPayloads[IDX_START_A],
                                    CTRL_CHANNEL_GENERIC,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Start A failed: %d\n", (int)LastSocketError());
        err = LastSocketFail();
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    // Send START B
    if (!sendMessageAndDiscardReply(packetTypes[IDX_START_B],
                                    payloadLengths[IDX_START_B],
                                    preconstructedPayloads[IDX_START_B],
                                    CTRL_CHANNEL_GENERIC,
                                    ENET_PACKET_FLAG_RELIABLE,
                                    false)) {
        Limelog("Start B failed: %d\n", (int)LastSocketError());
        err = LastSocketFail();
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    err = PltCreateThread("LossStats", lossStatsThreadFunc, NULL, &lossStatsThread);
    if (err != 0) {
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    err = PltCreateThread("ReqIdrFrame", requestIdrFrameFunc, NULL, &requestIdrFrameThread);
    if (err != 0) {
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&lossStatsThread);
        PltJoinThread(&lossStatsThread);

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }

        return err;
    }

    err = PltCreateThread("CtrlAsyncCb", asyncCallbackThreadFunc, NULL, &asyncCallbackThread);
    if (err != 0) {
        stopping = true;
        PltSetEvent(&idrFrameRequiredEvent);

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&lossStatsThread);
        PltJoinThread(&lossStatsThread);

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);

        PltInterruptThread(&requestIdrFrameThread);
        PltJoinThread(&requestIdrFrameThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }

        return err;
    }

    // Only create the reference frame invalidation thread if RFI is enabled
    if (isReferenceFrameInvalidationEnabled()) {
        err = PltCreateThread("InvRefFrames", referenceFrameControlFunc, NULL, &invalidateRefFramesThread);
        if (err != 0) {
            stopping = true;
            PltSetEvent(&idrFrameRequiredEvent);
            LbqSignalQueueShutdown(&asyncCallbackQueue);

            if (ctlSock != INVALID_SOCKET) {
                shutdownTcpSocket(ctlSock);
            }
            else {
                ConnectionInterrupted = true;
            }

            PltInterruptThread(&lossStatsThread);
            PltJoinThread(&lossStatsThread);

            PltInterruptThread(&controlReceiveThread);
            PltJoinThread(&controlReceiveThread);

            PltInterruptThread(&requestIdrFrameThread);
            PltJoinThread(&requestIdrFrameThread);

            PltInterruptThread(&asyncCallbackThread);
            PltJoinThread(&asyncCallbackThread);

            if (ctlSock != INVALID_SOCKET) {
                closeSocket(ctlSock);
                ctlSock = INVALID_SOCKET;
            }
            else {
                enet_peer_disconnect_now(peer, 0);
                peer = NULL;
                enet_host_destroy(client);
                client = NULL;
            }

            return err;
        }
    }

    // §VR：其餘執行緒都成功之後才建立（失敗路徑因此不用管它）。建立失敗時
    // vrRecoveryReportLoss 會退回同步送 LOSS，功能不受影響。
    if (VrFlags & VIPLE_VR_SF_ENABLED) {
        if (PltCreateThread("VrSend", vrSendThreadFunc, NULL, &vrSendThread) == 0) {
            vrSendThreadStarted = true;
        }
        else {
            Limelog("[VIPLE-VR-LOSS] failed to create VrSend thread — LOSS will be sent synchronously\n");
        }
    }

    return 0;
}

bool LiGetCurrentHostDisplayHdrMode(void) {
    return hdrEnabled;
}

bool LiGetHdrMetadata(PSS_HDR_METADATA metadata) {
    if (!IS_SUNSHINE() || !hdrEnabled) {
        return false;
    }

    *metadata = hdrMetadata;
    return true;
}
