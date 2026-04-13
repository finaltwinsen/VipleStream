/**
 * VipleStream: TCP Tunnel through Relay
 * Tunnels local TCP connections through the relay WebSocket to a remote server.
 *
 * IMPORTANT: moonlight-common-c's RTSP client opens a NEW TCP connection for each
 * request-response exchange (OPTIONS, DESCRIBE, SETUP×3, PLAY). The server closes
 * the TCP connection after sending the response (recv returns 0 = EOF).
 * Therefore this tunnel must accept MULTIPLE sequential TCP connections over the
 * same WebSocket relay channel.
 *
 * Protocol: sends "tcp_tunnel_data" messages with base64-encoded TCP data.
 * Per-request flow:
 *   1. RTSP client connects to our local port
 *   2. Client sends request → we forward as tcp_tunnel_data
 *   3. Sunshine relays back response as tcp_tunnel_data
 *   4. Sunshine sends tcp_tunnel_closed (RTSP server closed TCP)
 *   5. We close local client socket (EOF for RTSP client)
 *   6. Go back to step 1 for the next RTSP request
 */

#include "relaytcptunnel.h"
#include "relaylookup.h"

#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <SDL_log.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#endif

// Reuse WS frame helpers (client→server must be masked per RFC 6455)
static QByteArray tunWsFrame(const QByteArray &payload) {
    QByteArray frame;
    frame.append((char)0x81);
    int len = payload.size();
    if (len < 126) {
        frame.append((char)(0x80 | len));
    } else if (len < 65536) {
        frame.append((char)(0x80 | 126));
        frame.append((char)(len >> 8));
        frame.append((char)(len & 0xFF));
    } else {
        frame.append((char)(0x80 | 127));
        for (int i = 7; i >= 0; i--) frame.append((char)((len >> (i * 8)) & 0xFF));
    }
    quint32 maskVal = QRandomGenerator::global()->generate();
    QByteArray mask((const char *)&maskVal, 4);
    frame.append(mask);
    for (int i = 0; i < len; i++) {
        frame.append(payload[i] ^ mask[i % 4]);
    }
    return frame;
}

static QByteArray tunReadExact(QAbstractSocket *sock, int n, int timeoutMs) {
    QByteArray buf;
    while (buf.size() < n) {
        if (sock->bytesAvailable() == 0 && !sock->waitForReadyRead(timeoutMs)) break;
        buf.append(sock->read(n - buf.size()));
    }
    return buf;
}

static QByteArray tunWsRead(QAbstractSocket *sock, int timeoutMs) {
    QByteArray hdr = tunReadExact(sock, 2, timeoutMs);
    if (hdr.size() < 2) return QByteArray();
    int len = (quint8)hdr[1] & 0x7F;
    if (len == 126) {
        QByteArray ext = tunReadExact(sock, 2, timeoutMs);
        if (ext.size() < 2) return QByteArray();
        len = ((quint8)ext[0] << 8) | (quint8)ext[1];
    } else if (len == 127) {
        QByteArray ext = tunReadExact(sock, 8, timeoutMs);
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | (quint8)ext[i];
    }
    if (len == 0 || len > 1048576) return QByteArray();
    return tunReadExact(sock, len, timeoutMs);
}

RelayTcpTunnel::RelayTcpTunnel(const QString &relayUrl, const QString &relayPsk,
                                 const QString &serverUuid, uint16_t targetPort)
    : m_RelayUrl(relayUrl), m_RelayPsk(relayPsk),
      m_ServerUuid(serverUuid), m_TargetPort(targetPort) {}

RelayTcpTunnel::~RelayTcpTunnel() {
    stop();
    wait(10000);
}

uint16_t RelayTcpTunnel::startAndGetPort() {
    QThread::start();
    // Wait for the port to be assigned by the thread
    for (int i = 0; i < 300 && m_LocalPort == 0 && !m_Stop; i++) {
        QThread::msleep(10);
    }
    return m_LocalPort;
}

void RelayTcpTunnel::stop() {
    m_Stop = true;
}

