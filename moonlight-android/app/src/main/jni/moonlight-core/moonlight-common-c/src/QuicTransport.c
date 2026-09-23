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
// 若跨路徑 RTT excess > timeout/3（~3ms），WiFi OWD 抖動就可能讓
// 部分封包突破 timeout 導致 timedOut → drop。
// v1.5.141 實測 WiFi RTT=5.0ms excess=4.3ms 未觸發 5ms 閾值；
// 降為 3ms 讓 LAN WiFi（典型 3-8ms RTT）更早進 standby。
// 回復閾值 = threshold / 2 = 1.5ms（50% 遲滯防止震盪）。
// （STANDBY_RTT_EXCESS_MS=10 留給初始 ICMP probe 的靜態判斷，
//   不動；DYN_STANDBY_RTT_EXCESS_MS 只用在 live QUIC RTT 動態監測）
#define DYN_STANDBY_RTT_EXCESS_MS  3

// §Q-PATH-RECOVERY-STANDBY: 恢復路徑的 standby 寬限期（秒）。
// 路徑恢復後，立即標為 standby 不讓 server 切影片過去。寬限期內
// DYN-STANDBY 不會 promote，讓 server 繼續用已穩定的路徑。
// 5 秒 ≈ picoquic slow-start 需要的 warm-up 時間。
#define RECOVERY_STANDBY_GRACE_S   5

// §Q-STANDBY-REGAIN 2026-07-02: 恢復路徑的自動回歸。
// 2026-07-01 事故：乙太網路瞬斷 → §Q-MP-EMERGENCY-PROMOTE 升 VPN →
// 乙太 2 秒後恢復，但被 §Q-PATH-RECOVERY-STANDBY 放進 standby 後再也
// 沒有機制升回——standby 路徑在 health check 第一關就 continue，
// §Q-INTERFACE-FAILBACK 又要求 !keepAsStandby——視訊在高 loss 的 VPN
// 上困 86 分鐘、ABR 被持續 loss 壓死在 ~11Mbps。
// 機制：standby 路徑健康駐留 REGAIN_HEALTH_S 後，若所有健康 available
// 路徑的介面 rank（ETHERNET < WIFI < VPN）都嚴格比它差，把它升回
// available、把 rank 較差者降回 standby。REGAIN_FLAP_WINDOW_S 內同介面
// 再死視為 flap，駐留時間指數退避（10→20→40→80s）。
#define REGAIN_HEALTH_S        10
#define REGAIN_FLAP_WINDOW_S   60
#define REGAIN_FLAP_MAX        3

// §Q-LOSS-DEMOTE 2026-07-06 凍結事故：zombie 路徑的 loss 降級準則。
// 事故：乙太鏈路劣化成「ICMP 通、UDP 大流量 77~100% 丟包」，slot 0 卡在
// ACTIVE 十二分鐘不降級——stall 偵測只看 lastRecvTime，而 IO 迴圈收到
// 任一 UDP 封包（含 ACK/控制面零星封包）就刷新它。補 loss 準則：合格
// 樣本（每 100ms stats tick 至少 LOSS_MIN_DELTA_SENT 個送出封包）的瞬時
// loss 連續 ≥ LOSS_DEMOTE_PCT 達 LOSS_DEMOTE_HOLD_US、且存在替代路徑
// （健康 available 或 probe 新鮮的 standby）→ 走既有 INACTIVE 序列降級。
// 40% 門檻遠高於 ABR 較勁的典型 loss（<20%、亞秒級），不會誤殺。
#define LOSS_DEMOTE_PCT        40.0f
#define LOSS_DEMOTE_HOLD_US    4000000ull   // 4s（promote 後 probation 內 2s）
#define LOSS_MIN_DELTA_SENT    20
#define LOSS_PROBATION_US      10000000ull  // 促升後 10s probation

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

    // §Q-PERF.2: 上次經 quicSetPathStatusTracked 設定的 path status
    // （+1 編碼；0 = 未設定/slot 回收後未知）。picoquic_set_path_status
    // 不去重——每次呼叫都排一個 PATH_AVAILABLE/BACKUP frame 上線路
    // （frames.c picoquic_queue_path_available_or_backup_frame 無條件
    // queue_misc_frame），§5c guard 每 ~1ms 重設一次 → 多路徑時每秒
    // 數千個冗餘控制 frame。所有 set_path_status 改走 wrapper 快取去重。
    uint8_t lastSetStatusPlus1;

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
    // §Q-PATH-RECOVERY-STANDBY 2026-05-27: 恢復的路徑以 standby 加入，
    // 設寬限期（5 秒）讓 cwnd warm up，防止 server 端 bestVideoPath
    // 立即切回低 RTT 但 cwnd 冷的恢復路徑導致震盪。
    // DYN-STANDBY promotion 在此時間前不執行。0 = 不受限。
    uint64_t recoveryStandbyUntil;

    // §Q-STANDBY-REGAIN: 此 standby 路徑可被升回 available 的最早時刻
    // （健康駐留期滿）。0 = 尚未武裝（進入 standby 或第一次評估時設定）。
    uint64_t regainEligibleAt;
    // §Q-STANDBY-REGAIN liveness（review 補強）：recovery thread 對武裝
    // 中的 regain 候選做 ICMP + 介面 up 檢查，成功時蓋章。standby 路徑
    // 無應用流量且跳過 stall 偵測，光靠計時器無法證明路徑活著——REGAIN
    // 要求此章新鮮（<25s）才升級。0 = 尚未通過或最近一次探測失敗。
    uint64_t lastStandbyProbeOk;

    // §Q-LOSS-DEMOTE (2026-07-06 凍結事故)：瞬時 loss 首次越檻
    // （LOSS_DEMOTE_PCT）的時刻；0 = 目前不超標。合格樣本
    // （deltaSent ≥ LOSS_MIN_DELTA_SENT）低於門檻即歸零 → 等效
    // 「連續超標」，天然濾掉 ABR/BBR 探測造成的秒級 loss 尖峰。
    uint64_t lossBadSinceUs;
    // §Q-LOSS-DEMOTE：最近一次 picoquic_get_path_quality 成功時刻。
    // 查詢失敗時 rttMs/lossPercent 凍結為舊值（事故中 zombie 路徑 RTT
    // 卡在 570ms 十二分鐘），此欄位供 stats 過期診斷。0 = 從未成功。
    uint64_t statsUpdatedUs;
    // §Q-LOSS-DEMOTE：最近一次被促升為 available 的時刻。促升後
    // LOSS_PROBATION_US 內 loss 越檻的降級 hold 減半（4s → 2s），
    // 讓 REGAIN 促升到壞路徑的停格更短。
    uint64_t lastPromotedAtUs;
} QUIC_SUBFLOW;

typedef struct _QUIC_TRANSPORT_CTX {
    picoquic_quic_t* quic;
    picoquic_cnx_t* cnx;

    QUIC_SUBFLOW subflows[QUIC_MAX_SUBFLOWS];
    int subflowCount;
    int nextSubflowId;

    int scheduler[QUIC_FLOW_COUNT]; // index 0 = default, 1..4 = per QUIC_FLOW_*
    int aggregateRRIndex; // round-robin index for AGGREGATE

    // Per-flow receive callbacks (index = flow type: 0=unused,
    // 1=VIDEO, 2=AUDIO, 3=CONTROL). 各 flow 獨立註冊，
    // QUIC datagram 到達時根據 header 的 flowType 分派。
    QuicRecvCallback recvCallbacks[QUIC_FLOW_COUNT];
    void*            recvContexts[QUIC_FLOW_COUNT];

    // Failover callback
    QuicFailoverCallback failoverCallback;
    void* failoverContext;

    // Per-flow sequence counters (for datagram header)
    unsigned short seqCounters[QUIC_FLOW_COUNT];

    // I/O thread
    PLT_THREAD ioThread;
    bool ioRunning;
    // §Q-K13-ZOMBIE-FIX (review)：§K.13 bail（picoquic 0 alive path 或
    // cnx>=disconnecting）讓 IO thread 永久退出，但 cnx 沒被 close、
    // state 凍結在 ready → quicIsConnected() 仍回 true → ControlStream
    // 的 quicControlFallbackAvailable 永遠判 QUIC 可用，control/input
    // 走一條沒有 IO thread 在送收的死管道，recovery thread 也照常 probe
    // 但封包永遠送不出 → 不可自癒殭屍態（TODO Q.r14）。bail 時設此旗標，
    // 讓 quicIsConnected() 立即回 false、recovery thread 退出。
    volatile int ioDead;

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

    // §Q-STANDBY-REGAIN: flap 退避狀態。lastRegainIf 介面在 regain 後
    // REGAIN_FLAP_WINDOW_S 內死亡 → regainFlapCount++（cap
    // REGAIN_FLAP_MAX），下次駐留時間 ×2^count；存活滿視窗歸零。
    int regainFlapCount;
    int lastRegainIf;       // -1 = 尚無 regain 紀錄
    uint64_t lastRegainTime;
    // §Q-REGAIN-BREAKER 2026-07-02（round 4 IDR 風暴教訓）：flap 達
    // REGAIN_FLAP_MAX → 暫停 REGAIN 與 INACTIVE-REVIVE 五分鐘。ICMP 通
    // 不代表 QUIC 通（例如剛升級路徑踩 picoquic per-path queue 排程
    // 缺陷），持續重升只會製造 PATH-SWITCH + IDR 風暴；斷路後留在
    // 當前可用路徑，五分鐘後（狀態沉澱）再試。0 = 未斷路。
    uint64_t regainSuspendedUntil;

    // §Q-BREAKER-ESC (2026-07-06 凍結事故)：斷路器遞增。原本 300s 到期
    // flapCount 歸零，同一顆壞介面每 5-6 分鐘重演「3 次 flap（= 3 次
    // 停格）→ 斷路」。改為同介面重複跳閘 → 暫停 300s×4^(n-1)、上限
    // 1800s；期滿 regainFlapCount 照舊歸零但 tripCount 保留，單次 flap
    // 即再斷路（不必再付 3 次停格）。forgiveness：斷路之後真的有一次
    // regain 且存活滿 flap 視窗才全赦（見 quicMaybeRegainStandby）。
    int breakerTripCount;
    int lastBreakerIf;       // -1 = 尚無跳閘紀錄
    uint64_t lastBreakerTripTime;

    // §Q-FAILOVER-SETTLED (Fix A6)：failover 承載者連續健康的起算時刻。
    // 連續健康 30s → failoverPromotedSlot=-1 宣告新常態（不切路徑）。
    uint64_t failoverHealthySinceUs;

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

    // §5a.r3 v1.5.198 Fix Q：代表性 active-path RTT（微秒），供 jitter
    // buffer 做 RTT 自適應 reorder timeout。在 quicUpdatePathStats 更新
    // rttMs 的同回圈寫入（取所有 active non-deleted subflow 的最大 RTT）。
    // 讀寫都在單一 IO thread，volatile 即可（比照 g_dgramsRecvByFlow）。
    volatile uint32_t jitterReorderRttUs;
} QUIC_TRANSPORT_CTX;

static QUIC_TRANSPORT_CTX g_ctx;
static bool g_initialized = false;

// §K.10 diag: per-flow 接收計數器（全域，IO thread + callback thread 共用）
// index: 0=unused, 1=VIDEO, 2=AUDIO, 3=CONTROL, 4=INPUT
static volatile uint64_t g_dgramsRecvByFlow[QUIC_FLOW_COUNT] = {};
static volatile uint64_t g_bytesRecvByFlow[QUIC_FLOW_COUNT] = {};
static uint64_t g_prevBytesRecvByFlow[QUIC_FLOW_COUNT] = {};
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
// §Q-IDR-JITTER-RELAX v1.5.177：正常 10ms，failover 期間 200ms。
// WiFi cwnd 冷啟動只有 ~0.45 Mbps（根因 E），IDR shard 到達間距
// 可能 >> 10ms。放寬 timeout 讓 jitter buffer 等更久，讓 IDR 的
// 所有 shard 有機會到齊。failover 結束後自動回到 10ms。
#define QUIC_JITTER_MAX_WAIT_NORMAL_US   10000  // 10ms 正常（floor）
#define QUIC_JITTER_MAX_WAIT_FAILOVER_US 200000 // 200ms failover
// §5a.r3 v1.5.198 Fix Q：正常模式改成 RTT 自適應（1.5×RTT，夾在
// floor 10ms ~ cap）。寫死 10ms 在 ~30ms RTT 遠端路徑會把「會晚到
// 的重排封包」過早跳過 → 過早 unrecoverable frame + lag。
//
// §5a-CAP-REMOTE 2026-07-20：cap 40ms → 250ms。07-17/07-20 遠端事故
// （RTT 89-300ms）實證：40ms cap 把 1.5×RTT 應有的 130-450ms 等待窗
// 截斷，3-31% 的遲到/重排 shard 被人工判丟 → 幀組不齊 → 凍結。
// cap 只在「有缺口等待時」才生效（LAN 1.5×RTT≈10ms floor 不受影響）；
// 遠端寧可多等一個 RTT 也不要把可用 shard 丟掉——凍結比延遲糟。
#define QUIC_JITTER_MAX_WAIT_CAP_US      250000  // 250ms 上限

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
    uint64_t timedOut;   // skipped due to timeout（封包數）
    // §5a.r3 v1.5.198 Fix Q 診斷：
    uint32_t lastJitterMaxWait; // 當前實際採用的 reorder timeout（us）
    uint32_t maxReorderDist;    // 觀察到的最大 reorder 距離（diff 峰值）
    uint64_t timeoutEvents;     // timeout 跳過「事件」次數（與 timedOut 封包數分開）
    uint64_t lateAfterSkip;     // 真的被 timeout 跳過的 seq 又遲到（timeout 太激進訊號，應 ~0）
    // §5a.r4 v1.5.202 Fix Q.5：把「FEC 冗餘遲到」從 lateAfterSkip 分離。
    // FEC 重建 shard 後推進 deliverNextSeq，該 shard 的真實封包稍後遲到 →
    // diff<0。這在有損路徑很常見且良性，不該汙染「timeout 太激進」訊號。
    uint64_t fecRedundantLate;  // FEC 已重建、真實封包稍後遲到的良性丟棄數
    uint16_t lastSkipFrom;      // 最近一次 timeout 跳過的起始 seq
    int      lastSkipCount;     // 最近一次 timeout 跳過的 seq 數量（範圍 [from, from+count)）
    uint64_t lastSkipUs;        // 最近一次 timeout 跳過的時戳（過期窗用）
    // §Q-PERF.4: 目前佔用的 slot 數。quicJitterPoll 每 IO tick 都會被
    // 呼叫，用這個計數讓「buffer 為空」的常見情況 O(1) 提前返回。
    // 只在 IO thread 讀寫（insert/deliver/flush 全在 IO thread）。
    int      bufferedCount;
    // §Q-PERF.7 診斷：timeout 想跳過但缺口屬於仍可重建的 FEC 群組而
    // 延後的次數（每 tick 計一次）。
    uint64_t fecDeferred;
    // §Q-PERF.8 診斷：FEC 重建完成但該 seq 已被跳過/交付的次數——
    // 與 lateAfterSkip（真實封包遲到）分開，T2 實測 906/910 全是這類，
    // 混在一起會誤判 timeout 過於激進。
    uint64_t fecRecoveredLate;
} QUIC_JITTER_BUF;

