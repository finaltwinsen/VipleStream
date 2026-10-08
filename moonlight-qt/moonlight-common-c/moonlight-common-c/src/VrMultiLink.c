// VipleStream 2.0 §VR-MULTILINK：VR session 的多連線（client 端）。說明見 VrMultiLink.h。
//
// 執行緒：
//   - 影像接收執行緒：vrmlRecvVideo／vrmlNoteVideoUsed（收封包、PONG、每條連線的接收統計）
//   - 音訊接收執行緒：vrmlRecvAudio
//   - ping 執行緒：每 250 ms 在每條連線送 PING、每 10 s 印統計、LINK_READY 沒回來時重送 LINK_HELLO
//   - control 接收執行緒：vrmlOnS2C（LINK_READY）
//   - 追蹤執行緒：vrmlSendTracking；VrSend 執行緒與 XR 執行緒：vrmlSendCtrl（LOSS、LATCH）
// 每個統計欄位只有一條執行緒寫，其他執行緒讀到稍舊的值也無妨，所以不加鎖；加密用的 context 有自己的鎖。

#include "Limelight-internal.h"
#include "VrMultiLink.h"

#ifdef VIPLE_MPQUIC

#include "PlatformNetIf.h"

// 與 VideoStream.c 的 RTP_RECV_PACKETS_BUFFERED 相同：每條連線的影像 socket 要能裝下同樣多的封包
#define VRML_RECV_PACKETS_BUFFERED 8192
#define VRML_PING_INTERVAL_MS     250
#define VRML_CONFIRM_TIMEOUT_MS   1000   // 這麼久沒收到 PONG 就不算已確認（PING 不再帶 CONFIRMED）；實測的空檔 50～170 ms，1 s 夠寬
#define VRML_RUN_MIN_DUR_US       300    // 一幀的到達時間差小於這個值：快到量不出來（同一個聚合框交上來），回報 65535
#define VRML_CARRY_WINDOW_US      200000 // 「這條連線有沒有在送影像」的統計視窗
#define VRML_CARRY_KEEP_WINDOWS   5      // 曾經有兩條在送之後，連續這麼多個視窗都不足才算「只剩一條」（一條連線停頓 150～200 ms 是常態）
#define VRML_HELLO_RETRY_MS       2000
#define VRML_HELLO_MAX_TRIES      5
#define VRML_STATS_INTERVAL_MS    10000
#define VRML_REPOLL_EVERY         16     // 連續收這麼多個封包就重新看一次其他 socket，免得一條連線的積壓擋住另一條
#define VRML_RUN_IDLE_US          20000  // 這麼久沒有下一個封包：這一幀在這條連線算結束了（約兩個幀週期，server 也只留這麼久）
#define VRML_RUN_MIN_PKTS         32     // 至少這麼多個才算一次速率量測
#define VRML_STAGE_MAX            256    // 一次最多暫存、排序的影像封包數（約一幀）
#define VRML_LEAD_RING            8      // 記最近幾幀「最先送到的那一份」的到達時刻（量各條連線晚多久用）
#define VRML_LAG_MAX_US           50000  // 一次取樣的上限
#define VRML_LAG_HIST             4      // 上升時看最近幾個樣本（取第二大：單一離群值不採用）
#define VRML_LEAD_JUMP_MAX        4096   // 幀號比目前最新的大超過這麼多：不可信（影像加密時幀號取自還沒驗證的標頭）
#define VRML_LEAD_STALE_RESET     64     // 連續這麼多次遇到「比最新的舊太多」：紀錄多半被壞封包帶歪了，整個重來

typedef struct _VRML_LINK {
    uint8_t id;
    bool primary;                 // session 本身連線的那一組位址
    char ifName[LC_NETIF_MAX_NAME];
    struct sockaddr_in local;     // 本機位址（埠由系統配）
    struct sockaddr_in serverV;   // server 位址＋影像埠（LINK_READY 之前埠是 0）
    struct sockaddr_in serverA;
    SOCKET videoSock;
    SOCKET audioSock;
    uint16_t localVideoPort;
    uint16_t localAudioPort;
    volatile bool ready;          // LINK_READY 回了 OK
    bool refused;

    // ping／pong
    uint32_t pingSeq;
    volatile uint64_t lastPongMs; // 影像接收執行緒寫
    volatile uint16_t rttMs10;

    // 接收統計（影像接收執行緒寫）
    volatile uint32_t rxPkts;
    volatile uint32_t rxUsed;
    uint64_t lastRxUs;
    volatile uint32_t gapMaxUsPing;   // 上一個 PING 以來
    volatile uint32_t gapMaxUsStats;  // 上一次統計以來
    uint32_t rxPktsPrev;
    uint32_t rxUsedPrev;

    // 到達速率量測（影像接收執行緒寫）：同一幀在這條連線到達的封包＝一次量測（從第一個到最後一個花了多久）。
    // 不以封包間的空檔分段：鏈路被擠到一小撮一小撮送時，分段會把每一小撮都量成「很快」
    uint32_t runFrame;
    uint32_t winPkts;        // 這個統計視窗收到的影像封包數（判斷有沒有在送影像）
    uint32_t runCount;
    uint32_t runBytes;
    uint64_t runStartTsUs;   // 這一串第一個／最後一個封包的到達時刻（有核心時戳就用它）
    uint64_t runLastTsUs;
    uint64_t runLastUs;      // 最後一個封包的本機時刻（判斷這一串結束了沒）
    volatile uint16_t burstMbps;
    volatile uint8_t burstSeq;

    // 這條連線平常比最先送到的那條晚多久（影像接收執行緒寫）：每一幀取一次樣，快升慢降
    bool lagValid;
    uint8_t lagHistPos;
    uint8_t lagHistCount;
    uint32_t lagFrame;       // 最近取樣的幀
    uint32_t lagHist[VRML_LAG_HIST];
    volatile uint32_t lagUs;

    // 追蹤（追蹤執行緒寫）
    volatile uint32_t trkSent;
    volatile uint32_t trkDrop;
    uint32_t trkSentPrev;
    uint32_t trkDropPrev;
} VRML_LINK;

typedef struct _VRML_RXSET {
    SOCKET socks[VIPLE_VR_LINK_MAX + 1];
    int8_t link[VIPLE_VR_LINK_MAX + 1];   // -1＝原本的 socket
    bool readable[VIPLE_VR_LINK_MAX + 1];
    bool dead[VIPLE_VR_LINK_MAX + 1];
    int count;
    int next;
    int sincePoll;
    uint64_t lastTsUs;   // 最近收到的那個封包的核心時戳（us）；0＝這個平台或這個 socket 沒有
} VRML_RXSET;

static VRML_LINK links[VIPLE_VR_LINK_MAX];
static int linkCount;
static bool started;
static PLT_THREAD pingThread;
static PLT_MUTEX trkMutex;
static bool trkMutexCreated;  // 只建立一次、不刪除：追蹤執行緒可能在 session 收尾時還握著它
static PPLT_CRYPTO_CONTEXT trkCrypto;
static uint32_t dataSeq;
static volatile bool helloAcked;
static VRML_RXSET videoSet;
static VRML_RXSET audioSet;
static int videoPollMs = UDP_RECV_POLL_TIMEOUT_MS;  // 影像這一次 poll 最多等多久（影像接收執行緒寫）
static uint64_t carryWinStartUs;
static int carryKeep;
static volatile int carryLinks;
static volatile uint8_t carryMask;         // 哪幾條在送影像（bit i＝links[i]；遲滯期間留著上一次的）
static struct {
    bool valid;
    bool paired;     // 已經有另一條連線也送到這一幀（領先那條的樣本算過了）
    int8_t link;     // 最先送到的是哪一條
    uint32_t frame;
    uint64_t us;
} leadRing[VRML_LEAD_RING];                // 每一幀最先送到的那一份的到達時刻（只在影像接收執行緒存取）
static bool leadAny;
static uint32_t leadNewest;
static int leadStale;
static volatile uint32_t legacyRxPkts;

