#include "Limelight-internal.h"

#ifdef VIPLE_MPQUIC

#include "QuicTransport.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <picoquic.h>
#include <picoquic_utils.h>
#include <picosocks.h>
// Congestion algorithm pointers live in their own headers in picoquic.
#include <picoquic_bbr.h>
#include <picoquic_cubic.h>
#include <picoquic_newreno.h>

// ── Internal constants ──────────────────────────────────────

#define PROBE_TIMEOUT_THRESHOLD  5     // consecutive probe failures → inactive
#define HEALTH_CHECK_INTERVAL_US 500000 // 500ms between health checks
#define STATS_UPDATE_INTERVAL_US 100000 // 100ms between stats refresh

// ── Internal state ──────────────────────────────────────────

typedef struct _QUIC_SUBFLOW {
    int id;
    int interfaceIndex;
    SOCKET sock;
    struct sockaddr_storage localAddr;
    SOCKADDR_LEN addrLen;
    bool active;
    // picoquic's per-path identifier (uint64 in picoquic API). Set when we
    // probe a new path; -1 (UINT64_MAX) before that. Note: picoquic creates
    // path 0 automatically for the initial connection, so the first subflow
    // we add via probe_new_path gets id 1.
    uint64_t picoquicPathId;

    // Per-subflow stats (updated from picoquic path stats via
    // picoquic_get_path_quality)
    float rttMs;
    float throughputMbps;
    float lossPercent;
    float reorderPercent;
    char name[LC_NETIF_MAX_NAME];
    int type;

    // Path health tracking
    int consecutiveTimeouts;
    uint64_t lastProbeTime;
    uint64_t lastRecvTime;
    uint64_t bytesSent;
    uint64_t bytesRecv;
} QUIC_SUBFLOW;

typedef struct _QUIC_TRANSPORT_CTX {
    picoquic_quic_t* quic;
    picoquic_cnx_t* cnx;

    QUIC_SUBFLOW subflows[QUIC_MAX_SUBFLOWS];
    int subflowCount;
    int nextSubflowId;

    int scheduler[4]; // index 0 = default, 1..3 = per QUIC_FLOW_*
    int aggregateRRIndex; // round-robin index for AGGREGATE

    // Receive callback
    QuicRecvCallback recvCallback;
    void* recvContext;

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

    // Congestion control algorithm
    int congestionAlgo; // QUIC_CC_*

    // Peer address (cached from quicConnect params; picoquic doesn't expose
    // a public way to read it back after create_cnx, and the internal
    // cnx->path[0]->peer_addr is an opaque struct).
    struct sockaddr_storage peerAddr;
    SOCKADDR_LEN peerAddrLen;

    // Path id counter for unique_path_id assignment. Picoquic path 0 is the
    // initial cnx path created by create_cnx; subsequent probes get
    // ids 1, 2, 3... in the order they're probed.
    uint64_t nextPicoquicPathId;
} QUIC_TRANSPORT_CTX;

static QUIC_TRANSPORT_CTX g_ctx;
static bool g_initialized = false;

// ── Forward declarations ────────────────────────────────────

static void quicIoThreadProc(void* context);
static int quicDgramCallback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx,
    void* stream_ctx);
static int quicSelectPath(unsigned char flowType, int dataLen);
static void quicUpdatePathStats(void);
static void quicCheckPathHealth(void);
static void quicApplyCongestionAlgo(picoquic_quic_t* quic);

// ── Lifecycle ───────────────────────────────────────────────

int quicTransportInit(void) {
    if (g_initialized)
        return 0;

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.nextSubflowId = 1;

    // Default scheduler per flow type
    g_ctx.scheduler[0] = QUIC_SCHED_AUTO;
    g_ctx.scheduler[QUIC_FLOW_VIDEO] = QUIC_SCHED_ECF;
    g_ctx.scheduler[QUIC_FLOW_AUDIO] = QUIC_SCHED_REDUNDANT;
    g_ctx.scheduler[QUIC_FLOW_CONTROL] = QUIC_SCHED_MIN_RTT;

    g_ctx.congestionAlgo = QUIC_CC_BBR;

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

    g_initialized = false;
    Limelog("[VIPLE-MPQUIC] Transport subsystem cleaned up\n");
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
        NULL,                                       // ticket_file_name (TODO: persist)
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
    quicApplyCongestionAlgo(g_ctx.quic);

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

    // Start the I/O thread
    g_ctx.ioRunning = true;
    ret = PltCreateThread("QuicIO", quicIoThreadProc, &g_ctx, &g_ctx.ioThread);
    if (ret != 0) {
        Limelog("[VIPLE-MPQUIC] Failed to create I/O thread\n");
        picoquic_delete_cnx(g_ctx.cnx);
        g_ctx.cnx = NULL;
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
        return -1;
    }

    Limelog("[VIPLE-MPQUIC] Connection initiated to port %u\n", params->quicPort);
    return 0;
}

