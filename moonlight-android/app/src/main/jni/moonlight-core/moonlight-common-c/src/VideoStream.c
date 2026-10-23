#include "Limelight-internal.h"

#ifdef VIPLE_MPQUIC
#include "QuicTransport.h"

// §FRZ-ESCALATE 2026-08-07：定義在 ControlStream.c（自帶 200ms 節流；
// marker 0x49 對 server 等效於 IDR request）。watchdog 升級處置使用。
void quicSendIdrMarkerRateLimited(const char* tag, const char* what);
#endif

#define FIRST_FRAME_MAX 1500
#define FIRST_FRAME_TIMEOUT_SEC 10

#define FIRST_FRAME_PORT 47996

static RTP_VIDEO_QUEUE rtpQueue;

#include "VrMultiLink.h"

// §VR-REPAIR-RTT ─────────────────────────────────────────────────────────────
// 補包（§VR-LINK-REPAIR）每個封包的決定與量測。決定本身是純函式 vrrrDecide()（VrRepairRtt.h），這裡只負責把
// 連線層的狀態餵給它、把結果填進佇列，以及：
//   - 協商了補包就每 10 秒印一行 [VIPLE-VR-REPAIR] gate 10s（量測；不管 server 的主開關）；
//   - --vr-repair-ab：同一場每隔幾秒在原本的規則與 RTT 規則之間切換，換邊時印那一邊的小結；
//   - 保險觸發時印一行；
//   - 佇列回報「被補包拖晚的幀放行了」時記下時刻，給 XR 那一側用（LiGetVrRepairQuietMs）。
// 量測的代價：協商了補包之後，每個影像封包多讀兩次時鐘（這裡取 nowUs 一次、vrmlRepairRtt 看連線是否已確認一次）、
// 多合併一次各條連線的往返統計——server 的主開關關著時也一樣（gate 10s 要用），不改任何決定。
// log 的欄位見 docs/log_tags.md。rr 只在影像接收執行緒存取，每場串流在執行緒開頭歸零。
#define RR_GATE_WINDOW_US 10000000

typedef struct _RR_GATE_WINDOW {
    uint64_t startUs;         // 這個視窗從什麼時候開始（0＝還沒開始）
    uint64_t lastPktUs;       // 上一個封包的時刻
    uint64_t onUs, totalUs;   // 補包開著的時間、全部的時間（以相鄰兩個封包的間隔累計）
    uint32_t flips;           // 補包開／關翻了幾次
    uint32_t trips;           // 保險觸發了幾次
    bool prevOn, havePrev;
    bool sawLegacy, sawRtt;   // 這個視窗用過哪一套規則
    RTPV_REPAIR_TOTALS base;  // 視窗開始時佇列的累計值
} RR_GATE_WINDOW;

typedef struct _RR_AB {
    uint64_t startUs;         // A/B 從什麼時候開始算（第一個協商了 RTT 規則的影像封包；0＝還沒開始）
    uint64_t armStartUs;      // 目前這一邊從什麼時候開始
    uint64_t onUs, totalUs;   // 這一邊：補包開著的時間、全部的時間
    bool armRtt;              // 目前在 RTT 那一邊
    RTPV_REPAIR_TOTALS base;  // 這一邊開始時佇列的累計值
} RR_AB;

static struct {
    VRRR_STATE state;         // vrrrDecide 的狀態（門檻的遲滯、保險）
    uint32_t seenOk, seenBad; // 已經交給保險的 mlTot.repairedUsed／lostNacked
    uint64_t seenDelayedUs;   // 已經處理過的 rtpQueue.mlDelayedReleaseUs
    uint32_t capOverrideMs;   // --vr-repair-hold（0＝自動）
    RR_GATE_WINDOW gate;
    RR_AB ab;
} rr;

// 給別的執行緒讀的（XR 的 frame thread）：最近一次「被補包拖晚的幀放行」的時刻（毫秒時戳的低 32 bit）、有沒有過、
// 目前照哪一套規則。接收執行緒寫；讀到稍舊的值無妨
static volatile uint32_t rrDelayedReleaseMs;
static volatile int rrDelayedAny;
static volatile int rrRulesNow;

// 佇列被整個重設了（§FRZ-WATCHDOG）：它的累計值歸零，這裡留的副本跟著重來，不然下一次取差值會是一個很大的數
// （交給保險的那兩個會變成幾十億次失敗）
static void rrOnQueueReset(void) {
    memset(&rr.gate.base, 0, sizeof(rr.gate.base));
    memset(&rr.ab.base, 0, sizeof(rr.ab.base));
    rr.seenOk = rr.seenBad = 0;
    rr.seenDelayedUs = 0;
}

// 佇列處理完一批事情之後呼叫：有被補包機制拖晚的幀剛放行，就記下現在的時刻
static void rrNoteDelayedRelease(void) {
    if (rtpQueue.mlDelayedReleaseUs != 0 && rtpQueue.mlDelayedReleaseUs != rr.seenDelayedUs) {
        rr.seenDelayedUs = rtpQueue.mlDelayedReleaseUs;
        rrDelayedReleaseMs = (uint32_t)PltGetMillis();
        rrDelayedAny = 1;
    }
}

static unsigned rrPct(uint64_t onUs, uint64_t totalUs) {
    return totalUs != 0 ? (unsigned)((onUs * 100 + totalUs / 2) / totalUs) : 0;
}

// --vr-repair-ab：剛結束的那一邊的小結（從那一邊開始到現在佇列累計值的差）
static void rrAbSummary(uint64_t nowUs) {
    const RTPV_REPAIR_TOTALS* t = &rtpQueue.mlTot;
    const RTPV_REPAIR_TOTALS* b = &rr.ab.base;
    const uint32_t fixes = t->fixCount - b->fixCount;

    Limelog("[VIPLE-VR-REPAIR] ab: arm=%s secs=%.0f on=%u%% lost=%u (nacked=%u unasked=%u whole=%u) repaired=%u used=%u nack=%u spurious=%u"
            " late=%u tooMany=%u fixMs avg=%.1f max=%.1f\n",
            rr.ab.armRtt ? "rtt" : "legacy", (nowUs - rr.ab.armStartUs) / 1000000.0, rrPct(rr.ab.onUs, rr.ab.totalUs),
            t->lost - b->lost, t->lostNacked - b->lostNacked, t->lostUnasked - b->lostUnasked, t->lostWhole - b->lostWhole,
            t->repaired - b->repaired, t->repairedUsed - b->repairedUsed, t->nack - b->nack, t->spurious - b->spurious,
            t->late - b->late, t->tooMany - b->tooMany,
            fixes != 0 ? (t->fixUsSum - b->fixUsSum) / 1000.0 / fixes : 0.0, t->fixUsMax / 1000.0);
}