// §VR-LINK-CTRL
static volatile uint8_t hubCaps;           // server 同意啟用的 VIPLE_VR_LINK_F_*（control 接收執行緒寫）
static PPLT_CRYPTO_CONTEXT s2cCrypto;      // T_S2C 的解密（只在影像接收執行緒用）
static bool s2cAny;                        // 以下三個＝T_S2C 的去重視窗（只在影像接收執行緒存取）
static uint32_t s2cTop;
static uint64_t s2cMask;
static volatile uint32_t ctlSent, ctlDrop; // 送出的 0x5507（則數）／沒有任何一條連線送得出去的
static bool gonePending;                   // §VR-LINK-REPAIR：最近一則 REPAIR_GONE（只在影像接收執行緒存取）
static uint32_t goneFrame;
static uint8_t goneBlock;
static volatile uint32_t s2cRx, s2cDup, s2cBad;
static uint32_t ctlSentPrev, ctlDropPrev, s2cRxPrev, s2cDupPrev, s2cBadPrev;

// 影像封包的暫存（只在影像接收執行緒存取）
typedef struct _VRML_STAGED {
    uint32_t frame;
    uint32_t seq;    // 未加密：RTP 序號（16 bit）；加密：IV 計數器的低 32 bit
    uint16_t slot;   // 在 stageBuf 裡的格子
    uint16_t len;
    int8_t link;     // -1＝原本的 socket
} VRML_STAGED;
static char* stageBuf;
static int stageSlotSize;
static VRML_STAGED stageIdx[VRML_STAGE_MAX];
static int stageCount;
static int stageNext;
static uint32_t legacyRxPktsPrev;

static bool sameSubnet4(uint32_t aBe, uint32_t bBe, int prefixLen) {
    uint32_t mask;
    if (prefixLen <= 0 || prefixLen > 32) {
        return false;
    }
    mask = prefixLen == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefixLen);
    return ((ntohl(aBe) ^ ntohl(bBe)) & mask) == 0;
}

static bool isConfirmed(const VRML_LINK* L, uint64_t nowMs) {
    uint64_t t = L->lastPongMs;
    // t 可能比 nowMs 新（另一條執行緒剛更新）：無號相減會下溢，要先比大小
    return L->ready && t != 0 && (t >= nowMs || nowMs - t < VRML_CONFIRM_TIMEOUT_MS);
}

static void closeLinkSockets(VRML_LINK* L) {
    if (L->videoSock != INVALID_SOCKET) {
        closeSocket(L->videoSock);
        L->videoSock = INVALID_SOCKET;
    }
    if (L->audioSock != INVALID_SOCKET) {
        closeSocket(L->audioSock);
        L->audioSock = INVALID_SOCKET;
    }
}

static uint16_t boundPort(SOCKET s) {
    struct sockaddr_in sa;
    SOCKADDR_LEN len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (getsockname(s, (struct sockaddr*)&sa, &len) != 0) {
        return 0;
    }
    return ntohs(sa.sin_port);
}

static SOCKET openLinkSocket(const VRML_LINK* L, int bufferSize, int qosType) {
    struct sockaddr_storage ss;
    SOCKET s;

    memset(&ss, 0, sizeof(ss));
    memcpy(&ss, &L->local, sizeof(L->local));
    s = bindUdpSocket(AF_INET, &ss, sizeof(struct sockaddr_in), bufferSize, qosType);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
#if defined(__linux__) && defined(SO_BINDTODEVICE)
    // Linux 是 weak host 模型：只 bind 位址不保證封包從那張網卡出去。主連線維持和原本的 socket 一樣（不綁裝置）。
    if (!L->primary && L->ifName[0] != '\0') {
        if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, L->ifName, (socklen_t)strlen(L->ifName) + 1) != 0) {
            Limelog("[VIPLE-VR-LINK] SO_BINDTODEVICE '%s' failed (errno %d); relying on routing\n", L->ifName, (int)LastSocketError());
        }
    }
#endif
#if defined(__linux__) && defined(SO_TIMESTAMPNS)
    {
        // 影像封包的到達時間用核心收到的時刻（量每條連線的到達速率用）：在 user space 讀取的時刻會把
        // 已經排在 socket 裡的封包擠成同一瞬間，量出來的是讀取速度
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
    }
#endif
    setSocketNonBlocking(s, true);
    return s;
}

static bool addLink(uint32_t localBe, uint32_t serverBe, const char* ifName, bool primary) {
    VRML_LINK* L;
    char localStr[INET_ADDRSTRLEN], serverStr[INET_ADDRSTRLEN];

    if (linkCount >= VIPLE_VR_LINK_MAX) {
        return false;
    }
    L = &links[linkCount];
    memset(L, 0, sizeof(*L));
    L->id = (uint8_t)(linkCount + 1);
    L->primary = primary;
    L->videoSock = INVALID_SOCKET;
    L->audioSock = INVALID_SOCKET;
    L->rttMs10 = VIPLE_VR_LINK_RTT_UNKNOWN;
    if (ifName != NULL) {
        snprintf(L->ifName, sizeof(L->ifName), "%s", ifName);
    }
    L->local.sin_family = AF_INET;
    L->local.sin_addr.s_addr = localBe;
    L->serverV.sin_family = AF_INET;
    L->serverV.sin_addr.s_addr = serverBe;
    L->serverA = L->serverV;

    L->videoSock = openLinkSocket(L, VRML_RECV_PACKETS_BUFFERED * (StreamConfig.packetSize + MAX_RTP_HEADER_SIZE), SOCK_QOS_TYPE_VIDEO);
    L->audioSock = openLinkSocket(L, 0, SOCK_QOS_TYPE_AUDIO);
    if (L->videoSock == INVALID_SOCKET || L->audioSock == INVALID_SOCKET) {
        Limelog("[VIPLE-VR-LINK] could not open sockets for link on '%s' (error %d)\n", L->ifName, (int)LastSocketError());
        closeLinkSockets(L);
        return false;
    }
    L->localVideoPort = boundPort(L->videoSock);
    L->localAudioPort = boundPort(L->audioSock);
    if (L->localVideoPort == 0 || L->localAudioPort == 0) {
        closeLinkSockets(L);
        return false;
    }

    inet_ntop(AF_INET, &L->local.sin_addr, localStr, sizeof(localStr));
    inet_ntop(AF_INET, &L->serverV.sin_addr, serverStr, sizeof(serverStr));
    Limelog("[VIPLE-VR-LINK] link %d: '%s' %s <-> server %s%s\n", (int)L->id, L->ifName, localStr, serverStr,
            primary ? " (session address)" : "");
    linkCount++;
    return true;
}

