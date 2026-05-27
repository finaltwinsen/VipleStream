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
#define QUIC_FLOW_INPUT   0x04  // §Q-INPUT-QUIC-FALLBACK: failover 時鍵鼠走 QUIC datagram
#define QUIC_FLOW_COUNT   5     // 0=default, 1-4=per flow

// ── Scheduler strategies ─────────────────────────────────────
#define QUIC_SCHED_AUTO       0  // Per-flow default (ECF video, redundant audio, min-RTT control)
#define QUIC_SCHED_MIN_RTT    1  // Always pick the lowest-RTT subflow
#define QUIC_SCHED_AGGREGATE  2  // Round-robin across all active subflows
#define QUIC_SCHED_REDUNDANT  3  // Duplicate on every subflow
#define QUIC_SCHED_ECF        4  // Earliest Completion First

// ── Congestion control algorithms ───────────────────────────
#define QUIC_CC_NEWRENO  0
#define QUIC_CC_BBR      1  // Bandwidth-based, preferred for low-latency streaming
#define QUIC_CC_CUBIC    2

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

// ── Failover callback ────────────────────────────────────────
// Called when a subflow changes state (active ↔ inactive).
// `reactivated` is true when a dead path comes back alive.
typedef void (*QuicFailoverCallback)(int subflowId, int interfaceIndex,
                                     bool reactivated, void* context);

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

// Block up to `timeoutMs` waiting for the connection to reach
// picoquic_state_ready (i.e. TLS handshake complete, AEAD contexts
// installed).  Polls every 10 ms.  Returns 0 if ready, -1 on timeout,
// -2 if the connection does not exist.
//
// §Q-REVIEW-P2 (v1.5.25+1, dev/cli-quic-linux): added because
// quicAddSubflow's picoquic_probe_new_path used to fire before the
// initial path's crypto context existed.  picoquic queued the probe
// packet anyway, and the QuicIO thread crashed inside
// picoquic_aead_get_checksum_length (NULL `algo` pointer in the
// non-installed AEAD context) the moment it tried to prepare and
// protect the probe.  Gating quicAddSubflow on this readiness
// check eliminates the segfault.
int quicWaitReady(unsigned int timeoutMs);

// ── Server listener ──────────────────────────────────────────
// (Used by Sunshine server — not called from moonlight-common-c
// client code, but declared here for API completeness.)

int quicServerStart(unsigned short port, const char* certPath,
                    const char* keyPath);
void quicServerStop(void);

// ── Subflow management ───────────────────────────────────────

// Add a new subflow bound to the given local interface.
// interfaceName / interfaceType are stored for overlay display.
// Returns subflow ID (>= 0) on success, -1 on failure.
int quicAddSubflow(int interfaceIndex,
                   const char* interfaceName,
                   int interfaceType,
                   const struct sockaddr_storage* localAddr,
                   SOCKADDR_LEN addrLen);

// Same as quicAddSubflow but with an optional peer-address override.
// When peerOverride is non-NULL, the routing reachability probe and the
// picoquic_probe_new_path call both use this addr instead of the global
// peerAddr cached in quicConnect.  This lets a single QUIC cnx span
// multiple server endpoints — e.g. path 0 = server LAN IP via the LAN
// NIC, path 1 = server Tailscale IP via the Tailscale tunnel, path 2 =
// server WAN IP via cellular.  picoquic routes both paths to the same
// cnx by connection ID, regardless of peer addr.
int quicAddSubflowEx(int interfaceIndex,
                     const char* interfaceName,
                     int interfaceType,
                     const struct sockaddr_storage* localAddr,
                     SOCKADDR_LEN localAddrLen,
                     const struct sockaddr_storage* peerOverride,
                     SOCKADDR_LEN peerOverrideLen);

// Remove a subflow by ID. Returns 0 on success, -1 if not found.
int quicRemoveSubflow(int subflowId);

// §MP-ADV: 儲存伺服器的所有 alt peer 位址（供未來動態路徑管理用）。
// Connection.c Phase B 透過 quicAddSubflowEx 的 peerOverride 直接傳入，
// 這裡的儲存是為了 quicRecheckPaths 將來需要時可用。
void quicSetAltPeers(const struct sockaddr_storage* addrs,
                     const SOCKADDR_LEN* addrLens,
                     int count);

// Set the scheduling strategy for a specific flow type,
// or for all flows if flowType == 0.
int quicSetScheduler(unsigned char flowType, int strategy);

// Set the congestion control algorithm. Must be called before
// quicConnect() or quicServerStart(). Default is QUIC_CC_BBR.
void quicSetCongestionAlgo(int algo);

// ── Datagram I/O ─────────────────────────────────────────────

// Send an unreliable datagram tagged with the given flow type.
// The QUIC_DGRAM_HEADER is prepended automatically.
// Returns 0 on success, -1 on failure.
int quicSendDatagram(unsigned char flowType,
                     const unsigned char* data, int dataLen);

// Register a callback for incoming datagrams (all flow types).
// Called on the QUIC I/O thread; must be non-blocking.
// 後向相容：設定所有 flow type 的 callback。新程式碼應用
// quicSetRecvCallbackForFlow 按 flow type 各別註冊。
void quicSetRecvCallback(QuicRecvCallback callback, void* context);

// Register a callback for a specific flow type (QUIC_FLOW_VIDEO,
// QUIC_FLOW_AUDIO, or QUIC_FLOW_CONTROL).  Each flow type has its
// own callback slot; datagrams are dispatched by flowType header.
void quicSetRecvCallbackForFlow(unsigned char flowType,
                                QuicRecvCallback callback, void* context);

// Register a callback for path failover events.
void quicSetFailoverCallback(QuicFailoverCallback callback, void* context);

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

// ── Video ring buffer diagnostics ────────────────────────────
// Called by IO thread's §K.10 diagnostic to report ring depth/drops.
void quicVideoGetRingStats(int* depth, int* capacity, uint64_t* drops);

// ── Failover status ─────────────────────────────────────────

// §Q-MP-FAILBACK: 查詢是否正在進行 failover。
// ControlStream 用此判斷是否要延遲 connectionTerminated。
// Returns 1 if a failover is in progress (primary dead, standby active).
int quicIsFailoverActive(void);

// ── Session ticket (0-RTT) ───────────────────────────────────

// Set the ticket store file path. picoquic auto-loads existing tickets
// at quicConnect() and auto-saves new ones at disconnect. The same file
// can hold tickets for many SNIs (picoquic keys them internally).
// Must be called BEFORE quicConnect() to take effect.
// Pass NULL or empty string to disable 0-RTT (default).
void quicSetTicketStorePath(const char* path);

// Deprecated: picoquic now persists tickets to disk via the file set by
// quicSetTicketStorePath(); the application no longer needs to extract
// the ticket blob manually. Kept for ABI compatibility — returns 0.
int quicGetSessionTicket(unsigned char* buf, int bufLen);

#ifdef __cplusplus
}
#endif

#endif // VIPLE_MPQUIC
