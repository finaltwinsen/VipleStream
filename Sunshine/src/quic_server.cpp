/**
 * @file src/quic_server.cpp
 * @brief VipleStream MP-QUIC server implementation.
 */
#ifdef VIPLE_MPQUIC

#include "quic_server.h"
#include "config.h"
#include "logging.h"

#include <picoquic.h>
#include <picoquic_utils.h>

#include <algorithm>
#include <cstring>

using namespace std::chrono_literals;

// Datagram frame header (must match moonlight-common-c QuicTransport.h)
#pragma pack(push, 1)
struct QuicDgramHeader {
  uint8_t flowType;
  uint8_t reserved;
  uint16_t seq;   // big-endian
};
#pragma pack(pop)

static constexpr size_t DGRAM_HDR_SIZE = sizeof(QuicDgramHeader);

// Flow type constants (must match QuicTransport.h)
static constexpr uint8_t FLOW_VIDEO   = 0x01;
static constexpr uint8_t FLOW_AUDIO   = 0x02;
static constexpr uint8_t FLOW_CONTROL = 0x03;

namespace quic_server {

  // ── QuicSession ──────────────────────────────────────────

  QuicSession::QuicSession(picoquic_cnx_t *cnx) : _cnx(cnx) {}

  QuicSession::~QuicSession() = default;

  bool QuicSession::isReady() const {
    return _cnx &&
           picoquic_get_cnx_state(_cnx) == picoquic_state_ready;
  }

  // Scheduler constants (must match QuicTransport.h)
  static constexpr int SCHED_AUTO      = 0;
  static constexpr int SCHED_MIN_RTT   = 1;
  static constexpr int SCHED_AGGREGATE = 2;
  static constexpr int SCHED_REDUNDANT = 3;
  static constexpr int SCHED_ECF       = 4;

  bool QuicSession::sendDatagram(uint8_t flowType,
                                  const uint8_t *data, size_t len) {
    if (!isReady())
      return false;

    std::vector<uint8_t> frame(DGRAM_HDR_SIZE + len);
    QuicDgramHeader hdr{};
    hdr.flowType = flowType;
    hdr.reserved = 0;
    hdr.seq = htons(_seqCounters[flowType]++);
    std::memcpy(frame.data(), &hdr, DGRAM_HDR_SIZE);
    std::memcpy(frame.data() + DGRAM_HDR_SIZE, data, len);

    std::lock_guard<std::mutex> lock(_sendMutex);
    int ret = picoquic_queue_datagram_frame(
        _cnx, frame.size(), frame.data());
    return ret == 0;
  }

