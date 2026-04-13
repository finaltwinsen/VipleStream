/**
 * VipleStream: Relay Lookup Utility
 * Queries the signaling relay for a server's current STUN endpoint.
 * Uses a synchronous TCP+WebSocket connection (blocks for up to timeoutMs).
 */
#pragma once

#include <QString>
#include <QHostAddress>

class RelayLookup {
public:
    struct Result {
        bool success = false;
        QString stunIp;
        uint16_t stunPort = 0;
        QString natType;
    };

    /**
     * Query the relay for a server's endpoint.
     * @param relayUrl  "ws://host:port" or "wss://host:port"
     * @param relayPsk  Pre-shared key (empty = no auth)
     * @param serverUuid  Server UUID to look up
     * @param timeoutMs  Timeout in milliseconds
     * @return Result with stunIp/stunPort if found
     */
    static Result lookup(const QString &relayUrl, const QString &relayPsk,
                         const QString &serverUuid, int timeoutMs = 5000);

    /**
     * Proxy an HTTP GET request to a server through the relay.
     * @param relayUrl    "wss://host:port"
     * @param relayPsk    Pre-shared key
     * @param serverUuid  Target server UUID
     * @param path        HTTP path (e.g. "/serverinfo?uniqueid=xxx")
     * @param timeoutMs   Timeout
     * @return HTTP response body (empty on failure)
     */
    static QString httpProxy(const QString &relayUrl, const QString &relayPsk,
                             const QString &serverUuid, const QString &path,
                             int timeoutMs = 8000);
};
