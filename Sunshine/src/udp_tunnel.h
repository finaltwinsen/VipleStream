/**
 * @file src/udp_tunnel.h
 * @brief VipleStream UDP tunnel wire format and peer-side types.
 *
 * When direct peer-to-peer UDP and NAT hole punch both fail, Sunshine can
 * tunnel its streaming UDP traffic (video / audio / control / input) through
 * the signaling relay server. Two carriers are supported:
 *
 *  - UDP tunnel: send the datagram as a UDP packet to the relay's UDP port.
 *    Fast (single relay hop) but requires outbound UDP to the relay.
 *
 *  - WS tunnel:  send the datagram as a WebSocket binary frame (opcode 0x2)
 *    over the existing relay signaling WSS connection. Works anywhere the
 *    control WS already works (e.g. through Cloudflare Tunnel) but carries
 *    the TCP head-of-line-blocking cost.
 *
 * Both carriers share the same 24-byte authenticated header:
 *
 *     offset 0 : magic 'VP'           (2 bytes)
 *     offset 2 : flow_id              (uint16, big-endian)
 *     offset 4 : src_port             (uint16, big-endian)
 *     offset 6 : dst_port             (uint16, big-endian)
 *     offset 8 : HMAC-SHA256(token, magic||flow_id||src_port||dst_port||payload)[:16]
 *     offset 24: payload (opaque UDP datagram: RTP, ENet frame, etc.)
 *
 * The HMAC covers everything except its own bytes — both peers share the
 * 16-byte token handed out by the relay at allocation, so either side can
 * verify without the relay re-signing on forward.
 *
 * src_port / dst_port carry the Sunshine/Moonlight logical port (e.g.
 * 47999 for ENet control, 47998 for video RTP) so a single tunnel flow
 * can multiplex all four streaming ports between the same pair of peers.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace udp_tunnel {

  // Wire-format constants.
  inline constexpr uint8_t MAGIC_0 = 'V';
  inline constexpr uint8_t MAGIC_1 = 'P';
  inline constexpr size_t HEADER_LEN = 24;   // magic(2) + flow(2) + src(2) + dst(2) + hmac(16)
  inline constexpr size_t HMAC_LEN = 16;
  inline constexpr size_t TOKEN_LEN = 16;

  /**
   * Per-session preference for how to carry streaming UDP traffic.
   * Populated from config::stream.udp_tunnel_mode.
   */
  enum class Mode : int {
    AUTO = 0,          // Try direct → UDP tunnel → WS tunnel (probe-based)
    DIRECT_ONLY = 1,   // Only direct P2P; no relay fallback
    UDP_TUNNEL = 2,    // Always use relay UDP tunnel
    WS_TUNNEL = 3,     // Always use relay WS tunnel (worst latency, but most reachable)
  };

  Mode parse_mode(const std::string &s);
  const char *mode_name(Mode m);

  /**
   * Transport actually in use for a given flow at the moment.
   * AUTO is only a preference — the live state is one of the three carriers
   * below (or NONE before handshake / on shutdown).
   */
  enum class Carrier : int {
    NONE = 0,
    DIRECT = 1,
    UDP_RELAY = 2,
    WS_RELAY = 3,
  };

  const char *carrier_name(Carrier c);

  /**
   * One allocated tunnel flow between this Sunshine server and a specific
   * remote peer (client UUID). Populated when the relay responds to an
   * `udp_tunnel_allocate` request. The `token` is a shared secret used to
   * authenticate every tunnel datagram.
   */
  struct Flow {
    uint16_t flow_id = 0;                         // relay-assigned, 1..65535
    std::array<uint8_t, TOKEN_LEN> token{};       // HMAC key
    std::string relay_udp_host;                   // resolved by caller
    uint16_t relay_udp_port = 0;
    std::string remote_uuid;                      // peer on the other side
    bool is_side_a = false;                       // true if we requested allocate

    bool valid() const { return flow_id != 0; }
  };

  /**
   * Compute HMAC-SHA256(token, data)[:16].
   * Thread-safe; uses OpenSSL HMAC under the hood.
   */
  void compute_hmac16(const std::array<uint8_t, TOKEN_LEN> &token,
                      const uint8_t *data, size_t len,
                      uint8_t out[HMAC_LEN]);

  /**
   * Build one tunnel packet: [header][payload]. `out` is resized to
   * HEADER_LEN + payload_len bytes.
   */
  void encode_packet(const Flow &flow,
                     uint16_t src_port, uint16_t dst_port,
                     const uint8_t *payload, size_t payload_len,
                     std::vector<uint8_t> &out);

  /**
   * Validate an inbound tunnel packet (check magic + HMAC under this flow's
   * token). On success fills the out parameters and returns true; on any
   * validation failure returns false without touching the out params.
   *
   * The returned pointer aliases the input buffer, so the caller must keep
   * the buffer alive while using the payload.
   */
  bool parse_packet(const Flow &flow,
                    const uint8_t *data, size_t len,
                    uint16_t &out_src_port, uint16_t &out_dst_port,
                    const uint8_t *&out_payload, size_t &out_payload_len);

  /**
   * Convenience helpers to convert between the hex token used on the WS
   * signaling wire and the raw 16-byte form used for HMAC.
   */
  bool hex_to_token(const std::string &hex, std::array<uint8_t, TOKEN_LEN> &out);
  std::string token_to_hex(const std::array<uint8_t, TOKEN_LEN> &token);

}  // namespace udp_tunnel
