#include "Limelight-internal.h"

#ifdef VIPLE_MPQUIC

#include "QuicTransport.h"

#include <string.h>
#include <stdlib.h>

#include <picoquic.h>
#include <picoquic_utils.h>
#include <picosocks.h>

// ── Internal state ───────────────────────────────────────────

typedef struct _QUIC_SUBFLOW {
    int id;
    int interfaceIndex;
    SOCKET sock;
    struct sockaddr_storage localAddr;
    SOCKADDR_LEN addrLen;
    bool active;
    // Per-subflow stats (updated by picoquic callbacks)
    float rttMs;
    float throughputMbps;
    float lossPercent;
    float reorderPercent;
    char name[LC_NETIF_MAX_NAME];
    int type;
} QUIC_SUBFLOW;

typedef struct _QUIC_TRANSPORT_CTX {
    picoquic_quic_t* quic;
    picoquic_cnx_t* cnx;

    QUIC_SUBFLOW subflows[QUIC_MAX_SUBFLOWS];
    int subflowCount;
    int nextSubflowId;

    int scheduler[4]; // index 0 = default, 1..3 = per QUIC_FLOW_*

    // Receive callback
    QuicRecvCallback recvCallback;
    void* recvContext;

    // Per-flow sequence counters (for datagram header)
    unsigned short seqCounters[4];

    // I/O thread
    PLT_THREAD ioThread;
    bool ioRunning;

    // Server mode
    bool isServer;
    unsigned short serverPort;
} QUIC_TRANSPORT_CTX;

static QUIC_TRANSPORT_CTX g_ctx;
static bool g_initialized = false;

// ── Forward declarations ─────────────────────────────────────

static void quicIoThreadProc(void* context);
static int quicDgramCallback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx,
    void* stream_ctx);

// ── Lifecycle ────────────────────────────────────────────────

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

// ── Client connection ────────────────────────────────────────

int quicConnect(const QUIC_CONNECT_PARAMS* params) {
    struct sockaddr_storage serverAddr;
    uint64_t currentTime;
    int ret;

    if (!g_initialized)
        return -1;
    if (g_ctx.cnx)
        return -1; // already connected

    currentTime = picoquic_current_time();

    g_ctx.quic = picoquic_create(
        1,              // max connections (client = 1)
        NULL,           // cert (client doesn't serve)
        NULL,           // key
        NULL,           // cert_root (trust via fingerprint pinning)
        "viplestream",  // ALPN
        quicDgramCallback,
        &g_ctx,
        NULL,           // cnx_id_callback
        NULL,           // cnx_id_callback_ctx
        NULL,           // reset_seed
        currentTime,
        NULL,           // simulated_time
        NULL,           // ticket_file (we handle tickets ourselves)
        NULL,           // ticket_encryption_key
        0               // ticket_encryption_key_length
    );

    if (!g_ctx.quic) {
        Limelog("[VIPLE-MPQUIC] Failed to create QUIC context\n");
        return -1;
    }

    // Enable QUIC datagram extension (RFC 9221)
    picoquic_set_default_datagram_option(g_ctx.quic, 1);

    // Enable multipath (RFC 9443)
    picoquic_set_default_multipath_option(g_ctx.quic, 1);

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

    // Enable multipath on this connection
    picoquic_set_multipath_option(g_ctx.cnx, 1);

    // Enable datagram on this connection
    picoquic_mark_datagram_ready(g_ctx.cnx, 1);

    // Apply 0-RTT session ticket if available
    if (params->sessionTicket && params->sessionTicketLen > 0) {
        ret = picoquic_set_0rtt_ticket(g_ctx.cnx,
            params->sessionTicket, (uint16_t)params->sessionTicketLen);
        if (ret == 0) {
            Limelog("[VIPLE-MPQUIC] Applied 0-RTT session ticket\n");
        }
    }

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

    // Close all subflow sockets
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

// ── Server ───────────────────────────────────────────────────

int quicServerStart(unsigned short port, const char* certPath,
                    const char* keyPath) {
    uint64_t currentTime;

    if (!g_initialized)
        return -1;

    currentTime = picoquic_current_time();

    g_ctx.quic = picoquic_create(
        128,            // max connections
        certPath,
        keyPath,
        NULL,           // cert_root
        "viplestream",  // ALPN
        quicDgramCallback,
        &g_ctx,
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

// ── Subflow management ───────────────────────────────────────

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

    // Register with picoquic multipath
    picoquic_probe_new_path(g_ctx.cnx,
        (const struct sockaddr*)localAddr,
        (const struct sockaddr*)&g_ctx.cnx->path[0]->peer_addr,
        picoquic_current_time());

    g_ctx.subflowCount++;
    Limelog("[VIPLE-MPQUIC] Added subflow %d on interface %d\n",
            sf->id, interfaceIndex);
    return sf->id;
}

int quicRemoveSubflow(int subflowId) {
    for (int i = 0; i < g_ctx.subflowCount; i++) {
        if (g_ctx.subflows[i].id == subflowId) {
            if (g_ctx.subflows[i].sock != INVALID_SOCKET) {
                closeSocket(g_ctx.subflows[i].sock);
            }
            // Shift remaining subflows
            for (int j = i; j < g_ctx.subflowCount - 1; j++) {
                g_ctx.subflows[j] = g_ctx.subflows[j + 1];
            }
            g_ctx.subflowCount--;
            Limelog("[VIPLE-MPQUIC] Removed subflow %d\n", subflowId);
            return 0;
        }
    }
    return -1;
}

int quicSetScheduler(unsigned char flowType, int strategy) {
    if (flowType > 3 || strategy < 0 || strategy > QUIC_SCHED_ECF)
        return -1;
    g_ctx.scheduler[flowType] = strategy;
    return 0;
}

// ── Datagram I/O ─────────────────────────────────────────────

int quicSendDatagram(unsigned char flowType,
                     const unsigned char* data, int dataLen) {
    unsigned char* frame;
    QUIC_DGRAM_HEADER hdr;
    int frameLen;

    if (!quicIsConnected())
        return -1;

    frameLen = QUIC_DGRAM_HEADER_SIZE + dataLen;
    frame = (unsigned char*)malloc(frameLen);
    if (!frame)
        return -1;

    // Build datagram header
    hdr.flowType = flowType;
    hdr.reserved = 0;
    hdr.seq = htons(g_ctx.seqCounters[flowType]++);
    memcpy(frame, &hdr, QUIC_DGRAM_HEADER_SIZE);
    memcpy(frame + QUIC_DGRAM_HEADER_SIZE, data, dataLen);

    int ret = picoquic_queue_datagram_frame(g_ctx.cnx, frameLen, frame);

    free(frame);

    if (ret != 0) {
        return -1;
    }

    return 0;
}

void quicSetRecvCallback(QuicRecvCallback callback, void* context) {
    g_ctx.recvCallback = callback;
    g_ctx.recvContext = context;
}

// ── Reliable stream I/O ──────────────────────────────────────

int quicSendStream(const unsigned char* data, int dataLen) {
    if (!quicIsConnected())
        return -1;

    // Stream 0 is reserved for reliable control data
    int ret = picoquic_add_to_stream(g_ctx.cnx, 0, data, dataLen, 0);
    return (ret == 0) ? 0 : -1;
}

// ── Monitoring ───────────────────────────────────────────────

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

// ── Session ticket ───────────────────────────────────────────

int quicGetSessionTicket(unsigned char* buf, int bufLen) {
    // TODO: Extract session ticket from picoquic after handshake
    (void)buf;
    (void)bufLen;
    return 0;
}

// ── picoquic callbacks ───────────────────────────────────────

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
        break;

    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        // Reliable stream data (control channel on stream 0)
        if (stream_id == 0 && ctx->recvCallback) {
            ctx->recvCallback(QUIC_FLOW_CONTROL, bytes, (int)length,
                              ctx->recvContext);
        }
        break;

    case picoquic_callback_ready:
        Limelog("[VIPLE-MPQUIC] Connection ready\n");
        break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
        Limelog("[VIPLE-MPQUIC] Connection closed\n");
        break;

    default:
        break;
    }

    return 0;
}

