/**
 * VipleStream: TCP Tunnel through Relay
 * Creates a local TCP listener that tunnels connections through
 * the relay WebSocket to a target server's TCP port.
 * Used for RTSP handshake when server is behind double NAT.
 *
 * IMPORTANT: moonlight-common-c's RTSP client opens a NEW TCP connection for each
 * request-response exchange (OPTIONS, DESCRIBE, SETUP x3, PLAY). The server closes
 * the TCP connection after sending the response (recv returns 0 = EOF).
 * Therefore this tunnel must accept MULTIPLE sequential TCP connections over the
 * same WebSocket relay channel.
 *
 * Ported from the PC/Qt version (relaytcptunnel.cpp).
 */
package com.limelight.relay;

import android.util.Base64;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
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

public class RelayTcpTunnel extends Thread {

    private static final String TAG = "RelayTunnel";

    private final String mRelayUrl;
    private final String mRelayPsk;
    private final String mServerUuid;
    private final int mTargetPort;

    private volatile int mLocalPort = 0;
    private volatile boolean mStop = false;
    private volatile ServerSocket mServerSocket;
    private volatile Socket mRelaySocket;

    public RelayTcpTunnel(String relayUrl, String relayPsk,
                          String serverUuid, int targetPort) {
        super("RelayTcpTunnel");
        setDaemon(true);
        mRelayUrl = relayUrl;
        mRelayPsk = relayPsk;
        mServerUuid = serverUuid;
        mTargetPort = targetPort;
    }

    /**
     * Start the tunnel thread and wait for it to begin listening.
     * @return the local port to connect to, or 0 on failure.
     */
    public int startAndGetPort() {
        start();
        // Wait up to 3 seconds for mLocalPort to be assigned
        for (int i = 0; i < 300 && mLocalPort == 0 && !mStop; i++) {
            try {
                Thread.sleep(10);
            } catch (InterruptedException e) {
                break;
            }
        }
        return mLocalPort;
    }

    /**
     * Stop the tunnel. Closes sockets to unblock threads.
     */
    public void stopTunnel() {
        mStop = true;
        closeQuietly(mServerSocket);
        closeQuietly(mRelaySocket);
    }

