/**
 * VipleStream §REVIEW-1103：relay 的 wss 連線驗證。
 *
 * 原本 RelayClient／RelayTcpTunnel 用「全部信任」的 TrustManager、也不驗主機名稱：路徑上的主動中間人
 * 出示任意憑證就能拿到 /launch 的 rikey、解開整條串流，也能竄改 serverinfo／launch 回應。
 *
 * 現在的規則：
 *   1. 先走系統信任庫＋主機名稱驗證（Cloudflare 前置的 relay 是公開 CA 憑證，走這條）。
 *   2. 系統驗不過（自架 relay 的自簽憑證）時改用釘選：第一次連到某個 host:port 記下葉憑證的
 *      SHA-256，之後必須一模一樣，不同就拒絕連線（TOFU）。比全部信任安全：只有第一次連線時就被
 *      中間人攔才擋不住，之後換憑證一律擋下。
 *   3. 釘選需要儲存；還沒 init(Context) 時（理論上不會）自簽憑證一律拒絕，不退回全部信任。
 *
 * 換了自簽憑證的自架 relay 要清掉釘選：清除 app 資料，或刪掉 SharedPreferences「relay_tls_pins」。
 */
package com.limelight.relay;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;

import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLPeerUnverifiedException;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

public final class RelayTls {

    private static final String TAG = "RelayTls";
    private static final String PREFS_NAME = "relay_tls_pins";

    private static volatile SharedPreferences sPins;

    private RelayTls() {}

    /** 由 Activity／Service 在使用 relay 之前呼叫一次（重複呼叫無妨）。 */
    public static void init(Context context) {
        if (sPins == null && context != null) {
            sPins = context.getApplicationContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        }
    }

    /**
     * 建立並完成驗證的 TLS socket。驗證失敗丟 IOException（呼叫端原本就把例外當成連線失敗）。
     */
    public static SSLSocket connect(String host, int port, int timeoutMs) throws IOException {
        final PinningTrustManager tm;
        final SSLSocket sock;
        try {
            tm = new PinningTrustManager(host, port);
            SSLContext ctx = SSLContext.getInstance("TLS");
            ctx.init(null, new TrustManager[]{tm}, new SecureRandom());
            sock = (SSLSocket) ctx.getSocketFactory().createSocket();
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException("TLS setup failed: " + e.getMessage(), e);
        }

        boolean ok = false;
        try {
            sock.setSoTimeout(timeoutMs);
            sock.connect(new InetSocketAddress(host, port), timeoutMs);
            sock.startHandshake();

            // 系統信任鏈驗過的才驗主機名稱；釘選路徑比的是憑證本身，自簽憑證的 CN 常常跟連線用的名稱不同。
            if (tm.systemTrusted
                    && !HttpsURLConnection.getDefaultHostnameVerifier().verify(host, sock.getSession())) {
                throw new SSLPeerUnverifiedException("relay certificate does not match host " + host);
            }
            ok = true;
            return sock;
        } finally {
            if (!ok) {
                try { sock.close(); } catch (IOException ignored) {}
            }
        }
    }

    private static final class PinningTrustManager implements X509TrustManager {
        private final String pinKey;
        private final X509TrustManager system;
        volatile boolean systemTrusted = false;

        PinningTrustManager(String host, int port) throws Exception {
            pinKey = host.toLowerCase(java.util.Locale.ROOT) + ":" + port;
            TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            tmf.init((KeyStore) null);
            X509TrustManager found = null;
            for (TrustManager t : tmf.getTrustManagers()) {
                if (t instanceof X509TrustManager) {
                    found = (X509TrustManager) t;
                    break;
                }
            }
            if (found == null) {
                throw new IllegalStateException("no system X509TrustManager");
            }
            system = found;
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
            throw new CertificateException("client auth not supported");
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {
            try {
                system.checkServerTrusted(chain, authType);
                systemTrusted = true;
                return;
            } catch (CertificateException systemFailure) {
                if (chain == null || chain.length == 0) {
                    throw systemFailure;
                }
                SharedPreferences pins = sPins;
                if (pins == null) {
                    Log.w(TAG, "relay certificate not trusted by system and pin store unavailable; rejecting");
                    throw systemFailure;
                }
                String fp = sha256Hex(chain[0].getEncoded());
                synchronized (RelayTls.class) {
                    String pinned = pins.getString(pinKey, null);
                    if (pinned == null) {
                        pins.edit().putString(pinKey, fp).apply();
                        Log.w(TAG, "relay " + pinKey + " uses a non-public certificate; pinned on first use (sha256 "
                                + fp.substring(0, 16) + "..)");
                        return;
                    }
                    if (pinned.equals(fp)) {
                        return;
                    }
                }
                Log.w(TAG, "relay " + pinKey + " certificate changed (pinned " + "sha256 mismatch); rejecting");
                throw new CertificateException("relay certificate does not match pinned fingerprint");
            }
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() {
            return system.getAcceptedIssuers();
        }
    }

    private static String sha256Hex(byte[] data) throws CertificateException {
        try {
            byte[] d = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder sb = new StringBuilder(d.length * 2);
            for (byte b : d) {
                sb.append(String.format("%02x", b & 0xFF));
            }
            return sb.toString();
        } catch (Exception e) {
            throw new CertificateException("sha256 failed", e);
        }
    }
}