int vrmlPrepare(void) {
    LC_NET_INTERFACE ifs[LC_NETIF_MAX_COUNT];
    uint32_t localBe, serverBe;
    uint32_t candidates[QUIC_MAX_ALT_PEERS + 1];
    int candidateCount = 0;
    int ifCount, i, c;
    const char* primaryIf = NULL;

    linkCount = 0;
    started = false;
    helloAcked = false;
    dataSeq = 0;
    legacyRxPkts = 0;
    legacyRxPktsPrev = 0;
    hubCaps = 0;
    gonePending = false;
    s2cAny = false;
    s2cTop = 0;
    s2cMask = 0;
    ctlSent = ctlDrop = s2cRx = s2cDup = s2cBad = 0;
    ctlSentPrev = ctlDropPrev = s2cRxPrev = s2cDupPrev = s2cBadPrev = 0;
    memset(&videoSet, 0, sizeof(videoSet));
    memset(&audioSet, 0, sizeof(audioSet));
    videoPollMs = UDP_RECV_POLL_TIMEOUT_MS;
    carryWinStartUs = 0;
    carryKeep = 0;
    carryLinks = 0;
    carryMask = 0;
    memset(leadRing, 0, sizeof(leadRing));
    leadAny = false;
    leadNewest = 0;
    leadStale = 0;
    stageCount = 0;
    stageNext = 0;

    if (!(StreamConfig.vrFlags & VIPLE_VR_SF_ENABLED) || !(StreamConfig.vrFlags & VIPLE_VR_SF_MULTILINK)) {
        return 0;
    }
    if (RemoteAddr.ss_family != AF_INET || LocalAddr.ss_family != AF_INET) {
        Limelog("[VIPLE-VR-LINK] multi-link needs an IPv4 session; staying on the single path\n");
        return 0;
    }
    if (!(EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2)) {
        Limelog("[VIPLE-VR-LINK] multi-link needs the encrypted control stream; staying on the single path\n");
        return 0;
    }

    localBe = ((struct sockaddr_in*)&LocalAddr)->sin_addr.s_addr;
    serverBe = ((struct sockaddr_in*)&RemoteAddr)->sin_addr.s_addr;

    ifCount = lcEnumNetInterfaces(ifs, LC_NETIF_MAX_COUNT);
    if (ifCount < 0) {
        ifCount = 0;
    }
    for (i = 0; i < ifCount; i++) {
        if (ifs[i].family == AF_INET && ((struct sockaddr_in*)&ifs[i].addr)->sin_addr.s_addr == localBe) {
            primaryIf = ifs[i].name;
        }
    }

    // 主連線：session 本身用的那一組位址（已經證明通，不要求同子網路）
    if (!addLink(localBe, serverBe, primaryIf, true)) {
        Limelog("[VIPLE-VR-LINK] could not set up the primary link; staying on the single path\n");
        linkCount = 0;
        return 0;
    }

    // 其他網卡：只配「和這張網卡同子網路」的 server 位址（清單來自 /serverinfo 的介面通告）
    candidates[candidateCount++] = serverBe;
    for (c = 0; c < StreamConfig.quicAltPeerCount && c < QUIC_MAX_ALT_PEERS; c++) {
        struct in_addr a;
        if (inet_pton(AF_INET, StreamConfig.quicAltPeers[c], &a) == 1) {
            candidates[candidateCount++] = a.s_addr;
        }
    }
    for (i = 0; i < ifCount; i++) {
        uint32_t ifBe;
        if (!ifs[i].up || ifs[i].family != AF_INET || ifs[i].type == LC_NETIF_TYPE_LOOPBACK) {
            continue;
        }
#ifdef __linux__
        // USB 網路 gadget（usb0／rndis0／ncm0）目前一有 UDP 流量就卡死、要實體重插（docs/steam_frame_client.md §8.7），
        // 觸發條件定位之前不拿來當連線
        if (strncmp(ifs[i].name, "usb", 3) == 0 || strncmp(ifs[i].name, "rndis", 5) == 0 || strncmp(ifs[i].name, "ncm", 3) == 0) {
            Limelog("[VIPLE-VR-LINK] skipping '%s' (USB network link is not used for multi-link)\n", ifs[i].name);
            continue;
        }
#endif
        ifBe = ((struct sockaddr_in*)&ifs[i].addr)->sin_addr.s_addr;
        if (ifBe == localBe) {
            continue;
        }
        for (c = 0; c < candidateCount; c++) {
            if (sameSubnet4(ifBe, candidates[c], ifs[i].prefixLen)) {
                addLink(ifBe, candidates[c], ifs[i].name, false);
                break;
            }
        }
    }

    // 開發用：只有一張網卡時在同一張網卡再開一條，驗複製與去重
    if ((StreamConfig.vrFlags & VIPLE_VR_SF_MULTILINK_SELFTEST) && linkCount == 1) {
        addLink(localBe, serverBe, primaryIf, false);
    }

    Limelog("[VIPLE-VR-LINK] %d link(s) prepared (interfaces=%d, server addresses=%d)\n", linkCount, ifCount, candidateCount);
    return linkCount;
}

int vrmlLinkCount(void) {
    return linkCount;
}

static void sendHello(void) {
    uint8_t tlv[2 + 1 + VIPLE_VR_LINK_MAX * sizeof(VIPLE_VR_LINK_DESC)];
    int i;

    tlv[0] = VIPLE_VR_C2S_LINK_HELLO;
    tlv[1] = (uint8_t)(1 + linkCount * sizeof(VIPLE_VR_LINK_DESC));
    tlv[2] = (uint8_t)linkCount;
    for (i = 0; i < linkCount; i++) {
        VIPLE_VR_LINK_DESC d;
        memset(&d, 0, sizeof(d));
        d.linkId = links[i].id;
        d.flags = VIPLE_VR_LINK_F_CTRL | VIPLE_VR_LINK_F_REPAIR;  // 我們支援的；要不要啟用由 server 決定（LINK_READY 的 hubCaps）
        d.clientVideoPort = LE16(links[i].localVideoPort);
        d.clientAudioPort = LE16(links[i].localAudioPort);
        memcpy(d.clientAddr, &links[i].local.sin_addr.s_addr, 4);
        memcpy(d.serverAddr, &links[i].serverV.sin_addr.s_addr, 4);
        memcpy(&tlv[3 + i * sizeof(d)], &d, sizeof(d));
    }
    if (LiSendVrMessage(tlv, 2 + tlv[1], true) != 0) {
        Limelog("[VIPLE-VR-LINK] LINK_HELLO could not be sent\n");
    }
}

static void logStats(uint64_t nowMs) {
    char line[768];
    int off = 0;
    int i;
    uint32_t lg = legacyRxPkts;

    off += snprintf(line + off, sizeof(line) - off, "legacy rx=%u", lg - legacyRxPktsPrev);
    legacyRxPktsPrev = lg;
    for (i = 0; i < linkCount && off < (int)sizeof(line) - 160; i++) {
        VRML_LINK* L = &links[i];
        uint32_t rx = L->rxPkts, used = L->rxUsed, ts = L->trkSent, td = L->trkDrop;
        uint32_t gap = L->gapMaxUsStats;
        uint16_t rtt = L->rttMs10;
        char rttStr[16];

        L->gapMaxUsStats = 0;
        if (rtt == VIPLE_VR_LINK_RTT_UNKNOWN) {
            snprintf(rttStr, sizeof(rttStr), "-");
        }
        else {
            snprintf(rttStr, sizeof(rttStr), "%.1f", rtt / 10.0);
        }
        off += snprintf(line + off, sizeof(line) - off,
                        " | L%d %s %s rx=%u used=%u gapMaxMs=%.1f rttMs=%s burstMbps=%u lagMs=%.1f trk=%u drop=%u",
                        (int)L->id, L->ifName[0] ? L->ifName : "?",
                        L->refused ? "refused" : (isConfirmed(L, nowMs) ? "up" : (L->ready ? "unconfirmed" : "pending")),
                        rx - L->rxPktsPrev, used - L->rxUsedPrev, gap / 1000.0, rttStr, (unsigned)L->burstMbps,
                        L->lagUs / 1000.0, ts - L->trkSentPrev, td - L->trkDropPrev);
        L->rxPktsPrev = rx;
        L->rxUsedPrev = used;
        L->trkSentPrev = ts;
        L->trkDropPrev = td;
    }
    if (hubCaps != 0 && off < (int)sizeof(line) - 96) {
        uint32_t cs = ctlSent, cd = ctlDrop, sr = s2cRx, sd = s2cDup, sb = s2cBad;
        off += snprintf(line + off, sizeof(line) - off, " | ctl tx=%u drop=%u s2c rx=%u dup=%u bad=%u",
                        cs - ctlSentPrev, cd - ctlDropPrev, sr - s2cRxPrev, sd - s2cDupPrev, sb - s2cBadPrev);
        ctlSentPrev = cs;
        ctlDropPrev = cd;
        s2cRxPrev = sr;
        s2cDupPrev = sd;
        s2cBadPrev = sb;
    }
    Limelog("[VIPLE-VR-LINK] 10s: %s\n", line);
}

