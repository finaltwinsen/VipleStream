package com.limelight.nvstream;

import android.app.ActivityManager;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.IpPrefix;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.RouteInfo;
import android.os.Build;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.security.cert.X509Certificate;
import java.util.Timer;
import java.util.TimerTask;
import java.util.concurrent.Semaphore;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

import org.xmlpull.v1.XmlPullParserException;

import com.limelight.LimeLog;
import com.limelight.nvstream.av.audio.AudioRenderer;
import com.limelight.nvstream.av.video.VideoDecoderRenderer;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.HostHttpResponseException;
import com.limelight.nvstream.http.LimelightCryptoProvider;
import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.http.NvHTTP;
import com.limelight.nvstream.http.PairingManager;
import com.limelight.nvstream.input.MouseButtonPacket;
import com.limelight.nvstream.jni.MoonBridge;
import com.limelight.relay.RelayClient;
import com.limelight.relay.RelayTcpTunnel;

public class NvConnection {
    // Context parameters
    private LimelightCryptoProvider cryptoProvider;
    private String uniqueId;
    private ConnectionContext context;
    private static Semaphore connectionAllowed = new Semaphore(1);
    // §REVIEW-1104：這個連線是否真的拿到了 connectionAllowed，以及是否已被要求停止。
    // 原本 stop() 一律 release()：連線還在 HTTP 階段（還沒 acquire）就被停時會多還一次，計數變 2，
    // start 執行緒之後照樣把連線建起來（沒人會再停它）。
    private final AtomicBoolean holdsConnectionPermit = new AtomicBoolean(false);
    private volatile boolean stopRequested = false;
    private final boolean isMonkey;
    private final Context appContext;
    private RelayTcpTunnel relayTunnel;  // VipleStream: RTSP tunnel for relay
    // §REVIEW-1104：記錄本次 serverinfo 或 launch/resume 是否走過 relay，stop() 清掉 relayTunnel 後旗標仍保留
    private volatile boolean usedRelay = false;

    public NvConnection(Context appContext, ComputerDetails.AddressTuple host, int httpsPort, String uniqueId, StreamConfiguration config, LimelightCryptoProvider cryptoProvider, X509Certificate serverCert)
    {
        this.appContext = appContext;
        this.cryptoProvider = cryptoProvider;
        this.uniqueId = uniqueId;

        this.context = new ConnectionContext();
        this.context.serverAddress = host;
        this.context.httpsPort = httpsPort;
        this.context.streamConfig = config;
        this.context.serverCert = serverCert;

        // This is unique per connection
        this.context.riKey = generateRiAesKey();
        this.context.riKeyId = generateRiKeyId();

        this.isMonkey = ActivityManager.isUserAMonkey();
    }

    // VipleStream: Set relay configuration for NAT traversal
    public void setRelayConfig(String relayUrl, String relayPsk, String serverUuid, String stunAddress, int stunPunchPort) {
        context.relayUrl = relayUrl;
        context.relayPsk = relayPsk;
        context.serverUuid = serverUuid;
        context.stunAddress = stunAddress;
        context.stunPunchPort = stunPunchPort;  // §REVIEW-1104
    }

    // §REVIEW-1104：公開本次 serverinfo 或 launch/resume 是否走過 relay（stop() 清掉 relayTunnel 後旗標仍保留）
    public boolean isUsingRelay() {
        return usedRelay;
    }

    private static SecretKey generateRiAesKey() {
        try {
            KeyGenerator keyGen = KeyGenerator.getInstance("AES");

            // RI keys are 128 bits
            keyGen.init(128);

            return keyGen.generateKey();
        } catch (NoSuchAlgorithmException e) {
            e.printStackTrace();
            throw new RuntimeException(e);
        }
    }
    
    private static int generateRiKeyId() {
        return new SecureRandom().nextInt();
    }

    // §REVIEW-1104（使用者決策：開多路徑＝要更流暢）：把目前有網際網路的 Wi-Fi／行動網路／乙太網路介面交給原生層。
    // 原本只在連線開始時列一次；現在網路有變動（Wi-Fi 斷、行動網路連上）時也會重交，QUIC 的路徑復原執行緒才看得到新介面
    private void injectNetInterfaces(ConnectivityManager cm) {
        try {
            injectNetInterfacesUnchecked(cm);
        } catch (Exception e) {
            // 網路回呼執行緒上不可以丟例外出去（網路正在變動時查屬性可能失敗）
            LimeLog.warning("[VIPLE-MPQUIC] Failed to enumerate interfaces: " + e.getMessage());
        }
    }

