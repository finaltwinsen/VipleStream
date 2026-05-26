/**
 * @file src/quic_server.h
 * @brief VipleStream MP-QUIC server: QUIC listener + multipath subflow
 *        management for streaming sessions.
 *
 * When MP-QUIC is negotiated, the server opens a QUIC listener on
 * config::stream.mpquic_port. Each streaming session gets a dedicated
 * picoquic connection. Video and audio are sent as QUIC DATAGRAMs
 * (RFC 9221); control uses a reliable QUIC stream (#0).
 *
 * The server drives multipath from its side by probing paths that the
 * client advertises through QUIC path validation (RFC 9443 §5).
 */
#pragma once

#ifdef VIPLE_MPQUIC

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

// picoquic uses anonymous-enum typedefs and tag-name typedefs that
// can't be forward-declared cleanly. Just include the full header.
#include <picoquic.h>

struct _reed_solomon;

namespace quic_server {

  struct SubflowStats {
    int pathId;
    float rttMs;
    float throughputMbps;
    float lossPercent;
    bool active;
    uint64_t bytesSent;
    uint64_t bytesRecv;
  };

  using RecvHandler = std::function<void(uint8_t flowType,
                                         const uint8_t *data, size_t len)>;

  class QuicSession {
  public:
    QuicSession(picoquic_cnx_t *cnx);
    ~QuicSession();

    QuicSession(const QuicSession &) = delete;
    QuicSession &operator=(const QuicSession &) = delete;

    bool sendDatagram(uint8_t flowType, const uint8_t *data, size_t len);

    // Scheduler-aware datagram send (picks best path based on strategy)
    bool sendDatagramScheduled(uint8_t flowType, const uint8_t *data,
                               size_t len, int scheduler);

    bool sendStream(const uint8_t *data, size_t len);

    void setRecvHandler(RecvHandler handler);

    std::vector<SubflowStats> getStats() const;

    bool isReady() const;

    // Drain all pending datagrams into picoquic — MUST be called from
    // the IO thread only (picoquic is single-threaded).
    void drainPendingToQuic();

  private:
    friend class QuicListener;

    picoquic_cnx_t *_cnx;
    RecvHandler _recvHandler;

    // §K.4 fix: video/audio threads enqueue into _pendingQueue;
    // the IO thread drains it via drainPendingToQuic().
    // All picoquic_queue_datagram_frame calls happen on the IO thread.
    struct PendingDgram {
      uint8_t flowType;
      int scheduler;
      bool isStream;          // true → reliable stream #0; false → datagram
      std::vector<uint8_t> data;
    };
    std::mutex _pendingMutex;
    std::vector<PendingDgram> _pendingQueue;

    uint16_t _seqCounters[4] = {};

    // Picoquic does not expose a public "get number of paths" API, so we
    // track active path ids ourselves via picoquic_callback_path_available
    // / _suspended / _deleted. Path 0 is always present (the initial cnx
    // path), so we start at 1.
    mutable std::mutex _pathMutex;
    std::vector<uint64_t> _activePaths{0};

    // RR cursor for REDUNDANT scheduler (rotates the preferred path so
    // picoquic spreads load across subflows over consecutive frames).
    int _redundantRR = 0;

    // §K.7 diag: datagram queue/send 計數器
    std::atomic<uint64_t> _dgramQueued{0};
    std::atomic<uint64_t> _dgramPushed{0};
    std::atomic<uint64_t> _dgramDroppedStale{0};
    std::atomic<uint64_t> _dgramPendingPeak{0};

    // §K.10 diag: per-flow 計數器 (index = flowType: 0=unused, 1=video, 2=audio, 3=control)
    std::atomic<uint64_t> _dgramQueuedByFlow[4]{};
    std::atomic<uint64_t> _dgramFailedByFlow[4]{};
    std::atomic<uint64_t> _bytesByFlow[4]{};
    // 上次 stats snapshot（用於計算 delta 速率）
    uint64_t _prevBytesByFlow[4] = {};
    std::chrono::steady_clock::time_point _prevStatsTime = std::chrono::steady_clock::now();

    // §5b FEC: video datagram RS encoding (4 data + 2 parity)
    struct FecGroupState {
      std::vector<uint8_t> payloads[4];
      uint16_t seqs[4] = {};
      int count = 0;
      uint16_t baseSeq = 0;
    };
    FecGroupState _fecGroup;
    ::_reed_solomon *_fecRs = nullptr;
    void flushFecParity(int scheduler);
  };

  class QuicListener {
  public:
    QuicListener();
    ~QuicListener();

    QuicListener(const QuicListener &) = delete;
    QuicListener &operator=(const QuicListener &) = delete;

    /**
     * Start the QUIC listener.
     * @param port       UDP port to bind
     * @param certPath   PEM certificate path
     * @param keyPath    PEM private key path
     * @return true on success
     */
    bool start(uint16_t port,
               const std::string &certPath,
               const std::string &keyPath);

    void stop();

    bool isRunning() const { return _running.load(); }

    /**
     * Get the active session for a given client address, or nullptr.
     * Thread-safe.
     */
    std::shared_ptr<QuicSession> getSession(
        const boost::asio::ip::address &clientAddr);

    /**
     * Called when a new QUIC session is accepted (after handshake).
     */
    using SessionCallback = std::function<void(std::shared_ptr<QuicSession>)>;
    void setSessionCallback(SessionCallback cb);

    // Log aggregate stats for all sessions (called periodically)
    void logStats();

  private:
    void ioLoop();

    static int picoquicCallback(picoquic_cnx_t *cnx,
        uint64_t stream_id, uint8_t *bytes, size_t length,
        picoquic_call_back_event_t fin_or_event,
        void *callback_ctx, void *stream_ctx);

    picoquic_quic_t *_quic = nullptr;
    std::thread _ioThread;
    std::atomic<bool> _running{false};

    std::mutex _sessionMutex;
    std::unordered_map<std::string, std::shared_ptr<QuicSession>> _sessions;

    SessionCallback _sessionCallback;
    uint16_t _port = 0;

    // Periodic stats logging
    std::thread _statsThread;
    std::atomic<bool> _statsRunning{false};
    void statsLoop();
  };

  // Global listener instance (created by stream.cpp when MP-QUIC is active)
  inline QuicListener *g_listener = nullptr;

}  // namespace quic_server

#endif  // VIPLE_MPQUIC
