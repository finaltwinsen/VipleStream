/**
 * @file src/relay.h
 * @brief VipleStream signaling relay client.
 *
 * Connects to the relay server via WebSocket, registers the server UUID
 * and periodically publishes the STUN endpoint so clients can discover us.
 *
 * Also exposes a tunnel API used by the streaming path when direct UDP
 * and hole-punched UDP both fail. The tunnel uses:
 *   - relay-issued flow allocations (one per client session), and
 *   - WebSocket binary frames (opcode 0x2) over the same signaling WSS as
 *     a fallback carrier when outbound UDP to the relay is blocked.
 * The UDP-relay carrier is handled entirely in stream.cpp with its own
 * UDP socket; this module only covers the control plane and the WS
 * binary-frame fallback.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "platform/common.h"
#include "udp_tunnel.h"

namespace relay {

  /**
   * Start the relay client background thread.
   * Reads relay_url and relay_psk from config.
   * No-op if relay_url is empty.
   */
  std::unique_ptr<platf::deinit_t> start();

  /**
   * Request a tunnel flow from the relay to the given remote UUID.
   *
   * `cb` is invoked exactly once (on the relay thread or the calling thread)
   * with a populated Flow on success, or a default-constructed Flow
   * (flow_id == 0) on send failure, disconnect, or timeout.
   *
   * Thread-safe.
   */
  void allocate_flow(const std::string &target_uuid,
                     std::function<void(udp_tunnel::Flow)> cb);

  /**
   * Inform the relay we are done with a flow. No-op if not connected.
   * Thread-safe.
   */
  void close_flow(uint16_t flow_id);

  /**
   * Send the exact bytes as a single WebSocket binary frame (opcode 0x2).
   * Callers must have already built the 24-byte tunnel header + HMAC via
   * `udp_tunnel::encode_packet` — this function is transport-only.
   *
   * Returns true if the frame was queued for send. Returns false if the
   * relay is not currently connected. Thread-safe.
   */
  bool send_tunnel_binary(const uint8_t *data, size_t len);

  /**
   * Register a callback invoked for every inbound WS binary frame.
   * Runs on the relay thread — the handler must not block.
   * Pass an empty std::function to clear.
   * Thread-safe.
   */
  void set_tunnel_binary_handler(std::function<void(const uint8_t *, size_t)> cb);

}  // namespace relay
