package com.limelight.binding.input.driver;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.os.SystemClock;

import com.limelight.LimeLog;
import com.limelight.nvstream.jni.MoonBridge;

import java.util.ArrayList;
import java.util.Arrays;

// §SC-HID: Steam Controller (gen-2) raw HID passthrough driver.
//
// Unlike the Xbox drivers, this driver does NOT parse reports into button/axis
// state for UsbDriverListener. It forwards the controller's vendor HID input
// reports verbatim to the host via MoonBridge.sendScHidInputReport(), where
// Sunshine replays them on a virtual Steam Controller device. It also services
// the host's feature report tunnel (Steam's GetControllerInfo handshake) by
// replaying HID GET/SET_REPORT control transfers against the real controller.
// Semantics are aligned with moonlight-qt's app/streaming/input/sc_hid.cpp.
//
// Supported transports: USB-direct (PID 0x1302) and the Puck wireless receiver
// (PID 0x1304, a composite device whose pairing slots are separate HID
// interfaces). Bluetooth LE (PID 0x1303) 不走 USB 堆疊，這輪不處理——BLE 需要
// 另走 BluetoothGatt 的路徑，且不會經過 UsbDriverService。
public class SteamControllerDriver implements MoonBridge.ScHidFeatureRequestHandler {

    // Notifies the owner (UsbDriverService) when this driver stops, so it can
    // be dropped from the service's driver list.
    public interface Listener {
        void onStopped(SteamControllerDriver driver);
    }

    private static final int VALVE_VID = 0x28de;
    private static final int SC_GEN2_PID_USB = 0x1302;  // USB-direct
    private static final int SC_GEN2_PID_PUCK = 0x1304; // Puck wireless receiver

    // Fixed wire report size (SS_SC_HID_REPORT_MAX in moonlight-common-c).
    // LiSendScHid*Report only accepts exactly this many bytes.
    private static final int SC_HID_WIRE_BYTES = 64;

    // gen-2 SC 的 vendor gamepad 串流 report id（server 端虛擬裝置的 report
    // descriptor 只宣告這個 input id；其他 id 是 puck 無線狀態等雜訊，不轉發）
    private static final int SC_REPORT_ID_GAMEPAD = 0x45;

    // HID class control requests (HID 1.11 spec, section 7.2)
    private static final int HID_REQUEST_GET_REPORT = 0x01;
    private static final int HID_REQUEST_SET_REPORT = 0x09;
    private static final int HID_REPORT_TYPE_FEATURE = 0x03 << 8; // wValue high byte

    private static final int CONTROL_TIMEOUT_MS = 1000;
    private static final int READ_TIMEOUT_MS = 3000;

    private final UsbDevice device;
    private final UsbDeviceConnection connection;
    private final Listener listener;

    // Claimed HID interfaces (feature transfers target each in turn) and the
    // interrupt IN endpoints polled by the read threads. Both are populated in
    // start() before any thread is spawned and never mutated afterwards.
    private final ArrayList<UsbInterface> claimedIfaces = new ArrayList<>();
    private final ArrayList<UsbEndpoint> readEndpoints = new ArrayList<>();
    private final ArrayList<Thread> readThreads = new ArrayList<>();

    // Guards feature control transfers against stop() closing the connection
    // out from under them. Read threads intentionally don't take this lock:
    // they block in bulkTransfer() and are woken by connection.close(), same
    // shutdown pattern as AbstractXboxController.
    private final Object featureLock = new Object();
    private volatile boolean stopped;

    // One-shot diagnostic log per skipped report id (mirrors sc_hid.cpp).
    // Races between read threads are benign (worst case a duplicate log line).
    private final boolean[] seenReportIds = new boolean[256];

    public static boolean canClaimDevice(UsbDevice device) {
        return device.getVendorId() == VALVE_VID &&
                (device.getProductId() == SC_GEN2_PID_USB ||
                 device.getProductId() == SC_GEN2_PID_PUCK);
    }

    public SteamControllerDriver(UsbDevice device, UsbDeviceConnection connection, Listener listener) {
        this.device = device;
        this.connection = connection;
        this.listener = listener;
    }