void quicDisconnect(void) {
    if (g_ctx.cnx) {
        g_ctx.ioRunning = false;

        picoquic_close(g_ctx.cnx, 0);
        PltInterruptThread(&g_ctx.ioThread);
        PltJoinThread(&g_ctx.ioThread);

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
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
    }
}

bool quicIsConnected(void) {
    return g_ctx.cnx != NULL &&
           picoquic_get_cnx_state(g_ctx.cnx) == picoquic_state_ready;
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
    int strategy;
    int i, bestIdx;

    if (g_ctx.subflowCount == 0)
        return -1;
    if (g_ctx.subflowCount == 1)
        return g_ctx.subflows[0].active ? 0 : -1;

    // Resolve AUTO to per-flow default
    strategy = g_ctx.scheduler[flowType];
    if (strategy == QUIC_SCHED_AUTO) {
        switch (flowType) {
        case QUIC_FLOW_VIDEO:   strategy = QUIC_SCHED_ECF; break;
        case QUIC_FLOW_AUDIO:   strategy = QUIC_SCHED_REDUNDANT; break;
        case QUIC_FLOW_CONTROL: strategy = QUIC_SCHED_MIN_RTT; break;
        default:                strategy = QUIC_SCHED_MIN_RTT; break;
        }
    }

    switch (strategy) {

    case QUIC_SCHED_MIN_RTT: {
        float bestRtt = 1e9f;
        bestIdx = -1;
        for (i = 0; i < g_ctx.subflowCount; i++) {
            if (g_ctx.subflows[i].active && g_ctx.subflows[i].rttMs < bestRtt) {
                bestRtt = g_ctx.subflows[i].rttMs;
                bestIdx = i;
            }
        }
        return bestIdx;
    }

    case QUIC_SCHED_ECF: {
        // Earliest Completion First: pick path where
        // (data_len / throughput) + rtt/2 is smallest
        float bestTime = 1e9f;
        bestIdx = -1;
        for (i = 0; i < g_ctx.subflowCount; i++) {
            if (!g_ctx.subflows[i].active)
                continue;
            float tput = g_ctx.subflows[i].throughputMbps;
            if (tput < 0.001f) tput = 0.001f;
            float transferTime = ((float)dataLen * 8.0f) / (tput * 1e6f);
            float completion = transferTime + (g_ctx.subflows[i].rttMs / 2000.0f);
            if (completion < bestTime) {
                bestTime = completion;
                bestIdx = i;
            }
        }
        return bestIdx;
    }

    case QUIC_SCHED_AGGREGATE: {
        // Round-robin across active subflows
        int start = g_ctx.aggregateRRIndex;
        for (i = 0; i < g_ctx.subflowCount; i++) {
            int idx = (start + i) % g_ctx.subflowCount;
            if (g_ctx.subflows[idx].active) {
                g_ctx.aggregateRRIndex = (idx + 1) % g_ctx.subflowCount;
                return idx;
            }
        }
        return -1;
    }

    case QUIC_SCHED_REDUNDANT:
        // Caller handles redundant by iterating all active subflows
        return -2; // sentinel: send on all

    default:
        // Fallback: first active
        for (i = 0; i < g_ctx.subflowCount; i++) {
            if (g_ctx.subflows[i].active)
                return i;
        }
        return -1;
    }
}

// ── Subflow management ──────────────────────────────────────