// 這個時刻在 A/B 的哪一邊（true＝RTT）。沒開 --vr-repair-ab 恆為 true。換邊時印一行，並印剛結束那一邊的小結
static bool rrArmRtt(uint64_t nowUs) {
    const uint32_t abSec = StreamConfig.vrRepairAbSec > 0 ? (uint32_t)StreamConfig.vrRepairAbSec : 0;
    const bool first = rr.ab.startUs == 0;
    bool arm;

    if (abSec == 0 || nowUs == 0) {
        return true;
    }
    if (first) {
        rr.ab.startUs = nowUs;
    }
    arm = vrrrAbArmRtt(nowUs - rr.ab.startUs, abSec);
    if (first || arm != rr.ab.armRtt) {
        if (!first) {
            rrAbSummary(nowUs);
        }
        Limelog("[VIPLE-VR-REPAIR] ab -> %s frame=%u\n", arm ? "rtt" : "legacy", RtpvGetCurrentFrameNumber(&rtpQueue));
        rr.ab.armRtt = arm;
        rr.ab.armStartUs = nowUs;
        rr.ab.onUs = rr.ab.totalUs = 0;
        rr.ab.base = rtpQueue.mlTot;
        rtpQueue.mlTot.fixUsMax = 0;  // 最大值沒辦法取差值：每一邊重來（只有這裡的小結在用）
    }
    return arm;
}

static void rrGateLog(uint32_t holdUs) {
    const RTPV_REPAIR_TOTALS* t = &rtpQueue.mlTot;
    const RTPV_REPAIR_TOTALS* b = &rr.gate.base;
    VRRR_RTT_STATS st;
    char rtt[96];

    // 往返時間是印的這一刻的值（正在送影像的連線最近 VRRR_RTT_WIN 筆）；eta／try／cap 是照它算出來、§VR-REPAIR-RTT 會用
    // （或正在用）的值，RTT 規則沒開的時候也印，方便事先看它會怎麼訂
    if (vrmlRepairRtt(rtpQueue.mlCarryMask, &st)) {
        const uint32_t etaUs = vrrrEtaUs(st.hiUs);
        snprintf(rtt, sizeof(rtt), "lo=%.1f mid=%.1f hi=%.1f eta=%.1f try=%.1f cap=%.1f", st.loUs / 1000.0, st.midUs / 1000.0,
                 st.hiUs / 1000.0, etaUs / 1000.0, vrrrEtaTryUs(st.midUs) / 1000.0,
                 vrrrCapUs(holdUs, etaUs, vrrrEtaFastUs(st.loUs), rr.capOverrideMs) / 1000.0);
    }
    else {
        snprintf(rtt, sizeof(rtt), "lo=- mid=- hi=- eta=- try=- cap=-");
    }
    Limelog("[VIPLE-VR-REPAIR] gate 10s: rules=%s on=%u%% flips=%u paused=%u | lost=%u (nacked=%u unasked=%u whole=%u) repaired=%u used=%u"
            " | rtt %s | lateDone 5/10/20/33/+=%u/%u/%u/%u/%u never=%u | stall 4/6/8/12/+=%u/%u/%u/%u/%u\n",
            rr.gate.sawRtt ? (rr.gate.sawLegacy ? "mixed" : "rtt") : "legacy", rrPct(rr.gate.onUs, rr.gate.totalUs), rr.gate.flips,
            rr.gate.trips,
            t->lost - b->lost, t->lostNacked - b->lostNacked, t->lostUnasked - b->lostUnasked, t->lostWhole - b->lostWhole,
            t->repaired - b->repaired, t->repairedUsed - b->repairedUsed, rtt,
            t->lateDone[0] - b->lateDone[0], t->lateDone[1] - b->lateDone[1], t->lateDone[2] - b->lateDone[2],
            t->lateDone[3] - b->lateDone[3], t->lateDone[4] - b->lateDone[4], t->lateNever - b->lateNever,
            t->stall[0] - b->stall[0], t->stall[1] - b->stall[1], t->stall[2] - b->stall[2], t->stall[3] - b->stall[3],
            t->stall[4] - b->stall[4]);
}

// 每個影像封包做完決定之後呼叫一次（協商了補包才呼叫）：累計 gate 10s 與 A/B 小結要的時間比例，保險觸發時印一行
static void rrNote(const VRRR_IN* in, const VRRR_OUT* out) {
    const uint64_t nowUs = in->nowUs;

    if (out->tripped) {
        rr.gate.trips++;
        Limelog("[VIPLE-VR-REPAIR] rtt: paused %.0f ms (level %u)\n",
                rr.state.brk.pausedUntilUs > nowUs ? (rr.state.brk.pausedUntilUs - nowUs) / 1000.0 : 0.0, (unsigned)rr.state.brk.level);
    }
    if (rr.gate.startUs == 0) {
        rr.gate.startUs = nowUs;
        rr.gate.base = rtpQueue.mlTot;
    }
    if (rr.gate.lastPktUs != 0 && nowUs > rr.gate.lastPktUs) {
        // 上一個封包到這一個之間，照上一個封包當時的決定算。影像停了很久（暫停、斷線）的那一段最多算 1 秒
        uint64_t dt = nowUs - rr.gate.lastPktUs;
        if (dt > 1000000) {
            dt = 1000000;
        }
        rr.gate.totalUs += dt;
        rr.ab.totalUs += dt;
        if (rr.gate.prevOn) {
            rr.gate.onUs += dt;
            rr.ab.onUs += dt;
        }
    }
    if (rr.gate.havePrev && out->mlRepair != rr.gate.prevOn) {
        rr.gate.flips++;
    }
    rr.gate.prevOn = out->mlRepair;
    rr.gate.havePrev = true;
    rr.gate.lastPktUs = nowUs;
    if (out->mlRttMode) {
        rr.gate.sawRtt = true;
    }
    else {
        rr.gate.sawLegacy = true;
    }
    if (nowUs - rr.gate.startUs >= RR_GATE_WINDOW_US) {
        rrGateLog(in->holdUs);
        rr.gate.startUs = nowUs;
        rr.gate.base = rtpQueue.mlTot;
        rr.gate.onUs = rr.gate.totalUs = 0;
        rr.gate.flips = rr.gate.trips = 0;
        rr.gate.sawLegacy = rr.gate.sawRtt = false;
    }
}

static SOCKET rtpSocket = INVALID_SOCKET;
static SOCKET firstFrameSocket = INVALID_SOCKET;

static PPLT_CRYPTO_CONTEXT decryptionCtx;

static PLT_THREAD udpPingThread;

#ifdef VIPLE_MPQUIC
// QUIC receive ring buffer for video datagrams.
// The QUIC I/O thread pushes via quicVideoRecvCallback();
// VideoReceiveThreadProc polls this instead of the UDP socket.
#define QUIC_VIDEO_RING_SIZE 4096
#define QUIC_VIDEO_MAX_PKT   1500

