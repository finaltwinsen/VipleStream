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
        {
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

        queueStatus = RtpvAddPacket(&rtpQueue, packet, err, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);

        if (queueStatus == RTPF_RET_QUEUED) {
            // The queue owns the buffer
            buffer = NULL;
        }
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