    private void injectNetInterfacesUnchecked(ConnectivityManager cm) {
        Network[] networks = cm.getAllNetworks();
        if (networks != null) {
            java.util.ArrayList<String> nameList = new java.util.ArrayList<>();
            java.util.ArrayList<Integer> typeList = new java.util.ArrayList<>();
            java.util.ArrayList<Integer> familyList = new java.util.ArrayList<>();
            java.util.ArrayList<byte[]> addrList = new java.util.ArrayList<>();

            for (Network net : networks) {
                NetworkCapabilities caps = cm.getNetworkCapabilities(net);
                LinkProperties props = cm.getLinkProperties(net);
                if (caps == null || props == null) continue;
                if (!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET))
                    continue;

                int transport = -1;
                if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI))
                    transport = NetworkCapabilities.TRANSPORT_WIFI;
                else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR))
                    transport = NetworkCapabilities.TRANSPORT_CELLULAR;
                else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET))
                    transport = NetworkCapabilities.TRANSPORT_ETHERNET;
                if (transport < 0) continue;

                String ifName = props.getInterfaceName();
                if (ifName == null) ifName = "unknown";

                for (android.net.LinkAddress la : props.getLinkAddresses()) {
                    InetAddress a = la.getAddress();
                    if (a.isLoopbackAddress()) continue;
                    byte[] raw = a.getAddress();
                    int family = (raw.length == 4) ? 2 : 10; // AF_INET=2, AF_INET6=10
                    nameList.add(ifName);
                    typeList.add(transport);
                    familyList.add(family);
                    addrList.add(raw);
                    LimeLog.info("[VIPLE-MPQUIC] Interface: " + ifName
                            + " type=" + transport + " addr=" + a.getHostAddress());
                }
            }

            if (!nameList.isEmpty()) {
                String[] names = nameList.toArray(new String[0]);
                int[] types = new int[typeList.size()];
                int[] families = new int[familyList.size()];
                for (int i = 0; i < typeList.size(); i++) {
                    types[i] = typeList.get(i);
                    families[i] = familyList.get(i);
                }
                byte[][] addrs = addrList.toArray(new byte[0][]);
                MoonBridge.setNetInterfaces(names, types, families, addrs);
            }
        }
    }

    // §REVIEW-1104：串流期間讓行動網路保持連線待命，並監看網路變動。只在開多路徑時啟用，stop() 時釋放
    private ConnectivityManager.NetworkCallback cellKeepaliveCallback;
    private ConnectivityManager.NetworkCallback netChangeCallback;

    private void startMultipathNetworkWatch(final ConnectivityManager cm) {
        try {
            android.net.NetworkRequest cellRequest = new android.net.NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
                    .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                    .build();
            cellKeepaliveCallback = new ConnectivityManager.NetworkCallback() {
                @Override
                public void onAvailable(Network network) {
                    LimeLog.info("[VIPLE-MPQUIC] cellular keepalive: network available");
                    injectNetInterfaces(cm);
                }
            };
            cm.requestNetwork(cellRequest, cellKeepaliveCallback);
            LimeLog.info("[VIPLE-MPQUIC] cellular keepalive requested for this session");
        } catch (Exception e) {
            cellKeepaliveCallback = null;
            LimeLog.warning("[VIPLE-MPQUIC] cellular keepalive request failed: " + e.getMessage());
        }
        try {
            android.net.NetworkRequest anyRequest = new android.net.NetworkRequest.Builder()
                    .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                    .build();
            netChangeCallback = new ConnectivityManager.NetworkCallback() {
                @Override
                public void onAvailable(Network network) {
                    injectNetInterfaces(cm);
                }

                @Override
                public void onLost(Network network) {
                    LimeLog.info("[VIPLE-MPQUIC] network lost - refreshing interface list");
                    injectNetInterfaces(cm);
                }

                @Override
                public void onLinkPropertiesChanged(Network network, LinkProperties linkProperties) {
                    injectNetInterfaces(cm);
                }
            };
            cm.registerNetworkCallback(anyRequest, netChangeCallback);
        } catch (Exception e) {
            netChangeCallback = null;
            LimeLog.warning("[VIPLE-MPQUIC] network change watch failed: " + e.getMessage());
        }
    }

    private void stopMultipathNetworkWatch() {
        ConnectivityManager cm = (ConnectivityManager) appContext.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm == null) {
            return;
        }
        if (cellKeepaliveCallback != null) {
            try { cm.unregisterNetworkCallback(cellKeepaliveCallback); } catch (Exception ignored) {}
            cellKeepaliveCallback = null;
        }
        if (netChangeCallback != null) {
            try { cm.unregisterNetworkCallback(netChangeCallback); } catch (Exception ignored) {}
            netChangeCallback = null;
        }
    }

    public void stop() {
        stopRequested = true;  // §REVIEW-1104：start 執行緒拿到 permit 後會看這個旗標
        stopMultipathNetworkWatch();  // §REVIEW-1104：釋放行動網路保活與網路監看
        // Interrupt any pending connection. This is thread-safe.
        MoonBridge.interruptConnection();

        // VipleStream: Clean up RTSP tunnel
        if (relayTunnel != null) {
            relayTunnel.stopTunnel();
            relayTunnel = null;
        }

        // Moonlight-core is not thread-safe with respect to connection start and stop, so
        // we must not invoke that functionality in parallel.
        synchronized (MoonBridge.class) {
            MoonBridge.stopConnection();
            MoonBridge.cleanupBridge();
        }

        // Now a pending connection can be processed
        // §REVIEW-1104：只還這個連線真的拿到的那一個
        if (holdsConnectionPermit.compareAndSet(true, false)) {
            connectionAllowed.release();
        }
    }

    private InetAddress resolveServerAddress() throws IOException {
        // Try to find an address that works for this host
        InetAddress[] addrs = InetAddress.getAllByName(context.serverAddress.address);
        for (InetAddress addr : addrs) {
            try (Socket s = new Socket()) {
                s.setSoLinger(true, 0);
                s.connect(new InetSocketAddress(addr, context.serverAddress.port), 1000);
                return addr;
            } catch (IOException e) {
                e.printStackTrace();
            }
        }

        // If we made it here, we didn't manage to find a working address. If DNS returned any
        // address, we'll use the first available address and hope for the best.
        if (addrs.length > 0) {
            return addrs[0];
        }
        else {
            throw new IOException("No addresses found for "+context.serverAddress);
        }
    }

    private int detectServerConnectionType() {
        ConnectivityManager connMgr = (ConnectivityManager) appContext.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Network activeNetwork = connMgr.getActiveNetwork();
            if (activeNetwork != null) {
                NetworkCapabilities netCaps = connMgr.getNetworkCapabilities(activeNetwork);
                if (netCaps != null) {
                    if (netCaps.hasTransport(NetworkCapabilities.TRANSPORT_VPN) ||
                            !netCaps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)) {
                        // VPNs are treated as remote connections
                        return StreamConfiguration.STREAM_CFG_REMOTE;
                    }
                    else if (netCaps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
                        // Cellular is always treated as remote to avoid any possible
                        // issues with 464XLAT or similar technologies.
                        return StreamConfiguration.STREAM_CFG_REMOTE;
                    }
                }

                // Check if the server address is on-link
                LinkProperties linkProperties = connMgr.getLinkProperties(activeNetwork);
                if (linkProperties != null) {
                    InetAddress serverAddress;
                    try {
                        serverAddress = resolveServerAddress();
                    } catch (IOException e) {
                        e.printStackTrace();

                        // We can't decide without being able to resolve the server address
                        return StreamConfiguration.STREAM_CFG_AUTO;
                    }

                    // If the address is in the NAT64 prefix, always treat it as remote
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        IpPrefix nat64Prefix = linkProperties.getNat64Prefix();
                        if (nat64Prefix != null && nat64Prefix.contains(serverAddress)) {
                            return StreamConfiguration.STREAM_CFG_REMOTE;
                        }
                    }

                    for (RouteInfo route : linkProperties.getRoutes()) {
                        // Skip non-unicast routes (which are all we get prior to Android 13)
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && route.getType() != RouteInfo.RTN_UNICAST) {
                            continue;
                        }

                        // Find the first route that matches this address
                        if (route.matches(serverAddress)) {
                            // If there's no gateway, this is an on-link destination
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                                // We want to use hasGateway() because getGateway() doesn't adhere
                                // to documented behavior of returning null for on-link addresses.
                                if (!route.hasGateway()) {
                                    return StreamConfiguration.STREAM_CFG_LOCAL;
                                }
                            }
                            else {
                                // getGateway() is documented to return null for on-link destinations,
                                // but it actually returns the unspecified address (0.0.0.0 or ::).
                                InetAddress gateway = route.getGateway();
                                if (gateway == null || gateway.isAnyLocalAddress()) {
                                    return StreamConfiguration.STREAM_CFG_LOCAL;
                                }
                            }

                            // We _should_ stop after the first matching route, but for some reason
                            // Android doesn't always report IPv6 routes in descending order of
                            // specificity and metric. To handle that case, we enumerate all matching
                            // routes, assuming that an on-link route will always be preferred.
                        }
                    }
                }
            }
        }
        else {
            NetworkInfo activeNetworkInfo = connMgr.getActiveNetworkInfo();
            if (activeNetworkInfo != null) {
                switch (activeNetworkInfo.getType()) {
                    case ConnectivityManager.TYPE_VPN:
                    case ConnectivityManager.TYPE_MOBILE:
                    case ConnectivityManager.TYPE_MOBILE_DUN:
                    case ConnectivityManager.TYPE_MOBILE_HIPRI:
                    case ConnectivityManager.TYPE_MOBILE_MMS:
                    case ConnectivityManager.TYPE_MOBILE_SUPL:
                    case ConnectivityManager.TYPE_WIMAX:
                        // VPNs and cellular connections are always remote connections
                        return StreamConfiguration.STREAM_CFG_REMOTE;
                }
            }
        }

        // If we can't determine the connection type, let moonlight-common-c decide.
        return StreamConfiguration.STREAM_CFG_AUTO;
    }
    
    private boolean startApp() throws XmlPullParserException, IOException
    {
        NvHTTP h = new NvHTTP(context.serverAddress, context.httpsPort, uniqueId, context.serverCert, cryptoProvider);

        // VipleStream: try direct serverinfo first; if that fails (e.g. LAN unreachable
        // and only relay is available), fall back to relay HTTP proxy. When we use the
        // relay we cannot send a TLS client cert, so the server always reports
        // PairStatus=0 — we skip the pair check in that case and trust the local cache.
        String serverInfo;
        boolean usedRelayForServerInfo = false;
        try {
            serverInfo = h.getServerInfo(true);
        } catch (IOException directFail) {
            if (context.relayUrl != null && !context.relayUrl.isEmpty()
                    && context.serverUuid != null && !context.serverUuid.isEmpty()) {
                LimeLog.info("[VIPLE-NAT] startApp direct serverinfo failed ("
                        + directFail.getMessage() + "), falling back to relay HTTP proxy");
                String resp = com.limelight.relay.RelayClient.httpProxy(context.relayUrl,
                        context.relayPsk, context.serverUuid,
                        "/serverinfo?uniqueid=0123456789ABCDEF", 8000);
                if (resp == null || resp.isEmpty()) {
                    throw directFail;
                }
                serverInfo = resp;
                usedRelayForServerInfo = true;
                // §REVIEW-1104：記錄 serverinfo 走過 relay，stop() 清掉 relayTunnel 後仍保留此旗標
                usedRelay = true;
            } else {
                throw directFail;
            }
        }

        context.serverAppVersion = h.getServerVersion(serverInfo);
        if (context.serverAppVersion == null) {
            context.connListener.displayMessage("Server version malformed");
            return false;
        }

        ComputerDetails details = h.getComputerDetails(serverInfo);
        context.isNvidiaServerSoftware = details.nvidiaServer;

        // May be missing for older servers
        context.serverGfeVersion = h.getGfeVersion(serverInfo);

        // VipleStream: skip pair-state check when serverinfo came from relay proxy —
        // proxy can't send our client cert, so PairStatus in the response is always 0
        // even when we're actually paired.
        if (!usedRelayForServerInfo
                && h.getPairState(serverInfo) != PairingManager.PairState.PAIRED) {
            context.connListener.displayMessage("Device not paired with computer");
            return false;
        }

        context.serverCodecModeSupport = (int)h.getServerCodecModeSupport(serverInfo);

        context.negotiatedHdr = (context.streamConfig.getSupportedVideoFormats() & MoonBridge.VIDEO_FORMAT_MASK_10BIT) != 0;
        // §REVIEW-1104：改用與 Limelight.h SCM_MASK_10BIT 一致的完整 10-bit 遮罩，避免 PyroWave 10-bit 或 4:4:4 10-bit 被誤判不支援 HDR
        if ((context.serverCodecModeSupport & 0x0A520200) == 0 && context.negotiatedHdr) {
            context.connListener.displayTransientMessage("Your PC GPU does not support streaming HDR. The stream will be SDR.");
            context.negotiatedHdr = false;
        }
        
        //
        // Decide on negotiated stream parameters now
        //
        
        // Check for a supported stream resolution
        if ((context.streamConfig.getWidth() > 4096 || context.streamConfig.getHeight() > 4096) &&
                (h.getServerCodecModeSupport(serverInfo) & 0x200) == 0 && context.isNvidiaServerSoftware) {
            context.connListener.displayMessage("Your host PC does not support streaming at resolutions above 4K.");
            return false;
        }
        else if ((context.streamConfig.getWidth() > 4096 || context.streamConfig.getHeight() > 4096) &&
                (context.streamConfig.getSupportedVideoFormats() & ~MoonBridge.VIDEO_FORMAT_MASK_H264) == 0) {
            context.connListener.displayMessage("Your streaming device must support HEVC or AV1 to stream at resolutions above 4K.");
            return false;
        }
        else if (context.streamConfig.getHeight() >= 2160 && !h.supports4K(serverInfo)) {
            // Client wants 4K but the server can't do it
            context.connListener.displayTransientMessage("You must update GeForce Experience to stream in 4K. The stream will be 1080p.");
            
            // Lower resolution to 1080p
            context.negotiatedWidth = 1920;
            context.negotiatedHeight = 1080;
        }
        else {
            // Take what the client wanted
            context.negotiatedWidth = context.streamConfig.getWidth();
            context.negotiatedHeight = context.streamConfig.getHeight();
        }

        // We will perform some connection type detection if the caller asked for it
        if (context.streamConfig.getRemote() == StreamConfiguration.STREAM_CFG_AUTO) {
            context.negotiatedRemoteStreaming = detectServerConnectionType();
            context.negotiatedPacketSize =
                    context.negotiatedRemoteStreaming == StreamConfiguration.STREAM_CFG_REMOTE ?
                            1024 : context.streamConfig.getMaxPacketSize();
        }
        else {
            context.negotiatedRemoteStreaming = context.streamConfig.getRemote();
            context.negotiatedPacketSize = context.streamConfig.getMaxPacketSize();
        }
        
        //
        // Video stream format will be decided during the RTSP handshake
        //
        
        NvApp app = context.streamConfig.getApp();
        
        // If the client did not provide an exact app ID, do a lookup with the applist
        if (!context.streamConfig.getApp().isInitialized()) {
            LimeLog.info("Using deprecated app lookup method - Please specify an app ID in your StreamConfiguration instead");
            app = h.getAppByName(context.streamConfig.getApp().getAppName());
            if (app == null) {
                context.connListener.displayMessage("The app " + context.streamConfig.getApp().getAppName() + " is not in GFE app list");
                return false;
            }
        }
        
        // If there's a game running, resume it
        if (h.getCurrentGame(serverInfo) != 0) {
            try {
                if (h.getCurrentGame(serverInfo) == app.getAppId()) {
                    try {
                        if (!h.launchApp(context, "resume", app.getAppId(), context.negotiatedHdr)) {
                            context.connListener.displayMessage("Failed to resume existing session");
                            return false;
                        }
                    } catch (IOException resumeEx) {
                        // VipleStream: Direct resume failed, try relay
                        if (!tryRelayLaunch("resume", app.getAppId(), context)) {
                            throw resumeEx;
                        }
                    }

                } else {
                    return quitAndLaunch(h, context);
                }
            } catch (HostHttpResponseException e) {
                if (e.getErrorCode() == 470) {
                    // This is the error you get when you try to resume a session that's not yours.
                    // Because this is fairly common, we'll display a more detailed message.
                    context.connListener.displayMessage("This session wasn't started by this device," +
                            " so it cannot be resumed. End streaming on the original " +
                            "device or the PC itself and try again. (Error code: "+e.getErrorCode()+")");
                    return false;
                }
                else if (e.getErrorCode() == 525) {
                    context.connListener.displayMessage("The application is minimized. Resume it on the PC manually or " +
                            "quit the session and start streaming again.");
                    return false;

                } else {
                    throw e;
                }
            }
            
            LimeLog.info("Resumed existing game session");
            return true;
        }
        else {
            return launchNotRunningApp(h, context);
        }
    }

    protected boolean quitAndLaunch(NvHTTP h, ConnectionContext context) throws IOException,
            XmlPullParserException {
        try {
            if (!h.quitApp()) {
                context.connListener.displayMessage("Failed to quit previous session! You must quit it manually");
                return false;
            } 
        } catch (HostHttpResponseException e) {
            if (e.getErrorCode() == 599) {
                context.connListener.displayMessage("This session wasn't started by this device," +
                        " so it cannot be quit. End streaming on the original " +
                        "device or the PC itself. (Error code: "+e.getErrorCode()+")");
                return false;
            }
            else {
                throw e;
            }
        }

        return launchNotRunningApp(h, context);
    }
    
    private boolean launchNotRunningApp(NvHTTP h, ConnectionContext context)
            throws IOException, XmlPullParserException {
        // Launch the app since it's not running
        try {
            if (!h.launchApp(context, "launch", context.streamConfig.getApp().getAppId(), context.negotiatedHdr)) {
                context.connListener.displayMessage("Failed to launch application");
                return false;
            }
        } catch (IOException e) {
            // VipleStream: Direct launch failed, try relay
            if (!tryRelayLaunch("launch", context.streamConfig.getApp().getAppId(), context)) {
                throw e; // No relay configured or relay also failed, re-throw
            }
        }

        LimeLog.info("Launched new game session");

        return true;
    }

    // VipleStream: Try launching/resuming via relay proxy when direct connection fails
    private boolean tryRelayLaunch(String verb, int appId, ConnectionContext ctx) throws IOException {
        if (ctx.relayUrl == null || ctx.relayUrl.isEmpty() || ctx.serverUuid == null) {
            return false;
        }

        LimeLog.info("[VIPLE-NAT] Direct " + verb + " failed, trying relay proxy...");

        String launchPath = buildRelayLaunchPath(verb, appId, ctx);
        String launchResp = RelayClient.httpProxy(ctx.relayUrl, ctx.relayPsk,
                ctx.serverUuid, launchPath, 45000);

        // VipleStream: /launch response contains <gamesession>, /resume contains
        // <resume>1</resume> — both have <sessionUrl0>. Check that instead of
        // gamesession-only (resume path was incorrectly reported as failed).
        if (launchResp != null && launchResp.contains("sessionUrl0")) {
            // §REVIEW-1104：記錄 launch/resume 走過 relay，stop() 清掉 relayTunnel 後仍保留此旗標
            usedRelay = true;
            try {
                // Extract RTSP session URL from XML response
                ctx.rtspSessionUrl = extractXmlTag(launchResp, "sessionUrl0");

                // Start RTSP tunnel
                relayTunnel = new RelayTcpTunnel(ctx.relayUrl, ctx.relayPsk,
                        ctx.serverUuid, 48010);
                int localPort = relayTunnel.startAndGetPort();
                if (localPort == 0) {
                    // §REVIEW-1103：tunnel 沒起來就別拿 port 0 去連 RTSP
                    relayTunnel.stopTunnel();
                    relayTunnel = null;
                    throw new IOException("Relay TCP tunnel failed to start");
                }
                ctx.rtspSessionUrl = "rtspenc://127.0.0.1:" + localPort;

                // §REVIEW-1104：UDP 目標維持原本的主機位址（不可連 loopback，Android 無 UDP relay）；若已有 STUN 位址則指向該位址
                if (ctx.stunAddress != null && !ctx.stunAddress.isEmpty()) {
                    ctx.serverAddress = new ComputerDetails.AddressTuple(ctx.stunAddress, ctx.serverAddress.port);




                }

                LimeLog.info("[VIPLE-NAT] Relay " + verb + " OK, RTSP tunnel on port " + localPort);
                return true;
            } catch (Exception e) {
                // §REVIEW-1104：tunnel 已 start() 之後若後續設定失敗，需及時關閉 tunnel 並清空欄位以防 leak
                if (relayTunnel != null) {
                    relayTunnel.stopTunnel();
                    relayTunnel = null;
                }
                LimeLog.info("[VIPLE-NAT] Relay post-launch setup failed: " + e.getMessage());
                throw new IOException("Relay launch setup failed", e);
            }
        } else {
            LimeLog.info("[VIPLE-NAT] Relay " + verb + " failed (no gamesession in response)");
            throw new IOException("Relay " + verb + " failed");
        }
    }

    private String buildRelayLaunchPath(String verb, int appId, ConnectionContext ctx) {
        // §REVIEW-1104：relay launch 參數組改與 NvHTTP.launchApp 一致（FPS、SOPS、HDR、音訊模式與聲道等）
        int fps = ctx.isNvidiaServerSoftware && ctx.streamConfig.getLaunchRefreshRate() > 60 ?
                0 : ctx.streamConfig.getLaunchRefreshRate();

        boolean enableSops = ctx.streamConfig.getSops();
        if (ctx.isNvidiaServerSoftware) {
            if (ctx.negotiatedWidth * ctx.negotiatedHeight > 1280 * 720 &&
                    ctx.negotiatedWidth * ctx.negotiatedHeight != 1920 * 1080 &&
                    ctx.negotiatedWidth * ctx.negotiatedHeight != 3840 * 2160) {
                enableSops = false;
            }
        }

        String hdrParam = !ctx.negotiatedHdr ? "" :
                "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0";

        return "/" + verb + "?appid=" + appId +
                "&mode=" + ctx.negotiatedWidth + "x" + ctx.negotiatedHeight + "x" + fps +
                "&additionalStates=1&sops=" + (enableSops ? 1 : 0) +
                "&rikey=" + bytesToHex(ctx.riKey.getEncoded()) +
                "&rikeyid=" + ctx.riKeyId +
                hdrParam +
                "&localAudioPlayMode=" + (ctx.streamConfig.getPlayLocalAudio() ? 1 : 0) +
                "&surroundAudioInfo=" + ctx.streamConfig.getAudioConfiguration().getSurroundAudioInfo() +
                "&remoteControllersBitmap=" + ctx.streamConfig.getAttachedGamepadMask() +
                "&gcmap=" + ctx.streamConfig.getAttachedGamepadMask() +
                "&gcpersist=" + (ctx.streamConfig.getPersistGamepadsAfterDisconnect() ? 1 : 0) +
                MoonBridge.getLaunchUrlQueryParameters() +
                "&uniqueid=0123456789ABCDEF";







    }

    private static String bytesToHex(byte[] bytes) {
        char[] hexChars = new char[bytes.length * 2];
        final char[] hexArray = "0123456789ABCDEF".toCharArray();
        for (int j = 0; j < bytes.length; j++) {
            int v = bytes[j] & 0xFF;
            hexChars[j * 2] = hexArray[v >>> 4];
            hexChars[j * 2 + 1] = hexArray[v & 0x0F];
        }
        return new String(hexChars);
    }

    // Simple XML tag extraction for relay responses (avoids needing package-private NvHTTP.getXmlString)
    private static String extractXmlTag(String xml, String tagName) {
        if (xml == null) return null;
        String openTag = "<" + tagName + ">";
        String closeTag = "</" + tagName + ">";
        int start = xml.indexOf(openTag);
        if (start < 0) return null;
        start += openTag.length();
        int end = xml.indexOf(closeTag, start);
        if (end < 0) return null;
        return xml.substring(start, end);
    }

    // §REVIEW-1104：判斷連線目標是否為 loopback、RFC1918 私網、link-local 或 100.64.0.0/10 (CGNAT)
    private static boolean isPrivateOrLoopbackAddress(String addressStr) {
        if (addressStr == null || addressStr.isEmpty()) {
            return false;
        }
        try {
            InetAddress addr = InetAddress.getByName(addressStr);
            if (addr.isLoopbackAddress() || addr.isSiteLocalAddress() || addr.isLinkLocalAddress()) {
                return true;
            }
            byte[] bytes = addr.getAddress();
            if (bytes.length == 4) {
                // 100.64.0.0/10 (CGNAT RFC 6598: 100.64.0.0 ~ 100.127.255.255)
                int b0 = bytes[0] & 0xFF;
                int b1 = bytes[1] & 0xFF;
                if (b0 == 100 && ((b1 & 0xC0) == 64)) {
                    return true;
                }
            } else if (bytes.length == 16) {
                // IPv6 Unique Local Address (fc00::/7, RFC 4193)
                if (((bytes[0] & 0xFF) & 0xFE) == 0xFC) {
                    return true;
                }
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    public void start(final AudioRenderer audioRenderer, final VideoDecoderRenderer videoDecoderRenderer, final NvConnectionListener connectionListener)
    {
        new Thread(new Runnable() {
            public void run() {
                context.connListener = connectionListener;
                context.videoCapabilities = videoDecoderRenderer.getCapabilities();

                String appName = context.streamConfig.getApp().getAppName();

                context.connListener.stageStarting(appName);

                try {
                    if (!startApp()) {
                        context.connListener.stageFailed(appName, 0, 0);
                        return;
                    }
                    context.connListener.stageComplete(appName);
                } catch (HostHttpResponseException e) {
                    e.printStackTrace();
                    context.connListener.displayMessage(e.getMessage());
                    context.connListener.stageFailed(appName, 0, e.getErrorCode());
                    return;
                } catch (XmlPullParserException | IOException e) {
                    e.printStackTrace();
                    context.connListener.displayMessage(e.getMessage());
                    context.connListener.stageFailed(appName, MoonBridge.ML_PORT_FLAG_TCP_47984 | MoonBridge.ML_PORT_FLAG_TCP_47989, 0);
                    return;
                }

                ByteBuffer ib = ByteBuffer.allocate(16);
                ib.putInt(context.riKeyId);

                // Acquire the connection semaphore to ensure we only have one
                // connection going at once.
                try {
                    connectionAllowed.acquire();
                } catch (InterruptedException e) {
                    context.connListener.displayMessage(e.getMessage());
                    context.connListener.stageFailed(appName, 0, 0);
                    return;
                }
                holdsConnectionPermit.set(true);
                // §REVIEW-1104：連線還在準備時使用者就離開了（stop() 已跑過）：不要再建立連線，自己把 permit 還回去
                if (stopRequested) {
                    if (holdsConnectionPermit.compareAndSet(true, false)) {
                        connectionAllowed.release();
                    }
                    return;
                }

                // Moonlight-core is not thread-safe with respect to connection start and stop, so
                // we must not invoke that functionality in parallel.
                synchronized (MoonBridge.class) {
                    MoonBridge.setupBridge(videoDecoderRenderer, audioRenderer, connectionListener);

                    // VipleStream: if ComputerManagerService didn't pre-populate stunAddress
                    // (happens when LAN direct connect succeeds and the fallback branch never
                    // runs), do an on-demand relay lookup so hole punch can still be attempted.
                    if ((context.stunAddress == null || context.stunAddress.isEmpty())
                            && context.relayUrl != null && !context.relayUrl.isEmpty()
                            && context.serverUuid != null) {
                        LimeLog.info("[VIPLE-NAT] stunAddress empty — on-demand relay lookup");
                        try {
                            com.limelight.relay.RelayClient.LookupResult r =
                                    com.limelight.relay.RelayClient.lookup(context.relayUrl,
                                            context.relayPsk, context.serverUuid, 5000);
                            if (r != null && r.success && r.stunIp != null) {
                                context.stunAddress = r.stunIp;
                                context.stunPunchPort = r.stunPort;  // §REVIEW-1104
                                // §REVIEW-1104：走 relay 時 lookup 得到 stunIp 後，將 UDP serverAddress 更新為該位址
                                if (relayTunnel != null) {
                                    context.serverAddress = new ComputerDetails.AddressTuple(r.stunIp, context.serverAddress.port);
                                }
                                LimeLog.info("[VIPLE-NAT] Relay lookup OK: stunAddress="
                                        + r.stunIp);
            
                            } else {
                                LimeLog.warning("[VIPLE-NAT] Relay lookup returned no stunIp");
                            }
                        } catch (Exception e) {
                            LimeLog.warning("[VIPLE-NAT] Relay lookup failed: "
                                    + e.getMessage());
                        }
                    }

                    // §REVIEW-1104：relay 連線且無 STUN 位址又無 UDP relay 時直接中止，避免 UDP 打向 loopback
                    if (relayTunnel != null && (context.stunAddress == null || context.stunAddress.isEmpty())) {
                        LimeLog.severe("[VIPLE-NAT] Relay connection has no STUN address and no UDP relay; aborting");
                        context.connListener.displayMessage("Relay connection requires STUN address");
                        context.connListener.stageFailed(appName, MoonBridge.ML_PORT_FLAG_UDP_47998 | MoonBridge.ML_PORT_FLAG_UDP_47999, 0);
                        relayTunnel.stopTunnel();
                        relayTunnel = null;
                        if (holdsConnectionPermit.compareAndSet(true, false)) {
                            connectionAllowed.release();
                        }
                        return;
                    }

                    // VipleStream: NAT hole punch before LiStartConnection so ENet control
                    // socket reuses the bound local port. Best-effort — failure is non-fatal.
                    // §REVIEW-1104：連線目標為私網、loopback 或 CGNAT (100.64.0.0/10) 時跳過打洞，避免無謂等待 3 秒超時
                    if (isPrivateOrLoopbackAddress(context.serverAddress.address)) {
                        LimeLog.info("[VIPLE-NAT] Skipping hole punch: direct private address "
                                + context.serverAddress.address);
                    } else
                    if (context.stunAddress != null && !context.stunAddress.isEmpty()
                            && context.serverUuid != null) {
                        // §REVIEW-1104（使用者決策 B）：有 STUN 回報的實際埠就打它（NAT 換埠時 +10 會打錯）；舊版 server 沒給才 +10
                        int controlPort = context.stunPunchPort != 0 ? context.stunPunchPort : context.serverAddress.port + 10;
                        byte[] srvU = new byte[16];
                        byte[] srvSrc = context.serverUuid.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                        System.arraycopy(srvSrc, 0, srvU, 0, Math.min(16, srvSrc.length));
                        byte[] cliU = new byte[16]; // placeholder — server intercept doesn't verify
                        LimeLog.info("[VIPLE-NAT] Attempting hole punch to "
                                + context.stunAddress + ":" + controlPort);
                        int punchResult = MoonBridge.holePunch(context.stunAddress, controlPort,
                                srvU, cliU, 3000);
                        if (punchResult == 0) {
                            LimeLog.info("[VIPLE-NAT] Hole punch succeeded!");
        
                        } else {
                            LimeLog.warning("[VIPLE-NAT] Hole punch failed (" + punchResult
                                    + "), proceeding with direct connection");
                        }
                        LimeLog.info("[VIPLE-NAT] LocalControlPort = "
                                + MoonBridge.getLocalControlPort());
                    }

                    // VipleStream §Q: inject network interfaces before starting
                    // so QUIC multipath can create subflows for WiFi + Cellular.
                    if (context.streamConfig.getMpQuicEnabled()) {
                        try {
                            ConnectivityManager cm = (ConnectivityManager)
                                    appContext.getSystemService(Context.CONNECTIVITY_SERVICE);
                            if (cm != null) {
                                injectNetInterfaces(cm);
                                // §REVIEW-1104：開了多路徑＝要更流暢：讓行動網路保持待命並監看網路變動
                                if (!stopRequested) {
                                    startMultipathNetworkWatch(cm);
                                }
                            }
                        } catch (Exception e) {
                            LimeLog.warning("[VIPLE-MPQUIC] Failed to enumerate interfaces: " + e.getMessage());
                        }

                        // §5f: per-user 0-RTT ticket cache (sandboxed app storage).
                        // picoquic keys tickets by SNI so a single file handles all servers.
                        try {
                            java.io.File ticketDir = appContext.getFilesDir();
                            if (ticketDir != null) {
                                String ticketPath = ticketDir.getAbsolutePath() + "/quic-tickets.bin";
                                MoonBridge.setQuicTicketStorePath(ticketPath);
                                LimeLog.info("[VIPLE-MPQUIC] Ticket store: " + ticketPath);
                            } else {
            
                                MoonBridge.setQuicTicketStorePath(null);
                            }
                        } catch (Exception e) {
                            LimeLog.warning("[VIPLE-MPQUIC] Ticket store unavailable: " + e.getMessage());
                            MoonBridge.setQuicTicketStorePath(null);
                        }
                    }

                    int ret = MoonBridge.startConnection(context.serverAddress.address,
                            context.serverAppVersion, context.serverGfeVersion, context.rtspSessionUrl,
                            context.serverCodecModeSupport,
                            context.negotiatedWidth, context.negotiatedHeight,
                            context.streamConfig.getRefreshRate(), context.streamConfig.getBitrate(),
                            context.negotiatedPacketSize, context.negotiatedRemoteStreaming,
                            context.streamConfig.getAudioConfiguration().toInt(),
                            context.streamConfig.getSupportedVideoFormats(),
                            context.streamConfig.getClientRefreshRateX100(),
                            context.riKey.getEncoded(), ib.array(),
                            context.videoCapabilities,
                            context.streamConfig.getColorSpace(),
                            context.streamConfig.getColorRange(),
                            context.streamConfig.getMpQuicEnabled(),
                            context.streamConfig.getMpQuicScheduler(),
                            context.streamConfig.getMpQuicPort(),
                            context.streamConfig.getAutoAdjustBitrate());
                    if (ret != 0) {
                        // §REVIEW-1104：startConnection 失敗時及時關閉 relay tunnel，避免洩漏 WebSocket 連線
                        if (relayTunnel != null) {
                            relayTunnel.stopTunnel();
                            relayTunnel = null;
                        }
                        // LiStartConnection() failed, so the caller is not expected
                        // to stop the connection themselves. We need to release their
                        // semaphore count for them.
                        if (holdsConnectionPermit.compareAndSet(true, false)) {  // §REVIEW-1104
                            connectionAllowed.release();
                        }
                        return;
                    }
                }
            }
        }).start();
    }
    
    public void sendMouseMove(final short deltaX, final short deltaY)
    {
        if (!isMonkey) {
            MoonBridge.sendMouseMove(deltaX, deltaY);
        }
    }

    public void sendMousePosition(short x, short y, short referenceWidth, short referenceHeight)
    {
        if (!isMonkey) {
            MoonBridge.sendMousePosition(x, y, referenceWidth, referenceHeight);
        }
    }

    public void sendMouseMoveAsMousePosition(short deltaX, short deltaY, short referenceWidth, short referenceHeight)
    {
        if (!isMonkey) {
            MoonBridge.sendMouseMoveAsMousePosition(deltaX, deltaY, referenceWidth, referenceHeight);
        }
    }

    public void sendMouseButtonDown(final byte mouseButton)
    {
        if (!isMonkey) {
            MoonBridge.sendMouseButton(MouseButtonPacket.PRESS_EVENT, mouseButton);
        }
    }
    
    public void sendMouseButtonUp(final byte mouseButton)
    {
        if (!isMonkey) {
            MoonBridge.sendMouseButton(MouseButtonPacket.RELEASE_EVENT, mouseButton);
        }
    }
    
    public void sendControllerInput(final short controllerNumber,
            final short activeGamepadMask, final int buttonFlags,
            final byte leftTrigger, final byte rightTrigger,
            final short leftStickX, final short leftStickY,
            final short rightStickX, final short rightStickY)
    {
        if (!isMonkey) {
            MoonBridge.sendMultiControllerInput(controllerNumber, activeGamepadMask, buttonFlags,
                    leftTrigger, rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);
        }
    }

    public void sendKeyboardInput(final short keyMap, final byte keyDirection, final byte modifier, final byte flags) {
        if (!isMonkey) {
            MoonBridge.sendKeyboardInput(keyMap, keyDirection, modifier, flags);
        }
    }
    
    public void sendMouseScroll(final byte scrollClicks) {
        if (!isMonkey) {
            MoonBridge.sendMouseHighResScroll((short)(scrollClicks * 120)); // WHEEL_DELTA
        }
    }

    public void sendMouseHScroll(final byte scrollClicks) {
        if (!isMonkey) {
            MoonBridge.sendMouseHighResHScroll((short)(scrollClicks * 120)); // WHEEL_DELTA
        }
    }

    public void sendMouseHighResScroll(final short scrollAmount) {
        if (!isMonkey) {
            MoonBridge.sendMouseHighResScroll(scrollAmount);
        }
    }

    public void sendMouseHighResHScroll(final short scrollAmount) {
        if (!isMonkey) {
            MoonBridge.sendMouseHighResHScroll(scrollAmount);
        }
    }

    public int sendTouchEvent(byte eventType, int pointerId, float x, float y, float pressureOrDistance,
                              float contactAreaMajor, float contactAreaMinor, short rotation) {
        if (!isMonkey) {
            return MoonBridge.sendTouchEvent(eventType, pointerId, x, y, pressureOrDistance,
                    contactAreaMajor, contactAreaMinor, rotation);
        }
        else {
            return MoonBridge.LI_ERR_UNSUPPORTED;
        }
    }

    public int sendPenEvent(byte eventType, byte toolType, byte penButtons, float x, float y,
                            float pressureOrDistance, float contactAreaMajor, float contactAreaMinor,
                            short rotation, byte tilt) {
        if (!isMonkey) {
            return MoonBridge.sendPenEvent(eventType, toolType, penButtons, x, y, pressureOrDistance,
                    contactAreaMajor, contactAreaMinor, rotation, tilt);
        }
        else {
            return MoonBridge.LI_ERR_UNSUPPORTED;
        }
    }

    public int sendControllerArrivalEvent(byte controllerNumber, short activeGamepadMask, byte type,
                                          int supportedButtonFlags, short capabilities) {
        return MoonBridge.sendControllerArrivalEvent(controllerNumber, activeGamepadMask, type, supportedButtonFlags, capabilities);
    }

    public int sendControllerTouchEvent(byte controllerNumber, byte eventType, int pointerId,
                                        float x, float y, float pressure) {
        if (!isMonkey) {
            return MoonBridge.sendControllerTouchEvent(controllerNumber, eventType, pointerId, x, y, pressure);
        }
        else {
            return MoonBridge.LI_ERR_UNSUPPORTED;
        }
    }

    public int sendControllerMotionEvent(byte controllerNumber, byte motionType,
                                         float x, float y, float z) {
        if (!isMonkey) {
            return MoonBridge.sendControllerMotionEvent(controllerNumber, motionType, x, y, z);
        }
        else {
            return MoonBridge.LI_ERR_UNSUPPORTED;
        }
    }

    public void sendControllerBatteryEvent(byte controllerNumber, byte batteryState, byte batteryPercentage) {
        MoonBridge.sendControllerBatteryEvent(controllerNumber, batteryState, batteryPercentage);
    }

    public void sendUtf8Text(final String text) {
        if (!isMonkey) {
            MoonBridge.sendUtf8Text(text);
        }
    }

    public static String findExternalAddressForMdns(String stunHostname, int stunPort) {
        return MoonBridge.findExternalAddressIP4(stunHostname, stunPort);
    }
}