static struct {
    unsigned char data[QUIC_VIDEO_RING_SIZE][QUIC_VIDEO_MAX_PKT];
    int           len[QUIC_VIDEO_RING_SIZE];
    volatile int  head;
    volatile int  tail;
} quicVideoRing;

static volatile uint64_t quicVideoRingDrops = 0;

static void quicVideoRecvCallback(unsigned char flowType,
                                   const unsigned char* data, int dataLen,
                                   void* context) {
    (void)context;
    if (flowType != QUIC_FLOW_VIDEO)
        return;
    if (dataLen > QUIC_VIDEO_MAX_PKT)
        return;

    // §M01-A：與 AudioStream.c 的 quicAudioRing 同一個 SPSC 記憶體序問題
    // （屏障巨集與原理見 Limelight-internal.h）。舊版只靠 volatile head/tail，
    // ARM64 上消費端（VideoRecv）可能看到前進後的 head，卻讀到 slot 上一輪
    // （4096 個 datagram 前）殘留的 len，或部分 cache line 仍是舊內容的 data。
    // Android 影像沒有加密，標頭正確、payload 混了舊內容的 shard 會被
    // RtpVideoQueue 當成合法 data 收下，畫面靜默毀損直到下一個 IDR；debug 版
    // 還可能撞上 RtpVideoQueue.c 的 LC_ASSERT_VT 而 abort。
    int head = quicVideoRing.head;
    int next = (head + 1) % QUIC_VIDEO_RING_SIZE;
    if (next == quicVideoRing.tail) {
        quicVideoRingDrops++;
        return;
    }

    // §M01-A：確認 tail 已前進（消費端讀完這個 slot）之後才覆寫 slot
    QUIC_RING_ACQUIRE_FENCE();
    memcpy(quicVideoRing.data[head], data, dataLen);
    quicVideoRing.len[head] = dataLen;
    // §M01-A：data/len 必須先於 head 對消費端可見
    QUIC_RING_RELEASE_FENCE();
    quicVideoRing.head = next;
}

static int quicVideoRecv(char* buf, int bufLen) {
    int tail = quicVideoRing.tail;
    if (tail == quicVideoRing.head)
        return 0; // empty

    // §M01-A：看到 head 前進之後才讀這個 slot 的 len/data
    QUIC_RING_ACQUIRE_FENCE();
    int len = quicVideoRing.len[tail];
    if (len > bufLen)
        len = bufLen;
    memcpy(buf, quicVideoRing.data[tail], len);
    // §M01-A：讀完 slot 才把它還給生產端
    QUIC_RING_RELEASE_FENCE();
    quicVideoRing.tail = (tail + 1) % QUIC_VIDEO_RING_SIZE;
    return len;
}

static bool useQuicVideo = false;

void quicVideoGetRingStats(int* depth, int* capacity, uint64_t* drops) {
    int h = quicVideoRing.head;
    int t = quicVideoRing.tail;
    *depth = (h - t + QUIC_VIDEO_RING_SIZE) % QUIC_VIDEO_RING_SIZE;
    *capacity = QUIC_VIDEO_RING_SIZE;
    *drops = quicVideoRingDrops;
}
#endif
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

static bool receivedDataFromPeer;
static uint64_t firstDataTimeMs;
static bool receivedFullFrame;

// We can't request an IDR frame until the depacketizer knows
// that a packet was lost. This timeout bounds the time that
// the RTP queue will wait for missing/reordered packets.
#define RTP_QUEUE_DELAY 10

// This is the desired number of video packets that can be
// stored in the socket's receive buffer. 2048 is chosen
// because it should be large enough for all reasonable
// frame sizes (probably 2 or 3 frames) without using too
// much kernel memory with larger packet sizes. It also
// can smooth over transient pauses in network traffic
// and subsequent packet/frame bursts that follow.
// VipleStream §J HEVC 1440p120 server-cap diagnosis (TODO §J): client RX side
// was dropping ~50% of frames at high-bitrate-burst codecs (HEVC 1440p120 has
// ~38 KB / 27 packets per frame in 0.4 ms server-side bursts).  Bumping from
// 2048 → 8192 packets requested bumps the SO_RCVBUF target to ~12 MB at
// 1500 B/packet — still capped by Windows AFD's max but should land at
// 1-2 MB instead of the default ~256 KB Windows cap on the original 3 MB
// request.  Verify-via-getsockopt below is now always-on so we can read
// the actual realized buffer size from the log instead of guessing.
#define RTP_RECV_PACKETS_BUFFERED 8192

// Initialize the video stream
void initializeVideoStream(void) {
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&rtpQueue);
    decryptionCtx = PltCreateCryptoContext();
    receivedDataFromPeer = false;
    firstDataTimeMs = 0;
    receivedFullFrame = false;
    // §VR-REPAIR-RTT：上一場留下來的不算
    rrDelayedAny = 0;
    rrDelayedReleaseMs = 0;
    rrRulesNow = 0;

#ifdef VIPLE_MPQUIC
    // §Q-VIDEO-RING: 防禦性清空 — 與 AudioStream 同理。
    if (quicVideoRing.head != quicVideoRing.tail) {
        Limelog("[VIPLE-MPQUIC] VideoStream init: flushing %d stale QUIC video packets\n",
                (quicVideoRing.head - quicVideoRing.tail + QUIC_VIDEO_RING_SIZE) % QUIC_VIDEO_RING_SIZE);
    }
    quicVideoRing.head = 0;
    quicVideoRing.tail = 0;
    quicVideoRingDrops = 0;
    useQuicVideo = false;
#endif
}

// Clean up the video stream
void destroyVideoStream(void) {
    PltDestroyCryptoContext(decryptionCtx);
    destroyVideoDepacketizer();
    RtpvCleanupQueue(&rtpQueue);
}

// UDP Ping proc
static void VideoPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    LC_SOCKADDR saddr;

    LC_ASSERT(VideoPortNumber != 0);

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, VideoPortNumber);

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        if (VideoPingPayload.payload[0] != 0) {
            pingCount++;
            VideoPingPayload.sequenceNumber = BE32(pingCount);

            sendto(rtpSocket, (char*)&VideoPingPayload, sizeof(VideoPingPayload), 0, (struct sockaddr*)&saddr, AddrLen);
        }
        else {
            sendto(rtpSocket, legacyPingData, sizeof(legacyPingData), 0, (struct sockaddr*)&saddr, AddrLen);
        }

        PltSleepMsInterruptible(&udpPingThread, 500);
    }
}

