/*
 * UdpTunnel.c — VipleStream relay UDP tunnel wire format (client side).
 * See UdpTunnel.h for the wire layout and full documentation.
 */
#include "UdpTunnel.h"

#include <string.h>

#include "PlatformCrypto.h"

/*
 * Platform-agnostic HMAC-SHA256 via PlatformCrypto. moonlight-common-c
 * already wraps openssl's HMAC in PlatformCrypto_hmac256 (or an
 * equivalent mbedtls path on Android), so this file avoids including
 * openssl/hmac.h directly.
 *
 * The first 16 bytes of the full 32-byte HMAC are used; truncating HMAC
 * is standard practice (RFC 2104 §5).
 */
void VpTunnel_hmac16(const uint8_t token[VP_UDPTUNNEL_TOKEN_LEN],
                     const uint8_t *data, size_t len,
                     uint8_t out[VP_UDPTUNNEL_HMAC_LEN]) {
    uint8_t full[32];
    PltHmac256(token, VP_UDPTUNNEL_TOKEN_LEN, data, len, full);
    memcpy(out, full, VP_UDPTUNNEL_HMAC_LEN);
}

static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

size_t VpTunnel_encode(const VpTunnelFlow *flow,
                       uint16_t src_port, uint16_t dst_port,
                       const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t out_capacity) {
    size_t total = VP_UDPTUNNEL_HEADER_LEN + payload_len;
    if (out_capacity < total) return 0;

    /* Fixed 8-byte prefix: magic + flow + src + dst. */
    out[0] = VP_UDPTUNNEL_MAGIC_0;
    out[1] = VP_UDPTUNNEL_MAGIC_1;
    write_be16(out + 2, flow->flow_id);
    write_be16(out + 4, src_port);
    write_be16(out + 6, dst_port);

    /* Append payload now so HMAC input is [fixed 8][payload]. */
    if (payload_len > 0 && payload != NULL) {
        memcpy(out + VP_UDPTUNNEL_HEADER_LEN, payload, payload_len);
    }

    /*
     * Build HMAC input on the stack when small; fall back to
     * chunked HMAC via a heap buffer only if the payload is large.
     * Moonlight packets are typically under 1500 bytes so the stack
     * path is the common case.
     */
    uint8_t stackbuf[2048];
    size_t signed_len = 8 + payload_len;
    uint8_t *signbuf = stackbuf;
    uint8_t *heapbuf = NULL;
    if (signed_len > sizeof(stackbuf)) {
        heapbuf = (uint8_t *)malloc(signed_len);
        if (!heapbuf) return 0;
        signbuf = heapbuf;
    }
    memcpy(signbuf, out, 8);
    if (payload_len > 0 && payload != NULL) {
        memcpy(signbuf + 8, payload, payload_len);
    }

    uint8_t mac[VP_UDPTUNNEL_HMAC_LEN];
    VpTunnel_hmac16(flow->token, signbuf, signed_len, mac);
    if (heapbuf) free(heapbuf);

    memcpy(out + 8, mac, VP_UDPTUNNEL_HMAC_LEN);
    return total;
}

int VpTunnel_parse(const VpTunnelFlow *flow,
                   const uint8_t *data, size_t len,
                   uint16_t *out_src_port, uint16_t *out_dst_port,
                   const uint8_t **out_payload, size_t *out_payload_len) {
    if (len < VP_UDPTUNNEL_HEADER_LEN) return 0;
    if (data[0] != VP_UDPTUNNEL_MAGIC_0 || data[1] != VP_UDPTUNNEL_MAGIC_1) return 0;

    uint16_t flow_id  = read_be16(data + 2);
    if (flow_id != flow->flow_id) return 0;
    uint16_t src_port = read_be16(data + 4);
    uint16_t dst_port = read_be16(data + 6);
    const uint8_t *recv_mac    = data + 8;
    const uint8_t *payload     = data + VP_UDPTUNNEL_HEADER_LEN;
    size_t         payload_len = len - VP_UDPTUNNEL_HEADER_LEN;

    uint8_t stackbuf[2048];
    size_t signed_len = 8 + payload_len;
    uint8_t *signbuf = stackbuf;
    uint8_t *heapbuf = NULL;
    if (signed_len > sizeof(stackbuf)) {
        heapbuf = (uint8_t *)malloc(signed_len);
        if (!heapbuf) return 0;
        signbuf = heapbuf;
    }
    memcpy(signbuf, data, 8);
    if (payload_len > 0) memcpy(signbuf + 8, payload, payload_len);

    uint8_t expected[VP_UDPTUNNEL_HMAC_LEN];
    VpTunnel_hmac16(flow->token, signbuf, signed_len, expected);
    if (heapbuf) free(heapbuf);

    /* Constant-time compare to avoid timing leaks. */
    unsigned diff = 0;
    for (size_t i = 0; i < VP_UDPTUNNEL_HMAC_LEN; i++) {
        diff |= (unsigned)(expected[i] ^ recv_mac[i]);
    }
    if (diff != 0) return 0;

    if (out_src_port) *out_src_port = src_port;
    if (out_dst_port) *out_dst_port = dst_port;
    if (out_payload) *out_payload = payload;
    if (out_payload_len) *out_payload_len = payload_len;
    return 1;
}

static int hex_nibble(char c, uint8_t *v) {
    if (c >= '0' && c <= '9') { *v = (uint8_t)(c - '0'); return 1; }
    if (c >= 'a' && c <= 'f') { *v = (uint8_t)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *v = (uint8_t)(c - 'A' + 10); return 1; }
    return 0;
}

int VpTunnel_hex_to_token(const char *hex, uint8_t out[VP_UDPTUNNEL_TOKEN_LEN]) {
    if (!hex || strlen(hex) != VP_UDPTUNNEL_TOKEN_LEN * 2) return 0;
    for (size_t i = 0; i < VP_UDPTUNNEL_TOKEN_LEN; i++) {
        uint8_t hi, lo;
        if (!hex_nibble(hex[2 * i], &hi) || !hex_nibble(hex[2 * i + 1], &lo)) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

void VpTunnel_token_to_hex(const uint8_t token[VP_UDPTUNNEL_TOKEN_LEN],
                           char out[VP_UDPTUNNEL_TOKEN_LEN * 2 + 1]) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < VP_UDPTUNNEL_TOKEN_LEN; i++) {
        out[2 * i]     = H[(token[i] >> 4) & 0x0F];
        out[2 * i + 1] = H[token[i] & 0x0F];
    }
    out[VP_UDPTUNNEL_TOKEN_LEN * 2] = '\0';
}
