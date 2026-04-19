/**
 * @file src/relay.cpp
 * @brief VipleStream signaling relay client for Sunshine (server side).
 *
 * Supports both ws:// (plain) and wss:// (TLS via OpenSSL) connections.
 * wss:// is required when relay is behind Cloudflare Tunnel or similar.
 */

#include "relay.h"
#include "config.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "stun.h"
#include "udp_tunnel.h"

#include <cstring>
#include <chrono>
#include <thread>
#include <random>
#include <sstream>
#include <mutex>
#include <map>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #define SOCKET int
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR (-1)
  #define closesocket close
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

using namespace std::literals;

namespace relay {

  static constexpr int RECONNECT_INTERVAL_SEC = 30;
  static constexpr int ENDPOINT_PUBLISH_SEC = 60;
  static constexpr int RECV_TIMEOUT_MS = 5000;

  // ============================================================
  // Transport: abstracts plain TCP vs TLS socket
  // ============================================================

  class Transport;  // forward-declared for tunnel-state block below

  // ============================================================
  // Tunnel control-plane state, shared between the relay thread
  // and the streaming session threads. Declared up here so the
  // WS recv helper can dispatch binary frames to the handler.
  //
  // Locking order: always g_state_mtx before g_send_mtx when both
  // are needed (in practice they are taken disjointly).
  // ============================================================
  namespace {
    std::mutex g_send_mtx;                  // serializes transport->send()
    Transport *g_current_transport = nullptr;

    std::mutex g_state_mtx;
    std::map<std::string, std::function<void(udp_tunnel::Flow)>> g_pending_allocates;
    std::function<void(const uint8_t *, size_t)> g_tunnel_binary_handler;
    std::function<void(udp_tunnel::Flow)> g_allocated_notify_handler;
  }  // namespace

  class Transport {
  public:
    virtual ~Transport() = default;
    virtual int send(const void *data, int len) = 0;
    virtual int recv(void *buf, int len) = 0;
    virtual void close() = 0;
    virtual void setRecvTimeout(int ms) = 0;
  };

  class PlainTransport : public Transport {
    SOCKET _sock;
  public:
    PlainTransport(SOCKET s) : _sock(s) {}
    int send(const void *data, int len) override {
      return ::send(_sock, (const char *)data, len, 0);
    }
    int recv(void *buf, int len) override {
      return ::recv(_sock, (char *)buf, len, 0);
    }
    void close() override {
      closesocket(_sock);
      _sock = INVALID_SOCKET;
    }
    void setRecvTimeout(int ms) override {
#ifdef _WIN32
      DWORD tv = ms;
      setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
      struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
      setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
  };

  class TlsTransport : public Transport {
    SOCKET _sock;
    SSL_CTX *_ctx;
    SSL *_ssl;
  public:
    TlsTransport(SOCKET s) : _sock(s), _ctx(nullptr), _ssl(nullptr) {}

    bool handshake(const std::string &host) {
      _ctx = SSL_CTX_new(TLS_client_method());
      if (!_ctx) return false;

      // Don't verify server cert (Cloudflare certs are valid, but for
      // self-hosted relays we want to allow self-signed)
      SSL_CTX_set_verify(_ctx, SSL_VERIFY_NONE, nullptr);

      _ssl = SSL_new(_ctx);
      if (!_ssl) { SSL_CTX_free(_ctx); _ctx = nullptr; return false; }

      SSL_set_fd(_ssl, (int)_sock);
      SSL_set_tlsext_host_name(_ssl, host.c_str());  // SNI for Cloudflare

      if (SSL_connect(_ssl) <= 0) {
        BOOST_LOG(warning) << "[RELAY] TLS handshake failed: "
          << ERR_reason_error_string(ERR_get_error());
        SSL_free(_ssl); _ssl = nullptr;
        SSL_CTX_free(_ctx); _ctx = nullptr;
        return false;
      }
      return true;
    }

    int send(const void *data, int len) override {
      return SSL_write(_ssl, data, len);
    }
    int recv(void *buf, int len) override {
      return SSL_read(_ssl, buf, len);
    }
    void setRecvTimeout(int ms) override {
#ifdef _WIN32
      DWORD tv = ms;
      setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
      struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
      setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    void close() override {
      if (_ssl) { SSL_shutdown(_ssl); SSL_free(_ssl); _ssl = nullptr; }
      if (_ctx) { SSL_CTX_free(_ctx); _ctx = nullptr; }
      closesocket(_sock);
      _sock = INVALID_SOCKET;
    }
  };

  // ============================================================
  // Utilities
  // ============================================================

  static std::string base64_encode_bytes(const uint8_t *data, int len) {
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, len);
    BIO_flush(b64);
    BUF_MEM *bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
  }

