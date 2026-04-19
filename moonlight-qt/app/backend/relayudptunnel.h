/**
 * VipleStream: UDP Tunnel through Relay (client side)
 *
 * Bridges moonlight-common-c's streaming UDP sockets through the
 * VipleStream relay when direct/STUN paths aren't available.
 *
 * Architecture: local UDP proxy.
 *   - For each logical server port P in {47998 (video), 48000 (audio),
 *     47999 (ENet control)} this tunnel binds a socket on
 *     127.0.0.1:P so that moonlight-common-c — which already has 47998
 *     etc. hard-wired — can target `127.0.0.1` as its "server address"
 *     and the datagrams land here.
 *   - Outbound datagrams from common-c are encapsulated with the
 *     24-byte VipleStream relay header (magic + flow_id + src/dst
 *     ports + HMAC-SHA256[:16]) and shipped over a single UDP socket
 *     to the relay's tunnel endpoint.
 *   - Inbound tunnel packets are decoded, routed to the proxy socket
 *     that matches their src_port (= server logical port), and sent
 *     back to the originating client ephemeral port on 127.0.0.1.
 *
 * Control plane (WS): used only to allocate the flow once at startup
 * (udp_tunnel_allocate → udp_tunnel_allocated). If the relay's UDP
 * endpoint isn't reachable, future work can fall back to the WS
 * binary-frame carrier using the same encoded packets.
 */
#pragma once

#include <QByteArray>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QThread>
#include <QVector>
#include <atomic>
#include <cstdint>

class RelayUdpTunnel : public QThread {
    Q_OBJECT

public:
    /**
     * @param relayUrl    "wss://relay.example.com" (or ws://host:port)
     * @param relayPsk    Pre-shared key (empty = no auth)
     * @param serverUuid  Target Sunshine server UUID
     * @param serverPorts Logical server ports to proxy. Typical value:
     *                    {47998, 48000, 47999} for video / audio /
     *                    control. Each will bind a local socket on
     *                    127.0.0.1:PORT.
     */
    RelayUdpTunnel(const QString &relayUrl,
                   const QString &relayPsk,
                   const QString &serverUuid,
                   const QVector<uint16_t> &serverPorts);
    ~RelayUdpTunnel() override;

    /**
     * Allocate the relay flow and bring up the local proxy sockets.
     * Returns true iff the tunnel is ready and common-c can use
     * 127.0.0.1 as its server address. On false, call stop() and
     * fall back to another carrier.
     *
     * Blocks up to `timeoutMs` waiting for the `udp_tunnel_allocated`
     * response from the relay.
     */
    bool startAndWaitReady(int timeoutMs = 5000);

    /** Stop the tunnel and tear down all sockets / the WS. */
    void stop();

    /** True iff the local proxy sockets are live. */
    bool isReady() const { return m_Ready.load(); }

    /**
     * After startAndWaitReady(), returns the mapping between each
     * server-side logical port (e.g. 47998) and the local ephemeral
     * port on 127.0.0.2 that moonlight-common-c should target. The
     * caller uses this to rewrite the RTSP Transport headers that
     * flow through the TCP tunnel so common-c targets our proxies.
     *
     * Returns {} if startAndWaitReady() hasn't succeeded yet.
     */
    QVector<QPair<uint16_t, uint16_t>> portMap() const {
        QMutexLocker lk(&m_PortMapMutex);
        return m_PortMap;
    }

private:
    void run() override;

    QString m_RelayUrl;
    QString m_RelayPsk;
    QString m_ServerUuid;
    QVector<uint16_t> m_ServerPorts;

    std::atomic<bool> m_Stop{false};
    std::atomic<bool> m_Ready{false};
    std::atomic<bool> m_AllocFailed{false};

    // Populated before m_Ready is set. Held by value once run() starts
    // the proxy loop, so external readers need a mutex only against the
    // race with the very-early readers during startup.
    mutable QMutex m_PortMapMutex;
    QVector<QPair<uint16_t, uint16_t>> m_PortMap;  // {server_port, local_port}
};
