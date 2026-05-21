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

    // Query picoquic for path stats
    int pathCount = picoquic_get_cnx_nb_paths(_cnx);
    for (int i = 0; i < pathCount; i++) {
      SubflowStats s{};
      s.pathId = i;
      s.rttMs = (float)picoquic_get_cnx_path_rtt(_cnx, i) / 1000.0f;
      s.active = true;
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
        128,                       // max connections
        certPath.c_str(),
        keyPath.c_str(),
        nullptr,                   // cert_root
        "viplestream",             // ALPN
        QuicListener::picoquicCallback,
        this,
        nullptr, nullptr, nullptr,
        currentTime,
        nullptr, nullptr, nullptr, 0);

    if (!_quic) {
      BOOST_LOG(error) << "[VIPLE-MPQUIC] Failed to create QUIC server context";
      return false;
    }

    // Enable datagram extension and multipath
    picoquic_set_default_datagram_option(_quic, 1);
    picoquic_set_default_multipath_option(_quic, 1);

    _port = port;
    _running.store(true);
    _ioThread = std::thread(&QuicListener::ioLoop, this);

    BOOST_LOG(info) << "[VIPLE-MPQUIC] Server listening on port " << port;
    return true;
  }

  void QuicListener::stop() {
    if (!_running.load())
      return;

    _running.store(false);
    if (_ioThread.joinable())
      _ioThread.join();

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

      // Process outgoing packets
      picoquic_cnx_t *iterCnx = picoquic_get_earliest_cnx_to_wake(
          _quic, currentTime + 1000);

      while (iterCnx) {
        struct sockaddr_storage destAddr{};
        socklen_t destLen = sizeof(destAddr);
        size_t sendLen = 0;

        int ret = picoquic_prepare_next_packet(
            iterCnx, picoquic_current_time(),
            sendBuf, sizeof(sendBuf), &sendLen,
            (struct sockaddr *)&destAddr, &destLen,
            nullptr, nullptr, nullptr);

        if (ret == 0 && sendLen > 0) {
          sendto(sock, (const char *)sendBuf, (int)sendLen, 0,
                 (struct sockaddr *)&destAddr, destLen);
        }

        if (ret != 0 || sendLen == 0)
          break;
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

    switch (event) {
    case picoquic_callback_ready: {
      BOOST_LOG(info) << "[VIPLE-MPQUIC] Client connection ready";

      auto session = std::make_shared<QuicSession>(cnx);
      picoquic_set_callback(cnx, QuicListener::picoquicCallback, listener);

      // Store session keyed by peer address
      struct sockaddr_storage peerAddr{};
      picoquic_get_peer_addr(cnx, (struct sockaddr **)&peerAddr);

      {
        std::lock_guard<std::mutex> lock(listener->_sessionMutex);
        char addrStr[INET6_ADDRSTRLEN];
        if (peerAddr.ss_family == AF_INET) {
          inet_ntop(AF_INET,
                    &((struct sockaddr_in *)&peerAddr)->sin_addr,
                    addrStr, sizeof(addrStr));
        } else {
          inet_ntop(AF_INET6,
                    &((struct sockaddr_in6 *)&peerAddr)->sin6_addr,
                    addrStr, sizeof(addrStr));
        }
        listener->_sessions[addrStr] = session;
      }

      if (listener->_sessionCallback)
        listener->_sessionCallback(session);

      // Store session pointer as picoquic stream context for fast lookup
      picoquic_set_callback(cnx, QuicListener::picoquicCallback, listener);
      break;
    }

    case picoquic_callback_datagram: {
      if (length < DGRAM_HDR_SIZE)
        break;

      auto *hdr = reinterpret_cast<const QuicDgramHeader *>(bytes);

      // Find the session for this connection
      std::shared_ptr<QuicSession> session;
      {
        std::lock_guard<std::mutex> lock(listener->_sessionMutex);
        for (auto &[key, sess] : listener->_sessions) {
          if (sess->_cnx == cnx) {
            session = sess;
            break;
          }
        }
      }

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

      std::shared_ptr<QuicSession> session;
      {
        std::lock_guard<std::mutex> lock(listener->_sessionMutex);
        for (auto &[key, sess] : listener->_sessions) {
          if (sess->_cnx == cnx) {
            session = sess;
            break;
          }
        }
      }

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

}  // namespace quic_server

#endif  // VIPLE_MPQUIC
