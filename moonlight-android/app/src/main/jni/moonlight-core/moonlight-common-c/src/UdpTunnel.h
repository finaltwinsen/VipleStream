/*
 * UdpTunnel.h — VipleStream relay UDP tunnel wire format (client side).
 *
 * Mirror of Sunshine/src/udp_tunnel.h. See that file for the motivation
 * and the full wire-format description. This C header exists so that
 * both moonlight-qt and moonlight-android can share the header
 * encode/decode and HMAC helpers without pulling in Sunshine's C++
 * implementation.
 *
 * Wire format (24-byte header + payload):
 *
 *   offset 0 : magic 'V' 'P'
 *   offset 2 : flow_id   (uint16 BE, relay-assigned)
 *   offset 4 : src_port  (uint16 BE)
 *   offset 6 : dst_port  (uint16 BE)
 *   offset 8 : HMAC-SHA256(token, header[0..7] || payload)[:16]
 *   offset 24: payload (RTP / ENet / ping / whatever)
 */
#ifndef VIPLESTREAM_UDPTUNNEL_H
#define VIPLESTREAM_UDPTUNNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP_UDPTUNNEL_MAGIC_0   'V'
#define VP_UDPTUNNEL_MAGIC_1   'P'
#define VP_UDPTUNNEL_HEADER_LEN  24u
#define VP_UDPTUNNEL_HMAC_LEN    16u
#define VP_UDPTUNNEL_TOKEN_LEN   16u

typedef enum {
    VP_TUNNEL_MODE_AUTO         = 0,
    VP_TUNNEL_MODE_DIRECT_ONLY  = 1,
    VP_TUNNEL_MODE_UDP_TUNNEL   = 2,
    VP_TUNNEL_MODE_WS_TUNNEL    = 3
} VpTunnelMode;

typedef enum {
    VP_CARRIER_NONE      = 0,
    VP_CARRIER_DIRECT    = 1,
    VP_CARRIER_UDP_RELAY = 2,
    VP_CARRIER_WS_RELAY  = 3
} VpCarrier;

typedef struct VpTunnelFlow {
    uint16_t flow_id;                              /* 1..65535, 0 == invalid */
    uint8_t  token[VP_UDPTUNNEL_TOKEN_LEN];        /* shared HMAC key */
} VpTunnelFlow;

/* Compute HMAC-SHA256(token, data[:len])[:16] into out. Thread-safe. */
void VpTunnel_hmac16(const uint8_t token[VP_UDPTUNNEL_TOKEN_LEN],
                     const uint8_t *data, size_t len,
                     uint8_t out[VP_UDPTUNNEL_HMAC_LEN]);

/*
 * Fill `out` (caller-provided, at least VP_UDPTUNNEL_HEADER_LEN + payload_len
 * bytes) with one complete tunnel packet. Returns the number of bytes
 * written (always VP_UDPTUNNEL_HEADER_LEN + payload_len).
 */
size_t VpTunnel_encode(const VpTunnelFlow *flow,
                       uint16_t src_port, uint16_t dst_port,
                       const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t out_capacity);

/*
 * Validate an inbound tunnel packet. On success fills the out pointers
 * (payload aliases `data`, caller owns the underlying buffer) and
 * returns 1. On any failure (bad magic, bad flow, bad HMAC) returns 0.
 */
int VpTunnel_parse(const VpTunnelFlow *flow,
                   const uint8_t *data, size_t len,
                   uint16_t *out_src_port, uint16_t *out_dst_port,
                   const uint8_t **out_payload, size_t *out_payload_len);

/*
 * Parse a 32-hex-char token string into a 16-byte raw token.
 * Returns 1 on success, 0 on any invalid input (length or non-hex char).
 */
int VpTunnel_hex_to_token(const char *hex,
                          uint8_t out[VP_UDPTUNNEL_TOKEN_LEN]);

/*
 * Hex-encode a 16-byte token into `out` (33 bytes including NUL).
 */
void VpTunnel_token_to_hex(const uint8_t token[VP_UDPTUNNEL_TOKEN_LEN],
                           char out[VP_UDPTUNNEL_TOKEN_LEN * 2 + 1]);

#ifdef __cplusplus
}
#endif

#endif /* VIPLESTREAM_UDPTUNNEL_H */
