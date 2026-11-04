package com.limelight.binding.net;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;
import android.util.Log;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.List;

/**
 * VipleStream §Q MP-QUIC: Android multi-network interface adapter.
 *
 * Enumerates available networks (WiFi, cellular, Ethernet) and monitors
 * connectivity changes for QUIC multipath subflow management.
 * Uses ConnectivityManager.getAllNetworks() + NetworkCapabilities.
 *
 * Cellular keepalive (requestCellularKeepalive) is used during a session
 * to keep the cellular radio active when WiFi is the primary path, so a
 * WiFi → cellular failover does not have to wait for the cell to spin up.
 *
 * Note: the native interface injection used by QUIC subflow creation lives
 * in NvConnection.start() (inline ConnectivityManager enumeration calling
 * MoonBridge.setNetInterfaces). This class is the long-running monitor and
 * keepalive helper, not the one-shot enumerator.
 */
public class MultiNetworkAdapter {
    private static final String TAG = "VIPLE-MPQUIC-NET";

    public static class NetworkInfo {
        public final String name;
        public final int transport; // NetworkCapabilities.TRANSPORT_*
        public final InetAddress address;
        public final boolean isMetered;
        public final int linkDownBandwidthKbps;
        public final Network network;

        NetworkInfo(String name, int transport, InetAddress address,
                    boolean isMetered, int linkDownBandwidthKbps, Network network) {
            this.name = name;
            this.transport = transport;
            this.address = address;
            this.isMetered = isMetered;
            this.linkDownBandwidthKbps = linkDownBandwidthKbps;
            this.network = network;
        }

        public String transportName() {
            switch (transport) {
                case NetworkCapabilities.TRANSPORT_WIFI: return "WiFi";
                case NetworkCapabilities.TRANSPORT_CELLULAR: return "Cellular";
                case NetworkCapabilities.TRANSPORT_ETHERNET: return "Ethernet";
                case NetworkCapabilities.TRANSPORT_VPN: return "VPN";
                default: return "Unknown";
            }
        }
    }

    public interface NetworkChangeListener {
        void onNetworkAvailable(NetworkInfo info);
        void onNetworkLost(String name);
    }

    private final ConnectivityManager connectivityManager;
    // §REVIEW-1104：記錄 Network 與介面名稱對應，避免 onLost 時取不到 LinkProperties 變成 unknown
    private final java.util.Map<Network, String> networkNames = new java.util.concurrent.ConcurrentHashMap<>();
    private ConnectivityManager.NetworkCallback networkCallback;
    // §REVIEW-1104：宣告為 volatile 確保跨執行緒能即時看見更新
    private volatile NetworkChangeListener listener;

    public MultiNetworkAdapter(Context context) {
        connectivityManager = (ConnectivityManager)
                context.getSystemService(Context.CONNECTIVITY_SERVICE);
    }

