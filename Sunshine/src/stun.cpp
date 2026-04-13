/**
 * @file src/stun.cpp
 * @brief STUN NAT traversal prober for VipleStream.
 *
 * Periodically discovers the server's public endpoint by sending STUN Binding
 * Requests from the control port. Also detects NAT type via double-probe.
 *
 * Reference: RFC 5389 (STUN)
 */

#include "stun.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "network.h"

// Forward-declare the port constant from stream.h to avoid pulling in rtsp_stream dependencies
namespace stream { constexpr auto CONTROL_PORT = 10; }

#include <cstring>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
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

using namespace std::literals;

namespace stun {

  // STUN constants (RFC 5389)
  static constexpr uint16_t BINDING_REQUEST  = 0x0001;
  static constexpr uint16_t BINDING_SUCCESS  = 0x0101;
  static constexpr uint32_t MAGIC_COOKIE     = 0x2112A442;
  static constexpr uint16_t ATTR_XOR_MAPPED  = 0x0020;
  static constexpr uint16_t ATTR_MAPPED      = 0x0001;

  static constexpr int PROBE_INTERVAL_SEC    = 60;
  static constexpr int RECV_TIMEOUT_MS       = 3000;

  // Default STUN servers (public, free)
  static constexpr const char *DEFAULT_STUN_1 = "stun.l.google.com";
  static constexpr const char *DEFAULT_STUN_2 = "stun1.l.google.com";
  static constexpr uint16_t    DEFAULT_STUN_PORT = 19302;

  #pragma pack(push, 1)
  struct stun_msg_t {
    uint16_t type;
    uint16_t length;
    uint32_t cookie;
    uint8_t  txn_id[12];
  };

  struct stun_attr_t {
    uint16_t type;
    uint16_t length;
  };

  struct stun_xor_addr_t {
    stun_attr_t hdr;
    uint8_t  reserved;
    uint8_t  family;   // 1=IPv4
    uint16_t port;
    uint32_t address;
  };
  #pragma pack(pop)

  // Thread-safe storage for the last probed endpoint
  static std::mutex s_mutex;
  static endpoint_t s_endpoint;

  endpoint_t get_endpoint() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_endpoint;
  }

  static void set_endpoint(const endpoint_t &ep) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_endpoint = ep;
  }

  /**
   * Send a STUN Binding Request from a specific local port and parse the response.
   * Returns the XOR-MAPPED-ADDRESS (our public IP:port as seen by the STUN server).
   */
  static bool probe_once(const char *stun_host, uint16_t stun_port,
                          uint16_t local_port, std::string &out_ip, uint16_t &out_port) {
    // Resolve STUN server
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", stun_port);

    if (getaddrinfo(stun_host, port_str, &hints, &res) != 0 || !res) {
      return false;
    }

    // Create UDP socket bound to the control port
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
      freeaddrinfo(res);
      return false;
    }

    // Allow address reuse (control port may already be bound by ENet)
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char *)&reuse, sizeof(reuse));
#endif

    // Bind to local_port (same as control port for consistent NAT mapping)
    struct sockaddr_in local_addr = {};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(local_port);

    // If bind fails (port in use), use ephemeral port — still useful for NAT type detection
    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
      local_addr.sin_port = 0;
      bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr));
    }

    // Set receive timeout
