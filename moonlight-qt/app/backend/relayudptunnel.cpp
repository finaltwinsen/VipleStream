/**
 * VipleStream: UDP Tunnel through Relay (client side)
 * See relayudptunnel.h for the design overview.
 */
#include "relayudptunnel.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QSslSocket>
#include <QTcpSocket>
#include <QUrl>

#include <SDL_log.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
typedef int SOCKET;
#endif

extern "C" {
#include "UdpTunnel.h"
}

/* --------------------------------------------------------------
 * WebSocket frame helpers (reuse the same masking pattern as
 * relaytcptunnel.cpp). Client→server frames MUST be masked per
 * RFC 6455 §5.3.
 * -------------------------------------------------------------- */

static QByteArray wsFrame(const QByteArray &payload) {
    QByteArray frame;
    frame.append((char)0x81);  // FIN + text
    int len = payload.size();
    if (len < 126) {
        frame.append((char)(0x80 | len));
    } else if (len < 65536) {
        frame.append((char)(0x80 | 126));
        frame.append((char)(len >> 8));
        frame.append((char)(len & 0xFF));
    } else {
        frame.append((char)(0x80 | 127));
        for (int i = 7; i >= 0; i--) {
            frame.append((char)((len >> (i * 8)) & 0xFF));
        }
    }
    quint32 maskVal = QRandomGenerator::global()->generate();
    QByteArray mask((const char *)&maskVal, 4);
    frame.append(mask);
    for (int i = 0; i < len; i++) {
        frame.append(payload[i] ^ mask[i % 4]);
    }
    return frame;
}

static QByteArray wsReadExact(QAbstractSocket *sock, int n, int timeoutMs) {
    QByteArray buf;
    while (buf.size() < n) {
        if (sock->bytesAvailable() == 0 && !sock->waitForReadyRead(timeoutMs)) break;
        buf.append(sock->read(n - buf.size()));
    }
    return buf;
}

static QByteArray wsReadFrame(QAbstractSocket *sock, int timeoutMs) {
    QByteArray hdr = wsReadExact(sock, 2, timeoutMs);
    if (hdr.size() < 2) return QByteArray();
    int len = (quint8)hdr[1] & 0x7F;
    if (len == 126) {
        QByteArray ext = wsReadExact(sock, 2, timeoutMs);
        if (ext.size() < 2) return QByteArray();
        len = ((quint8)ext[0] << 8) | (quint8)ext[1];
    } else if (len == 127) {
        QByteArray ext = wsReadExact(sock, 8, timeoutMs);
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | (quint8)ext[i];
    }
    if (len <= 0 || len > 1048576) return QByteArray();
    return wsReadExact(sock, len, timeoutMs);
}

/* --------------------------------------------------------------
 * Lifecycle glue
 * -------------------------------------------------------------- */

RelayUdpTunnel::RelayUdpTunnel(const QString &relayUrl,
                               const QString &relayPsk,
                               const QString &serverUuid,
                               const QVector<uint16_t> &serverPorts)
    : m_RelayUrl(relayUrl),
      m_RelayPsk(relayPsk),
      m_ServerUuid(serverUuid),
      m_ServerPorts(serverPorts) {}

RelayUdpTunnel::~RelayUdpTunnel() {
    stop();
    wait(10000);
}

void RelayUdpTunnel::stop() {
    m_Stop = true;
}

bool RelayUdpTunnel::startAndWaitReady(int timeoutMs) {
    QThread::start();
    int waited = 0;
    while (waited < timeoutMs && !m_Ready.load() && !m_AllocFailed.load() && !m_Stop.load()) {
        QThread::msleep(50);
        waited += 50;
    }
    return m_Ready.load();
}

/* --------------------------------------------------------------
 * Worker thread — owns the WS control channel, the tunnel UDP
 * socket, and all the local proxy sockets.
 *
 * This code intentionally uses raw Berkeley sockets instead of
 * QUdpSocket because the proxy loop needs blocking select/poll
 * semantics on three or more file descriptors in a single thread,
 * which QUdpSocket's signal/slot API doesn't express cleanly.
 * -------------------------------------------------------------- */

namespace {
struct ProxySock {
    SOCKET fd = INVALID_SOCKET;
    uint16_t serverPort = 0;      // e.g. 47998 / 48000 / 47999
    uint16_t localPort = 0;       // OS-assigned local ephemeral port
    uint16_t clientPort = 0;      // learned on first packet from common-c
};

// Resolve hostname to a single sockaddr_in (IPv4). Returns true on success.
bool resolveIpv4(const QString &host, uint16_t port, sockaddr_in &out) {
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);