    /**
     * Enumerate all currently available network interfaces.
     */
    public List<NetworkInfo> getAvailableNetworks() {
        List<NetworkInfo> result = new ArrayList<>();

        if (connectivityManager == null)
            return result;

        Network[] networks = connectivityManager.getAllNetworks();
        if (networks == null)
            return result;

        for (Network network : networks) {
            NetworkCapabilities caps = connectivityManager.getNetworkCapabilities(network);
            LinkProperties props = connectivityManager.getLinkProperties(network);
            if (caps == null || props == null)
                continue;

            if (!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET))
                continue;

            int transport = -1;
            String name = props.getInterfaceName();
            if (name == null) name = "unknown";

            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                transport = NetworkCapabilities.TRANSPORT_WIFI;
            } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
                transport = NetworkCapabilities.TRANSPORT_CELLULAR;
            } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) {
                transport = NetworkCapabilities.TRANSPORT_ETHERNET;
            } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) {
                transport = NetworkCapabilities.TRANSPORT_VPN;
            }

            if (transport < 0)
                continue;

            boolean metered = !caps.hasCapability(
                    NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
            int bandwidth = caps.getLinkDownstreamBandwidthKbps();

            // Get first non-loopback address.
            // LinkProperties.getLinkAddresses() returns List<LinkAddress>
            // (each LinkAddress wraps an InetAddress + prefix length).
            List<LinkAddress> addresses = props.getLinkAddresses();
            InetAddress addr = null;
            for (LinkAddress la : addresses) {
                InetAddress a = la.getAddress();
                // §REVIEW-1104：過濾 link-local 位址，避免 IPv6 fe80:: 等無法路由的位址被選為本機連線位址
                if (!a.isLoopbackAddress() && !a.isLinkLocalAddress()) {
                    addr = a;
                    break;
                }
            }

            if (addr != null) {
                result.add(new NetworkInfo(name, transport, addr,
                        metered, bandwidth, network));
                Log.i(TAG, "Found network: " + name + " (" +
                        result.get(result.size() - 1).transportName() +
                        ") addr=" + addr.getHostAddress() +
                        " metered=" + metered +
                        " bw=" + bandwidth + "kbps");
            }
        }

        return result;
    }

    /**
     * Start monitoring network changes. The listener will be called
     * when new networks appear or existing ones disappear.
     */
    public void startMonitoring(NetworkChangeListener listener) {
        this.listener = listener;

        NetworkRequest request = new NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();

        networkCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                NetworkCapabilities caps = connectivityManager.getNetworkCapabilities(network);
                LinkProperties props = connectivityManager.getLinkProperties(network);
                if (caps == null || props == null) return;

                int transport = -1;
                if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI))
                    transport = NetworkCapabilities.TRANSPORT_WIFI;
                else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR))
                    transport = NetworkCapabilities.TRANSPORT_CELLULAR;
                else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET))
                    transport = NetworkCapabilities.TRANSPORT_ETHERNET;

                if (transport < 0) return;

                String name = props.getInterfaceName();
                if (name == null) name = "unknown";
                // §REVIEW-1104：記錄 Network 對應的介面名稱，供 onLost 查表避免回傳 unknown
                networkNames.put(network, name);



                boolean metered = !caps.hasCapability(
                        NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
                int bandwidth = caps.getLinkDownstreamBandwidthKbps();

                List<LinkAddress> addresses = props.getLinkAddresses();
                InetAddress addr = null;
                for (LinkAddress la : addresses) {
                    InetAddress a = la.getAddress();
                    // §REVIEW-1104：過濾 link-local 位址，避免 IPv6 fe80:: 等無法路由的位址被選為本機連線位址
                    if (!a.isLoopbackAddress() && !a.isLinkLocalAddress()) {
                        addr = a;
                        break;
                    }
                }

                // §REVIEW-1104：複製 listener 至區域變數，避免 check-then-act 期間被 stopMonitoring 清為 null 導致 NPE
                final NetworkChangeListener currentListener = MultiNetworkAdapter.this.listener;
                if (addr != null && currentListener != null) {
                    NetworkInfo info = new NetworkInfo(name, transport, addr,
                            metered, bandwidth, network);
                    Log.i(TAG, "Network available: " + name + " (" +
                            info.transportName() + ")");
                    currentListener.onNetworkAvailable(info);
                }
            }

            @Override
            // §REVIEW-1104：監聽 LinkProperties 變更並更新介面名稱對照表
            public void onLinkPropertiesChanged(Network network, LinkProperties props) {
                if (props != null && props.getInterfaceName() != null) {
                    networkNames.put(network, props.getInterfaceName());
                }
            }

            @Override
            public void onLost(Network network) {
                LinkProperties props = connectivityManager.getLinkProperties(network);
                // §REVIEW-1104：優先從 networkNames 查表取得介面名稱，避免 onLost 時 getLinkProperties 為 null 造成名稱遺失為 unknown
                String name = networkNames.remove(network);
                if (name == null) {
                    name = (props != null) ? props.getInterfaceName() : "unknown";
                }
                Log.i(TAG, "Network lost: " + name);
                // §REVIEW-1104：複製 listener 至區域變數，避免 check-then-act 期間被 stopMonitoring 清為 null 導致 NPE
                final NetworkChangeListener currentListener = MultiNetworkAdapter.this.listener;
                if (currentListener != null) {
                    currentListener.onNetworkLost(
                            name != null ? name : "unknown");
                }
            }
        };

        connectivityManager.registerNetworkCallback(request, networkCallback);
        Log.i(TAG, "Network monitoring started");
    }

    /**
     * Stop monitoring network changes.
     */
    public void stopMonitoring() {
        if (networkCallback != null) {
            try {
                connectivityManager.unregisterNetworkCallback(networkCallback);
            } catch (IllegalArgumentException e) {
                // Already unregistered
            }
            networkCallback = null;
            // §REVIEW-1104：清理網路名稱對照快取
            networkNames.clear();
            Log.i(TAG, "Network monitoring stopped");
        }
        listener = null;
    }

    /**
     * Request cellular to stay up alongside WiFi (required for multipath).
     * On Android, the OS normally drops cellular when WiFi connects.
     */
    public void requestCellularKeepalive() {
        NetworkRequest cellRequest = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();

        connectivityManager.requestNetwork(cellRequest,
                new ConnectivityManager.NetworkCallback() {
                    @Override
                    public void onAvailable(Network network) {
                        Log.i(TAG, "Cellular keepalive: network retained");
                    }

                    @Override
                    public void onUnavailable() {
                        Log.w(TAG, "Cellular keepalive: network unavailable");
                    }
                });
        Log.i(TAG, "Cellular keepalive requested");
    }
}
