/**
 * VipleStream: Relay Client
 * Synchronous WebSocket client for querying the signaling relay server.
 * Provides lookup (STUN endpoint discovery) and HTTP proxy (relay-forwarded GET).
 * Ported from the PC/Qt version (relaylookup.cpp).
 */
package com.limelight.relay;

import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

import org.json.JSONObject;

public class RelayClient {

    private static final String TAG = "RelayClient";

    public static class LookupResult {
        public boolean success;
        public String stunIp;
        public int stunPort;
        public String natType;
    }

    /**
     * Query the relay for a server's STUN endpoint.
     */
    public static LookupResult lookup(String relayUrl, String psk,
                                      String serverUuid, int timeoutMs) {
        LookupResult result = new LookupResult();
        if (relayUrl == null || relayUrl.isEmpty() ||
                serverUuid == null || serverUuid.isEmpty()) {
            return result;
        }

        Socket sock = null;
        try {
            // Parse URL
            boolean useTls;
            String host;
            int port;
            String raw = relayUrl;
            if (raw.startsWith("wss://")) {
                useTls = true;
                raw = raw.substring(6);
            } else if (raw.startsWith("ws://")) {
                useTls = false;
                raw = raw.substring(5);
            } else {
                useTls = false;
            }
            int colonIdx = raw.lastIndexOf(':');
            if (colonIdx > 0) {
                host = raw.substring(0, colonIdx);
                try {
                    port = Integer.parseInt(raw.substring(colonIdx + 1));
                } catch (NumberFormatException e) {
                    port = useTls ? 443 : 9999;
                }
            } else {
                host = raw;
                port = useTls ? 443 : 9999;
            }

            Log.i(TAG, "Looking up server " + serverUuid.substring(0, Math.min(8, serverUuid.length()))
                    + ".. via " + (useTls ? "wss" : "ws") + "://" + host + ":" + port);

            // Connect
            if (useTls) {
                SSLSocketFactory factory = createTrustAllFactory();
                sock = factory.createSocket();
                sock.setSoTimeout(timeoutMs);
                sock.connect(new InetSocketAddress(host, port), timeoutMs);
                ((SSLSocket) sock).startHandshake();
            } else {
                sock = new Socket();
                sock.setSoTimeout(timeoutMs);
                sock.connect(new InetSocketAddress(host, port), timeoutMs);
            }

            InputStream in = sock.getInputStream();
            OutputStream out = sock.getOutputStream();

            // WebSocket handshake
            byte[] wsKeyBytes = new byte[16];
            new SecureRandom().nextBytes(wsKeyBytes);
            String wsKey = android.util.Base64.encodeToString(wsKeyBytes, android.util.Base64.NO_WRAP);

            String httpReq = "GET / HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" +
                    "Upgrade: websocket\r\n" +
                    "Connection: Upgrade\r\n" +
                    "Sec-WebSocket-Key: " + wsKey + "\r\n" +
                    "Sec-WebSocket-Version: 13\r\n" +
                    "\r\n";
            out.write(httpReq.getBytes("UTF-8"));
            out.flush();

            // Read handshake response until \r\n\r\n
            String hsResp = readUntilHeaderEnd(in, timeoutMs);
            if (hsResp == null || !hsResp.contains("101")) {
                Log.w(TAG, "WS handshake failed");
                return result;
            }
            Log.i(TAG, "WS handshake OK");

            // Register with unique UUID
            String clientUuid = "client-" + serverUuid.substring(0, Math.min(8, serverUuid.length()))
                    + "-" + randomHex(6);

            String pskHash = computePskHash(psk, clientUuid);

            JSONObject regMsg = new JSONObject();
            regMsg.put("type", "register");
            regMsg.put("uuid", clientUuid);
            regMsg.put("role", "client");
            regMsg.put("psk_hash", pskHash);
            out.write(wsFrameMasked(regMsg.toString().getBytes("UTF-8")));
            out.flush();

            // Read register response
            String regResp = wsRead(in, timeoutMs);
            if (regResp == null || !regResp.contains("registered")) {
                Log.w(TAG, "Registration failed");
                return result;
            }
            Log.i(TAG, "Registered OK, sending lookup");

            // Send lookup
            JSONObject lookupMsg = new JSONObject();
            lookupMsg.put("type", "lookup");
            lookupMsg.put("target_uuid", serverUuid);
            out.write(wsFrameMasked(lookupMsg.toString().getBytes("UTF-8")));
            out.flush();

            // Read lookup response (skip non-lookup messages)
            JSONObject obj = null;
            for (int attempt = 0; attempt < 5; attempt++) {
                String resp = wsRead(in, timeoutMs);
                if (resp == null || resp.isEmpty()) {
                    Log.w(TAG, "Lookup timeout");
                    return result;
                }
                obj = new JSONObject(resp);
                String msgType = obj.optString("type", "");
                if ("lookup_result".equals(msgType)) {
                    break;
                }
                Log.i(TAG, "Skipping non-lookup message: " + msgType);
            }

            if (obj != null && "lookup_result".equals(obj.optString("type", ""))
                    && !obj.has("error")) {
                result.success = true;
                result.stunIp = obj.optString("stun_ip", "");
                result.stunPort = obj.optInt("stun_port", 0);
                result.natType = obj.optString("nat_type", "");

                if (!result.stunIp.isEmpty()) {
                    Log.i(TAG, "Found server: " + result.stunIp + ":" + result.stunPort
                            + " (NAT: " + result.natType + ")");
                } else {
                    Log.i(TAG, "Server online (no STUN endpoint, relay-only mode)");
                }
            } else {
                String error = obj != null ? obj.optString("error", "unknown") : "no response";
                Log.w(TAG, "Server not found: " + error);
            }

        } catch (Exception e) {
            Log.w(TAG, "Lookup failed: " + e.getMessage());
        } finally {
            closeQuietly(sock);
        }

        return result;
    }