    public boolean start() {
        // Claim every HID-class interface. The gamepad stream lives on a
        // vendor-usage HID interface; on the Puck each pairing slot has its own
        // interface (MI_02~05) and only the active one delivers reports, so we
        // claim and poll them all — same strategy as moonlight-qt's sc_hid.cpp.
        // (Android's UsbInterface doesn't expose HID usage pages, so we can't
        // pre-filter to UsagePage 0xFF00 like the hidapi-based Qt client does;
        // the report id filter in the read loop covers that instead.)
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface iface = device.getInterface(i);
            if (iface.getInterfaceClass() != UsbConstants.USB_CLASS_HID) {
                continue;
            }

            if (!connection.claimInterface(iface, true)) {
                LimeLog.warning("[SC-HID] Failed to claim interface "+iface.getId());
                continue;
            }

            claimedIfaces.add(iface);

            for (int j = 0; j < iface.getEndpointCount(); j++) {
                UsbEndpoint endpt = iface.getEndpoint(j);
                if (endpt.getDirection() == UsbConstants.USB_DIR_IN &&
                        endpt.getType() == UsbConstants.USB_ENDPOINT_XFER_INT) {
                    readEndpoints.add(endpt);
                    break;
                }
            }
        }

        if (claimedIfaces.isEmpty() || readEndpoints.isEmpty()) {
            LimeLog.warning("[SC-HID] No usable HID interface on "+device.getDeviceName());
            return false;
        }

        // Register for the feature tunnel before priming, so a request arriving
        // mid-start can't slip past us.
        MoonBridge.setScHidFeatureRequestHandler(this);

        // §SC-HID Phase 2C 暖機（對齊 Qt 端）：主動 GET feature 0x01 一次回送
        // host，讓 server 在 Steam 第一次查 GetControllerInfo 前就有真實 SC 的
        // firmware 資料。best-effort：串流連線還沒起來時 LiSend* 只會回錯誤。
        primeFeatureCache();

        // One read thread per claimed interrupt IN endpoint
        for (int i = 0; i < readEndpoints.size(); i++) {
            Thread thread = createReadThread(readEndpoints.get(i), i);
            readThreads.add(thread);
            thread.start();
        }

