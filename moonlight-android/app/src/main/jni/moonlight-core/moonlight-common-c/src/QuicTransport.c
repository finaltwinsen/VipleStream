#include "Limelight-internal.h"

#ifdef VIPLE_MPQUIC

#include "QuicTransport.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <picoquic.h>
#include <picoquic_internal.h>
#include <picoquic_utils.h>
#include <picosocks.h>
// Congestion algorithm pointers live in their own headers in picoquic.
#include <picoquic_bbr.h>
#include <picoquic_cubic.h>
#include <picoquic_newreno.h>

#ifdef LC_WINDOWS
#include <icmpapi.h>
// iphlpapi.lib 已在 PlatformNetIf.c 連結
#endif

// ── Internal constants ──────────────────────────────────────

#define PROBE_TIMEOUT_THRESHOLD  2     // consecutive stall checks → inactive
                                      // (2 × 500ms health check = 1s after first stall;
                                      //  first stall at stallThreshold ≥ 3s → failover
                                      //  triggers at ~4s, well before ENet 10s timeout)
#define HEALTH_CHECK_INTERVAL_US 500000 // 500ms between health checks
#define STATS_UPDATE_INTERVAL_US 100000 // 100ms between stats refresh
#define PATH_RECHECK_INTERVAL_US 30000000 // 30s between interface re-enumeration
#define ICMP_PROBE_TIMEOUT_MS    200      // ICMP probe timeout for reachability check
#define RECOVERY_POLL_INTERVAL_S 10       // §Q.path-recovery: seconds between recovery checks
#define RECOVERY_INITIAL_DELAY_S 30       // §Q.path-recovery: let Phase B finish before first check

// §Q-MP-STANDBY: 延遲路徑降級閾值。
// 如果新路徑的 ICMP RTT（Phase B 階段測得）超過 path 0 的 RTT 加
// 此閾值，path_available callback 不會把它升級為 available，而是
// 維持 backup 作為 failover 備用。這防止 server 的 multipath
// scheduler 把 video datagram 分到高延遲路徑上，造成 depacketizer
// 因延遲差異而組裝失敗。
// 值 10ms：LAN RTT ~1ms + 10ms = 11ms 閾值。Tailscale 同 LAN
// 通常 <5ms 能通過；跨 ISP 的 Tailscale relay 通常 >30ms 會被擋。
#define STANDBY_RTT_EXCESS_MS     10

// §Q-MP-DYN-STANDBY: 動態 RTT 監測閾值（毫秒）。
// 推導：jitter buffer timeout = QUIC_JITTER_MAX_WAIT_US（10ms）。
// 若跨路徑 RTT 差超過 timeout / 2（5ms），WiFi OWD 在抖動時很容易
// 突破 timeout，導致持續 timedOut → drop。
#define DYN_STANDBY_RTT_EXCESS_MS  5

// ── Internal state ──────────────────────────────────────────

typedef struct _QUIC_SUBFLOW {
    int id;
    int interfaceIndex;
    SOCKET sock;
    struct sockaddr_storage localAddr;
    SOCKADDR_LEN addrLen;
    bool active;
    bool picoquicDeleted;  // path permanently deleted by picoquic;
                           // eligible for re-probe on next recheck cycle
    // picoquic's per-path identifier (uint64 in picoquic API). Set when we
    // probe a new path; -1 (UINT64_MAX) before that. Note: picoquic creates
    // path 0 automatically for the initial connection, so the first subflow
    // we add via probe_new_path gets id 1.
    uint64_t picoquicPathId;

    // Which peer address this subflow was probed against (may differ from
    // the connection's primary peerAddr when alt-peer is used).
    struct sockaddr_storage peerUsed;
    SOCKADDR_LEN peerUsedLen;

    // Per-subflow stats (updated from picoquic path stats via
    // picoquic_get_path_quality)
    float rttMs;
    float throughputMbps;
    float lossPercent;
    float reorderPercent;
    // §K.19: 上一次更新的累積計數器，用於計算瞬時 loss%
    uint64_t prevSent;
    uint64_t prevLost;
    char name[LC_NETIF_MAX_NAME];
    int type;

    // Path health tracking
    int consecutiveTimeouts;
    uint64_t lastProbeTime;
    uint64_t lastRecvTime;
    uint64_t bytesSent;
    uint64_t bytesRecv;

    // §Q-MP-STANDBY: Phase B 的 ICMP RTT（毫秒）。path 0 不做 ICMP
    // 所以 icmpRttMs=0。path_available callback 比較此值和 path 0 的
    // RTT 來決定是否保持 backup。
    unsigned int icmpRttMs;
    // 若為 true，path_available 不升級此路徑（維持 backup failover）
    bool keepAsStandby;
} QUIC_SUBFLOW;

typedef struct _QUIC_TRANSPORT_CTX {
    picoquic_quic_t* quic;
    picoquic_cnx_t* cnx;

    QUIC_SUBFLOW subflows[QUIC_MAX_SUBFLOWS];
    int subflowCount;
    int nextSubflowId;

    int scheduler[4]; // index 0 = default, 1..3 = per QUIC_FLOW_*
    int aggregateRRIndex; // round-robin index for AGGREGATE

    // Per-flow receive callbacks (index = flow type: 0=unused,
    // 1=VIDEO, 2=AUDIO, 3=CONTROL). 各 flow 獨立註冊，
    // QUIC datagram 到達時根據 header 的 flowType 分派。
    QuicRecvCallback recvCallbacks[4];
    void*            recvContexts[4];

    // Failover callback
    QuicFailoverCallback failoverCallback;
    void* failoverContext;

    // Per-flow sequence counters (for datagram header)
    unsigned short seqCounters[4];

    // I/O thread
    PLT_THREAD ioThread;
    bool ioRunning;

    // Server mode
    bool isServer;
    unsigned short serverPort;

    // Health check timing
    uint64_t lastHealthCheck;
    uint64_t lastStatsUpdate;
    uint64_t lastPathRecheck; // periodic interface re-enumeration

    // Congestion control algorithm
    int congestionAlgo; // QUIC_CC_*

    // Peer address (cached from quicConnect params; picoquic doesn't expose
    // a public way to read it back after create_cnx, and the internal
    // cnx->path[0]->peer_addr is an opaque struct).
    struct sockaddr_storage peerAddr;
    SOCKADDR_LEN peerAddrLen;

    // §MP-ADV: 伺服器的所有 alt peer 位址（供動態路徑管理用）。
    struct sockaddr_storage altPeerAddrs[QUIC_MAX_ALT_PEERS];
    SOCKADDR_LEN altPeerAddrLens[QUIC_MAX_ALT_PEERS];
    int altPeerCount;

    // Path id counter for unique_path_id assignment. Picoquic path 0 is the
    // initial cnx path created by create_cnx; subsequent probes get
    // ids 1, 2, 3... in the order they're probed.
    uint64_t nextPicoquicPathId;

    // §Q-MP-FAILBACK: failover 時記錄被升級的 standby subflow slot index。
    // -1 = 沒有 failover 進行中。當原 primary（slot 0）恢復時，用此
    // index 把 failover 路徑降級回 backup。
    int failoverPromotedSlot;

    // §Q-DYN-PATH ICMP 拒絕快取。Phase B（IO 執行緒啟動前）的 ICMP 探測
    // 如果判定某介面不可達，記錄其 interfaceIndex。quicRecheckPaths 在 IO
    // 執行緒上不能做 ICMP（200ms 阻塞會造成掉幀），用這個快取提供診斷資訊。
    int icmpRejectedIfs[QUIC_MAX_SUBFLOWS];
    int icmpRejectedCount;

    // §MP-ADV-MUTEX 2026-05-24 — 保護 picoquic 狀態 + subflow 陣列。
    // picoquic 不是 thread-safe；Connection thread 的 Phase B
    // (picoquic_probe_new_path) 和 IO thread (incoming_packet /
    // prepare_next_packet) 同時存取會破壞內部狀態，導致 ACK 路由
    // 錯誤→假性 loss→畫面靜止。
    PLT_MUTEX picoquicMutex;

    // §Q.path-recovery: 背景復原執行緒。IO thread 不能做 ICMP（200ms
    // 阻塞掉幀），此執行緒每 10 秒掃描介面，對 deleted/新介面做
    // ICMP + quicAddSubflowEx（已 thread-safe via picoquicMutex）。
    PLT_THREAD recoveryThread;
    bool recoveryRunning;
} QUIC_TRANSPORT_CTX;

static QUIC_TRANSPORT_CTX g_ctx;
static bool g_initialized = false;

// §K.10 diag: per-flow 接收計數器（全域，IO thread + callback thread 共用）
// index: 0=unused, 1=VIDEO, 2=AUDIO, 3=CONTROL
static volatile uint64_t g_dgramsRecvByFlow[4] = {};
static volatile uint64_t g_bytesRecvByFlow[4] = {};
static uint64_t g_prevBytesRecvByFlow[4] = {};
static uint64_t g_lastDiagLogTime = 0;
#define DIAG_LOG_INTERVAL_US 5000000  // 5 秒

// ── §5a Jitter buffer ───────────────────────────────────────
// Sliding-window reorder buffer for multipath QUIC datagrams.
// When paths have different RTTs, datagrams arrive out of order.
// The jitter buffer holds early arrivals and delivers in seq order.
// In single-path (§MP-PRIMARY) mode, packets arrive in order and
// the fast path (seq == deliverNextSeq) adds near-zero overhead.

#define QUIC_JITTER_BUF_SIZE    256   // slots per flow (power of 2)
#define QUIC_JITTER_BUF_MASK    (QUIC_JITTER_BUF_SIZE - 1)
#define QUIC_JITTER_MAX_PKT     1500  // max datagram size
#define QUIC_JITTER_MAX_WAIT_US 10000 // 10ms reorder timeout

typedef struct _QUIC_JITTER_SLOT {
    unsigned char data[QUIC_JITTER_MAX_PKT];
    int len;            // 0 = empty
    uint64_t arrivalUs;
} QUIC_JITTER_SLOT;

typedef struct _QUIC_JITTER_BUF {
    QUIC_JITTER_SLOT slots[QUIC_JITTER_BUF_SIZE];
    uint16_t deliverNextSeq;
    int initialized;
    uint64_t lastDeliverUs;
    uint64_t delivered;
    uint64_t reordered;  // arrived ahead of expected seq
    uint64_t dropped;    // arrived after already delivered
    uint64_t timedOut;   // skipped due to timeout
} QUIC_JITTER_BUF;

static QUIC_JITTER_BUF g_jitterBufs[4]; // per flow type

// ── §5b FEC decoder (video only, RS 4+2) ────────────────────
#include "rswrapper.h"

#define QUIC_FEC_DATA_SHARDS   4
#define QUIC_FEC_PARITY_SHARDS 2
#define QUIC_FEC_TOTAL_SHARDS  6
#define QUIC_FEC_MAX_GROUPS    8
#define QUIC_FEC_SHARD_MAX     1500
#define QUIC_FEC_PARITY_FLAG   0x80
#define QUIC_FEC_META_SIZE     (QUIC_FEC_DATA_SHARDS * 2) // 4 × uint16_t BE

typedef struct _QUIC_FEC_GROUP {
    uint16_t baseSeq;
    int active;
    uint64_t createTimeUs;
    uint8_t dataPresent;   // bitmask bits 0-3
    uint8_t parityPresent; // bitmask bits 0-1
    unsigned char shardBufs[QUIC_FEC_TOTAL_SHARDS][QUIC_FEC_SHARD_MAX];
    int shardLens[QUIC_FEC_TOTAL_SHARDS];
    uint16_t originalLens[QUIC_FEC_DATA_SHARDS]; // actual lengths from parity metadata
    int maxShardLen;
    uint64_t recovered;
} QUIC_FEC_GROUP;

static QUIC_FEC_GROUP g_fecGroups[QUIC_FEC_MAX_GROUPS];
static reed_solomon* g_fecRs = NULL;
static uint64_t g_fecRecovered = 0;
static uint64_t g_fecFailed = 0;

// ── Forward declarations ────────────────────────────────────

static void quicIoThreadProc(void* context);
static void quicRecoveryThreadProc(void* context);
static int quicDgramCallback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx,
    void* stream_ctx);
static int quicSelectPath(unsigned char flowType, int dataLen);
static void quicUpdatePathStats(void);
static void quicCheckPathHealth(void);
static void quicRecheckPaths(void);
static void quicApplyCongestionAlgo(picoquic_quic_t* quic);
static void quicJitterDeliver(QUIC_TRANSPORT_CTX* ctx, QUIC_JITTER_BUF* jb, unsigned char ft);
static void quicJitterFlush(QUIC_TRANSPORT_CTX* ctx, QUIC_JITTER_BUF* jb, unsigned char ft);
static void quicJitterInsert(QUIC_TRANSPORT_CTX* ctx, unsigned char ft, uint8_t* bytes, size_t length);
static void quicFecOnShard(QUIC_TRANSPORT_CTX* ctx, unsigned char ft, uint8_t fecInfo,
    uint16_t seq, uint8_t* payload, int payloadLen);
static void quicFecTryRecover(QUIC_TRANSPORT_CTX* ctx, unsigned char ft, QUIC_FEC_GROUP* grp);

// §Q Phase 5 (5f): 0-RTT ticket store path. picoquic auto-loads/saves
// session tickets to this file across runs.  Set via
// quicSetTicketStorePath() before the first quicConnect(); empty = off.
static char s_ticketStorePath[512] = {0};

// ── Lifecycle ───────────────────────────────────────────────