  static std::string compute_psk_hash(const std::string &psk, const std::string &uuid) {
    if (psk.empty()) return "";
    unsigned char hash[32];
    unsigned int len = 0;
    HMAC(EVP_sha256(), psk.c_str(), (int)psk.size(),
         (const unsigned char *)uuid.c_str(), uuid.size(), hash, &len);
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    hex[16] = 0;
    return hex;
  }

  // Build one masked WebSocket frame with the given opcode and send it.
  // Returns true iff the full frame was written to the transport.
  static bool ws_send_frame(Transport &tr, uint8_t opcode,
                            const uint8_t *data, size_t len) {
    std::vector<uint8_t> frame;
    frame.reserve(len + 14);
    frame.push_back(0x80 | (opcode & 0x0F));  // FIN + opcode
    if (len < 126) {
      frame.push_back(0x80 | (uint8_t)len);
    } else if (len <= 0xFFFF) {
      frame.push_back(0x80 | 126);
      frame.push_back((uint8_t)(len >> 8));
      frame.push_back((uint8_t)(len & 0xFF));
    } else {
      frame.push_back(0x80 | 127);
      for (int i = 7; i >= 0; i--) {
        frame.push_back((uint8_t)((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
      }
    }
    uint8_t mask[4];
    std::random_device rd;
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)rd();
    frame.insert(frame.end(), mask, mask + 4);
    for (size_t i = 0; i < len; i++) {
      frame.push_back(data[i] ^ mask[i % 4]);
    }
    return tr.send(frame.data(), (int)frame.size()) == (int)frame.size();
  }

  static bool ws_send(Transport &tr, const std::string &data) {
    return ws_send_frame(tr, 0x1,
                         reinterpret_cast<const uint8_t *>(data.data()),
                         data.size());
  }

  // ws_recv_frame opcode sentinels:
  //   0xFE — recv timed out (no data yet, carrier still alive)
  //   0xFF — connection closed or fatal read error
  // Real WebSocket opcodes are 0x0–0xA so these values can never collide.
  struct WsFrame {
    uint8_t opcode = 0xFF;
    std::vector<uint8_t> payload;
  };

  static WsFrame ws_recv_frame(Transport &tr) {
    WsFrame f;
    uint8_t hdr[2];
    int n = tr.recv(hdr, 1);
    if (n == 0) { f.opcode = 0xFF; return f; }
    if (n < 0)  { f.opcode = 0xFE; return f; }
    int n2 = tr.recv(hdr + 1, 1);
    if (n2 == 0) { f.opcode = 0xFF; return f; }
    if (n2 < 0)  { f.opcode = 0xFF; return f; }

    uint8_t opcode = hdr[0] & 0x0F;
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
      uint8_t ext[2];
      if (tr.recv(ext, 2) != 2) return f;
      len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      uint8_t ext[8];
      if (tr.recv(ext, 8) != 8) return f;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > 10 * 1024 * 1024) return f;  // cap at 10 MiB

    if (opcode == 0x8) { f.opcode = 0xFF; return f; }  // close

    f.payload.resize(static_cast<size_t>(len));
    size_t total = 0;
    while (total < len) {
      int m = tr.recv(f.payload.data() + total,
                      static_cast<int>(len - total));
      if (m <= 0) { f.opcode = 0xFF; return f; }
      total += static_cast<size_t>(m);
    }
    f.opcode = opcode;
    return f;
  }

  // Kept for the tcp_tunnel / http_proxy paths: returns text-frame payload
  // as a string, or empty for any non-text frame / timeout / close.
  //
  // Binary frames are routed to the tunnel handler here so that binary
  // tunnel data is never dropped even when the relay thread is parked
  // inside a nested loop (e.g. the tcp_tunnel_open handler).
  static std::string ws_recv(Transport &tr) {
    auto f = ws_recv_frame(tr);
    if (f.opcode == 0x2) {
      std::function<void(const uint8_t *, size_t)> h;
      {
        std::lock_guard<std::mutex> l(g_state_mtx);
        h = g_tunnel_binary_handler;
      }
      if (h) h(f.payload.data(), f.payload.size());
      return "";
    }
    if (f.opcode != 0x1) return "";
    return std::string(f.payload.begin(), f.payload.end());
  }

  static bool ws_handshake(Transport &tr, const std::string &host, uint16_t port) {
    uint8_t rnd[16];
    std::random_device rd;
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)rd();
    std::string key = base64_encode_bytes(rnd, 16);

    std::ostringstream req;
    req << "GET / HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << key << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n"
        << "\r\n";

    std::string reqStr = req.str();
    if (tr.send(reqStr.c_str(), (int)reqStr.size()) != (int)reqStr.size()) return false;

    // Read HTTP response byte-by-byte until \r\n\r\n to avoid consuming WS frames
    std::string resp;
    char c;
    while (resp.size() < 4096) {
      if (tr.recv(&c, 1) != 1) break;
      resp.push_back(c);
      if (resp.size() >= 4 && resp.substr(resp.size() - 4) == "\r\n\r\n") break;
    }
    return resp.find("101") != std::string::npos;
  }

