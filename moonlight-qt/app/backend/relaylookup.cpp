/**
 * VipleStream: Relay Lookup — synchronous WebSocket client for querying
 * the signaling relay server for a server's STUN endpoint.
 */

#include "relaylookup.h"

#include <QTcpSocket>
#include <QSslSocket>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QRandomGenerator>
#include <SDL_log.h>

// Minimal WebSocket frame helpers
static QByteArray wsFrame(const QByteArray &payload) {
    QByteArray frame;
    frame.append((char)0x81); // FIN + text
    int len = payload.size();

    // Masked (client→server must be masked per RFC 6455)
    if (len < 126) {
        frame.append((char)(0x80 | len));
    } else {
        frame.append((char)(0x80 | 126));
        frame.append((char)(len >> 8));
        frame.append((char)(len & 0xFF));
    }

    // Random mask
    quint32 maskVal = QRandomGenerator::global()->generate();
    QByteArray mask((const char *)&maskVal, 4);
    frame.append(mask);

    // Masked payload
    for (int i = 0; i < len; i++) {
        frame.append(payload[i] ^ mask[i % 4]);
    }
    return frame;
}

// Read exactly N bytes from socket, blocking
static QByteArray readExact(QAbstractSocket *sock, int n, int timeoutMs) {
    QByteArray buf;
    while (buf.size() < n) {
        if (sock->bytesAvailable() == 0 && !sock->waitForReadyRead(timeoutMs))
            break;
        buf.append(sock->read(n - buf.size()));
    }
    return buf;
}

static QByteArray wsRead(QAbstractSocket *sock, int timeoutMs) {
    QByteArray hdr = readExact(sock, 2, timeoutMs);
    if (hdr.size() < 2) return QByteArray();

    int len = (quint8)hdr[1] & 0x7F;
    if (len == 126) {
        QByteArray ext = readExact(sock, 2, timeoutMs);
        if (ext.size() < 2) return QByteArray();
        len = ((quint8)ext[0] << 8) | (quint8)ext[1];
    }

    if (len == 0 || len > 65536) return QByteArray();

    QByteArray data = readExact(sock, len, timeoutMs);
    return data;
}