static void PingThreadProc(void* context) {
    uint64_t lastHelloMs = PltGetMillis();
    uint64_t lastStatsMs = lastHelloMs;
    int helloTries = 1;
    (void)context;

    while (!PltIsThreadInterrupted(&pingThread)) {
        uint64_t nowMs = PltGetMillis();
        int i;

        if (!helloAcked && helloTries < VRML_HELLO_MAX_TRIES && nowMs - lastHelloMs >= VRML_HELLO_RETRY_MS) {
            Limelog("[VIPLE-VR-LINK] no LINK_READY yet; sending LINK_HELLO again (%d)\n", helloTries + 1);
            sendHello();
            lastHelloMs = nowMs;
            helloTries++;
        }

        for (i = 0; i < linkCount; i++) {
            VRML_LINK* L = &links[i];
            struct {
                VIPLE_VR_LINK_HDR hdr;
                VIPLE_VR_LINK_PING ping;
            } pkt;
            uint32_t gapUs;

            if (!L->ready) {
                continue;
            }
            memset(&pkt, 0, sizeof(pkt));
            pkt.hdr.magic[0] = VIPLE_VR_LINK_MAGIC0;
            pkt.hdr.magic[1] = VIPLE_VR_LINK_MAGIC1;
            pkt.hdr.type = VIPLE_VR_LINK_T_PING;
            pkt.hdr.linkId = L->id;
            pkt.hdr.seq = LE32(++L->pingSeq);
            pkt.hdr.arg = LE32((uint32_t)PltGetMicroseconds());  // PONG 原樣帶回，用來算往返時間
            pkt.ping.flags = isConfirmed(L, nowMs) ? VIPLE_VR_LINK_PING_F_CONFIRMED : 0;
            pkt.ping.rttMs10 = LE16(L->rttMs10);
            pkt.ping.rxPkts = LE32(L->rxPkts);
            pkt.ping.rxUsed = LE32(L->rxUsed);
            gapUs = L->gapMaxUsPing;
            L->gapMaxUsPing = 0;
            pkt.ping.maxGapMs = LE16((uint16_t)(gapUs / 1000 > 0xFFFF ? 0xFFFF : gapUs / 1000));
            pkt.ping.burstSeq = L->burstSeq;
            pkt.ping.burstMbps = LE16(L->burstMbps);
            // 送不出去（網卡忙或斷線）就算了，下一輪再送
            sendto(L->videoSock, (char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&L->serverV, sizeof(L->serverV));
            sendto(L->audioSock, (char*)&pkt.hdr, sizeof(pkt.hdr), 0, (struct sockaddr*)&L->serverA, sizeof(L->serverA));
        }

        if (nowMs - lastStatsMs >= VRML_STATS_INTERVAL_MS) {
            logStats(nowMs);
            lastStatsMs = nowMs;
        }

        PltSleepMsInterruptible(&pingThread, VRML_PING_INTERVAL_MS);
    }
}

void vrmlStart(void) {
    if (linkCount == 0 || started) {
        return;
    }
    if (!trkMutexCreated) {
        if (PltCreateMutex(&trkMutex) != 0) {
            return;
        }
        trkMutexCreated = true;
    }
    PltLockMutex(&trkMutex);
    trkCrypto = PltCreateCryptoContext();
    PltUnlockMutex(&trkMutex);
    s2cCrypto = PltCreateCryptoContext();
    sendHello();
    if (PltCreateThread("VrLinkPing", PingThreadProc, NULL, &pingThread) != 0) {
        PltLockMutex(&trkMutex);
        PltDestroyCryptoContext(trkCrypto);
        trkCrypto = NULL;
        PltUnlockMutex(&trkMutex);
        return;
    }
    started = true;
}

void vrmlStop(void) {
    int i;

    if (started) {
        PltInterruptThread(&pingThread);
        PltJoinThread(&pingThread);
        // 追蹤執行緒這時可能還在送：拿到鎖之後才釋放加密 context，之後的 vrmlSendTracking 看到 linkCount==0 直接返回
        PltLockMutex(&trkMutex);
        started = false;
        if (trkCrypto != NULL) {
            PltDestroyCryptoContext(trkCrypto);
            trkCrypto = NULL;
        }
        PltUnlockMutex(&trkMutex);
    }
    for (i = 0; i < linkCount; i++) {
        closeLinkSockets(&links[i]);
    }
    linkCount = 0;
    hubCaps = 0;
    if (s2cCrypto != NULL) {
        // 影像接收執行緒已經結束（呼叫端保證），沒有人在用了
        PltDestroyCryptoContext(s2cCrypto);
        s2cCrypto = NULL;
    }
    free(stageBuf);
    stageBuf = NULL;
    stageSlotSize = 0;
    stageCount = 0;
    stageNext = 0;
}

void vrmlOnS2C(const unsigned char* tlv, int len) {
    int off = 0;

    if (linkCount == 0) {
        return;
    }
    while (off + 2 <= len) {
        uint8_t type = tlv[off];
        uint8_t blen = tlv[off + 1];
        const unsigned char* body = tlv + off + 2;
        if (off + 2 + blen > len) {
            break;
        }
        if (type == VIPLE_VR_S2C_LINK_READY && blen >= 1) {
            int count = body[0];
            int i, k;
            for (i = 0; i < count && 1 + (i + 1) * (int)sizeof(VIPLE_VR_LINK_PORTS) <= blen; i++) {
                VIPLE_VR_LINK_PORTS p;
                memcpy(&p, body + 1 + i * sizeof(p), sizeof(p));
                for (k = 0; k < linkCount; k++) {
                    VRML_LINK* L = &links[k];
                    if (L->id != p.linkId) {
                        continue;
                    }
                    if (p.status == VIPLE_VR_LINK_READY_OK && p.serverVideoPort != 0 && p.serverAudioPort != 0) {
                        L->serverV.sin_port = htons(LE16(p.serverVideoPort));
                        L->serverA.sin_port = htons(LE16(p.serverAudioPort));
                        if (!L->ready) {
                            Limelog("[VIPLE-VR-LINK] link %d ready (server video:%u audio:%u)\n", (int)L->id,
                                    (unsigned)LE16(p.serverVideoPort), (unsigned)LE16(p.serverAudioPort));
                        }
                        L->ready = true;
                    }
                    else if (!L->ready) {
                        if (!L->refused) {
                            Limelog("[VIPLE-VR-LINK] link %d refused by the server\n", (int)L->id);
                        }
                        L->refused = true;
                    }
                }
            }
            {
                // §VR-LINK-CTRL：陣列之後多一個 byte＝server 同意啟用的功能（舊 server 不附）
                int used = 1 + count * (int)sizeof(VIPLE_VR_LINK_PORTS);
                uint8_t caps = (count >= 0 && used < blen) ? body[used] : 0;
                if (caps != hubCaps) {
                    Limelog("[VIPLE-VR-LINK] link features agreed by the server: ctrl=%s repair=%s\n",
                            (caps & VIPLE_VR_LINK_F_CTRL) ? "on" : "off", (caps & VIPLE_VR_LINK_F_REPAIR) ? "on" : "off");
                }
                hubCaps = caps;
            }
            helloAcked = true;
        }
        off += 2 + blen;
    }
}

// ── 接收 ──────────────────────────────────────────────────────────

static void initRxSet(VRML_RXSET* set, SOCKET legacy, bool video) {
    int i;

    memset(set, 0, sizeof(*set));
    set->socks[0] = legacy;
    set->link[0] = -1;
    set->count = 1;
    for (i = 0; i < linkCount; i++) {
        set->socks[set->count] = video ? links[i].videoSock : links[i].audioSock;
        set->link[set->count] = (int8_t)i;
        set->count++;
    }
    // 原本的 socket 也改成非阻塞（一次只等一個 socket 的 SO_RCVTIMEO 不適用了）
    setSocketNonBlocking(legacy, true);
}

static bool wouldBlock(int err) {
    return err == EWOULDBLOCK || err == EAGAIN || err == EINTR
#if defined(LC_WINDOWS)
           || err == WSA_IO_PENDING
#endif
        ;
}

static bool icmpNoise(int err) {
    // 之前送出的封包換來 ICMP port unreachable：忽略，繼續收
#if defined(LC_WINDOWS)
    return err == WSAECONNRESET;
#else
    return err == ECONNREFUSED;
#endif
}

// recvfrom，另外取核心的接收時戳（有的話）
static int recvWithTs(SOCKET s, char* buffer, int size, uint64_t* tsUs) {
    *tsUs = 0;
#if defined(__linux__) && defined(SO_TIMESTAMPNS)
    {
        struct msghdr msg;
        struct iovec iov;
        char ctrl[64];
        struct cmsghdr* cm;
        int n;

        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buffer;
        iov.iov_len = (size_t)size;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);
        n = (int)recvmsg(s, &msg, 0);
        if (n > 0) {
            for (cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPNS) {
                    struct timespec ts;
                    memcpy(&ts, CMSG_DATA(cm), sizeof(ts));
                    *tsUs = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
                }
            }
        }
        return n;
    }
