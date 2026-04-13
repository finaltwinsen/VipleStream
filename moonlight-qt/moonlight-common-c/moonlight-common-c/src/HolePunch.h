/**
 * VipleStream: UDP Hole Punch Protocol
 *
 * Used to establish NAT mappings before ENet connection.
 * Punch packets share the control port (47999) with ENet.
 * ENet ignores them (different magic), our pre-filter intercepts them.
 */
#pragma once

#include <stdint.h>

#define HOLEPUNCH_MAGIC 0x56504C50  /* "VPLP" */
#define HOLEPUNCH_VERSION 1

#define HOLEPUNCH_SYN     0
#define HOLEPUNCH_ACK     1
#define HOLEPUNCH_CONFIRM 2

#pragma pack(push, 1)
typedef struct _HOLEPUNCH_PACKET {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;       /* SYN, ACK, CONFIRM */
    uint8_t  serverUuid[16];
    uint8_t  clientUuid[16];
    uint32_t timestamp;  /* milliseconds, for RTT measurement */
} HOLEPUNCH_PACKET;
#pragma pack(pop)

/**
 * Client-side: Send punch SYN packets to the server's STUN endpoint,
 * wait for ACK, then send CONFIRM.
 *
 * @param serverAddr  Server's STUN-discovered public IP string
 * @param serverPort  Server's STUN-discovered public port
 * @param serverUuid  Server's UUID (16 bytes)
 * @param clientUuid  Client's UUID (16 bytes)
 * @param timeoutMs   Timeout in milliseconds
 * @return 0 on success, negative on failure
 */
int LiHolePunch(const char *serverAddr, unsigned short serverPort,
                const uint8_t *serverUuid, const uint8_t *clientUuid,
                int timeoutMs);

/**
 * Check if a received UDP packet is a hole punch packet.
 * Call from ENet intercept callback.
 *
 * @param data  Raw packet data
 * @param len   Packet length
 * @return 1 if it's a punch packet, 0 otherwise
 */
int isHolePunchPacket(const void *data, int len);