static QUIC_JITTER_BUF g_jitterBufs[QUIC_FLOW_COUNT]; // per flow type

// ── §5b FEC decoder (video only, RS 4+2) ────────────────────
#include "rswrapper.h"

#define QUIC_FEC_DATA_SHARDS   4
#define QUIC_FEC_PARITY_SHARDS 2
#define QUIC_FEC_TOTAL_SHARDS  6
// §Q-PERF.3: 8 → 32。舊值搭配 gi = baseSeq % 8 且 baseSeq stride=4
// （server 端 parity 不消耗 seq）→ gcd(4,8)=4，8 個 bucket 實際只用 2 個，
// 高碼率下晚到 >1 群組時間（~0.5ms）的 shard 就會發現群組已被驅逐。
// 32 群組 × ~9KB ≈ 290KB 靜態記憶體，換 128 個 data shard 的 in-flight 窗。
#define QUIC_FEC_MAX_GROUPS    32
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
// §Q-PERF.3 診斷：群組未湊齊（dataPresent != 0xF）就被新群組驅逐或過期
// 的次數。修復 bucket 碰撞前這個值在高碼率有損路徑會快速累積。
static uint64_t g_fecEvictedIncomplete = 0;
// §Q-PERF.8: 標記「目前的 quicJitterInsert 來自 FEC 重建」，讓 diff<0
// 分支把重建遲到與真實封包遲到分開計數。只在 IO thread 讀寫。
static int g_fecInsertingRecovered = 0;

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
static void quicApplyCongestionAlgo(picoquic_quic_t* quic);
static void quicJitterDeliver(QUIC_TRANSPORT_CTX* ctx, QUIC_JITTER_BUF* jb, unsigned char ft);
static void quicJitterFlush(QUIC_TRANSPORT_CTX* ctx, QUIC_JITTER_BUF* jb, unsigned char ft);
static void quicJitterInsert(QUIC_TRANSPORT_CTX* ctx, unsigned char ft, uint8_t* bytes, size_t length);
static int  quicJitterComputeMaxWait(QUIC_TRANSPORT_CTX* ctx);
static void quicJitterCheckTimeout(QUIC_TRANSPORT_CTX* ctx, QUIC_JITTER_BUF* jb,
    unsigned char ft, uint64_t now);
static void quicJitterPoll(QUIC_TRANSPORT_CTX* ctx);
static void quicFecOnShard(QUIC_TRANSPORT_CTX* ctx, unsigned char ft, uint8_t fecInfo,
    uint16_t seq, uint8_t* payload, int payloadLen);
static void quicFecTryRecover(QUIC_TRANSPORT_CTX* ctx, unsigned char ft, QUIC_FEC_GROUP* grp);
static int  quicFecGapStillCompletable(uint16_t startSeq, int count,
    uint64_t now, int jitterMaxWaitUs);

// §Q-PERF.2: picoquic_set_path_status 的去重 wrapper。picoquic 端每次
// 呼叫都無條件排一個 PATH_AVAILABLE/PATH_BACKUP misc frame（sequence
// 遞增）上線路；§5c guard 每 ~1ms 重設全部 subflow → 多路徑串流時
// 每秒數千個冗餘 frame + 配置。快取上次設定值，相同即跳過。
// 所有 picoquic_set_path_status 呼叫一律走這裡，確保快取一致——
// 漏掉的直呼叫會讓快取 stale，§5c 可能跳過必要的重設。
// 呼叫端必須持 picoquicMutex（與原本直呼叫的前提相同）。
static int quicSetPathStatusTracked(QUIC_SUBFLOW* sf, picoquic_path_status_enum st) {
    uint8_t plus1 = (uint8_t)(st + 1);
    if (sf->lastSetStatusPlus1 == plus1)
        return 0;
    int ret = picoquic_set_path_status(g_ctx.cnx, sf->picoquicPathId, st);
    if (ret == 0)
        sf->lastSetStatusPlus1 = plus1;
    return ret;
}

