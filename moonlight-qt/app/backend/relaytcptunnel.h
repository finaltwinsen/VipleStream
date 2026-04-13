/**
 * VipleStream: TCP Tunnel through Relay
 * Creates a local TCP listener that tunnels connections through
 * the relay WebSocket to a target server's TCP port.
 * Used for RTSP handshake when server is behind double NAT.
 */
#pragma once

#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSslSocket>
#include <QString>
#include <atomic>

class RelayTcpTunnel : public QThread {
    Q_OBJECT

public:
    /**
     * @param relayUrl    "wss://relay.example.com"
     * @param relayPsk    Pre-shared key
     * @param serverUuid  Target server UUID
     * @param targetPort  Server-side TCP port (e.g. 48010 for RTSP)
     */
    RelayTcpTunnel(const QString &relayUrl, const QString &relayPsk,
                   const QString &serverUuid, uint16_t targetPort);
    ~RelayTcpTunnel();

    /** Start listening. Returns the local port to connect to. */
    uint16_t startAndGetPort();

    /** Stop the tunnel. */
    void stop();

private:
    void run() override;

    QString m_RelayUrl, m_RelayPsk, m_ServerUuid;
    uint16_t m_TargetPort;
    std::atomic<uint16_t> m_LocalPort{0};
    std::atomic<bool> m_Stop{false};
};
