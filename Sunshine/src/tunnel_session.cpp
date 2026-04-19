/**
 * @file src/tunnel_session.cpp
 * @brief TunnelSession implementation — see header for semantics.
 */
#include "tunnel_session.h"

#include <chrono>

#include "logging.h"
#include "relay.h"

namespace tunnel_session {

  namespace asio = boost::asio;

  TunnelSession::TunnelSession() = default;

  TunnelSession::~TunnelSession() {
    stop();
  }

  bool TunnelSession::start(const udp_tunnel::Flow &flow,
                            udp_tunnel::Mode mode) {
    stop();
    if (!flow.valid()) return false;
    _flow = flow;

    // Always install the WS binary handler; for UDP_RELAY this acts
    // as a backup so the peer can still reach us via WS if its UDP
    // leg is blocked. Only this TunnelSession is active at a time
    // per process, so a single global handler is sufficient.
    relay::set_tunnel_binary_handler(
      [this](const uint8_t *data, size_t len) {
        dispatch(data, len);
      });

    const bool try_udp = (mode == udp_tunnel::Mode::AUTO ||
                          mode == udp_tunnel::Mode::UDP_TUNNEL);

    if (try_udp && !_flow.relay_udp_host.empty() &&
        _flow.relay_udp_port > 0) {
      try {
        _udp_io = std::make_unique<asio::io_context>();
        _udp_sock = std::make_unique<asio::ip::udp::socket>(*_udp_io);
        _udp_sock->open(asio::ip::udp::v4());

        asio::ip::udp::resolver r(*_udp_io);
        auto results = r.resolve(_flow.relay_udp_host,
                                 std::to_string(_flow.relay_udp_port));
        if (results.empty()) {
          throw std::runtime_error("relay host did not resolve");
        }
        _udp_endpoint = results.begin()->endpoint();

        _udp_running.store(true);
        _udp_thread = std::thread([this] { udp_recv_loop(); });
        _carrier.store(udp_tunnel::Carrier::UDP_RELAY);
        BOOST_LOG(info) << "[TUNNEL] flow " << _flow.flow_id
                        << " carrier=UDP_RELAY via "
                        << _flow.relay_udp_host << ":"
                        << _flow.relay_udp_port;
        return true;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "[TUNNEL] UDP carrier failed, falling back "
                              "to WS: " << e.what();
        if (_udp_sock) {
          boost::system::error_code ec;
          _udp_sock->close(ec);
        }
        _udp_sock.reset();
        _udp_io.reset();
      }
    }

    if (mode == udp_tunnel::Mode::DIRECT_ONLY) {
      _carrier.store(udp_tunnel::Carrier::DIRECT);
      return true;
    }

    _carrier.store(udp_tunnel::Carrier::WS_RELAY);
    BOOST_LOG(info) << "[TUNNEL] flow " << _flow.flow_id
                    << " carrier=WS_RELAY";
    return true;
  }

  void TunnelSession::stop() {
    // Tear down the ENet loopback bridge first so its send path no
    // longer touches the tunnel state we're about to clear.
    _bridge_running.store(false);
    if (_bridge_sock) {
      boost::system::error_code ec;
      _bridge_sock->close(ec);
    }
    if (_bridge_io) _bridge_io->stop();
    if (_bridge_thread.joinable()) _bridge_thread.join();
    _bridge_sock.reset();
    _bridge_io.reset();
    _bridge_server_port = 0;
    _bridge_peer_port.store(0);

    _udp_running.store(false);
    if (_udp_sock) {
      boost::system::error_code ec;
      _udp_sock->close(ec);  // unblocks a blocking receive_from
    }
    if (_udp_io) _udp_io->stop();
    if (_udp_thread.joinable()) _udp_thread.join();
    _udp_sock.reset();
    _udp_io.reset();

    if (_flow.valid()) {
      relay::close_flow(_flow.flow_id);
      relay::set_tunnel_binary_handler({});
      _flow = {};
    }

    {
      std::lock_guard<std::mutex> l(_handlers_mtx);
      _handlers.clear();
    }
    _carrier.store(udp_tunnel::Carrier::NONE);
  }