// §Q-LOSS-DEMOTE: 促升點統一重置健康欄位。五個促升處（REGAIN /
// MP-FAILOVER / SECONDARY / EMERGENCY / DYN-STANDBY-ALIVE）原本各自
// 重複 lastRecvTime/consecutiveTimeouts 兩行，抽 helper 統一追加
// loss 計時歸零與 probation 起算，避免日後新增促升點漏改。
static void quicResetSubflowHealthOnPromote(QUIC_SUBFLOW* sf, uint64_t now) {
    sf->lastRecvTime = now;        // §Q-MP-FAILOVER-GRACE: 防立即 stall 誤判
    sf->consecutiveTimeouts = 0;
    sf->lossBadSinceUs = 0;        // 舊任期的 loss 帳不帶入新任期
    sf->lastPromotedAtUs = now;    // §Q-LOSS-DEMOTE probation 起算
}

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
    g_ctx.lastRegainIf = -1;         // §Q-STANDBY-REGAIN: 尚無 regain 紀錄
    g_ctx.lastBreakerIf = -1;        // §Q-BREAKER-ESC: 尚無跳閘紀錄

    // Default scheduler per flow type
    g_ctx.scheduler[0] = QUIC_SCHED_AUTO;
    g_ctx.scheduler[QUIC_FLOW_VIDEO] = QUIC_SCHED_ECF;
    g_ctx.scheduler[QUIC_FLOW_AUDIO] = QUIC_SCHED_REDUNDANT;
    g_ctx.scheduler[QUIC_FLOW_CONTROL] = QUIC_SCHED_MIN_RTT;
    g_ctx.scheduler[QUIC_FLOW_INPUT] = QUIC_SCHED_MIN_RTT;

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
    g_ctx.ioDead = 0; // §Q-K13-ZOMBIE-FIX：新連線清除殭屍旗標
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
    // §Q-K13-ZOMBIE-FIX：IO thread §K.13 bail 後 cnx state 凍結在 ready，
    // 但已無人收送封包——ioDead 讓上層（ControlStream fallback 閘門、
    // VideoStream/AudioStream 路由）正確判定 QUIC 已死。
    return g_ctx.cnx != NULL && !g_ctx.ioDead &&
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

    // §Q-SLOT-CLEANUP: 找可用 slot。優先順序：
    //   1. 同介面的死 slot（path-recovery 重建時最常見，避免陣列堆積
    //      重複 interfaceIndex 造成 overlay 顯示兩筆同名條目）
    //   2. 任意死 slot（不同介面，回收空間）
    //   3. 陣列尾端（容量未滿時 append）
    int slotIdx = -1;

    // Pass 1: 同介面的死 slot
    // §Q-SLOT0-FAILBACK-FIX (review)：跳過 slot 0。slot 0 是初始 primary，
    // 若被回收重生會被 §Q-PATH-RECOVERY-STANDBY 強制 keepAsStandby，而所有
    // 把 standby promote 回 available 的 health-check 分支都以 i>0 排除
    // slot 0 → primary 物理恢復後永久卡 standby、failoverPromotedSlot 永不
    // 清除（quicIsFailoverActive 永久成立 → 200ms jitter timeout 鎖死、
    // video 永走次優路徑）。讓恢復落在新 slot，§Q-INTERFACE-FAILBACK
    //（i>0 + interfaceIndex 匹配 slot 0）才能 promote 新 slot 並清 failover。
    for (int i = 1; i < g_ctx.subflowCount; i++) {
        if (g_ctx.subflows[i].picoquicDeleted &&
            g_ctx.subflows[i].interfaceIndex == interfaceIndex) {
            if (g_ctx.subflows[i].sock != INVALID_SOCKET) {
                closeSocket(g_ctx.subflows[i].sock);
                g_ctx.subflows[i].sock = INVALID_SOCKET; // §Q-SLOT-HANDLE-FIX
            }
            slotIdx = i;
            Limelog("[VIPLE-MPQUIC] Recycling dead slot %d for same interface "
                    "if %d\n", i, interfaceIndex);
            break;
        }
    }

    // Pass 2: 任意死 slot 或 append
    if (slotIdx < 0) {
        if (g_ctx.subflowCount < QUIC_MAX_SUBFLOWS) {
            slotIdx = g_ctx.subflowCount;
        } else {
            // 容量已滿才回收任意死 slot（含 slot 0——耗盡保護優先於
            // 上述 slot0-failback 偏好，避免直接 return -1 失敗）。
            for (int i = 0; i < g_ctx.subflowCount; i++) {
                if (g_ctx.subflows[i].picoquicDeleted) {
                    if (g_ctx.subflows[i].sock != INVALID_SOCKET) {
                        closeSocket(g_ctx.subflows[i].sock);
                        g_ctx.subflows[i].sock = INVALID_SOCKET; // §Q-SLOT-HANDLE-FIX
                    }
                    slotIdx = i;
                    break;
                }
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

        quicSetPathStatusTracked(sf, picoquic_path_status_backup);
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
    } else if (interfaceType == LC_NETIF_TYPE_WIFI &&
               g_ctx.subflowCount > 0 &&
               g_ctx.subflows[0].type == LC_NETIF_TYPE_ETHERNET) {
        // §Q-MP-WIFI-STANDBY v1.5.179: WiFi 在 Ethernet primary 存在時
        // 一律標 standby。WiFi jitter 變異量不可控（beacon、interference、
        // driver delay），即使 RTT 低（1.9ms），突發 spike 仍超過 jitter
        // buffer 的 10ms timeout → 跨路徑 shard timeout → frame 無法
        // FEC 重建 → IDR wait → 凍結。
        // WiFi 的價值在 failover 備援，不是正常操作分流。
        sf->keepAsStandby = true;
        Limelog("[VIPLE-MPQUIC] §Q-MP-WIFI-STANDBY: subflow %d (if %d) "
                "marked standby (WiFi path, primary is Ethernet — "
                "WiFi jitter risk exceeds 10ms timeout)\n",
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
    // 當 RTT 變化超過 DYN_STANDBY_RTT_EXCESS_MS/2（2.5ms → 取整為 2ms）
    // 時，picoquic 觸發 picoquic_callback_path_quality_changed。
    // 這讓我們在下一次 health check（500ms）之前就能偵測 RTT 惡化。
    if (g_ctx.cnx) {
        uint64_t rttDeltaUs = (uint64_t)(DYN_STANDBY_RTT_EXCESS_MS) * 500; // 2.5ms in µs
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
    if (flowType >= QUIC_FLOW_COUNT || strategy < 0 || strategy > QUIC_SCHED_ECF)
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

// §Q-INPUT-DIAG v1.5.195 Fix N: QUIC input datagram 排隊統計。
// quicCheckPathHealth 每 5 秒彙總一次，診斷 failover 期間 input
// 是否成功經由 QUIC 送出。
static volatile int g_quicInputQueued = 0;
static volatile int g_quicInputFailed = 0;

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

    // §Q-INPUT-DIAG: 追蹤 input datagram 排隊成功/失敗
    if (flowType == QUIC_FLOW_INPUT) {
        if (ret == 0) g_quicInputQueued++;
        else g_quicInputFailed++;
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
    for (int i = 0; i < QUIC_FLOW_COUNT; i++) {
        g_ctx.recvCallbacks[i] = callback;
        g_ctx.recvContexts[i] = context;
    }
}

void quicSetRecvCallbackForFlow(unsigned char flowType,
                                QuicRecvCallback callback, void* context) {
    if (flowType < QUIC_FLOW_COUNT) {
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

    // §Q-PERF.1: picoquic_add_to_stream 修改 picoquic 內部 stream 佇列，
    // 必須與 IO thread 互斥（同 quicSendOnPath 的 §MP-ADV-MUTEX）。
    // 此函式由 lossStatsThread（§Q-REMOTE Fix R.2 的 FEC status/ping，
    // ENet 斷線期間每 100ms）與 ControlStream 的 IDR request 路徑呼叫
    // ——failover 期間 path churn 最劇烈時與 prepare_next_packet 併發，
    // 無鎖會破壞內部狀態（ACK 路由錯亂/假性 loss/隨機斷線）。
    PltLockMutex(&g_ctx.picoquicMutex);
    if (g_ctx.cnx == NULL) { // 鎖前檢查與取鎖之間可能被斷線清掉
        PltUnlockMutex(&g_ctx.picoquicMutex);
        return -1;
    }
    int ret = picoquic_add_to_stream(g_ctx.cnx, 0, data, dataLen, 0);
    PltUnlockMutex(&g_ctx.picoquicMutex);
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

    // §5a.r5 v1.5.204 優化：取「吞吐量最高的 active path」(= 實際載 video
    // 的路徑) 的 RTT 給 jitter reorder timeout，而非所有 active subflow 的
    // MAX。實測 6 場串流發現：閒置 standby（WiFi 29ms / Tailscale）會把 MAX
    // 拉到 25-40ms，即使 video 全走 1.7ms 的 Ethernet → reorder 等待被不必要
    // 拉長。改用 video 路徑 RTT 後多路徑回到 floor；單路徑該路徑即最高吞吐，
    // 行為不變（遠端 WARP/Tailscale 單路徑仍得 ~40ms 自適應）。
    float videoPathRttMs = 0.0f;
    float maxThroughputMbps = -1.0f;

    for (int i = 0; i < g_ctx.subflowCount; i++) {
        picoquic_path_quality_t pq;
        memset(&pq, 0, sizeof(pq));

        // picoquic_get_path_quality returns 0 on success, non-zero if the
        // path id is invalid (e.g., path got deleted out from under us).
        if (picoquic_get_path_quality(g_ctx.cnx,
                g_ctx.subflows[i].picoquicPathId, &pq) != 0) {
            // §Q-LOSS-DEMOTE: 查詢失敗時 rttMs/lossPercent 凍結為舊值
            // （2026-07-06 事故中 zombie 的 RTT 卡在 570ms 十二分鐘）。
            // 首次越過 3s 過期線時印一次診斷（100ms tick，窗口判斷即可）。
            if (g_ctx.subflows[i].statsUpdatedUs != 0 &&
                (now - g_ctx.subflows[i].statsUpdatedUs) > 3000000 &&
                (now - g_ctx.subflows[i].statsUpdatedUs) <
                    3000000 + STATS_UPDATE_INTERVAL_US * 2) {
                Limelog("[VIPLE-MPQUIC] §Q-LOSS-DEMOTE: subflow %d (if %d) "
                        "path quality stats stale >3s (query failing) — "
                        "rtt/loss values frozen\n",
                        g_ctx.subflows[i].id,
                        g_ctx.subflows[i].interfaceIndex);
            }
            continue;
        }
        g_ctx.subflows[i].statsUpdatedUs = now;

        // pq.rtt is in microseconds (smoothed estimate)
        g_ctx.subflows[i].rttMs = (float)pq.rtt / 1000.0f;

        // Receive rate estimate is bytes/sec → Mbps (×8 bits, ÷1e6).
        // Uses receive_rate_estimate (measured incoming throughput) instead
        // of pacing_rate (BBR's theoretical send rate, unreliable on LAN).
        g_ctx.subflows[i].throughputMbps =
            (float)((double)pq.receive_rate_estimate * 8.0 / 1e6);

        // §5a.r5：選吞吐量最高的 active non-deleted path（= 實際載 video
        // 的路徑），記其 RTT 供 jitter reorder timeout 使用。
        if (g_ctx.subflows[i].active && !g_ctx.subflows[i].picoquicDeleted &&
            g_ctx.subflows[i].throughputMbps > maxThroughputMbps) {
            maxThroughputMbps = g_ctx.subflows[i].throughputMbps;
            videoPathRttMs = g_ctx.subflows[i].rttMs;
        }

        // §K.19: 瞬時 loss% — 用 delta（本次 - 上次）而非累積值。
        // 累積值包含連線初期的暫態丟包，長期趨近穩定值但不反映
        // 當前路徑品質。每 STATS_UPDATE_INTERVAL_US（100ms）更新一次。
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
            // 否則 deltaSent==0 表示這段期間沒送任何封包，保留上次的值

            // §Q-LOSS-DEMOTE: 合格樣本（≥LOSS_MIN_DELTA_SENT 封包）的越檻
            // 計時。任一合格樣本低於門檻即歸零 → 等效「連續超標」，天然
            // 濾掉 ABR/BBR 探測造成的秒級 loss 尖峰；樣本不足不改判。
            if (deltaSent >= LOSS_MIN_DELTA_SENT) {
                if (g_ctx.subflows[i].lossPercent >= LOSS_DEMOTE_PCT) {
                    if (g_ctx.subflows[i].lossBadSinceUs == 0)
                        g_ctx.subflows[i].lossBadSinceUs = now;
                } else {
                    g_ctx.subflows[i].lossBadSinceUs = 0;
                }
            }

            g_ctx.subflows[i].prevSent = pq.sent;
            g_ctx.subflows[i].prevLost = pq.lost;
        }

        // Surface byte counters too so health monitor can see fresh activity
        g_ctx.subflows[i].bytesSent = pq.bytes_sent;
        g_ctx.subflows[i].bytesRecv = pq.bytes_received;
    }

    // §5a.r5：只在有實際載 video 的 path（吞吐 > 1 Mbps）時，用該路徑 RTT
    // 更新 jitter reorder timeout；否則保留前值（從未設定時 quicJitterInsert
    // fallback 到 10ms floor）。避免閒置 standby 的高 RTT 污染 reorder 等待。
    if (maxThroughputMbps > 1.0f) {
        g_ctx.jitterReorderRttUs = (uint32_t)(videoPathRttMs * 1000.0f);
    }

    // §Q-PERF.6: reorderPercent 從未被寫入（overlay 恆顯示 0）。reorder
    // 是 flow 級訊號（jitter buffer 量到的），無法歸因單一 path——掛在
    // 實際載 video 的路徑上（其餘 path 維持 0），用 delta 算瞬時值。
    {
        static uint64_t prevReordered = 0, prevDelivered = 0;
        QUIC_JITTER_BUF* vjb = &g_jitterBufs[QUIC_FLOW_VIDEO];
        uint64_t dReordered = vjb->reordered - prevReordered;
        uint64_t dDelivered = vjb->delivered - prevDelivered;
        prevReordered = vjb->reordered;
        prevDelivered = vjb->delivered;
        if (maxThroughputMbps > 1.0f && dDelivered > 0) {
            float rp = (float)((double)dReordered / (double)dDelivered * 100.0);
            for (int i = 0; i < g_ctx.subflowCount; i++) {
                bool isVideoPath = (g_ctx.subflows[i].active &&
                                    !g_ctx.subflows[i].picoquicDeleted &&
                                    g_ctx.subflows[i].throughputMbps >= maxThroughputMbps);
                g_ctx.subflows[i].reorderPercent = isVideoPath ? rp : 0.0f;
            }
        }
    }
}

// ── Path health monitoring ──────────────────────────────────
// Detect dead paths via increasing RTT or missing responses

// §Q-STANDBY-REGAIN: 介面優先等級（越小越優先載視訊）。
// 沿用 §Q-VPN-LAST 哲學：實體有線 > WiFi > 其他 > VPN(overlay)。
static int quicIfTypeRank(int type) {
    switch (type) {
        case LC_NETIF_TYPE_ETHERNET: return 0;
        case LC_NETIF_TYPE_WIFI:     return 1;
        case LC_NETIF_TYPE_VPN:      return 3;
        default:                     return 2;  // cellular/other
    }
}

// §Q-STANDBY-REGAIN: 剛 regain 的介面在 flap 視窗內又死亡 → 退避計數，
// 下次駐留時間加倍，防止不穩介面造成 regain/failover ping-pong。
// §Q-REGAIN-BREAKER：達 REGAIN_FLAP_MAX → 斷路 5 分鐘（REGAIN 與
// INACTIVE-REVIVE 都停），防止「ICMP 通但 QUIC 不通」的路徑被反覆
// 重升造成 server 端 PATH-SWITCH + IDR 風暴（round 4 實測）。
static void quicNoteRegainFlap(int ifIdx, uint64_t now) {
    if (g_ctx.lastRegainIf >= 0 && ifIdx == g_ctx.lastRegainIf &&
        g_ctx.lastRegainTime > 0 &&
        (now - g_ctx.lastRegainTime) <
            (uint64_t)REGAIN_FLAP_WINDOW_S * 1000000 &&
        g_ctx.regainFlapCount < REGAIN_FLAP_MAX) {
        g_ctx.regainFlapCount++;
        Limelog("[VIPLE-MPQUIC] §Q-STANDBY-REGAIN: if %d died %.1fs after "
                "regain — flap backoff %d (next dwell %ds)\n",
                ifIdx, (float)(now - g_ctx.lastRegainTime) / 1e6f,
                g_ctx.regainFlapCount,
                REGAIN_HEALTH_S << g_ctx.regainFlapCount);

        // §Q-BREAKER-ESC 2026-07-06：曾跳閘過的同一介面，單次 flap 即再
        // 斷路（期滿後不必再付 3 次停格才重新斷路）；暫停時長 300s×4^(n-1)
        // 上限 1800s。原本 300s 到期 flapCount 歸零，同一顆壞介面每 5-6
        // 分鐘重演「3 次 flap = 3 次停格 → 斷路」無限循環。
        {
            bool trip = (g_ctx.regainFlapCount >= REGAIN_FLAP_MAX) ||
                        (g_ctx.breakerTripCount > 0 &&
                         ifIdx == g_ctx.lastBreakerIf);
            bool suspended = (g_ctx.regainSuspendedUntil != 0 &&
                              now < g_ctx.regainSuspendedUntil);
            if (trip && !suspended) {
                g_ctx.breakerTripCount =
                    (ifIdx == g_ctx.lastBreakerIf)
                        ? g_ctx.breakerTripCount + 1 : 1;
                g_ctx.lastBreakerIf = ifIdx;
                g_ctx.lastBreakerTripTime = now;
                uint64_t suspendS = 300;
                for (int t = 1; t < g_ctx.breakerTripCount && suspendS < 1800; t++)
                    suspendS *= 4;
                if (suspendS > 1800) suspendS = 1800;
                g_ctx.regainSuspendedUntil = now + suspendS * 1000000ull;
                Limelog("[VIPLE-MPQUIC] §Q-REGAIN-BREAKER: if %d flapped "
                        "(flapCount=%d, trip #%d) — suspending regain/revive "
                        "for %llus (staying on current path)\n",
                        ifIdx, g_ctx.regainFlapCount, g_ctx.breakerTripCount,
                        (unsigned long long)suspendS);
            }
        }
    }
}

// §Q-STANDBY-REGAIN 2026-07-02: standby 路徑的自動回歸評估。
// 前提：呼叫端已確認 sf->keepAsStandby、sf->active（2026-07-06 起含
// slot 0——復活統一走 standby 之後 primary 也可能以 standby 身份回歸），
// 且存在至少一條健康 available 參考路徑（anyAliveRef）——即「failover
// 後恢復的路徑躺在 standby、次佳路徑載著視訊」的情境。
// 觸發條件（全部成立才動作）：
//   1. 健康駐留期滿（regainEligibleAt；flap 退避 ×2^regainFlapCount）
//   2. 所有健康 available 路徑的介面 rank 都嚴格比 sf 差
//      （同級或更優者存在 → 不動，避免同級雙網卡 ping-pong）
// 動作：升 sf 為 available、把 rank 較差的健康 available 路徑降回
// standby、清 failoverPromotedSlot（failover 結束，preferred path 回歸）。
// server 端收到 PATH_BACKUP frame 後（picoquic frames.c 無條件翻轉
// path_is_backup），§Q-PATH-SWITCH-STICKY 的健康檢查會判當前路徑
// backup=dead 而放行切換 → 視訊回到本路徑。單邊部署（新 client +
// 舊 server）即有效。
static void quicMaybeRegainStandby(QUIC_SUBFLOW* sf, int slot, uint64_t now) {
    // §Q-REGAIN-BREAKER：斷路中不評估
    if (g_ctx.regainSuspendedUntil != 0) {
        if (now < g_ctx.regainSuspendedUntil)
            return;
        g_ctx.regainSuspendedUntil = 0;   // 斷路期滿，重新開放
        g_ctx.regainFlapCount = 0;
        // §Q-BREAKER-ESC：breakerTripCount 刻意不歸零——期滿後同介面
        // 單次 flap 即再斷路，且暫停時長遞增（見 quicNoteRegainFlap）。
    }

    // flap 計數在最近一次 regain 存活滿視窗後歸零
    if (g_ctx.regainFlapCount > 0 && g_ctx.lastRegainTime > 0 &&
        (now - g_ctx.lastRegainTime) >
            (uint64_t)REGAIN_FLAP_WINDOW_S * 1000000) {
        g_ctx.regainFlapCount = 0;
    }

    // §Q-BREAKER-ESC forgiveness：斷路「之後」真的有一次 regain 且存活
    // 滿 flap 視窗才全赦。沒有 lastRegainTime > lastBreakerTripTime 這個
    // 條件，斷路期間沒有任何 regain、lastRegainTime 陳舊，會被視窗條件
    // 誤當「存活滿視窗」而立刻大赦。
    if (g_ctx.breakerTripCount > 0 && g_ctx.lastRegainTime > 0 &&
        g_ctx.lastRegainTime > g_ctx.lastBreakerTripTime &&
        (now - g_ctx.lastRegainTime) >
            (uint64_t)REGAIN_FLAP_WINDOW_S * 1000000) {
        Limelog("[VIPLE-MPQUIC] §Q-BREAKER-ESC: if %d survived full flap "
                "window after breaker — forgiving trip history\n",
                g_ctx.lastBreakerIf);
        g_ctx.breakerTripCount = 0;
        g_ctx.lastBreakerIf = -1;
    }

    // 駐留武裝：進 standby 後第一次評估時設定（涵蓋 path-recovery 之外
    // 的 standby 來源，例如啟動時的 §Q-MP-STANDBY）。
    if (sf->regainEligibleAt == 0) {
        int backoff = g_ctx.regainFlapCount;
        // §Q-BREAKER-ESC：斷路期滿後的第一次嘗試直接用最大 dwell（80s）
        if (g_ctx.breakerTripCount > 0) backoff = REGAIN_FLAP_MAX;
        if (backoff > REGAIN_FLAP_MAX) backoff = REGAIN_FLAP_MAX;
        sf->regainEligibleAt = now +
            ((uint64_t)REGAIN_HEALTH_S << backoff) * 1000000;
        return;
    }
    if (now < sf->regainEligibleAt)
        return;

    // liveness（review 補強）：standby 路徑無應用流量、health check 跳過
    // stall 偵測，駐留計時器本身無法證明路徑活著——介面在駐留期內第二次
    // 斷線不會留下任何痕跡。要求 recovery thread 的 ICMP + 介面 up 檢查
    // 蓋章新鮮（掃描週期 10s，容忍漏一輪 → 25s），否則不升級，避免把死
    // 路徑升回 available 又把健康路徑降掉。
    if (sf->lastStandbyProbeOk == 0 ||
        (now - sf->lastStandbyProbeOk) > 25000000)
        return;

    // 所有健康 available 路徑的 rank 必須嚴格比 sf 差，且至少有一條
    // （全滅情境交給既有 §Q-DYN-STANDBY-ALIVE / §Q-MP-FAILOVER）。
    // 健康定義與呼叫端 anyAliveRef 一致：active、非 standby、未刪除、
    // 無連續 timeout、lastRecvTime 在 stall 門檻內。
    int myRank = quicIfTypeRank(sf->type);
    int worseRefs = 0;
    for (int j = 0; j < g_ctx.subflowCount; j++) {
        QUIC_SUBFLOW* ref = &g_ctx.subflows[j];
        if (ref == sf || !ref->active || ref->keepAsStandby ||
            ref->picoquicDeleted)
            continue;
        if (ref->consecutiveTimeouts > 0 || ref->lastRecvTime == 0)
            continue;   // 不健康的 available 路徑不擋 regain，也不算
                        // worseRefs——死路徑交給 failover 邏輯處理
        uint64_t refStall = (uint64_t)(ref->rttMs * 5000.0f);
        if (refStall < 3000000) refStall = 3000000;
        if ((now - ref->lastRecvTime) >= refStall)
            continue;
        if (quicIfTypeRank(ref->type) <= myRank)
            return;     // 存在同級或更優的健康 available 路徑 → 不動
        worseRefs++;
    }
    if (worseRefs == 0)
        return;

    // 升級自己回 available
    sf->keepAsStandby = false;
    sf->recoveryStandbyUntil = 0;
    sf->regainEligibleAt = 0;
    quicResetSubflowHealthOnPromote(sf, now);
    quicSetPathStatusTracked(sf, picoquic_path_status_available);

    // 把 rank 較差的健康 available 路徑降回 standby
    int demoted = 0;
    for (int j = 0; j < g_ctx.subflowCount; j++) {
        QUIC_SUBFLOW* ref = &g_ctx.subflows[j];
        if (ref == sf || !ref->active || ref->keepAsStandby ||
            ref->picoquicDeleted)
            continue;
        if (quicIfTypeRank(ref->type) > myRank) {
            ref->keepAsStandby = true;
            ref->regainEligibleAt = 0;
            ref->lastStandbyProbeOk = 0;  // 新 standby episode，章重蓋
            quicSetPathStatusTracked(ref, picoquic_path_status_backup);
            demoted++;
        }
    }

    // failover 結束：同時解除 quicIsFailoverActive() 的 200ms jitter
    // timeout 鎖與 Fix L 的 ENet 繞道
    g_ctx.failoverPromotedSlot = -1;
    g_ctx.lastRegainTime = now;
    g_ctx.lastRegainIf = sf->interfaceIndex;

    Limelog("[VIPLE-MPQUIC] §Q-STANDBY-REGAIN: subflow %d (if %d, slot %d, "
            "%s, RTT=%.1fms) promoted after healthy dwell — demoted %d "
            "lower-rank path(s), failover cleared (flapCount=%d)\n",
            sf->id, sf->interfaceIndex, slot,
            lcNetIfTypeName(sf->type),
            (sf->rttMs > 0) ? sf->rttMs : (float)sf->icmpRttMs,
            demoted, g_ctx.regainFlapCount);

    if (g_ctx.failoverCallback) {
        g_ctx.failoverCallback(sf->id, sf->interfaceIndex,
                               true, g_ctx.failoverContext);
    }
}

static void quicCheckPathHealth(void) {
    uint64_t now;

    if (!g_ctx.cnx)
        return;

    now = picoquic_current_time();
    if (now - g_ctx.lastHealthCheck < HEALTH_CHECK_INTERVAL_US)
        return;
    g_ctx.lastHealthCheck = now;

    // §Q-INPUT-DIAG: 每 5 秒彙總 QUIC input datagram 統計
    {
        static uint64_t lastInputDiag = 0;
        if (now - lastInputDiag > 5000000 &&
            (g_quicInputQueued > 0 || g_quicInputFailed > 0)) {
            int q = g_quicInputQueued;
            int f = g_quicInputFailed;
            g_quicInputQueued = 0;
            g_quicInputFailed = 0;
            Limelog("[VIPLE-MPQUIC] §Q-INPUT-DIAG: QUIC input queued=%d "
                    "failed=%d (last 5s)\n", q, f);
            lastInputDiag = now;
        } else if (now - lastInputDiag > 5000000) {
            lastInputDiag = now;
        }
    }

    for (int i = 0; i < g_ctx.subflowCount; i++) {
        QUIC_SUBFLOW* sf = &g_ctx.subflows[i];

        // 被 picoquic 永久刪除的 slot 不需要 health check
        if (sf->picoquicDeleted)
            continue;

        // §Q-MP-STANDBY: standby 路徑故意不接收應用資料，
        // 不做 stall 偵測（否則會被誤判為 inactive）。
        // 只要 picoquic 沒刪它，它就是活的。
        //
        // §Q-DYN-STANDBY-ALIVE 2026-05-27: 但要先檢查所有非 standby
        // 參考路徑是否都已死亡。若是，此 standby 路徑是唯一存活者，
        // 必須 promote 為 available，否則 server 無法送 video 到它。
        //
        // §Q-VPN-LAST 2026-05-27 (v1.5.171 實測修正)：VPN 路徑（Tailscale
        // 等）物理上常依賴主介面的網路堆疊。主介面死掉時，VPN 跟著失效，
        // 直到 VPN daemon 切到 WiFi 才恢復。所以 promote 順序：
        //   1. 先找非 VPN 的 standby（WiFi 等）
        //   2. 都沒有才促升 VPN standby
        // 若此 path 是 VPN，先掃描有沒有非 VPN 的 standby 候選，有 → skip。
        if (sf->keepAsStandby) {
            // §Q-REVIVE-STANDBY (Fix A4): 原本排除 slot 0（i > 0）——但復活
            // 統一走 standby 之後，slot 0 也可能以 standby 身份存在，全滅
            // 時必須能被促升，否則 last-resort 復活的 primary 永遠閒置。
            if (sf->active) {
                bool anyAliveRef = false;
                for (int j = 0; j < g_ctx.subflowCount; j++) {
                    QUIC_SUBFLOW* ref = &g_ctx.subflows[j];
                    if (j != i && ref->active && !ref->keepAsStandby &&
                        !ref->picoquicDeleted &&
                        ref->consecutiveTimeouts == 0 &&
                        ref->lastRecvTime > 0) {
                        // Fix M v1.5.194: 門檻對齊 stall detection 的
                        // stallThreshold（max(3s, 5×RTT)），而非 hardcoded
                        // 2 秒。2 秒在 LAN 上太激進，正常 jitter 就會觸發
                        // 假性 failover。
                        uint64_t refStallThreshold = (uint64_t)(ref->rttMs * 5000.0f);
                        if (refStallThreshold < 3000000) refStallThreshold = 3000000;
                        if ((now - ref->lastRecvTime) < refStallThreshold) {
                            anyAliveRef = true;
                            break;
                        }
                    }
                }
                // §Q-VPN-LAST：若此 path 是 VPN，先看有沒有非 VPN standby
                bool deferToNonVpn = false;
                if (!anyAliveRef && sf->type == LC_NETIF_TYPE_VPN) {
                    for (int j = 0; j < g_ctx.subflowCount; j++) {
                        QUIC_SUBFLOW* alt = &g_ctx.subflows[j];
                        if (j != i && alt->active && alt->keepAsStandby &&
                            !alt->picoquicDeleted &&
                            alt->type != LC_NETIF_TYPE_VPN) {
                            deferToNonVpn = true;
                            break;
                        }
                    }
                }
                if (!anyAliveRef && !deferToNonVpn) {
                    // 所有參考路徑都死了 → promote
                    sf->keepAsStandby = false;
                    sf->recoveryStandbyUntil = 0;
                    quicResetSubflowHealthOnPromote(sf, now);
                    quicSetPathStatusTracked(sf, picoquic_path_status_available);
                    // §Q-REVIVE-STANDBY (Fix A4): slot 0 本身是 preferred
                    // primary——促升自己不算 failover（設 0 會讓
                    // §Q-FALSE-POSITIVE-FAILBACK 與 quicIsFailoverActive()
                    // 語意錯亂）。
                    if (i > 0)
                        g_ctx.failoverPromotedSlot = i;
                    Limelog("[VIPLE-MPQUIC] §Q-DYN-STANDBY-ALIVE: subflow %d "
                            "(if %d, slot %d, %s) → active (no valid reference "
                            "path — all others dead/stale, RTT=%.1fms)\n",
                            sf->id, sf->interfaceIndex, i,
                            (sf->type == LC_NETIF_TYPE_VPN) ? "VPN-fallback" : "non-VPN",
                            sf->rttMs);
                    // 不 continue — fall through 到正常 health check
                } else {
                    // §Q-STANDBY-REGAIN 2026-07-02: 有健康 available 參考
                    // 路徑存在時，standby 路徑原本到此直接 continue——
                    // 導致 failover 後恢復的 preferred path 永遠躺在
                    // standby（§Q-INTERFACE-FAILBACK 要求 !keepAsStandby、
                    // DYN-STANDBY 的 promote 分支也到不了）。2026-07-01
                    // 事故：視訊因此在高 loss VPN 上困 86 分鐘。
                    quicMaybeRegainStandby(sf, i, now);
                    continue;
                }
            } else {
                // §Q-REVIVE-STANDBY (Fix A5): inactive 的 standby 路徑不再
                // continue——fall through 到下面的 re-activate 區塊
                // （lastRecvTime 新鮮時復活，維持 standby 身份）。
                // 其餘區塊皆以 sf->active 為閘，inactive 路徑不受影響。
            }
        }

        // Check if path has stalled (no data received for > 5x RTT or 3 seconds)
        uint64_t stallThreshold = (uint64_t)(sf->rttMs * 5000.0f);
        if (stallThreshold < 3000000) stallThreshold = 3000000; // min 3s

        bool stalled = sf->active && sf->lastRecvTime > 0 &&
                       (now - sf->lastRecvTime) > stallThreshold;

        // §Q-LOSS-DEMOTE (2026-07-06 凍結事故)：zombie 準則。stall 偵測
        // 只看 lastRecvTime，而 IO 迴圈收到任一 UDP 封包（含 ACK/控制面
        // 零星封包）就刷新它——「ICMP 通、UDP 大流量死」的半死鏈路永遠
        // 不 INACTIVE（事故中 slot 0 卡 ACTIVE loss=100% 十二分鐘）。
        // loss 持續越檻（hold 4s；促升後 probation 內 2s）且存在替代路徑
        // （健康 available 或 probe 章新鮮的 standby——後者必含，否則
        // 「zombie primary + 只剩 standby」的事故場景永遠不降級）→ 走
        // 同一個 INACTIVE 序列。無替代路徑不降（爛路徑也比沒路徑好）。
        bool lossCritical = false;
        if (sf->active && !stalled && sf->lossBadSinceUs != 0) {
            uint64_t hold = LOSS_DEMOTE_HOLD_US;
            if (sf->lastPromotedAtUs != 0 &&
                (now - sf->lastPromotedAtUs) < LOSS_PROBATION_US)
                hold /= 2;
            if ((now - sf->lossBadSinceUs) >= hold) {
                for (int j = 0; j < g_ctx.subflowCount; j++) {
                    QUIC_SUBFLOW* alt = &g_ctx.subflows[j];
                    if (j == i || alt->picoquicDeleted || !alt->active)
                        continue;
                    bool healthyAvail = !alt->keepAsStandby &&
                        alt->consecutiveTimeouts == 0 &&
                        alt->lastRecvTime > 0 &&
                        (now - alt->lastRecvTime) < 3000000;
                    bool freshStandby = alt->keepAsStandby &&
                        alt->lastStandbyProbeOk != 0 &&
                        (now - alt->lastStandbyProbeOk) < 25000000;
                    if (healthyAvail || freshStandby) {
                        lossCritical = true;
                        break;
                    }
                }
            }
        }

        if (stalled || lossCritical) {
            sf->consecutiveTimeouts++;
            if (lossCritical && !stalled && sf->consecutiveTimeouts == 1) {
                Limelog("[VIPLE-MPQUIC] §Q-LOSS-DEMOTE: subflow %d (if %d) "
                        "loss=%.1f%% sustained %.1fs with alternative path "
                        "available — demoting\n",
                        sf->id, sf->interfaceIndex, sf->lossPercent,
                        (float)(now - sf->lossBadSinceUs) / 1e6f);
            }

            if (sf->consecutiveTimeouts >= PROBE_TIMEOUT_THRESHOLD) {
                sf->active = false;
                Limelog("[VIPLE-MPQUIC] Subflow %d (if %d) marked INACTIVE "
                        "(%d consecutive timeouts, last recv %.1fs ago)\n",
                        sf->id, sf->interfaceIndex,
                        sf->consecutiveTimeouts,
                        (float)(now - sf->lastRecvTime) / 1e6f);

                // §Q-STANDBY-REGAIN: 剛 regain 的介面又死 → flap 退避
                quicNoteRegainFlap(sf->interfaceIndex, now);

                if (g_ctx.failoverCallback) {
                    g_ctx.failoverCallback(sf->id, sf->interfaceIndex,
                                           false, g_ctx.failoverContext);
                }

                // §Q-MP-FAILBACK: 失效路徑降級為 backup，避免
                // picoquic 繼續往死路徑排程。
                quicSetPathStatusTracked(sf, picoquic_path_status_backup);

                // §Q-REVIVE-STANDBY (Fix A1): 狀態與旗標一致化——上面已把
                // picoquic status 設成 backup，但 keepAsStandby 原本留在
                // false：復活後 §5c guard 每 ~1ms 就把 status 翻回
                // available、§Q-INTERFACE-FAILBACK 也隨即放行，dwell 與
                // cwnd 暖身全被繞過（2026-07-06 三次停格事故根因）。補上
                // 旗標，讓復活統一走 §Q-STANDBY-REGAIN 的 dwell+liveness。
                sf->keepAsStandby = true;
                sf->regainEligibleAt = 0;     // 新 standby episode
                sf->lastStandbyProbeOk = 0;   // 章重蓋
                sf->lossBadSinceUs = 0;       // §Q-LOSS-DEMOTE 計時歸零

                // §Q-MP-FAILOVER: 主路徑失效 → 升級 standby 路徑頂上。
                //
                // §Q-FAILOVER-VPN-LAST 2026-05-27 (使用者洞察)：VPN 路徑
                // （Tailscale 等）物理上常依賴主介面的網路堆疊。主介面
                // 死掉時，VPN 也會跟著失效一段時間直到 VPN daemon 切到
                // 另一個底層介面（WiFi）才恢復。
                //
                // 所以升級優先順序：
                //   1. 非 VPN standby（WiFi 等）裡 RTT 最低的
                //   2. 若沒有非 VPN，才升級 VPN standby

                // §Q-MP-FAILOVER-DEDUP v1.5.196 Fix O：§Q-DYN-STANDBY-ALIVE
                // 可能在前一次 health check 就已 promote 了非 VPN 路徑
                // （例如 WiFi）。此時再 promote 另一條（尤其是 VPN）會
                // 導致 picoquic multipath scheduler 把 datagram 分流到
                // 死路徑上，造成 video frame shard 遺失 → 畫面凍結。
                //
                // 實測：v1.5.195 DYN-STANDBY-ALIVE promote WiFi 後，
                // FAILOVER 又 promote VPN（依賴已死 Ethernet 的 Tailscale）
                // → VPN shard 全丟 → IDR 無法組裝 → 畫面凍結 31 秒。
                bool alreadyHaveBackup = false;
                for (int j = 0; j < g_ctx.subflowCount; j++) {
                    if (j == i) continue;  // 跳過正在失效的 primary
                    QUIC_SUBFLOW* alt = &g_ctx.subflows[j];
                    if (alt->active && !alt->keepAsStandby &&
                        !alt->picoquicDeleted &&
                        alt->consecutiveTimeouts == 0) {
                        alreadyHaveBackup = true;
                        Limelog("[VIPLE-MPQUIC] §Q-MP-FAILOVER-DEDUP: "
                                "skip standby promotion — subflow %d "
                                "(if %d, slot %d) already active and "
                                "available\n",
                                alt->id, alt->interfaceIndex, j);
                        break;
                    }
                }

                int bestJ = -1;
                if (!alreadyHaveBackup) {
                float bestRttMs = 1e9f;
                bool preferNonVpn = true;

                for (int pass = 0; pass < 2 && bestJ < 0; pass++) {
                    for (int j = 0; j < g_ctx.subflowCount; j++) {
                        QUIC_SUBFLOW* candidate = &g_ctx.subflows[j];
                        if (!candidate->keepAsStandby ||
                            !candidate->active ||
                            candidate->picoquicDeleted)
                            continue;
                        bool isVpn = (candidate->type == LC_NETIF_TYPE_VPN);
                        // Pass 0: only non-VPN; Pass 1: VPN-fallback
                        if (preferNonVpn && pass == 0 && isVpn) continue;
                        if (pass == 1 && !isVpn) continue;

                        float effRtt = (candidate->rttMs > 0)
                            ? candidate->rttMs
                            : (float)candidate->icmpRttMs;
                        if (effRtt < bestRttMs) {
                            bestRttMs = effRtt;
                            bestJ = j;
                        }
                    }
                }

                if (bestJ >= 0) {
                    QUIC_SUBFLOW* candidate = &g_ctx.subflows[bestJ];
                    candidate->keepAsStandby = false;
                    candidate->recoveryStandbyUntil = 0; // failover 覆蓋 recovery grace

                    // §Q-MP-FAILOVER-GRACE: 重置 health check 計時器，
                    // 給新升級路徑足夠時間（≥ stallThreshold 3 秒）開始
                    // 接收 server 資料，避免下次 health check 因為
                    // lastRecvTime 來自路徑建立時（~250 秒前）而立即
                    // 判定 INACTIVE。
                    quicResetSubflowHealthOnPromote(candidate, now);

                    quicSetPathStatusTracked(candidate,
                                             picoquic_path_status_available);
                    g_ctx.failoverPromotedSlot = bestJ;
                    Limelog("[VIPLE-MPQUIC] §Q-MP-FAILOVER: promoted "
                            "standby subflow %d (if %d, slot %d, %s, "
                            "RTT=%.1fms) to available (primary if %d "
                            "failed)\n",
                            candidate->id,
                            candidate->interfaceIndex, bestJ,
                            (candidate->type == LC_NETIF_TYPE_VPN)
                                ? "VPN-fallback" : "non-VPN",
                            bestRttMs,
                            sf->interfaceIndex);
                    if (g_ctx.failoverCallback) {
                        g_ctx.failoverCallback(
                            candidate->id,
                            candidate->interfaceIndex,
                            true, g_ctx.failoverContext);
                    }
                }
                } // §Q-MP-FAILOVER-DEDUP: end of !alreadyHaveBackup

                // §Q-MP-SECONDARY-FAILOVER v1.5.196 Fix O：非 primary 的
                // 已 promote 路徑也可能失效（例如 WiFi 在 failover 後停
                // 止接收資料）。若所有 non-standby 路徑都死了，嘗試
                // promote 剩餘的 standby（如果有的話）。
                //
                // 注意：這是安全網。§Q-MP-FAILOVER（上面）只搜尋
                // keepAsStandby 候選；§Q-MP-EMERGENCY-PROMOTE 只在
                // picoquic 刪除 path 時觸發。當 health check 把一條
                // 已 promote 的非 primary 路徑標記 INACTIVE 時，兩者
                // 都不會觸發二次 failover。
                if (i > 0 && bestJ < 0 && !alreadyHaveBackup) {
                    int activeAvailCount = 0;
                    for (int j = 0; j < g_ctx.subflowCount; j++) {
                        QUIC_SUBFLOW* alt = &g_ctx.subflows[j];
                        if (alt->active && !alt->keepAsStandby &&
                            !alt->picoquicDeleted)
                            activeAvailCount++;
                    }
                    if (activeAvailCount == 0) {
                        // 所有 available 路徑都死了 → 嘗試 promote standby
                        int secBestJ = -1;
                        float secBestRtt = 1e9f;
                        for (int pass = 0; pass < 2 && secBestJ < 0; pass++) {
                            for (int j = 0; j < g_ctx.subflowCount; j++) {
                                QUIC_SUBFLOW* cand = &g_ctx.subflows[j];
                                if (!cand->keepAsStandby || !cand->active ||
                                    cand->picoquicDeleted)
                                    continue;
                                bool isVpn = (cand->type == LC_NETIF_TYPE_VPN);
                                if (pass == 0 && isVpn) continue;
                                if (pass == 1 && !isVpn) continue;
                                float eff = (cand->rttMs > 0)
                                    ? cand->rttMs : (float)cand->icmpRttMs;
                                if (eff < secBestRtt) {
                                    secBestRtt = eff;
                                    secBestJ = j;
                                }
                            }
                        }
                        if (secBestJ >= 0) {
                            QUIC_SUBFLOW* cand = &g_ctx.subflows[secBestJ];
                            cand->keepAsStandby = false;
                            cand->recoveryStandbyUntil = 0;
                            quicResetSubflowHealthOnPromote(cand, now);
                            quicSetPathStatusTracked(cand,
                                picoquic_path_status_available);
                            g_ctx.failoverPromotedSlot = secBestJ;
                            Limelog("[VIPLE-MPQUIC] §Q-MP-SECONDARY-"
                                    "FAILOVER: all available paths dead"
                                    " — promoting standby subflow %d "
                                    "(if %d, slot %d, %s, RTT=%.1fms)"
                                    "\n",
                                    cand->id, cand->interfaceIndex,
                                    secBestJ,
                                    (cand->type == LC_NETIF_TYPE_VPN)
                                        ? "VPN" : "non-VPN",
                                    secBestRtt);
                        }
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
            // 找最快的 non-standby active path（通常是 slot 0）
            // §Q-DYN-STANDBY-ALIVE 2026-05-27: 排除已超時或近期無收到
            // 資料的路徑。死亡路徑保留快取的低 RTT（如乙太網路 0.5ms），
            // 會把存活的 WiFi 路徑（5.3ms）誤判為「excess > 3ms」而踢成
            // standby，導致 failover 時唯一存活路徑被停用 → 影像凍結。
            // 加入 consecutiveTimeouts==0 + lastRecvTime 2 秒新鮮度檢查。
            float bestRttMs = -1.0f;
            for (int j = 0; j < g_ctx.subflowCount; j++) {
                QUIC_SUBFLOW* best = &g_ctx.subflows[j];
                if (j != i && best->active && !best->keepAsStandby &&
                    !best->picoquicDeleted && best->rttMs > 0 &&
                    best->consecutiveTimeouts == 0 &&
                    best->lastRecvTime > 0 &&
                    (now - best->lastRecvTime) < 2000000) {
                    if (bestRttMs < 0 || best->rttMs < bestRttMs)
                        bestRttMs = best->rttMs;
                }
            }
            if (bestRttMs > 0) {
                float excess = sf->rttMs - bestRttMs;
                if (!sf->keepAsStandby &&
                    excess > (float)DYN_STANDBY_RTT_EXCESS_MS) {
                    // §Q-DYN-STANDBY-FAILOVER-GUARD v1.5.181: Ethernet primary
                    // dead/stale → 不降級任何 active path。failover 期間所有
                    // active path 都珍貴，即使 RTT 較高也保留冗餘。避免 server
                    // 收到 PATH_STATUS=backup 後失去 fallback path →
                    // §Q-STALE 死亡螺旋 → video 永久停止。
                    int ethernetDead = 0;
                    if (g_ctx.subflowCount > 0 &&
                        g_ctx.subflows[0].type == LC_NETIF_TYPE_ETHERNET &&
                        (!g_ctx.subflows[0].active ||
                         g_ctx.subflows[0].consecutiveTimeouts > 0 ||
                         g_ctx.subflows[0].picoquicDeleted ||
                         (g_ctx.subflows[0].lastRecvTime > 0 &&
                          (now - g_ctx.subflows[0].lastRecvTime) > 2000000))) {
                        ethernetDead = 1;
                    }
                    // Fix N v1.5.195: state-transition log。failover 期間
                    // 每 500ms 重複印同一訊息無意義，改為只在狀態變化時印。
                    {
                        static int prevGuardState = -1;
                        if (ethernetDead != prevGuardState) {
                            if (ethernetDead) {
                                Limelog("[VIPLE-MPQUIC] §Q-DYN-STANDBY-FAILOVER-GUARD: "
                                        "Ethernet primary dead — suppressing non-primary "
                                        "standby demotion (subflow %d, if %d, "
                                        "RTT=%.1fms, best=%.1fms, excess=%.1f)\n",
                                        sf->id, sf->interfaceIndex,
                                        sf->rttMs, bestRttMs, excess);
                            } else {
                                Limelog("[VIPLE-MPQUIC] §Q-DYN-STANDBY-FAILOVER-GUARD: "
                                        "Ethernet primary alive — resuming normal "
                                        "standby demotion\n");
                            }
                            prevGuardState = ethernetDead;
                        }
                    }
                    if (ethernetDead) {
                        // 不降級：保留所有 active path
                    } else {
                    sf->keepAsStandby = true;
                    quicSetPathStatusTracked(sf, picoquic_path_status_backup);
                    Limelog("[VIPLE-MPQUIC] §Q-MP-DYN-STANDBY: subflow %d (if %d) "
                            "→ standby (QUIC RTT=%.1fms, best=%.1fms, "
                            "excess=%.1f > %dms)\n",
                            sf->id, sf->interfaceIndex,
                            sf->rttMs, bestRttMs, excess,
                            DYN_STANDBY_RTT_EXCESS_MS);
                    }
                } else if (sf->keepAsStandby &&
                           excess <= ((float)DYN_STANDBY_RTT_EXCESS_MS / 2.0f)) {
                    // §Q-MP-WIFI-STANDBY guard v1.5.179: WiFi 在 Ethernet primary
                    // 仍活躍時不 promote。WiFi 只在 failover/emergency 時才升級。
                    // 檢查條件：Ethernet primary 活著、非 standby、無連續 timeout。
                    if (sf->type == LC_NETIF_TYPE_WIFI &&
                        g_ctx.subflowCount > 0 &&
                        g_ctx.subflows[0].type == LC_NETIF_TYPE_ETHERNET &&
                        g_ctx.subflows[0].active && !g_ctx.subflows[0].keepAsStandby &&
                        !g_ctx.subflows[0].picoquicDeleted &&
                        g_ctx.subflows[0].consecutiveTimeouts == 0) {
                        // Ethernet primary alive → WiFi 維持 standby，不 promote
                    }
                    // §Q-PATH-RECOVERY-STANDBY: 寬限期內不 promote
                    else if (sf->recoveryStandbyUntil > 0 &&
                        picoquic_current_time() < sf->recoveryStandbyUntil) {
                        // 寬限期未過，維持 standby
                    } else {
                        // RTT 恢復：重新 available（50% 滯後防抖動）
                        sf->keepAsStandby = false;
                        sf->recoveryStandbyUntil = 0;
                        quicSetPathStatusTracked(sf, picoquic_path_status_available);
                        Limelog("[VIPLE-MPQUIC] §Q-MP-DYN-STANDBY: subflow %d (if %d) "
                                "→ active (QUIC RTT=%.1fms, best=%.1fms, "
                                "excess=%.1f recovered)\n",
                                sf->id, sf->interfaceIndex,
                                sf->rttMs, bestRttMs, excess);
                    }
                }
            }

            // §Q-MP-WIFI-STANDBY re-demote v1.5.179: Failover 曾升級 WiFi 為 active，
            // 但 Ethernet primary 已復原 → 把 WiFi 重新降為 standby。
            // 條件：WiFi 目前是 active（keepAsStandby==false）、Ethernet primary 活躍
            // 且最近 2 秒內有收到封包（確認是真的活著不是剛 re-activate 的空殼）。
            if (!sf->keepAsStandby &&
                sf->type == LC_NETIF_TYPE_WIFI &&
                g_ctx.subflowCount > 0 &&
                g_ctx.subflows[0].type == LC_NETIF_TYPE_ETHERNET &&
                g_ctx.subflows[0].active && !g_ctx.subflows[0].keepAsStandby &&
                !g_ctx.subflows[0].picoquicDeleted &&
                g_ctx.subflows[0].consecutiveTimeouts == 0 &&
                g_ctx.subflows[0].lastRecvTime > 0 &&
                (now - g_ctx.subflows[0].lastRecvTime) < 2000000) {
                sf->keepAsStandby = true;
                quicSetPathStatusTracked(sf, picoquic_path_status_backup);
                Limelog("[VIPLE-MPQUIC] §Q-MP-WIFI-STANDBY: subflow %d (if %d) "
                        "→ re-standby (WiFi, Ethernet primary recovered)\n",
                        sf->id, sf->interfaceIndex);
            }

            // §Q-VPN-REDEMOTE 2026-07-02: §Q-MP-WIFI-STANDBY re-demote 的
            // 一般化。failover 曾把 VPN 升為 available，但健康的非 VPN
            // available 路徑已存在——可能在任意 slot（2026-07-01 事故中
            // 乙太網路恢復在新 slot，只看 slot 0 的規則管不到）→ 把 VPN
            // 降回 standby，恢復「VPN 只當備援」不變式。這是
            // §Q-STANDBY-REGAIN 的 safety net。
            if (!sf->keepAsStandby && sf->type == LC_NETIF_TYPE_VPN) {
                for (int j = 0; j < g_ctx.subflowCount; j++) {
                    QUIC_SUBFLOW* ref = &g_ctx.subflows[j];
                    if (j != i && ref->active && !ref->keepAsStandby &&
                        !ref->picoquicDeleted &&
                        ref->type != LC_NETIF_TYPE_VPN &&
                        ref->consecutiveTimeouts == 0 &&
                        ref->lastRecvTime > 0 &&
                        (now - ref->lastRecvTime) < 2000000) {
                        sf->keepAsStandby = true;
                        sf->regainEligibleAt = 0;
                        sf->lastStandbyProbeOk = 0;  // 新 standby episode
                        quicSetPathStatusTracked(sf, picoquic_path_status_backup);
                        if (g_ctx.failoverPromotedSlot == i)
                            g_ctx.failoverPromotedSlot = -1;
                        Limelog("[VIPLE-MPQUIC] §Q-VPN-REDEMOTE: subflow %d "
                                "(if %d) → re-standby (VPN, healthy non-VPN "
                                "path if %d available)\n",
                                sf->id, sf->interfaceIndex,
                                ref->interfaceIndex);
                        break;
                    }
                }
            }
        }

        // Re-activate a previously dead path if picoquic reports it alive.
        // §Q-REVIVE-STANDBY (Fix A5): 復活一律以 standby 身份回歸。原
        // §Q-MP-FAILBACK「slot 0 復活即立刻降級 failover 路徑、自己升回
        // available」已移除——那條路完全不查 dwell/breaker/cwnd 暖身，
        // 半死鏈路（ICMP 通、UDP 大量丟包）每 ~10s 就被切回一次，每次
        // 3 秒視訊斷糧（2026-07-06 三次停格事故）。回歸統一由
        // quicMaybeRegainStandby 的 dwell + liveness 決定。這裡同時是
        // 「inactive 但旗標為 false」漏網來源（如 IO 迴圈封包驅動復活）
        // 的防禦網。
        if (!sf->active && sf->lastRecvTime > 0 &&
            (now - sf->lastRecvTime) < stallThreshold) {
            sf->active = true;
            sf->consecutiveTimeouts = 0;
            if (!sf->keepAsStandby) {
                sf->keepAsStandby = true;
                sf->regainEligibleAt = 0;
                sf->lastStandbyProbeOk = 0;
            }
            quicSetPathStatusTracked(sf, picoquic_path_status_backup);
            Limelog("[VIPLE-MPQUIC] Subflow %d (if %d) re-activated into "
                    "standby (promotion gated by REGAIN)\n",
                    sf->id, sf->interfaceIndex);

            if (g_ctx.failoverCallback) {
                g_ctx.failoverCallback(sf->id, sf->interfaceIndex,
                                       true, g_ctx.failoverContext);
            }
        }

        // §Q-FALSE-POSITIVE-FAILBACK v1.5.194 Fix M：
        // DYN-STANDBY-ALIVE 可能在 primary 短暫 stale 時觸發假性
        // failover（設定 failoverPromotedSlot），但 primary 實際上仍然
        // 健康（active、無 timeout、lastRecvTime 新鮮）。此時 §Q-MP-FAILBACK
        // 不會觸發（因為 slot 0 從未變成 !active），failover 狀態永遠
        // 不清除 → Fix L 永久繞過正常 ENet。
        //
        // 修正：在 slot 0 的 health check 中，若 failoverPromotedSlot 已
        // 設定但 slot 0 仍然健康，就清除 failover 狀態並還原 promoted
        // 路徑為 standby。
        if (i == 0 && g_ctx.failoverPromotedSlot >= 0 &&
            sf->active && !sf->keepAsStandby &&
            sf->consecutiveTimeouts == 0 &&
            sf->lastRecvTime > 0 &&
            (now - sf->lastRecvTime) < stallThreshold) {
            int fslot = g_ctx.failoverPromotedSlot;
            QUIC_SUBFLOW* promoted = &g_ctx.subflows[fslot];
            if (!promoted->picoquicDeleted) {
                promoted->keepAsStandby = true;
                quicSetPathStatusTracked(promoted, picoquic_path_status_backup);
                Limelog("[VIPLE-MPQUIC] §Q-FALSE-POSITIVE-FAILBACK: primary "
                        "if %d still healthy (RTT=%.1fms, lastRecv %.1fs ago"
                        "), demoting failover subflow %d (if %d, slot %d) "
                        "back to backup\n",
                        sf->interfaceIndex, sf->rttMs,
                        (float)(now - sf->lastRecvTime) / 1e6f,
                        promoted->id, promoted->interfaceIndex, fslot);
            }
            g_ctx.failoverPromotedSlot = -1;
        }

        // §Q-INTERFACE-FAILBACK（v1.5.195 Fix N）已於 2026-07-06 移除：
        // 它在 revive 的介面匹配 primary 時「立刻」清 failover 並促升，
        // 不查 dwell/breaker/暖身——正是三次停格事故的 failback 引擎。
        // 其正當情境（primary 介面在新 slot 恢復）已由 path-recovery →
        // §Q-PATH-RECOVERY-STANDBY → §Q-STANDBY-REGAIN 完整覆蓋。
    }

    // §Q-FAILOVER-SETTLED (Fix A6)：failover 承載者連續健康 30 秒 →
    // 宣告新常態（failoverPromotedSlot=-1，不切路徑）。解決兩個殘留：
    // 1) 同 rank 情境 REGAIN 刻意不動（防 ping-pong）→ failover 旗標
    //    永掛，200ms jitter relax 與 Fix L ENet 繞道永久生效；
    // 2) dwell/breaker 期間 failover 狀態掛著的持續時間上界。
    // 復活的同級路徑留在 standby 當備援。
    if (g_ctx.failoverPromotedSlot >= 0 &&
        g_ctx.failoverPromotedSlot < g_ctx.subflowCount) {
        QUIC_SUBFLOW* pf = &g_ctx.subflows[g_ctx.failoverPromotedSlot];
        uint64_t pfStall = (uint64_t)(pf->rttMs * 5000.0f);
        if (pfStall < 3000000) pfStall = 3000000;
        bool pfHealthy = pf->active && !pf->keepAsStandby &&
                         !pf->picoquicDeleted &&
                         pf->consecutiveTimeouts == 0 &&
                         pf->lastRecvTime > 0 &&
                         (now - pf->lastRecvTime) < pfStall;
        if (!pfHealthy) {
            g_ctx.failoverHealthySinceUs = 0;
        } else if (g_ctx.failoverHealthySinceUs == 0) {
            g_ctx.failoverHealthySinceUs = now;
        } else if (now - g_ctx.failoverHealthySinceUs > 30000000) {
            Limelog("[VIPLE-MPQUIC] §Q-FAILOVER-SETTLED: promoted subflow "
                    "%d (if %d, slot %d) healthy for 30s — declaring new "
                    "normal, failover state cleared (no path switch)\n",
                    pf->id, pf->interfaceIndex, g_ctx.failoverPromotedSlot);
            g_ctx.failoverPromotedSlot = -1;
            g_ctx.failoverHealthySinceUs = 0;
        }
    } else {
        g_ctx.failoverHealthySinceUs = 0;
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

// §Q-PERF.5: quicRecheckPaths 已移除——它每 30 秒在 picoquicMutex 鎖內
// 做整套 lcEnumNetInterfaces（GetAdaptersAddresses，可阻塞數 ms~數十 ms），
// 造成週期性輸入/收包停滯；其介面異動診斷功能已由 recovery thread
//（quicRecoveryThreadProc，每 10s、鎖外列舉 + ICMP 探測 + 自帶日誌）取代。

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

    // §Q-REJECT-CACHE-TTL 2026-07-02（failover_regain_test round 3 補洞）：
    // rejected 快取原本只在「介面集合變動」時清空——防火牆級/交換器級
    // 瞬斷不改變介面集合，block 期間被 ICMP 拒絕的介面（乙太網路）在
    // 恢復後永遠不再重試，subflow 永遠不重建（事故 bug 類變體 #3：
    // 探測時機恰好落在斷線視窗內就永久卡死）。給快取加 30 秒 TTL：
    // 到期整批清空重試。最壞成本 = 每個 rejected 介面每 30s 一次
    // 200ms ICMP（在 recovery thread 上，不影響 IO thread）。
    {
        static uint64_t lastRejectPurge = 0;
        uint64_t nowR = picoquic_current_time();
        if (*rejectedCount == 0) {
            lastRejectPurge = nowR;  // 快取空 → 滑動基準
        } else if (lastRejectPurge != 0 &&
                   (nowR - lastRejectPurge) > 30000000) {
            Limelog("[VIPLE-MPQUIC] §Q-REJECT-CACHE-TTL: purging %d rejected "
                    "interface(s) for periodic re-probe\n", *rejectedCount);
            *rejectedCount = 0;
            lastRejectPurge = nowR;
        }
    }

    // §Q-STANDBY-REGAIN liveness 掃描 2026-07-02（review 補強）：
    // standby 路徑無應用流量、health check 跳過 stall 偵測——REGAIN 的
    // 駐留計時器本身無法證明路徑活著（介面在駐留期內第二次斷線不會留
    // 下任何痕跡）。recovery thread 可以安全做 200ms ICMP：對每個武裝
    // 中的 regain 候選做「介面仍在且 up + ICMP 可達」檢查，成功才蓋
    // lastStandbyProbeOk 章（REGAIN 要求 <25s 新鮮）；失敗則清章 + 駐留
    // 重新武裝，死路徑永遠拿不到新鮮章、不會被升級。
    //
    // §Q-INACTIVE-REVIVE 2026-07-02（failover_regain_test 實測補洞）：
    // 同 bug 類的第二個變體——路徑被 health check 標 INACTIVE 但 picoquic
    // 沒刪它（例如防火牆級瞬斷，RTO 還沒殺滿）：slot 卡在 inactive+
    // backup，復活條件是「收到封包」，但 backup 路徑上 server 不會送任
    // 何東西 → 永遠不復活（recovery thread 的 alreadyActive 檢查也會跳
    // 過同介面未刪除的 slot）。這裡對 inactive 未刪除的 subflow（含
    // slot 0）做同一套 ICMP 檢查。
    //
    // §Q-REVIVE-STANDBY 2026-07-06（三次停格+凍結事故）：復活方式改為
    // 「直接以 standby 身份復活」。原本只刷 lastRecvTime 讓 health check
    // 的 re-activate → §Q-INTERFACE-FAILBACK 鏈接手——那條鏈完全不查
    // dwell/breaker/cwnd 暖身，「ICMP 通、UDP 大量丟包」的半死鏈路每
    // ~10s 就被切回 primary 一次、3 秒斷糧後又死，dwell 20s/40s 形同虛設。
    // 改為 standby 復活後，促升唯一出口是 quicMaybeRegainStandby
    // （dwell + liveness + rank）；全滅 last-resort 時單輪 ICMP OK 即
    // 復活並交給 §Q-DYN-STANDBY-ALIVE 立即促升（無 dwell）。
    for (int s = 0; s < g_ctx.subflowCount; s++) {
        QUIC_SUBFLOW* sf = &g_ctx.subflows[s];
        if (sf->picoquicDeleted)
            continue;
        bool armedStandby = sf->active && sf->keepAsStandby &&
                            sf->regainEligibleAt != 0;
        bool inactiveRevivable = !sf->active;  // §Q-INACTIVE-REVIVE
        if (!armedStandby && !inactiveRevivable)
            continue;

        bool ifUp = false;
        for (int i = 0; i < ifCount; i++) {
            if (interfaces[i].index == sf->interfaceIndex && interfaces[i].up) {
                ifUp = true;
                break;
            }
        }
        unsigned int rtt = 0;
        if (ifUp) {
            rtt = quicIcmpProbeReachable(&sf->localAddr, &sf->peerUsed, 200);
        }

        uint64_t nowP = picoquic_current_time();
        PltLockMutex(&g_ctx.picoquicMutex);
        if (ifUp && rtt > 0) {
            uint64_t prevOk = sf->lastStandbyProbeOk;
            sf->lastStandbyProbeOk = nowP;

            // §Q-REVIVE-STANDBY last-resort (Fix A3)：若不存在任何健康的
            // available 路徑（全滅），跳過「連續兩輪 OK」與斷路器——
            // 爛路徑也比沒路徑好。促升交給 ≤500ms 後 health check 的
            // §Q-DYN-STANDBY-ALIVE 全滅分支（無 dwell）。
            bool anyAliveRef = false;
            for (int j = 0; j < g_ctx.subflowCount; j++) {
                QUIC_SUBFLOW* ref = &g_ctx.subflows[j];
                if (j != s && ref->active && !ref->keepAsStandby &&
                    !ref->picoquicDeleted &&
                    ref->consecutiveTimeouts == 0 &&
                    ref->lastRecvTime > 0) {
                    uint64_t refStall = (uint64_t)(ref->rttMs * 5000.0f);
                    if (refStall < 3000000) refStall = 3000000;
                    if ((nowP - ref->lastRecvTime) < refStall) {
                        anyAliveRef = true;
                        break;
                    }
                }
            }

            if (inactiveRevivable &&
                (!anyAliveRef ||
                 (prevOk != 0 && (nowP - prevOk) < 15000000 &&
                  // §Q-REGAIN-BREAKER：斷路中不 revive（ICMP 通不代表
                  // QUIC 通；round 4 實測反覆 revive 造成 IDR 風暴）
                  (g_ctx.regainSuspendedUntil == 0 ||
                   nowP >= g_ctx.regainSuspendedUntil)))) {
                // §Q-REVIVE-STANDBY (Fix A2)：以 standby 身份復活。
                sf->active = true;
                sf->keepAsStandby = true;
                sf->consecutiveTimeouts = 0;
                sf->lastRecvTime = nowP;
                sf->regainEligibleAt = 0;  // REGAIN 第一次評估時武裝 dwell
                if (g_ctx.cnx) {
                    quicSetPathStatusTracked(sf, picoquic_path_status_backup);
                }
                // revive 也計入 flap 帳（若它稍後又死，quicNoteRegainFlap
                // 才有基準；純 revive 循環也要能觸發斷路器）
                g_ctx.lastRegainTime = nowP;
                g_ctx.lastRegainIf = sf->interfaceIndex;
                Limelog("[VIPLE-MPQUIC] §Q-REVIVE-STANDBY: subflow %d "
                        "(if %d, slot %d) ICMP OK %s (RTT=%ums) — revived "
                        "into standby, promotion gated by REGAIN\n",
                        sf->id, sf->interfaceIndex, s,
                        anyAliveRef ? "twice" : "once (no alive ref — last resort)",
                        rtt);
            }
        } else {
            sf->lastStandbyProbeOk = 0;
            if (armedStandby) {
                sf->regainEligibleAt = nowP +
                    (uint64_t)REGAIN_HEALTH_S * 1000000;  // 健康觀察歸零重來
                Limelog("[VIPLE-MPQUIC] §Q-STANDBY-REGAIN: liveness probe "
                        "failed for subflow %d (if %d, up=%d) — dwell "
                        "re-armed\n",
                        sf->id, sf->interfaceIndex, ifUp ? 1 : 0);
            }
        }
        PltUnlockMutex(&g_ctx.picoquicMutex);
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

            // §Q-PATH-RECOVERY-STANDBY 2026-05-27: 恢復的路徑強制以
            // standby 加入，設 5 秒寬限期。防止 server 端 bestVideoPath
            // 立即切到 cwnd 冷的恢復路徑，造成震盪 + IDR 風暴。
            // v1.5.161 實測：Ethernet 恢復後 cwin=72KB 勉強過 64KB 門檻，
            // 但 RTT 與 WiFi 相近（都 ~1ms），server 每 3-8ms 在兩者間
            // 切換 bestVideoPath → 每次都觸發 IDR → 影像永久凍結。
            PltLockMutex(&g_ctx.picoquicMutex);
            for (int s = 0; s < g_ctx.subflowCount; s++) {
                if (g_ctx.subflows[s].id == ret) {
                    g_ctx.subflows[s].keepAsStandby = true;
                    g_ctx.subflows[s].recoveryStandbyUntil =
                        picoquic_current_time() +
                        (uint64_t)RECOVERY_STANDBY_GRACE_S * 1000000;
                    // §Q-STANDBY-REGAIN: 武裝回歸駐留計時——grace 之後再
                    // 觀察 REGAIN_HEALTH_S（flap 退避 ×2^count）才有資格
                    // 升回 available。
                    {
                        int backoff = g_ctx.regainFlapCount;
                        // §Q-BREAKER-ESC：跳閘史存在 → 直接用最大 dwell
                        if (g_ctx.breakerTripCount > 0)
                            backoff = REGAIN_FLAP_MAX;
                        if (backoff > REGAIN_FLAP_MAX)
                            backoff = REGAIN_FLAP_MAX;
                        g_ctx.subflows[s].regainEligibleAt =
                            picoquic_current_time() +
                            (uint64_t)(RECOVERY_STANDBY_GRACE_S +
                                       (REGAIN_HEALTH_S << backoff)) * 1000000;
                    }
                    if (g_ctx.cnx) {
                        quicSetPathStatusTracked(&g_ctx.subflows[s],
                                                 picoquic_path_status_backup);
                    }
                    Limelog("[VIPLE-MPQUIC] §Q-PATH-RECOVERY-STANDBY: "
                            "subflow %d forced standby for %d seconds "
                            "(cwnd warm-up grace)\n",
                            ret, RECOVERY_STANDBY_GRACE_S);
                    break;
                }
            }
            PltUnlockMutex(&g_ctx.picoquicMutex);

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
    // §Q-SLOT-CLEANUP: 跳過 picoquicDeleted 的死 slot。
    // path-recovery 會在同介面重建新 subflow，舊 slot 標記 deleted
    // 但仍留在陣列裡等回收。不過濾的話 overlay 會出現兩筆同名條目
    // （例如「乙太網路」出現兩次——一個 ✘ 一個 ✔）。
    int outIdx = 0;

    for (int i = 0; i < g_ctx.subflowCount && outIdx < maxCount; i++) {
        QUIC_SUBFLOW* sf = &g_ctx.subflows[i];
        if (sf->picoquicDeleted)
            continue;  // 死 slot，跳過
        out[outIdx].interfaceIndex = sf->interfaceIndex;
        PltSafeStrcpy(out[outIdx].interfaceName, sizeof(out[outIdx].interfaceName), sf->name);
        out[outIdx].interfaceType = sf->type;
        out[outIdx].rttMs = sf->rttMs;
        out[outIdx].throughputMbps = sf->throughputMbps;
        out[outIdx].lossPercent = sf->lossPercent;
        out[outIdx].reorderPercent = sf->reorderPercent;
        out[outIdx].active = sf->active;
        outIdx++;
    }

    return outIdx;
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
    // §Q-NOQUIC-STALE-FLAG-FIX (review batch 2 驗測發現)：QUIC 從未連線
    // （--no-quic、或 quicConnect 還沒跑）時 g_ctx 是零值初始化——
    // failoverPromotedSlot 的「無 failover」哨兵值是 -1，但只在
    // quicConnect 內設定；零值 0 會被當成「slot 0 已 promote」→
    // quicIsFailoverActive() 誤回 1，配上 subflowCount==0 讓
    // quicIsPrimaryPathUnhealthy() 也回 1 → Fix L bypass 在 --no-quic
    // 模式的第一個 control 訊息就走 QUIC（必失敗）→ 連線啟動即死。
    // v1.5.192 Fix L 以來 --no-quic 一直壞在這裡（驗測都走 QUIC 沒踩到）。
    if (g_ctx.cnx == NULL)
        return 0;
    // §Q-K13-ZOMBIE-FIX：IO thread 已 §K.13 bail 時，QUIC 整體已廢，
    // 不可再讓 control fallback 閘門（quicControlFallbackAvailable）
    // 因殘留的 failoverPromotedSlot 而繼續 suppress connectionTerminated。
    if (g_ctx.ioDead)
        return 0;
    return (g_ctx.failoverPromotedSlot >= 0) ? 1 : 0;
}

// Fix M v1.5.194: 查詢 primary path（slot 0）是否確認有問題。
// Fix L bypass 用此做二次驗證，防止假性 failover 時繞過正常 ENet。
// Returns 1 if primary path is deleted/inactive/has timeouts.
int quicIsPrimaryPathUnhealthy(void) {
    if (g_ctx.subflowCount <= 0) return 1;
    QUIC_SUBFLOW* sf = &g_ctx.subflows[0];
    if (sf->picoquicDeleted || !sf->active) return 1;
    if (sf->consecutiveTimeouts > 0) return 1;
    // §Q-REVIVE-STANDBY: slot 0 復活後躺在 standby（dwell 未滿）期間，
    // 視訊仍在備援路徑上——control 面應維持 failover 行為（Fix L bypass
    // / §Q-IDR-QUIC-FIRST）。
    if (sf->keepAsStandby) return 1;
    // §Q-LOSS-DEMOTE: zombie 判定——loss 持續越檻即視為不健康，即使
    // lastRecvTime 被零星控制面封包保鮮（2026-07-06 事故中此函式被
    // zombie 騙過 → 凍結期間 RFI 沒有 QUIC 備援管道）。
    if (sf->lossBadSinceUs != 0) {
        uint64_t nowU = picoquic_current_time();
        if (nowU - sf->lossBadSinceUs >= LOSS_DEMOTE_HOLD_US) return 1;
    }
    return 0;
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

        if (pktLen >= QUIC_DGRAM_HEADER_SIZE && ft < QUIC_FLOW_COUNT && ctx->recvCallbacks[ft]) {
            ctx->recvCallbacks[ft](
                ft,
                pkt + QUIC_DGRAM_HEADER_SIZE,
                pktLen - QUIC_DGRAM_HEADER_SIZE,
                ctx->recvContexts[ft]);
        }

        jb->delivered++;
        jb->slots[idx].len = 0;
        if (jb->bufferedCount > 0) jb->bufferedCount--; // §Q-PERF.4
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
            if (pktLen >= QUIC_DGRAM_HEADER_SIZE && ft < QUIC_FLOW_COUNT && ctx->recvCallbacks[ft]) {
                ctx->recvCallbacks[ft](
                    ft,
                    pkt + QUIC_DGRAM_HEADER_SIZE,
                    pktLen - QUIC_DGRAM_HEADER_SIZE,
                    ctx->recvContexts[ft]);
            }
            jb->delivered++;
            jb->slots[idx].len = 0;
            if (jb->bufferedCount > 0) jb->bufferedCount--; // §Q-PERF.4
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
        // §Q-PERF.8：FEC 重建插入但該 seq 已被跳過/交付——獨立計數，
        // 不汙染 lateAfterSkip（「timeout 過於激進」訊號）。
        if (g_fecInsertingRecovered) {
            jb->fecRecoveredLate++;
            return;
        }
        // §5a.r4 v1.5.202 Fix Q.5：區分兩種「已交付過的遲到封包」：
        //  (a) 真的被 timeout 跳過的 seq 又遲到 → lateAfterSkip（timeout 太
        //      激進的訊號，應 ~0）。判定：seq 落在最近一次 timeout 跳過的
        //      範圍 [lastSkipFrom, +lastSkipCount) 內，且在 100ms 過期窗內
        //      （避免 uint16 seq 繞回誤判舊範圍）。
        //  (b) 其餘小距離遲到 → fecRedundantLate（多為 FEC 已重建、真實封包
        //      稍後遲到；良性，有損路徑常見）。
        int16_t sinceFrom = (int16_t)(seq - jb->lastSkipFrom);
        if (jb->lastSkipCount > 0 &&
            sinceFrom >= 0 && sinceFrom < jb->lastSkipCount &&
            now - jb->lastSkipUs < 100000 /* 100ms */) {
            jb->lateAfterSkip++;
        } else if (diff > -32) {
            jb->fecRedundantLate++;
        }
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
        jb->bufferedCount++; // §Q-PERF.4
    }

    if (diff > 0) {
        jb->reordered++;
        // §5a.r3 Fix Q 診斷：記錄最大 reorder 距離（diff 峰值）
        if ((uint32_t)diff > jb->maxReorderDist)
            jb->maxReorderDist = (uint32_t)diff;
    }

    quicJitterDeliver(ctx, jb, ft);

    quicJitterCheckTimeout(ctx, jb, ft, now);
}

// §Q-IDR-JITTER-RELAX v1.5.177：failover 期間放寬 jitter timeout
// §5a.r3 v1.5.198 Fix Q：正常模式改成 RTT 自適應（1.5×RTT，夾在
// 10ms floor ~ 40ms cap）。寫死 10ms 在 ~30ms RTT 遠端路徑會把會晚到
// 的重排封包過早跳過 → 過早 unrecoverable frame + lag。
// §Q-PERF.4：從 quicJitterInsert 抽出共用——FEC 群組過期窗（§Q-PERF.3）
// 與 quicJitterPoll 也需要同一個自適應值。
static int quicJitterComputeMaxWait(QUIC_TRANSPORT_CTX* ctx) {
    if (quicIsFailoverActive())
        return QUIC_JITTER_MAX_WAIT_FAILOVER_US;
    uint32_t rttUs = ctx->jitterReorderRttUs;
    if (rttUs == 0)
        return QUIC_JITTER_MAX_WAIT_NORMAL_US;
    uint64_t w = (uint64_t)rttUs * 3 / 2; // 1.5× RTT
    if (w < QUIC_JITTER_MAX_WAIT_NORMAL_US) w = QUIC_JITTER_MAX_WAIT_NORMAL_US;
    if (w > QUIC_JITTER_MAX_WAIT_CAP_US)    w = QUIC_JITTER_MAX_WAIT_CAP_US;
    return (int)w;
}

static void quicJitterCheckTimeout(QUIC_TRANSPORT_CTX* ctx, QUIC_JITTER_BUF* jb,
                                   unsigned char ft, uint64_t now) {
    int jitterMaxWait = quicJitterComputeMaxWait(ctx);
    jb->lastJitterMaxWait = (uint32_t)jitterMaxWait; // §5a.r3 診斷
    if (now - jb->lastDeliverUs >= (uint64_t)jitterMaxWait) {
        for (int i = 0; i < QUIC_JITTER_BUF_SIZE; i++) {
            uint16_t trySeq = (uint16_t)(jb->deliverNextSeq + i);
            int tidx = trySeq & QUIC_JITTER_BUF_MASK;
            if (jb->slots[tidx].len > 0) {
                // §Q-PERF.7: 跳過前先看缺口是否屬於「仍有機會重建」的
                // FEC 群組——是的話讓一拍（quicJitterPoll 每 ~1ms 再來），
                // 否則 timeout 把馬上能重建的封包跳掉，重建變 late 插入
                // 白做工（T2 實測 8% 丟包下 906 次輸給 10ms floor）。
                // 絕對上限 3×maxWait 防止 parity 也全丟時卡住交付。
                if (ft == QUIC_FLOW_VIDEO &&
                    now - jb->lastDeliverUs < (uint64_t)jitterMaxWait * 3 &&
                    quicFecGapStillCompletable(jb->deliverNextSeq, i, now,
                                               jitterMaxWait)) {
                    jb->fecDeferred++;
                    return;
                }
                // §5a.fix: i 是正確的跳過距離（uint16_t 模加法保證正）。
                // 舊寫法 (uint64_t)(trySeq - deliverNextSeq) 在 uint16_t
                // 邊界繞回時，int 升型導致負值轉 uint64_t 造成溢位。
                jb->timedOut += (uint64_t)i;
                jb->timeoutEvents++; // §5a.r3 診斷：timeout 事件次數
                // §5a.r4 Fix Q.5：記錄這次 timeout 跳過的 seq 範圍
                // [deliverNextSeq, +i)，供 diff<0 分支判定遲到封包是否真的
                // 被 timeout 跳過（vs FEC 冗餘遲到）。i 必 >=1（slot 空才會
                // 進到 timeout 跳過）。
                jb->lastSkipFrom = jb->deliverNextSeq;
                jb->lastSkipCount = i;
                jb->lastSkipUs = now;
                jb->deliverNextSeq = (uint16_t)(jb->deliverNextSeq + i);
                quicJitterDeliver(ctx, jb, ft);
                break;
            }
        }
    }
}

// §Q-PERF.4: 由 IO 迴圈每輪呼叫。舊行為 timeout 只在「同 flow 下一個
// datagram 到達」時（quicJitterInsert 尾端）才檢查——frame 尾包遺失後
// 若流量暫停（BBR pacing 間隙、低 fps、failover 縫隙、缺口後只到 FEC
// parity），已緩衝的封包會一直卡到下一包才放行，最壞多等一整個 frame
// interval。輪詢讓交付延遲有上界（= jitterMaxWait + 1 個 IO tick）。
// bufferedCount==0 時 O(1) 返回，常態（順序到達、buffer 空）零成本。
static void quicJitterPoll(QUIC_TRANSPORT_CTX* ctx) {
    uint64_t now = picoquic_current_time();
    for (int ft = 1; ft < QUIC_FLOW_COUNT && ft < 4; ft++) {
        QUIC_JITTER_BUF* jb = &g_jitterBufs[ft];
        if (!jb->initialized || jb->bufferedCount == 0)
            continue;
        quicJitterCheckTimeout(ctx, jb, (unsigned char)ft, now);
    }
}

// ── §5b FEC decoder ────────────────────────────────────────

// §Q-PERF.7: timeout 跳過前的 FEC 完成度檢查。skip 範圍 [startSeq,
// startSeq+count) 內任一缺漏 data seq 若屬於 active、年齡 ≤ 2×maxWait、
// 且尚未湊滿 4 個 shard 的群組 → 回傳 1（該再等，重建可能馬上發生）。
// 群組的 6 個 shard 在線路上只跨 ~1ms，年齡超過 2×maxWait 還沒湊齊
// 就是真的丟了不值得等。已湊滿（含已重建）的群組不影響 skip。
static int quicFecGapStillCompletable(uint16_t startSeq, int count,
                                      uint64_t now, int jitterMaxWaitUs) {
    uint64_t ageCap = (uint64_t)jitterMaxWaitUs * 2;
    for (int g = 0; g < QUIC_FEC_MAX_GROUPS; g++) {
        QUIC_FEC_GROUP* grp = &g_fecGroups[g];
        if (!grp->active || now - grp->createTimeUs > ageCap)
            continue;
        int have = 0;
        for (int b = 0; b < QUIC_FEC_DATA_SHARDS; b++)
            if (grp->dataPresent & (1 << b)) have++;
        if (have == QUIC_FEC_DATA_SHARDS)
            continue; // 完整或已重建
        for (int b = 0; b < QUIC_FEC_PARITY_SHARDS; b++)
            if (grp->parityPresent & (1 << b)) have++;
        if (have >= QUIC_FEC_DATA_SHARDS)
            continue; // 已達重建門檻（quicFecOnShard 會立即重建）
        for (int b = 0; b < QUIC_FEC_DATA_SHARDS; b++) {
            if (grp->dataPresent & (1 << b)) continue;
            uint16_t missSeq = (uint16_t)(grp->baseSeq + b);
            if ((uint16_t)(missSeq - startSeq) < (uint16_t)count)
                return 1;
        }
    }
    return 0;
}

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

            g_fecInsertingRecovered = 1; // §Q-PERF.8
            quicJitterInsert(ctx, ft, recoveredPkt,
                             QUIC_DGRAM_HEADER_SIZE + origLen);
            g_fecInsertingRecovered = 0;
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

    // §Q-PERF.3: 舊式 gi = baseSeq % 8 在 baseSeq stride=4（server 端
    // parity 用 baseSeq、不消耗 seq）下 gcd(4,8)=4 → 只用到 2 個 bucket，
    // in-flight 窗僅 8 個封包。改 (baseSeq >> 2) 讓相鄰群組必落不同
    // bucket、32 槽全用上。reinject 造成的 baseSeq 偏移不影響（僅 hash）。
    int gi = (int)((uint16_t)(baseSeq >> 2) % QUIC_FEC_MAX_GROUPS);
    QUIC_FEC_GROUP* grp = &g_fecGroups[gi];
    uint64_t now = picoquic_current_time();

    // §Q-PERF.3: 過期窗從寫死 50ms 改為跟隨 §5a 自適應 jitter timeout
    // （2×，下限 50ms）。failover 期間 jitter 願意等 200ms，FEC 群組卻
    // 50ms 就作廢——WiFi 冷啟動 IDR shard 間距 >50ms 時 FEC 完全幫不上。
    {
        uint64_t expiryUs = (uint64_t)quicJitterComputeMaxWait(ctx) * 2;
        if (expiryUs < 50000) expiryUs = 50000;
        if (grp->active && (grp->baseSeq != baseSeq ||
                            now - grp->createTimeUs > expiryUs)) {
            if ((grp->dataPresent & 0x0F) != 0x0F)
                g_fecEvictedIncomplete++; // §Q-PERF.3 診斷
            grp->active = 0;
        }
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
                if (ft < QUIC_FLOW_COUNT) {
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
                        quicSetPathStatusTracked(&ctx->subflows[i],
                                                 picoquic_path_status_backup);
                    } else {
                        quicSetPathStatusTracked(&ctx->subflows[i],
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
        // §Q-MP-DYN-STANDBY: picoquic 偵測到路徑 RTT 變化超過訂閱閾值。
        // 強制 health check 立即重跑（重設計時器），讓 dynamic standby
        // 邏輯在下一個 IO tick 中及早執行，不等固定 500ms 週期。
        ctx->lastHealthCheck = 0;
        break;
    }

    case picoquic_callback_path_suspended:
    case picoquic_callback_path_deleted: {
        Limelog("[VIPLE-MPQUIC] Path %s: id=%llu\n",
                fin_or_event == picoquic_callback_path_deleted
                    ? "deleted" : "suspended",
                (unsigned long long)stream_id);
        bool wasNonStandbyActive = false;
        int killedIfIdx = -1;
        for (int i = 0; i < ctx->subflowCount; i++) {
            if (ctx->subflows[i].picoquicPathId == stream_id) {
                // 紀錄這個被刪/暫停的路徑是不是 non-standby 活躍路徑——
                // 若是，下面要做緊急 standby 升級。
                wasNonStandbyActive = ctx->subflows[i].active &&
                                      !ctx->subflows[i].keepAsStandby;
                killedIfIdx = ctx->subflows[i].interfaceIndex;

                ctx->subflows[i].active = false;
                if (fin_or_event == picoquic_callback_path_deleted) {
                    // §Q-DYN-PATH: 標記為 picoquic 永久刪除。slot 會在
                    // 下一次 quicRecheckPaths() 週期中被回收（重新探測
                    // 同一介面）或讓出給新介面使用。
                    ctx->subflows[i].picoquicDeleted = true;
                }
                else {
                    // §Q-REVIVE-STANDBY (Fix A5): suspended（未刪除）的
                    // 路徑之後可能復活——與 INACTIVE 標記一致，先進
                    // standby，促升走 REGAIN 的 dwell + liveness。
                    ctx->subflows[i].keepAsStandby = true;
                    ctx->subflows[i].regainEligibleAt = 0;
                    ctx->subflows[i].lastStandbyProbeOk = 0;
                }
                // §Q-STANDBY-REGAIN: 剛 regain 的介面被 picoquic 判死
                // → flap 退避
                quicNoteRegainFlap(ctx->subflows[i].interfaceIndex,
                                   picoquic_current_time());
                if (ctx->failoverCallback) {
                    ctx->failoverCallback(ctx->subflows[i].id,
                        ctx->subflows[i].interfaceIndex,
                        false, ctx->failoverContext);
                }
                break;
            }
        }

        // §Q-MP-EMERGENCY-PROMOTE 2026-05-27:
        // picoquic 直接刪除一個 non-standby active 路徑時，繞過了 health
        // check 的 5-timeout failover 邏輯。檢查是否還有其他 non-standby
        // 活躍路徑；若沒有，立即升級任意 standby 路徑頂上。
        //
        // 歷史教訓：v1.5.143 1080p180 串流，主路徑（Ethernet）被 picoquic
        // 刪除後，Tailscale standby 路徑始終閒置（rx=1KB 整場），
        // 串流陷入 "Waiting for IDR frame" 死亡螺旋 1 分鐘以上。
        if (wasNonStandbyActive) {
            int activeNonStandbyCount = 0;
            for (int j = 0; j < ctx->subflowCount; j++) {
                QUIC_SUBFLOW* sf = &ctx->subflows[j];
                if (sf->active && !sf->keepAsStandby && !sf->picoquicDeleted)
                    activeNonStandbyCount++;
            }
            if (activeNonStandbyCount == 0) {
                // §Q-MP-EMERGENCY-PROMOTE v3 (2026-05-27)：兩階段挑選——
                // 先在「非 VPN」standby 裡挑 best-RTT；若無，再 fallback
                // 到 VPN standby。
                //
                // 使用者洞察：VPN（Tailscale 等）物理上常走主介面的網路
                // 堆疊。主介面死掉時 VPN 也會跟著失效一段時間。所以
                // EMERGENCY-PROMOTE 不該優先挑 VPN，即使其 RTT 較低
                // （那個 RTT 是底層健康時測到的，已經失效）。
                int bestJ = -1;
                float bestRttMs = 1e9f;

                for (int pass = 0; pass < 2 && bestJ < 0; pass++) {
                    for (int j = 0; j < ctx->subflowCount; j++) {
                        QUIC_SUBFLOW* candidate = &ctx->subflows[j];
                        if (!candidate->keepAsStandby ||
                            !candidate->active ||
                            candidate->picoquicDeleted)
                            continue;
                        bool isVpn = (candidate->type == LC_NETIF_TYPE_VPN);
                        if (pass == 0 && isVpn) continue;       // 跳過 VPN
                        if (pass == 1 && !isVpn) continue;      // 只看 VPN

                        float effRtt = (candidate->rttMs > 0)
                            ? candidate->rttMs
                            : (float)candidate->icmpRttMs;
                        if (effRtt < bestRttMs) {
                            bestRttMs = effRtt;
                            bestJ = j;
                        }
                    }
                }

                if (bestJ >= 0) {
                    QUIC_SUBFLOW* candidate = &ctx->subflows[bestJ];
                    int j = bestJ;
                    candidate->keepAsStandby = false;
                    candidate->recoveryStandbyUntil = 0; // emergency 覆蓋 recovery grace
                    quicResetSubflowHealthOnPromote(candidate,
                                                    picoquic_current_time());
                    quicSetPathStatusTracked(candidate,
                                             picoquic_path_status_available);
                    ctx->failoverPromotedSlot = j;
                    Limelog("[VIPLE-MPQUIC] §Q-MP-EMERGENCY-PROMOTE: "
                            "primary if %d killed by picoquic, no other "
                            "active non-standby paths — promoting "
                            "standby subflow %d (if %d, slot %d, %s, "
                            "RTT=%.1fms) to available\n",
                            killedIfIdx,
                            candidate->id,
                            candidate->interfaceIndex, j,
                            (candidate->type == LC_NETIF_TYPE_VPN)
                                ? "VPN-fallback" : "non-VPN",
                            bestRttMs);
                    if (ctx->failoverCallback) {
                        ctx->failoverCallback(
                            candidate->id,
                            candidate->interfaceIndex,
                            true, ctx->failoverContext);
                    }
                }
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
            // §Q-SELECT-GUARD-FIX (review)：select() 若集合含無效 fd 會立即
            // 回 SOCKET_ERROR（Windows WSAENOTSOCK）且不消耗 timeout——下方
            // recv 區塊被 selRet>0 gate 住全部跳過、迴圈無延遲空轉燒滿一核。
            // 主因（回收死 slot 留壞 handle）已於 quicAddSubflowEx 修掉，此處
            // 為防禦縱深：select 失敗時睡 1ms 避免 busy-spin（下一輪 fd_set
            // 重建，瞬時一次失敗無害）。
            if (selRet < 0) {
                PltSleepMs(1);
            }
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
            ctx->ioDead = 1;          // §Q-K13-ZOMBIE-FIX：通知上層 QUIC 已死
            ctx->recoveryRunning = false; // 停掉 recovery thread 的無效 probe
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
                ctx->ioDead = 1;          // §Q-K13-ZOMBIE-FIX：通知上層 QUIC 已死
                ctx->recoveryRunning = false; // 停掉 recovery thread 的無效 probe
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
        // §Q-PERF.2: 改走 quicSetPathStatusTracked 去重——舊版每 ~1ms
        // 對每條 path 無條件重設，每次都排一個 PATH_AVAILABLE/BACKUP
        // frame 上線路（picoquic 不去重）。
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
                (void)quicSetPathStatusTracked(&ctx->subflows[i], st);
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
        // §Q-PERF.5: 移除 quicRecheckPaths()——它每 30 秒在 picoquicMutex
        // 鎖內做整套 lcEnumNetInterfaces（GetAdaptersAddresses，可阻塞數 ms
        // 到數十 ms），期間 input/control 送出全被擋 → 週期性輸入延遲尖峰。
        // 其功能（介面異動診斷）已由 recovery thread（每 10s、鎖外列舉、
        // §Q.path-recovery 自帶日誌）完整取代。
        quicJitterPoll(ctx); // §Q-PERF.4: jitter timeout 上界輪詢

        // §K.10 diag: 每 5 秒印出 per-flow 接收統計 + per-path 品質
        {
            uint64_t nowDiag = picoquic_current_time();
            if (g_lastDiagLogTime == 0) g_lastDiagLogTime = nowDiag;
            if (nowDiag - g_lastDiagLogTime >= DIAG_LOG_INTERVAL_US) {
                double dtSec = (double)(nowDiag - g_lastDiagLogTime) / 1e6;
                if (dtSec < 0.1) dtSec = 0.1;

                Limelog("[VIPLE-MPQUIC] §K.10 RECV VIDEO: %llu dgrams %llu B (%.1f Mbps) | "
                        "AUDIO: %llu dgrams %llu B (%.1f Mbps) | "
                        "CTRL: %llu dgrams | INPUT: %llu dgrams\n",
                        (unsigned long long)g_dgramsRecvByFlow[1],
                        (unsigned long long)g_bytesRecvByFlow[1],
                        (double)(g_bytesRecvByFlow[1] - g_prevBytesRecvByFlow[1]) * 8.0 / dtSec / 1e6,
                        (unsigned long long)g_dgramsRecvByFlow[2],
                        (unsigned long long)g_bytesRecvByFlow[2],
                        (double)(g_bytesRecvByFlow[2] - g_prevBytesRecvByFlow[2]) * 8.0 / dtSec / 1e6,
                        (unsigned long long)g_dgramsRecvByFlow[3],
                        (unsigned long long)g_dgramsRecvByFlow[4]);

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
                    const char* flowNames[] = {"(none)", "VIDEO", "AUDIO", "CTRL", "INPUT"};
                    for (int ji = 0; ji < QUIC_FLOW_COUNT; ji++) {
                        QUIC_JITTER_BUF* jb = &g_jitterBufs[ji];
                        if (jb->delivered || jb->reordered || jb->dropped || jb->timedOut) {
                            // §5a.r3 v1.5.198 Fix Q：加印 maxWait（實際採用的
                            // RTT 自適應 timeout）、maxReorder（reorder 距離峰值）、
                            // toEvents（timeout 事件數）、lateAfterSkip（過早跳過偵測）
                            Limelog("[VIPLE-MPQUIC] §5a Jitter[%s]: delivered=%llu reordered=%llu dropped=%llu timedOut=%llu maxWait=%uus maxReorder=%u toEvents=%llu lateAfterSkip=%llu fecRedundantLate=%llu fecDeferred=%llu fecRecoveredLate=%llu\n",
                                    flowNames[ji],
                                    (unsigned long long)jb->delivered,
                                    (unsigned long long)jb->reordered,
                                    (unsigned long long)jb->dropped,
                                    (unsigned long long)jb->timedOut,
                                    jb->lastJitterMaxWait,
                                    jb->maxReorderDist,
                                    (unsigned long long)jb->timeoutEvents,
                                    (unsigned long long)jb->lateAfterSkip,
                                    (unsigned long long)jb->fecRedundantLate,
                                    (unsigned long long)jb->fecDeferred,
                                    (unsigned long long)jb->fecRecoveredLate);
                        }
                    }
                }

                // §5b FEC 診斷（§Q-PERF.3：加 evictedIncomplete——群組
                // 未湊齊就被驅逐/過期的次數，bucket 碰撞與過期窗調參依據）
                if (g_fecRecovered || g_fecFailed || g_fecEvictedIncomplete) {
                    Limelog("[VIPLE-MPQUIC] §5b FEC: recovered=%llu failed=%llu evictedIncomplete=%llu\n",
                            (unsigned long long)g_fecRecovered,
                            (unsigned long long)g_fecFailed,
                            (unsigned long long)g_fecEvictedIncomplete);
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

                for (int bi = 1; bi < QUIC_FLOW_COUNT; bi++) {
                    g_prevBytesRecvByFlow[bi] = g_bytesRecvByFlow[bi];
                }
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