    @Override
    public void run() {
        ServerSocket serverSock = null;
        Socket relaySock = null;

        try {
            // Create local TCP server on loopback, random port
            serverSock = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
            mServerSocket = serverSock;
            mLocalPort = serverSock.getLocalPort();

            Log.i(TAG, "Listening on localhost:" + mLocalPort + " -> server "
                    + mServerUuid.substring(0, Math.min(8, mServerUuid.length()))
                    + " port " + mTargetPort);

            // --- Connect to relay via WebSocket (persistent for entire tunnel lifetime) ---
            boolean useTls;
            String host;
            int port;
            String raw = mRelayUrl;
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

            if (useTls) {
                SSLSocketFactory factory = createTrustAllFactory();
                relaySock = factory.createSocket();
                relaySock.setSoTimeout(10000);
                relaySock.connect(new java.net.InetSocketAddress(host, port), 10000);
                ((SSLSocket) relaySock).startHandshake();
            } else {
                relaySock = new Socket();
                relaySock.setSoTimeout(10000);
                relaySock.connect(new java.net.InetSocketAddress(host, port), 10000);
            }
            mRelaySocket = relaySock;

            InputStream relayIn = relaySock.getInputStream();
            OutputStream relayOut = relaySock.getOutputStream();

            // WS handshake
            byte[] wsKeyBytes = new byte[16];
            new SecureRandom().nextBytes(wsKeyBytes);
            String wsKey = Base64.encodeToString(wsKeyBytes, Base64.NO_WRAP);

            String httpReq = "GET / HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" +
                    "Upgrade: websocket\r\n" +
                    "Connection: Upgrade\r\n" +
                    "Sec-WebSocket-Key: " + wsKey + "\r\n" +
                    "Sec-WebSocket-Version: 13\r\n" +
                    "\r\n";
            relayOut.write(httpReq.getBytes("UTF-8"));
            relayOut.flush();

            String hsResp = readUntilHeaderEnd(relayIn, 10000);
            if (hsResp == null || !hsResp.contains("101")) {
                Log.w(TAG, "WS handshake failed");
                return;
            }

            // Register with relay
            String tunnelUuid = "tunnel-"
                    + mServerUuid.substring(0, Math.min(8, mServerUuid.length()))
                    + "-" + randomHex(6);

            String pskHash = computePskHash(mRelayPsk, tunnelUuid);

            JSONObject regMsg = new JSONObject();
            regMsg.put("type", "register");
            regMsg.put("uuid", tunnelUuid);
            regMsg.put("role", "client");
            regMsg.put("psk_hash", pskHash);
            relayOut.write(wsFrameMasked(regMsg.toString().getBytes("UTF-8")));
            relayOut.flush();

            // Consume register response
            relaySock.setSoTimeout(5000);
            wsRead(relayIn);

            // Request TCP tunnel to server (once, for the whole session)
            JSONObject tunnelReq = new JSONObject();
            tunnelReq.put("type", "tcp_tunnel_open");
            tunnelReq.put("target_uuid", mServerUuid);
            tunnelReq.put("target_port", mTargetPort);
            tunnelReq.put("tunnel_id", tunnelUuid);
            relayOut.write(wsFrameMasked(tunnelReq.toString().getBytes("UTF-8")));
            relayOut.flush();

            Log.i(TAG, "TCP tunnel registered (uuid=" + tunnelUuid + "), accepting connections");

            // ===================================================================
            // Main loop: accept multiple sequential RTSP connections
            // ===================================================================
            int connectionCount = 0;

            while (!mStop) {
                // Wait for an incoming connection (short timeout so we can check mStop)
                serverSock.setSoTimeout(1000);
                Socket localClient;
                try {
                    localClient = serverSock.accept();
                } catch (SocketTimeoutException e) {
                    // Drain any WS messages while waiting (keep-alive pings, etc.)
                    drainRelayMessages(relaySock, relayIn, relayOut);
                    continue;
                }

                connectionCount++;
                Log.i(TAG, "Connection #" + connectionCount + " from RTSP client");

                // Signal server to open a new TCP connection (for connections after the first)
                if (connectionCount > 1) {
                    JSONObject reconnReq = new JSONObject();
                    reconnReq.put("type", "tcp_tunnel_reconnect");
                    reconnReq.put("target_uuid", mServerUuid);
                    reconnReq.put("target_port", mTargetPort);
                    reconnReq.put("tunnel_id", tunnelUuid);
                    relayOut.write(wsFrameMasked(reconnReq.toString().getBytes("UTF-8")));
                    relayOut.flush();
                    Log.i(TAG, "Sent reconnect request for connection #" + connectionCount);
                }

                // Relay data for this connection until it closes
                InputStream localIn = localClient.getInputStream();
                OutputStream localOut = localClient.getOutputStream();
                boolean tunnelClosed = false;
                byte[] localBuf = new byte[65536];

                while (!mStop && !tunnelClosed && !localClient.isClosed()) {
                    boolean activity = false;

                    // Local -> Relay: read from local client (short timeout)
                    try {
                        localClient.setSoTimeout(50);
                        int n = localIn.read(localBuf);
                        if (n > 0) {
                            String encoded = Base64.encodeToString(localBuf, 0, n, Base64.NO_WRAP);
                            JSONObject dataMsg = new JSONObject();
                            dataMsg.put("type", "tcp_tunnel_data");
                            dataMsg.put("tunnel_id", tunnelUuid);
                            dataMsg.put("target_uuid", mServerUuid);
                            dataMsg.put("data", encoded);
                            relayOut.write(wsFrameMasked(dataMsg.toString().getBytes("UTF-8")));
                            relayOut.flush();
                            activity = true;
                            Log.i(TAG, "#" + connectionCount + " Local->Relay: " + n + " bytes");
                        } else if (n < 0) {
                            // Local client disconnected
                            Log.i(TAG, "#" + connectionCount + " Local client disconnected");
                            break;
                        }
                    } catch (SocketTimeoutException e) {
                        // No data from local client yet, that's fine
                    }

                    // Relay -> Local: read WS frames from relay
                    try {
                        relaySock.setSoTimeout(100);
                        String wsData = wsRead(relayIn);
                        if (wsData != null && !wsData.isEmpty()) {
                            JSONObject obj = new JSONObject(wsData);
                            String msgType = obj.optString("type", "");

                            if ("tcp_tunnel_data".equals(msgType)) {
                                byte[] decoded = Base64.decode(
                                        obj.optString("data", ""), Base64.NO_WRAP);
                                localOut.write(decoded);
                                localOut.flush();
                                activity = true;
                                Log.i(TAG, "#" + connectionCount + " Relay->Local: "
                                        + decoded.length + " bytes");

                            } else if ("tcp_tunnel_closed".equals(msgType)) {
                                // Server closed TCP -- normal for per-request RTSP
                                Log.i(TAG, "#" + connectionCount
                                        + " Server closed TCP (normal per-request close)");
                                tunnelClosed = true;

                            } else if ("ping".equals(msgType)) {
                                JSONObject pong = new JSONObject();
                                pong.put("type", "pong");
                                relayOut.write(wsFrameMasked(
                                        pong.toString().getBytes("UTF-8")));
                                relayOut.flush();
                            } else {
                                Log.i(TAG, "#" + connectionCount + " Got msg: "
                                        + msgType + " (ignored)");
                            }
                        }
                    } catch (SocketTimeoutException e) {
                        // No data from relay yet
                    }

                    if (!activity && !tunnelClosed) {
                        try {
                            Thread.sleep(5);
                        } catch (InterruptedException e) {
                            break;
                        }
                    }
                }

                // Close local client socket
                closeQuietly(localClient);
                Log.i(TAG, "Connection #" + connectionCount + " completed");
            }

            Log.i(TAG, "Tunnel closed after " + connectionCount + " connections");

        } catch (Exception e) {
            if (!mStop) {
                Log.w(TAG, "Tunnel error: " + e.getMessage());
            }
        } finally {
            closeQuietly(relaySock);
            closeQuietly(serverSock);
        }
    }