#ifdef _WIN32
    DWORD tv = RECV_TIMEOUT_MS;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv = { RECV_TIMEOUT_MS / 1000, (RECV_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Build STUN Binding Request
    stun_msg_t req = {};
    req.type = htons(BINDING_REQUEST);
    req.length = 0;
    req.cookie = htonl(MAGIC_COOKIE);

    // Random transaction ID
    std::random_device rd;
    for (int i = 0; i < 12; i++) {
      req.txn_id[i] = static_cast<uint8_t>(rd());
    }

    // Send request
    bool success = false;
    for (int attempt = 0; attempt < 3 && !success; attempt++) {
      if (sendto(sock, (const char *)&req, sizeof(req), 0,
                 res->ai_addr, (socklen_t)res->ai_addrlen) == SOCKET_ERROR) {
        continue;
      }

      // Receive response
      char buf[1024];
      int n = recvfrom(sock, buf, sizeof(buf), 0, nullptr, nullptr);
      if (n < (int)sizeof(stun_msg_t)) {
        continue;
      }

      auto *resp = reinterpret_cast<stun_msg_t *>(buf);
      if (ntohs(resp->type) != BINDING_SUCCESS) continue;
      if (ntohl(resp->cookie) != MAGIC_COOKIE) continue;
      if (memcmp(resp->txn_id, req.txn_id, 12) != 0) continue;

      // Parse attributes
      int offset = sizeof(stun_msg_t);
      while (offset + (int)sizeof(stun_attr_t) <= n) {
        auto *attr = reinterpret_cast<stun_attr_t *>(buf + offset);
        uint16_t attr_type = ntohs(attr->type) & 0x7FFF;
        uint16_t attr_len = ntohs(attr->length);

        if (offset + (int)sizeof(stun_attr_t) + attr_len > n) break;

        if (attr_type == ATTR_XOR_MAPPED || attr_type == ATTR_MAPPED) {
          auto *addr = reinterpret_cast<stun_xor_addr_t *>(attr);
          if (addr->family == 1 && attr_len == 8) { // IPv4
            uint32_t ip;
            uint16_t port;
            if (attr_type == ATTR_XOR_MAPPED) {
              ip = ntohl(addr->address ^ resp->cookie);
              port = ntohs(addr->port) ^ (uint16_t)(ntohl(resp->cookie) >> 16);
            } else {
              ip = ntohl(addr->address);
              port = ntohs(addr->port);
            }

            char ip_str[INET_ADDRSTRLEN];
            struct in_addr in;
            in.s_addr = htonl(ip);
            inet_ntop(AF_INET, &in, ip_str, sizeof(ip_str));

            out_ip = ip_str;
            out_port = port;
            success = true;
            break;
          }
        }
        offset += sizeof(stun_attr_t) + attr_len;
        // Align to 4-byte boundary
        offset = (offset + 3) & ~3;
      }
    }

    closesocket(sock);
    freeaddrinfo(res);
    return success;
  }

  /**
   * Detect NAT type by probing two different STUN servers from the same port.
   * If both return the same mapped address, NAT is EIM (hole-punchable).
   * If different, NAT is Symmetric (not hole-punchable).
   */
  static nat_type_e detect_nat_type(uint16_t local_port,
                                     const std::string &stun1, const std::string &stun2) {
    std::string ip1, ip2;
    uint16_t port1 = 0, port2 = 0;

    bool ok1 = probe_once(stun1.c_str(), DEFAULT_STUN_PORT, local_port, ip1, port1);
    bool ok2 = probe_once(stun2.c_str(), DEFAULT_STUN_PORT, local_port, ip2, port2);

    if (!ok1 || !ok2) return NAT_UNKNOWN;

    if (ip1 == ip2 && port1 == port2) {
      // Same mapping for different destinations → Endpoint Independent Mapping
      return NAT_RESTRICTED;  // Could be Full Cone or Restricted, but EIM is what matters for hole-punch
    } else {
      // Different mapping → Symmetric NAT
      return NAT_SYMMETRIC;
    }
  }

  static const char *nat_type_str(nat_type_e t) {
    switch (t) {
      case NAT_FULL_CONE:       return "full_cone";
      case NAT_RESTRICTED:      return "restricted";
      case NAT_PORT_RESTRICTED: return "port_restricted";
      case NAT_SYMMETRIC:       return "symmetric";
      default:                  return "unknown";
    }
  }

  /**
   * Background thread: periodically probe STUN and update the stored endpoint.
   */
  static void stun_thread_proc() {
    platf::set_thread_name("stun");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    // Parse STUN server from config (or use default)
    // Config may contain "host:port" format — split them
    std::string stun_server_1 = config::stream.stun_server;
    uint16_t stun_port_1 = DEFAULT_STUN_PORT;
    if (stun_server_1.empty()) {
      stun_server_1 = DEFAULT_STUN_1;
    } else {
      auto colon = stun_server_1.rfind(':');
      if (colon != std::string::npos) {
        try {
          stun_port_1 = (uint16_t)std::stoi(stun_server_1.substr(colon + 1));
          stun_server_1 = stun_server_1.substr(0, colon);
        } catch (...) {}
      }
    }
    std::string stun_server_2 = DEFAULT_STUN_2;

    uint16_t control_port = net::map_port(stream::CONTROL_PORT);

    BOOST_LOG(info) << "[STUN] Starting prober: server=" << stun_server_1
                    << ":" << stun_port_1 << ", local_port=" << control_port;

    do {
      std::string ip;
      uint16_t port = 0;

      if (probe_once(stun_server_1.c_str(), stun_port_1, control_port, ip, port)) {
        nat_type_e nat = detect_nat_type(control_port, stun_server_1, stun_server_2);

        endpoint_t ep;
        ep.ip = ip;
        ep.port = port;
        ep.nat_type = nat;
        set_endpoint(ep);

        BOOST_LOG(info) << "[STUN] Public endpoint: " << ip << ":" << port
                        << ", NAT type: " << nat_type_str(nat)
                        << (nat == NAT_SYMMETRIC ? " (hole-punch not possible)" : " (hole-punchable)");
      } else {
        BOOST_LOG(warning) << "[STUN] Probe failed (no response from " << stun_server_1 << ")";
      }
    } while (!shutdown_event->peek() &&
             shutdown_event->pop(std::chrono::seconds(PROBE_INTERVAL_SEC)));

    BOOST_LOG(info) << "[STUN] Prober stopped";
  }

  class deinit_t : public platf::deinit_t {
  public:
    deinit_t() {
      _thread = std::thread(stun_thread_proc);
    }

    ~deinit_t() override {
      if (_thread.joinable()) {
        _thread.join();
      }
    }

  private:
    std::thread _thread;
  };

  std::unique_ptr<platf::deinit_t> start() {
    return std::make_unique<deinit_t>();
  }

}  // namespace stun