    /**
     * Proxy an HTTP GET request to a server through the relay.
     */
    public static String httpProxy(String relayUrl, String psk,
                                   String serverUuid, String path, int timeoutMs) {
        if (relayUrl == null || relayUrl.isEmpty() ||
                serverUuid == null || serverUuid.isEmpty()) {
            return null;
        }

        Socket sock = null;
        try {
            // Parse URL
            boolean useTls;
            String host;
            int port;
            String raw = relayUrl;
            if (raw.startsWith("wss://")) {
                useTls = true;
                raw = raw.substring(6);
            } else if (raw.startsWith("ws://")) {
                useTls = false;
                raw = raw.substring(5);
            } else {
                useTls = false;
            }
            int colonIdx = raw.lastIndexOf(':');
            if (colonIdx > 0) {
                host = raw.substring(0, colonIdx);
                try {
                    port = Integer.parseInt(raw.substring(colonIdx + 1));
                } catch (NumberFormatException e) {
                    port = useTls ? 443 : 9999;
                }
            } else {
                host = raw;
                port = useTls ? 443 : 9999;
            }

            // Connect
            if (useTls) {
                SSLSocketFactory factory = createTrustAllFactory();
                sock = factory.createSocket();
                sock.setSoTimeout(timeoutMs);
                sock.connect(new InetSocketAddress(host, port), timeoutMs);
                ((SSLSocket) sock).startHandshake();
            } else {
                sock = new Socket();
                sock.setSoTimeout(timeoutMs);
                sock.connect(new InetSocketAddress(host, port), timeoutMs);
            }

            InputStream in = sock.getInputStream();
            OutputStream out = sock.getOutputStream();

            // WS handshake
            byte[] wsKeyBytes = new byte[16];
            new SecureRandom().nextBytes(wsKeyBytes);
            String wsKey = android.util.Base64.encodeToString(wsKeyBytes, android.util.Base64.NO_WRAP);

            String httpReq = "GET / HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" +
                    "Upgrade: websocket\r\n" +
                    "Connection: Upgrade\r\n" +
                    "Sec-WebSocket-Key: " + wsKey + "\r\n" +
                    "Sec-WebSocket-Version: 13\r\n" +
                    "\r\n";
            out.write(httpReq.getBytes("UTF-8"));
            out.flush();

            String hsResp = readUntilHeaderEnd(in, timeoutMs);
            if (hsResp == null || !hsResp.contains("101")) {
                return null;
            }

            // Register with unique UUID
            String clientUuid = "proxy-" + serverUuid.substring(0, Math.min(8, serverUuid.length()))
                    + "-" + randomHex(6);

            String pskHash = computePskHash(psk, clientUuid);

            JSONObject regMsg = new JSONObject();
            regMsg.put("type", "register");
            regMsg.put("uuid", clientUuid);
            regMsg.put("role", "client");
            regMsg.put("psk_hash", pskHash);
            out.write(wsFrameMasked(regMsg.toString().getBytes("UTF-8")));
            out.flush();

            String regResp = wsRead(in, timeoutMs);
            if (regResp == null || !regResp.contains("registered")) {
                return null;
            }

            // Send http_proxy request
            String reqId = randomHex(8);
            JSONObject proxyMsg = new JSONObject();
            proxyMsg.put("type", "http_proxy");
            proxyMsg.put("target_uuid", serverUuid);
            proxyMsg.put("request_id", reqId);
            proxyMsg.put("method", "GET");
            proxyMsg.put("path", path);
            out.write(wsFrameMasked(proxyMsg.toString().getBytes("UTF-8")));
            out.flush();

            Log.i(TAG, "Proxy sent: " + serverUuid.substring(0, Math.min(8, serverUuid.length()))
                    + " -> " + path);

            // Wait for http_proxy_response with matching request_id
            for (int attempt = 0; attempt < 10; attempt++) {
                String resp = wsRead(in, timeoutMs);
                if (resp == null || resp.isEmpty()) {
                    Log.w(TAG, "Proxy timeout waiting for response");
                    return null;
                }
                JSONObject respObj = new JSONObject(resp);
                if ("http_proxy_response".equals(respObj.optString("type", ""))
                        && reqId.equals(respObj.optString("request_id", ""))) {
                    int status = respObj.optInt("status", 0);
                    String body = respObj.optString("body", "");
                    Log.i(TAG, "Proxy response: " + status + " (" + body.length() + " bytes)");
                    return (status == 200) ? body : null;
                }
            }

        } catch (Exception e) {
            Log.w(TAG, "httpProxy failed: " + e.getMessage());
        } finally {
            closeQuietly(sock);
        }

        return null;
    }