    // -----------------------------------------------------------------------
    // WebSocket frame helpers (duplicated from RelayClient for thread safety)
    // -----------------------------------------------------------------------

    private static byte[] wsFrameMasked(byte[] payload) {
        int len = payload.length;
        int headerLen;
        byte[] frame;

        if (len < 126) {
            headerLen = 2 + 4;
            frame = new byte[headerLen + len];
            frame[0] = (byte) 0x81;
            frame[1] = (byte) (0x80 | len);
        } else if (len < 65536) {
            headerLen = 2 + 2 + 4;
            frame = new byte[headerLen + len];
            frame[0] = (byte) 0x81;
            frame[1] = (byte) (0x80 | 126);
            frame[2] = (byte) (len >> 8);
            frame[3] = (byte) (len & 0xFF);
        } else {
            headerLen = 2 + 8 + 4;
            frame = new byte[headerLen + len];
            frame[0] = (byte) 0x81;
            frame[1] = (byte) (0x80 | 127);
            for (int i = 7; i >= 0; i--) {
                frame[2 + (7 - i)] = (byte) ((len >> (i * 8)) & 0xFF);
            }
        }

        byte[] mask = new byte[4];
        new SecureRandom().nextBytes(mask);
        int maskOffset = headerLen - 4;
        System.arraycopy(mask, 0, frame, maskOffset, 4);

        for (int i = 0; i < len; i++) {
            frame[headerLen + i] = (byte) (payload[i] ^ mask[i % 4]);
        }

        return frame;
    }

    private static String wsRead(InputStream in) {
        try {
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

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

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

    private static String computePskHash(String psk, String uuid) {
        if (psk == null || psk.isEmpty()) return "";
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            SecretKeySpec keySpec = new SecretKeySpec(psk.getBytes("UTF-8"), "HmacSHA256");
            mac.init(keySpec);
            byte[] hmacResult = mac.doFinal(uuid.getBytes("UTF-8"));
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
     * Drain any pending WS messages from the relay while waiting for accept().
     * Responds to pings to keep the connection alive.
     */
    private void drainRelayMessages(Socket relaySock, InputStream relayIn,
                                    OutputStream relayOut) {
        try {
            relaySock.setSoTimeout(50);
            String wsData = wsRead(relayIn);
            if (wsData != null && !wsData.isEmpty()) {
                JSONObject obj = new JSONObject(wsData);
                String msgType = obj.optString("type", "");
                if ("ping".equals(msgType)) {
                    JSONObject pong = new JSONObject();
                    pong.put("type", "pong");
                    relayOut.write(wsFrameMasked(pong.toString().getBytes("UTF-8")));
                    relayOut.flush();
                }
            }
        } catch (SocketTimeoutException e) {
            // Expected when no data available
        } catch (Exception e) {
            // Ignore drain errors
        }
    }

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
            try { sock.close(); } catch (IOException ignored) {}
        }
    }

    private static void closeQuietly(ServerSocket sock) {
        if (sock != null) {
            try { sock.close(); } catch (IOException ignored) {}
        }
    }
}
