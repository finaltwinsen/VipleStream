// Adapted from DroidDeck (GPL-3.0) / elasticjava/viture-v2 (MIT, Copyright (c) 2026 Holger Bartnick).
package com.limelight.glasses;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Looper;
import android.util.Log;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * §VITURE：VID 符合就列出來。Beast 與社群回報的麥克風 PID 不固定，白名單會把要探測的介面濾掉。
 * 相機是另一個裝置（0x0C45:0x6368），這裡不開啟。
 */
public final class VitureUsb {
    public static final int VENDOR_ID = 0x35CA;
    public static final int PID_BEAST = 0x1201;
    public static final int PID_BEAST_ALT = 0x1211;
    public static final int PID_PHANTOM_BEAST = 0x1221;
    public static final int PID_PRO_2 = 0x1301;
    /** §VITURE：社群把 35ca:1102 描述成麥克風，仍要探測，不能拿來當排除條件。 */
    public static final int PID_REPORTED_MICROPHONE = 0x1102;
    public static final int CAMERA_VENDOR_ID = 0x0C45;
    public static final int CAMERA_PRODUCT_ID = 0x6368;

    public static final String ACTION_USB_PERMISSION = "com.limelight.VITURE_GLASSES_USB_PERMISSION";

    private static final String LOG_TAG = "VipleViture";
    // §VITURE：Activity 結束時若探測執行緒還卡在傳輸裡，要用這份連線把介面放掉。
    private static volatile Session active;

    private VitureUsb() {
    }

    static void closeActive() {
        Session current = active;
        if (current != null) {
            current.close();
        }
    }

    public static void logI(String message) {
        Log.i(LOG_TAG, "[VIPLE-VITURE] " + message);
    }

    public static void logW(String message, Throwable error) {
        Log.w(LOG_TAG, "[VIPLE-VITURE] " + message, error);
    }

    public static List<UsbDevice> findDevices(UsbManager usb) {
        ArrayList<UsbDevice> found = new ArrayList<>();
        if (usb == null) {
            return found;
        }
        for (UsbDevice device : usb.getDeviceList().values()) {
            if (device != null && device.getVendorId() == VENDOR_ID) {
                found.add(device);
            }
        }
        sortDevices(found);
        return found;
    }

    public static String knownLabel(int vendorId, int productId) {
        if (vendorId == CAMERA_VENDOR_ID && productId == CAMERA_PRODUCT_ID) {
            return "VITURE camera (not opened)";
        }
        if (vendorId != VENDOR_ID) {
            return "";
        }
        switch (productId) {
            case PID_BEAST:
            case PID_BEAST_ALT:
                return "Beast";
            case PID_PHANTOM_BEAST:
                return "Phantom Beast";
            case PID_PRO_2:
                return "Pro 2";
            case PID_REPORTED_MICROPHONE:
                return "community-reported microphone";
            default:
                return "unknown-pid";
        }
    }