int quicTransportInit(void) {
    if (g_initialized)
        return 0;

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.nextSubflowId = 1;
    g_ctx.failoverPromotedSlot = -1; // §Q-MP-FAILBACK: 無 failover 進行中

    // Default scheduler per flow type
    g_ctx.scheduler[0] = QUIC_SCHED_AUTO;
    g_ctx.scheduler[QUIC_FLOW_VIDEO] = QUIC_SCHED_ECF;
    g_ctx.scheduler[QUIC_FLOW_AUDIO] = QUIC_SCHED_REDUNDANT;
    g_ctx.scheduler[QUIC_FLOW_CONTROL] = QUIC_SCHED_MIN_RTT;

    g_ctx.congestionAlgo = QUIC_CC_BBR;

    // §K.10 diag: 重置 per-flow 計數器
    memset((void*)g_dgramsRecvByFlow, 0, sizeof(g_dgramsRecvByFlow));
    memset((void*)g_bytesRecvByFlow, 0, sizeof(g_bytesRecvByFlow));
    memset(g_prevBytesRecvByFlow, 0, sizeof(g_prevBytesRecvByFlow));
    g_lastDiagLogTime = 0;

    // §5a: reset jitter buffers
    memset(g_jitterBufs, 0, sizeof(g_jitterBufs));

    // §5b: init FEC decoder
    memset(g_fecGroups, 0, sizeof(g_fecGroups));
    g_fecRecovered = 0;
    g_fecFailed = 0;
    if (!g_fecRs) {
        g_fecRs = reed_solomon_new(QUIC_FEC_DATA_SHARDS, QUIC_FEC_PARITY_SHARDS);
    }

    g_initialized = true;
    Limelog("[VIPLE-MPQUIC] Transport subsystem initialized\n");
    return 0;
}

void quicTransportCleanup(void) {
    if (!g_initialized)
        return;

    quicDisconnect();

    if (g_ctx.quic) {
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
    }

    // Wipe transport context so a subsequent quicTransportInit() starts
    // from a clean slate (avoids stale subflowCount / seqCounters /
    // sessionTicket carrying over into the next session).
    memset(&g_ctx, 0, sizeof(g_ctx));
    memset(g_jitterBufs, 0, sizeof(g_jitterBufs));

    // §5b: release FEC decoder
    memset(g_fecGroups, 0, sizeof(g_fecGroups));
    if (g_fecRs) {
        reed_solomon_release(g_fecRs);
        g_fecRs = NULL;
    }

    g_initialized = false;
    Limelog("[VIPLE-MPQUIC] Transport subsystem cleaned up\n");
}

void quicSetTicketStorePath(const char* path) {
    if (path && *path) {
        size_t len = strlen(path);
        if (len >= sizeof(s_ticketStorePath))
            len = sizeof(s_ticketStorePath) - 1;
        memcpy(s_ticketStorePath, path, len);
        s_ticketStorePath[len] = '\0';
        Limelog("[VIPLE-MPQUIC] Ticket store: %s\n", s_ticketStorePath);
    } else {
        s_ticketStorePath[0] = '\0';
        Limelog("[VIPLE-MPQUIC] Ticket store disabled\n");
    }
}

// ── Client connection ───────────────────────────────────────

int quicConnect(const QUIC_CONNECT_PARAMS* params) {
    struct sockaddr_storage serverAddr;
    uint64_t currentTime;
    int ret;

    if (!g_initialized)
        return -1;
    if (g_ctx.cnx)
        return -1;

    currentTime = picoquic_current_time();

    // picoquic_create takes 15 args. 0-RTT support: pass a ticket_file_name
    // for picoquic to auto-load/save session tickets across runs. The actual
    // path lives in the connect params now; passing NULL disables 0-RTT
    // resumption silently (handshake still works).
    g_ctx.quic = picoquic_create(
        1,                                          // max_nb_connections
        NULL, NULL, NULL,                           // cert / key / root (client)
        "viplestream",                              // default ALPN
        quicDgramCallback, &g_ctx,                  // default callback
        NULL, NULL, NULL,                           // cnx_id_cb, reset_seed (NULL → random)
        currentTime,
        NULL,                                       // simulated_time
        s_ticketStorePath[0] ? s_ticketStorePath : NULL,  // ticket_file_name (0-RTT, §5f)
        NULL, 0                                     // ticket_encryption_key (server side only)
    );

    if (!g_ctx.quic) {
        Limelog("[VIPLE-MPQUIC] Failed to create QUIC context\n");
        return -1;
    }

    // Enable DATAGRAM frames by advertising a non-zero max_datagram_frame_size
    // transport parameter. Picoquic gates the datagram extension on this TP;
    // there is no separate "set_default_datagram_option" API.
    picoquic_set_default_tp_value(g_ctx.quic,
        picoquic_tp_max_datagram_frame_size, 65535);
    picoquic_set_default_multipath_option(g_ctx.quic, 1);
    // §Q-MP-FIX 2026-05-24: 必須啟用 path callbacks，否則
    // picoquic_callback_path_available / path_suspended / path_deleted
    // 永遠不會觸發。picoquic 預設 are_path_callbacks_enabled=0。
    picoquic_enable_path_callbacks_default(g_ctx.quic, 1);
    quicApplyCongestionAlgo(g_ctx.quic);

    // §Q-REVIEW-P4 (dev/cli-quic-linux): VipleStream-Server uses the same
    // self-signed Sunshine cert (config::nvhttp.cert) for its QUIC listener.
    // Without disabling cert verification, picotls rejects the server hello
    // and the connection silently stalls in client_init_sent.  The legacy
    // Moonlight HTTPS path already cert-pins via NvHTTP::serverCert at the
    // application layer, so trusting *any* peer cert on the QUIC layer is
    // safe here — peer auth still depends on the pinned cert in NvComputer.
    picoquic_set_null_verifier(g_ctx.quic);

    // Copy server address with negotiated QUIC port
    memcpy(&serverAddr, &params->remoteAddr, params->remoteAddrLen);
    if (serverAddr.ss_family == AF_INET) {
        ((struct sockaddr_in*)&serverAddr)->sin_port = htons(params->quicPort);
    } else {
        ((struct sockaddr_in6*)&serverAddr)->sin6_port = htons(params->quicPort);
    }

    // Cache peer addr for subsequent probe_new_path calls (picoquic has no
    // public getter for the cnx's initial peer addr).
    memcpy(&g_ctx.peerAddr, &serverAddr, params->remoteAddrLen);
    g_ctx.peerAddrLen = params->remoteAddrLen;

    g_ctx.cnx = picoquic_create_cnx(
        g_ctx.quic,
        picoquic_null_connection_id,
        picoquic_null_connection_id,
        (struct sockaddr*)&serverAddr,
        currentTime,
        0,                                          // preferred version
        params->sni,
        "viplestream",                              // ALPN
        1                                           // client mode
    );

    if (!g_ctx.cnx) {
        Limelog("[VIPLE-MPQUIC] Failed to create QUIC connection\n");
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
        return -1;
    }

    // §Q-REVIEW-P3 (dev/cli-quic-linux): picoquic_create_cnx only
    // *allocates* the connection — it does not queue the Client
    // Hello.  Without picoquic_start_client_cnx, the cnx stays in
    // picoquic_state_client_init forever and the QuicIO thread's
    // picoquic_prepare_next_packet returns sendLen=0 every time,
    // so no Initial packet ever hits the wire.  All upstream
    // sample/*.c clients call this immediately after create_cnx.
    {
        int startRet = picoquic_start_client_cnx(g_ctx.cnx);
        if (startRet != 0) {
            Limelog("[VIPLE-MPQUIC] picoquic_start_client_cnx failed: %d\n", startRet);
            picoquic_delete_cnx(g_ctx.cnx);
            g_ctx.cnx = NULL;
            picoquic_free(g_ctx.quic);
            g_ctx.quic = NULL;
            return -1;
        }
    }

    // Multipath is enabled per-quic-context via the default option set
    // above; there's no per-cnx multipath toggle in picoquic.
    // Datagram-ready is signalled in the picoquic_callback_ready path
    // (we can't mark ready until the connection has actually reached the
    // ready state).

    // Path 0 (the initial cnx path) belongs to the first subflow that we
    // bind via quicAddSubflow — we account for that in nextPicoquicPathId.
    g_ctx.nextPicoquicPathId = 0;
    g_ctx.lastHealthCheck = currentTime;
    g_ctx.lastStatsUpdate = currentTime;
    g_ctx.lastPathRecheck = currentTime;  // 避免 IO thread 立刻觸發重掃，
                                          // 讓 Connection.c Phase B 先完成

    // §MP-ADV-MUTEX: 在 IO thread 啟動前初始化 mutex
    if (PltCreateMutex(&g_ctx.picoquicMutex) != 0) {
        Limelog("[VIPLE-MPQUIC] Failed to create picoquic mutex\n");
        picoquic_delete_cnx(g_ctx.cnx);
        g_ctx.cnx = NULL;
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
        return -1;
    }

    // Start the I/O thread
    g_ctx.ioRunning = true;
    ret = PltCreateThread("QuicIO", quicIoThreadProc, &g_ctx, &g_ctx.ioThread);
    if (ret != 0) {
        Limelog("[VIPLE-MPQUIC] Failed to create I/O thread\n");
        PltDeleteMutex(&g_ctx.picoquicMutex);
        picoquic_delete_cnx(g_ctx.cnx);
        g_ctx.cnx = NULL;
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
        return -1;
    }

    // §Q.path-recovery: 啟動背景復原執行緒（ICMP 探測在此執行緒安全）
    g_ctx.recoveryRunning = true;
    ret = PltCreateThread("QuicRecovery", quicRecoveryThreadProc, &g_ctx,
                          &g_ctx.recoveryThread);
    if (ret != 0) {
        Limelog("[VIPLE-MPQUIC] §Q.path-recovery: failed to create thread "
                "(non-fatal, path recovery disabled)\n");
        g_ctx.recoveryRunning = false;
    }

    Limelog("[VIPLE-MPQUIC] Connection initiated to port %u\n", params->quicPort);
    return 0;
}

void quicDisconnect(void) {
    // §Q.path-recovery: 先停復原執行緒（它可能正在 quicAddSubflowEx
    // 裡持有 mutex，需要 cnx 存活），再停 IO thread。
    if (g_ctx.recoveryRunning) {
        g_ctx.recoveryRunning = false;
        PltJoinThread(&g_ctx.recoveryThread);
        Limelog("[VIPLE-MPQUIC] §Q.path-recovery: thread joined\n");
    }

    if (g_ctx.cnx) {
        // §K.13 UAF fix 2026-05-26 — stop and JOIN the IO thread BEFORE
        // touching picoquic state.  Previously picoquic_close(cnx, 0) ran
        // before the join, racing with the IO thread that was still inside
        // its mutex region calling picoquic_incoming_packet /
        // picoquic_prepare_next_packet on the same cnx.  When an OS-level
        // adapter disable forced picoquic into mass path-deletion, the
        // half-modified cnx state observed by the IO thread post-close
        // dereferenced a freed picoquic_path_t → 0xC0000005.
        //
        // The mutex inside the IO loop only protects READS / WRITES of
        // picoquic state from one direction; calling picoquic_close from
        // a different thread bypasses that protection.  Real fix is to
        // make sure no other thread is in picoquic at all when we close.
        g_ctx.ioRunning = false;
        PltInterruptThread(&g_ctx.ioThread);
        PltJoinThread(&g_ctx.ioThread);

        // Safe now — the IO thread has fully exited.
        picoquic_close(g_ctx.cnx, 0);
        g_ctx.cnx = NULL;
    }

    for (int i = 0; i < g_ctx.subflowCount; i++) {
        if (g_ctx.subflows[i].sock != INVALID_SOCKET) {
            closeSocket(g_ctx.subflows[i].sock);
            g_ctx.subflows[i].sock = INVALID_SOCKET;
        }
    }
    g_ctx.subflowCount = 0;

    if (g_ctx.quic) {
        // §5f: save 0-RTT session tickets before freeing context
        if (s_ticketStorePath[0]) {
            int saveRet = picoquic_save_session_tickets(g_ctx.quic, s_ticketStorePath);
            Limelog("[VIPLE-MPQUIC] §5f ticket save: %s (ret=%d)\n",
                    saveRet == 0 ? "OK" : "FAILED", saveRet);
        }

        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
    }

    PltDeleteMutex(&g_ctx.picoquicMutex);
}

bool quicIsConnected(void) {
    return g_ctx.cnx != NULL &&
           picoquic_get_cnx_state(g_ctx.cnx) == picoquic_state_ready;
}

int quicWaitReady(unsigned int timeoutMs) {
    if (!g_ctx.cnx) {
        return -2;
    }
    // Poll picoquic state from the caller thread.  10 ms granularity
    // keeps wakeup latency low and total wall-clock work negligible
    // (handshake on LAN is typically 30-60 ms; over Tailscale ~150 ms).
    unsigned int elapsed = 0;
    picoquic_state_enum lastSt = -1;
    while (elapsed < timeoutMs) {
        picoquic_state_enum st = picoquic_get_cnx_state(g_ctx.cnx);
        if (st != lastSt) {
            Limelog("[VIPLE-MPQUIC] handshake state -> %d (after %u ms)\n",
                    (int)st, elapsed);
            lastSt = st;
        }
        if (st == picoquic_state_ready) {
            return 0;
        }
        if (st == picoquic_state_disconnected) {
            return -1;
        }
        PltSleepMs(10);
        elapsed += 10;
    }
    Limelog("[VIPLE-MPQUIC] handshake stalled at state %d after %u ms\n",
            (int)lastSt, elapsed);
    return -1;
}

// ── Server ──────────────────────────────────────────────────

int quicServerStart(unsigned short port, const char* certPath,
                    const char* keyPath) {
    uint64_t currentTime;

    if (!g_initialized)
        return -1;

    currentTime = picoquic_current_time();

    g_ctx.quic = picoquic_create(
        128,
        certPath, keyPath, NULL,                    // cert / key / root
        "viplestream",
        quicDgramCallback, &g_ctx,
        NULL, NULL, NULL,                           // cnx_id_cb, reset_seed
        currentTime,
        NULL,                                       // simulated_time
        NULL,                                       // ticket_file_name
        NULL, 0                                     // ticket_encryption_key
    );

    if (!g_ctx.quic) {
        Limelog("[VIPLE-MPQUIC] Failed to create server QUIC context\n");
        return -1;
    }

    // Enable DATAGRAM via transport parameter; multipath via default option.
    picoquic_set_default_tp_value(g_ctx.quic,
        picoquic_tp_max_datagram_frame_size, 65535);
    picoquic_set_default_multipath_option(g_ctx.quic, 1);
    picoquic_enable_path_callbacks_default(g_ctx.quic, 1);
    quicApplyCongestionAlgo(g_ctx.quic);

    g_ctx.isServer = true;
    g_ctx.serverPort = port;

    g_ctx.ioRunning = true;
    int ret = PltCreateThread("QuicSrvIO", quicIoThreadProc, &g_ctx, &g_ctx.ioThread);
    if (ret != 0) {
        Limelog("[VIPLE-MPQUIC] Failed to create server I/O thread\n");
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
        return -1;
    }

    Limelog("[VIPLE-MPQUIC] Server listening on port %u\n", port);
    return 0;
}