  bool QuicSession::sendDatagramScheduled(uint8_t flowType,
                                           const uint8_t *data, size_t len,
                                           int scheduler) {
    if (!isReady())
      return false;

    // Snapshot the active path set (mutated by picoquic callbacks on the
    // I/O thread).
    std::vector<uint64_t> paths;
    {
      std::lock_guard<std::mutex> lock(_pathMutex);
      paths = _activePaths;
    }
    int pathCount = (int)paths.size();

    // Resolve AUTO per flow type
    if (scheduler == SCHED_AUTO) {
      switch (flowType) {
      case FLOW_VIDEO:   scheduler = SCHED_ECF; break;
      case FLOW_AUDIO:   scheduler = SCHED_REDUNDANT; break;
      case FLOW_CONTROL: scheduler = SCHED_MIN_RTT; break;
      default:           scheduler = SCHED_MIN_RTT; break;
      }
    }

    // Single-path: nothing to schedule, just send.
    if (pathCount <= 1) {
      return sendDatagram(flowType, data, len);
    }

    if (scheduler == SCHED_REDUNDANT) {
      // picoquic_queue_datagram_frame is per-cnx and dedupes identical
      // payloads, so we can't actually queue the same frame on every
      // path. Approximation: rotate which path picoquic prefers each
      // frame via set_path_status; over N frames this spreads load.
      // True per-path duplication needs an upstream picoquic feature.
      int preferred = (_redundantRR++) % pathCount;
      std::lock_guard<std::mutex> lock(_sendMutex);
      for (int i = 0; i < pathCount; i++) {
        picoquic_set_path_status(_cnx, paths[i],
            (i == preferred) ? picoquic_path_status_available
                             : picoquic_path_status_backup);
      }
      return sendDatagram(flowType, data, len);
    }

    if (scheduler == SCHED_AGGREGATE) {
      // Mark all paths available; picoquic will spread datagrams.
      std::lock_guard<std::mutex> lock(_sendMutex);
      for (uint64_t p : paths) {
        picoquic_set_path_status(_cnx, p, picoquic_path_status_available);
      }
      return sendDatagram(flowType, data, len);
    }

    // MIN_RTT / ECF: pick the best path by per-path quality
    int bestIdx = 0;
    float bestMetric = 1e9f;
    for (int i = 0; i < pathCount; i++) {
      picoquic_path_quality_t pq{};
      if (picoquic_get_path_quality(_cnx, paths[i], &pq) != 0)
        continue;

      float rttMs = (float)pq.rtt / 1000.0f;
      float metric;
      if (scheduler == SCHED_ECF) {
        // tput in Mbps from pacing_rate (bytes/sec)
        float tput = (float)((double)pq.pacing_rate * 8.0 / 1e6);
        if (tput < 0.001f) tput = 0.001f;
        // (size in bits / throughput in bps) + half-RTT in seconds
        float transferSec = ((float)len * 8.0f) / (tput * 1e6f);
        metric = transferSec + (rttMs / 2000.0f);
      } else {
        metric = rttMs;
      }
      if (metric < bestMetric) {
        bestMetric = metric;
        bestIdx = i;
      }
    }

    {
      std::lock_guard<std::mutex> lock(_sendMutex);
      for (int i = 0; i < pathCount; i++) {
        picoquic_set_path_status(_cnx, paths[i],
            (i == bestIdx) ? picoquic_path_status_available
                           : picoquic_path_status_backup);
      }
    }
    return sendDatagram(flowType, data, len);
  }

  bool QuicSession::sendStream(const uint8_t *data, size_t len) {
    if (!isReady())
      return false;

    std::lock_guard<std::mutex> lock(_sendMutex);
    int ret = picoquic_add_to_stream(_cnx, 0, data, len, 0);
    return ret == 0;
  }

  void QuicSession::setRecvHandler(RecvHandler handler) {
    _recvHandler = std::move(handler);
  }

  std::vector<SubflowStats> QuicSession::getStats() const {
    std::vector<SubflowStats> stats;
    if (!_cnx)
      return stats;

    std::vector<uint64_t> paths;
    {
      std::lock_guard<std::mutex> lock(_pathMutex);
      paths = _activePaths;
    }

    for (uint64_t pathId : paths) {
      picoquic_path_quality_t pq{};
      if (picoquic_get_path_quality(_cnx, pathId, &pq) != 0) {
        continue;
      }
      SubflowStats s{};
      s.pathId = (int)pathId;
      s.rttMs = (float)pq.rtt / 1000.0f;
      s.throughputMbps = (float)((double)pq.pacing_rate * 8.0 / 1e6);
      s.lossPercent = (pq.sent > 0)
          ? (float)((double)pq.lost / (double)pq.sent * 100.0)
          : 0.0f;
      s.active = true;
      s.bytesSent = pq.bytes_sent;
      s.bytesRecv = pq.bytes_received;
      stats.push_back(s);
    }

    return stats;
  }

  // ── QuicListener ─────────────────────────────────────────

  QuicListener::QuicListener() = default;

  QuicListener::~QuicListener() {
    stop();
  }