#else
    return (int)recvfrom(s, buffer, size, 0, NULL, NULL);
#endif
}

static int pollRxSet(VRML_RXSET* set, int timeoutMs) {
    struct pollfd pfds[VIPLE_VR_LINK_MAX + 1];
    int map[VIPLE_VR_LINK_MAX + 1];
    int n = 0;
    int i, err;

    for (i = 0; i < set->count; i++) {
        if (set->dead[i]) {
            continue;
        }
        pfds[n].fd = set->socks[i];
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        map[n] = i;
        n++;
    }
    err = pollSockets(pfds, n, timeoutMs);
    if (err <= 0) {
        return err;
    }
    for (i = 0; i < n; i++) {
        if (pfds[i].revents != 0) {
            set->readable[map[i]] = true;
        }
    }
    return err;
}

// 回傳 > 0＝資料長度（*slot 是 set 裡的位置）、0＝逾時（wait=false 時是「現在沒有資料」）、< 0＝原本的 socket 出錯
static int recvRxSet(VRML_RXSET* set, char* buffer, int size, int* slot, bool wait) {
    bool polledNoWait = false;

    for (;;) {
        int n, err;
        bool any = false;

        if (set->sincePoll >= VRML_REPOLL_EVERY) {
            set->sincePoll = 0;
            pollRxSet(set, 0);
        }

        for (n = 0; n < set->count; n++) {
            int i = (set->next + n) % set->count;
            if (!set->readable[i] || set->dead[i]) {
                continue;
            }
            any = true;
            err = recvWithTs(set->socks[i], buffer, size, &set->lastTsUs);
            if (err > 0) {
                set->next = (i + 1) % set->count;  // 輪流，兩條連線同時有資料時不偏袒任何一條
                set->sincePoll++;
                *slot = i;
                return err;
            }
            if (err == 0) {
                continue;  // 空資料報：當沒收到
            }
            err = LastSocketError();
            if (wouldBlock(err)) {
                set->readable[i] = false;
            }
            else if (icmpNoise(err)) {
                // 留著 readable，下一輪再讀
            }
            else if (set->link[i] < 0) {
                SetLastSocketError(err);
                return -1;  // 原本的 socket 出錯：和單一路徑時一樣交給呼叫端處理
            }
            else {
                Limelog("[VIPLE-VR-LINK] link %d socket error %d - dropping this link's receive side\n", (int)links[set->link[i]].id, err);
                set->dead[i] = true;
            }
        }
        if (any) {
            continue;
        }

        set->sincePoll = 0;
        if (!wait) {
            // 不等：再看一次有沒有剛到的，沒有就返回
            if (polledNoWait || pollRxSet(set, 0) <= 0) {
                return 0;
            }
            polledNoWait = true;
            continue;
        }
        err = pollRxSet(set, set == &videoSet ? videoPollMs : UDP_RECV_POLL_TIMEOUT_MS);
        if (err < 0) {
            if (wouldBlock(LastSocketError())) {
                continue;
            }
            return err;
        }
        if (err == 0) {
            return 0;
        }
    }
}

// T_S2C 的去重視窗（64 則）：同一則訊息在每條連線各來一份，seq 相同；也擋重放
static bool s2cFresh(uint32_t seq) {
    int32_t d;
    if (!s2cAny) {
        return true;
    }
    d = (int32_t)(seq - s2cTop);
    if (d > 0) {
        return true;
    }
    if (d <= -64) {
        return false;
    }
    return (s2cMask & (1ull << (-d))) == 0;
}

static void s2cMark(uint32_t seq) {
    int32_t d;
    if (!s2cAny) {
        s2cAny = true;
        s2cTop = seq;
        s2cMask = 1;
        return;
    }
    d = (int32_t)(seq - s2cTop);
    if (d > 0) {
        s2cMask = d >= 64 ? 0 : (s2cMask << d);
        s2cMask |= 1;
        s2cTop = seq;
    }
    else if (d > -64) {
        s2cMask |= 1ull << (-d);
    }
}