void quicServerStop(void) {
    g_ctx.ioRunning = false;
    if (g_ctx.quic) {
        PltInterruptThread(&g_ctx.ioThread);
        PltJoinThread(&g_ctx.ioThread);
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
    }
    g_ctx.isServer = false;
}

// ── Scheduler: path selection ───────────────────────────────
// Returns the index into g_ctx.subflows[] for the best path,
// or -1 if no active subflow exists.

static int quicSelectPath(unsigned char flowType, int dataLen) {
    // §5c: client 出站（input/control）走 MIN_RTT — 回傳第一個
    // active subflow。視訊/音訊是 server→client，由 server 端
    // picoquic ECF scheduler 選路。
    (void)flowType;
    (void)dataLen;

    for (int i = 0; i < g_ctx.subflowCount; i++) {
        if (g_ctx.subflows[i].active)
            return i;
    }
    return -1;
}

// ── Subflow management ──────────────────────────────────────

int quicAddSubflow(int interfaceIndex,
                   const char* interfaceName,
                   int interfaceType,
                   const struct sockaddr_storage* localAddr,
                   SOCKADDR_LEN addrLen) {
    return quicAddSubflowEx(interfaceIndex, interfaceName, interfaceType,
                            localAddr, addrLen, NULL, 0);
}

// §Q-DYN-PATH ICMP reachability probe.
// picoquic_probe_new_path() 一旦被呼叫，server 端的 picoquic 就會把
// 新 path 加入 multipath scheduler 並開始往上面送 video datagram。如
// 果該 path 實際上是死路（例如 UsbNcm → cellular → private LAN IP），
// ~50% 的 datagram 會消失，造成 IDR frame 永遠無法組裝。
//
// 這個函式在呼叫 picoquic_probe_new_path 之前，用 Windows ICMP API
// (IcmpSendEcho2Ex) 從指定介面 ping 目標 IP。如果 ping 成功（雙向
// 連通），才允許進入 picoquic；如果 timeout（單向或完全不通），就擋
// 下來。
//
// 限制：只支援 IPv4，IPv6 需要 Icmp6SendEcho2 另外處理。
// 回傳值：0 = 失敗/不可達，>0 = 成功的 ICMP RTT（毫秒）。
// §Q-MP-STANDBY: RTT 用於 path_available 判斷是否保持 standby。
static unsigned int quicIcmpProbeReachable(
    const struct sockaddr_storage* localAddr,
    const struct sockaddr_storage* peerAddr,
    unsigned int timeoutMs) {
#ifdef LC_WINDOWS
    if (localAddr->ss_family != AF_INET || peerAddr->ss_family != AF_INET)
        return 1;  // 非 IPv4：不擋，回傳假 RTT=1ms

    const struct sockaddr_in* src = (const struct sockaddr_in*)localAddr;
    const struct sockaddr_in* dst = (const struct sockaddr_in*)peerAddr;

    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        Limelog("[VIPLE-MPQUIC] IcmpCreateFile failed (%lu); skipping probe\n",
                GetLastError());
        return 1;  // 無法建立 ICMP handle → 不阻擋，假 RTT=1ms
    }

    char sendData[] = "VIPLE";
    char replyBuf[sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 16];
    memset(replyBuf, 0, sizeof(replyBuf));

    DWORD ret = IcmpSendEcho2Ex(hIcmp,
        NULL, NULL, NULL,         // event / apc routine / apc context
        src->sin_addr.s_addr,     // source IP（綁定介面）
        dst->sin_addr.s_addr,     // destination IP
        sendData, (WORD)sizeof(sendData),
        NULL,                     // no IP options
        replyBuf, (DWORD)sizeof(replyBuf),
        timeoutMs);

    IcmpCloseHandle(hIcmp);

    if (ret > 0) {
        PICMP_ECHO_REPLY reply = (PICMP_ECHO_REPLY)replyBuf;
        if (reply->Status == IP_SUCCESS) {
            unsigned int rtt = (unsigned int)reply->RoundTripTime;
            if (rtt == 0) rtt = 1;  // 0ms 保留給失敗
            Limelog("[VIPLE-MPQUIC] ICMP probe OK: %u.%u.%u.%u → %u.%u.%u.%u "
                    "(RTT=%u ms)\n",
                    (src->sin_addr.s_addr) & 0xFF,
                    (src->sin_addr.s_addr >> 8) & 0xFF,
                    (src->sin_addr.s_addr >> 16) & 0xFF,
                    (src->sin_addr.s_addr >> 24) & 0xFF,
                    (dst->sin_addr.s_addr) & 0xFF,
                    (dst->sin_addr.s_addr >> 8) & 0xFF,
                    (dst->sin_addr.s_addr >> 16) & 0xFF,
                    (dst->sin_addr.s_addr >> 24) & 0xFF,
                    rtt);
            return rtt;
        }
    }
    Limelog("[VIPLE-MPQUIC] ICMP probe FAILED: %u.%u.%u.%u → %u.%u.%u.%u "
            "(timeout=%u ms)\n",
            (src->sin_addr.s_addr) & 0xFF,
            (src->sin_addr.s_addr >> 8) & 0xFF,
            (src->sin_addr.s_addr >> 16) & 0xFF,
            (src->sin_addr.s_addr >> 24) & 0xFF,
            (dst->sin_addr.s_addr) & 0xFF,
            (dst->sin_addr.s_addr >> 8) & 0xFF,
            (dst->sin_addr.s_addr >> 16) & 0xFF,
            (dst->sin_addr.s_addr >> 24) & 0xFF,
            timeoutMs);
    return 0;  // 失敗
#else
    // Linux/macOS: 暫不實作 ICMP probe，預設允許，假 RTT=1ms
    (void)localAddr; (void)peerAddr; (void)timeoutMs;
    return 1;
#endif
}

int quicAddSubflowEx(int interfaceIndex,
                     const char* interfaceName,
                     int interfaceType,
                     const struct sockaddr_storage* localAddr,
                     SOCKADDR_LEN addrLen,
                     const struct sockaddr_storage* peerOverride,
                     SOCKADDR_LEN peerOverrideLen) {
    QUIC_SUBFLOW* sf;
    SOCKET sock;
    unsigned int icmpRtt = 0;  // §Q-MP-STANDBY: 記錄 ICMP RTT 供 standby 判斷

    if (!g_ctx.cnx)
        return -1;

    // §Q.link-local: WiFi enabled 但未連接 AP 時，Windows 分配 APIPA
    // 169.254.x.x。connect() 路由檢查和 ICMP 探測都會透過 Ethernet NIC
    // 成功，但 WiFi 實際上不通——picoquic 分配封包到該 path 後封包被丟棄，
    // 造成嚴重的輸入延遲（滑鼠慢好幾秒）。
    if (localAddr->ss_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)localAddr;
        uint32_t ip = ntohl(sin->sin_addr.s_addr);
        if ((ip & 0xFFFF0000) == 0xA9FE0000) {
            Limelog("[VIPLE-MPQUIC] Rejecting if %d: link-local 169.254.x.x "
                    "(no real connectivity)\n", interfaceIndex);
            return -1;
        }
    }

    // Find a usable slot: prefer the end of the array, but recycle
    // slots whose path was permanently deleted by picoquic.
    int slotIdx = -1;
    if (g_ctx.subflowCount < QUIC_MAX_SUBFLOWS) {
        slotIdx = g_ctx.subflowCount;
    } else {
        for (int i = 0; i < g_ctx.subflowCount; i++) {
            if (g_ctx.subflows[i].picoquicDeleted) {
                if (g_ctx.subflows[i].sock != INVALID_SOCKET)
                    closeSocket(g_ctx.subflows[i].sock);
                slotIdx = i;
                break;
            }
        }
    }
    if (slotIdx < 0)
        return -1;  // all slots occupied by live paths

    // Resolve effective peer for reachability probe + probe_new_path.
    // Caller-supplied override takes precedence (path 1+ on alternate
    // server endpoint); otherwise fall back to the global peerAddr that
    // quicConnect cached for path 0.
    const struct sockaddr_storage* effPeer =
        (peerOverride && peerOverrideLen > 0) ? peerOverride : &g_ctx.peerAddr;
    SOCKADDR_LEN effPeerLen =
        (peerOverride && peerOverrideLen > 0) ? peerOverrideLen : g_ctx.peerAddrLen;

    sock = createSocket(localAddr->ss_family, SOCK_DGRAM, IPPROTO_UDP, true);
    if (sock == INVALID_SOCKET) {
        Limelog("[VIPLE-MPQUIC] Failed to create subflow socket for if %d\n",
                interfaceIndex);
        return -1;
    }

    // §Q-DYN-PATH: 給 subflow socket 足夠大的 recv buffer。
    // Windows UDP 預設只有 8 KB（~5 個 datagram），IO thread 在呼叫
    // GetAdaptersAddresses / picoquic_probe_new_path 時會短暫停滯，
    // 如果 buffer 太小就會溢出丟封包。2 MB 足以容納 >1400 個
    // 1412-byte datagram，能撐過 >1 秒的 IO 停滯。
    {
        int rcvBufSize = 2 * 1024 * 1024; // 2 MB
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   (const char*)&rcvBufSize, sizeof(rcvBufSize));
    }

    if (bind(sock, (const struct sockaddr*)localAddr, addrLen) != 0) {
        Limelog("[VIPLE-MPQUIC] Failed to bind subflow socket to if %d\n",
                interfaceIndex);
        closeSocket(sock);
        return -1;
    }

    // §Q-MP-REACH 2026-05-23 — Routing reachability probe.  UDP connect()
    // doesn't put any bytes on the wire; it just walks the routing table
    // and ARP/ND state for the destination, returning ENETUNREACH /
    // EHOSTUNREACH if no route exists from this NIC.  Without this filter
    // we accept every up NIC (Tailscale 100.x, WSL 172.28.x, Hyper-V
    // default switch 172.20.x) into the subflow list, and the IO thread's
    // first-IPv4-family-match sendto in quicIoThreadProc later happily
    // hands picoquic-prepared packets to e.g. the Tailscale socket — which
    // silently drops them because the destination 192.168.51.x isn't in
    // any Tailscale subnet.  Probe against the EFFECTIVE peer (peerOverride
    // for cross-endpoint subflows, otherwise the global path-0 peer).
    // Server-mode subflows skip this check (effPeer family will be 0).
    if (effPeer && effPeer->ss_family != 0 && effPeerLen > 0) {
        if (connect(sock, (const struct sockaddr*)effPeer, effPeerLen) != 0) {
            Limelog("[VIPLE-MPQUIC] Subflow on if %d cannot reach peer "
                    "(route check failed); skipping path\n",
                    interfaceIndex);
            closeSocket(sock);
            return -1;
        }
        // Disconnect (connect to AF_UNSPEC) so the socket falls back to
        // unconnected-UDP semantics — sendto with arbitrary dest, recvfrom
        // with arbitrary source.  picoquic uses sendto/recvfrom, not
        // send/recv, so we MUST detach the connected state.
        struct sockaddr_storage unspec;
        memset(&unspec, 0, sizeof(unspec));
        unspec.ss_family = AF_UNSPEC;
        connect(sock, (const struct sockaddr*)&unspec, sizeof(unspec));

        // §Q-DYN-PATH ICMP 連通性探測 — path 1+ 專用。
        // UDP connect() 只檢查路由表，不驗證端對端連通性。擁有 default
        // route 的介面（如 UsbNcm）永遠通過 connect()，但目的地可能根本
        // 不可達。如果讓 picoquic_probe_new_path 在一條死路上執行，
        // server 端的 picoquic 會把 ~50% video datagram 分到該死路，
        // 造成 IDR frame 永遠無法組裝。
        //
        // ICMP ping 確認雙向連通：封包能到達 server 且 server 能回來。
        // path 0 不做此檢查（它是 handshake 用的初始路徑）。
        if (g_ctx.subflowCount > 0) {
            icmpRtt = quicIcmpProbeReachable(localAddr, effPeer,
                                              ICMP_PROBE_TIMEOUT_MS);
            if (icmpRtt == 0) {
                Limelog("[VIPLE-MPQUIC] Subflow on if %d cannot reach peer "
                        "(ICMP probe failed); skipping path\n",
                        interfaceIndex);
                // 記入 ICMP 拒絕快取，quicRecheckPaths 可據此跳過
                if (g_ctx.icmpRejectedCount < QUIC_MAX_SUBFLOWS) {
                    bool alreadyCached = false;
                    for (int r = 0; r < g_ctx.icmpRejectedCount; r++) {
                        if (g_ctx.icmpRejectedIfs[r] == interfaceIndex) {
                            alreadyCached = true;
                            break;
                        }
                    }
                    if (!alreadyCached) {
                        g_ctx.icmpRejectedIfs[g_ctx.icmpRejectedCount++] = interfaceIndex;
                    }
                }
                closeSocket(sock);
                return -1;
            }
        }
    }

    // §MP-ADV-MUTEX: 保護 picoquic 狀態 + subflow 陣列。
    // socket 建立 / bind / ICMP probe 已在上面完成（不碰 picoquic），
    // 只有 probe_new_path + set_path_status + subflow 陣列寫入需要
    // 和 IO thread 互斥。
    PltLockMutex(&g_ctx.picoquicMutex);

    sf = &g_ctx.subflows[slotIdx];
    memset(sf, 0, sizeof(*sf));
    sf->id = g_ctx.nextSubflowId++;
    sf->interfaceIndex = interfaceIndex;
    if (interfaceName)
        PltSafeStrcpy(sf->name, sizeof(sf->name), interfaceName);
    sf->type = interfaceType;
    sf->sock = sock;
    memcpy(&sf->localAddr, localAddr, addrLen);
    sf->addrLen = addrLen;
    sf->picoquicDeleted = false;
    memcpy(&sf->peerUsed, effPeer, effPeerLen);
    sf->peerUsedLen = effPeerLen;

    if (g_ctx.subflowCount == 0) {
        sf->active = true;
        sf->lastRecvTime = picoquic_current_time();
        sf->picoquicPathId = 0;
    } else {
        sf->active = false;
        sf->lastRecvTime = 0;
        int ret = picoquic_probe_new_path(g_ctx.cnx,
            (const struct sockaddr*)effPeer,
            (const struct sockaddr*)localAddr,
            picoquic_current_time());
        if (ret != 0) {
            Limelog("[VIPLE-MPQUIC] probe_new_path failed (%d) for if %d\n",
                    ret, interfaceIndex);
            closeSocket(sock);
            PltUnlockMutex(&g_ctx.picoquicMutex);
            return -1;
        }
        g_ctx.nextPicoquicPathId++;
        sf->picoquicPathId = g_ctx.nextPicoquicPathId;

        picoquic_set_path_status(g_ctx.cnx, sf->picoquicPathId,
                                 picoquic_path_status_backup);
    }

    sf->icmpRttMs = icmpRtt;
    sf->keepAsStandby = false;

    if (interfaceType == LC_NETIF_TYPE_VPN &&
        g_ctx.subflowCount > 0 &&
        g_ctx.subflows[0].type != LC_NETIF_TYPE_VPN) {
        sf->keepAsStandby = true;
        Limelog("[VIPLE-MPQUIC] §Q-MP-STANDBY: subflow %d (if %d) "
                "marked standby (VPN path, primary is non-VPN)\n",
                sf->id, interfaceIndex);
    } else if (icmpRtt > 0 && g_ctx.subflows[0].rttMs > 0.0f) {
        if ((float)icmpRtt - g_ctx.subflows[0].rttMs >
            (float)STANDBY_RTT_EXCESS_MS) {
            sf->keepAsStandby = true;
            Limelog("[VIPLE-MPQUIC] §Q-MP-STANDBY: subflow %d (if %d) "
                    "marked standby (ICMP RTT=%u ms, path0 RTT=%.1f ms, "
                    "excess > %d ms)\n",
                    sf->id, interfaceIndex, icmpRtt,
                    g_ctx.subflows[0].rttMs, STANDBY_RTT_EXCESS_MS);
        }
    } else if (icmpRtt > (unsigned int)STANDBY_RTT_EXCESS_MS) {
        sf->keepAsStandby = true;
        Limelog("[VIPLE-MPQUIC] §Q-MP-STANDBY: subflow %d (if %d) "
                "marked standby (ICMP RTT=%u ms > %d ms absolute)\n",
                sf->id, interfaceIndex, icmpRtt, STANDBY_RTT_EXCESS_MS);
    }

    if (slotIdx == g_ctx.subflowCount)
        g_ctx.subflowCount++;
    Limelog("[VIPLE-MPQUIC] Added subflow %d on interface %d (path %llu, "
            "slot %d, standby=%s)\n",
            sf->id, interfaceIndex, (unsigned long long)sf->picoquicPathId,
            slotIdx, sf->keepAsStandby ? "YES" : "no");

    // §Q-MP-DYN-STANDBY: 訂閱此路徑的 RTT quality update。
    if (g_ctx.cnx) {
        uint64_t rttDeltaUs = (uint64_t)(DYN_STANDBY_RTT_EXCESS_MS) * 500;
        picoquic_subscribe_to_quality_update_per_path(
            g_ctx.cnx, sf->picoquicPathId, 0, rttDeltaUs);
    }

    PltUnlockMutex(&g_ctx.picoquicMutex);
    return sf->id;
}