    QByteArray hostBytes = host.toUtf8();
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(hostBytes.constData(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }
    out.sin_addr = ((sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}
}  // namespace

void RelayUdpTunnel::run() {
    /* ---------- 1. Connect to relay over WS ---------- */
    QUrl url(m_RelayUrl);
    bool useTls = (url.scheme() == "wss");
    QString host = url.host();
    int wsPort = url.port(useTls ? 443 : 9999);
    if (host.isEmpty()) {
        QString raw = m_RelayUrl;
        if (raw.startsWith("wss://")) { raw = raw.mid(6); useTls = true; }
        else if (raw.startsWith("ws://")) { raw = raw.mid(5); }
        int ci = raw.lastIndexOf(':');
        if (ci > 0) { host = raw.left(ci); wsPort = raw.mid(ci + 1).toInt(); }
        else host = raw;
    }

    QAbstractSocket *ws = nullptr;
    if (useTls) {
        auto ssl = new QSslSocket();
        ssl->setPeerVerifyMode(QSslSocket::VerifyNone);
        ssl->connectToHostEncrypted(host, wsPort);
        if (!ssl->waitForEncrypted(10000)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UDPTUN] TLS connect failed");
            delete ssl;
            m_AllocFailed = true;
            return;
        }
        ws = ssl;
    } else {
        auto tcp = new QTcpSocket();
        tcp->connectToHost(host, wsPort);
        if (!tcp->waitForConnected(10000)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UDPTUN] TCP connect failed");
            delete tcp;
            m_AllocFailed = true;
            return;
        }
        ws = tcp;
    }

    // WS HTTP upgrade
    QByteArray wsKey(16, 0);
    QRandomGenerator::global()->fillRange((quint32 *)wsKey.data(), 4);
    QString httpReq = QString(
        "GET / HTTP/1.1\r\nHost: %1\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %2\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n")
        .arg(host).arg(QString(wsKey.toBase64()));
    ws->write(httpReq.toUtf8());
    ws->waitForBytesWritten(5000);