    // -----------------------------------------------------------------------
    // WebSocket frame helpers
    // -----------------------------------------------------------------------

    /**
     * Build a masked WebSocket text frame (client->server MUST be masked per RFC 6455).
     */
    static byte[] wsFrameMasked(byte[] payload) {
        int len = payload.length;
        int headerLen;
        byte[] frame;

        if (len < 126) {
            headerLen = 2 + 4; // opcode+len + mask
            frame = new byte[headerLen + len];
            frame[0] = (byte) 0x81; // FIN + text
            frame[1] = (byte) (0x80 | len);
        } else if (len < 65536) {
            headerLen = 2 + 2 + 4; // opcode+126 + ext_len(2) + mask
            frame = new byte[headerLen + len];
            frame[0] = (byte) 0x81;
            frame[1] = (byte) (0x80 | 126);
            frame[2] = (byte) (len >> 8);
            frame[3] = (byte) (len & 0xFF);
        } else {
            headerLen = 2 + 8 + 4; // opcode+127 + ext_len(8) + mask
            frame = new byte[headerLen + len];
            frame[0] = (byte) 0x81;
            frame[1] = (byte) (0x80 | 127);
            for (int i = 7; i >= 0; i--) {
                frame[2 + (7 - i)] = (byte) ((len >> (i * 8)) & 0xFF);
            }
        }

        // Random mask key
        byte[] mask = new byte[4];
        new SecureRandom().nextBytes(mask);
        int maskOffset = headerLen - 4;
        System.arraycopy(mask, 0, frame, maskOffset, 4);

        // Masked payload
        for (int i = 0; i < len; i++) {
            frame[headerLen + i] = (byte) (payload[i] ^ mask[i % 4]);
        }

        return frame;
    }