RelayLookup::Result RelayLookup::lookup(const QString &relayUrl, const QString &relayPsk,
                                         const QString &serverUuid, int timeoutMs) {
    Result result;

    if (relayUrl.isEmpty() || serverUuid.isEmpty()) return result;

    // Parse URL
    QUrl url(relayUrl);
    bool useTls = (url.scheme() == "wss");
    QString host = url.host();
    int port = url.port(useTls ? 443 : 9999);

    if (host.isEmpty()) {
        // Fallback: parse manually for formats like "wss://host:port" without scheme parsing
        QString raw = relayUrl;
        if (raw.startsWith("wss://")) { raw = raw.mid(6); useTls = true; }
        else if (raw.startsWith("ws://")) { raw = raw.mid(5); useTls = false; }

        int colonIdx = raw.lastIndexOf(':');
        if (colonIdx > 0) {
            host = raw.left(colonIdx);
            port = raw.mid(colonIdx + 1).toInt();
        } else {
            host = raw;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RELAY] Looking up server %s via %s://%s:%d",
                qPrintable(serverUuid.left(8) + ".."),
                useTls ? "wss" : "ws", qPrintable(host), port);

    // Connect
    QAbstractSocket *sock;
    if (useTls) {
        QSslSocket *sslSock = new QSslSocket();
        sslSock->setPeerVerifyMode(QSslSocket::VerifyNone); // Allow self-signed
        sslSock->connectToHostEncrypted(host, port);
        if (!sslSock->waitForEncrypted(timeoutMs)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RELAY] TLS connect failed: %s", qPrintable(sslSock->errorString()));
            delete sslSock;
            return result;
        }
        sock = sslSock;
    } else {
        QTcpSocket *tcpSock = new QTcpSocket();
        tcpSock->connectToHost(host, port);
        if (!tcpSock->waitForConnected(timeoutMs)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RELAY] TCP connect failed: %s", qPrintable(tcpSock->errorString()));
            delete tcpSock;
            return result;
        }
        sock = tcpSock;
    }

    // WebSocket handshake
    QByteArray wsKey = QByteArray(16, 0);
    QRandomGenerator::global()->fillRange((quint32 *)wsKey.data(), 4);
    QString keyB64 = wsKey.toBase64();

    QString httpReq = QString(
        "GET / HTTP/1.1\r\n"
        "Host: %1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %2\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    ).arg(host).arg(keyB64);

    sock->write(httpReq.toUtf8());
    sock->waitForBytesWritten(timeoutMs);

    if (!sock->waitForReadyRead(timeoutMs)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RELAY] WS handshake timeout");
        delete sock;
        return result;
    }

    // Read HTTP handshake response — accumulate until \r\n\r\n, don't over-read
    QByteArray handshakeResp;
    while (handshakeResp.size() < 8192) {
        if (sock->bytesAvailable() == 0) {
            if (!sock->waitForReadyRead(qMin(timeoutMs, 5000))) break;
        }
        // Read available data in chunks, but stop at \r\n\r\n boundary
        QByteArray chunk = sock->peek(sock->bytesAvailable());
        int endIdx = chunk.indexOf("\r\n\r\n");
        if (endIdx >= 0) {
            // Found end of headers — read exactly up to and including \r\n\r\n
            handshakeResp.append(sock->read(endIdx + 4));
            break;
        } else {
            // Haven't found end yet, consume all available
            handshakeResp.append(sock->readAll());
        }
    }
    if (!handshakeResp.contains("101")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RELAY] WS handshake failed (%d bytes): %s",
                    handshakeResp.size(), handshakeResp.left(120).constData());
        delete sock;
        return result;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RELAY] WS handshake OK (%d bytes)", handshakeResp.size());

    // Register UUID — unique per connection to avoid collision
    QString clientUuid = "client-" + serverUuid.left(8) + "-" +
                         QString::number(QRandomGenerator::global()->generate(), 16).left(6);

    // Compute PSK hash using the SAME uuid that goes in the register message
    QString pskHash;
    if (!relayPsk.isEmpty()) {
        QMessageAuthenticationCode mac(QCryptographicHash::Sha256);
        mac.setKey(relayPsk.toUtf8());
        mac.addData(clientUuid.toUtf8());
        pskHash = mac.result().toHex().left(16);
    }

    // Register as client
    QJsonObject regMsg;
    regMsg["type"] = "register";
    regMsg["uuid"] = clientUuid;
    regMsg["role"] = "client";
    regMsg["psk_hash"] = pskHash;
    sock->write(wsFrame(QJsonDocument(regMsg).toJson(QJsonDocument::Compact)));
    sock->waitForBytesWritten(timeoutMs);

    // Read register response
    QByteArray regResp = wsRead(sock, timeoutMs);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RELAY] Register response (%d bytes): %s",
                regResp.size(), regResp.left(200).constData());
    if (regResp.isEmpty() || !regResp.contains("registered")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RELAY] Registration failed (empty=%d)",
                    regResp.isEmpty());
        delete sock;
        return result;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RELAY] Registered OK, sending lookup");

    // Lookup server
    QJsonObject lookupMsg;
    lookupMsg["type"] = "lookup";
    lookupMsg["target_uuid"] = serverUuid;
    sock->write(wsFrame(QJsonDocument(lookupMsg).toJson(QJsonDocument::Compact)));
    sock->waitForBytesWritten(timeoutMs);

    // Read lookup response (skip non-lookup messages that may arrive in between)
    QJsonObject obj;
    for (int attempt = 0; attempt < 5; attempt++) {
        QByteArray lookupResp = wsRead(sock, timeoutMs);
        if (lookupResp.isEmpty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RELAY] Lookup timeout");
            delete sock;
            return result;
        }

        QJsonDocument doc = QJsonDocument::fromJson(lookupResp);
        obj = doc.object();
        QString msgType = obj["type"].toString();

        if (msgType == "lookup_result") {
            break; // Got what we want
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RELAY] Skipping non-lookup message: %s", qPrintable(msgType));
    }
    delete sock;

    if (obj["type"].toString() == "lookup_result" && !obj.contains("error")) {
        // Server is online (connected to relay). May or may not have a STUN endpoint.
        result.success = true;
        result.stunIp = obj["stun_ip"].toString();
        result.stunPort = (uint16_t)obj["stun_port"].toInt();
        result.natType = obj["nat_type"].toString();

        if (!result.stunIp.isEmpty()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RELAY] Found server: %s:%d (NAT: %s)",
                        qPrintable(result.stunIp), result.stunPort, qPrintable(result.natType));
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RELAY] Server online (no STUN endpoint, relay-only mode)");
        }
    } else {
        QString error = obj["error"].toString();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RELAY] Server not found: %s", qPrintable(error));
    }

    return result;
}

