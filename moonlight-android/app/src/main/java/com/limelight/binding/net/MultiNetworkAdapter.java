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
 * §Q-REVIEW-P2 — JNI bridge gap (tracked for Phase 5):
 *   moonlight-common-c's PlatformNetIf.c on Android falls through to
 *   lcSetNetInterfacesFromJni(), which expects this Java class to call
 *   a native method that pushes the network list across. That native
 *   method is not yet implemented. Currently the native side sees zero
 *   subflows on Android (single-path QUIC still works, but multipath
 *   does not). Phase 5: add nativeSetInterfaces(...) JNI method in
 *   MoonBridge and call it from NvConnection.start() right after
 *   listing available networks.
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
    private ConnectivityManager.NetworkCallback networkCallback;
    private NetworkChangeListener listener;

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
                if (!a.isLoopbackAddress()) {
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
                boolean metered = !caps.hasCapability(
                        NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
                int bandwidth = caps.getLinkDownstreamBandwidthKbps();

                List<LinkAddress> addresses = props.getLinkAddresses();
                InetAddress addr = null;
                for (LinkAddress la : addresses) {
                    InetAddress a = la.getAddress();
                    if (!a.isLoopbackAddress()) {
                        addr = a;
                        break;
                    }
                }

                if (addr != null && MultiNetworkAdapter.this.listener != null) {
                    NetworkInfo info = new NetworkInfo(name, transport, addr,
                            metered, bandwidth, network);
                    Log.i(TAG, "Network available: " + name + " (" +
                            info.transportName() + ")");
                    MultiNetworkAdapter.this.listener.onNetworkAvailable(info);
                }
            }

            @Override
            public void onLost(Network network) {
                LinkProperties props = connectivityManager.getLinkProperties(network);
                String name = (props != null) ? props.getInterfaceName() : "unknown";
                Log.i(TAG, "Network lost: " + name);
                if (MultiNetworkAdapter.this.listener != null) {
                    MultiNetworkAdapter.this.listener.onNetworkLost(
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