  bool QuicListener::start(uint16_t port,
                           const std::string &certPath,
                           const std::string &keyPath) {
    if (_running.load())
      return false;

    uint64_t currentTime = picoquic_current_time();

    _quic = picoquic_create(
        128,                       // max_nb_connections
        certPath.c_str(),
        keyPath.c_str(),
        nullptr,                   // cert_root
        "viplestream",             // default ALPN
        QuicListener::picoquicCallback,
        this,
        nullptr, nullptr, nullptr, // cnx_id_cb, reset_seed
        currentTime,
        nullptr,                   // simulated_time
        nullptr,                   // ticket_file_name (server doesn't load)
        nullptr, 0);               // ticket_encryption_key (default = random)

    if (!_quic) {
      BOOST_LOG(error) << "[VIPLE-MPQUIC] Failed to create QUIC server context";
      return false;
    }

    // Enable DATAGRAM by advertising a non-zero max_datagram_frame_size
    // transport parameter (picoquic has no separate "enable datagram"
    // option — it's purely TP-driven). Multipath enabled globally.
    picoquic_set_default_tp_value(_quic,
        picoquic_tp_max_datagram_frame_size, 65535);
    picoquic_set_default_multipath_option(_quic, 1);

    // Apply configured congestion control algorithm (BBR default)
    switch (config::stream.mpquic_congestion) {
      case 0:
        picoquic_set_default_congestion_algorithm(_quic, picoquic_newreno_algorithm);
        BOOST_LOG(info) << "[VIPLE-MPQUIC] Congestion: NewReno";
        break;
      case 2:
        picoquic_set_default_congestion_algorithm(_quic, picoquic_cubic_algorithm);
        BOOST_LOG(info) << "[VIPLE-MPQUIC] Congestion: Cubic";
        break;
      default:
        picoquic_set_default_congestion_algorithm(_quic, picoquic_bbr_algorithm);
        BOOST_LOG(info) << "[VIPLE-MPQUIC] Congestion: BBR";
        break;
    }

    _port = port;
    _running.store(true);
    _ioThread = std::thread(&QuicListener::ioLoop, this);

    // Start periodic stats logging (every 30s)
    _statsRunning.store(true);
    _statsThread = std::thread(&QuicListener::statsLoop, this);

    BOOST_LOG(info) << "[VIPLE-MPQUIC] Server listening on port " << port;
    return true;
  }

  void QuicListener::stop() {
    if (!_running.load())
      return;

    _running.store(false);
    _statsRunning.store(false);

    if (_ioThread.joinable())
      _ioThread.join();
    if (_statsThread.joinable())
      _statsThread.join();

    {
      std::lock_guard<std::mutex> lock(_sessionMutex);
      _sessions.clear();
    }

    if (_quic) {
      picoquic_free(_quic);
      _quic = nullptr;
    }

    BOOST_LOG(info) << "[VIPLE-MPQUIC] Server stopped";
  }

  std::shared_ptr<QuicSession> QuicListener::getSession(
      const boost::asio::ip::address &clientAddr) {
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _sessions.find(clientAddr.to_string());
    if (it != _sessions.end())
      return it->second;
    return nullptr;
  }

  void QuicListener::setSessionCallback(SessionCallback cb) {
    _sessionCallback = std::move(cb);
  }

  void QuicListener::ioLoop() {
    uint8_t recvBuf[2048];
    uint8_t sendBuf[2048];

    // Bind dual-stack UDP socket
    struct sockaddr_in6 listenAddr{};
    listenAddr.sin6_family = AF_INET6;
    listenAddr.sin6_port = htons(_port);
    listenAddr.sin6_addr = in6addr_any;

    SOCKET sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
      BOOST_LOG(error) << "[VIPLE-MPQUIC] Failed to create server socket";
      return;
    }