        LimeLog.info("[SC-HID] Steam Controller passthrough started ("+claimedIfaces.size()+
                " HID interface(s), "+readThreads.size()+" input endpoint(s))");
        return true;
    }

    public void stop() {
        synchronized (featureLock) {
            if (stopped) {
                return;
            }
            stopped = true;

            MoonBridge.clearScHidFeatureRequestHandler(this);

            // Closing the connection wakes any read thread blocked in
            // bulkTransfer() (same shutdown pattern as AbstractXboxController).
            connection.close();
        }

        for (Thread thread : readThreads) {
            thread.interrupt();
        }

        if (listener != null) {
            listener.onStopped(this);
        }

        LimeLog.info("[SC-HID] Steam Controller passthrough stopped");
    }

    private Thread createReadThread(final UsbEndpoint endpoint, final int endpointIndex) {
        return new Thread("SC-HID input "+endpointIndex) {
            @Override
            public void run() {
                byte[] buffer = new byte[SC_HID_WIRE_BYTES];

                while (!isInterrupted() && !stopped) {
                    long lastMillis = SystemClock.uptimeMillis();
                    int res = connection.bulkTransfer(endpoint, buffer, buffer.length, READ_TIMEOUT_MS);
                    if (res <= 0) {
                        // Timeouts are normal on idle Puck slot interfaces.
                        // Failing long before the timeout expired means the
                        // device went away (same heuristic as the Xbox driver).
                        if (SystemClock.uptimeMillis() - lastMillis < 1000) {
                            LimeLog.warning("[SC-HID] Detected device I/O error (endpoint "+
                                    endpointIndex+")");
                            SteamControllerDriver.this.stop();
                            break;
                        }
                        continue;
                    }

                    // Only forward the gamepad stream (report id 0x45). Idle
                    // Puck slot interfaces emit wireless status packets on
                    // other report ids; the host's virtual device only declares
                    // input 0x45, so forwarding noise would corrupt Steam's
                    // view of it. Log each skipped id once for diagnostics.
                    int reportId = buffer[0] & 0xFF;
                    if (reportId != SC_REPORT_ID_GAMEPAD) {
                        if (!seenReportIds[reportId]) {
                            seenReportIds[reportId] = true;
                            LimeLog.info("[SC-HID] Skipping non-gamepad report id=0x"+
                                    Integer.toHexString(reportId)+" (endpoint="+endpointIndex+
                                    ", "+res+" bytes) - not forwarding");
                        }
                        continue;
                    }

                    // Zero-pad short reads to exactly the fixed wire report size
                    if (res < SC_HID_WIRE_BYTES) {
                        Arrays.fill(buffer, res, SC_HID_WIRE_BYTES, (byte) 0);
                    }
                    MoonBridge.sendScHidInputReport(buffer);
                }
            }
        };
    }

    // §SC-HID Phase 2C warm-up: read feature 0x01 from the first interface that
    // answers and push it to the host proactively (seq=0). Even if the physical
    // SC doesn't answer a cold GET_FEATURE(0x01) this is best-effort — the
    // proxy's normal SET+GET round-trips will fill the cache within a few
    // hundred ms anyway.
    private void primeFeatureCache() {
        synchronized (featureLock) {
            if (stopped) {
                return;
            }

            byte[] buf = new byte[SC_HID_WIRE_BYTES];
            for (UsbInterface iface : claimedIfaces) {
                Arrays.fill(buf, (byte) 0);
                buf[0] = 0x01; // feature report id
                int res = getFeatureReport(iface, (byte) 0x01, buf);
                if (res > 0) {
                    MoonBridge.sendScHidFeatureReport((byte) 0, (byte) 0x01, buf);
                    LimeLog.info("[SC-HID] Proactive cache prime sent (iface="+iface.getId()+
                            ", "+res+" bytes)");
                    break;
                }
            }
        }
    }

    // May be invoked on an arbitrary native callback thread (common-c control
    // stream). featureLock serializes it against primeFeatureCache() and stop(),
    // so the connection can't be closed mid-transfer. Holding the lock through
    // the 20 ms SET settle delay is fine: feature ops only occur during Steam's
    // handshake (low rate) and input reads don't take this lock.
    @Override
    public void onScHidFeatureRequest(byte reportId, byte op, byte seq, byte[] query, byte queryLen) {
        synchronized (featureLock) {
            if (stopped) {
                LimeLog.warning("[SC-HID] Feature request after stop; dropping");
                return;
            }

            byte[] buf = new byte[SC_HID_WIRE_BYTES];

            // §SC-HID Phase 2C — transparent proxy, aligned with moonlight-qt's
            // forwardFeatureRequest(). We replay EXACTLY what Steam did on the
            // host against the real SC:
            //   op==2 (SET): write Steam's query via SET_REPORT, wait 20 ms,
            //                then read the response via GET_REPORT.
            //   op==1 (GET): just read via GET_REPORT.
            // The first interface that yields a response wins; its bytes go
            // back tagged with the same seq.
            for (UsbInterface iface : claimedIfaces) {
                if (op == 2 /* SET */) {
                    byte[] qbuf = new byte[SC_HID_WIRE_BYTES];
                    if (query != null) {
                        int qn = Math.min(queryLen & 0xFF, Math.min(query.length, SC_HID_WIRE_BYTES));
                        System.arraycopy(query, 0, qbuf, 0, qn);
                    }
                    qbuf[0] = reportId; // ensure report id is correct in byte 0

                    // bmRequestType 0x21 = host-to-device | class | interface
                    connection.controlTransfer(0x21, HID_REQUEST_SET_REPORT,
                            HID_REPORT_TYPE_FEATURE | (reportId & 0xFF), iface.getId(),
                            qbuf, SC_HID_WIRE_BYTES, CONTROL_TIMEOUT_MS);

                    // Let the SC process the query before reading back
                    try {
                        Thread.sleep(20);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }

                Arrays.fill(buf, (byte) 0);
                buf[0] = reportId;
                int res = getFeatureReport(iface, reportId, buf);
                if (res > 0) {
                    // controlTransfer only wrote 'res' bytes; the rest of the
                    // buffer is still zero, so it's already padded to 64.
                    MoonBridge.sendScHidFeatureReport(seq, reportId, buf);
                    return; // first successful response is enough
                }
            }

            // No interface produced a response. Still answer the host (zeros)
            // so its seq round-trip completes and Steam's GET_FEATURE doesn't
            // hang on a retry.
            Arrays.fill(buf, (byte) 0);
            buf[0] = reportId;
            MoonBridge.sendScHidFeatureReport(seq, reportId, buf);
            LimeLog.warning("[SC-HID] Feature GET 0x"+Integer.toHexString(reportId & 0xFF)+
                    " empty on all "+claimedIfaces.size()+" interface(s) (op="+op+
                    " seq="+(seq & 0xFF)+")");
        }
    }

    // HID GET_REPORT(Feature) on the control pipe. Like hidapi's libusb backend
    // (which the Qt client uses on Linux), the device's response lands at
    // buf[0] with the report id prefix included. Returns bytes read or <0.
    private int getFeatureReport(UsbInterface iface, byte reportId, byte[] buf) {
        // bmRequestType 0xA1 = device-to-host | class | interface
        return connection.controlTransfer(0xA1, HID_REQUEST_GET_REPORT,
                HID_REPORT_TYPE_FEATURE | (reportId & 0xFF), iface.getId(),
                buf, SC_HID_WIRE_BYTES, CONTROL_TIMEOUT_MS);
    }
}