  // ============================================================
  // Tunnel control plane — public API implementations.
  // The underlying state (g_send_mtx, g_current_transport,
  // g_state_mtx, g_pending_allocates, g_tunnel_binary_handler)
  // is declared at the top of this namespace.
  // ============================================================

  namespace {
    std::string gen_request_id() {
      static thread_local std::mt19937_64 rng{std::random_device{}()};
      static constexpr char HEX[] = "0123456789abcdef";
      char buf[17];
      uint64_t v = rng();
      for (int i = 0; i < 16; i++) {
        buf[i] = HEX[(v >> (60 - i * 4)) & 0xF];
      }
      buf[16] = 0;
      return buf;
    }

    // Tiny JSON field extractors. The relay's msg format is
    // stable enough (flat objects, no nested escapes in the fields
    // we read) that avoiding a full JSON dep is worth it here.
    std::string json_str(const std::string &msg, const std::string &key) {
      auto pos = msg.find("\"" + key + "\"");
      if (pos == std::string::npos) return "";
      pos = msg.find(':', pos + key.size() + 2);
      if (pos == std::string::npos) return "";
      auto q1 = msg.find('"', pos + 1);
      if (q1 == std::string::npos) return "";
      auto q2 = msg.find('"', q1 + 1);
      if (q2 == std::string::npos) return "";
      return msg.substr(q1 + 1, q2 - q1 - 1);
    }

    bool json_int(const std::string &msg, const std::string &key, int64_t &out) {
      auto pos = msg.find("\"" + key + "\"");
      if (pos == std::string::npos) return false;
      pos = msg.find(':', pos + key.size() + 2);
      if (pos == std::string::npos) return false;
      auto numStart = msg.find_first_of("-0123456789", pos);
      if (numStart == std::string::npos) return false;
      try {
        out = std::stoll(msg.substr(numStart));
        return true;
      } catch (...) {
        return false;
      }
    }
  }  // namespace

  void allocate_flow(const std::string &target_uuid,
                     std::function<void(udp_tunnel::Flow)> cb) {
    std::string request_id = gen_request_id();
    {
      std::lock_guard<std::mutex> l(g_state_mtx);
      g_pending_allocates[request_id] = std::move(cb);
    }
    std::string msg = "{\"type\":\"udp_tunnel_allocate\",\"request_id\":\"" +
                      request_id + "\",\"target_uuid\":\"" + target_uuid + "\"}";
    bool sent = false;
    {
      std::lock_guard<std::mutex> l(g_send_mtx);
      if (g_current_transport) sent = ws_send(*g_current_transport, msg);
    }
    if (!sent) {
      // Not connected / send failed — fail the callback immediately.
      std::function<void(udp_tunnel::Flow)> dead;
      {
        std::lock_guard<std::mutex> l(g_state_mtx);
        auto it = g_pending_allocates.find(request_id);
        if (it != g_pending_allocates.end()) {
          dead = std::move(it->second);
          g_pending_allocates.erase(it);
        }
      }
      if (dead) dead(udp_tunnel::Flow{});
    }
  }

  void close_flow(uint16_t flow_id) {
    std::string msg = "{\"type\":\"udp_tunnel_close\",\"flow_id\":" +
                      std::to_string(flow_id) + "}";
    std::lock_guard<std::mutex> l(g_send_mtx);
    if (g_current_transport) ws_send(*g_current_transport, msg);
  }

  bool send_tunnel_binary(const uint8_t *data, size_t len) {
    std::lock_guard<std::mutex> l(g_send_mtx);
    if (!g_current_transport) return false;
    return ws_send_frame(*g_current_transport, 0x2, data, len);
  }

  void set_tunnel_binary_handler(std::function<void(const uint8_t *, size_t)> cb) {
    std::lock_guard<std::mutex> l(g_state_mtx);
    g_tunnel_binary_handler = std::move(cb);
  }

  void set_allocated_notify_handler(std::function<void(udp_tunnel::Flow)> cb) {
    std::lock_guard<std::mutex> l(g_state_mtx);
    g_allocated_notify_handler = std::move(cb);
  }