int quicRemoveSubflow(int subflowId) {
    for (int i = 0; i < g_ctx.subflowCount; i++) {
        if (g_ctx.subflows[i].id == subflowId) {
            Limelog("[VIPLE-MPQUIC] Removing subflow %d (if %d)\n",
                    subflowId, g_ctx.subflows[i].interfaceIndex);
            if (g_ctx.subflows[i].sock != INVALID_SOCKET) {
                closeSocket(g_ctx.subflows[i].sock);
            }
            for (int j = i; j < g_ctx.subflowCount - 1; j++) {
                g_ctx.subflows[j] = g_ctx.subflows[j + 1];
            }
            g_ctx.subflowCount--;
            return 0;
        }
    }
    return -1;
}

int quicSetScheduler(unsigned char flowType, int strategy) {
    if (flowType > 3 || strategy < 0 || strategy > QUIC_SCHED_ECF)
        return -1;
    g_ctx.scheduler[flowType] = strategy;
    Limelog("[VIPLE-MPQUIC] Scheduler for flow %d set to %d\n",
            flowType, strategy);
    return 0;
}

void quicSetCongestionAlgo(int algo) {
    if (algo < QUIC_CC_NEWRENO || algo > QUIC_CC_CUBIC)
        algo = QUIC_CC_BBR;
    g_ctx.congestionAlgo = algo;
}

static void quicApplyCongestionAlgo(picoquic_quic_t* quic) {
    const char* algoName;
    switch (g_ctx.congestionAlgo) {
    case QUIC_CC_NEWRENO:
        picoquic_set_default_congestion_algorithm(quic, picoquic_newreno_algorithm);
        algoName = "NewReno";
        break;
    case QUIC_CC_CUBIC:
        picoquic_set_default_congestion_algorithm(quic, picoquic_cubic_algorithm);
        algoName = "Cubic";
        break;
    case QUIC_CC_BBR:
    default:
        picoquic_set_default_congestion_algorithm(quic, picoquic_bbr_algorithm);
        algoName = "BBR";
        break;
    }
    Limelog("[VIPLE-MPQUIC] Congestion algorithm: %s\n", algoName);
}

// ── Datagram I/O ────────────────────────────────────────────

static int quicSendOnPath(int pathIdx, unsigned char flowType,
                          const unsigned char* data, int dataLen) {
    unsigned char frame[2048];
    QUIC_DGRAM_HEADER hdr;
    int frameLen;

    if (!quicIsConnected())
        return -1;

    frameLen = QUIC_DGRAM_HEADER_SIZE + dataLen;
    if (frameLen > (int)sizeof(frame))
        return -1;

    hdr.flowType = flowType;
    hdr.reserved = 0;
    hdr.seq = htons(g_ctx.seqCounters[flowType]++);
    memcpy(frame, &hdr, QUIC_DGRAM_HEADER_SIZE);
    memcpy(frame + QUIC_DGRAM_HEADER_SIZE, data, dataLen);

    // §MP-ADV-MUTEX: picoquic_queue_datagram_frame 修改 picoquic
    // 內部狀態，需和 IO thread 互斥。
    //
    // §MP-PRIMARY 2026-05-24: 不再 per-datagram 切換 path_status。
    // picoquic_set_path_status 是長期路徑管理 API（available vs
    // backup），不是 per-packet 路由提示。每次送 datagram 都遍歷
    // 所有路徑切換狀態會造成：
    //   (a) picoquic 排程器無法收斂（連續 datagram 偏好相反）
    //   (b) per-path PN space 下 ACK 路由混亂→假性 62% loss
    //   (c) IDR frame 封包分散到不同 RTT 路徑→depacketizer 超時
    // 改為 primary-path 模式：path 0 = available，其餘 = backup。
    // picoquic 自動把所有 datagram 送到唯一 available 路徑。
    PltLockMutex(&g_ctx.picoquicMutex);

    int ret = picoquic_queue_datagram_frame(g_ctx.cnx, frameLen, frame);
    if (ret == 0 && pathIdx >= 0 && pathIdx < g_ctx.subflowCount) {
        g_ctx.subflows[pathIdx].bytesSent += dataLen;
    }

    PltUnlockMutex(&g_ctx.picoquicMutex);
    return (ret == 0) ? 0 : -1;
}

int quicSendDatagram(unsigned char flowType,
                     const unsigned char* data, int dataLen) {
    int pathIdx;

    if (!quicIsConnected())
        return -1;

    pathIdx = quicSelectPath(flowType, dataLen);

    if (pathIdx == -2) {
        // REDUNDANT: picoquic's queue_datagram_frame is per-cnx and dedups,
        // so we can't actually duplicate on-wire from here. We rotate the
        // preferred-path hint round-robin across active subflows, which at
        // least diversifies which physical path picoquic picks frame-to-frame.
        // Real redundancy needs picoquic per-path queue API (not yet
        // upstream — tracked in §Q Phase 5 backlog).
        static int rrIdx = 0;
        int activeCount = quicGetActiveSubflowCount();
        if (activeCount == 0)
            return -1;
        int target = rrIdx++ % activeCount;
        int chosen = -1;
        int seen = 0;
        for (int i = 0; i < g_ctx.subflowCount; i++) {
            if (g_ctx.subflows[i].active) {
                if (seen == target) { chosen = i; break; }
                seen++;
            }
        }
        if (chosen < 0) chosen = 0;
        return quicSendOnPath(chosen, flowType, data, dataLen);
    }

    if (pathIdx < 0) {
        // No active path — try first subflow as last resort
        if (g_ctx.subflowCount > 0)
            pathIdx = 0;
        else
            return -1;
    }

    return quicSendOnPath(pathIdx, flowType, data, dataLen);
}

void quicSetRecvCallback(QuicRecvCallback callback, void* context) {
    // 後向相容：不帶 flow type 的呼叫設定所有 flow 的 callback。
    // 新程式碼應改用 quicSetRecvCallbackForFlow。
    for (int i = 0; i < 4; i++) {
        g_ctx.recvCallbacks[i] = callback;
        g_ctx.recvContexts[i] = context;
    }
}

void quicSetRecvCallbackForFlow(unsigned char flowType,
                                QuicRecvCallback callback, void* context) {
    if (flowType < 4) {
        g_ctx.recvCallbacks[flowType] = callback;
        g_ctx.recvContexts[flowType] = context;
    }
}

void quicSetFailoverCallback(QuicFailoverCallback callback, void* context) {
    g_ctx.failoverCallback = callback;
    g_ctx.failoverContext = context;
}

// ── Reliable stream I/O ─────────────────────────────────────

int quicSendStream(const unsigned char* data, int dataLen) {
    if (!quicIsConnected())
        return -1;

    int ret = picoquic_add_to_stream(g_ctx.cnx, 0, data, dataLen, 0);
    return (ret == 0) ? 0 : -1;
}

// ── Path stats update ───────────────────────────────────────
// Pull RTT / throughput / loss from picoquic path objects

static void quicUpdatePathStats(void) {
    uint64_t now;

    if (!g_ctx.cnx)
        return;

    now = picoquic_current_time();
    if (now - g_ctx.lastStatsUpdate < STATS_UPDATE_INTERVAL_US)
        return;
    g_ctx.lastStatsUpdate = now;

    for (int i = 0; i < g_ctx.subflowCount; i++) {
        picoquic_path_quality_t pq;
        memset(&pq, 0, sizeof(pq));

        // picoquic_get_path_quality returns 0 on success, non-zero if the
        // path id is invalid (e.g., path got deleted out from under us).
        if (picoquic_get_path_quality(g_ctx.cnx,
                g_ctx.subflows[i].picoquicPathId, &pq) != 0) {
            continue;
        }

        // pq.rtt is in microseconds (smoothed estimate)
        g_ctx.subflows[i].rttMs = (float)pq.rtt / 1000.0f;

        // Receive rate estimate is bytes/sec → Mbps (×8 bits, ÷1e6).
        // Uses receive_rate_estimate (measured incoming throughput) instead
        // of pacing_rate (BBR's theoretical send rate, unreliable on LAN).
        g_ctx.subflows[i].throughputMbps =
            (float)((double)pq.receive_rate_estimate * 8.0 / 1e6);

        // §K.19: 瞬時 loss% — 用 delta（本次 - 上次）而非累積值。
        // 累積值包含連線初期的暫態丟包，長期趨近穩定值但不反映
        // 當前路徑品質。每 STATS_UPDATE_INTERVAL_US（5 秒）更新一次。
        {
            uint64_t deltaSent = pq.sent - g_ctx.subflows[i].prevSent;
            uint64_t deltaLost = pq.lost - g_ctx.subflows[i].prevLost;
            if (deltaSent > 0) {
                float lp = (float)((double)deltaLost / (double)deltaSent * 100.0);
                // §K.19.clamp: 路徑死亡時 picoquic 把所有未 ACK 封包計為 lost，
                // 導致 deltaLost > deltaSent（loss > 100%）。Clamp 避免顯示誤導。
                g_ctx.subflows[i].lossPercent = lp > 100.0f ? 100.0f : lp;
            } else if (g_ctx.subflows[i].prevSent == 0 && pq.sent > 0) {
                // 第一次：用累積值作為初始值
                float lp = (float)((double)pq.lost / (double)pq.sent * 100.0);
                g_ctx.subflows[i].lossPercent = lp > 100.0f ? 100.0f : lp;
            }
            // 否則 deltaSent==0 表示這 5 秒沒送任何封包，保留上次的值
            g_ctx.subflows[i].prevSent = pq.sent;
            g_ctx.subflows[i].prevLost = pq.lost;
        }

        // Surface byte counters too so health monitor can see fresh activity
        g_ctx.subflows[i].bytesSent = pq.bytes_sent;
        g_ctx.subflows[i].bytesRecv = pq.bytes_received;
    }
}

// ── Path health monitoring ──────────────────────────────────
// Detect dead paths via increasing RTT or missing responses