    int v6only = 0;
    setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
               (const char *)&v6only, sizeof(v6only));

    if (bind(sock, (struct sockaddr *)&listenAddr,
             sizeof(listenAddr)) != 0) {
      BOOST_LOG(error) << "[VIPLE-MPQUIC] Failed to bind server socket on port "
                       << _port;
      closesocket(sock);
      return;
    }

    while (_running.load()) {
      struct sockaddr_storage peerAddr{};
      socklen_t peerLen = sizeof(peerAddr);
      uint64_t currentTime = picoquic_current_time();

      // Poll for incoming packets (1ms timeout)
      fd_set readSet;
      struct timeval tv{};
      tv.tv_usec = 1000;
      FD_ZERO(&readSet);
      FD_SET(sock, &readSet);

      int selRet = select((int)(sock + 1), &readSet, nullptr, nullptr, &tv);
      if (selRet > 0) {
        int recvLen = recvfrom(sock, (char *)recvBuf, sizeof(recvBuf), 0,
                               (struct sockaddr *)&peerAddr, &peerLen);
        if (recvLen > 0) {
          picoquic_incoming_packet(
              _quic, recvBuf, (size_t)recvLen,
              (struct sockaddr *)&peerAddr,
              (struct sockaddr *)&listenAddr,
              0, 0, currentTime);
        }
      }

      // Process outgoing packets.
      // picoquic_prepare_next_packet takes the QUIC context (not a cnx)
      // and walks the wake queue internally, returning the cnx it sent
      // for via p_last_cnx.
      while (true) {
        struct sockaddr_storage destAddr{};
        struct sockaddr_storage fromAddr{};
        int ifIndex = 0;
        size_t sendLen = 0;
        picoquic_connection_id_t logCid{};
        picoquic_cnx_t *lastCnx = nullptr;

        int ret = picoquic_prepare_next_packet(
            _quic, picoquic_current_time(),
            sendBuf, sizeof(sendBuf), &sendLen,
            &destAddr, &fromAddr,
            &ifIndex, &logCid, &lastCnx);

        if (ret != 0 || sendLen == 0)
          break;

        socklen_t destLen = (destAddr.ss_family == AF_INET)
                              ? sizeof(sockaddr_in)
                              : sizeof(sockaddr_in6);
        sendto(sock, (const char *)sendBuf, (int)sendLen, 0,
               (struct sockaddr *)&destAddr, destLen);
      }
    }

    closesocket(sock);
  }

  int QuicListener::picoquicCallback(picoquic_cnx_t *cnx,
      uint64_t stream_id, uint8_t *bytes, size_t length,
      int fin_or_event, void *callback_ctx, void *stream_ctx) {
    auto *listener = static_cast<QuicListener *>(callback_ctx);
    (void)stream_ctx;

    auto event = static_cast<picoquic_call_back_event_t>(fin_or_event);

    // Helper: look up the session bound to this cnx.
    auto findSession = [&](picoquic_cnx_t *c) -> std::shared_ptr<QuicSession> {
      std::lock_guard<std::mutex> lock(listener->_sessionMutex);
      for (auto &[key, sess] : listener->_sessions) {
        if (sess->_cnx == c) return sess;
      }
      return nullptr;
    };

    switch (event) {
    case picoquic_callback_ready: {
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Client connection ready";

      auto session = std::make_shared<QuicSession>(cnx);
      picoquic_set_callback(cnx, QuicListener::picoquicCallback, listener);

      // Mandatory: tell picoquic the app wants to send datagrams. Without
      // this, the datagram outbound path is never polled.
      picoquic_mark_datagram_ready(cnx, 1);

      // Store session keyed by peer address. picoquic_get_peer_addr writes
      // a sockaddr* through the out-pointer — it does NOT memcpy into a
      // storage buffer.
      struct sockaddr *peerAddrPtr = nullptr;
      picoquic_get_peer_addr(cnx, &peerAddrPtr);

      if (peerAddrPtr) {
        std::lock_guard<std::mutex> lock(listener->_sessionMutex);
        char addrStr[INET6_ADDRSTRLEN] = {};
        if (peerAddrPtr->sa_family == AF_INET) {
          inet_ntop(AF_INET,
                    &reinterpret_cast<struct sockaddr_in *>(peerAddrPtr)->sin_addr,
                    addrStr, sizeof(addrStr));
        } else if (peerAddrPtr->sa_family == AF_INET6) {
          inet_ntop(AF_INET6,
                    &reinterpret_cast<struct sockaddr_in6 *>(peerAddrPtr)->sin6_addr,
                    addrStr, sizeof(addrStr));
        }
        listener->_sessions[addrStr] = session;
      }

      if (listener->_sessionCallback)
        listener->_sessionCallback(session);
      break;
    }

    case picoquic_callback_path_available: {
      // A new multipath path was probed and validated. picoquic passes
      // the unique_path_id in stream_id for path events (per picoquic.h
      // §"path event callbacks"). Track it in the session's path set.
      auto session = findSession(cnx);
      if (session) {
        std::lock_guard<std::mutex> lock(session->_pathMutex);
        if (std::find(session->_activePaths.begin(),
                      session->_activePaths.end(), stream_id)
            == session->_activePaths.end()) {
          session->_activePaths.push_back(stream_id);
        }
      }
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Path available: id=" << stream_id;
      break;
    }

    case picoquic_callback_path_suspended:
    case picoquic_callback_path_deleted: {
      auto session = findSession(cnx);
      if (session) {
        std::lock_guard<std::mutex> lock(session->_pathMutex);
        auto &v = session->_activePaths;
        v.erase(std::remove(v.begin(), v.end(), stream_id), v.end());
      }
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Path "
                      << (event == picoquic_callback_path_deleted ? "deleted" : "suspended")
                      << ": id=" << stream_id;
      break;
    }

    case picoquic_callback_datagram: {
      if (length < DGRAM_HDR_SIZE)
        break;
      auto *hdr = reinterpret_cast<const QuicDgramHeader *>(bytes);
      auto session = findSession(cnx);
      if (session && session->_recvHandler) {
        session->_recvHandler(
            hdr->flowType,
            bytes + DGRAM_HDR_SIZE,
            length - DGRAM_HDR_SIZE);
      }
      break;
    }

    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin: {
      if (stream_id != 0)
        break;
      auto session = findSession(cnx);
      if (session && session->_recvHandler) {
        session->_recvHandler(FLOW_CONTROL, bytes, length);
      }
      break;
    }

    case picoquic_callback_close:
    case picoquic_callback_application_close: {
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Client connection closed";

      std::lock_guard<std::mutex> lock(listener->_sessionMutex);
      for (auto it = listener->_sessions.begin();
           it != listener->_sessions.end(); ++it) {
        if (it->second->_cnx == cnx) {
          it->second->_cnx = nullptr;
          listener->_sessions.erase(it);
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

  void QuicListener::logStats() {
    std::lock_guard<std::mutex> lock(_sessionMutex);
    if (_sessions.empty())
      return;

    for (auto &[addr, session] : _sessions) {
      if (!session || !session->isReady())
        continue;

      auto stats = session->getStats();
      if (stats.empty())
        continue;

      std::string line = "[VIPLE-MPQUIC] stats " + addr + ": ";
      for (size_t i = 0; i < stats.size(); i++) {
        if (i > 0) line += " | ";
        line += "path" + std::to_string(stats[i].pathId)
              + " RTT=" + std::to_string((int)stats[i].rttMs) + "ms"
              + " " + std::to_string((int)stats[i].throughputMbps) + "Mbps";
      }
      BOOST_LOG(info) << line;
    }
  }

  void QuicListener::statsLoop() {
    while (_statsRunning.load()) {
      for (int i = 0; i < 300 && _statsRunning.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (_statsRunning.load()) {
        logStats();
      }
    }
  }

}  // namespace quic_server

#endif  // VIPLE_MPQUIC