  // Parse a `udp_tunnel_allocated` JSON message into a Flow. Returns an
  // invalid Flow (flow_id == 0) if any mandatory field is missing.
  // Field names follow the relay wire protocol: `relay_udp_host`,
  // `relay_udp_port`, `role` (not `udp_host`/`udp_port`/`side`).
  static udp_tunnel::Flow parse_allocated(const std::string &msg) {
    udp_tunnel::Flow flow;
    int64_t fid = 0;
    if (!json_int(msg, "flow_id", fid) || fid <= 0 || fid > 0xFFFF) {
      return flow;
    }
    std::string token_hex = json_str(msg, "token");
    if (!udp_tunnel::hex_to_token(token_hex, flow.token)) {
      return flow;
    }
    flow.flow_id = static_cast<uint16_t>(fid);
    flow.relay_udp_host = json_str(msg, "relay_udp_host");
    int64_t udp_port = 0;
    if (json_int(msg, "relay_udp_port", udp_port) && udp_port > 0 && udp_port < 65536) {
      flow.relay_udp_port = static_cast<uint16_t>(udp_port);
    }
    flow.remote_uuid = json_str(msg, "remote_uuid");
    flow.is_side_a = (json_str(msg, "role") == "a");
    return flow;
  }

  // Dispatch one udp_tunnel_allocated text message received from the relay.
  //
  //  - If `request_id` matches a pending allocate_flow, fire that callback
  //    (this is the requester-side response: role="a").
  //  - Otherwise, treat it as a role="b" notification and hand it to the
  //    allocated-notify handler. Sunshine uses this to pick up flows
  //    initiated by the connecting Moonlight client so both ends share
  //    the same flow_id / token.
  static void handle_allocated(const std::string &msg) {
    udp_tunnel::Flow flow = parse_allocated(msg);

    std::string request_id = json_str(msg, "request_id");
    if (!request_id.empty()) {
      std::function<void(udp_tunnel::Flow)> cb;
      {
        std::lock_guard<std::mutex> l(g_state_mtx);
        auto it = g_pending_allocates.find(request_id);
        if (it != g_pending_allocates.end()) {
          cb = std::move(it->second);
          g_pending_allocates.erase(it);
        }
      }
      if (cb) {
        BOOST_LOG(info) << "[RELAY-TUNNEL] Flow " << flow.flow_id
                        << " allocated (peer=" << flow.remote_uuid.substr(0, 12)
                        << ", udp=" << flow.relay_udp_host << ":" << flow.relay_udp_port
                        << ", role=" << (flow.is_side_a ? "a" : "b") << ")";
        cb(flow);
        return;
      }
    }

    // No pending request matched — this is a peer-initiated allocation
    // notification (role="b").
    std::function<void(udp_tunnel::Flow)> notify;
    {
      std::lock_guard<std::mutex> l(g_state_mtx);
      notify = g_allocated_notify_handler;
    }
    if (notify && flow.valid()) {
      BOOST_LOG(info) << "[RELAY-TUNNEL] Flow " << flow.flow_id
                      << " notified (peer=" << flow.remote_uuid.substr(0, 12)
                      << ", udp=" << flow.relay_udp_host << ":" << flow.relay_udp_port
                      << ", role=" << (flow.is_side_a ? "a" : "b") << ")";
      notify(flow);
    }
  }

  // ============================================================
  // Relay client thread
  // ============================================================

  static void relay_thread_proc() {
    platf::set_thread_name("relay");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    std::string relay_url = config::stream.relay_url;
    std::string relay_psk = config::stream.relay_psk;

    if (relay_url.empty()) {
      BOOST_LOG(info) << "[RELAY] No relay_url configured, relay client disabled";
      return;
    }

    // Parse URL: ws://host:port or wss://host:port or host:port
    std::string host;
    uint16_t port;
    bool useTls = false;
    {
      std::string url = relay_url;
      if (url.substr(0, 6) == "wss://") { url = url.substr(6); useTls = true; port = 443; }
      else if (url.substr(0, 5) == "ws://") { url = url.substr(5); port = 9999; }
      else { port = 9999; }

      auto colonPos = url.rfind(':');
      if (colonPos != std::string::npos) {
        host = url.substr(0, colonPos);
        port = (uint16_t)std::stoi(url.substr(colonPos + 1));
      } else {
        host = url;
      }
    }

    BOOST_LOG(info) << "[RELAY] Target: " << (useTls ? "wss://" : "ws://")
                    << host << ":" << port
                    << " (TLS=" << (useTls ? "yes" : "no") << ")";

    // Wait for http::unique_id to be initialized (race condition with nvhttp startup)
    while (http::unique_id.empty() && !shutdown_event->peek()) {
      BOOST_LOG(info) << "[RELAY] Waiting for server UUID to be initialized...";
      if (shutdown_event->pop(std::chrono::seconds(2))) return;
    }

    std::string server_uuid = http::unique_id;
    std::string psk_hash = compute_psk_hash(relay_psk, server_uuid);
    BOOST_LOG(info) << "[RELAY] Server UUID: " << server_uuid.substr(0, 8) << "..";

    while (!shutdown_event->peek()) {
      // Resolve host
      struct addrinfo hints = {}, *res = nullptr;
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      char portStr[8];
      snprintf(portStr, sizeof(portStr), "%u", port);

      if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        BOOST_LOG(warning) << "[RELAY] Failed to resolve " << host;
        if (shutdown_event->pop(std::chrono::seconds(RECONNECT_INTERVAL_SEC))) break;
        continue;
      }

      SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        if (shutdown_event->pop(std::chrono::seconds(RECONNECT_INTERVAL_SEC))) break;
        continue;
      }

#ifdef _WIN32
      DWORD tv = RECV_TIMEOUT_MS;
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
      struct timeval tv = { RECV_TIMEOUT_MS / 1000, (RECV_TIMEOUT_MS % 1000) * 1000 };
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