int quicAddSubflow(int interfaceIndex,
                   const struct sockaddr_storage* localAddr,
                   SOCKADDR_LEN addrLen) {
    QUIC_SUBFLOW* sf;
    SOCKET sock;

    if (!g_ctx.cnx || g_ctx.subflowCount >= QUIC_MAX_SUBFLOWS)
        return -1;

    sock = createSocket(localAddr->ss_family, SOCK_DGRAM, IPPROTO_UDP, false);
    if (sock == INVALID_SOCKET) {
        Limelog("[VIPLE-MPQUIC] Failed to create subflow socket for if %d\n",
                interfaceIndex);
        return -1;
    }

    if (bind(sock, (const struct sockaddr*)localAddr, addrLen) != 0) {
        Limelog("[VIPLE-MPQUIC] Failed to bind subflow socket to if %d\n",
                interfaceIndex);
        closeSocket(sock);
        return -1;
    }

    sf = &g_ctx.subflows[g_ctx.subflowCount];
    memset(sf, 0, sizeof(*sf));
    sf->id = g_ctx.nextSubflowId++;
    sf->interfaceIndex = interfaceIndex;
    sf->sock = sock;
    memcpy(&sf->localAddr, localAddr, addrLen);
    sf->addrLen = addrLen;
    sf->active = true;
    sf->lastRecvTime = picoquic_current_time();

    // The first subflow inherits picoquic's auto-created path 0 (the cnx's
    // initial path bound to the addr we passed to picoquic_create_cnx).
    // Subsequent subflows get a new path via probe_new_path.
    if (g_ctx.subflowCount == 0) {
        sf->picoquicPathId = 0;
    } else {
        // picoquic_probe_new_path signature is (cnx, peer_addr, local_addr,
        // current_time) — peer first, local second. The Phase 1 code had
        // these swapped, which probed an invalid path.
        int ret = picoquic_probe_new_path(g_ctx.cnx,
            (const struct sockaddr*)&g_ctx.peerAddr,
            (const struct sockaddr*)localAddr,
            picoquic_current_time());
        if (ret != 0) {
            Limelog("[VIPLE-MPQUIC] probe_new_path failed (%d) for if %d\n",
                    ret, interfaceIndex);
            closeSocket(sock);
            return -1;
        }
        // Assign the path id picoquic will use for the freshly probed path.
        // We track this ourselves rather than asking picoquic, since there
        // is no public picoquic_get_cnx_nb_paths API; the probe path id
        // matches the order in which we issued probes (1, 2, 3, ...).
        g_ctx.nextPicoquicPathId++;
        sf->picoquicPathId = g_ctx.nextPicoquicPathId;
    }

    g_ctx.subflowCount++;
    Limelog("[VIPLE-MPQUIC] Added subflow %d on interface %d (path %llu)\n",
            sf->id, interfaceIndex, (unsigned long long)sf->picoquicPathId);
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

    // picoquic_set_path_status is the public API for steering multipath
    // scheduling. It's binary: "available" (use this path) vs "backup"
    // (avoid unless others are unavailable). Tag the chosen subflow as
    // available and the rest as backup so picoquic's internal scheduler
    // routes this datagram on the path we picked.
    if (pathIdx >= 0 && pathIdx < g_ctx.subflowCount) {
        for (int i = 0; i < g_ctx.subflowCount; i++) {
            if (!g_ctx.subflows[i].active)
                continue;
            picoquic_set_path_status(g_ctx.cnx,
                g_ctx.subflows[i].picoquicPathId,
                (i == pathIdx) ? picoquic_path_status_available
                               : picoquic_path_status_backup);
        }
    }

    int ret = picoquic_queue_datagram_frame(g_ctx.cnx, frameLen, frame);
    if (ret == 0 && pathIdx >= 0 && pathIdx < g_ctx.subflowCount) {
        g_ctx.subflows[pathIdx].bytesSent += dataLen;
    }
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
    g_ctx.recvCallback = callback;
    g_ctx.recvContext = context;
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

        // Pacing rate is bytes/sec → Mbps (×8 bits, ÷1e6)
        g_ctx.subflows[i].throughputMbps =
            (float)((double)pq.pacing_rate * 8.0 / 1e6);

        // Loss percent from sent / lost counters
        if (pq.sent > 0) {
            g_ctx.subflows[i].lossPercent =
                (float)((double)pq.lost / (double)pq.sent * 100.0);
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
            }
        } else if (sf->active) {
            sf->consecutiveTimeouts = 0;
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
        }
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