  bool TunnelSession::enable_local_bridge(uint16_t bound_port) {
    if (_bridge_running.load()) return true;   // already set up
    if (bound_port == 0) return false;
    auto c = _carrier.load();
    if (c == udp_tunnel::Carrier::NONE) return false;

    try {
      _bridge_io = std::make_unique<asio::io_context>();
      _bridge_sock = std::make_unique<asio::ip::udp::socket>(*_bridge_io);
      _bridge_sock->open(asio::ip::udp::v4());

      // Bind to 127.0.0.1:ephemeral. We intentionally use loopback-only
      // so no external traffic can reach ENet through this shim.
      asio::ip::udp::endpoint any(asio::ip::make_address_v4("127.0.0.1"), 0);
      _bridge_sock->bind(any);

      // Target for injection: Sunshine's ENet listener on this host.
      _bridge_loopback = asio::ip::udp::endpoint(
        asio::ip::make_address_v4("127.0.0.1"), bound_port);
      _bridge_server_port = bound_port;
      _bridge_peer_port.store(0);

      _bridge_running.store(true);
      _bridge_thread = std::thread([this] { bridge_recv_loop(); });

      // When a tunnel packet arrives with dst_port==bound_port, inject
      // it into ENet via the shim socket. ENet sees the peer at
      // 127.0.0.1:<shim_ephemeral>. We also remember the client's
      // src_port so the reply path can tunnel back to the right peer.
      // Hot-path handler: no per-packet log. Captures the client's
      // ENet ephemeral port on the first packet (and any time it
      // changes) so the reply path can tunnel replies back to it.
      set_handler(bound_port,
        [this](const uint8_t *data, size_t len, uint16_t src_port) {
          bool peer_changed =
            _bridge_peer_port.exchange(src_port) != src_port;
          if (peer_changed) {
            BOOST_LOG(info) << "[TUNNEL/BRIDGE] learned client ENet "
                               "port " << src_port;
          }
          if (!_bridge_sock) return;
          boost::system::error_code ec;
          std::lock_guard<std::mutex> l(_bridge_send_mtx);
          _bridge_sock->send_to(asio::buffer(data, len),
                                _bridge_loopback, 0, ec);
        });

      BOOST_LOG(info) << "[TUNNEL] ENet loopback bridge up for port "
                      << bound_port;
      return true;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "[TUNNEL] ENet bridge setup failed: " << e.what();
      _bridge_running.store(false);
      if (_bridge_sock) {
        boost::system::error_code ec;
        _bridge_sock->close(ec);
      }
      _bridge_sock.reset();
      _bridge_io.reset();
      _bridge_server_port = 0;
      return false;
    }
  }

  void TunnelSession::bridge_recv_loop() {
    std::vector<uint8_t> buf(2048);
    asio::ip::udp::endpoint sender;
    while (_bridge_running.load() && _bridge_sock) {
      boost::system::error_code ec;
      size_t n = _bridge_sock->receive_from(asio::buffer(buf), sender, 0, ec);
      if (ec) {
        if (!_bridge_running.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (sender != _bridge_loopback) continue;
      uint16_t peer_port = _bridge_peer_port.load();
      if (peer_port == 0) continue;  // client port not learned yet
      send(_bridge_server_port, peer_port, buf.data(), n);
    }
  }

  bool TunnelSession::send(uint16_t src_port, uint16_t dst_port,
                           const uint8_t *payload, size_t len) {
    if (!_flow.valid()) return false;
    auto c = _carrier.load();
    if (c == udp_tunnel::Carrier::NONE ||
        c == udp_tunnel::Carrier::DIRECT) {
      return false;
    }

    std::vector<uint8_t> buf;
    udp_tunnel::encode_packet(_flow, src_port, dst_port,
                              payload, len, buf);

    if (c == udp_tunnel::Carrier::UDP_RELAY && _udp_sock) {
      boost::system::error_code ec;
      {
        std::lock_guard<std::mutex> l(_udp_send_mtx);
        _udp_sock->send_to(asio::buffer(buf), _udp_endpoint, 0, ec);
      }
      if (!ec) return true;
      BOOST_LOG(debug) << "[TUNNEL] UDP send failed (" << ec.message()
                       << "), failing over to WS";
      _carrier.store(udp_tunnel::Carrier::WS_RELAY);
    }

    return relay::send_tunnel_binary(buf.data(), buf.size());
  }

  void TunnelSession::set_handler(uint16_t dst_port, PortHandler handler) {
    std::lock_guard<std::mutex> l(_handlers_mtx);
    if (handler) {
      _handlers[dst_port] = std::move(handler);
    } else {
      _handlers.erase(dst_port);
    }
  }

  void TunnelSession::udp_recv_loop() {
    // Max UDP payload is 65507 bytes; Sunshine's RTP packets are much
    // smaller (≤ MTU). 2048 covers tunnel header + payload generously.
    std::vector<uint8_t> buf(2048);
    asio::ip::udp::endpoint sender;

    while (_udp_running.load() && _udp_sock) {
      boost::system::error_code ec;
      size_t n = _udp_sock->receive_from(asio::buffer(buf), sender, 0, ec);
      if (ec) {
        if (!_udp_running.load()) break;
        // Transient error (interrupt, bad source, etc.) — back off.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      // Silently ignore datagrams from unexpected sources: the relay
      // endpoint is the only valid peer and the HMAC check would reject
      // anything else anyway, but rejecting by address saves CPU.
      if (sender != _udp_endpoint) continue;
      dispatch(buf.data(), n);
    }
  }

  void TunnelSession::dispatch(const uint8_t *data, size_t len) {
    if (!_flow.valid()) return;

    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
    if (!udp_tunnel::parse_packet(_flow, data, len,
                                  src_port, dst_port,
                                  payload, payload_len)) {
      return;  // magic/flow/HMAC mismatch — silent drop
    }

    PortHandler h;
    {
      std::lock_guard<std::mutex> l(_handlers_mtx);
      auto it = _handlers.find(dst_port);
      if (it != _handlers.end()) h = it->second;
    }
    if (h) h(payload, payload_len, src_port);
  }

}  // namespace tunnel_session