      if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        BOOST_LOG(warning) << "[RELAY] TCP connect failed to " << host << ":" << port;
        closesocket(sock);
        freeaddrinfo(res);
        if (shutdown_event->pop(std::chrono::seconds(RECONNECT_INTERVAL_SEC))) break;
        continue;
      }
      freeaddrinfo(res);

      // Create transport (plain or TLS)
      std::unique_ptr<Transport> transport;
      if (useTls) {
        auto tls = std::make_unique<TlsTransport>(sock);
        if (!tls->handshake(host)) {
          BOOST_LOG(warning) << "[RELAY] TLS handshake failed";
          closesocket(sock);
          if (shutdown_event->pop(std::chrono::seconds(RECONNECT_INTERVAL_SEC))) break;
          continue;
        }
        BOOST_LOG(info) << "[RELAY] TLS connected to " << host;
        transport = std::move(tls);
      } else {
        transport = std::make_unique<PlainTransport>(sock);
      }

      // WebSocket handshake
      if (!ws_handshake(*transport, host, port)) {
        BOOST_LOG(warning) << "[RELAY] WebSocket handshake failed";
        transport->close();
        if (shutdown_event->pop(std::chrono::seconds(RECONNECT_INTERVAL_SEC))) break;
        continue;
      }

      BOOST_LOG(info) << "[RELAY] WebSocket handshake OK";

      // Register
      std::string regMsg = "{\"type\":\"register\",\"uuid\":\"" + server_uuid +
                           "\",\"role\":\"server\",\"psk_hash\":\"" + psk_hash + "\"}";
      BOOST_LOG(info) << "[RELAY] Sending register: uuid=" << server_uuid.substr(0, 8) << "..";
      if (!ws_send(*transport, regMsg)) {
        BOOST_LOG(warning) << "[RELAY] Send register failed";
        transport->close();
        if (shutdown_event->pop(std::chrono::seconds(RECONNECT_INTERVAL_SEC))) break;
        continue;
      }

      std::string resp = ws_recv(*transport);
      BOOST_LOG(info) << "[RELAY] Register response (" << resp.size() << " bytes): " << resp.substr(0, 200);
      if (resp.find("registered") != std::string::npos) {
        BOOST_LOG(info) << "[RELAY] Registered with relay as server (TLS=" << (useTls ? "yes" : "no") << ")";
      } else {
        BOOST_LOG(warning) << "[RELAY] Registration failed: " << resp;
        transport->close();
        if (shutdown_event->pop(std::chrono::seconds(RECONNECT_INTERVAL_SEC))) break;
        continue;
      }

      // Expose this transport to the public tunnel API. All subsequent
      // sends — from the relay thread and from any caller of
      // allocate_flow / send_tunnel_binary — must hold g_send_mtx.
      {
        std::lock_guard<std::mutex> l(g_send_mtx);
        g_current_transport = transport.get();
      }

