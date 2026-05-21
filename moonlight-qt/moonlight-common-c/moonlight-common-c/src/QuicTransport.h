#pragma once

#ifdef VIPLE_MPQUIC

#include "Platform.h"
#include "PlatformSockets.h"
#include "PlatformNetIf.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Flow type tags for datagram framing ──────────────────────
// Each QUIC datagram carries a 4-byte header: [flow_type:1][reserved:1][seq:2]
// so the receiver can demux to the right stream handler.
#define QUIC_FLOW_VIDEO   0x01
#define QUIC_FLOW_AUDIO   0x02
#define QUIC_FLOW_CONTROL 0x03

// ── Scheduler strategies ─────────────────────────────────────
#define QUIC_SCHED_AUTO       0  // Per-flow default (ECF video, redundant audio, min-RTT control)
#define QUIC_SCHED_MIN_RTT    1  // Always pick the lowest-RTT subflow
#define QUIC_SCHED_AGGREGATE  2  // Round-robin across all active subflows
#define QUIC_SCHED_REDUNDANT  3  // Duplicate on every subflow
#define QUIC_SCHED_ECF        4  // Earliest Completion First

// ── Subflow stats ────────────────────────────────────────────
#define QUIC_MAX_SUBFLOWS 8

typedef struct _QUIC_SUBFLOW_STATS {
    int interfaceIndex;
    char interfaceName[LC_NETIF_MAX_NAME];
    int interfaceType;
    float rttMs;
    float throughputMbps;
    float lossPercent;
    float reorderPercent;
    bool active;
} QUIC_SUBFLOW_STATS, *PQUIC_SUBFLOW_STATS;

// ── Connection parameters ────────────────────────────────────
typedef struct _QUIC_CONNECT_PARAMS {
    struct sockaddr_storage remoteAddr;
    SOCKADDR_LEN remoteAddrLen;

    // SNI for TLS 1.3 handshake (typically server UUID)
    const char* sni;

    // Port the QUIC server is listening on (negotiated via RTSP SDP)
    unsigned short quicPort;

    // TLS session ticket for 0-RTT resumption (NULL if none)
    const unsigned char* sessionTicket;
    int sessionTicketLen;

    // X.509 cert/key paths for mutual TLS (server uses these)
    const char* certPath;
    const char* keyPath;

    // Peer certificate fingerprint for pinning (SHA-256, 32 bytes, NULL to skip)
    const unsigned char* peerCertFingerprint;
} QUIC_CONNECT_PARAMS, *PQUIC_CONNECT_PARAMS;

// ── Datagram frame header ────────────────────────────────────
// Prepended to every QUIC DATAGRAM frame payload.
// Total overhead: 4 bytes (vs. 24-byte relay tunnel header).
#pragma pack(push, 1)
typedef struct _QUIC_DGRAM_HEADER {
    unsigned char flowType;
    unsigned char reserved;
    unsigned short seq;       // big-endian sequence number (per-flow)
} QUIC_DGRAM_HEADER, *PQUIC_DGRAM_HEADER;
#pragma pack(pop)

#define QUIC_DGRAM_HEADER_SIZE 4

// ── Receive callback ─────────────────────────────────────────
// Called on the QUIC receive thread when a datagram or stream
// data arrives. `flowType` is one of QUIC_FLOW_*.
typedef void (*QuicRecvCallback)(unsigned char flowType,
                                 const unsigned char* data, int dataLen,
                                 void* context);

// ── Lifecycle ────────────────────────────────────────────────

// Initialize the QUIC transport subsystem (call once at startup).
// Returns 0 on success, -1 on failure.
int quicTransportInit(void);

// Tear down the subsystem (call once at shutdown).
void quicTransportCleanup(void);

// ── Client connection ────────────────────────────────────────

// Establish a QUIC connection to the server.
// Returns 0 on success, -1 on failure.
int quicConnect(const QUIC_CONNECT_PARAMS* params);

// Gracefully close the QUIC connection.
void quicDisconnect(void);

// True if a QUIC connection is established and at least one
// subflow is active.
bool quicIsConnected(void);

// ── Server listener ──────────────────────────────────────────
// (Used by Sunshine server — not called from moonlight-common-c
// client code, but declared here for API completeness.)

int quicServerStart(unsigned short port, const char* certPath,
                    const char* keyPath);
void quicServerStop(void);

// ── Subflow management ───────────────────────────────────────

// Add a new subflow bound to the given local interface.
// Returns subflow ID (>= 0) on success, -1 on failure.
int quicAddSubflow(int interfaceIndex,
                   const struct sockaddr_storage* localAddr,
                   SOCKADDR_LEN addrLen);

// Remove a subflow by ID. Returns 0 on success, -1 if not found.
int quicRemoveSubflow(int subflowId);

// Set the scheduling strategy for a specific flow type,
// or for all flows if flowType == 0.
int quicSetScheduler(unsigned char flowType, int strategy);

// ── Datagram I/O ─────────────────────────────────────────────

// Send an unreliable datagram tagged with the given flow type.
// The QUIC_DGRAM_HEADER is prepended automatically.
// Returns 0 on success, -1 on failure.
int quicSendDatagram(unsigned char flowType,
                     const unsigned char* data, int dataLen);

// Register a callback for incoming datagrams.
// Called on the QUIC I/O thread; must be non-blocking.
void quicSetRecvCallback(QuicRecvCallback callback, void* context);

// ── Reliable stream I/O (for control channel) ────────────────

// Send data on the reliable control stream (QUIC stream #0).
// Returns 0 on success, -1 on failure.
int quicSendStream(const unsigned char* data, int dataLen);

// ── Monitoring ───────────────────────────────────────────────

// Fill `out` with stats for up to `maxCount` subflows.
// Returns the number of active subflows, or -1 on error.
int quicGetSubflowStats(PQUIC_SUBFLOW_STATS out, int maxCount);

// Return the number of currently active subflows.
int quicGetActiveSubflowCount(void);

// ── Session ticket (0-RTT) ───────────────────────────────────

// After a successful connection, retrieve the session ticket
// for future 0-RTT resumption. Returns ticket length, or 0
// if no ticket is available. Caller provides buffer.
int quicGetSessionTicket(unsigned char* buf, int bufLen);

#ifdef __cplusplus
}
#endif

#endif // VIPLE_MPQUIC