// Receive thread proc
static void VideoReceiveThreadProc(void* context) {
    int err;
    int bufferSize, receiveSize, decryptedSize, minSize;
    char* buffer;
    char* encryptedBuffer;
    int queueStatus;
    bool useSelect;
    int waitingForVideoMs;
    bool encrypted;
#ifndef LC_FUZZING
    // §FRZ-DIAG 2026-08-07：滾動 1 秒視訊 datagram 計數。watchdog fire 時
    // 區分「有流量組不出幀」vs「幾乎無流量」兩種凍結型態（07-20 事故的
    // 每秒 reset+IDR 全無效，log 看不出當下到底有沒有封包抵達）。
    uint64_t pktWindowStartUs = 0;
    uint32_t pktWindowCount = 0;
    uint32_t pktsLastSecond = 0;
    // §FRZ-ESCALATE：watchdog 連續無效 reset 的追蹤狀態。原為 function-static
    // 會跨串流 session 殘留（上一場累積 ≥2 時新場首次 fire 就提前升級），
    // 改為 thread 區域變數，每個接收執行緒（= 每場串流）自然歸零。
    uint64_t prevFireLastFrameUs = 0;
    uint32_t consecutiveIneffectiveFires = 0;
    // §M01-D：-101 診斷，加密視訊在進 RtpVideoQueue 之前就會丟掉的封包
    // （只計數、不改行為；每個接收執行緒 = 每場串流自然歸零）。
    // 加密時落後封包在解密前就被丟，佇列的 far 組一律是 0，要看這兩個數。
    uint32_t diagDecryptFailPkts = 0;        // 解密失敗（例如 rikey 不符）
    uint32_t diagPreDecryptStalePkts = 0;    // 解密前依幀號判落後而丟棄（含正常尾端 parity）
    uint32_t diagPreDecryptFarStalePkts = 0; // 同上，lag > RTPV_DIAG_FAR_STALE_LAG
#endif

    // §VR-MULTILINK：連線在影像串流啟動前就建好了（vrmlPrepare），這裡只看數量；0＝單一路徑，以下行為不變
    const int mlLinks = vrmlLinkCount();
    // 多連線：下一幀先到時，還沒收齊的這一幀最多再等一個幀週期（等落後那條連線送來的那一份）。
    // server 每條連線最多把一批留兩個幀週期，等一個週期是延遲與救回率的折衷
    uint32_t mlHoldUs = 0;
    int mlHoldPollMs = 0;
    // §VR-REPAIR-RTT：串流的幀週期（不夾；上面的 mlHoldUs 是夾在 4～14 ms 的同一個值）
    const uint32_t mlFramePeriodUs = StreamConfig.fps > 0 ? 1000000u / (uint32_t)StreamConfig.fps : 11111u;
    if (mlLinks > 0) {
        mlHoldUs = mlFramePeriodUs;
        if (mlHoldUs < 4000) {
            mlHoldUs = 4000;
        }
        if (mlHoldUs > 14000) {
            mlHoldUs = 14000;
        }
    }
    int rxLink = -1;
    // §VR-REPAIR-RTT：每場串流重來
    memset(&rr, 0, sizeof(rr));
    rr.capOverrideMs = StreamConfig.vrRepairHoldMs > 0 ? (uint32_t)StreamConfig.vrRepairHoldMs : 0;

    encrypted = !!(EncryptionFeaturesEnabled & SS_ENC_VIDEO);
    decryptedSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    minSize = sizeof(RTP_PACKET) + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    receiveSize = decryptedSize + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    bufferSize = decryptedSize + sizeof(RTPV_QUEUE_ENTRY);
    buffer = NULL;

    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        // SO_RCVTIMEO failed, so use select() to wait
        useSelect = true;
    }
    else {
        // SO_RCVTIMEO timeout set for recv()
        useSelect = false;
    }

    // Allocate a staging buffer to use for each received packet
    if (encrypted) {
        encryptedBuffer = (char*)malloc(receiveSize);
        if (encryptedBuffer == NULL) {
            Limelog("Video Receive: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }
    }
    else {
        encryptedBuffer = NULL;
    }

    waitingForVideoMs = 0;
    while (!PltIsThreadInterrupted(&receiveThread)) {
        PRTP_PACKET packet;

        if (buffer == NULL) {
            buffer = (char*)malloc(bufferSize);
            if (buffer == NULL) {
                Limelog("Video Receive: malloc() failed\n");
                ListenerCallbacks.connectionTerminated(-1);
                break;
            }
        }

        // §VR-REPAIR-RTT：上一圈（加封包、等待到期、收到 REPAIR_GONE）若放行了被補包拖晚的幀，在開始等下一個封包之前記下來
        rrNoteDelayedRelease();

#ifdef VIPLE_MPQUIC
        if (useQuicVideo) {
            // §K.10: 不在此層加背壓 — ring buffer 必須盡快清空，
            // 否則 QUIC IO 線程持續推入會讓 ring 溢出（4096 entries
            // ≈ 5s @30fps），造成幀丟失。decode queue 容量 240
            // 已足夠吸收解碼器啟動延遲。
            err = quicVideoRecv(encrypted ? encryptedBuffer : buffer, receiveSize);
            if (err == 0) {
                // QUIC ring 空時 sleep 1ms，計時器按實際經過時間遞增
                PltSleepMs(1);
            }
        }
        else
#endif
        if (mlLinks > 0) {
            // 同時等原本的 socket 與每條連線的影像 socket；重複的封包由下面的 RtpvAddPacket 丟掉
            // 佇列有封包在等期限時，這一次最多等到期限為止（VR 沒有新畫面時 server 100 ms 才送一幀，
            // 光靠「下一個封包到了再檢查」會把已經到齊的下一幀壓住那麼久）
            // §VR-LINK-REPAIR：server 說某個 block 補不了（連線層收到的 REPAIR_GONE，同一條執行緒）。在這裡取——
            // 上一個封包已經交給佇列了；和 GONE 同一批收進來的影像封包全部處理完，vrmlTakeGone 才會給
            uint32_t goneFrame;
            uint8_t goneBlock;
            bool goneOnly = false;

            while (vrmlTakeGone(&goneFrame, &goneBlock)) {
                RtpvRepairGone(&rtpQueue, goneFrame, goneBlock);
            }
            mlHoldPollMs = RtpvHoldTimeoutMs(&rtpQueue);
            err = vrmlRecvVideo(rtpSocket,
                                encrypted ? encryptedBuffer : buffer,
                                receiveSize,
                                &rxLink,
                                mlHoldPollMs);
            while (err == 0 && vrmlTakeGone(&goneFrame, &goneBlock)) {
                // 只收到 GONE、沒有影像封包：vrmlRecvVideo 提早返回，不是逾時
                RtpvRepairGone(&rtpQueue, goneFrame, goneBlock);
                goneOnly = true;
            }
            if (goneOnly) {
                mlHoldPollMs = 0;
                continue;
            }
        }
        else {
            err = recvUdpSocket(rtpSocket,
                                encrypted ? encryptedBuffer : buffer,
                                receiveSize,
                                useSelect);
        }
        if (err < 0) {
            Limelog("Video Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            break;
        }
        else if  (err == 0) {
            RtpvPollGrace(&rtpQueue);  // §K.17-GRACE-TIMEOUT：接收逾時也要放行延後的封包
            if (mlHoldPollMs > 0) {
                // 為了等待期限而縮短的逾時：把到期的放行，不算進「一直沒有影像」的計時
                RtpvPollHold(&rtpQueue);
                mlHoldPollMs = 0;
                continue;
            }
            if (!receivedDataFromPeer) {
                // If we wait many seconds without ever receiving a video packet,
                // assume something is broken and terminate the connection.
#ifdef VIPLE_MPQUIC
                // §K.10：QUIC 模式每圈只 sleep 1ms，UDP 模式每圈
                // recvUdpSocket 阻塞 ~100ms。用正確的步進值。
                waitingForVideoMs += useQuicVideo ? 1 : UDP_RECV_POLL_TIMEOUT_MS;
#else
                waitingForVideoMs += UDP_RECV_POLL_TIMEOUT_MS;
#endif
                if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of video traffic\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                    break;
                }
            }

            // Receive timed out; try again
            continue;
        }

        if (!receivedDataFromPeer) {
            receivedDataFromPeer = true;
            Limelog("Received first video packet after %d ms\n", waitingForVideoMs);

            firstDataTimeMs = PltGetMillis();
        }

#ifndef LC_FUZZING
        // §FRZ-DIAG：此處 err > 0，計入所有收到的 datagram（含 runt / FEC /
        // 舊幀），watchdog fire 時的流量證據要的就是原始抵達量
        pktWindowCount++;

        if (!receivedFullFrame) {
            if (PltGetMillis() - firstDataTimeMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                Limelog("Terminating connection due to lack of a successful video frame\n");
                // §M01-D：-101 診斷（只加 log，不改任何行為）。判讀：
                // (a) far>0 且 maxLag 達數千以上、far 幀號是低幀號 ＝ 有另一條
                //     幀號較高的流超過佇列，也就是 host 還在送上一個 session 的
                //     畫面（殘留／殭屍 session，例如 app 被強制關閉後立刻重開），
                //     新 session 的 IDR 被 §FRZ-B1 判過期。與封包到達順序無關；
                //     firstFrameIndex 很大只是佐證，不是必要條件。
                // (b) far=0 ＝ 一般網路、IDR 或解碼問題。stalePkts>0 是已完成幀的
                //     尾端 FEC parity，屬正常（見 RtpVideoQueue.h）。
                // (c) firstFrameIndex=none 且 enc=1、decryptFail 很大 ＝ 金鑰不符
                //     （例如 host 拿舊 pending launch 的 rikey 在加密）。
                // 加密時落後封包在解密前就被丟，佇列的 far 一律是 0，(a) 改看
                // preDecryptFar（讀的是未驗證的標頭，只能當參考）。
                {
                    char firstFrameStr[16];
                    char farRangeStr[32];

                    if (rtpQueue.diagFirstFrameSeen) {
                        snprintf(firstFrameStr, sizeof(firstFrameStr), "%u", rtpQueue.diagFirstFrameIndex);
                    }
                    else {
                        snprintf(firstFrameStr, sizeof(firstFrameStr), "none");
                    }
                    // far=0 時不印範圍，避免 0..0 和 §FRZ-RESYNC-SENTINEL 的哨兵幀號 0 混淆
                    if (rtpQueue.diagFarStaleFrames != 0) {
                        snprintf(farRangeStr, sizeof(farRangeStr), "%u..%u",
                                 rtpQueue.diagFarStaleMinFrame, rtpQueue.diagFarStaleMaxFrame);
                    }
                    else {
                        snprintf(farRangeStr, sizeof(farRangeStr), "-");
                    }

                    Limelog("[VIPLE-DEPACK] §M01-D no decodable frame: firstFrameIndex=%s queueFrame=%u "
                            "stalePkts=%u far=%u pkts/%u frames (frames %s, maxLag=%u) "
                            "enc=%d decryptFail=%u preDecryptStale=%u preDecryptFar=%u\n",
                            firstFrameStr,
                            RtpvGetCurrentFrameNumber(&rtpQueue),
                            rtpQueue.diagStalePkts,
                            rtpQueue.diagFarStalePkts,
                            rtpQueue.diagFarStaleFrames,
                            farRangeStr,
                            rtpQueue.diagFarStaleMaxLag,
                            encrypted ? 1 : 0,
                            diagDecryptFailPkts,
                            diagPreDecryptStalePkts,
                            diagPreDecryptFarStalePkts);
                }
                ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_FRAME);
                break;
            }
        }

        // §FRZ-WATCHDOG 2026-07-06 凍結事故的最後防線：封包持續進來
        // （此區塊只在 err > 0 時執行）但解碼器 >2s 沒有任何完整幀產出。
        // 事故中 RtpVideoQueue 被 stale datagram 毒化（isBefore16 迴繞
        // 盲區）後把所有新鮮幀判為過期，29 秒零解碼直到使用者退出。
        // isBefore32 修復已根治已知路徑；這裡把任何未知毒化模式的凍結
        // 上限壓到 ~2 秒：重置 RTP 佇列 + 重新對齊 depacketizer 幀號 +
        // 要求 IDR。誤觸（server 出 IDR 慢於 2s）僅多要一張 IDR。
        if (receivedFullFrame) {
            static uint64_t lastWatchdogCheckUs, lastWatchdogFireUs;
            uint64_t nowUs = PltGetMicroseconds();
            if (nowUs - lastWatchdogCheckUs > 250000) {
                lastWatchdogCheckUs = nowUs;
                // §FRZ-DIAG：1 秒視窗滾動（藉 250ms 檢查節奏進行；封包完全
                // 斷流時本區塊不再執行，pktsLastSecond 凍結在最後一窗——
                // 那正是「幾乎無流量」的證據）
                if (pktWindowStartUs == 0) {
                    pktWindowStartUs = nowUs;
                }
                else if (nowUs - pktWindowStartUs >= 1000000) {
                    pktsLastSecond = pktWindowCount;
                    pktWindowCount = 0;
                    pktWindowStartUs = nowUs;
                }
                uint64_t lastFrameUs = videoDepacketizerLastFrameTimeUs();
                if (lastFrameUs != 0 &&
                    nowUs - lastFrameUs > 2000000 &&
                    nowUs - lastWatchdogFireUs > 1000000) {
                    lastWatchdogFireUs = nowUs;
                    Limelog("[VIPLE-VIDEO] §FRZ-WATCHDOG: packets flowing but "
                            "no decode unit for %.1fs (queue frame=%u, pkts last 1s=%u) — "
                            "resetting RTP queue and requesting IDR\n",
                            (float)(nowUs - lastFrameUs) / 1e6f,
                            RtpvGetCurrentFrameNumber(&rtpQueue),
                            pktsLastSecond);
                    // §FRZ-GAP-FIX 2026-07-17：reset 只為清掉可能毒化的
                    // 佇列內容，幀號連續性必須保留。RtpvInitializeQueue
                    // 會把 currentFrameNumber 打回 1，下一個到達的幀
                    // （index 數千）會被 §FRZ-GAP-FEC 判成 gap≈4096 的
                    // 整幀消失、送出 missing 數萬的 synthetic FEC status
                    // → server AIMD 被假訊號釘死在 floor（07-17 事故的
                    // 自我維持凍結迴圈：watchdog 每秒 fire 一次假訊號）。
                    //
                    // 分級復原 v2（07-20 修正）：只在「上次 fire 之後有過
                    // 解碼進展」時保留幀號；連續 fire 且期間零解碼代表
                    // 保留救不了（可能幀號本身被前向毒化——watchdog 的
                    // 原始守備範圍），需要完整 reset。
                    //
                    // v1 的完整 reset 退回 1，結果 07-20 事故實證：凍結期間
                    // 封包持續流入，reset-to-1 → 下一幀 index 數萬 →
                    // §FRZ-GAP-FEC 合成 gap=4096 假回報 → server AIMD 每窗
                    // 被打滿額 cut（§FRZ-GAP-CLAMP 只封頂不歸零）→ 07-17
                    // 互咬迴圈部分回歸（log 實證 queue frame=1 連續 fire）。
                    // v2 改用哨兵 0：「無基準幀號」，下一個到達幀被靜默採納
                    //（RtpVideoQueue 的 GAP-FEC 遇哨兵跳過合成回報）——毒化
                    // 值一樣被丟棄，且永不製造假 gap。
                    //
                    // §M01-B：RtpvInitializeQueue 的 memset 也把序號基準
                    // nextContiguousSequenceNumber 清成 0。reset 後的序號基準
                    // 由 RtpVideoQueue 的 §M01-B 用第一個開新 FEC block 的封包
                    // 重建。修正前 isBefore16(seq,0) 會把 seq ≥ 32768 的封包
                    // 全部判成過期，凍結一路卡到 16-bit 序號繞回（約 50% 的
                    // reset 中招，每秒 fire 又把基準清回 0）。
                    {
                        bool madeProgress = (lastFrameUs != prevFireLastFrameUs);
                        uint32_t savedFrameNumber = RtpvGetCurrentFrameNumber(&rtpQueue);
                        prevFireLastFrameUs = lastFrameUs;
                        RtpvCleanupQueue(&rtpQueue);
                        RtpvInitializeQueue(&rtpQueue);
                        rrOnQueueReset();  // §VR-REPAIR-RTT（量測）：佇列的累計值歸零了
                        if (madeProgress) {
                            rtpQueue.currentFrameNumber = savedFrameNumber;
                            consecutiveIneffectiveFires = 0;
                        }
                        else {
                            // §FRZ-RESYNC-SENTINEL：無基準，採納下一個到達幀
                            rtpQueue.currentFrameNumber = 0;
                            consecutiveIneffectiveFires++;
#ifdef VIPLE_MPQUIC
                            // §FRZ-ESCALATE 2026-08-07：07-20 事故型態——連續
                            // 每秒 reset+IDR 全無效且哨兵 0 從未被認領
                            // （savedFrameNumber==0 = 上次 reset 後零視訊封包
                            // 抵達），代表 ENet IDR request 雖 reliable 送達
                            // 但救不回視訊流。多發一路 QUIC marker（server 收
                            // 0x49 即出 IDR；函式自帶 200ms 節流，每秒一次
                            // 無壓力）。首兩次 fire 行為不變（計數 <3 不動作），
                            // 穩定時段 madeProgress 會歸零計數、零誤觸。
                            //
                            // §M01-B：1.5.276 以前 savedFrameNumber==0 也可能是
                            // 序號基準失效，把到達的封包在佇列入口全部誤殺
                            //（封包其實有到）。修正後才真的代表 reset 之後
                            // 沒有任何封包通過解密與佇列的幀號檢查。
                            if (consecutiveIneffectiveFires >= 3 && savedFrameNumber == 0) {
                                Limelog("[VIPLE-VIDEO] §FRZ-ESCALATE: %u consecutive "
                                        "ineffective resets, sentinel unclaimed — "
                                        "escalating IDR via QUIC marker\n",
                                        consecutiveIneffectiveFires);
                                quicSendIdrMarkerRateLimited("§FRZ-ESCALATE", "IDR");
                            }
#endif
                        }
                    }
                    videoDepacketizerRequestResync();
                    requestDecoderRefresh();
                }
            }
        }
#endif

        if (err < minSize) {
            // Runt packet
            continue;
        }

        // Decrypt the packet into the buffer if encryption is enabled
        if (encrypted) {
            PENC_VIDEO_HEADER encHeader = (PENC_VIDEO_HEADER)encryptedBuffer;

            // If this frame is below our current frame number, discard it before decryption
            // to save CPU cycles decrypting FEC shards for a frame we already reassembled.
            //
            // Since this is happening _before_ decryption, this packet is not trusted yet.
            // It's imperative that we do not mutate any state based on this packet until
            // after it has been decrypted successfully!
            //
            // It's possible for an attacker to inject a fake packet that has any value of
            // header fields they want, however this provides them no benefit because we will
            // simply drop said packet here (if it's below the current frame number) or it
            // will pass this check and be dropped during decryption (if contents is tampered)
            // or after decryption in the RTP queue (if it's a replay of a previous authentic
            // packet from the host).
            //
            // In short, an attacker spoofing this value via MITM or sending malicious values
            // impersonating the host from off-link doesn't gain them anything. If they have
            // a true MITM, they can DoS our connection by just dropping all our traffic, so
            // tampering with packets to fail this check doesn't accomplish anything they
            // couldn't already do. If they're not on-link, we just throw their malicious
            // traffic away (as mentioned in the paragraph above) and continue accepting
            // legitmate video traffic.
            if (encHeader->frameNumber && LE32(encHeader->frameNumber) < RtpvGetCurrentFrameNumber(&rtpQueue)) {
#ifndef LC_FUZZING
                // §M01-D：只計數。這裡讀的是還沒驗證的標頭（見上方說明），
                // 數字只能當參考，不能據此改任何狀態。
                diagPreDecryptStalePkts++;
                if (RtpvGetCurrentFrameNumber(&rtpQueue) - LE32(encHeader->frameNumber) > RTPV_DIAG_FAR_STALE_LAG) {
                    diagPreDecryptFarStalePkts++;
                }
#endif
                // §VR-REPAIR-RTT（量測）：同樣只計數（「放棄之後多久才到齊」；沒協商補包時什麼都不做）
                RtpvMeasLateFrame(&rtpQueue, LE32(encHeader->frameNumber));
                continue;
            }

            if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                   (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                   encHeader->iv, sizeof(encHeader->iv),
                                   encHeader->tag, sizeof(encHeader->tag),
                                   ((unsigned char*)(encHeader + 1)), err - sizeof(ENC_VIDEO_HEADER), // The ciphertext is after the header
                                   (unsigned char*)buffer, &err)) {
#ifndef LC_FUZZING
                diagDecryptFailPkts++; // §M01-D：只計數
#endif
                Limelog("Failed to decrypt video packet!\n");
                continue;
            }
        }

        // Convert fields to host byte-order
        packet = (PRTP_PACKET)&buffer[0];
        packet->sequenceNumber = BE16(packet->sequenceNumber);
        packet->timestamp = BE32(packet->timestamp);
        packet->ssrc = BE32(packet->ssrc);

        rtpQueue.multiLink = mlLinks > 0;
        {
            // 這個封包的決定：等不等、回不回報、照哪一套規則。判斷全部在 vrrrDecide()（VrRepairRtt.h，有單元測試）：
            //
            // 原本的規則（vrrrDecide 的最後一段，和之前寫在這裡的算式逐位元相同）——有東西可等才等：至少兩條連線實際在送
            // 影像（另一條的那一份），或 server 會補包（§VR-LINK-REPAIR）。只有一條在送、又不能補包時（單網卡、primary、
            // 另一條還在探測）等只會讓掉幀回報晚一個幀週期。
            // 往返時間太長也不回報（legacyRttMs＝−1 是沒協商，或沒有已確認的連線、回報送不出去）：回報→補包至少一個往返，
            // 補包到的時候這一幀早就被放棄了——2026-10-07 在往返 18～39 ms 的 Wi-Fi 上實測，每 10 秒回報 30 次，補的封包
            // 只是在已經很擠的鏈路上再加碼。門檻＝一個半幀週期減 2 ms（90 Hz 約 14 ms）：缺一段的幀在尾端到的時候就回報了，
            // 離「下一幀到了之後再等一個幀週期」的期限還有將近兩個週期，往返十毫秒上下仍然來得及。頭盔實測適配器那條的
            // 往返中位數 3 ms、九成在 10 ms 以內、最大 13 ms；門檻訂在一個幀週期時，另一條剛好卡住的那一刻補包就被關掉，
            // 本來救得回來的幀掉了。重報間隔＝回報→補包大約一個往返，再給 3 ms（server 排進佇列、送出），夾在 4～10 ms。
            //
            // §VR-REPAIR-RTT：server 與 client 都同意（VIPLE_VR_LINK_F_REPAIR_RTT）、只有一條連線在送影像、那條連線有往返
            // 時間的樣本時，門檻、等待上限與重報改成跟著量到的往返時間走。沒協商時 vrrrDecide 一定走原本那一段。
            VRRR_IN in;
            VRRR_OUT out;
            const int feat = mlLinks > 0 ? vrmlFeatures() : 0;
            // 上一個封包（與它之後的等待逾時、REPAIR_GONE）是照 RTT 規則處理的，那段時間的結果才交給保險
            const bool prevRtt = rtpQueue.mlRttMode;
            bool haveRtt;

            memset(&in, 0, sizeof(in));
            in.negotiatedRepair = (feat & VIPLE_VR_LINK_F_REPAIR) != 0;
            in.negotiatedRtt = (feat & VIPLE_VR_LINK_F_REPAIR_RTT) != 0;
            in.carrying = vrmlVideoCarrying();
            in.legacyRttMs = in.negotiatedRepair ? vrmlCtrlRttMs() : -1;
            rtpQueue.mlCarryMask = (uint8_t)vrmlVideoCarryMask();
            in.nowUs = in.negotiatedRepair ? PltGetMicroseconds() : 0;
            // 往返時間的統計只看正在送影像的連線（補包只從那幾條回來）。沒協商 RTT 規則時也算：量測要用最短的那一筆
            haveRtt = in.negotiatedRepair && vrmlRepairRtt(rtpQueue.mlCarryMask, &in.st);
            in.haveStats = in.negotiatedRtt && haveRtt;
            in.armRtt = !in.negotiatedRtt || rrArmRtt(in.nowUs);
            in.holdUs = mlHoldUs;
            in.framePeriodUs = mlFramePeriodUs;
            in.capOverrideMs = rr.capOverrideMs;
            in.idlePolicy = (uint8_t)StreamConfig.vrRepairIdle;
            in.latched = in.negotiatedRtt && RtpvRepairLatched(&rtpQueue);
            in.okDelta = prevRtt ? rtpQueue.mlTot.repairedUsed - rr.seenOk : 0;
            in.badDelta = prevRtt ? rtpQueue.mlTot.lostNacked - rr.seenBad : 0;
            rr.seenOk = rtpQueue.mlTot.repairedUsed;
            rr.seenBad = rtpQueue.mlTot.lostNacked;
            vrrrDecide(&in, &rr.state, &out);
            RtpvSetRepairDecision(&rtpQueue, &out, mlFramePeriodUs, StreamConfig.vrRepairNoAhead == 0);
            if (out.mlRepair) {
                // 各條連線平常晚多久（要在算出這個封包的 mlRepair 之後才填，否則剛變成可以補包的那個封包拿到的是舊值）
                int ml;
                for (ml = 0; ml < RTPV_ML_LINKS; ml++) {
                    rtpQueue.mlLinkLagUs[ml] = vrmlVideoLinkLagUs(ml);
                }
            }
            // 量測（不分規則）：協商了補包就記帳、每 10 秒印一行，不管補包這一刻開著沒有
            RtpvSetRepairMeas(&rtpQueue, in.negotiatedRepair, in.nowUs, haveRtt ? in.st.loUs : 0);
            rrRulesNow = out.mlRttMode ? 1 : 0;
            if (in.negotiatedRepair) {
                rrNote(&in, &out);
            }
        }
        rtpQueue.mlRxLink = rxLink;
        queueStatus = RtpvAddPacket(&rtpQueue, packet, err, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);

        if (queueStatus != RTPF_RET_REJECTED) {
            // The queue owns the buffer
            buffer = NULL;
            if (mlLinks > 0 && queueStatus == RTPF_RET_QUEUED) {
                vrmlNoteVideoUsed(rxLink);  // 這條連線的這一份先到、被採用（排著的等放行時由佇列自己計）
            }
        }
    }

    if (rr.ab.startUs != 0) {
        rrAbSummary(PltGetMicroseconds());  // §VR-REPAIR-RTT：--vr-repair-ab 最後那一邊（多半沒跑滿）的小結
    }

    if (buffer != NULL) {
        free(buffer);
    }

    if (encryptedBuffer != NULL) {
        free(encryptedBuffer);
    }
}