static void quicCheckPathHealth(void) {
    uint64_t now;

    if (!g_ctx.cnx)
        return;

    now = picoquic_current_time();
    if (now - g_ctx.lastHealthCheck < HEALTH_CHECK_INTERVAL_US)
        return;
    g_ctx.lastHealthCheck = now;

    for (int i = 0; i < g_ctx.subflowCount; i++) {
        QUIC_SUBFLOW* sf = &g_ctx.subflows[i];

        // 被 picoquic 永久刪除的 slot 不需要 health check
        if (sf->picoquicDeleted)
            continue;

        // §Q-MP-STANDBY: standby 路徑故意不接收應用資料，
        // 不做 stall 偵測（否則會被誤判為 inactive）。
        // 只要 picoquic 沒刪它，它就是活的。
        if (sf->keepAsStandby)
            continue;

        // Check if path has stalled (no data received for > 5x RTT or 3 seconds)
        uint64_t stallThreshold = (uint64_t)(sf->rttMs * 5000.0f);
        if (stallThreshold < 3000000) stallThreshold = 3000000; // min 3s

        if (sf->active && sf->lastRecvTime > 0 &&
            (now - sf->lastRecvTime) > stallThreshold) {
            sf->consecutiveTimeouts++;

            if (sf->consecutiveTimeouts >= PROBE_TIMEOUT_THRESHOLD) {
                sf->active = false;
                Limelog("[VIPLE-MPQUIC] Subflow %d (if %d) marked INACTIVE "
                        "(%d consecutive timeouts, last recv %.1fs ago)\n",
                        sf->id, sf->interfaceIndex,
                        sf->consecutiveTimeouts,
                        (float)(now - sf->lastRecvTime) / 1e6f);

                if (g_ctx.failoverCallback) {
                    g_ctx.failoverCallback(sf->id, sf->interfaceIndex,
                                           false, g_ctx.failoverContext);
                }

                // §Q-MP-FAILBACK: 失效路徑降級為 backup，避免
                // picoquic 繼續往死路徑排程。
                picoquic_set_path_status(
                    g_ctx.cnx, sf->picoquicPathId,
                    picoquic_path_status_backup);

                // §Q-MP-STANDBY: 主路徑失效 → 找第一個 standby
                // 路徑升級為 available，讓 server 開始在它上面
                // 送資料（failover）。
                for (int j = 0; j < g_ctx.subflowCount; j++) {
                    QUIC_SUBFLOW* candidate = &g_ctx.subflows[j];
                    if (candidate->keepAsStandby &&
                        candidate->active &&
                        !candidate->picoquicDeleted) {
                        candidate->keepAsStandby = false;

                        // §Q-MP-FAILOVER-GRACE: 重置 health check 計時器，
                        // 給新升級路徑足夠時間（≥ stallThreshold 3 秒）開始
                        // 接收 server 資料，避免下次 health check 因為
                        // lastRecvTime 來自路徑建立時（~250 秒前）而立即
                        // 判定 INACTIVE。
                        candidate->lastRecvTime = now;
                        candidate->consecutiveTimeouts = 0;

                        picoquic_set_path_status(
                            g_ctx.cnx, candidate->picoquicPathId,
                            picoquic_path_status_available);
                        g_ctx.failoverPromotedSlot = j;
                        Limelog("[VIPLE-MPQUIC] §Q-MP-FAILOVER: promoted "
                                "standby subflow %d (if %d, slot %d) to "
                                "available (primary if %d failed)\n",
                                candidate->id,
                                candidate->interfaceIndex, j,
                                sf->interfaceIndex);
                        if (g_ctx.failoverCallback) {
                            g_ctx.failoverCallback(
                                candidate->id,
                                candidate->interfaceIndex,
                                true, g_ctx.failoverContext);
                        }
                        break;  // 只升級一個
                    }
                }
            }
        } else if (sf->active) {
            sf->consecutiveTimeouts = 0;
        }

        // §Q-MP-DYN-STANDBY: 動態 RTT 監測——若某路徑 QUIC RTT 超過
        // 最快路徑 + STANDBY_RTT_EXCESS_MS，改為 standby 避免 jitter
        // buffer timeout（10ms）被高 RTT 路徑拖垮。
        // 恢復條件：RTT 降回閾值 50% 以下（滯後，防抖動）。
        if (sf->active && sf->rttMs > 0 && i > 0) {
            float bestRttMs = -1.0f;
            for (int j = 0; j < g_ctx.subflowCount; j++) {
                QUIC_SUBFLOW* best = &g_ctx.subflows[j];
                if (j != i && best->active && !best->keepAsStandby &&
                    !best->picoquicDeleted && best->rttMs > 0) {
                    if (bestRttMs < 0 || best->rttMs < bestRttMs)
                        bestRttMs = best->rttMs;
                }
            }
            if (bestRttMs > 0) {
                float excess = sf->rttMs - bestRttMs;
                if (!sf->keepAsStandby &&
                    excess > (float)DYN_STANDBY_RTT_EXCESS_MS) {
                    sf->keepAsStandby = true;
                    picoquic_set_path_status(g_ctx.cnx, sf->picoquicPathId,
                                             picoquic_path_status_backup);
                    Limelog("[VIPLE-MPQUIC] §Q-MP-DYN-STANDBY: subflow %d (if %d) "
                            "→ standby (QUIC RTT=%.1fms, best=%.1fms, "
                            "excess=%.1f > %dms)\n",
                            sf->id, sf->interfaceIndex,
                            sf->rttMs, bestRttMs, excess,
                            DYN_STANDBY_RTT_EXCESS_MS);
                } else if (sf->keepAsStandby &&
                           excess <= (float)(DYN_STANDBY_RTT_EXCESS_MS / 2)) {
                    sf->keepAsStandby = false;
                    picoquic_set_path_status(g_ctx.cnx, sf->picoquicPathId,
                                             picoquic_path_status_available);
                    Limelog("[VIPLE-MPQUIC] §Q-MP-DYN-STANDBY: subflow %d (if %d) "
                            "→ active (QUIC RTT=%.1fms, best=%.1fms, "
                            "excess=%.1f recovered)\n",
                            sf->id, sf->interfaceIndex,
                            sf->rttMs, bestRttMs, excess);
                }
            }
        }

        // Re-activate a previously dead path if picoquic reports it alive
        if (!sf->active && sf->lastRecvTime > 0 &&
            (now - sf->lastRecvTime) < stallThreshold) {
            sf->active = true;
            sf->consecutiveTimeouts = 0;
            Limelog("[VIPLE-MPQUIC] Subflow %d (if %d) re-activated\n",
                    sf->id, sf->interfaceIndex);

            if (g_ctx.failoverCallback) {
                g_ctx.failoverCallback(sf->id, sf->interfaceIndex,
                                       true, g_ctx.failoverContext);
            }

            // §Q-MP-FAILBACK: 原 primary（slot 0）恢復 → 降級
            // failover 路徑回 backup，恢復 primary-only 排程。
            // 這防止兩條路徑同時 available 造成 server 分散
            // datagram → depacketizer 組裝失敗。
            if (i == 0 && g_ctx.failoverPromotedSlot >= 0) {
                int fslot = g_ctx.failoverPromotedSlot;
                QUIC_SUBFLOW* promoted = &g_ctx.subflows[fslot];
                if (!promoted->picoquicDeleted) {
                    promoted->keepAsStandby = true;
                    picoquic_set_path_status(
                        g_ctx.cnx, promoted->picoquicPathId,
                        picoquic_path_status_backup);
                    Limelog("[VIPLE-MPQUIC] §Q-MP-FAILBACK: primary "
                            "if %d recovered, demoting failover "
                            "subflow %d (if %d, slot %d) back to "
                            "backup\n",
                            sf->interfaceIndex,
                            promoted->id,
                            promoted->interfaceIndex, fslot);
                }
                // 恢復 primary 的 picoquic available 狀態
                picoquic_set_path_status(
                    g_ctx.cnx, sf->picoquicPathId,
                    picoquic_path_status_available);
                g_ctx.failoverPromotedSlot = -1;
            }
        }
    }
}

// ── §Q-DYN-PATH: 定期路徑重掃（v1.5.54+ 僅診斷模式） ─────────
// 每 30 秒重新列舉系統網路介面，記錄異動但 **不做任何探測或加入**。
//
// 設計理由（v1.5.50~v1.5.53 系列實測）：
//   • picoquic_probe_new_path() 一旦執行，server 端 picoquic 立刻把
//     新路徑加入 multipath scheduler 並往上面送 ~50% 的 video datagram。
//     若該路徑是死路，IDR frame 永遠無法組裝 → 畫面凍結。
//   • 唯一可靠的預檢是 ICMP 探測（IcmpSendEcho2Ex），但 200ms timeout
//     會阻塞 IO 執行緒 → depacketizer 收不到資料 → dropFrameState。
//   • 安全策略：所有 picoquic_probe_new_path 只在 Phase B（IO 執行緒
//     啟動前）呼叫，那裡可以安全做 ICMP。IO thread 上的 recheck 只做
//     介面掃描 + 診斷日誌。

static void quicRecheckPaths(void) {
    uint64_t now;

    if (!g_ctx.cnx)
        return;

    now = picoquic_current_time();
    if (now - g_ctx.lastPathRecheck < PATH_RECHECK_INTERVAL_US)
        return;
    g_ctx.lastPathRecheck = now;

    LC_NET_INTERFACE interfaces[LC_NETIF_MAX_COUNT];
    int ifCount = lcEnumNetInterfaces(interfaces, LC_NETIF_MAX_COUNT);
    if (ifCount <= 0)
        return;

    int serverFamily = g_ctx.peerAddr.ss_family;

    // ── Phase 1: 記錄已被 picoquic 刪除的路徑（僅診斷，不重新探測） ──
    // §Q-DYN-PATH 2026-05-24 — 不在 IO 執行緒呼叫 picoquic_probe_new_path。
    // 原因：probe_new_path 讓 server 的 picoquic 知道新路徑並立刻把 ~50%
    // 的 video datagram 排到上面。如果路徑是死路，那些 datagram 就丟了，
    // IDR frame 永遠無法組裝。唯一可靠的預檢是 ICMP 探測（200ms timeout），
    // 但這會阻塞 IO 執行緒造成掉幀（v1.5.53 實證：quicRecheckPaths 在
    // 00:00:41 做 ICMP → dropFrameState）。
    //
    // 安全策略：Phase B（IO 執行緒啟動前）做 ICMP 驗證所有候選路徑。
    // IO 執行緒上的 recheck 僅做介面掃描 + 診斷日誌，不呼叫任何會觸發
    // server 端 multipath scheduler 改變的 API。已刪除的路徑等下次 session
    // 重新連線時在 Phase B 重新評估。
    {
        int deletedCount = 0;
        for (int s = 0; s < g_ctx.subflowCount; s++) {
            QUIC_SUBFLOW* sf = &g_ctx.subflows[s];
            if (!sf->picoquicDeleted)
                continue;
            deletedCount++;
        }
        if (deletedCount > 0) {
            Limelog("[VIPLE-MPQUIC] Recheck: %d deleted path(s) — "
                    "not re-probing on IO thread (would require ICMP)\n",
                    deletedCount);
        }
    }

    // ── Phase 2: 偵測新介面（僅診斷，不加入） ──
    // §Q-DYN-PATH 2026-05-24 — 同上：quicAddSubflow/Ex 內的 ICMP 探測
    // 會在 IO 執行緒阻塞 200ms。新介面在下次 session 的 Phase B 加入。
    {
        int newIfCount = 0;
        for (int i = 0; i < ifCount; i++) {
            if (!interfaces[i].up)
                continue;
            if (interfaces[i].type == LC_NETIF_TYPE_LOOPBACK ||
                interfaces[i].type == LC_NETIF_TYPE_VIRTUAL ||
                interfaces[i].type == LC_NETIF_TYPE_UNKNOWN)
                continue;
            if (interfaces[i].family != serverFamily)
                continue;

            // 檢查是否已經有 subflow 用了這個介面
            bool alreadyTracked = false;
            for (int s = 0; s < g_ctx.subflowCount; s++) {
                if (g_ctx.subflows[s].interfaceIndex == interfaces[i].index &&
                    !g_ctx.subflows[s].picoquicDeleted) {
                    alreadyTracked = true;
                    break;
                }
            }
            if (alreadyTracked)
                continue;

            // 檢查是否在 ICMP 拒絕快取中
            bool icmpRejected = false;
            for (int r = 0; r < g_ctx.icmpRejectedCount; r++) {
                if (g_ctx.icmpRejectedIfs[r] == interfaces[i].index) {
                    icmpRejected = true;
                    break;
                }
            }

            newIfCount++;
            Limelog("[VIPLE-MPQUIC] Recheck: new interface if %d '%s' "
                    "(type=%s) detected but not added on IO thread%s\n",
                    interfaces[i].index, interfaces[i].name,
                    lcNetIfTypeName(interfaces[i].type),
                    icmpRejected ? " (ICMP-rejected in Phase B)" : "");
        }
        (void)newIfCount;
    }
}