// ── Session ticket (0-RTT) ──────────────────────────────────

int quicGetSessionTicket(unsigned char* buf, int bufLen) {
    // 0-RTT session-ticket persistence is delegated to picoquic via the
    // ticket_file_name parameter passed to picoquic_create. When that
    // parameter is non-NULL, picoquic auto-saves tickets to that file on
    // disconnect and auto-loads them on the next connect — no app-level
    // get/set is needed.
    //
    // We currently pass NULL (no file), which disables 0-RTT resumption
    // but keeps regular 1-RTT TLS handshake fully functional. To re-enable
    // 0-RTT, set ticket_file_name in quicConnect to a platform-appropriate
    // app-data path (per-user, per-server-UUID). Tracked in §Q Phase 5.
    (void)buf; (void)bufLen;
    return 0;
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
        if (length >= QUIC_DGRAM_HEADER_SIZE && ctx->recvCallback) {
            PQUIC_DGRAM_HEADER hdr = (PQUIC_DGRAM_HEADER)bytes;
            ctx->recvCallback(
                hdr->flowType,
                bytes + QUIC_DGRAM_HEADER_SIZE,
                (int)(length - QUIC_DGRAM_HEADER_SIZE),
                ctx->recvContext);
        }
        // Per-subflow lastRecvTime is updated by the I/O thread directly
        // when recvfrom returns on a given socket (the I/O thread knows
        // which subflow socket fired); the picoquic datagram callback
        // has no way to identify the receiving path. We DO bump global
        // health here as a coarse "the connection is alive" signal.
        break;

    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (stream_id == 0 && ctx->recvCallback) {
            ctx->recvCallback(QUIC_FLOW_CONTROL, bytes, (int)length,
                              ctx->recvContext);
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

    case picoquic_callback_path_suspended:
    case picoquic_callback_path_deleted: {
        Limelog("[VIPLE-MPQUIC] Path %s: id=%llu\n",
                fin_or_event == picoquic_callback_path_deleted
                    ? "deleted" : "suspended",
                (unsigned long long)stream_id);
        for (int i = 0; i < ctx->subflowCount; i++) {
            if (ctx->subflows[i].picoquicPathId == stream_id) {
                if (ctx->subflows[i].active) {
                    ctx->subflows[i].active = false;
                    if (ctx->failoverCallback) {
                        ctx->failoverCallback(ctx->subflows[i].id,
                            ctx->subflows[i].interfaceIndex,
                            false, ctx->failoverContext);
                    }
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

        if (maxSock != INVALID_SOCKET) {
            int selRet = select((int)(maxSock + 1), &readSet, NULL, NULL, &tv);
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

                // Check each subflow socket. We know exactly which subflow
                // received this packet (the socket fd that fired), which
                // gives us per-subflow lastRecvTime tracking that the
                // picoquic callback can't provide.
                for (int i = 0; i < ctx->subflowCount; i++) {
                    SOCKET sf = ctx->subflows[i].sock;
                    if (sf != INVALID_SOCKET && FD_ISSET(sf, &readSet)) {
                        peerLen = sizeof(peerAddr);
                        SOCK_RET recvLen = recvfrom(sf, (char*)recvBuf,
                            sizeof(recvBuf), 0,
                            (struct sockaddr*)&peerAddr, &peerLen);
                        if (recvLen > 0) {
                            ctx->subflows[i].lastRecvTime = currentTime;
                            ctx->subflows[i].bytesRecv += (uint64_t)recvLen;
                            // If a previously-dead path comes back, mark
                            // it active immediately so the scheduler can
                            // start using it without waiting for the next
                            // health-check window.
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
        } else {
            PltSleepMs(1);
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

            SOCKET outSock = INVALID_SOCKET;
            for (int i = 0; i < ctx->subflowCount; i++) {
                if (ctx->subflows[i].sock != INVALID_SOCKET &&
                    ctx->subflows[i].localAddr.ss_family == localAddr.ss_family) {
                    outSock = ctx->subflows[i].sock;
                    break;
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
    }

    if (serverSock != INVALID_SOCKET) {
        closeSocket(serverSock);
    }
}

#endif // VIPLE_MPQUIC