    QByteArray hsResp;
    while (hsResp.size() < 8192) {
        if (ws->bytesAvailable() == 0 && !ws->waitForReadyRead(5000)) break;
        QByteArray chunk = ws->peek(ws->bytesAvailable());
        int endIdx = chunk.indexOf("\r\n\r\n");
        if (endIdx >= 0) { hsResp.append(ws->read(endIdx + 4)); break; }
        hsResp.append(ws->readAll());
    }
    if (!hsResp.contains("101")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-UDPTUN] WS handshake failed");
        delete ws; m_AllocFailed = true; return;
    }

    // Register as client
    QString clientUuid = "udptun-" + m_ServerUuid.left(8) + "-" +
                         QString::number(QRandomGenerator::global()->generate(), 16).left(6);
    QString pskHash;
    if (!m_RelayPsk.isEmpty()) {
        QMessageAuthenticationCode mac(QCryptographicHash::Sha256);
        mac.setKey(m_RelayPsk.toUtf8());
        mac.addData(clientUuid.toUtf8());
        pskHash = mac.result().toHex().left(16);
    }
    {
        QJsonObject reg;
        reg["type"] = "register";
        reg["uuid"] = clientUuid;
        reg["role"] = "client";
        reg["psk_hash"] = pskHash;
        ws->write(wsFrame(QJsonDocument(reg).toJson(QJsonDocument::Compact)));
        ws->waitForBytesWritten(5000);
        wsReadFrame(ws, 5000);  // consume registered
    }

    /* ---------- 2. Request a tunnel flow ---------- */
    QString requestId = QString::number(QRandomGenerator::global()->generate(), 16)
                        + QString::number(QRandomGenerator::global()->generate(), 16);
    {
        QJsonObject req;
        req["type"] = "udp_tunnel_allocate";
        req["request_id"] = requestId;
        req["target_uuid"] = m_ServerUuid;
        ws->write(wsFrame(QJsonDocument(req).toJson(QJsonDocument::Compact)));
        ws->waitForBytesWritten(5000);
    }

    VpTunnelFlow flow = {};
    QString relayUdpHost;
    uint16_t relayUdpPort = 0;
    {
        QByteArray reply = wsReadFrame(ws, 7000);
        if (reply.isEmpty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UDPTUN] allocate timeout");
            delete ws; m_AllocFailed = true; return;
        }
        QJsonObject o = QJsonDocument::fromJson(reply).object();
        if (o["type"].toString() != "udp_tunnel_allocated" ||
            o["request_id"].toString() != requestId) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UDPTUN] unexpected reply: %s",
                        qPrintable(o["type"].toString()));
            delete ws; m_AllocFailed = true; return;
        }
        int fid = o["flow_id"].toInt(0);
        QByteArray tokenHex = o["token"].toString().toLatin1();
        if (fid <= 0 || fid > 0xFFFF ||
            !VpTunnel_hex_to_token(tokenHex.constData(), flow.token)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UDPTUN] malformed allocated payload");
            delete ws; m_AllocFailed = true; return;
        }
        flow.flow_id = (uint16_t)fid;
        relayUdpHost = o["relay_udp_host"].toString();
        relayUdpPort = (uint16_t)o["relay_udp_port"].toInt(0);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-UDPTUN] flow_id=%u udp=%s:%u",
                    flow.flow_id, qPrintable(relayUdpHost), relayUdpPort);
    }

    /* ---------- 3. Pick carrier: UDP if the relay has an advertised
     *                 UDP host we can actually reach, otherwise fall
     *                 back to WS binary frames over the same WSS
     *                 connection we just registered on. The WS carrier
     *                 is slower but works in environments where direct
     *                 UDP to the relay is blocked (e.g. Cloudflare WARP
     *                 or an ISP that drops UDP to public addresses).
     * -------------------------------------------------------------- */
    sockaddr_in relayAddr = {};
    SOCKET tunnelSock = INVALID_SOCKET;
    bool useUdpCarrier = false;
    if (!relayUdpHost.isEmpty() && relayUdpPort > 0 &&
        resolveIpv4(relayUdpHost, relayUdpPort, relayAddr)) {
        tunnelSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (tunnelSock != INVALID_SOCKET) {
            sockaddr_in anyAddr = {};
            anyAddr.sin_family = AF_INET;
            anyAddr.sin_addr.s_addr = htonl(INADDR_ANY);
            bind(tunnelSock, (sockaddr *)&anyAddr, sizeof(anyAddr));
            useUdpCarrier = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UDPTUN] carrier=UDP_RELAY %s:%u",
                        qPrintable(relayUdpHost), relayUdpPort);
        }
    }
    if (!useUdpCarrier) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-UDPTUN] carrier=WS_RELAY (udp_host='%s' unreachable)",
                    qPrintable(relayUdpHost));
    }

    /* ---------- 4. Bind local proxy sockets on 127.0.0.1 ----------
     *
     * We let the OS assign the local port (bind port 0) rather than
     * using the Sunshine logical ports like 47998 verbatim. Windows
     * hosts with Hyper-V / WSL2 running often have stealth UDP port
     * reservations covering the 47xxx-48xxx range (a known
     * Hyper-V / winnat leak), so a deterministic bind fails with
     * WSAEADDRINUSE even though netstat shows the port as free.
     *
     * The ephemeral local port is then published via portMap() so
     * session.cpp can rewrite the RTSP SETUP responses that pass
     * through the TCP tunnel (see RelayTcpTunnel's data rewriter) and
     * moonlight-common-c targets 127.0.0.1:<ephemeral> instead of
     * 127.0.0.1:<server_port>.
     * -------------------------------------------------------------- */
    QVector<ProxySock> proxies;
    proxies.reserve(m_ServerPorts.size());
    for (uint16_t p : m_ServerPorts) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = 0;  // OS-assigned
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (bind(s, (sockaddr *)&a, sizeof(a)) < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
#else
            int err = errno;
#endif
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UDPTUN] bind 127.0.0.1:0 (for server port %u) "
                        "failed (err=%d)", p, err);
            closesocket(s);
            continue;
        }
        // Read back the OS-chosen port.
        sockaddr_in got = {};
        socklen_t gotLen = sizeof(got);
        uint16_t localPort = 0;
        if (getsockname(s, (sockaddr *)&got, &gotLen) == 0) {
            localPort = ntohs(got.sin_port);
        }
        ProxySock ps;
        ps.fd = s;
        ps.serverPort = p;
        ps.localPort = localPort;
        proxies.append(ps);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-UDPTUN] proxy bound 127.0.0.1:%u -> server port %u",
                    localPort, p);
    }
    if (proxies.isEmpty()) {
        if (tunnelSock != INVALID_SOCKET) closesocket(tunnelSock);
        delete ws; m_AllocFailed = true; return;
    }
    // Publish the port map so RelayTcpTunnel can rewrite RTSP replies.
    {
        QMutexLocker lk(&m_PortMapMutex);
        m_PortMap.clear();
        for (const auto &ps : proxies) {
            m_PortMap.append({ps.serverPort, ps.localPort});
        }
    }

    m_Ready.store(true);

    /* ---------- 5. Proxy loop ----------
     *
     * Two carrier modes:
     *   - UDP: one select() over proxy sockets + the tunnel UDP socket;
     *     proxy → sendto(tunnel), tunnel → parse → sendto(proxy).
     *   - WS:  select() only over proxy sockets; proxy → encode →
     *     ws_send_binary. A dedicated reader thread drains inbound WS
     *     binary frames (the shared WS socket is already open and
     *     registered on the relay; the relay routes WS binary frames
     *     between peers when we're on flow N).
     *
     * The proxies vector is captured by the WS reader thread by
     * reference because clientPort is learned lazily on the main
     * thread; the mutex `proxy_mtx` guards those reads.
     * -------------------------------------------------------------- */
    uint8_t rxbuf[2048];
    uint8_t txbuf[2048];

    int kDeliverCount = 0;
    auto deliver_to_proxy = [&](uint16_t srcPort,
                                const uint8_t *payload, size_t payloadLen) {
        for (const auto &ps : proxies) {
            if (ps.serverPort != srcPort) continue;
            if (ps.clientPort == 0) {
                if (kDeliverCount < 5) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-UDPTUN] drop inbound srcPort=%u len=%zu "
                                "(no clientPort learned for that stream yet)",
                                srcPort, payloadLen);
                }
                kDeliverCount++;
                return;
            }
            sockaddr_in dst = {};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(ps.clientPort);
            inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
            int sent = sendto(ps.fd, (const char *)payload, (int)payloadLen, 0,
                              (sockaddr *)&dst, sizeof(dst));
            if (kDeliverCount++ < 5) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-UDPTUN] deliver #%d srv=%u -> 127.0.0.1:%u len=%zu sent=%d",
                            kDeliverCount, srcPort, ps.clientPort, payloadLen, sent);
            }
            return;
        }
        if (kDeliverCount < 5) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UDPTUN] drop inbound srcPort=%u len=%zu "
                        "(no proxy for that server port)",
                        srcPort, payloadLen);
        }
        kDeliverCount++;
    };

    // One helper for the send side — picks the active carrier.
    auto send_carrier = [&](const uint8_t *data, size_t len) {
        if (useUdpCarrier) {
            sendto(tunnelSock, (const char *)data, (int)len, 0,
                   (sockaddr *)&relayAddr, sizeof(relayAddr));
            return;
        }
        // WS binary frame (opcode 0x2), masked per RFC 6455 §5.3.
        QByteArray frame;
        frame.append((char)0x82);   // FIN + binary
        if (len < 126) {
            frame.append((char)(0x80 | len));
        } else if (len < 65536) {
            frame.append((char)(0x80 | 126));
            frame.append((char)(len >> 8));
            frame.append((char)(len & 0xFF));
        } else {
            frame.append((char)(0x80 | 127));
            for (int i = 7; i >= 0; i--) {
                frame.append((char)((len >> (i * 8)) & 0xFF));
            }
        }
        quint32 maskVal = QRandomGenerator::global()->generate();
        QByteArray mask((const char *)&maskVal, 4);
        frame.append(mask);
        for (size_t i = 0; i < len; i++) {
            frame.append((char)(data[i] ^ (uint8_t)mask[i % 4]));
        }
        ws->write(frame);
        ws->waitForBytesWritten(500);
    };

    // Single-threaded loop. We can't use a separate reader thread on
    // the WS socket because QAbstractSocket is thread-affined — the
    // write side on this thread would race with reads on another.
    // Instead, each iteration: poll proxies with select (short timeout)
    // and additionally drain any pending WS frames (only in WS mode).
    while (!m_Stop.load()) {
        fd_set rset;
        FD_ZERO(&rset);
        SOCKET nfds = 0;
        if (useUdpCarrier) {
            FD_SET(tunnelSock, &rset);
            if (tunnelSock > nfds) nfds = tunnelSock;
        }
        for (const auto &ps : proxies) {
            FD_SET(ps.fd, &rset);
            if (ps.fd > nfds) nfds = ps.fd;
        }
        // Short timeout so WS polling stays responsive. 20 ms is a
        // good balance: low latency for video/audio RTP without
        // burning CPU when idle.
        struct timeval tv = {0, 20000};
        int sel = select((int)nfds + 1, &rset, nullptr, nullptr, &tv);
        if (sel < 0) break;

        if (sel > 0) {
            // ----- Outbound: common-c → tunnel -----
            for (auto &ps : proxies) {
                if (!FD_ISSET(ps.fd, &rset)) continue;
                sockaddr_in from = {};
                socklen_t fromLen = sizeof(from);
                int n = recvfrom(ps.fd, (char *)rxbuf, sizeof(rxbuf), 0,
                                 (sockaddr *)&from, &fromLen);
                if (n <= 0) continue;
                uint16_t clientPort = ntohs(from.sin_port);
                if (ps.clientPort == 0) {
                    ps.clientPort = clientPort;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-UDPTUN] learned client port %u for server port %u",
                                clientPort, ps.serverPort);
                }
                size_t enc = VpTunnel_encode(&flow, clientPort, ps.serverPort,
                                             rxbuf, (size_t)n, txbuf, sizeof(txbuf));
                if (enc > 0) send_carrier(txbuf, enc);
            }

            // ----- Inbound (UDP carrier only): tunnel → common-c -----
            if (useUdpCarrier && FD_ISSET(tunnelSock, &rset)) {
                sockaddr_in from = {};
                socklen_t fromLen = sizeof(from);
                int n = recvfrom(tunnelSock, (char *)rxbuf, sizeof(rxbuf), 0,
                                 (sockaddr *)&from, &fromLen);
                if (n > 0 &&
                    from.sin_addr.s_addr == relayAddr.sin_addr.s_addr &&
                    from.sin_port == relayAddr.sin_port) {
                    uint16_t srcPort = 0, dstPort = 0;
                    const uint8_t *payload = nullptr;
                    size_t payloadLen = 0;
                    if (VpTunnel_parse(&flow, rxbuf, (size_t)n,
                                       &srcPort, &dstPort,
                                       &payload, &payloadLen)) {
                        deliver_to_proxy(srcPort, payload, payloadLen);
                    }
                }
            }
        }

        // ----- Inbound (WS carrier): drain any WS binary frames -----
        // bytesAvailable alone isn't enough (the TCP/TLS layer may
        // have received bytes without emitting readyRead yet), so we
        // let Qt drain what it has by polling with a 0-ms wait.
        if (!useUdpCarrier) {
            if (ws->bytesAvailable() == 0) {
                ws->waitForReadyRead(0);
            }
            while (ws->bytesAvailable() > 0 && !m_Stop.load()) {
                QByteArray frame = wsReadFrame(ws, 200);
                if (frame.isEmpty()) break;
                uint16_t srcPort = 0, dstPort = 0;
                const uint8_t *payload = nullptr;
                size_t payloadLen = 0;
                if (!VpTunnel_parse(&flow,
                                    (const uint8_t *)frame.constData(),
                                    (size_t)frame.size(),
                                    &srcPort, &dstPort,
                                    &payload, &payloadLen)) {
                    // Might be a stray text control frame (e.g.
                    // udp_tunnel_closed) — silently ignore.
                    continue;
                }
                deliver_to_proxy(srcPort, payload, payloadLen);
            }
        }
    }

    /* ---------- 6. Teardown ---------- */
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-UDPTUN] closing (flow_id=%u)", flow.flow_id);

    // Best-effort close_flow on the WS (fire and forget)
    {
        QJsonObject msg;
        msg["type"] = "udp_tunnel_close";
        msg["flow_id"] = (int)flow.flow_id;
        ws->write(wsFrame(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        ws->waitForBytesWritten(1000);
    }

    for (auto &ps : proxies) {
        if (ps.fd != INVALID_SOCKET) closesocket(ps.fd);
    }
    if (tunnelSock != INVALID_SOCKET) closesocket(tunnelSock);
    delete ws;
    m_Ready.store(false);
}