// §VR-LINK-CTRL：server 經連線送來的 0x5508 TLV（加密；每條連線各一份）
static void onLinkS2C(const VIPLE_VR_LINK_HDR* hdr, const char* buffer, int len) {
    unsigned char plain[VIPLE_VR_LINK_CTRL_MAX_LEN];
    unsigned char iv[12];
    uint32_t seq = LE32(hdr->seq);
    int cipherLen = len - (int)sizeof(*hdr) - 16;
    int plainLen = 0;

    // 至少要有 ptype（2）＋一個 TLV 標頭（2）；沒協商這個功能就不收
    if (cipherLen < 4 || cipherLen > (int)sizeof(plain) || s2cCrypto == NULL || !(hubCaps & VIPLE_VR_LINK_F_CTRL)) {
        s2cBad++;
        return;
    }
    if (!s2cFresh(seq)) {
        s2cDup++;  // 另一條連線已經送到
        return;
    }
    memset(iv, 0, sizeof(iv));
    iv[0] = (unsigned char)(seq);
    iv[1] = (unsigned char)(seq >> 8);
    iv[2] = (unsigned char)(seq >> 16);
    iv[3] = (unsigned char)(seq >> 24);
    iv[10] = (unsigned char)'H';
    iv[11] = (unsigned char)'L';
    if (!PltDecryptMessage(s2cCrypto, ALGORITHM_AES_GCM, 0,
                           (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                           iv, sizeof(iv),
                           (unsigned char*)buffer + sizeof(*hdr), 16,
                           (unsigned char*)buffer + sizeof(*hdr) + 16, cipherLen,
                           plain, &plainLen) || plainLen < 4) {
        s2cBad++;
        return;
    }
    s2cMark(seq);  // 驗證通過才記（解密前不動任何狀態）
    if ((uint16_t)(plain[0] | (plain[1] << 8)) != VIPLE_VR_PTYPE_S2C) {
        return;
    }
    s2cRx++;
    {
        // §VR-LINK-REPAIR：REPAIR_GONE 留給影像佇列（同一條執行緒，回到 VideoStream 之後由 vrmlTakeGone 取走）
        int off = 2;
        while (off + 2 <= plainLen) {
            uint8_t type = plain[off];
            uint8_t blen = plain[off + 1];
            if (off + 2 + blen > plainLen) {
                break;
            }
            if (type == VIPLE_VR_S2C_REPAIR_GONE && blen >= sizeof(VIPLE_VR_TLV_REPAIR_GONE)) {
                VIPLE_VR_TLV_REPAIR_GONE g;
                memcpy(&g, &plain[off + 2], sizeof(g));
                goneFrame = g.frame;
                goneBlock = g.block;
                gonePending = true;
            }
            off += 2 + blen;
        }
    }
    vrLinkS2C(plain + 2, plainLen - 2);
}

static void onVideoCtrl(VRML_LINK* L, const char* buffer, int len) {
    VIPLE_VR_LINK_HDR hdr;

    if (len < (int)sizeof(hdr)) {
        return;
    }
    memcpy(&hdr, buffer, sizeof(hdr));
    if (hdr.magic[0] != VIPLE_VR_LINK_MAGIC0 || hdr.magic[1] != VIPLE_VR_LINK_MAGIC1 || hdr.linkId != L->id) {
        return;
    }
    if (hdr.type == VIPLE_VR_LINK_T_PONG) {
        uint32_t rttUs = (uint32_t)PltGetMicroseconds() - LE32(hdr.arg);
        if (rttUs < 2000000) {
            L->rttMs10 = (uint16_t)(rttUs / 100 >= VIPLE_VR_LINK_RTT_UNKNOWN ? VIPLE_VR_LINK_RTT_UNKNOWN - 1 : rttUs / 100);
        }
        if (L->lastPongMs == 0) {
            Limelog("[VIPLE-VR-LINK] link %d confirmed (first PONG, rtt %.1f ms)\n", (int)L->id, rttUs / 1000.0);
        }
        L->lastPongMs = PltGetMillis();
    }
    else if (hdr.type == VIPLE_VR_LINK_T_S2C) {
        onLinkS2C(&hdr, buffer, len);
    }
}

// 一幀在這條連線結束了（下一幀的封包到了，或超過 VRML_RUN_IDLE_US 沒有下一個）：夠多就算出到達速率
static void finishRun(VRML_LINK* L) {
    // 核心時戳是牆上時鐘：被校時往回撥的那一幀不算
    if (L->runCount >= VRML_RUN_MIN_PKTS && L->runLastTsUs >= L->runStartTsUs) {
        uint64_t durUs = L->runLastTsUs - L->runStartTsUs;
        if (durUs >= VRML_RUN_MIN_DUR_US) {
            // 第一個封包的長度不算（量的是「之後這些位元組花了多久到」）
            uint64_t bits = (uint64_t)(L->runBytes - L->runBytes / L->runCount) * 8;
            uint64_t mbps = bits / durUs;
            L->burstMbps = (uint16_t)(mbps > 65534 ? 65534 : (mbps == 0 ? 1 : mbps));
        }
        else {
            // 整幀（至少 32 個封包）同一瞬間交上來＝一個聚合框就送完：鏈路夠快，但量不出數字。
            // 一定要回報：不回報的話 server 等不到新的量測，這條連線會一直停在探測
            L->burstMbps = 65535;
        }
        L->burstSeq++;
    }
    L->runCount = 0;
    L->runBytes = 0;
}

static void finishIdleRuns(void) {
    uint64_t nowUs = PltGetMicroseconds();
    int i;
    for (i = 0; i < linkCount; i++) {
        if (links[i].runCount > 0 && nowUs - links[i].runLastUs > VRML_RUN_IDLE_US) {
            finishRun(&links[i]);
        }
    }
}

// 統計視窗結束：收到的影像封包數達到最多那條的四分之一，才算「在送影像」（探測中的連線每秒一幀，遠低於這個比例）
static void rollCarryWindow(uint64_t nowUs) {
    uint32_t most = 0;
    uint8_t mask = 0;
    int carrying = 0;
    int i;
    for (i = 0; i < linkCount; i++) {
        if (links[i].winPkts > most) {
            most = links[i].winPkts;
        }
    }
    for (i = 0; i < linkCount; i++) {
        if (links[i].winPkts != 0 && links[i].winPkts * 4 >= most) {
            carrying++;
            mask |= (uint8_t)(1u << i);
        }
        links[i].winPkts = 0;
    }
    if (carrying >= 2) {
        carryKeep = VRML_CARRY_KEEP_WINDOWS;
        carryMask = mask;
    }
    else if (carryKeep > 0 && --carryKeep > 0) {
        carrying = 2;  // 遲滯：一條連線短暫停頓不算「只剩一條」（carryMask 留著上一次的）
    }
    else {
        carryMask = mask;
    }
    carryLinks = carrying;
    carryWinStartUs = nowUs;
}

int vrmlVideoCarrying(void) {
    return carryLinks;
}

// §VR-LINK-GRACE：有沒有任何一條連線「已確認」（最近 VRML_CONFIRM_TIMEOUT_MS 內收過 PONG）
int vrmlAnyAlive(void) {
    const uint64_t nowMs = PltGetMillis();
    for (int i = 0; i < linkCount; i++) {
        if (isConfirmed(&links[i], nowMs)) {
            return 1;
        }
    }
    return 0;
}

int vrmlVideoCarryMask(void) {
    return carryMask;
}

uint32_t vrmlVideoLinkLagUs(int linkIdx) {
    return linkIdx >= 0 && linkIdx < linkCount ? links[linkIdx].lagUs : 0;
}

// 一個新樣本。估計值往「最近 VRML_LAG_HIST 個樣本的第二大」靠：比它小就立刻升上去，比它大就每次降十六分之一。
// 取第二大：單一離群值（例如這條連線把某一幀整批掉了，補包成了它那一幀的第一個封包）不採用；兩條輪流領先、
// 樣本在 0 與實際的時間差之間交替時仍然跟得上。
static void lagSample(VRML_LINK* L, uint32_t sample) {
    uint32_t top = 0, second = 0, target;
    int i;

    L->lagHist[L->lagHistPos] = sample;
    L->lagHistPos = (uint8_t)((L->lagHistPos + 1) % VRML_LAG_HIST);
    if (L->lagHistCount < VRML_LAG_HIST) {
        L->lagHistCount++;
    }
    for (i = 0; i < L->lagHistCount; i++) {
        const uint32_t v = L->lagHist[i];
        if (v > top) {
            second = top;
            top = v;
        }
        else if (v > second) {
            second = v;
        }
    }
    target = L->lagHistCount >= 2 ? second : top;
    if (target >= L->lagUs) {
        L->lagUs = target;
    }
    else {
        L->lagUs -= (L->lagUs - target) >> 4;
    }
}

// 這條連線送來某一幀的第一個封包：和「最先送到這一幀的那條」差多久。最先到的那條記進 leadRing；它自己的樣本（0）
// 要等另一條也送到同一幀才算——只有它一條在送的那幾幀（另一條正好斷訊）不是「它比較快」的證據，照算的話
// 平常落後的那條會在快的那條斷訊時被量成 0，快的那條一回來就等得太短
static void noteLag(VRML_LINK* L, uint32_t frame, uint64_t arriveUs) {
    const int idx = (int)(L - links);
    int slot = (int)(frame % VRML_LEAD_RING);
    int i;

    if (leadAny && isBefore32(leadNewest, frame) && frame - leadNewest > VRML_LEAD_JUMP_MAX) {
        return;  // 幀號跳太遠：不採用（這個封包多半也過不了解密）
    }
    L->lagValid = true;
    L->lagFrame = frame;
    if (!leadAny || isBefore32(leadNewest, frame)) {
        // 這一幀第一次出現。只有正在送影像的連線能當「最先送到的」：探測中的連線每秒只送一幀，和它比沒有意義
        if (!(carryMask & (1u << idx))) {
            return;
        }
        leadAny = true;
        leadNewest = frame;
        leadStale = 0;
        leadRing[slot].valid = true;
        leadRing[slot].paired = false;
        leadRing[slot].link = (int8_t)idx;
        leadRing[slot].frame = frame;
        leadRing[slot].us = arriveUs;
    }
    else if (leadRing[slot].valid && leadRing[slot].frame == frame && arriveUs >= leadRing[slot].us &&
             arriveUs - leadRing[slot].us < 1000000) {
        const uint64_t d = arriveUs - leadRing[slot].us;
        leadStale = 0;
        lagSample(L, d > VRML_LAG_MAX_US ? VRML_LAG_MAX_US : (uint32_t)d);
        if (!leadRing[slot].paired) {
            leadRing[slot].paired = true;
            lagSample(&links[leadRing[slot].link], 0);
        }
    }
    else if (leadNewest - frame > VRML_LEAD_RING && ++leadStale >= VRML_LEAD_STALE_RESET) {
        // 一直對不上：最新幀號的紀錄多半是被一個壞封包帶歪的（第一個封包就是壞的時候沒有東西可比）。整個重來
        leadAny = false;
        leadStale = 0;
        for (i = 0; i < linkCount; i++) {
            links[i].lagValid = false;
        }
    }
    // 其餘：那一幀的紀錄已經被覆寫（落後超過 VRML_LEAD_RING 幀），或時戳對不上——不取樣
}

// ── 影像：暫存與排序 ──────────────────────────────────────────────
//
// 兩個 socket 各自是先進先出，但「先讀哪一個」會把順序打亂：連線 A 還留著第 N 幀的尾巴、連線 B 已經有第 N+1 幀
// 的開頭時，先讀到 B 就會讓佇列以為第 N 幀結束了而放棄它（S0 互補空檔實測：server 端每一批都確實送出了一份，
// client 仍然每 10 秒掉 2～3 幀）。所以每次等到有封包之後，把所有 socket 上「已經到了」的封包一次收進暫存
// （不等待，不增加延遲），依幀號（未加密時再依 RTP 序號）排好，再一個一個交給呼叫端。
// 真的晚到（另一條連線慢了幾毫秒）的封包不在這裡處理。

static bool ensureStage(int size) {
    if (stageBuf != NULL && stageSlotSize >= size) {
        return true;
    }
    free(stageBuf);
    stageBuf = (char*)malloc((size_t)VRML_STAGE_MAX * (size_t)size);
    stageSlotSize = stageBuf != NULL ? size : 0;
    return stageBuf != NULL;
}

static void stageKey(const char* pkt, int len, uint32_t* frame, uint32_t* seq) {
    const unsigned char* p = (const unsigned char*)pkt;

    *frame = 0;
    *seq = 0;
    if (EncryptionFeaturesEnabled & SS_ENC_VIDEO) {
        // ENC_VIDEO_HEADER：iv[12]、frameNumber（LE32）、tag[16]。RTP 序號在密文裡，但 IV 的前 8 byte 是 server 每送
        // 一個封包就加一的計數器（LE64），拿低 32 bit 當同一幀內的順序
        if (len >= 16) {
            *frame = (uint32_t)p[12] | ((uint32_t)p[13] << 8) | ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
            *seq = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
    }
    else {
        // RTP_PACKET（12 B，有 extension 再 4 B）之後是 NV_VIDEO_PACKET：streamPacketIndex、frameIndex（LE32）
        int off = (int)sizeof(RTP_PACKET) + ((p[0] & FLAG_EXTENSION) ? 4 : 0);
        if (len >= off + 8) {
            *frame = (uint32_t)p[off + 4] | ((uint32_t)p[off + 5] << 8) | ((uint32_t)p[off + 6] << 16) | ((uint32_t)p[off + 7] << 24);
            *seq = (uint32_t)(((uint32_t)p[2] << 8) | p[3]);
        }
    }
}

// a 是否應該排在 b 前面（幀號、序號都以有號差值比較，繞回也正確；其餘維持到達順序）
static bool stagedBefore(const VRML_STAGED* a, const VRML_STAGED* b) {
    if (a->frame != b->frame) {
        return (int32_t)(a->frame - b->frame) < 0;
    }
    if (a->seq != b->seq) {
        if (EncryptionFeaturesEnabled & SS_ENC_VIDEO) {
            return (int32_t)(a->seq - b->seq) < 0;
        }
        return (int16_t)(uint16_t)(a->seq - b->seq) < 0;
    }
    return false;
}

// 把剛收進 stageBuf 第 n 格的封包登記進排序表；回傳 false＝是連線控制資料報（已處理，不佔格子）
static bool stageAccept(int n, int len, int rxSlot) {
    char* pkt = stageBuf + (size_t)n * stageSlotSize;
    int idx = videoSet.link[rxSlot];
    VRML_STAGED e;
    int k;

    if (idx < 0) {
        legacyRxPkts++;
        stageKey(pkt, len, &e.frame, &e.seq);
        if (linkCount > 0) {
            // 影像退回原本的 socket 時（所有連線都不通）也要結算，否則「幾條在送」會停在最後一次的值
            uint64_t nowUs = PltGetMicroseconds();
            if (nowUs - carryWinStartUs >= VRML_CARRY_WINDOW_US) {
                rollCarryWindow(nowUs);
            }
        }
    }
    else {
        VRML_LINK* L = &links[idx];
        uint64_t nowUs;

        // 影像封包一定比這個長；短的是連線控制（PONG）
        if (len < VIPLE_VR_LINK_CTRL_MAX_LEN) {
            onVideoCtrl(L, pkt, len);
            return false;
        }
        stageKey(pkt, len, &e.frame, &e.seq);
        nowUs = PltGetMicroseconds();
        if (L->lastRxUs != 0) {
            uint64_t gap = nowUs - L->lastRxUs;
            uint32_t g = gap > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)gap;
            if (g > L->gapMaxUsPing) {
                L->gapMaxUsPing = g;
            }
            if (g > L->gapMaxUsStats) {
                L->gapMaxUsStats = g;
            }
        }
        L->lastRxUs = nowUs;
        L->rxPkts++;
        L->winPkts++;
        if (nowUs - carryWinStartUs >= VRML_CARRY_WINDOW_US) {
            rollCarryWindow(nowUs);
        }

        {
            uint64_t arriveUs = videoSet.lastTsUs != 0 ? videoSet.lastTsUs : nowUs;
            // §VR-LINK-REPAIR：這條連線每送來新的一幀就取一次「比最先送到的那條晚多久」（補包是舊幀，不取）
            if (e.frame != 0 && (!L->lagValid || isBefore32(L->lagFrame, e.frame))) {
                noteLag(L, e.frame, arriveUs);
            }
            // §VR-LINK-REPAIR：補回來的是比較舊的幀的封包，夾在目前這一幀中間到——不算進量測，也不拿它來切段
            if (!(L->runCount > 0 && isBefore32(e.frame, L->runFrame))) {
                // 換幀就結算上一幀。時戳往回跳（校時、或這個封包沒有核心時戳）或隔了很久也當成新的一次
                if (L->runCount > 0 && (e.frame != L->runFrame || arriveUs < L->runLastTsUs ||
                                        arriveUs - L->runLastTsUs > VRML_RUN_IDLE_US)) {
                    finishRun(L);
                }
                if (L->runCount == 0) {
                    L->runStartTsUs = arriveUs;
                    L->runFrame = e.frame;
                }
                L->runCount++;
                L->runBytes += (uint32_t)len;
                L->runLastTsUs = arriveUs;
                L->runLastUs = nowUs;
            }
        }
    }

    e.slot = (uint16_t)n;
    e.len = (uint16_t)len;
    e.link = (int8_t)idx;
    // 插入排序：資料本來就大致有序，平均只比較一兩次
    for (k = stageCount; k > 0 && stagedBefore(&e, &stageIdx[k - 1]); k--) {
        stageIdx[k] = stageIdx[k - 1];
    }
    stageIdx[k] = e;
    stageCount++;
    return true;
}

int vrmlRecvVideo(SOCKET legacy, char* buffer, int size, int* linkIdx, int timeoutMs) {
    uint64_t deadlineUs = 0;  // 這一次最晚等到什麼時候（開始等之前才算，手上有暫存封包可交時不必讀時鐘）

    if (videoSet.count == 0) {
        initRxSet(&videoSet, legacy, true);
    }
    videoPollMs = timeoutMs > 0 && timeoutMs < UDP_RECV_POLL_TIMEOUT_MS ? timeoutMs : UDP_RECV_POLL_TIMEOUT_MS;
    if (!ensureStage(size)) {
        // 記憶體不足：退回不排序的收法
        int slot = 0;
        int err = recvRxSet(&videoSet, buffer, size, &slot, true);
        *linkIdx = err > 0 ? videoSet.link[slot] : -1;
        return err;
    }

    for (;;) {
        int used = 0;  // stageBuf 已使用的格數（控制資料報不佔格子）
        bool drainOnly = false;  // 已經決定要返回了（GONE 要交出去、或等到期了）：只把各 socket 上已經到的收完，不再等

        if (stageNext < stageCount) {
            const VRML_STAGED* e = &stageIdx[stageNext++];
            memcpy(buffer, stageBuf + (size_t)e->slot * stageSlotSize, e->len);
            *linkIdx = e->link;
            return e->len;
        }

        // 暫存用完了：等到有封包，再把已經到的全部收進來
        stageCount = 0;
        stageNext = 0;
        finishIdleRuns();
        while (used < VRML_STAGE_MAX) {
            const bool wait = stageCount == 0 && !drainOnly;
            int slot = 0;
            int err;

            if (wait && deadlineUs == 0) {
                deadlineUs = PltGetMicroseconds() + (uint64_t)videoPollMs * 1000;
            }
            err = recvRxSet(&videoSet, stageBuf + (size_t)used * stageSlotSize, stageSlotSize, &slot, wait);
            if (err < 0) {
                if (stageCount > 0) {
                    break;  // 先把手上的交出去，錯誤下一輪再回報
                }
                *linkIdx = -1;
                return err;
            }
            if (err == 0) {
                if (stageCount > 0) {
                    break;
                }
                finishIdleRuns();
                *linkIdx = -1;
                return 0;  // 逾時
            }
            if (stageAccept(used, err, slot)) {
                used++;
            }
            else if (stageCount == 0 && !drainOnly) {
                // 連線控制資料報（PONG、T_S2C）不是影像。REPAIR_GONE 要盡快交給呼叫端（它返回後就會取走）；其餘的
                // 不能讓這一次的等待從頭算起——每條連線每秒 4 個 PONG，佇列等待的期限會一再被往後推。
                // 兩種情況都不直接返回：先把其他 socket 上已經到的收進來（不等待）。GONE 和另一條連線上的補包同時到的
                // 時候，先讀到哪一個只是輪流順序的巧合——補包要排在 GONE 前面處理（vrmlTakeGone 會等這一批交完）
                if (gonePending) {
                    drainOnly = true;
                }
                else {
                    const uint64_t nowUs = PltGetMicroseconds();
                    if (nowUs >= deadlineUs) {
                        drainOnly = true;
                    }
                    else {
                        videoPollMs = (int)((deadlineUs - nowUs + 999) / 1000);
                    }
                }
            }
        }
    }
}

void vrmlNoteVideoUsed(int linkIdx) {
    if (linkIdx >= 0 && linkIdx < linkCount) {
        links[linkIdx].rxUsed++;
    }
}

int vrmlRecvAudio(SOCKET legacy, char* buffer, int size) {
    int slot = 0;

    if (audioSet.count == 0) {
        initRxSet(&audioSet, legacy, false);
    }
    return recvRxSet(&audioSet, buffer, size, &slot, true);
}

// ── 追蹤 ──────────────────────────────────────────────────────────

// 把一則控制訊息加密後在每條已確認的連線各送一份（seq 相同，server 以它去重並防重放）。回傳送出的份數。
static int sendC2S(uint16_t ptype, const void* sample, int length, bool tracking) {
    // 標頭 12＋tag 16＋ptype 2＋payload
    unsigned char pkt[sizeof(VIPLE_VR_LINK_HDR) + 16 + 2 + VIPLE_VR_MAX_CTRL_PAYLOAD];
    unsigned char plain[2 + VIPLE_VR_MAX_CTRL_PAYLOAD];
    unsigned char iv[12];
    VIPLE_VR_LINK_HDR hdr;
    uint64_t nowMs;
    uint32_t seq;
    int cipherLen = 0;
    int sent = 0;
    int i;
    bool anyConfirmed = false;

    if (linkCount == 0 || !started || length <= 0 || length > VIPLE_VR_MAX_CTRL_PAYLOAD) {
        return 0;
    }
    nowMs = PltGetMillis();
    for (i = 0; i < linkCount; i++) {
        anyConfirmed = anyConfirmed || isConfirmed(&links[i], nowMs);
    }
    if (!anyConfirmed) {
        return 0;
    }

    PltLockMutex(&trkMutex);
    if (!started || trkCrypto == NULL) {
        PltUnlockMutex(&trkMutex);
        return 0;
    }
    seq = ++dataSeq;

    plain[0] = (unsigned char)(ptype & 0xFF);
    plain[1] = (unsigned char)(ptype >> 8);
    memcpy(&plain[2], sample, (size_t)length);

    // IV＝seq（LE32）＋6 個 0＋'C'＋'L'：和控制通道（'C','C'）、影像（'V'）的 IV 不會重複
    memset(iv, 0, sizeof(iv));
    iv[0] = (unsigned char)(seq);
    iv[1] = (unsigned char)(seq >> 8);
    iv[2] = (unsigned char)(seq >> 16);
    iv[3] = (unsigned char)(seq >> 24);
    iv[10] = (unsigned char)'C';
    iv[11] = (unsigned char)'L';

    if (!PltEncryptMessage(trkCrypto, ALGORITHM_AES_GCM, 0,
                           (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                           iv, sizeof(iv),
                           &pkt[sizeof(hdr)], 16,
                           plain, 2 + length,
                           &pkt[sizeof(hdr) + 16], &cipherLen)) {
        PltUnlockMutex(&trkMutex);
        return 0;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = VIPLE_VR_LINK_MAGIC0;
    hdr.magic[1] = VIPLE_VR_LINK_MAGIC1;
    hdr.type = VIPLE_VR_LINK_T_DATA;
    hdr.seq = LE32(seq);
    for (i = 0; i < linkCount; i++) {
        VRML_LINK* L = &links[i];
        if (!isConfirmed(L, nowMs)) {
            continue;
        }
        hdr.linkId = L->id;
        memcpy(pkt, &hdr, sizeof(hdr));
        // 非阻塞：這條連線的送出佇列滿了就丟這一份（另一條還有一份）
        if (sendto(L->videoSock, (char*)pkt, (int)sizeof(hdr) + 16 + cipherLen, 0, (struct sockaddr*)&L->serverV, sizeof(L->serverV)) > 0) {
            if (tracking) {
                L->trkSent++;
            }
            sent++;
        }
        else if (tracking) {
            L->trkDrop++;
        }
    }
    if (!tracking) {
        if (sent > 0) {
            ctlSent++;
        }
        else {
            ctlDrop++;
        }
    }
    PltUnlockMutex(&trkMutex);
    return sent;
}

int vrmlSendTracking(const void* sample, int length) {
    return sendC2S(VIPLE_VR_PTYPE_TRACKING, sample, length, true);
}

int vrmlSendCtrl(const unsigned char* tlv, int length) {
    if (!(hubCaps & VIPLE_VR_LINK_F_CTRL)) {
        return 0;
    }
    return sendC2S(VIPLE_VR_PTYPE_C2S, tlv, length, false);
}

int vrmlCtrlRttMs(void) {
    uint64_t nowMs;
    uint32_t best = VIPLE_VR_LINK_RTT_UNKNOWN;
    int i;

    if (!(hubCaps & VIPLE_VR_LINK_F_CTRL) || linkCount == 0) {
        return -1;
    }
    nowMs = PltGetMillis();
    for (i = 0; i < linkCount; i++) {
        uint16_t rtt = links[i].rttMs10;
        if (isConfirmed(&links[i], nowMs) && rtt < best) {
            best = rtt;
        }
    }
    return best == VIPLE_VR_LINK_RTT_UNKNOWN ? -1 : (int)((best + 9) / 10);
}

int vrmlFeatures(void) {
    return hubCaps;
}

bool vrmlTakeGone(uint32_t* frame, uint8_t* block) {
    // 和它同一批收進來的影像封包還沒全部交出去時先不給：GONE 是在暫存階段就解出來的，但同一批裡排在後面交出去
    // 的補包可能就讓那個 block 收齊了；先套用 GONE 會放棄一個其實救得回來的 block
    if (!gonePending || stageNext < stageCount) {
        return false;
    }
    gonePending = false;
    *frame = goneFrame;
    *block = goneBlock;
    return true;
}

#else // !VIPLE_MPQUIC

int vrmlPrepare(void) { return 0; }
void vrmlStart(void) {}
void vrmlStop(void) {}
int vrmlLinkCount(void) { return 0; }
void vrmlOnS2C(const unsigned char* tlv, int len) { (void)tlv; (void)len; }
int vrmlRecvVideo(SOCKET legacy, char* buffer, int size, int* linkIdx, int timeoutMs) { (void)legacy; (void)buffer; (void)size; (void)timeoutMs; *linkIdx = -1; return -1; }
void vrmlNoteVideoUsed(int linkIdx) { (void)linkIdx; }
int vrmlVideoCarrying(void) { return 0; }
int vrmlAnyAlive(void) { return 0; }
int vrmlVideoCarryMask(void) { return 0; }
uint32_t vrmlVideoLinkLagUs(int linkIdx) { (void)linkIdx; return 0; }
int vrmlRecvAudio(SOCKET legacy, char* buffer, int size) { (void)legacy; (void)buffer; (void)size; return -1; }
int vrmlSendTracking(const void* sample, int length) { (void)sample; (void)length; return 0; }
int vrmlSendCtrl(const unsigned char* tlv, int length) { (void)tlv; (void)length; return 0; }
int vrmlCtrlRttMs(void) { return -1; }
int vrmlFeatures(void) { return 0; }
bool vrmlTakeGone(uint32_t* frame, uint8_t* block) { (void)frame; (void)block; return false; }

#endif