QString RelayLookup::httpProxy(const QString &relayUrl, const QString &relayPsk,
                                const QString &serverUuid, const QString &path,
                                int timeoutMs) {
    if (relayUrl.isEmpty() || serverUuid.isEmpty()) return QString();

    // Reuse the same connection setup as lookup()
    QUrl url(relayUrl);
    bool useTls = (url.scheme() == "wss");
    QString host = url.host();
    int port = url.port(useTls ? 443 : 9999);

    if (host.isEmpty()) {
        QString raw = relayUrl;
        if (raw.startsWith("wss://")) { raw = raw.mid(6); useTls = true; }
        else if (raw.startsWith("ws://")) { raw = raw.mid(5); useTls = false; }
        int colonIdx = raw.lastIndexOf(':');
        if (colonIdx > 0) { host = raw.left(colonIdx); port = raw.mid(colonIdx + 1).toInt(); }
        else host = raw;
    }

    // Connect
    QAbstractSocket *sock;
    if (useTls) {
        QSslSocket *sslSock = new QSslSocket();
        sslSock->setPeerVerifyMode(QSslSocket::VerifyNone);
        sslSock->connectToHostEncrypted(host, port);
        if (!sslSock->waitForEncrypted(timeoutMs)) { delete sslSock; return QString(); }
        sock = sslSock;
    } else {
        QTcpSocket *tcpSock = new QTcpSocket();
        tcpSock->connectToHost(host, port);
        if (!tcpSock->waitForConnected(timeoutMs)) { delete tcpSock; return QString(); }
        sock = tcpSock;
    }

    // WS handshake (byte-by-byte to avoid consuming frames)
    QByteArray wsKey = QByteArray(16, 0);
    QRandomGenerator::global()->fillRange((quint32 *)wsKey.data(), 4);
    QString httpReq = QString("GET / HTTP/1.1\r\nHost: %1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %2\r\nSec-WebSocket-Version: 13\r\n\r\n")
        .arg(host).arg(QString(wsKey.toBase64()));
    sock->write(httpReq.toUtf8());
    sock->waitForBytesWritten(timeoutMs);

    QByteArray hsResp;
    while (hsResp.size() < 8192) {
        if (sock->bytesAvailable() == 0 && !sock->waitForReadyRead(qMin(timeoutMs, 5000))) break;
        QByteArray chunk = sock->peek(sock->bytesAvailable());
        int endIdx = chunk.indexOf("\r\n\r\n");
        if (endIdx >= 0) { hsResp.append(sock->read(endIdx + 4)); break; }
        else hsResp.append(sock->readAll());
    }
    if (!hsResp.contains("101")) { delete sock; return QString(); }

    // Register with UNIQUE UUID (avoid collision with concurrent connections)
    QString clientUuid = "proxy-" + serverUuid.left(8) + "-" +
                         QString::number(QRandomGenerator::global()->generate(), 16).left(6);
    QString pskHash;
    if (!relayPsk.isEmpty()) {
        QMessageAuthenticationCode mac(QCryptographicHash::Sha256);
        mac.setKey(relayPsk.toUtf8());
        mac.addData(clientUuid.toUtf8());
        pskHash = mac.result().toHex().left(16);
    }

    QJsonObject regMsg;
    regMsg["type"] = "register";
    regMsg["uuid"] = clientUuid;
    regMsg["role"] = "client";
    regMsg["psk_hash"] = pskHash;
    sock->write(wsFrame(QJsonDocument(regMsg).toJson(QJsonDocument::Compact)));
    sock->waitForBytesWritten(timeoutMs);

    QByteArray regResp = wsRead(sock, timeoutMs);
    if (!regResp.contains("registered")) { delete sock; return QString(); }

    // Send http_proxy request
    QString reqId = QString::number(QRandomGenerator::global()->generate());
    QJsonObject proxyMsg;
    proxyMsg["type"] = "http_proxy";
    proxyMsg["target_uuid"] = serverUuid;
    proxyMsg["request_id"] = reqId;
    proxyMsg["method"] = "GET";
    proxyMsg["path"] = path;
    sock->write(wsFrame(QJsonDocument(proxyMsg).toJson(QJsonDocument::Compact)));
    sock->waitForBytesWritten(timeoutMs);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PROXY] Sent: %s -> %s",
                qPrintable(serverUuid.left(8)), qPrintable(path));

    // Wait for http_proxy_response
    QByteArray proxyResp = wsRead(sock, timeoutMs);
    delete sock;

    if (proxyResp.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PROXY] Timeout waiting for response");
        return QString();
    }

    QJsonDocument respDoc = QJsonDocument::fromJson(proxyResp);
    QJsonObject respObj = respDoc.object();

    if (respObj["type"].toString() == "http_proxy_response") {
        int status = respObj["status"].toInt();
        QString body = respObj["body"].toString();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PROXY] Response: %d (%d bytes)",
                    status, body.size());
        return (status == 200) ? body : QString();
    }

    return QString();
}