void notifyKeyFrameReceived(void) {
    // Remember that we got a full frame successfully
    receivedFullFrame = true;
}

// Decoder thread proc
static void VideoDecoderThreadProc(void* context) {
    while (!PltIsThreadInterrupted(&decoderThread)) {
        VIDEO_FRAME_HANDLE frameHandle;
        PDECODE_UNIT decodeUnit;

        if (!LiWaitForNextVideoFrame(&frameHandle, &decodeUnit)) {
            return;
        }

        LiCompleteVideoFrame(frameHandle, VideoCallbacks.submitDecodeUnit(decodeUnit));
    }
}

// Read the first frame of the video stream
int readFirstFrame(void) {
    // All that matters is that we close this socket.
    // This starts the flow of video on Gen 3 servers.

    closeSocket(firstFrameSocket);
    firstFrameSocket = INVALID_SOCKET;

    return 0;
}

// Terminate the video stream
void stopVideoStream(void) {
    if (!receivedDataFromPeer) {
        Limelog("No video traffic was ever received from the host!\n");
    }

    VideoCallbacks.stop();

    // Wake up client code that may be waiting on the decode unit queue
    stopVideoDepacketizer();

    PltInterruptThread(&udpPingThread);
    PltInterruptThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltInterruptThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        shutdownTcpSocket(firstFrameSocket);
    }

    PltJoinThread(&udpPingThread);
    PltJoinThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltJoinThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        closeSocket(firstFrameSocket);
        firstFrameSocket = INVALID_SOCKET;
    }
    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

