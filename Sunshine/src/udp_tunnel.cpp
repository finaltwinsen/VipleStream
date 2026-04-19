/**
 * @file src/udp_tunnel.cpp
 * @brief Wire-format encode/decode for the VipleStream relay UDP tunnel.
 *
 * This module is intentionally free of any I/O or asio dependency — it only
 * manipulates buffers and crypto. The actual socket / websocket carriers are
 * wired up in stream.cpp and relay.cpp in later phases.
 */
#include "udp_tunnel.h"

#include <cstring>

#include <openssl/hmac.h>
#include <openssl/evp.h>

namespace udp_tunnel {

  Mode parse_mode(const std::string &s) {
    if (s == "direct" || s == "direct_only") return Mode::DIRECT_ONLY;
    if (s == "udp" || s == "udp_tunnel")     return Mode::UDP_TUNNEL;
    if (s == "ws"  || s == "ws_tunnel")      return Mode::WS_TUNNEL;
    // "auto", empty, and anything unknown fall back to AUTO.
    return Mode::AUTO;
  }

  const char *mode_name(Mode m) {
    switch (m) {
      case Mode::AUTO:        return "auto";
      case Mode::DIRECT_ONLY: return "direct_only";
      case Mode::UDP_TUNNEL:  return "udp_tunnel";
      case Mode::WS_TUNNEL:   return "ws_tunnel";
    }
    return "?";
  }

  const char *carrier_name(Carrier c) {
    switch (c) {
      case Carrier::NONE:      return "none";
      case Carrier::DIRECT:    return "direct";
      case Carrier::UDP_RELAY: return "udp-relay";
      case Carrier::WS_RELAY:  return "ws-relay";
    }
    return "?";
  }

  void compute_hmac16(const std::array<uint8_t, TOKEN_LEN> &token,
                      const uint8_t *data, size_t len,
                      uint8_t out[HMAC_LEN]) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), token.data(), static_cast<int>(token.size()),
         data, len, mac, &mac_len);
    // Truncate to first 16 bytes. Truncating HMAC is standard practice —
    // see RFC 2104 §5 and TLS 1.2 CBC-HMAC constructions.
    std::memcpy(out, mac, HMAC_LEN);
  }

  static void write_be16(std::vector<uint8_t> &buf, size_t offset, uint16_t v) {
    buf[offset]     = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>(v & 0xFF);
  }

  static uint16_t read_be16(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
  }

  void encode_packet(const Flow &flow,
                     uint16_t src_port, uint16_t dst_port,
                     const uint8_t *payload, size_t payload_len,
                     std::vector<uint8_t> &out) {
    out.resize(HEADER_LEN + payload_len);
    // magic, flow, src, dst — fill the first 8 bytes.
    out[0] = MAGIC_0;
    out[1] = MAGIC_1;
    write_be16(out, 2, flow.flow_id);
    write_be16(out, 4, src_port);
    write_be16(out, 6, dst_port);
    // HMAC slot (offset 8..23) is filled below; append payload first so the
    // HMAC input layout is exactly [fixed header 8B][payload] (i.e. the HMAC
    // bytes are excluded from the HMAC input).
    if (payload_len) {
      std::memcpy(out.data() + HEADER_LEN, payload, payload_len);
    }
    // Build the HMAC input: 8 header bytes + payload (HMAC slot itself is
    // excluded, matching the wire spec).
    std::vector<uint8_t> signed_buf;
    signed_buf.reserve(8 + payload_len);
    signed_buf.insert(signed_buf.end(), out.begin(), out.begin() + 8);
    if (payload_len) {
      signed_buf.insert(signed_buf.end(), payload, payload + payload_len);
    }
    uint8_t hmac[HMAC_LEN];
    compute_hmac16(flow.token, signed_buf.data(), signed_buf.size(), hmac);
    std::memcpy(out.data() + 8, hmac, HMAC_LEN);
  }

  bool parse_packet(const Flow &flow,
                    const uint8_t *data, size_t len,
                    uint16_t &out_src_port, uint16_t &out_dst_port,
                    const uint8_t *&out_payload, size_t &out_payload_len) {
    if (len < HEADER_LEN) return false;
    if (data[0] != MAGIC_0 || data[1] != MAGIC_1) return false;

    const uint16_t flow_id  = read_be16(data + 2);
    if (flow_id != flow.flow_id) return false;
    const uint16_t src_port = read_be16(data + 4);
    const uint16_t dst_port = read_be16(data + 6);
    const uint8_t *recv_mac = data + 8;
    const uint8_t *payload  = data + HEADER_LEN;
    const size_t   payload_len = len - HEADER_LEN;

    std::vector<uint8_t> signed_buf;
    signed_buf.reserve(8 + payload_len);
    signed_buf.insert(signed_buf.end(), data, data + 8);
    if (payload_len) {
      signed_buf.insert(signed_buf.end(), payload, payload + payload_len);
    }
    uint8_t expected[HMAC_LEN];
    compute_hmac16(flow.token, signed_buf.data(), signed_buf.size(), expected);
    // Constant-time compare to avoid timing leaks on mismatch.
    unsigned diff = 0;
    for (size_t i = 0; i < HMAC_LEN; ++i) {
      diff |= static_cast<unsigned>(expected[i] ^ recv_mac[i]);
    }
    if (diff != 0) return false;

    out_src_port = src_port;
    out_dst_port = dst_port;
    out_payload = payload;
    out_payload_len = payload_len;
    return true;
  }

  bool hex_to_token(const std::string &hex, std::array<uint8_t, TOKEN_LEN> &out) {
    if (hex.size() != TOKEN_LEN * 2) return false;
    auto nibble = [](char c, uint8_t &v) {
      if (c >= '0' && c <= '9') { v = static_cast<uint8_t>(c - '0'); return true; }
      if (c >= 'a' && c <= 'f') { v = static_cast<uint8_t>(c - 'a' + 10); return true; }
      if (c >= 'A' && c <= 'F') { v = static_cast<uint8_t>(c - 'A' + 10); return true; }
      return false;
    };
    for (size_t i = 0; i < TOKEN_LEN; ++i) {
      uint8_t hi, lo;
      if (!nibble(hex[2 * i], hi) || !nibble(hex[2 * i + 1], lo)) return false;
      out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
  }

  std::string token_to_hex(const std::array<uint8_t, TOKEN_LEN> &token) {
    static const char *const H = "0123456789abcdef";
    std::string s;
    s.resize(TOKEN_LEN * 2);
    for (size_t i = 0; i < TOKEN_LEN; ++i) {
      s[2 * i]     = H[(token[i] >> 4) & 0x0F];
      s[2 * i + 1] = H[token[i] & 0x0F];
    }
    return s;
  }

}  // namespace udp_tunnel