// ── I/O thread ───────────────────────────────────────────────
// Drives the picoquic event loop: polls sockets, processes
// incoming packets, triggers sends.

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
        int64_t deltaT = 0;
        struct sockaddr_storage peerAddr;
        SOCKADDR_LEN peerLen = sizeof(peerAddr);
        uint64_t currentTime = picoquic_current_time();

        // Determine which socket to poll
        SOCKET pollSock = INVALID_SOCKET;
        if (ctx->isServer) {
            pollSock = serverSock;
        } else if (ctx->subflowCount > 0) {
            pollSock = ctx->subflows[0].sock;
        }

        if (pollSock != INVALID_SOCKET) {
            // Non-blocking poll with short timeout
            fd_set readSet;
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 1000; // 1ms poll
            FD_ZERO(&readSet);
            FD_SET(pollSock, &readSet);

            int selRet = select((int)(pollSock + 1), &readSet, NULL, NULL, &tv);
            if (selRet > 0) {
                SOCK_RET recvLen = recvfrom(pollSock, (char*)recvBuf,
                    sizeof(recvBuf), 0,
                    (struct sockaddr*)&peerAddr, &peerLen);

                if (recvLen > 0) {
                    picoquic_incoming_packet(ctx->quic,
                        recvBuf, (size_t)recvLen,
                        (struct sockaddr*)&peerAddr,
                        NULL, // local addr (not needed for single-socket)
                        0,    // if_index
                        0,    // ECN
                        currentTime);
                }
            }
        } else {
            PltSleepMs(1);
        }

        // Process picoquic state machine: generate outgoing packets
        picoquic_cnx_t* iterCnx = picoquic_get_earliest_cnx_to_wake(
            ctx->quic, currentTime + 1000);

        while (iterCnx != NULL) {
            struct sockaddr_storage destAddr;
            SOCKADDR_LEN destLen = sizeof(destAddr);
            size_t sendLen = 0;
            int ret;

            ret = picoquic_prepare_next_packet(iterCnx,
                picoquic_current_time(),
                sendBuf, sizeof(sendBuf), &sendLen,
                (struct sockaddr*)&destAddr, &destLen,
                NULL, NULL, NULL);

            if (ret == 0 && sendLen > 0) {
                SOCKET outSock = pollSock;
                if (outSock != INVALID_SOCKET) {
                    sendto(outSock, (const char*)sendBuf, (int)sendLen, 0,
                           (struct sockaddr*)&destAddr, destLen);
                }
            }

            if (ret != 0 || sendLen == 0)
                break;
        }
    }

    if (serverSock != INVALID_SOCKET) {
        closeSocket(serverSock);
    }
}

#endif // VIPLE_MPQUIC