#ifdef VIPLE_MPQUIC
    // §Q-VIDEO-RING: session 結束後取消 QUIC video callback + 清空 ring。
    // §M01-A 已知（良性）：取消 callback 不是同步的，QuicIO 執行緒可能此刻
    // 仍在 quicVideoRecvCallback 裡，在下面 reset 之後又把 head 寫回 next；
    // 由下一場 initializeVideoStream() 的 flush 清掉。另案見 docs/TODO.md。
    if (useQuicVideo) {
        quicSetRecvCallbackForFlow(QUIC_FLOW_VIDEO, NULL, NULL);
    }
    quicVideoRing.head = 0;
    quicVideoRing.tail = 0;
    useQuicVideo = false;
#endif

    VideoCallbacks.cleanup();
}

// Start the video stream
int startVideoStream(void* rendererContext, int drFlags) {
    int err;

    firstFrameSocket = INVALID_SOCKET;

    // This must be called before the decoder thread starts submitting
    // decode units
    LC_ASSERT(NegotiatedVideoFormat != 0);
    err = VideoCallbacks.setup(NegotiatedVideoFormat, StreamConfig.width,
        StreamConfig.height, StreamConfig.fps, rendererContext, drFlags);
    if (err != 0) {
        return err;
    }

#ifdef VIPLE_MPQUIC
    // §VR：rc 以前 VR 視訊強制走 RTP/UDP（server 端 stream.cpp 同步強制，
    // vr_architecture.md §3.5）；QUIC 仍保留給控制與 tracking 的 fallback。
    // 接收執行緒在 useQuicVideo 時只讀 QUIC ring、不讀 UDP socket，所以兩端必須一致。
    useQuicVideo = (StreamConfig.useQuicTransport && quicIsConnected() &&
                    !(VrFlags & VIPLE_VR_SF_ENABLED));
    if (StreamConfig.useQuicTransport && (VrFlags & VIPLE_VR_SF_ENABLED)) {
        Limelog("[VIPLE-VR-SESSION] VR session: video over RTP/UDP (QUIC kept for control/tracking)\n");
    }
    if (useQuicVideo) {
        // Reset ring buffer and register callback
        quicVideoRing.head = 0;
        quicVideoRing.tail = 0;
        quicSetRecvCallbackForFlow(QUIC_FLOW_VIDEO, quicVideoRecvCallback, NULL);
        Limelog("[VIPLE-MPQUIC] Video using QUIC datagram transport\n");
        // Still bind UDP socket as fallback (needed for ping)
    }
#endif

    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen,
                              RTP_RECV_PACKETS_BUFFERED * (StreamConfig.packetSize + MAX_RTP_HEADER_SIZE),
                              SOCK_QOS_TYPE_VIDEO);
    if (rtpSocket == INVALID_SOCKET) {
        VideoCallbacks.cleanup();
        return LastSocketError();
    }

    VideoCallbacks.start();

    err = PltCreateThread("VideoRecv", VideoReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        VideoCallbacks.stop();
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
        VideoCallbacks.cleanup();
        return err;
    }

    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        err = PltCreateThread("VideoDec", VideoDecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            VideoCallbacks.stop();
            PltInterruptThread(&receiveThread);
            PltJoinThread(&receiveThread);
            closeSocket(rtpSocket);
            rtpSocket = INVALID_SOCKET;
            VideoCallbacks.cleanup();
            return err;
        }
    }

    if (AppVersionQuad[0] == 3) {
        // Connect this socket to open port 47998 for our ping thread
        firstFrameSocket = connectTcpSocket(&RemoteAddr, AddrLen,
                                            FIRST_FRAME_PORT, FIRST_FRAME_TIMEOUT_SEC);
        if (firstFrameSocket == INVALID_SOCKET) {
            VideoCallbacks.stop();
            stopVideoDepacketizer();
            PltInterruptThread(&receiveThread);
            if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
                PltInterruptThread(&decoderThread);
            }
            PltJoinThread(&receiveThread);
            if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
                PltJoinThread(&decoderThread);
            }
            closeSocket(rtpSocket);
            rtpSocket = INVALID_SOCKET;
            VideoCallbacks.cleanup();
            return LastSocketError();
        }
    }

    // Start pinging before reading the first frame so GFE knows where
    // to send UDP data
    err = PltCreateThread("VideoPing", VideoPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        VideoCallbacks.stop();
        stopVideoDepacketizer();
        PltInterruptThread(&receiveThread);
        if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
            PltInterruptThread(&decoderThread);
        }
        PltJoinThread(&receiveThread);
        if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
            PltJoinThread(&decoderThread);
        }
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
        if (firstFrameSocket != INVALID_SOCKET) {
            closeSocket(firstFrameSocket);
            firstFrameSocket = INVALID_SOCKET;
        }
        VideoCallbacks.cleanup();
        return err;
    }

    if (AppVersionQuad[0] == 3) {
        // Read the first frame to start the flow of video
        err = readFirstFrame();
        if (err != 0) {
            stopVideoStream();
            return err;
        }
    }

    return 0;
}

const RTP_VIDEO_STATS* LiGetRTPVideoStats(void) {
    return &rtpQueue.stats;
}

// §VR-REPAIR-RTT：距上一次「被補包機制拖晚的幀放行」幾毫秒。只有照 RTT 規則等待時佇列才會記（見 RtpVideoQueue.c 的
// mlDelayedReleaseUs），所以 server 的主開關關著時恆為 UINT32_MAX
uint32_t LiGetVrRepairQuietMs(void) {
    if (!rrDelayedAny) {
        return UINT32_MAX;
    }
    return (uint32_t)PltGetMillis() - rrDelayedReleaseMs;  // 都是毫秒時戳的低 32 bit，繞回也正確
}

int LiGetVrRepairRules(void) {
    return rrRulesNow;
}