    /**
     * Read one unmasked WebSocket text frame from the server.
     * Returns the payload as a String, or null on timeout/error.
     */
    static String wsRead(InputStream in, int timeoutMs) {
        try {
            // Read 2-byte header
            byte[] hdr = readExact(in, 2);
            if (hdr == null) return null;

            int len = hdr[1] & 0x7F;
            if (len == 126) {
                byte[] ext = readExact(in, 2);
                if (ext == null) return null;
                len = ((ext[0] & 0xFF) << 8) | (ext[1] & 0xFF);
            } else if (len == 127) {
                byte[] ext = readExact(in, 8);
                if (ext == null) return null;
                len = 0;
                for (int i = 0; i < 8; i++) {
                    len = (len << 8) | (ext[i] & 0xFF);
                }
            }

            if (len <= 0 || len > 1048576) return null;

            byte[] data = readExact(in, len);
            if (data == null) return null;

            return new String(data, "UTF-8");
        } catch (Exception e) {
            return null;
        }
    }

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    private static byte[] readExact(InputStream in, int n) throws IOException {
        byte[] buf = new byte[n];
        int off = 0;
        while (off < n) {
            int r = in.read(buf, off, n - off);
            if (r < 0) return null;
            off += r;
        }
        return buf;
    }

    /**
     * Read HTTP headers until \r\n\r\n is found.
     */
    private static String readUntilHeaderEnd(InputStream in, int timeoutMs) {
        try {
            StringBuilder sb = new StringBuilder();
            long deadline = System.currentTimeMillis() + timeoutMs;
            while (sb.length() < 8192) {
                if (System.currentTimeMillis() > deadline) return null;
                int b = in.read();
                if (b < 0) return null;
                sb.append((char) b);
                if (sb.length() >= 4) {
                    String tail = sb.substring(sb.length() - 4);
                    if ("\r\n\r\n".equals(tail)) {
                        return sb.toString();
                    }
                }
            }
        } catch (IOException e) {
            // fall through
        }
        return null;
    }

    /**
     * Compute HMAC-SHA256(psk, clientUuid) and return first 16 hex chars.
     * Returns empty string if psk is null or empty.
     */
    private static String computePskHash(String psk, String clientUuid) {
        if (psk == null || psk.isEmpty()) return "";
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            SecretKeySpec keySpec = new SecretKeySpec(psk.getBytes("UTF-8"), "HmacSHA256");
            mac.init(keySpec);
            byte[] hmacResult = mac.doFinal(clientUuid.getBytes("UTF-8"));
            return bytesToHex(hmacResult).substring(0, 16);
        } catch (Exception e) {
            Log.w(TAG, "HMAC failed: " + e.getMessage());
            return "";
        }
    }

    private static String bytesToHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(String.format("%02x", b & 0xFF));
        }
        return sb.toString();
    }

    private static String randomHex(int chars) {
        SecureRandom rng = new SecureRandom();
        byte[] bytes = new byte[(chars + 1) / 2];
        rng.nextBytes(bytes);
        return bytesToHex(bytes).substring(0, chars);
    }

    /**
     * Create an SSLSocketFactory that trusts all certificates (for self-signed relay servers).
     * Each call creates a new context -- not shared with the default.
     */
    private static SSLSocketFactory createTrustAllFactory() {
        try {
            TrustManager[] trustAll = new TrustManager[]{
                    new X509TrustManager() {
                        @Override
                        public void checkClientTrusted(X509Certificate[] chain, String authType) {}

                        @Override
                        public void checkServerTrusted(X509Certificate[] chain, String authType) {}

                        @Override
                        public X509Certificate[] getAcceptedIssuers() {
                            return new X509Certificate[0];
                        }
                    }
            };
            SSLContext ctx = SSLContext.getInstance("TLS");
            ctx.init(null, trustAll, new SecureRandom());
            return ctx.getSocketFactory();
        } catch (Exception e) {
            throw new RuntimeException("Failed to create TrustAll SSLContext", e);
        }
    }

    private static void closeQuietly(Socket sock) {
        if (sock != null) {
            try {
                sock.close();
            } catch (IOException ignored) {}
        }
    }
}