// §Q.path-recovery — 背景復原：掃描介面，對 deleted / 新介面做
// ICMP 探測 + quicAddSubflowEx。只在 recovery thread 呼叫（安全
// 做 200ms ICMP），不在 IO thread。
//
// rejectedIfs / rejectedCount：route-check 或 ICMP 失敗的介面快取。
// 避免每 10 秒重複探測永遠到不了 server 的介面（如 Tailscale 100.x
// 只能透過 alt peer 連）。當介面集合變動時清空快取重試。
static void quicTryRecoverPaths(int* rejectedIfs, int* rejectedCount) {
    if (!g_ctx.cnx)
        return;

    LC_NET_INTERFACE interfaces[LC_NETIF_MAX_COUNT];
    int ifCount = lcEnumNetInterfaces(interfaces, LC_NETIF_MAX_COUNT);
    if (ifCount <= 0)
        return;

    int serverFamily = g_ctx.peerAddr.ss_family;
    int recovered = 0;

    // 比較目前介面集合與上一輪——若有新增或消失，清空拒絕快取
    // （網路拓撲變化可能讓之前不通的介面變通）
    {
        static int lastIfFingerprint = 0;
        int fp = ifCount;
        for (int i = 0; i < ifCount; i++)
            fp = fp * 31 + interfaces[i].index;
        if (fp != lastIfFingerprint) {
            if (lastIfFingerprint != 0 && *rejectedCount > 0) {
                Limelog("[VIPLE-MPQUIC] §Q.path-recovery: interface set changed, "
                        "clearing %d rejected cache entries\n", *rejectedCount);
                *rejectedCount = 0;
            }
            lastIfFingerprint = fp;
        }
    }

    for (int i = 0; i < ifCount; i++) {
        if (!interfaces[i].up)
            continue;
        if (interfaces[i].type == LC_NETIF_TYPE_LOOPBACK ||
            interfaces[i].type == LC_NETIF_TYPE_VIRTUAL ||
            interfaces[i].type == LC_NETIF_TYPE_UNKNOWN)
            continue;
        if (interfaces[i].family != serverFamily)
            continue;

        bool alreadyActive = false;
        for (int s = 0; s < g_ctx.subflowCount; s++) {
            if (g_ctx.subflows[s].interfaceIndex == interfaces[i].index &&
                !g_ctx.subflows[s].picoquicDeleted) {
                alreadyActive = true;
                break;
            }
        }
        if (alreadyActive)
            continue;

        // 跳過已知不可達的介面
        bool rejected = false;
        for (int r = 0; r < *rejectedCount; r++) {
            if (rejectedIfs[r] == interfaces[i].index) {
                rejected = true;
                break;
            }
        }
        if (rejected)
            continue;

        Limelog("[VIPLE-MPQUIC] §Q.path-recovery: probing if %d '%s' (%s)\n",
                interfaces[i].index, interfaces[i].name,
                lcNetIfTypeName(interfaces[i].type));

        int ret = quicAddSubflowEx(
            interfaces[i].index,
            interfaces[i].name,
            interfaces[i].type,
            &interfaces[i].addr,
            interfaces[i].addrLen,
            NULL, 0);

        if (ret >= 0) {
            recovered++;
            Limelog("[VIPLE-MPQUIC] §Q.path-recovery: recovered subflow %d "
                    "on if %d '%s'\n",
                    ret, interfaces[i].index, interfaces[i].name);

            for (int r = 0; r < g_ctx.icmpRejectedCount; r++) {
                if (g_ctx.icmpRejectedIfs[r] == interfaces[i].index) {
                    for (int j = r; j < g_ctx.icmpRejectedCount - 1; j++)
                        g_ctx.icmpRejectedIfs[j] = g_ctx.icmpRejectedIfs[j + 1];
                    g_ctx.icmpRejectedCount--;
                    break;
                }
            }
        } else {
            if (*rejectedCount < QUIC_MAX_SUBFLOWS) {
                rejectedIfs[(*rejectedCount)++] = interfaces[i].index;
                Limelog("[VIPLE-MPQUIC] §Q.path-recovery: if %d '%s' rejected, "
                        "cached (%d total)\n",
                        interfaces[i].index, interfaces[i].name, *rejectedCount);
            }
        }
    }

    if (recovered > 0) {
        Limelog("[VIPLE-MPQUIC] §Q.path-recovery: %d path(s) recovered this cycle\n",
                recovered);
    }
}

static void quicRecoveryThreadProc(void* context) {
    QUIC_TRANSPORT_CTX* ctx = (QUIC_TRANSPORT_CTX*)context;
    int rejectedIfs[QUIC_MAX_SUBFLOWS];
    int rejectedCount = 0;

    for (int i = 0; i < RECOVERY_INITIAL_DELAY_S && ctx->recoveryRunning; i++)
        PltSleepMs(1000);

    Limelog("[VIPLE-MPQUIC] §Q.path-recovery: thread started "
            "(poll every %d s)\n", RECOVERY_POLL_INTERVAL_S);

    while (ctx->recoveryRunning) {
        quicTryRecoverPaths(rejectedIfs, &rejectedCount);

        for (int i = 0; i < RECOVERY_POLL_INTERVAL_S && ctx->recoveryRunning; i++)
            PltSleepMs(1000);
    }

    Limelog("[VIPLE-MPQUIC] §Q.path-recovery: thread exiting\n");
}

void quicSetAltPeers(const struct sockaddr_storage* addrs,
                     const SOCKADDR_LEN* addrLens,
                     int count) {
    if (count > QUIC_MAX_ALT_PEERS)
        count = QUIC_MAX_ALT_PEERS;

    g_ctx.altPeerCount = 0;
    if (addrs && addrLens && count > 0) {
        for (int i = 0; i < count; i++) {
            memcpy(&g_ctx.altPeerAddrs[i], &addrs[i], addrLens[i]);
            g_ctx.altPeerAddrLens[i] = addrLens[i];
        }
        g_ctx.altPeerCount = count;
    }
}

// ── Monitoring ──────────────────────────────────────────────

int quicGetSubflowStats(PQUIC_SUBFLOW_STATS out, int maxCount) {
    int count = g_ctx.subflowCount;
    if (count > maxCount)
        count = maxCount;

    for (int i = 0; i < count; i++) {
        QUIC_SUBFLOW* sf = &g_ctx.subflows[i];
        out[i].interfaceIndex = sf->interfaceIndex;
        PltSafeStrcpy(out[i].interfaceName, sizeof(out[i].interfaceName), sf->name);
        out[i].interfaceType = sf->type;
        out[i].rttMs = sf->rttMs;
        out[i].throughputMbps = sf->throughputMbps;
        out[i].lossPercent = sf->lossPercent;
        out[i].reorderPercent = sf->reorderPercent;
        out[i].active = sf->active;
    }

    return count;
}

int quicGetActiveSubflowCount(void) {
    int count = 0;
    for (int i = 0; i < g_ctx.subflowCount; i++) {
        if (g_ctx.subflows[i].active)
            count++;
    }
    return count;
}

// ── Failover status ──────────────────────────────────────────

int quicIsFailoverActive(void) {
    return (g_ctx.failoverPromotedSlot >= 0) ? 1 : 0;
}

// ── Session ticket (0-RTT) ──────────────────────────────────

int quicGetSessionTicket(unsigned char* buf, int bufLen) {
    // §5f: 0-RTT ticket persistence is handled by picoquic file I/O:
    // - Load: picoquic_create() auto-loads from s_ticketStorePath
    // - Save: quicDisconnect() calls picoquic_save_session_tickets()
    // This stub remains for the legacy API surface.
    (void)buf; (void)bufLen;
    return 0;
}

// ── §5a Jitter buffer implementation ────────────────────────

static void quicJitterDeliver(QUIC_TRANSPORT_CTX* ctx,
                              QUIC_JITTER_BUF* jb,
                              unsigned char ft) {
    while (1) {
        int idx = jb->deliverNextSeq & QUIC_JITTER_BUF_MASK;
        if (jb->slots[idx].len == 0)
            break;

        uint8_t* pkt = jb->slots[idx].data;
        int pktLen = jb->slots[idx].len;

        if (pktLen >= QUIC_DGRAM_HEADER_SIZE && ft < 4 && ctx->recvCallbacks[ft]) {
            ctx->recvCallbacks[ft](
                ft,
                pkt + QUIC_DGRAM_HEADER_SIZE,
                pktLen - QUIC_DGRAM_HEADER_SIZE,
                ctx->recvContexts[ft]);
        }

        jb->delivered++;
        jb->slots[idx].len = 0;
        jb->lastDeliverUs = picoquic_current_time();
        jb->deliverNextSeq++;
    }
}

static void quicJitterFlush(QUIC_TRANSPORT_CTX* ctx,
                            QUIC_JITTER_BUF* jb,
                            unsigned char ft) {
    for (int i = 0; i < QUIC_JITTER_BUF_SIZE; i++) {
        uint16_t trySeq = (uint16_t)(jb->deliverNextSeq + i);
        int idx = trySeq & QUIC_JITTER_BUF_MASK;
        if (jb->slots[idx].len > 0) {
            uint8_t* pkt = jb->slots[idx].data;
            int pktLen = jb->slots[idx].len;
            if (pktLen >= QUIC_DGRAM_HEADER_SIZE && ft < 4 && ctx->recvCallbacks[ft]) {
                ctx->recvCallbacks[ft](
                    ft,
                    pkt + QUIC_DGRAM_HEADER_SIZE,
                    pktLen - QUIC_DGRAM_HEADER_SIZE,
                    ctx->recvContexts[ft]);
            }
            jb->delivered++;
            jb->slots[idx].len = 0;
        }
    }
}

static void quicJitterInsert(QUIC_TRANSPORT_CTX* ctx,
                             unsigned char ft,
                             uint8_t* bytes, size_t length) {
    if (ft >= 4 || length < QUIC_DGRAM_HEADER_SIZE || length > QUIC_JITTER_MAX_PKT)
        return;

    // §5a.r2: Video 不再繞過 jitter buffer。之前的 bypass 造成多路徑
    // 封包亂序直送 depacketizer，導致 97% 幀丟失（depacketizer 看到
    // 後續幀封包就宣告當前幀不可恢復）。先前 jitter buffer 73% drop
    // 的根因是 link-local 169.254.x.x 死路徑丟失 ~40% 封包（已在
    // §Q.link-local 修復），不是 buffer 設計問題。
    // 研究支持：IETF draft-amend-iccrg-multipath-reordering 推薦
    // adaptive expiration reorder buffer 處理即時串流的多路徑亂序。

    QUIC_JITTER_BUF* jb = &g_jitterBufs[ft];
    PQUIC_DGRAM_HEADER hdr = (PQUIC_DGRAM_HEADER)bytes;
    uint16_t seq = ntohs(hdr->seq);
    uint64_t now = picoquic_current_time();

    if (!jb->initialized) {
        jb->deliverNextSeq = seq;
        jb->initialized = 1;
        jb->lastDeliverUs = now;
    }

    int16_t diff = (int16_t)(seq - jb->deliverNextSeq);

    if (diff < 0) {
        jb->dropped++;
        return;
    }

    if (diff >= QUIC_JITTER_BUF_SIZE) {
        quicJitterFlush(ctx, jb, ft);
        jb->deliverNextSeq = seq;
    }

    int idx = seq & QUIC_JITTER_BUF_MASK;
    if (jb->slots[idx].len == 0) {
        memcpy(jb->slots[idx].data, bytes, length);
        jb->slots[idx].len = (int)length;
        jb->slots[idx].arrivalUs = now;
    }

    if (diff > 0)
        jb->reordered++;

    quicJitterDeliver(ctx, jb, ft);

    if (now - jb->lastDeliverUs >= QUIC_JITTER_MAX_WAIT_US) {
        for (int i = 0; i < QUIC_JITTER_BUF_SIZE; i++) {
            uint16_t trySeq = (uint16_t)(jb->deliverNextSeq + i);
            int tidx = trySeq & QUIC_JITTER_BUF_MASK;
            if (jb->slots[tidx].len > 0) {
                // §5a.fix: i 是正確的跳過距離（uint16_t 模加法保證正）。
                // 舊寫法 (uint64_t)(trySeq - deliverNextSeq) 在 uint16_t
                // 邊界繞回時，int 升型導致負值轉 uint64_t 造成溢位。
                jb->timedOut += (uint64_t)i;
                jb->deliverNextSeq = (uint16_t)(jb->deliverNextSeq + i);
                quicJitterDeliver(ctx, jb, ft);
                break;
            }
        }
    }
}

// ── §5b FEC decoder ────────────────────────────────────────

static void quicFecTryRecover(QUIC_TRANSPORT_CTX* ctx, unsigned char ft, QUIC_FEC_GROUP* grp) {
    if (!g_fecRs || grp->maxShardLen <= 0)
        return;

    uint8_t padBufs[QUIC_FEC_TOTAL_SHARDS][QUIC_FEC_SHARD_MAX];
    uint8_t* shards[QUIC_FEC_TOTAL_SHARDS];
    uint8_t marks[QUIC_FEC_TOTAL_SHARDS];
    memset(padBufs, 0, sizeof(padBufs));

    for (int i = 0; i < QUIC_FEC_DATA_SHARDS; i++) {
        shards[i] = padBufs[i];
        if (grp->dataPresent & (1 << i)) {
            memcpy(padBufs[i], grp->shardBufs[i], grp->shardLens[i]);
            marks[i] = 0;
        } else {
            marks[i] = 1;
        }
    }
    for (int i = 0; i < QUIC_FEC_PARITY_SHARDS; i++) {
        int si = QUIC_FEC_DATA_SHARDS + i;
        shards[si] = padBufs[si];
        if (grp->parityPresent & (1 << i)) {
            memcpy(padBufs[si], grp->shardBufs[si], grp->shardLens[si]);
            marks[si] = 0;
        } else {
            marks[si] = 1;
        }
    }

    int ret = reed_solomon_decode(g_fecRs, shards, marks,
                                  QUIC_FEC_TOTAL_SHARDS, grp->maxShardLen);
    if (ret != 0) {
        g_fecFailed++;
        return;
    }

    for (int i = 0; i < QUIC_FEC_DATA_SHARDS; i++) {
        if (!(grp->dataPresent & (1 << i))) {
            int origLen = (int)grp->originalLens[i];
            if (origLen <= 0 || origLen > QUIC_FEC_SHARD_MAX)
                origLen = grp->maxShardLen;

            uint8_t recoveredPkt[QUIC_DGRAM_HEADER_SIZE + QUIC_FEC_SHARD_MAX];
            QUIC_DGRAM_HEADER recHdr;
            recHdr.flowType = ft;
            recHdr.reserved = (uint8_t)(i + 1);
            recHdr.seq = htons((uint16_t)(grp->baseSeq + i));
            memcpy(recoveredPkt, &recHdr, QUIC_DGRAM_HEADER_SIZE);
            memcpy(recoveredPkt + QUIC_DGRAM_HEADER_SIZE, shards[i], origLen);

            quicJitterInsert(ctx, ft, recoveredPkt,
                             QUIC_DGRAM_HEADER_SIZE + origLen);
            g_fecRecovered++;
            grp->recovered++;
            grp->dataPresent |= (1 << i);
        }
    }
}

