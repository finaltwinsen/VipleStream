package com.limelight.nvstream.http;

import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;


public class ComputerDetails {
    public enum State {
        ONLINE, OFFLINE, UNKNOWN
    }

    public static class DisplayMode {
        public int width;
        public int height;
        public int refreshRate;

        public DisplayMode(int width, int height, int refreshRate) {
            this.width = width;
            this.height = height;
            this.refreshRate = refreshRate;
        }

        @Override
        public String toString() {
            return width + "x" + height + "@" + refreshRate + "Hz";
        }
    }

    public static class AddressTuple {
        public String address;
        public int port;

        public AddressTuple(String address, int port) {
            if (address == null) {
                throw new IllegalArgumentException("Address cannot be null");
            }
            if (port <= 0) {
                throw new IllegalArgumentException("Invalid port");
            }

            // If this was an escaped IPv6 address, remove the brackets
            if (address.startsWith("[") && address.endsWith("]")) {
                address = address.substring(1, address.length() - 1);
            }

            this.address = address;
            this.port = port;
        }

        @Override
        public int hashCode() {
            return Objects.hash(address, port);
        }

        @Override
        public boolean equals(Object obj) {
            if (!(obj instanceof AddressTuple)) {
                return false;
            }

            AddressTuple that = (AddressTuple) obj;
            return address.equals(that.address) && port == that.port;
        }

        public String toString() {
            if (address.contains(":")) {
                // IPv6
                return "[" + address + "]:" + port;
            }
            else {
                // IPv4 and hostnames
                return address + ":" + port;
            }
        }
    }

    // Persistent attributes
    public String uuid;
    public String name;
    public AddressTuple localAddress;
    public AddressTuple remoteAddress;
    public AddressTuple manualAddress;
    public AddressTuple ipv6Address;
    public String macAddress;
    public X509Certificate serverCert;

    // Transient attributes
    public State state;
    public AddressTuple activeAddress;
    public int httpsPort;
    public int externalPort;
    public PairingManager.PairState pairState;
    public int runningGameId;
    public String rawAppList;
    public boolean nvidiaServer;
    public String stunAddress;  // VipleStream: STUN public endpoint IP
    public String stunNatType;  // VipleStream: NAT type (punchable/symmetric)

    // VipleStream capability marker (v1.2.93). Non-blocking — `false`
    // simply means the host is vanilla Sunshine / GFE and we get
    // standard Moonlight protocol behaviour. `true` lets the UI surface
    // VipleStream-only affordances (badge, extra settings entry points,
    // etc.).
    public boolean isVipleStreamPeer;
    public String vipleStreamProtocol;

    // VipleStream §M.parity Wire-W1/W2 (2026-05-20). Surface host
    // codec capability bit field (SCM_*) + advertised primary display
    // modes from /serverinfo so streamConfig negotiation can AND with
    // server caps and the UI can hint host native resolution.  Both
    // optional — vanilla Moonlight servers omit these elements and
    // we degrade to current default behaviour (0 / empty list).
    public int serverCodecModeSupport;
    public List<DisplayMode> supportedDisplayModes = new ArrayList<>();

    public ComputerDetails() {
        // Use defaults
        state = State.UNKNOWN;
    }

    public ComputerDetails(ComputerDetails details) {
        // Copy details from the other computer
        update(details);
    }

    public int guessExternalPort() {
        if (externalPort != 0) {
            return externalPort;
        }
        else if (remoteAddress != null) {
            return remoteAddress.port;
        }
        else if (activeAddress != null) {
            return activeAddress.port;
        }
        else if (ipv6Address != null) {
            return ipv6Address.port;
        }
        else if (localAddress != null) {
            return localAddress.port;
        }
        else {
            return NvHTTP.DEFAULT_HTTP_PORT;
        }
    }

    public void update(ComputerDetails details) {
        this.state = details.state;
        this.name = details.name;
        this.uuid = details.uuid;
        if (details.activeAddress != null) {
            this.activeAddress = details.activeAddress;
        }
        // We can get IPv4 loopback addresses with GS IPv6 Forwarder
        if (details.localAddress != null && !details.localAddress.address.startsWith("127.")) {
            this.localAddress = details.localAddress;
        }
        if (details.remoteAddress != null) {
            this.remoteAddress = details.remoteAddress;
        }
        else if (this.remoteAddress != null && details.externalPort != 0) {
            // If we have a remote address already (perhaps via STUN) but our updated details
            // don't have a new one (because GFE doesn't send one), propagate the external
            // port to the current remote address. We may have tried to guess it previously.
            this.remoteAddress.port = details.externalPort;
        }
        if (details.manualAddress != null) {
            this.manualAddress = details.manualAddress;
        }
        if (details.ipv6Address != null) {
            this.ipv6Address = details.ipv6Address;
        }
        if (details.macAddress != null && !details.macAddress.equals("00:00:00:00:00:00")) {
            this.macAddress = details.macAddress;
        }
        if (details.serverCert != null) {
            this.serverCert = details.serverCert;
        }
        this.externalPort = details.externalPort;
        this.httpsPort = details.httpsPort;
        this.pairState = details.pairState;
        this.runningGameId = details.runningGameId;
        this.nvidiaServer = details.nvidiaServer;
        this.rawAppList = details.rawAppList;
        this.isVipleStreamPeer = details.isVipleStreamPeer;
        this.vipleStreamProtocol = details.vipleStreamProtocol;
        this.serverCodecModeSupport = details.serverCodecModeSupport;
        if (details.supportedDisplayModes != null && !details.supportedDisplayModes.isEmpty()) {
            this.supportedDisplayModes = details.supportedDisplayModes;
        }
    }

    @Override
    public String toString() {
        StringBuilder str = new StringBuilder();
        str.append("Name: ").append(name).append("\n");
        str.append("State: ").append(state).append("\n");
        str.append("Active Address: ").append(activeAddress).append("\n");
        str.append("UUID: ").append(uuid).append("\n");
        str.append("Local Address: ").append(localAddress).append("\n");
        str.append("Remote Address: ").append(remoteAddress).append("\n");
        str.append("IPv6 Address: ").append(ipv6Address).append("\n");
        str.append("Manual Address: ").append(manualAddress).append("\n");
        str.append("MAC Address: ").append(macAddress).append("\n");
        str.append("Pair State: ").append(pairState).append("\n");
        str.append("Running Game ID: ").append(runningGameId).append("\n");
        str.append("HTTPS Port: ").append(httpsPort).append("\n");
        return str.toString();
    }
}