    public static void requestPermission(Context context, UsbManager usb, UsbDevice device) {
        int intentFlags = 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // §VITURE：跟 UsbDriverService 一樣，Android 12 以上必須是 MUTABLE，系統才填得了 EXTRA_DEVICE。
            intentFlags |= PendingIntent.FLAG_MUTABLE;
        }
        Intent intent = new Intent(ACTION_USB_PERMISSION);
        intent.setPackage(context.getPackageName());
        PendingIntent pending = PendingIntent.getBroadcast(context, 0, intent, intentFlags);
        usb.requestPermission(device, pending);
    }

    public static void appendInventory(UsbManager usb, StringBuilder report) {
        report.append("USB\n");
        if (usb == null) {
            report.append("count=0\n");
            report.append("usb_manager=unavailable\n");
            return;
        }
        ArrayList<UsbDevice> devices = new ArrayList<>(usb.getDeviceList().values());
        sortDevices(devices);
        report.append("count=").append(devices.size()).append('\n');
        for (UsbDevice device : devices) {
            if (device == null) {
                continue;
            }
            try {
                appendDeviceSummary(device, report);
                if (device.getVendorId() == VENDOR_ID) {
                    appendInterfaces(device, report);
                    appendHidResolve(device, report);
                }
            } catch (RuntimeException error) {
                report.append("device error=").append(VitureProbe.errorText(error)).append('\n');
            }
        }
    }

    public static final class Session implements VitureProbe.Link {
        private final UsbDeviceConnection connection;
        private final UsbInterface claimed;
        private final UsbEndpoint inEndpoint;
        private final UsbEndpoint outEndpoint;
        private final byte[] inBuffer;
        private final Object ioLock = new Object();
        private boolean closed;

        private Session(UsbDeviceConnection connection, UsbInterface claimed,
                        UsbEndpoint inEndpoint, UsbEndpoint outEndpoint) {
            this.connection = connection;
            this.claimed = claimed;
            this.inEndpoint = inEndpoint;
            this.outEndpoint = outEndpoint;
            int inSize = inEndpoint.getMaxPacketSize();
            if (inSize < VitureV2Protocol.PACKET_SIZE) {
                inSize = VitureV2Protocol.PACKET_SIZE;
            }
            if (inSize > 4096) {
                inSize = 4096;
            }
            this.inBuffer = new byte[inSize];
        }

        static Session open(UsbManager usb, UsbDevice device, StringBuilder report) {
            checkNotMainThread();
            UsbDeviceConnection connection = usb.openDevice(device);
            if (connection == null) {
                report.append("open_device=failed\n");
                return null;
            }
            Session session = claim(connection, device, report);
            if (session == null) {
                connection.close();
                return null;
            }
            active = session;
            return session;
        }

        @Override
        public int write(byte[] packet, int timeoutMs, boolean padToPacketSize) throws IOException {
            checkNotMainThread();
            if (packet == null) {
                throw new IOException("null packet");
            }
            byte[] tx = packet;
            if (padToPacketSize) {
                int target = outEndpoint.getMaxPacketSize();
                if (target > VitureV2Protocol.PACKET_SIZE) {
                    target = VitureV2Protocol.PACKET_SIZE;
                }
                if (target > packet.length) {
                    tx = new byte[target];
                    System.arraycopy(packet, 0, tx, 0, packet.length);
                }
            }
            UsbDeviceConnection conn;
            synchronized (ioLock) {
                if (closed) {
                    throw new IOException("closed");
                }
                conn = connection;
            }
            // §VITURE：interrupt 端點也走 bulkTransfer，Android 這個 API 才有逾時；舊版參考連線同樣這樣送。
            return conn.bulkTransfer(outEndpoint, tx, tx.length, timeoutMs);
        }

        @Override
        public byte[] read(int timeoutMs) throws IOException {
            checkNotMainThread();
            UsbDeviceConnection conn;
            synchronized (ioLock) {
                if (closed) {
                    throw new IOException("closed");
                }
                conn = connection;
            }
            int count = conn.bulkTransfer(inEndpoint, inBuffer, inBuffer.length, timeoutMs);
            if (count < 0) {
                return null;
            }
            if (count == 0) {
                return new byte[0];
            }
            byte[] copy = new byte[count];
            synchronized (ioLock) {
                System.arraycopy(inBuffer, 0, copy, 0, count);
            }
            return copy;
        }

        public void close() {
            UsbDeviceConnection conn;
            UsbInterface intf;
            synchronized (ioLock) {
                if (closed) {
                    return;
                }
                closed = true;
                if (active == this) {
                    active = null;
                }
                conn = connection;
                intf = claimed;
            }
            try {
                conn.releaseInterface(intf);
            } catch (RuntimeException ignored) {
                // 已經拔除時 release 會失敗，關閉仍要繼續。
            }
            try {
                conn.close();
            } catch (RuntimeException ignored) {
                // 同上。
            }
        }

        private static Session claim(UsbDeviceConnection connection, UsbDevice device, StringBuilder report) {
            ArrayList<UsbInterface> strict = new ArrayList<>();
            ArrayList<UsbInterface> hid = new ArrayList<>();
            for (int i = 0; i < device.getInterfaceCount(); i++) {
                UsbInterface intf = device.getInterface(i);
                if (intf.getInterfaceClass() != UsbConstants.USB_CLASS_HID) {
                    continue;
                }
                hid.add(intf);
                if (isStrictV2(intf)) {
                    strict.add(intf);
                }
            }
            report.append("strict_candidates=").append(strict.size()).append('\n');
            report.append("hid_interfaces=").append(hid.size()).append('\n');
            // §VITURE：參考的 V2 連線只接受恰好一個嚴格介面；0 個或多個就改逐一試 HID。
            if (strict.size() == 1) {
                Session session = tryClaim(connection, device, strict.get(0), "strict-v2", report);
                if (session != null) {
                    return session;
                }
                report.append("strict_claim_failed fallback=hid\n");
            } else if (strict.size() > 1) {
                report.append("strict_not_unique fallback=hid\n");
            }
            for (int i = 0; i < hid.size(); i++) {
                UsbInterface intf = hid.get(i);
                if (strict.size() == 1 && sameInterface(intf, strict.get(0))) {
                    continue;
                }
                if (intf.getAlternateSetting() != 0) {
                    // §VITURE：參考連線只用 alt 0。沒呼叫 setInterface 就 claim 其他 alt，讀到的不會是目前作用中的設定。
                    report.append("  interface index=").append(interfaceIndex(device, intf))
                            .append(" id=").append(intf.getId())
                            .append(" alt=").append(intf.getAlternateSetting())
                            .append(" skipped=nonzero_alt\n");
                    continue;
                }
                Session session = tryClaim(connection, device, intf, "hid-fallback", report);
                if (session != null) {
                    return session;
                }
            }
            report.append("open=no_claimable_interface\n");
            return null;
        }

        private static Session tryClaim(UsbDeviceConnection connection, UsbDevice device, UsbInterface intf,
                                        String via, StringBuilder report) {
            Pipe pipe = pickPipe(intf);
            report.append("  interface index=").append(interfaceIndex(device, intf))
                    .append(" id=").append(intf.getId())
                    .append(" alt=").append(intf.getAlternateSetting());
            if (pipe == null) {
                report.append(" pipe=none via=").append(via).append('\n');
                return null;
            }
            boolean claimed = false;
            try {
                // §VITURE：force=true 跟參考連線一樣，否則核心驅動佔著 HID 時 claim 會失敗。
                claimed = connection.claimInterface(intf, true);
            } catch (RuntimeException error) {
                report.append(" claim=error via=").append(via)
                        .append(" error=").append(VitureProbe.errorText(error)).append('\n');
                return null;
            }
            if (!claimed) {
                report.append(" claim=failed via=").append(via).append('\n');
                return null;
            }
            report.append(" claim=ok")
                    .append(" in=").append(hexByte(pipe.inEndpoint.getAddress()))
                    .append(" out=").append(hexByte(pipe.outEndpoint.getAddress()))
                    .append(" in_type=").append(endpointType(pipe.inEndpoint))
                    .append(" out_type=").append(endpointType(pipe.outEndpoint))
                    .append(" in_max=").append(pipe.inEndpoint.getMaxPacketSize())
                    .append(" out_max=").append(pipe.outEndpoint.getMaxPacketSize())
                    .append(" via=").append(via)
                    .append(" pipe=").append(pipe.how)
                    .append('\n');
            return new Session(connection, intf, pipe.inEndpoint, pipe.outEndpoint);
        }
    }

    private static boolean isStrictV2(UsbInterface intf) {
        if (intf.getInterfaceClass() != UsbConstants.USB_CLASS_HID || intf.getAlternateSetting() != 0) {
            return false;
        }
        if (intf.getEndpointCount() != 2) {
            return false;
        }
        UsbEndpoint in = null;
        UsbEndpoint out = null;
        for (int i = 0; i < intf.getEndpointCount(); i++) {
            UsbEndpoint endpoint = intf.getEndpoint(i);
            boolean interrupt64 = endpoint.getType() == UsbConstants.USB_ENDPOINT_XFER_INT
                    && endpoint.getMaxPacketSize() == 64;
            if (!interrupt64) {
                return false;
            }
            if (endpoint.getDirection() == UsbConstants.USB_DIR_IN) {
                if (in != null) {
                    return false;
                }
                in = endpoint;
            } else if (endpoint.getDirection() == UsbConstants.USB_DIR_OUT) {
                if (out != null) {
                    return false;
                }
                out = endpoint;
            } else {
                return false;
            }
        }
        return in != null && out != null;
    }

    private static final class Pipe {
        final UsbEndpoint inEndpoint;
        final UsbEndpoint outEndpoint;
        final String how;

        Pipe(UsbEndpoint inEndpoint, UsbEndpoint outEndpoint, String how) {
            this.inEndpoint = inEndpoint;
            this.outEndpoint = outEndpoint;
            this.how = how;
        }
    }

    private static Pipe pickPipe(UsbInterface intf) {
        UsbEndpoint strictIn = single(intf, true, true, 64);
        UsbEndpoint strictOut = single(intf, false, true, 64);
        if (strictIn != null && strictOut != null) {
            return new Pipe(strictIn, strictOut, "int64");
        }
        UsbEndpoint interruptIn = first(intf, true, true, false);
        UsbEndpoint interruptOut = first(intf, false, true, false);
        if (interruptIn != null && interruptOut != null) {
            return new Pipe(interruptIn, interruptOut, "int");
        }
        UsbEndpoint bulkIn = first(intf, true, false, true);
        UsbEndpoint bulkOut = first(intf, false, false, true);
        if (bulkIn != null && bulkOut != null) {
            return new Pipe(bulkIn, bulkOut, "bulk");
        }
        return null;
    }

    private static UsbEndpoint single(UsbInterface intf, boolean in, boolean interrupt, int maxPacket) {
        UsbEndpoint found = null;
        for (int i = 0; i < intf.getEndpointCount(); i++) {
            UsbEndpoint endpoint = intf.getEndpoint(i);
            if (!matches(endpoint, in, interrupt, false) || endpoint.getMaxPacketSize() != maxPacket) {
                continue;
            }
            if (found != null) {
                return null;
            }
            found = endpoint;
        }
        return found;
    }

    private static UsbEndpoint first(UsbInterface intf, boolean in, boolean interrupt, boolean bulk) {
        for (int i = 0; i < intf.getEndpointCount(); i++) {
            UsbEndpoint endpoint = intf.getEndpoint(i);
            if (matches(endpoint, in, interrupt, bulk)) {
                return endpoint;
            }
        }
        return null;
    }

    private static boolean matches(UsbEndpoint endpoint, boolean in, boolean interrupt, boolean bulk) {
        boolean directionOk = in
                ? endpoint.getDirection() == UsbConstants.USB_DIR_IN
                : endpoint.getDirection() == UsbConstants.USB_DIR_OUT;
        if (!directionOk) {
            return false;
        }
        if (interrupt) {
            return endpoint.getType() == UsbConstants.USB_ENDPOINT_XFER_INT;
        }
        if (bulk) {
            return endpoint.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK;
        }
        return false;
    }

    private static boolean sameInterface(UsbInterface a, UsbInterface b) {
        return a.getId() == b.getId() && a.getAlternateSetting() == b.getAlternateSetting();
    }

    private static int interfaceIndex(UsbDevice device, UsbInterface intf) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface candidate = device.getInterface(i);
            if (candidate.getId() == intf.getId()
                    && candidate.getAlternateSetting() == intf.getAlternateSetting()) {
                return i;
            }
        }
        return -1;
    }

    private static void appendDeviceSummary(UsbDevice device, StringBuilder report) {
        String label = knownLabel(device.getVendorId(), device.getProductId());
        report.append("device dev=").append(quoted(device.getDeviceName()))
                .append(" vid=").append(hex16Id(device.getVendorId()))
                .append(" pid=").append(hex16Id(device.getProductId()))
                .append(" manufacturer=").append(usbText(device, true))
                .append(" product=").append(usbText(device, false))
                .append(" class=").append(device.getDeviceClass())
                .append(' ').append(className(device.getDeviceClass()))
                .append(" interfaces=").append(device.getInterfaceCount());
        if (label.length() > 0) {
            report.append(" known=").append(label);
        }
        report.append('\n');
    }

    private static void appendInterfaces(UsbDevice device, StringBuilder report) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface intf = device.getInterface(i);
            String name = intf.getName();
            report.append("  interface index=").append(i)
                    .append(" id=").append(intf.getId())
                    .append(" alt=").append(intf.getAlternateSetting())
                    .append(" class=").append(intf.getInterfaceClass())
                    .append(' ').append(className(intf.getInterfaceClass()))
                    .append(" subclass=").append(intf.getInterfaceSubclass())
                    .append(" protocol=").append(intf.getInterfaceProtocol())
                    .append(" endpoints=").append(intf.getEndpointCount());
            if (name != null && name.length() > 0) {
                report.append(" name=").append(quoted(name));
            }
            report.append('\n');
            for (int e = 0; e < intf.getEndpointCount(); e++) {
                UsbEndpoint endpoint = intf.getEndpoint(e);
                report.append("    endpoint index=").append(e)
                        .append(" addr=").append(hexByte(endpoint.getAddress()))
                        .append(" dir=").append(endpoint.getDirection() == UsbConstants.USB_DIR_IN ? "IN" : "OUT")
                        .append(" type=").append(endpointType(endpoint))
                        .append(" maxPacket=").append(endpoint.getMaxPacketSize())
                        .append('\n');
            }
        }
    }

    private static String usbText(UsbDevice device, boolean manufacturer) {
        try {
            return quoted(manufacturer ? device.getManufacturerName() : device.getProductName());
        } catch (SecurityException error) {
            return "\"(denied)\"";
        }
    }

    static String quoted(String value) {
        if (value == null) {
            return "\"\"";
        }
        return "\"" + value.replace('\n', ' ').replace('\r', ' ').replace('"', '\'') + "\"";
    }

    private static String className(int usbClass) {
        switch (usbClass) {
            case UsbConstants.USB_CLASS_PER_INTERFACE:
                return "PER_INTERFACE";
            case UsbConstants.USB_CLASS_AUDIO:
                return "AUDIO";
            case UsbConstants.USB_CLASS_COMM:
                return "COMM";
            case UsbConstants.USB_CLASS_HID:
                return "HID";
            case UsbConstants.USB_CLASS_MASS_STORAGE:
                return "MASS_STORAGE";
            case UsbConstants.USB_CLASS_HUB:
                return "HUB";
            case UsbConstants.USB_CLASS_CDC_DATA:
                return "CDC_DATA";
            case UsbConstants.USB_CLASS_VENDOR_SPEC:
                return "VENDOR";
            case 14:
                return "VIDEO";
            default:
                return "CLASS_" + usbClass;
        }
    }

    private static String endpointType(UsbEndpoint endpoint) {
        switch (endpoint.getType()) {
            case UsbConstants.USB_ENDPOINT_XFER_CONTROL:
                return "CONTROL";
            case UsbConstants.USB_ENDPOINT_XFER_ISOC:
                return "ISOC";
            case UsbConstants.USB_ENDPOINT_XFER_BULK:
                return "BULK";
            case UsbConstants.USB_ENDPOINT_XFER_INT:
                return "INT";
            default:
                return "TYPE_" + endpoint.getType();
        }
    }

    private static String hex16Id(int value) {
        return VitureV2Protocol.hex16(value);
    }

    private static String hexByte(int value) {
        int byteValue = value & 0xFF;
        return "0x" + VitureV2Protocol.toHex(new byte[] {(byte) byteValue});
    }

    private static void sortDevices(List<UsbDevice> devices) {
        Collections.sort(devices, new Comparator<UsbDevice>() {
            @Override
            public int compare(UsbDevice a, UsbDevice b) {
                int vendor = a.getVendorId() - b.getVendorId();
                if (vendor != 0) {
                    return vendor;
                }
                int product = a.getProductId() - b.getProductId();
                if (product != 0) {
                    return product;
                }
                String aName = a.getDeviceName() == null ? "" : a.getDeviceName();
                String bName = b.getDeviceName() == null ? "" : b.getDeviceName();
                return aName.compareTo(bName);
            }
        });
    }

    /**
     * §VITURE：Beast 的 HID 在 interface 5（0x85／0x04），Pro 2 是單一介面（0x81／0x01）。
     * 寫死介面號會在另一個 PID 上開錯端點，所以只接受 class 3、alt 0、同時有 interrupt IN 與 OUT。
     * 有 64 B 的那組優先；否則用最小的 interface index。
     */
    public static HidEndpoints resolveHidEndpoints(UsbDevice device) {
        if (device == null) {
            return null;
        }
        HidChoice best = null;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            HidChoice choice = inspectHid(device, i);
            if (choice == null) {
                continue;
            }
            if (best == null) {
                best = choice;
                continue;
            }
            if (choice.packet64 && !best.packet64) {
                best = choice;
                continue;
            }
            if (choice.packet64 == best.packet64 && choice.interfaceIndex < best.interfaceIndex) {
                best = choice;
            }
        }
        if (best == null) {
            return null;
        }
        return new HidEndpoints(best.usbInterface, best.inEndpoint, best.outEndpoint,
                best.interfaceNumber, best.interfaceIndex, best.inEndpoint.getMaxPacketSize());
    }

    public static int countHidCandidates(UsbDevice device) {
        if (device == null) {
            return 0;
        }
        int count = 0;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            if (inspectHid(device, i) != null) {
                count++;
            }
        }
        return count;
    }

    /**
     * §VITURE：功能路徑只接受已知眼鏡。0x1201、0x1211、0x1221 是 Beast 系列，0x1301 是 Pro 2。
     * 同一個 VID 的轉接器或底座也可能有雙向 interrupt，未知 PID 若排進去會被 claim。
     * 診斷仍用 {@link #findDevices}，VID 相符的裝置全部列出來探測。
     */
    public static UsbDevice findGlassesDevice(UsbManager usb) {
        UsbDevice best = null;
        int bestRank = Integer.MAX_VALUE;
        for (UsbDevice device : findDevices(usb)) {
            // 同一個 VID 底下的麥克風或音訊介面沒有雙向 interrupt，開了會在 claim 時失敗。
            if (resolveHidEndpoints(device) == null) {
                continue;
            }
            int rank = glassesRank(device.getProductId());
            if (rank < bestRank) {
                best = device;
                bestRank = rank;
            }
        }
        return best;
    }

    public static final class HidEndpoints {
        public final int interfaceNumber;
        public final int interfaceIndex;
        public final int inAddress;
        public final int outAddress;
        /** §VITURE：讀取佇列看的是 IN 的 max packet。OUT 填滿時另讀 OUT 端點自己的長度。 */
        public final int maxPacketSize;
        final UsbInterface usbInterface;
        final UsbEndpoint inEndpoint;
        final UsbEndpoint outEndpoint;

        HidEndpoints(UsbInterface usbInterface, UsbEndpoint inEndpoint, UsbEndpoint outEndpoint,
                     int interfaceNumber, int interfaceIndex, int maxPacketSize) {
            this.usbInterface = usbInterface;
            this.inEndpoint = inEndpoint;
            this.outEndpoint = outEndpoint;
            this.interfaceNumber = interfaceNumber;
            this.interfaceIndex = interfaceIndex;
            this.inAddress = inEndpoint.getAddress();
            this.outAddress = outEndpoint.getAddress();
            this.maxPacketSize = maxPacketSize;
        }
    }

    private static final class HidChoice {
        final UsbInterface usbInterface;
        final UsbEndpoint inEndpoint;
        final UsbEndpoint outEndpoint;
        final int interfaceNumber;
        final int interfaceIndex;
        final boolean packet64;

        HidChoice(UsbInterface usbInterface, UsbEndpoint inEndpoint, UsbEndpoint outEndpoint,
                  int interfaceNumber, int interfaceIndex, boolean packet64) {
            this.usbInterface = usbInterface;
            this.inEndpoint = inEndpoint;
            this.outEndpoint = outEndpoint;
            this.interfaceNumber = interfaceNumber;
            this.interfaceIndex = interfaceIndex;
            this.packet64 = packet64;
        }
    }

    private static HidChoice inspectHid(UsbDevice device, int index) {
        UsbInterface intf = device.getInterface(index);
        if (intf.getInterfaceClass() != UsbConstants.USB_CLASS_HID || intf.getAlternateSetting() != 0) {
            // §VITURE：非 0 的 alt 不是目前作用中的設定，claim 到的不會是現在在跑的那組端點。
            return null;
        }
        UsbEndpoint in = null;
        UsbEndpoint out = null;
        for (int e = 0; e < intf.getEndpointCount(); e++) {
            UsbEndpoint endpoint = intf.getEndpoint(e);
            if (endpoint.getType() != UsbConstants.USB_ENDPOINT_XFER_INT) {
                continue;
            }
            if (endpoint.getDirection() == UsbConstants.USB_DIR_IN) {
                if (in == null) {
                    in = endpoint;
                }
            } else if (endpoint.getDirection() == UsbConstants.USB_DIR_OUT) {
                if (out == null) {
                    out = endpoint;
                }
            }
        }
        if (in == null || out == null) {
            return null;
        }
        boolean packet64 = in.getMaxPacketSize() == 64 && out.getMaxPacketSize() == 64;
        return new HidChoice(intf, in, out, intf.getId(), index, packet64);
    }

    private static int glassesRank(int productId) {
        switch (productId) {
            case PID_BEAST:
                return 0;
            case PID_BEAST_ALT:
                return 1;
            case PID_PHANTOM_BEAST:
                return 2;
            case PID_PRO_2:
                return 3;
            case PID_REPORTED_MICROPHONE:
            default:
                // 麥克風與未知 PID 都不是眼鏡。MAX_VALUE 讓呼叫端的比較選不中。
                return Integer.MAX_VALUE;
        }
    }

    private static void appendHidResolve(UsbDevice device, StringBuilder report) {
        int candidates = countHidCandidates(device);
        HidEndpoints hid = resolveHidEndpoints(device);
        if (hid == null) {
            report.append("hid_resolve none candidates=").append(candidates).append('\n');
            return;
        }
        report.append("hid_resolve interface=").append(hid.interfaceNumber)
                .append(" index=").append(hid.interfaceIndex)
                .append(" in=").append(hexByte(hid.inAddress))
                .append(" out=").append(hexByte(hid.outAddress))
                .append(" maxPacket=").append(hid.maxPacketSize)
                .append(" candidates=").append(candidates)
                .append('\n');
    }

    static void checkNotMainThread() {
        Looper main = Looper.getMainLooper();
        if (main != null && Looper.myLooper() == main) {
            throw new IllegalStateException("USB I/O must not run on the main thread");
        }
    }
}
