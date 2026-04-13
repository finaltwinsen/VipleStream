/**
 * VipleStream: UDP Hole Punch Implementation
 *
 * Client sends SYN → Server replies ACK → Client sends CONFIRM.
 * After this exchange, both NATs have bidirectional UDP mappings
 * and ENet can connect through the punched hole.
 */

#include "Limelight-internal.h"
#include "HolePunch.h"

#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #define SOCKET int
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR (-1)
  #define closesocket close
#endif

#define PUNCH_SEND_INTERVAL_MS 200
#define PUNCH_MAX_ATTEMPTS 25  /* 25 * 200ms = 5 seconds */

int isHolePunchPacket(const void *data, int len) {
    if (len < (int)sizeof(HOLEPUNCH_PACKET)) return 0;
    const HOLEPUNCH_PACKET *pkt = (const HOLEPUNCH_PACKET *)data;
    return (pkt->magic == HOLEPUNCH_MAGIC && pkt->version == HOLEPUNCH_VERSION);
}

static uint32_t getTimestampMs(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

int LiHolePunch(const char *serverAddr, unsigned short serverPort,
                const uint8_t *serverUuid, const uint8_t *clientUuid,
                int timeoutMs) {
    SOCKET sock;
    struct addrinfo hints, *res = NULL;
    char portStr[8];
    int err = -1;
    HOLEPUNCH_PACKET pkt;
    char recvBuf[256];

    if (!serverAddr || serverPort == 0) {
        Limelog("[PUNCH] No server address\n");
        return -1;
    }

    Limelog("[PUNCH] Attempting hole punch to %s:%d\n", serverAddr, serverPort);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    snprintf(portStr, sizeof(portStr), "%u", serverPort);
    if (getaddrinfo(serverAddr, portStr, &hints, &res) != 0 || !res) {
        Limelog("[PUNCH] Failed to resolve %s\n", serverAddr);
        return -2;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        return -3;
    }

    /* Set receive timeout */
#ifdef _WIN32
    {
        DWORD tv = PUNCH_SEND_INTERVAL_MS;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    }
#else
    {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = PUNCH_SEND_INTERVAL_MS * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
#endif

    /* Build SYN packet */
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = HOLEPUNCH_MAGIC;
    pkt.version = HOLEPUNCH_VERSION;
    pkt.type = HOLEPUNCH_SYN;
    memcpy(pkt.serverUuid, serverUuid, 16);
    memcpy(pkt.clientUuid, clientUuid, 16);

    int maxAttempts = timeoutMs / PUNCH_SEND_INTERVAL_MS;
    if (maxAttempts < 5) maxAttempts = 5;
    if (maxAttempts > PUNCH_MAX_ATTEMPTS) maxAttempts = PUNCH_MAX_ATTEMPTS;

    for (int i = 0; i < maxAttempts; i++) {
        pkt.timestamp = getTimestampMs();

        /* Send SYN */
        sendto(sock, (const char *)&pkt, sizeof(pkt), 0,
               res->ai_addr, (int)res->ai_addrlen);

        /* Wait for ACK */
        int n = recvfrom(sock, recvBuf, sizeof(recvBuf), 0, NULL, NULL);
        if (n >= (int)sizeof(HOLEPUNCH_PACKET)) {
            HOLEPUNCH_PACKET *resp = (HOLEPUNCH_PACKET *)recvBuf;
            if (resp->magic == HOLEPUNCH_MAGIC &&
                resp->version == HOLEPUNCH_VERSION &&
                resp->type == HOLEPUNCH_ACK) {

                uint32_t rtt = getTimestampMs() - pkt.timestamp;
                Limelog("[PUNCH] Received ACK (RTT=%ums, attempt=%d)\n", rtt, i + 1);

                /* Send CONFIRM */
                pkt.type = HOLEPUNCH_CONFIRM;
                pkt.timestamp = getTimestampMs();
                sendto(sock, (const char *)&pkt, sizeof(pkt), 0,
                       res->ai_addr, (int)res->ai_addrlen);

                /* Send a few more CONFIRMs to be safe */
                for (int j = 0; j < 3; j++) {
#ifdef _WIN32
                    Sleep(50);
#else
                    usleep(50000);
#endif
                    sendto(sock, (const char *)&pkt, sizeof(pkt), 0,
                           res->ai_addr, (int)res->ai_addrlen);
                }

                Limelog("[PUNCH] Hole punch SUCCESS to %s:%d\n", serverAddr, serverPort);
                err = 0;
                goto Exit;
            }
        }
    }

    Limelog("[PUNCH] Hole punch FAILED (no ACK after %d attempts)\n", maxAttempts);
    err = -4;

Exit:
    closesocket(sock);
    freeaddrinfo(res);
    return err;
}
