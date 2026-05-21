#include "Limelight-internal.h"

#ifdef VIPLE_MPQUIC

#include "QuicTransport.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <picoquic.h>
#include <picoquic_utils.h>
#include <picosocks.h>

// ── Internal constants ──────────────────────────────────────

#define PROBE_TIMEOUT_THRESHOLD  5     // consecutive probe failures → inactive
#define HEALTH_CHECK_INTERVAL_US 500000 // 500ms between health checks
#define STATS_UPDATE_INTERVAL_US 100000 // 100ms between stats refresh
#define SESSION_TICKET_MAX_SIZE  4096

// ── Internal state ──────────────────────────────────────────

typedef struct _QUIC_SUBFLOW {
    int id;
    int interfaceIndex;
    SOCKET sock;
    struct sockaddr_storage localAddr;
    SOCKADDR_LEN addrLen;
    bool active;
    int picoquicPathId;

    // Per-subflow stats (updated from picoquic path stats)
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

    // 0-RTT session ticket
    unsigned char sessionTicket[SESSION_TICKET_MAX_SIZE];
    int sessionTicketLen;
    bool ticketReady;
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

    g_ctx.quic = picoquic_create(
        1,              // max connections (client = 1)
        NULL,           // cert (client doesn't serve)
        NULL,           // key
        NULL,           // cert_root (trust via fingerprint pinning)
        "viplestream",  // ALPN
        quicDgramCallback,
        &g_ctx,
        NULL, NULL, NULL,
        currentTime,
        NULL, NULL, NULL, 0
    );

    if (!g_ctx.quic) {
        Limelog("[VIPLE-MPQUIC] Failed to create QUIC context\n");
        return -1;
    }

    picoquic_set_default_datagram_option(g_ctx.quic, 1);
    picoquic_set_default_multipath_option(g_ctx.quic, 1);
    quicApplyCongestionAlgo(g_ctx.quic);

    // Copy server address with negotiated QUIC port
    memcpy(&serverAddr, &params->remoteAddr, params->remoteAddrLen);
    if (serverAddr.ss_family == AF_INET) {
        ((struct sockaddr_in*)&serverAddr)->sin_port = htons(params->quicPort);
    } else {
        ((struct sockaddr_in6*)&serverAddr)->sin6_port = htons(params->quicPort);
    }

    g_ctx.cnx = picoquic_create_cnx(
        g_ctx.quic,
        picoquic_null_connection_id,
        picoquic_null_connection_id,
        (struct sockaddr*)&serverAddr,
        currentTime,
        0,              // preferred version
        params->sni,
        "viplestream",  // ALPN
        1               // client mode
    );

    if (!g_ctx.cnx) {
        Limelog("[VIPLE-MPQUIC] Failed to create QUIC connection\n");
        picoquic_free(g_ctx.quic);
        g_ctx.quic = NULL;
        return -1;
    }

    picoquic_set_multipath_option(g_ctx.cnx, 1);
    picoquic_mark_datagram_ready(g_ctx.cnx, 1);

    // Apply 0-RTT session ticket if available
    if (params->sessionTicket && params->sessionTicketLen > 0) {
        ret = picoquic_set_0rtt_ticket(g_ctx.cnx,
            params->sessionTicket, (uint16_t)params->sessionTicketLen);
        if (ret == 0) {
            Limelog("[VIPLE-MPQUIC] Applied 0-RTT session ticket (%d bytes)\n",
                    params->sessionTicketLen);
        }
    }

    g_ctx.ticketReady = false;
    g_ctx.sessionTicketLen = 0;
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
        PltCloseThread(&g_ctx.ioThread);

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
        certPath, keyPath,
        NULL, "viplestream",
        quicDgramCallback, &g_ctx,
        NULL, NULL, NULL,
        currentTime,
        NULL, NULL, NULL, 0
    );

    if (!g_ctx.quic) {
        Limelog("[VIPLE-MPQUIC] Failed to create server QUIC context\n");
        return -1;
    }

    picoquic_set_default_datagram_option(g_ctx.quic, 1);
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
        PltCloseThread(&g_ctx.ioThread);
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

    // Register with picoquic multipath
    picoquic_probe_new_path(g_ctx.cnx,
        (const struct sockaddr*)localAddr,
        (const struct sockaddr*)&g_ctx.cnx->path[0]->peer_addr,
        picoquic_current_time());

    // Map to picoquic path ID (current path count - 1 after probe)
    sf->picoquicPathId = picoquic_get_cnx_nb_paths(g_ctx.cnx) - 1;

    g_ctx.subflowCount++;
    Limelog("[VIPLE-MPQUIC] Added subflow %d on interface %d (path %d)\n",
            sf->id, interfaceIndex, sf->picoquicPathId);
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

    // picoquic handles multipath internally; we hint the preferred path
    // by setting path affinity before queuing
    if (pathIdx >= 0 && pathIdx < g_ctx.subflowCount) {
        int pqPath = g_ctx.subflows[pathIdx].picoquicPathId;
        picoquic_set_cnx_path_priority(g_ctx.cnx, pqPath, 1);
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
        // REDUNDANT: send on every active subflow
        int sent = 0;
        for (int i = 0; i < g_ctx.subflowCount; i++) {
            if (g_ctx.subflows[i].active) {
                if (quicSendOnPath(i, flowType, data, dataLen) == 0)
                    sent++;
            }
        }
        return (sent > 0) ? 0 : -1;
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
    int pathCount;
    uint64_t now;

    if (!g_ctx.cnx)
        return;

    now = picoquic_current_time();
    if (now - g_ctx.lastStatsUpdate < STATS_UPDATE_INTERVAL_US)
        return;
    g_ctx.lastStatsUpdate = now;

    pathCount = picoquic_get_cnx_nb_paths(g_ctx.cnx);

    for (int i = 0; i < g_ctx.subflowCount; i++) {
        int pqPath = g_ctx.subflows[i].picoquicPathId;
        if (pqPath < 0 || pqPath >= pathCount)
            continue;

        // RTT in microseconds from picoquic
        uint64_t rttUs = picoquic_get_cnx_path_rtt(g_ctx.cnx, pqPath);
        g_ctx.subflows[i].rttMs = (float)rttUs / 1000.0f;

        // Estimate throughput from CWIN and RTT
        uint64_t cwin = picoquic_get_cnx_path_cwin(g_ctx.cnx, pqPath);
        if (rttUs > 0) {
            // throughput ≈ CWIN / RTT (bytes/sec → Mbps)
            double bps = ((double)cwin / (double)rttUs) * 1e6;
            g_ctx.subflows[i].throughputMbps = (float)(bps / 1e6 * 8.0);
        }
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
    if (!g_ctx.ticketReady || g_ctx.sessionTicketLen == 0)
        return 0;
    if (bufLen < g_ctx.sessionTicketLen)
        return 0;
    memcpy(buf, g_ctx.sessionTicket, g_ctx.sessionTicketLen);
    return g_ctx.sessionTicketLen;
}

static void quicTryExtractTicket(void) {
    const uint8_t* ticket;
    uint16_t ticketLen;

    if (g_ctx.ticketReady || !g_ctx.cnx)
        return;

    // picoquic stores the TLS session ticket after handshake
    ticket = picoquic_get_app_message(g_ctx.cnx, &ticketLen);
    if (ticket && ticketLen > 0 && ticketLen <= SESSION_TICKET_MAX_SIZE) {
        memcpy(g_ctx.sessionTicket, ticket, ticketLen);
        g_ctx.sessionTicketLen = ticketLen;
        g_ctx.ticketReady = true;
        Limelog("[VIPLE-MPQUIC] Session ticket captured (%d bytes)\n", ticketLen);
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
        if (length >= QUIC_DGRAM_HEADER_SIZE && ctx->recvCallback) {
            PQUIC_DGRAM_HEADER hdr = (PQUIC_DGRAM_HEADER)bytes;
            ctx->recvCallback(
                hdr->flowType,
                bytes + QUIC_DGRAM_HEADER_SIZE,
                (int)(length - QUIC_DGRAM_HEADER_SIZE),
                ctx->recvContext);
        }
        // Update recv time on all subflows (we can't easily determine
        // which picoquic path received this datagram from the callback)
        {
            uint64_t now = picoquic_current_time();
            int pathId = picoquic_get_cnx_last_recv_path(cnx);
            for (int i = 0; i < ctx->subflowCount; i++) {
                if (ctx->subflows[i].picoquicPathId == pathId) {
                    ctx->subflows[i].lastRecvTime = now;
                    ctx->subflows[i].bytesRecv += length;
                    break;
                }
            }
        }
        break;

    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (stream_id == 0 && ctx->recvCallback) {
            ctx->recvCallback(QUIC_FLOW_CONTROL, bytes, (int)length,
                              ctx->recvContext);
        }
        break;

    case picoquic_callback_ready:
        Limelog("[VIPLE-MPQUIC] Connection ready (handshake complete)\n");
        quicTryExtractTicket();
        break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
        Limelog("[VIPLE-MPQUIC] Connection closed\n");
        break;

    case picoquic_callback_path_available:
        Limelog("[VIPLE-MPQUIC] New path available (id=%d)\n",
                (int)picoquic_get_cnx_nb_paths(cnx) - 1);
        break;

    case picoquic_callback_path_suspended:
        Limelog("[VIPLE-MPQUIC] Path suspended\n");
        break;

    case picoquic_callback_path_deleted:
        Limelog("[VIPLE-MPQUIC] Path deleted\n");
        break;

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

                // Check each subflow socket
                for (int i = 0; i < ctx->subflowCount; i++) {
                    SOCKET sf = ctx->subflows[i].sock;
                    if (sf != INVALID_SOCKET && FD_ISSET(sf, &readSet)) {
                        peerLen = sizeof(peerAddr);
                        SOCK_RET recvLen = recvfrom(sf, (char*)recvBuf,
                            sizeof(recvBuf), 0,
                            (struct sockaddr*)&peerAddr, &peerLen);
                        if (recvLen > 0) {
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

        // Process picoquic state machine
        picoquic_cnx_t* iterCnx = picoquic_get_earliest_cnx_to_wake(
            ctx->quic, currentTime + 1000);

        while (iterCnx != NULL) {
            struct sockaddr_storage destAddr, localAddr;
            SOCKADDR_LEN destLen = sizeof(destAddr);
            SOCKADDR_LEN localLen = sizeof(localAddr);
            size_t sendLen = 0;

            int ret = picoquic_prepare_next_packet_ex(iterCnx,
                picoquic_current_time(),
                sendBuf, sizeof(sendBuf), &sendLen,
                (struct sockaddr*)&destAddr, &destLen,
                (struct sockaddr*)&localAddr, &localLen,
                NULL, NULL, NULL);

            if (ret == 0 && sendLen > 0) {
                // Find matching subflow socket for the local address
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

            if (ret != 0 || sendLen == 0)
                break;
        }

        // Periodic maintenance
        quicUpdatePathStats();
        quicCheckPathHealth();

        // Try to extract session ticket (may become available after handshake)
        if (!ctx->ticketReady) {
            quicTryExtractTicket();
        }
    }

    if (serverSock != INVALID_SOCKET) {
        closeSocket(serverSock);
    }
}

#endif // VIPLE_MPQUIC