static void quicFecOnShard(QUIC_TRANSPORT_CTX* ctx, unsigned char ft,
    uint8_t fecInfo, uint16_t seq, uint8_t* payload, int payloadLen) {

    int isParity = (fecInfo & QUIC_FEC_PARITY_FLAG) != 0;
    int shardIndex;
    uint16_t baseSeq;

    if (isParity) {
        shardIndex = QUIC_FEC_DATA_SHARDS + (fecInfo & 0x7F) - 1;
        baseSeq = seq;
    } else {
        shardIndex = (fecInfo & 0x7F) - 1;
        baseSeq = (uint16_t)(seq - shardIndex);
    }

    if (shardIndex < 0 || shardIndex >= QUIC_FEC_TOTAL_SHARDS)
        return;

    int gi = baseSeq % QUIC_FEC_MAX_GROUPS;
    QUIC_FEC_GROUP* grp = &g_fecGroups[gi];
    uint64_t now = picoquic_current_time();

    if (grp->active && (grp->baseSeq != baseSeq ||
                        now - grp->createTimeUs > 50000)) {
        grp->active = 0;
    }

    if (!grp->active) {
        memset(grp, 0, sizeof(*grp));
        grp->baseSeq = baseSeq;
        grp->active = 1;
        grp->createTimeUs = now;
    }

    if (isParity) {
        int pi = shardIndex - QUIC_FEC_DATA_SHARDS;
        if (pi < QUIC_FEC_PARITY_SHARDS &&
            !(grp->parityPresent & (1 << pi)) &&
            payloadLen > QUIC_FEC_META_SIZE) {
            for (int i = 0; i < QUIC_FEC_DATA_SHARDS; i++) {
                uint16_t origLen;
                memcpy(&origLen, payload + i * 2, 2);
                grp->originalLens[i] = ntohs(origLen);
            }
            int parityDataLen = payloadLen - QUIC_FEC_META_SIZE;
            if (parityDataLen > QUIC_FEC_SHARD_MAX)
                parityDataLen = QUIC_FEC_SHARD_MAX;
            memcpy(grp->shardBufs[shardIndex],
                   payload + QUIC_FEC_META_SIZE, parityDataLen);
            grp->shardLens[shardIndex] = parityDataLen;
            if (parityDataLen > grp->maxShardLen)
                grp->maxShardLen = parityDataLen;
            grp->parityPresent |= (1 << pi);
        }
    } else {
        if (!(grp->dataPresent & (1 << shardIndex))) {
            int copyLen = payloadLen;
            if (copyLen > QUIC_FEC_SHARD_MAX)
                copyLen = QUIC_FEC_SHARD_MAX;
            memcpy(grp->shardBufs[shardIndex], payload, copyLen);
            grp->shardLens[shardIndex] = copyLen;
            if (copyLen > grp->maxShardLen)
                grp->maxShardLen = copyLen;
            grp->dataPresent |= (1 << shardIndex);
        }
    }

    int dataCount = 0, parityCount = 0;
    for (int i = 0; i < QUIC_FEC_DATA_SHARDS; i++)
        if (grp->dataPresent & (1 << i)) dataCount++;
    for (int i = 0; i < QUIC_FEC_PARITY_SHARDS; i++)
        if (grp->parityPresent & (1 << i)) parityCount++;

    if (dataCount < QUIC_FEC_DATA_SHARDS &&
        (dataCount + parityCount) >= QUIC_FEC_DATA_SHARDS) {
        quicFecTryRecover(ctx, ft, grp);
    }
}

// ── picoquic callbacks ──────────────────────────────────────

static int quicDgramCallback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx,
    void* stream_ctx) {
    QUIC_TRANSPORT_CTX* ctx = (QUIC_TRANSPORT_CTX*)callback_ctx;
    (void)stream_ctx;

    switch (fin_or_event) {
    case picoquic_callback_datagram:
        if (length >= QUIC_DGRAM_HEADER_SIZE) {
            PQUIC_DGRAM_HEADER hdr = (PQUIC_DGRAM_HEADER)bytes;
            unsigned char ft = hdr->flowType;
            // §K.10 diag: per-flow 接收計數
            {
                if (ft < 4) {
                    g_dgramsRecvByFlow[ft]++;
                    g_bytesRecvByFlow[ft] += (uint64_t)length;
                }
                // 首筆 datagram 記錄
                static int firstLogCount = 0;
                if (firstLogCount < 3) {
                    firstLogCount++;
                    Limelog("[VIPLE-MPQUIC] §K.10 recv dgram flow=%d len=%d seq=%u\n",
                            (int)ft, (int)length, (unsigned)ntohs(hdr->seq));
                }
            }
            // §5b FEC + §5a jitter buffer dispatch
            if (hdr->reserved == 0 || ft != QUIC_FLOW_VIDEO) {
                quicJitterInsert(ctx, ft, bytes, (int)length);
            } else if (hdr->reserved & QUIC_FEC_PARITY_FLAG) {
                uint8_t* payload = bytes + QUIC_DGRAM_HEADER_SIZE;
                int payloadLen = (int)(length - QUIC_DGRAM_HEADER_SIZE);
                quicFecOnShard(ctx, ft, hdr->reserved,
                               ntohs(hdr->seq), payload, payloadLen);
            } else {
                uint8_t* payload = bytes + QUIC_DGRAM_HEADER_SIZE;
                int payloadLen = (int)(length - QUIC_DGRAM_HEADER_SIZE);
                quicFecOnShard(ctx, ft, hdr->reserved,
                               ntohs(hdr->seq), payload, payloadLen);
                quicJitterInsert(ctx, ft, bytes, (int)length);
            }
        }
        // Per-subflow lastRecvTime is updated by the I/O thread directly
        // when recvfrom returns on a given socket (the I/O thread knows
        // which subflow socket fired); the picoquic datagram callback
        // has no way to identify the receiving path. We DO bump global
        // health here as a coarse "the connection is alive" signal.
        break;

    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (stream_id == 0 && ctx->recvCallbacks[QUIC_FLOW_CONTROL]) {
            ctx->recvCallbacks[QUIC_FLOW_CONTROL](
                QUIC_FLOW_CONTROL, bytes, (int)length,
                ctx->recvContexts[QUIC_FLOW_CONTROL]);
        }
        break;

    case picoquic_callback_ready:
        // Handshake complete — now we can enable datagram sends.
        // picoquic_mark_datagram_ready signals to the stack that the
        // application has datagrams to send; without it, the callback
        // path is never asked for outbound datagrams.
        Limelog("[VIPLE-MPQUIC] Connection ready (handshake complete)\n");
        picoquic_mark_datagram_ready(cnx, 1);
        break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
        Limelog("[VIPLE-MPQUIC] Connection closed\n");
        break;

    case picoquic_callback_path_available: {
        // picoquic passes the unique_path_id in stream_id for path events.
        // Mark the corresponding subflow active (it may have been pending
        // since quicAddSubflow probed it before the path validated).
        Limelog("[VIPLE-MPQUIC] Path available: id=%llu\n",
                (unsigned long long)stream_id);
        for (int i = 0; i < ctx->subflowCount; i++) {
            if (ctx->subflows[i].picoquicPathId == stream_id) {
                if (!ctx->subflows[i].active) {
                    ctx->subflows[i].active = true;
                    ctx->subflows[i].lastRecvTime = picoquic_current_time();

                    // §5c: jitter buffer + FEC 就位後允許多路徑 available。
                    // keepAsStandby 路徑（cellular 等）仍維持 standby。
                    if (ctx->subflows[i].keepAsStandby) {
                        picoquic_set_path_status(
                            ctx->cnx, stream_id,
                            picoquic_path_status_backup);
                    } else {
                        picoquic_set_path_status(
                            ctx->cnx, stream_id,
                            picoquic_path_status_available);
                    }

                    if (ctx->subflows[i].keepAsStandby) {
                        Limelog("[VIPLE-MPQUIC] Path %llu validated, "
                                "backup/standby (if %d, ICMP RTT=%u ms)\n",
                                (unsigned long long)stream_id,
                                ctx->subflows[i].interfaceIndex,
                                ctx->subflows[i].icmpRttMs);
                    } else {
                        Limelog("[VIPLE-MPQUIC] Path %llu validated, "
                                "backup/failover-ready (if %d)\n",
                                (unsigned long long)stream_id,
                                ctx->subflows[i].interfaceIndex);
                    }

                    if (ctx->failoverCallback) {
                        ctx->failoverCallback(ctx->subflows[i].id,
                            ctx->subflows[i].interfaceIndex,
                            true, ctx->failoverContext);
                    }
                }
                break;
            }
        }
        break;
    }

    case picoquic_callback_path_quality_changed: {
        // §Q-MP-DYN-STANDBY: 強制 health check 立即重跑
        ctx->lastHealthCheck = 0;
        break;
    }

    case picoquic_callback_path_suspended:
    case picoquic_callback_path_deleted: {
        Limelog("[VIPLE-MPQUIC] Path %s: id=%llu\n",
                fin_or_event == picoquic_callback_path_deleted
                    ? "deleted" : "suspended",
                (unsigned long long)stream_id);
        for (int i = 0; i < ctx->subflowCount; i++) {
            if (ctx->subflows[i].picoquicPathId == stream_id) {
                ctx->subflows[i].active = false;
                if (fin_or_event == picoquic_callback_path_deleted) {
                    // §Q-DYN-PATH: 標記為 picoquic 永久刪除。slot 會在
                    // 下一次 quicRecheckPaths() 週期中被回收（重新探測
                    // 同一介面）或讓出給新介面使用。
                    ctx->subflows[i].picoquicDeleted = true;
                }
                if (ctx->failoverCallback) {
                    ctx->failoverCallback(ctx->subflows[i].id,
                        ctx->subflows[i].interfaceIndex,
                        false, ctx->failoverContext);
                }
                break;
            }
        }
        break;
    }

    default:
        break;
    }

    return 0;
}

// ── I/O thread ──────────────────────────────────────────────

