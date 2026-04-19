/**
 * @file src/tunnel_session.h
 * @brief Per-stream-session tunnel carrier for VipleStream relay.
 *
 * When direct peer-to-peer UDP and NAT hole-punch both fail, the relay
 * provides two fallback carriers for Sunshine↔Moonlight streaming
 * traffic: a dedicated UDP relay (single extra hop) and a WebSocket
 * binary-frame fallback (for environments where the client can reach
 * the relay only over TCP/TLS via Cloudflare Tunnel).
 *
 * `tunnel_session::TunnelSession` is the session-scoped object that
 * owns the allocated relay flow, picks the active carrier, and
 * transports encoded packets on behalf of the video / audio / control
 * streams. Stream code calls `send(src, dst, payload, len)` to emit a
 * datagram and registers per-logical-port handlers to receive.
 *
 * All tunnel packets use the wire format defined in `udp_tunnel.h`:
 * a 24-byte header (magic + flow_id + src/dst ports + HMAC-SHA256[:16])
 * followed by the opaque payload bytes.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "udp_tunnel.h"

namespace tunnel_session {

  /**
   * Tunnel carrier owned by a single streaming session. One object
   * per client session. Lives for the duration of the session.
   *
   * Thread model:
   *   - Send path is thread-safe and may be called from the video /
   *     audio / control broadcast threads concurrently.
   *   - Receive path runs an internal thread when the UDP_RELAY
   *     carrier is active; it calls the registered PortHandler
   *     callbacks on that thread. Handlers must be non-blocking.
   *   - When the active carrier is WS_RELAY the relay::set_tunnel_
   *     binary_handler delivers inbound frames on the relay WS thread.
   */
  class TunnelSession {
  public:
    using PortHandler = std::function<void(const uint8_t *data,
                                           size_t len,
                                           uint16_t src_port)>;

    TunnelSession();
    ~TunnelSession();

    TunnelSession(const TunnelSession &) = delete;
    TunnelSession &operator=(const TunnelSession &) = delete;

    /** The flow this session is bound to. valid() iff start() succeeded. */
    const udp_tunnel::Flow &flow() const { return _flow; }

    /** Carrier currently dispatching traffic. NONE until start(). */
    udp_tunnel::Carrier carrier() const { return _carrier.load(); }

    /**
     * Bind to an allocated relay flow and open the chosen carrier.
     *
     *   AUTO / UDP_TUNNEL → try to open a UDP socket to
     *                       flow.relay_udp_host:relay_udp_port;
     *                       on failure fall back to WS_RELAY.
     *   WS_TUNNEL         → skip UDP and use WS binary frames.
     *   DIRECT_ONLY       → no-op, carrier stays DIRECT; callers should
     *                       route through their existing UDP socket.
     *
     * Returns true iff a usable carrier was established.
     * Calling start() again stops the previous carrier first.
     */
    bool start(const udp_tunnel::Flow &flow, udp_tunnel::Mode mode);

    /**
     * Close the carrier, stop the receive thread, and tell the relay
     * we are done with this flow. Safe to call multiple times; called
     * automatically on destruction.
     */
    void stop();

    /**
     * Send one datagram through the active carrier. `src_port` and
     * `dst_port` carry the Sunshine/Moonlight logical port so the
     * receiving peer can demux to the right stream. Returns false if
     * not connected or the send fails on both UDP and WS carriers.
     */
    bool send(uint16_t src_port, uint16_t dst_port,
              const uint8_t *payload, size_t len);

    /**
     * Register a handler for inbound packets whose `dst_port` field
     * equals the given port. Pass {} to clear. Called on the carrier's
     * receive thread (UDP_RELAY) or the relay WS thread (WS_RELAY).
     */
    void set_handler(uint16_t dst_port, PortHandler handler);

    /**
     * Enable a loopback bridge for ENet control on the given port.
     *
     * Sunshine's ENet is bound to `bound_port` locally (e.g. 47999).
     * The bridge opens a shim UDP socket on 127.0.0.1:<ephemeral> and:
     *   - Inbound tunnel packets with dst_port==bound_port are
     *     forwarded to 127.0.0.1:bound_port via the shim so ENet sees
     *     them arrive from the shim's loopback address.
     *   - ENet replies (sent back to the shim's loopback port) are
     *     tunneled out with src_port=bound_port, dst_port=<client's
     *     ENet ephemeral port learned from the first inbound packet>.
     *
     * This lets the client's ENet connect through the relay without
     * any changes to libenet's socket code.
     *
     * Must be called after start(); stop() also tears the bridge down.
     * Returns true iff the shim socket and receive thread came up.
     */
    bool enable_local_bridge(uint16_t bound_port);

  private:
    void udp_recv_loop();
    void dispatch(const uint8_t *data, size_t len);
    void bridge_recv_loop();

    udp_tunnel::Flow _flow;
    std::atomic<udp_tunnel::Carrier> _carrier{udp_tunnel::Carrier::NONE};

    // UDP-relay carrier state. Only populated when a UDP socket to
    // the relay succeeds; the WS carrier needs no local state beyond
    // relay::send_tunnel_binary / set_tunnel_binary_handler.
    std::unique_ptr<boost::asio::io_context> _udp_io;
    std::unique_ptr<boost::asio::ip::udp::socket> _udp_sock;
    boost::asio::ip::udp::endpoint _udp_endpoint;
    std::thread _udp_thread;
    std::atomic<bool> _udp_running{false};

    // Send-side serialization for the UDP socket. asio's send_to is
    // not explicitly documented as MT-safe for concurrent writers.
    std::mutex _udp_send_mtx;

    // Port dispatch table.
    std::mutex _handlers_mtx;
    std::unordered_map<uint16_t, PortHandler> _handlers;

    // ENet loopback bridge state. Only populated when enable_local_bridge
    // is called. The shim socket is bound to 127.0.0.1:ephemeral; its
    // receive thread reads ENet replies destined to that loopback port
    // and tunnels them back to the peer.
    std::unique_ptr<boost::asio::io_context> _bridge_io;
    std::unique_ptr<boost::asio::ip::udp::socket> _bridge_sock;
    boost::asio::ip::udp::endpoint _bridge_loopback;   // 127.0.0.1:bound_port
    std::thread _bridge_thread;
    std::atomic<bool> _bridge_running{false};
    uint16_t _bridge_server_port{0};                   // e.g. 47999
    std::atomic<uint16_t> _bridge_peer_port{0};        // client's ENet ephemeral
    std::mutex _bridge_send_mtx;
  };

}  // namespace tunnel_session