      // Main loop: publish STUN endpoint + handle incoming proxy requests
      auto lastEndpointPublish = std::chrono::steady_clock::now();
      bool send_failed = false;
      while (!shutdown_event->peek()) {
        // Publish STUN endpoint periodically
        auto now = std::chrono::steady_clock::now();
        if (now - lastEndpointPublish >= std::chrono::seconds(ENDPOINT_PUBLISH_SEC)) {
          auto ep = stun::get_endpoint();
          if (ep.valid()) {
            std::string epMsg = "{\"type\":\"endpoint\",\"uuid\":\"" + server_uuid +
                                "\",\"stun_ip\":\"" + ep.ip +
                                "\",\"stun_port\":" + std::to_string(ep.port) +
                                ",\"nat_type\":\"" + (ep.nat_type == stun::NAT_SYMMETRIC ? "symmetric" : "punchable") + "\"}";
            std::lock_guard<std::mutex> l(g_send_mtx);
            if (!ws_send(*transport, epMsg)) { send_failed = true; break; }
          }
          {
            std::lock_guard<std::mutex> l(g_send_mtx);
            ws_send(*transport, "{\"type\":\"ping\"}");
          }
          lastEndpointPublish = now;
        }

        // Read incoming messages (short timeout — non-blocking poll)
        std::string msg = ws_recv(*transport);
        if (msg.empty()) {
          // Timeout, non-text frame, or close — loop again (outer while
          // drops out naturally if the transport is dead, because
          // subsequent sends will fail and we'll break below).
          if (shutdown_event->pop(std::chrono::seconds(1))) break;
          continue;
        }

        // Tunnel control plane: allocate / close acknowledgements from relay
        if (msg.find("udp_tunnel_allocated") != std::string::npos) {
          handle_allocated(msg);
          continue;
        }
        if (msg.find("udp_tunnel_closed") != std::string::npos) {
          int64_t fid = 0;
          if (json_int(msg, "flow_id", fid)) {
            BOOST_LOG(info) << "[RELAY-TUNNEL] Flow " << fid << " closed by relay";
          }
          continue;
        }

        // JSON field extractor (reused by http_proxy and tcp_tunnel)
        auto extractField = [&](const std::string &key) -> std::string {
            auto pos = msg.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            pos = msg.find("\"", pos + key.size() + 3);
            if (pos == std::string::npos) return "";
            auto end = msg.find("\"", pos + 1);
            return (end != std::string::npos) ? msg.substr(pos + 1, end - pos - 1) : "";
          };

        // Handle http_proxy_request
        if (msg.find("http_proxy_request") != std::string::npos) {
          std::string requestId = extractField("request_id");
          std::string fromUuid = extractField("from_uuid");
          std::string path = extractField("path");

          BOOST_LOG(info) << "[RELAY-PROXY] Request: " << path << " (from=" << fromUuid.substr(0, 12) << ")";

          // Make local HTTPS request to Sunshine (localhost)
          std::string responseBody;
          int httpStatus = 500;

          // All routes go to HTTP (47989) now — /launch and /resume are registered on HTTP with localhost guard
          uint16_t localPort = net::map_port(0);
          // /launch needs longer timeout (encoder probe + display config can take 10+ seconds)
          bool isLaunch = (path.find("/launch") != std::string::npos ||
                           path.find("/resume") != std::string::npos);
          {
            SOCKET localSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (localSock != INVALID_SOCKET) {
              struct sockaddr_in localAddr = {};
              localAddr.sin_family = AF_INET;
              localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
              localAddr.sin_port = htons(localPort);

              int timeoutMs = isLaunch ? 30000 : 5000; // /launch needs 30s (encoder probe)
#ifdef _WIN32
              DWORD tv2 = timeoutMs;
              setsockopt(localSock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv2, sizeof(tv2));
#else
              struct timeval tv2 = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
              setsockopt(localSock, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
#endif

              if (::connect(localSock, (struct sockaddr *)&localAddr, sizeof(localAddr)) == 0) {
                std::string httpReq = "GET " + path + " HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
                ::send(localSock, httpReq.c_str(), (int)httpReq.size(), 0);
                char buf[65536];
                std::string rawResp;
                int n;
                while ((n = ::recv(localSock, buf, sizeof(buf) - 1, 0)) > 0)
                  rawResp.append(buf, n);

                auto headerEnd = rawResp.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                  auto statusPos = rawResp.find(" ");
                  if (statusPos != std::string::npos)
                    httpStatus = std::stoi(rawResp.substr(statusPos + 1, 3));
                  responseBody = rawResp.substr(headerEnd + 4);
                }
              } else {
                responseBody = "connection to local sunshine failed";
                httpStatus = 502;
              }
              closesocket(localSock);
            }
          }

          BOOST_LOG(info) << "[RELAY-PROXY] Response: " << httpStatus << " (" << responseBody.size() << " bytes)";

          // Escape JSON special chars in response body
          std::string escapedBody;
          for (char c : responseBody) {
            if (c == '"') escapedBody += "\\\"";
            else if (c == '\\') escapedBody += "\\\\";
            else if (c == '\n') escapedBody += "\\n";
            else if (c == '\r') escapedBody += "\\r";
            else if (c == '\t') escapedBody += "\\t";
            else escapedBody += c;
          }

          std::string proxyResp = "{\"type\":\"http_proxy_response\",\"request_id\":\"" + requestId +
                                  "\",\"from_uuid\":\"" + fromUuid +
                                  "\",\"status\":" + std::to_string(httpStatus) +
                                  ",\"body\":\"" + escapedBody + "\"}";
          std::lock_guard<std::mutex> l(g_send_mtx);
          ws_send(*transport, proxyResp);
        }

        // Handle TCP tunnel: connect to local port and relay data
        // moonlight-common-c RTSP uses one TCP connection per request-response:
        //   connect → send request → recv response → server closes → done
        // So we must handle multiple sequential TCP connections over the same WS tunnel.
        if (msg.find("tcp_tunnel_open") != std::string::npos) {
          std::string tunnelId = extractField("tunnel_id");
          std::string fromUuid = extractField("from_uuid");
          int targetPort = 0;
          auto portPos = msg.find("\"target_port\"");
          if (portPos != std::string::npos) {
            auto numStart = msg.find_first_of("0123456789", portPos);
            if (numStart != std::string::npos) targetPort = std::stoi(msg.substr(numStart));
          }

          BOOST_LOG(info) << "[RELAY-TUNNEL] Opening TCP tunnel to localhost:" << targetPort
                          << " (from=" << fromUuid.substr(0, 12) << ")";

          // Switch WS transport to short timeout for tunnel polling
          transport->setRecvTimeout(100); // 100ms poll

          // Helper lambda: connect to localhost:targetPort with recv timeout
          auto connectLocal = [&]() -> SOCKET {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) return INVALID_SOCKET;
            struct sockaddr_in tAddr = {};
            tAddr.sin_family = AF_INET;
            tAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            tAddr.sin_port = htons((uint16_t)targetPort);
#ifdef _WIN32
            DWORD tv3 = 500;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv3, sizeof(tv3));
#else
            struct timeval tv3 = {0, 500000};
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv3, sizeof(tv3));
#endif
            if (::connect(s, (struct sockaddr *)&tAddr, sizeof(tAddr)) != 0) {
              closesocket(s);
              return INVALID_SOCKET;
            }
            return s;
          };

          // Connect to RTSP for the first request
          SOCKET tunnelSock = connectLocal();
          int connectionCount = 1;
          if (tunnelSock != INVALID_SOCKET) {
            BOOST_LOG(info) << "[RELAY-TUNNEL] #" << connectionCount << " Connected to localhost:" << targetPort;
          } else {
            BOOST_LOG(warning) << "[RELAY-TUNNEL] Connect to localhost:" << targetPort << " failed";
          }

          // Tunnel loop: handles multiple sequential TCP connections
          // Outer loop runs until tunnel is explicitly closed, timeout, or shutdown.
          // Timeout after 30s of inactivity to avoid blocking the relay thread forever.
          bool tunnelDone = false;
          auto lastActivity = std::chrono::steady_clock::now();
          constexpr int TUNNEL_IDLE_TIMEOUT_SEC = 30;
          while (!shutdown_event->peek() && !tunnelDone) {
            bool tunnelActivity = false;

            // Check idle timeout
            auto idleTime = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - lastActivity).count();
            if (idleTime >= TUNNEL_IDLE_TIMEOUT_SEC) {
              BOOST_LOG(info) << "[RELAY-TUNNEL] Idle timeout (" << TUNNEL_IDLE_TIMEOUT_SEC
                              << "s), closing tunnel after " << connectionCount << " connections";
              tunnelDone = true;
              break;
            }

            // Check for data from relay WS (client → server)
            std::string tunnelMsg = ws_recv(*transport); // 100ms timeout
            if (!tunnelMsg.empty()) {
              if (tunnelMsg.find("tcp_tunnel_data") != std::string::npos) {
                // If TCP socket is not connected, reconnect (shouldn't happen normally)
                if (tunnelSock == INVALID_SOCKET) {
                  tunnelSock = connectLocal();
                  if (tunnelSock != INVALID_SOCKET) {
                    connectionCount++;
                    BOOST_LOG(info) << "[RELAY-TUNNEL] #" << connectionCount
                                    << " Auto-reconnected to localhost:" << targetPort;
                  }
                }

                if (tunnelSock != INVALID_SOCKET) {
                  // Extract base64 data
                  auto dataPos = tunnelMsg.find("\"data\"");
                  if (dataPos != std::string::npos) {
                    auto qStart = tunnelMsg.find("\"", dataPos + 7);
                    auto qEnd = tunnelMsg.find("\"", qStart + 1);
                    if (qStart != std::string::npos && qEnd != std::string::npos) {
                      std::string b64 = tunnelMsg.substr(qStart + 1, qEnd - qStart - 1);
                      BIO *bio = BIO_new_mem_buf(b64.c_str(), (int)b64.size());
                      BIO *b64bio = BIO_new(BIO_f_base64());
                      BIO_set_flags(b64bio, BIO_FLAGS_BASE64_NO_NL);
                      bio = BIO_push(b64bio, bio);
                      char decoded[65536];
                      int decodedLen = BIO_read(bio, decoded, sizeof(decoded));
                      BIO_free_all(bio);

                      if (decodedLen > 0) {
                        ::send(tunnelSock, decoded, decodedLen, 0);
                        tunnelActivity = true;
                        BOOST_LOG(info) << "[RELAY-TUNNEL] #" << connectionCount
                                        << " WS→TCP: " << decodedLen << " bytes";
                      }
                    }
                  }
                }
              } else if (tunnelMsg.find("tcp_tunnel_reconnect") != std::string::npos) {
                // Client requests a new TCP connection (next RTSP request)
                if (tunnelSock != INVALID_SOCKET) {
                  closesocket(tunnelSock);
                }
                tunnelSock = connectLocal();
                connectionCount++;
                if (tunnelSock != INVALID_SOCKET) {
                  BOOST_LOG(info) << "[RELAY-TUNNEL] #" << connectionCount
                                  << " Reconnected to localhost:" << targetPort;
                } else {
                  BOOST_LOG(warning) << "[RELAY-TUNNEL] #" << connectionCount
                                     << " Reconnect to localhost:" << targetPort << " failed";
                }
              } else if (tunnelMsg.find("tcp_tunnel_close_all") != std::string::npos) {
                // Client done with all requests
                BOOST_LOG(info) << "[RELAY-TUNNEL] Client closed tunnel after "
                                << connectionCount << " connections";
                tunnelDone = true;
              } else if (tunnelMsg.find("ping") != std::string::npos) {
                std::lock_guard<std::mutex> l(g_send_mtx);
                ws_send(*transport, "{\"type\":\"pong\"}");
              }
              // Ignore endpoint updates, other messages during tunnel
            }

            // Check for data from local TCP (server → client)
            if (tunnelSock != INVALID_SOCKET) {
              char tcpBuf[65536];
              int tcpN = ::recv(tunnelSock, tcpBuf, sizeof(tcpBuf), 0);
              if (tcpN > 0) {
                std::string b64data = base64_encode_bytes((const uint8_t *)tcpBuf, tcpN);
                std::string dataResp = "{\"type\":\"tcp_tunnel_data\",\"tunnel_id\":\"" + tunnelId +
                                       "\",\"from_uuid\":\"" + fromUuid +
                                       "\",\"data\":\"" + b64data + "\"}";
                {
                  std::lock_guard<std::mutex> l(g_send_mtx);
                  ws_send(*transport, dataResp);
                }
                tunnelActivity = true;
                BOOST_LOG(info) << "[RELAY-TUNNEL] #" << connectionCount
                                << " TCP→WS: " << tcpN << " bytes";
              } else if (tcpN == 0) {
                // RTSP server closed TCP — this is NORMAL (one conn per request)
                BOOST_LOG(info) << "[RELAY-TUNNEL] #" << connectionCount
                                << " RTSP server closed TCP (normal per-request close)";
                std::string closeMsg = "{\"type\":\"tcp_tunnel_closed\",\"tunnel_id\":\"" + tunnelId +
                                        "\",\"from_uuid\":\"" + fromUuid + "\"}";
                {
                  std::lock_guard<std::mutex> l(g_send_mtx);
                  ws_send(*transport, closeMsg);
                }
                closesocket(tunnelSock);
                tunnelSock = INVALID_SOCKET;
                // DON'T break — wait for next tcp_tunnel_data or tcp_tunnel_reconnect
              }
              // tcpN < 0 with timeout is normal (no data yet)
            }

            if (tunnelActivity) {
              lastActivity = std::chrono::steady_clock::now();
            } else {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
          }

          // Clean up
          if (tunnelSock != INVALID_SOCKET) closesocket(tunnelSock);
          transport->setRecvTimeout(RECV_TIMEOUT_MS);
          BOOST_LOG(info) << "[RELAY-TUNNEL] Tunnel ended after " << connectionCount
                          << " connections, restored normal timeout";
        }
      }

      // Remove transport pointer so other threads stop routing sends
      // through a dying carrier, then fail any in-flight allocate_flow
      // callbacks so the streaming path doesn't hang on us.
      {
        std::lock_guard<std::mutex> l(g_send_mtx);
        g_current_transport = nullptr;
      }
      std::map<std::string, std::function<void(udp_tunnel::Flow)>> dead;
      {
        std::lock_guard<std::mutex> l(g_state_mtx);
        dead.swap(g_pending_allocates);
      }
      for (auto &p : dead) {
        if (p.second) p.second(udp_tunnel::Flow{});
      }

      transport->close();
      BOOST_LOG(info) << "[RELAY] Disconnected"
                      << (send_failed ? " (send failed)" : "")
                      << ", reconnecting in " << RECONNECT_INTERVAL_SEC << "s";
      if (shutdown_event->pop(std::chrono::seconds(RECONNECT_INTERVAL_SEC))) break;
    }

    BOOST_LOG(info) << "[RELAY] Client stopped";
  }

  class deinit_t : public platf::deinit_t {
  public:
    deinit_t() { _thread = std::thread(relay_thread_proc); }
    ~deinit_t() override { if (_thread.joinable()) _thread.join(); }
  private:
    std::thread _thread;
  };

  std::unique_ptr<platf::deinit_t> start() {
    if (config::stream.relay_url.empty()) {
      BOOST_LOG(info) << "[RELAY] No relay_url configured, skipping";
      return nullptr;
    }
    return std::make_unique<deinit_t>();
  }

}  // namespace relay