static void quicIoThreadProc(void* context) {
    QUIC_TRANSPORT_CTX* ctx = (QUIC_TRANSPORT_CTX*)context;
    unsigned char recvBuf[2048];
    unsigned char sendBuf[2048];
    SOCKET serverSock = INVALID_SOCKET;

    // In server mode, bind the listening socket
    if (ctx->isServer) {
        struct sockaddr_in6 listenAddr;
        memset(&listenAddr, 0, sizeof(listenAddr));
        listenAddr.sin6_family = AF_INET6;
        listenAddr.sin6_port = htons(ctx->serverPort);
        listenAddr.sin6_addr = in6addr_any;

        serverSock = createSocket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, false);
        if (serverSock == INVALID_SOCKET) {
            Limelog("[VIPLE-MPQUIC] Failed to create server socket\n");
            return;
        }

        int v6only = 0;
        setsockopt(serverSock, IPPROTO_IPV6, IPV6_V6ONLY,
                   (const char*)&v6only, sizeof(v6only));

        if (bind(serverSock, (struct sockaddr*)&listenAddr,
                 sizeof(listenAddr)) != 0) {
            Limelog("[VIPLE-MPQUIC] Failed to bind server socket\n");
            closeSocket(serverSock);
            return;
        }
    }

    while (ctx->ioRunning) {
        struct sockaddr_storage peerAddr;
        SOCKADDR_LEN peerLen = sizeof(peerAddr);
        uint64_t currentTime = picoquic_current_time();

        // Build fd_set for all subflow sockets + server socket
        fd_set readSet;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000; // 1ms poll
        FD_ZERO(&readSet);

        SOCKET maxSock = INVALID_SOCKET;

        if (ctx->isServer && serverSock != INVALID_SOCKET) {
            FD_SET(serverSock, &readSet);
            maxSock = serverSock;
        }

        // Poll ALL subflow sockets (multipath: each path has its own socket)
        for (int i = 0; i < ctx->subflowCount; i++) {
            if (ctx->subflows[i].sock != INVALID_SOCKET) {
                FD_SET(ctx->subflows[i].sock, &readSet);
                if (ctx->subflows[i].sock > maxSock || maxSock == INVALID_SOCKET) {
                    maxSock = ctx->subflows[i].sock;
                }
            }
        }

        int selRet = 0;
        if (maxSock != INVALID_SOCKET) {
            selRet = select((int)(maxSock + 1), &readSet, NULL, NULL, &tv);
        } else {
            PltSleepMs(1);
        }

        // §MP-ADV-MUTEX: picoquic 不是 thread-safe。Connection thread
        // 的 Phase B (quicAddSubflowEx → picoquic_probe_new_path) 和
        // 送出 thread (quicSendOnPath → picoquic_queue_datagram_frame)
        // 可能同時存取 picoquic 內部狀態。Lock 涵蓋所有 picoquic 呼叫：
        // incoming_packet、prepare_next_packet、get_path_quality 等。
        // select() 和 fd_set 建構在 lock 外（不碰 picoquic 狀態）。
        PltLockMutex(&g_ctx.picoquicMutex);

        // §K.13 UAF fix 2026-05-26 — 防 picoquic 內部 mass path-deletion
        // 期間 cnx->path[0]->first_tuple-> ... NULL deref（sender.c:826
        // picoquic_predict_packet_header_length 已實證命中）。兩道 bail：
        //
        // (a) cnx state 已進 disconnecting/disconnected — picoquic 自己
        //     在 close 流程，再 prepare_next_packet 一定踩雷。
        // (b) cnx state 還是 ready 但所有 subflow path 都被 picoquic 刪
        //     完了——這時 picoquic 內部 cnx->path[0] 可能 transient 為
        //     NULL/half-freed（callback 已 fire，但 array shift 還沒
        //     原子完成）。用 picoquic_get_path_quality 對任一未刪 subflow
        //     的 unique_path_id 查一次：成功表示 picoquic 真的還持有它，
        //     全部都 fail 表示 picoquic 已沒可用 path 但 cnx 還來不及變
        //     disconnecting，我們就主動 bail。
        if (ctx->cnx == NULL ||
            picoquic_get_cnx_state(ctx->cnx) >= picoquic_state_disconnecting) {
            PltUnlockMutex(&g_ctx.picoquicMutex);
            Limelog("[VIPLE-MPQUIC] §K.13 IO loop bail: cnx state >= "
                    "disconnecting, stopping cleanly\n");
            ctx->ioRunning = false;
            break;
        }
        {
            int alivePaths = 0;
            for (int i = 0; i < ctx->subflowCount; i++) {
                if (ctx->subflows[i].picoquicDeleted) continue;
                picoquic_path_quality_t pq;
                memset(&pq, 0, sizeof(pq));
                if (picoquic_get_path_quality(ctx->cnx,
                        ctx->subflows[i].picoquicPathId, &pq) == 0) {
                    alivePaths = 1;
                    break;
                }
                // get_path_quality fail 表示 picoquic 已不認 unique_path_id
                // (路徑被刪但 callback 還沒 fire / 我們 picoquicDeleted 旗
                // 標還沒設)。標起來避免下次 iteration 再查。
                ctx->subflows[i].picoquicDeleted = true;
            }
            if (alivePaths == 0 && ctx->subflowCount > 0) {
                PltUnlockMutex(&g_ctx.picoquicMutex);
                Limelog("[VIPLE-MPQUIC] §K.13 IO loop bail: picoquic has 0 "
                        "alive paths but cnx state=%d, stopping before "
                        "next picoquic_prepare_next_packet hits a NULL "
                        "path[0]\n",
                        (int)picoquic_get_cnx_state(ctx->cnx));
                ctx->ioRunning = false;
                break;
            }
        }

        if (selRet > 0) {
            // Check server socket
            if (ctx->isServer && serverSock != INVALID_SOCKET &&
                FD_ISSET(serverSock, &readSet)) {
                peerLen = sizeof(peerAddr);
                SOCK_RET recvLen = recvfrom(serverSock, (char*)recvBuf,
                    sizeof(recvBuf), 0,
                    (struct sockaddr*)&peerAddr, &peerLen);
                if (recvLen > 0) {
                    picoquic_incoming_packet(ctx->quic,
                        recvBuf, (size_t)recvLen,
                        (struct sockaddr*)&peerAddr,
                        (struct sockaddr*)&((struct sockaddr_in6){
                            .sin6_family = AF_INET6,
                            .sin6_port = htons(ctx->serverPort),
                            .sin6_addr = in6addr_any
                        }),
                        0, 0, currentTime);
                }
            }

            // Check each subflow socket — drain pending packets.
            // Sockets are non-blocking; the inner loop exits on
            // EWOULDBLOCK. Reading multiple packets per select
            // reduces the overhead of mutex unlock → fd_set rebuild
            // → select → mutex lock per packet. After each batch,
            // picoquic_prepare_next_packet sends ACKs promptly.
            for (int i = 0; i < ctx->subflowCount; i++) {
                SOCKET sf = ctx->subflows[i].sock;
                if (sf != INVALID_SOCKET && FD_ISSET(sf, &readSet)) {
                    int pktsThisSocket = 0;
                    while (pktsThisSocket < 16) {
                        peerLen = sizeof(peerAddr);
                        SOCK_RET recvLen = recvfrom(sf, (char*)recvBuf,
                            sizeof(recvBuf), 0,
                            (struct sockaddr*)&peerAddr, &peerLen);
                        if (recvLen <= 0)
                            break;
                        pktsThisSocket++;
                        ctx->subflows[i].lastRecvTime = currentTime;
                        ctx->subflows[i].bytesRecv += (uint64_t)recvLen;
                        if (!ctx->subflows[i].active) {
                            ctx->subflows[i].active = true;
                            ctx->subflows[i].consecutiveTimeouts = 0;
                        }
                        picoquic_incoming_packet(ctx->quic,
                            recvBuf, (size_t)recvLen,
                            (struct sockaddr*)&peerAddr,
                            (struct sockaddr*)&ctx->subflows[i].localAddr,
                            ctx->subflows[i].interfaceIndex,
                            0, currentTime);
                    }
                }
            }
        }

        // §5c: 多路徑 available guard — 所有 active non-standby 路徑
        // 設為 available，讓 picoquic ECF scheduler 自行選路。
        // §K.13 UAF fix: 使用 API 而非直接 access cnx->path[]。
        if (ctx->cnx != NULL && ctx->subflowCount > 1 &&
            picoquic_get_cnx_state(ctx->cnx) < picoquic_state_disconnecting) {
            for (int i = 0; i < ctx->subflowCount; i++) {
                if (ctx->subflows[i].picoquicDeleted) continue;
                picoquic_path_status_enum st;
                if (!ctx->subflows[i].active || ctx->subflows[i].keepAsStandby) {
                    st = picoquic_path_status_backup;
                } else {
                    st = picoquic_path_status_available;
                }
                (void)picoquic_set_path_status(ctx->cnx,
                    ctx->subflows[i].picoquicPathId, st);
            }
        }

        // Process picoquic state machine.
        // picoquic_prepare_next_packet takes the QUIC context (not a cnx)
        // and walks the wake queue internally; the lastCnx output tells us
        // which connection it sent for.
        while (true) {
            struct sockaddr_storage destAddr, localAddr;
            int ifIndex = 0;
            size_t sendLen = 0;
            picoquic_connection_id_t logCid;
            picoquic_cnx_t* lastCnx = NULL;

            int ret = picoquic_prepare_next_packet(ctx->quic,
                picoquic_current_time(),
                sendBuf, sizeof(sendBuf), &sendLen,
                &destAddr, &localAddr,
                &ifIndex, &logCid, &lastCnx);

            if (ret != 0 || sendLen == 0)
                break;

            // Find matching subflow socket for the local address picoquic
            // chose. Picoquic populates `localAddr` with the local endpoint
            // it wants to send from when multipath is active.
            SOCKADDR_LEN destLen = (destAddr.ss_family == AF_INET)
                ? (SOCKADDR_LEN)sizeof(struct sockaddr_in)
                : (SOCKADDR_LEN)sizeof(struct sockaddr_in6);

            // §Q-MP-DISPATCH 2026-05-23 — Pick the right subflow socket for
            // the packet picoquic prepared.  Old logic was "first IPv4-or-
            // IPv6-family socket wins" which always selected subflows[0]
            // even when picoquic's chosen path was on a different NIC.
            // That's how every multipath packet ended up funnelled through
            // (e.g.) the Tailscale socket and silently dropped en route to
            // a LAN destination.  New priority order:
            //   1. ifIndex exact match — picoquic populates this with the
            //      OS interface index of the path it scheduled
            //   2. localAddr exact IP match — picoquic populates this with
            //      the path's local endpoint when multipath is active
            //   3. destination-family match — fallback for the initial
            //      packet where path 0 is unvalidated and picoquic gives
            //      INADDR_ANY / 0-ifIndex
            //   4. subflows[0] — last resort to keep the cnx alive
            SOCKET outSock = INVALID_SOCKET;

            // 1. ifIndex match (most precise; populated for established paths)
            if (ifIndex != 0) {
                for (int i = 0; i < ctx->subflowCount; i++) {
                    if (ctx->subflows[i].sock != INVALID_SOCKET &&
                        ctx->subflows[i].interfaceIndex == ifIndex) {
                        outSock = ctx->subflows[i].sock;
                        break;
                    }
                }
            }

            // 2. localAddr exact IP match (multipath per-path local endpoint)
            if (outSock == INVALID_SOCKET && localAddr.ss_family != 0) {
                for (int i = 0; i < ctx->subflowCount; i++) {
                    if (ctx->subflows[i].sock == INVALID_SOCKET) continue;
                    if (ctx->subflows[i].localAddr.ss_family != localAddr.ss_family) continue;
                    if (localAddr.ss_family == AF_INET) {
                        const struct sockaddr_in* sa = (const struct sockaddr_in*)&ctx->subflows[i].localAddr;
                        const struct sockaddr_in* sb = (const struct sockaddr_in*)&localAddr;
                        if (sb->sin_addr.s_addr == 0) continue; // INADDR_ANY = no preference
                        if (sa->sin_addr.s_addr == sb->sin_addr.s_addr) {
                            outSock = ctx->subflows[i].sock;
                            break;
                        }
                    } else if (localAddr.ss_family == AF_INET6) {
                        const struct sockaddr_in6* sa = (const struct sockaddr_in6*)&ctx->subflows[i].localAddr;
                        const struct sockaddr_in6* sb = (const struct sockaddr_in6*)&localAddr;
                        // Treat all-zero IPv6 addr as unspecified
                        static const uint8_t zero16[16] = {0};
                        if (memcmp(&sb->sin6_addr, zero16, 16) == 0) continue;
                        if (memcmp(&sa->sin6_addr, &sb->sin6_addr, 16) == 0) {
                            outSock = ctx->subflows[i].sock;
                            break;
                        }
                    }
                }
            }

            // 3. Destination-family match (legacy fallback; uses destAddr now
            //    that the reachability probe in quicAddSubflow has already
            //    pruned subflows whose interface can't reach the peer)
            if (outSock == INVALID_SOCKET) {
                for (int i = 0; i < ctx->subflowCount; i++) {
                    if (ctx->subflows[i].sock != INVALID_SOCKET &&
                        ctx->subflows[i].localAddr.ss_family == destAddr.ss_family) {
                        outSock = ctx->subflows[i].sock;
                        break;
                    }
                }
            }
            if (outSock == INVALID_SOCKET && ctx->isServer) {
                outSock = serverSock;
            }
            if (outSock == INVALID_SOCKET && ctx->subflowCount > 0) {
                outSock = ctx->subflows[0].sock;
            }

            if (outSock != INVALID_SOCKET) {
                sendto(outSock, (const char*)sendBuf, (int)sendLen, 0,
                       (struct sockaddr*)&destAddr, destLen);
            }
        }

        // Periodic maintenance
        quicUpdatePathStats();
        quicCheckPathHealth();
        quicRecheckPaths();  // §Q-DYN-PATH: 定期重掃介面（僅診斷日誌，不探測/加入）

        // §K.10 diag: 每 5 秒印出 per-flow 接收統計 + per-path 品質
        {
            uint64_t nowDiag = picoquic_current_time();
            if (g_lastDiagLogTime == 0) g_lastDiagLogTime = nowDiag;
            if (nowDiag - g_lastDiagLogTime >= DIAG_LOG_INTERVAL_US) {
                double dtSec = (double)(nowDiag - g_lastDiagLogTime) / 1e6;
                if (dtSec < 0.1) dtSec = 0.1;

                Limelog("[VIPLE-MPQUIC] §K.10 RECV VIDEO: %llu dgrams %llu B (%.1f Mbps) | "
                        "AUDIO: %llu dgrams %llu B (%.1f Mbps) | "
                        "CTRL: %llu dgrams\n",
                        (unsigned long long)g_dgramsRecvByFlow[1],
                        (unsigned long long)g_bytesRecvByFlow[1],
                        (double)(g_bytesRecvByFlow[1] - g_prevBytesRecvByFlow[1]) * 8.0 / dtSec / 1e6,
                        (unsigned long long)g_dgramsRecvByFlow[2],
                        (unsigned long long)g_bytesRecvByFlow[2],
                        (double)(g_bytesRecvByFlow[2] - g_prevBytesRecvByFlow[2]) * 8.0 / dtSec / 1e6,
                        (unsigned long long)g_dgramsRecvByFlow[3]);

                // Video ring buffer 診斷
                {
                    int ringDepth = 0, ringCap = 0;
                    uint64_t ringDrops = 0;
                    quicVideoGetRingStats(&ringDepth, &ringCap, &ringDrops);
                    Limelog("[VIPLE-MPQUIC] §K.10 videoRing depth=%d/%d drops=%llu\n",
                            ringDepth, ringCap,
                            (unsigned long long)ringDrops);
                }

                // §5a Jitter buffer 診斷
                {
                    const char* flowNames[] = {"(none)", "VIDEO", "AUDIO", "CTRL"};
                    for (int ji = 0; ji < 4; ji++) {
                        QUIC_JITTER_BUF* jb = &g_jitterBufs[ji];
                        if (jb->delivered || jb->reordered || jb->dropped || jb->timedOut) {
                            Limelog("[VIPLE-MPQUIC] §5a Jitter[%s]: delivered=%llu reordered=%llu dropped=%llu timedOut=%llu\n",
                                    flowNames[ji],
                                    (unsigned long long)jb->delivered,
                                    (unsigned long long)jb->reordered,
                                    (unsigned long long)jb->dropped,
                                    (unsigned long long)jb->timedOut);
                        }
                    }
                }

                // §5b FEC 診斷
                if (g_fecRecovered || g_fecFailed) {
                    Limelog("[VIPLE-MPQUIC] §5b FEC: recovered=%llu failed=%llu\n",
                            (unsigned long long)g_fecRecovered,
                            (unsigned long long)g_fecFailed);
                }

                // Per-path 摘要
                for (int pi = 0; pi < ctx->subflowCount; pi++) {
                    QUIC_SUBFLOW* sf = &ctx->subflows[pi];
                    Limelog("[VIPLE-MPQUIC] §K.10 path[%d] if=%d %s RTT=%.1fms %.1fMbps loss=%.1f%% tx=%lluKB rx=%lluKB\n",
                            pi, sf->interfaceIndex,
                            sf->active ? "ACTIVE" : "DEAD",
                            sf->rttMs, sf->throughputMbps, sf->lossPercent,
                            (unsigned long long)(sf->bytesSent / 1024),
                            (unsigned long long)(sf->bytesRecv / 1024));
                }

                g_prevBytesRecvByFlow[1] = g_bytesRecvByFlow[1];
                g_prevBytesRecvByFlow[2] = g_bytesRecvByFlow[2];
                g_prevBytesRecvByFlow[3] = g_bytesRecvByFlow[3];
                g_lastDiagLogTime = nowDiag;
            }
        }

        PltUnlockMutex(&g_ctx.picoquicMutex);
    }

    if (serverSock != INVALID_SOCKET) {
        closeSocket(serverSock);
    }
}

#endif // VIPLE_MPQUIC