void RelayTcpTunnel::run() {
    // Create TCP server INSIDE the thread (Qt objects must be used in their own thread)
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    m_LocalPort = server.serverPort();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-TUNNEL] Listening on localhost:%d -> server %s port %d",
                m_LocalPort.load(), qPrintable(m_ServerUuid.left(8)), m_TargetPort);

    // --- Connect to relay via WebSocket (persistent for entire tunnel lifetime) ---
    QUrl url(m_RelayUrl);
    bool useTls = (url.scheme() == "wss");
    QString host = url.host();
    int port = url.port(useTls ? 443 : 9999);
    if (host.isEmpty()) {
        QString raw = m_RelayUrl;
        if (raw.startsWith("wss://")) { raw = raw.mid(6); useTls = true; }
        else if (raw.startsWith("ws://")) { raw = raw.mid(5); }
        int ci = raw.lastIndexOf(':');
        if (ci > 0) { host = raw.left(ci); port = raw.mid(ci + 1).toInt(); }
        else host = raw;
    }

    QAbstractSocket *relaySock;
    if (useTls) {
        QSslSocket *ssl = new QSslSocket();
        ssl->setPeerVerifyMode(QSslSocket::VerifyNone);
        ssl->connectToHostEncrypted(host, port);
        if (!ssl->waitForEncrypted(10000)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-TUNNEL] TLS connect failed");
            delete ssl; return;
        }
        relaySock = ssl;
    } else {
        QTcpSocket *tcp = new QTcpSocket();
        tcp->connectToHost(host, port);
        if (!tcp->waitForConnected(10000)) {
            delete tcp; return;
        }
        relaySock = tcp;
    }

    // WS handshake
    QByteArray wsKey = QByteArray(16, 0);
    QRandomGenerator::global()->fillRange((quint32 *)wsKey.data(), 4);
    QString httpReq = QString("GET / HTTP/1.1\r\nHost: %1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %2\r\nSec-WebSocket-Version: 13\r\n\r\n")
        .arg(host).arg(QString(wsKey.toBase64()));
    relaySock->write(httpReq.toUtf8());
    relaySock->waitForBytesWritten(5000);

    QByteArray hsResp;
    while (hsResp.size() < 8192) {
        if (relaySock->bytesAvailable() == 0 && !relaySock->waitForReadyRead(5000)) break;
        QByteArray chunk = relaySock->peek(relaySock->bytesAvailable());
        int endIdx = chunk.indexOf("\r\n\r\n");
        if (endIdx >= 0) { hsResp.append(relaySock->read(endIdx + 4)); break; }
        else hsResp.append(relaySock->readAll());
    }
    if (!hsResp.contains("101")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-TUNNEL] WS handshake failed");
        delete relaySock; return;
    }

    // Register with relay
    QString tunnelUuid = "tunnel-" + m_ServerUuid.left(8) + "-" +
                         QString::number(QRandomGenerator::global()->generate(), 16).left(6);
    QString pskHash;
    if (!m_RelayPsk.isEmpty()) {
        QMessageAuthenticationCode mac(QCryptographicHash::Sha256);
        mac.setKey(m_RelayPsk.toUtf8());
        mac.addData(tunnelUuid.toUtf8());
        pskHash = mac.result().toHex().left(16);
    }
    QJsonObject regMsg;
    regMsg["type"] = "register";
    regMsg["uuid"] = tunnelUuid;
    regMsg["role"] = "client";
    regMsg["psk_hash"] = pskHash;
    relaySock->write(tunWsFrame(QJsonDocument(regMsg).toJson(QJsonDocument::Compact)));
    relaySock->waitForBytesWritten(5000);
    tunWsRead(relaySock, 5000); // consume register response

    // Request TCP tunnel to server (once, for the whole session)
    QJsonObject tunnelReq;
    tunnelReq["type"] = "tcp_tunnel_open";
    tunnelReq["target_uuid"] = m_ServerUuid;
    tunnelReq["target_port"] = m_TargetPort;
    tunnelReq["tunnel_id"] = tunnelUuid;
    relaySock->write(tunWsFrame(QJsonDocument(tunnelReq).toJson(QJsonDocument::Compact)));
    relaySock->waitForBytesWritten(5000);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-TUNNEL] TCP tunnel registered (uuid=%s), accepting connections",
                qPrintable(tunnelUuid));

    // ===================================================================
    // Main loop: accept multiple sequential RTSP connections
    // Each RTSP request is a new TCP connection (connect → send → recv → close)
    // ===================================================================
    int connectionCount = 0;
    while (!m_Stop) {
        // Wait for an incoming connection (short timeout so we can check m_Stop)
        if (!server.waitForNewConnection(1000)) {
            // Drain any WS messages while waiting (keep-alive, endpoint updates, etc.)
            QByteArray wsData = tunWsRead(relaySock, 50);
            if (!wsData.isEmpty()) {
                QJsonDocument doc = QJsonDocument::fromJson(wsData);
                QString msgType = doc.object()["type"].toString();
                if (msgType == "ping") {
                    // Respond to keep-alive
                    QJsonObject pong;
                    pong["type"] = "pong";
                    relaySock->write(tunWsFrame(QJsonDocument(pong).toJson(QJsonDocument::Compact)));
                    relaySock->waitForBytesWritten(1000);
                }
            }
            continue;
        }

        connectionCount++;
        QTcpSocket *localClient = server.nextPendingConnection();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-TUNNEL] Connection #%d from RTSP client", connectionCount);

        // Signal Sunshine to open a new TCP connection to RTSP server
        // (for connections after the first one)
        if (connectionCount > 1) {
            QJsonObject reopenReq;
            reopenReq["type"] = "tcp_tunnel_reconnect";
            reopenReq["target_uuid"] = m_ServerUuid;
            reopenReq["target_port"] = m_TargetPort;
            reopenReq["tunnel_id"] = tunnelUuid;
            relaySock->write(tunWsFrame(QJsonDocument(reopenReq).toJson(QJsonDocument::Compact)));
            relaySock->waitForBytesWritten(5000);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-TUNNEL] Sent reconnect request for connection #%d", connectionCount);
        }

        // Relay data for this connection until it closes
        // Use native socket descriptor for reliable I/O in non-event-loop thread
        qintptr localFd = localClient->socketDescriptor();
        bool tunnelClosed = false;
        while (!m_Stop && !tunnelClosed && localFd != -1 &&
               localClient->state() == QAbstractSocket::ConnectedState) {
            bool activity = false;

            // Local → Relay: poll local socket for incoming data (50ms)
            {
#ifdef _WIN32
                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET((SOCKET)localFd, &readSet);
                struct timeval tv = {0, 50000}; // 50ms
                int sel = ::select(0, &readSet, nullptr, nullptr, &tv);
#else
                struct pollfd pfd = { (int)localFd, POLLIN, 0 };
                int sel = ::poll(&pfd, 1, 50);
#endif
                if (sel > 0) {
                    char buf[65536];
#ifdef _WIN32
                    int n = ::recv((SOCKET)localFd, buf, sizeof(buf), 0);
#else
                    int n = ::recv((int)localFd, buf, sizeof(buf), 0);
#endif
                    if (n > 0) {
                        QJsonObject dataMsg;
                        dataMsg["type"] = "tcp_tunnel_data";
                        dataMsg["tunnel_id"] = tunnelUuid;
                        dataMsg["target_uuid"] = m_ServerUuid;
                        dataMsg["data"] = QString(QByteArray(buf, n).toBase64());
                        QByteArray frame = tunWsFrame(QJsonDocument(dataMsg).toJson(QJsonDocument::Compact));
                        relaySock->write(frame);
                        relaySock->waitForBytesWritten(5000);
                        activity = true;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-TUNNEL] #%d Local→Relay: %d bytes", connectionCount, n);
                    } else if (n == 0) {
                        // Client disconnected
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-TUNNEL] #%d Local client disconnected", connectionCount);
                        break;
                    }
                }
            }

            // Relay → Local: read WS frames, write to local client
            {
                QByteArray wsData = tunWsRead(relaySock, 100);
                if (!wsData.isEmpty()) {
                    QJsonDocument doc = QJsonDocument::fromJson(wsData);
                    QJsonObject obj = doc.object();
                    QString msgType = obj["type"].toString();
                    if (msgType == "tcp_tunnel_data") {
                        QByteArray decoded = QByteArray::fromBase64(obj["data"].toString().toUtf8());
                        // Write directly to socket fd for reliability
#ifdef _WIN32
                        ::send((SOCKET)localFd, decoded.constData(), decoded.size(), 0);
#else
                        ::send((int)localFd, decoded.constData(), decoded.size(), 0);
#endif
                        activity = true;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-TUNNEL] #%d Relay→Local: %d bytes", connectionCount, decoded.size());
                    } else if (msgType == "tcp_tunnel_closed") {
                        // Server-side RTSP closed TCP — this is NORMAL for per-request connections
                        // Close local socket so RTSP client sees EOF (recv returns 0)
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-TUNNEL] #%d Server closed TCP (normal per-request close)",
                                    connectionCount);
                        localClient->disconnectFromHost();
                        tunnelClosed = true;
                    } else if (msgType == "ping") {
                        QJsonObject pong;
                        pong["type"] = "pong";
                        relaySock->write(tunWsFrame(QJsonDocument(pong).toJson(QJsonDocument::Compact)));
                        relaySock->waitForBytesWritten(1000);
                    } else {
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-TUNNEL] #%d Got msg: %s (ignored)", connectionCount,
                                    qPrintable(msgType));
                    }
                }
            }

            if (!activity && !tunnelClosed) {
                QThread::msleep(5);
            }
        }

        // Wait for local client to fully close
        if (localClient->state() != QAbstractSocket::UnconnectedState) {
            localClient->waitForDisconnected(2000);
        }
        delete localClient;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-TUNNEL] Connection #%d completed", connectionCount);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-TUNNEL] Tunnel closed after %d connections", connectionCount);
    delete relaySock;
}
