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
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

// §SC-HID: Steam Controller (gen-2) raw HID passthrough driver.
//
// Unlike the Xbox drivers, this driver does NOT parse reports into button/axis
// state for UsbDriverListener. It forwards the controller's vendor HID input
// reports verbatim to the host via MoonBridge.sendScHidInputReport(), where
// Sunshine replays them on a virtual Steam Controller device. It also services
// the host's feature report tunnel (Steam's GetControllerInfo handshake) by
// replaying HID GET/SET_REPORT control transfers against the real controller.
// Semantics are aligned with moonlight-qt's app/streaming/input/sc_hid.cpp
// (Round 1, 2026-09).
//
// Supported transports: USB-direct (PID 0x1302) and the Puck / Nereid wireless
// receivers (PID 0x1304 / 0x1305 — composite devices whose pairing slots are
// separate HID interfaces), both via USB host (OTG).
//
// Bluetooth LE (PID 0x1303) 在 Android 上**無法**由一般 App 原生轉發：gen-2 SC 的
// BLE 是標準 HID over GATT，Android 的 Bluetooth 堆疊（GattService）把 HID 服務
// 0x1812 的 Report 特徵列為受限、非系統 App 讀寫會被拒；而它的 HID 描述符只有
// 滑鼠＋鍵盤＋vendor collection、沒有 gamepad，Android 也不會把它當手把。
// Game.java 在串流開始時偵測到 0x28DE:0x1303 的藍牙 InputDevice 會以 toast 提示
// 改用 USB-C 或接收器（OTG）。
public class SteamControllerDriver implements MoonBridge.ScHidFeatureRequestHandler {

    // Notifies the owner (UsbDriverService) when this driver stops, so it can
    // be dropped from the service's driver list.
    public interface Listener {
        void onStopped(SteamControllerDriver driver);
    }

    private static final int VALVE_VID = 0x28de;
    private static final int SC_GEN2_PID_USB = 0x1302;    // USB-direct
    private static final int SC_GEN2_PID_PUCK = 0x1304;   // Puck wireless receiver (Proteus)
    private static final int SC_GEN2_PID_NEREID = 0x1305; // Nereid wireless receiver

    // Fixed wire report size (SS_SC_HID_REPORT_MAX in moonlight-common-c).
    // LiSendScHid*Report only accepts exactly this many bytes.
    private static final int SC_HID_WIRE_BYTES = 64;

    // Valve Triton report id 家族（SDL controller_structs.h ETritonReportIDTypes）。
    // 實測：USB 直連與 Puck 吐 0x42（ID_TRITON_CONTROLLER_STATE），藍牙才用 0x45
    // （..._STATE_BLE）；兩者 payload 同為 TritonMTUNoQuat_t，可直接改標。0x47
    // （..._STATE_TIMESTAMP）佈局不同，不得改標；0x43 電量、0x44、0x7B 遙測、
    // 0x46/0x79 無線狀態一律不轉發（host 虛擬裝置是 0x1302 有線身分）。
    private static final int SC_RID_STATE = 0x42;
    private static final int SC_RID_BATTERY = 0x43;
    private static final int SC_RID_STATE_BLE = 0x45;
    private static final int SC_RID_WIRELESS_X = 0x46;
    private static final int SC_RID_STATE_TS = 0x47;
    private static final int SC_RID_WIRELESS = 0x79;
    private static final int SC_RID_TELEMETRY = 0x7B;

    // host driver 1.0.5.0 也宣告了 0x42 通道；預設仍改標成 0x45（與 Qt 端 kNormalize42
    // 一致），若 host Steam 對改標資料解析異常，關掉即原樣透傳。
    private static final boolean NORMALIZE_42 = true;

    // Valve feature report（id 0x01）訊息 type：SET query byte1 / 回應 byte1。
    private static final int SC_FEATURE_RID = 0x01;
    private static final int SC_MSG_GET_ATTRIBUTES_VALUES = 0x83;

    // HID class control requests (HID 1.11 spec, section 7.2)
    private static final int HID_REQUEST_GET_REPORT = 0x01;
    private static final int HID_REQUEST_SET_REPORT = 0x09;
    private static final int HID_REPORT_TYPE_FEATURE = 0x03 << 8; // wValue high byte

    // feature 時序（對齊 Qt 端；實測實體 SC 的回應是一次性暫存器，SET 後 13~21 ms
    // 才出現、被讀走前 GET 會 STALL；host driver 的閘控逾時是 400 ms）：
    private static final int FEAT_SET_POLL_MS = 100;    // SET 後輪詢 GET 的總預算
    private static final int FEAT_GET_POLL_MS = 10;     // GET op（無 SET）輪詢預算
    private static final int FEAT_GET_WINDOW_MS = 400;  // 距最近一次 SET 多久內 GET 才輪詢
    private static final int FEAT_WARMUP_BUDGET_MS = 60;
    private static final int FEAT_CACHE_TTL_MS = 5000;
    private static final int FEAT_POLL_STEP_MS = 1;
    private static final int CONTROL_XFER_TIMEOUT_MS = 50; // 單次 control transfer 上限
    private static final int READ_TIMEOUT_MS = 3000;
    private static final int STATS_PERIOD_MS = 5000;
    private static final int STATS_HEARTBEAT_MS = 60000;

    private final UsbDevice device;
    private final UsbDeviceConnection connection;
    private final Listener listener;

    // Claimed HID interfaces (feature transfers target each in turn) and the
    // interrupt IN endpoints polled by the read threads. Both are populated in
    // start() before any thread is spawned and never mutated afterwards.
    private final ArrayList<UsbInterface> claimedIfaces = new ArrayList<>();
    private final ArrayList<UsbEndpoint> readEndpoints = new ArrayList<>();
    private final ArrayList<Integer> readEndpointIface = new ArrayList<>(); // endpoint idx → claimedIfaces idx
    private final ArrayList<Thread> readThreads = new ArrayList<>();

    // Guards feature control transfers against stop() closing the connection
    // out from under them. Read threads intentionally don't take this lock:
    // they block in bulkTransfer() and are woken by connection.close(), same
    // shutdown pattern as AbstractXboxController.
    private final Object featureLock = new Object();
    private volatile boolean stopped;

    // 最近吐出 state report 的介面（claimedIfaces 索引；-1 = 未知）。Puck 有多個 slot
    // 介面，feature 查詢先送這個，避免對閒置 slot 空等。
    private final AtomicInteger activeIface = new AtomicInteger(-1);

    // ---- rx 統計（read threads 寫、stats/stop 讀；跨執行緒用 atomic）----
    private final AtomicLong rxTotal = new AtomicLong();
    private final AtomicLong rxFwd = new AtomicLong();
    private final AtomicLong rxNorm42 = new AtomicLong();
    private final AtomicLong rxDrop = new AtomicLong();
    private final AtomicLong rxSendErr = new AtomicLong();
    private final AtomicLong[] rxById = new AtomicLong[256];
    private final AtomicLong[] rxByEndpoint;
    private final AtomicLong[] fwdByEndpoint;
    private final boolean[] seenReportIds = new boolean[256]; // one-shot log per id（race 只會多印一行）
    private final long startMillis = SystemClock.uptimeMillis();
    private final AtomicLong lastStatsMillis = new AtomicLong(0);
    private volatile String lastStatsLine = "";
    private volatile boolean noReportWarned;

    // ---- feature 統計與快取（只在 featureLock 內存取）----
    private int featReq, featOk, featStolen, featEmpty, featCache;
    private long featLatSumMs, featLatMaxMs;
    private int lastSetType;              // 最近一次 SET 的 Valve 訊息 type（0 = 尚無）
    private long lastSetMillis;
    private boolean cacheValid;
    private int cacheType;
    private long cacheMillis;
    private final byte[] cacheQuery = new byte[SC_HID_WIRE_BYTES];
    private final byte[] cacheResp = new byte[SC_HID_WIRE_BYTES];

    public static boolean canClaimDevice(UsbDevice device) {
        if (device.getVendorId() != VALVE_VID) {
            return false;
        }
        int pid = device.getProductId();
        return pid == SC_GEN2_PID_USB || pid == SC_GEN2_PID_PUCK || pid == SC_GEN2_PID_NEREID;
    }

    public SteamControllerDriver(UsbDevice device, UsbDeviceConnection connection, Listener listener) {
        this.device = device;
        this.connection = connection;
        this.listener = listener;
        for (int i = 0; i < rxById.length; i++) {
            rxById[i] = new AtomicLong();
        }
        // 上限 = 介面數（每介面最多一個 IN endpoint）
        int maxEp = Math.max(1, device.getInterfaceCount());
        rxByEndpoint = new AtomicLong[maxEp];
        fwdByEndpoint = new AtomicLong[maxEp];
        for (int i = 0; i < maxEp; i++) {
            rxByEndpoint[i] = new AtomicLong();
            fwdByEndpoint[i] = new AtomicLong();
        }
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

            int ifaceIdx = claimedIfaces.size();
            claimedIfaces.add(iface);

            for (int j = 0; j < iface.getEndpointCount(); j++) {
                UsbEndpoint endpt = iface.getEndpoint(j);
                if (endpt.getDirection() == UsbConstants.USB_DIR_IN &&
                        endpt.getType() == UsbConstants.USB_ENDPOINT_XFER_INT) {
                    readEndpoints.add(endpt);
                    readEndpointIface.add(ifaceIdx);
                    break;
                }
            }
            LimeLog.info("[SC-HID] Opened iface="+iface.getId()+" pid=0x"+
                    Integer.toHexString(device.getProductId())+" endpoints="+iface.getEndpointCount());
        }

        if (claimedIfaces.isEmpty() || readEndpoints.isEmpty()) {
            LimeLog.warning("[SC-HID] No usable HID interface on "+device.getDeviceName());
            return false;
        }

        // Register for the feature tunnel before priming, so a request arriving
        // mid-start can't slip past us.
        MoonBridge.setScHidFeatureRequestHandler(this);

        // 暖機（對齊 Qt 端）：以真查詢 SET 0x83（GET_ATTRIBUTES_VALUES）換取實體 SC 的
        // 屬性回應並主動送 host（seq=0），讓 server 在 Steam 第一次查
        // GetControllerInfo 前就有真實資料。cold GET 對實體 SC 必失（一次性暫存器），
        // 舊版就是這樣從未成功。失敗不回零、只印 warning。
        primeFeatureCache();

        // One read thread per claimed interrupt IN endpoint
        for (int i = 0; i < readEndpoints.size(); i++) {
            Thread thread = createReadThread(readEndpoints.get(i), i, readEndpointIface.get(i));
            readThreads.add(thread);
            thread.start();
        }

        LimeLog.info("[SC-HID] Steam Controller passthrough started ("+claimedIfaces.size()+
                " HID interface(s), "+readThreads.size()+" input endpoint(s), normalize42="+
                (NORMALIZE_42 ? 1 : 0)+")");
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

        logRxStats("final");

        if (listener != null) {
            listener.onStopped(this);
        }

        LimeLog.info("[SC-HID] Steam Controller passthrough stopped");
    }

    private Thread createReadThread(final UsbEndpoint endpoint, final int endpointIndex, final int ifaceIndex) {
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
                        maybeLogStats();
                        continue;
                    }

                    handleInputReport(buffer, res, endpointIndex, ifaceIndex);
                    maybeLogStats();
                }
            }
        };
    }

    private void handleInputReport(byte[] buffer, int n, int endpointIndex, int ifaceIndex) {
        int reportId = buffer[0] & 0xFF;
        rxTotal.incrementAndGet();
        rxById[reportId].incrementAndGet();
        if (endpointIndex < rxByEndpoint.length) {
            rxByEndpoint[endpointIndex].incrementAndGet();
        }

        // 首見 id 印全部 bytes（判斷佈局／哪個 slot 活躍的唯一現場證據）
        if (!seenReportIds[reportId]) {
            seenReportIds[reportId] = true;
            LimeLog.info("[SC-HID] First report id=0x"+Integer.toHexString(reportId)+
                    " on endpoint="+endpointIndex+" iface="+ifaceIndex+" ("+n+" bytes): "+hex(buffer, n));
        }

        boolean forward;
        switch (reportId) {
            case SC_RID_STATE_BLE:
                forward = true;
                break;
            case SC_RID_STATE:
                if (NORMALIZE_42) {
                    buffer[0] = (byte) SC_RID_STATE_BLE;
                    rxNorm42.incrementAndGet();
                }
                forward = true;
                break;
            case SC_RID_STATE_TS:
                // 佈局不同（多 trackpad timestamp、IMU timestamp 16-bit），不能改標；
                // host driver 也未宣告 0x47。實測 USB/Puck/BT 都沒看過，先丟棄並警告。
                forward = false;
                break;
            case SC_RID_WIRELESS:
            case SC_RID_WIRELESS_X:
                // 無線狀態（byte1: 1=disconnect 2=connect）只記 log，不送 host。
                if (n >= 2) {
                    LimeLog.info("[SC-HID] Wireless status id=0x"+Integer.toHexString(reportId)+
                            " state="+(buffer[1] & 0xFF)+" (iface="+ifaceIndex+")");
                }
                forward = false;
                break;
            default:
                // 0x43 電量、0x44、0x7B 遙測、0x40/0x41 滑鼠鍵盤 collection、其他
                forward = false;
                break;
        }

        if (!forward) {
            rxDrop.incrementAndGet();
            return;
        }

        activeIface.set(ifaceIndex);

        // Zero-pad short reads to exactly the fixed wire report size
        if (n < SC_HID_WIRE_BYTES) {
            Arrays.fill(buffer, n, SC_HID_WIRE_BYTES, (byte) 0);
        }
        int err = MoonBridge.sendScHidInputReport(buffer);
        if (err != 0) {
            long c = rxSendErr.incrementAndGet();
            if (c == 1 || (c % 1000) == 0) {
                LimeLog.warning("[SC-HID] sendScHidInputReport failed err="+err+" (count="+c+")");
            }
            return;
        }
        rxFwd.incrementAndGet();
        if (endpointIndex < fwdByEndpoint.length) {
            fwdByEndpoint[endpointIndex].incrementAndGet();
        }
    }

    // 每 5 s 有變才印一行；60 s 無變印心跳；啟動 3 s 仍零封包警告一次。
    private void maybeLogStats() {
        long now = SystemClock.uptimeMillis();
        if (!noReportWarned && rxTotal.get() == 0 && now - startMillis > 3000) {
            noReportWarned = true;
            LimeLog.warning("[SC-HID] No input report from any of "+readEndpoints.size()+
                    " endpoint(s) in 3 s - controller asleep (press the Steam button) or not paired to this receiver");
        }
        long last = lastStatsMillis.get();
        if (now - last < STATS_PERIOD_MS) {
            return;
        }
        if (!lastStatsMillis.compareAndSet(last, now)) {
            return; // 另一條 read thread 搶到了
        }
        String line = buildStatsLine();
        if (!line.equals(lastStatsLine)) {
            lastStatsLine = line;
            LimeLog.info("[SC-HID] rx stats(5s): "+line);
        }
        else if (now - last >= STATS_HEARTBEAT_MS) {
            LimeLog.info("[SC-HID] rx stats(heartbeat): "+line);
        }
    }

    private void logRxStats(String tag) {
        LimeLog.info("[SC-HID] rx stats("+tag+"): "+buildStatsLine());
    }

    private String buildStatsLine() {
        StringBuilder sb = new StringBuilder(256);
        sb.append("total=").append(rxTotal.get())
          .append(" fwd=").append(rxFwd.get())
          .append(" norm42=").append(rxNorm42.get())
          .append(" drop=").append(rxDrop.get())
          .append(" sendErr=").append(rxSendErr.get())
          .append(" | id42=").append(rxById[SC_RID_STATE].get())
          .append(" id45=").append(rxById[SC_RID_STATE_BLE].get())
          .append(" id43=").append(rxById[SC_RID_BATTERY].get())
          .append(" id7B=").append(rxById[SC_RID_TELEMETRY].get())
          .append(" id47=").append(rxById[SC_RID_STATE_TS].get());
        synchronized (featureLock) {
            long avg = featOk > 0 ? featLatSumMs / featOk : 0;
            sb.append(" | feat req=").append(featReq).append(" ok=").append(featOk)
              .append(" stolen=").append(featStolen).append(" empty=").append(featEmpty)
              .append(" cache=").append(featCache)
              .append(" lat avg/max=").append(avg).append('/').append(featLatMaxMs).append(" ms");
        }
        sb.append(" | active=").append(activeIface.get()).append(" | ep rx/fwd:");
        for (int i = 0; i < readEndpoints.size() && i < rxByEndpoint.length; i++) {
            sb.append(' ').append(i).append('[').append(readEndpointIface.get(i)).append("]=")
              .append(rxByEndpoint[i].get()).append('/').append(fwdByEndpoint[i].get());
        }
        return sb.toString();
    }

    private static String hex(byte[] b, int n) {
        StringBuilder sb = new StringBuilder(n * 3);
        for (int i = 0; i < n && i < b.length; i++) {
            if (i > 0) sb.append(' ');
            String h = Integer.toHexString(b[i] & 0xFF);
            if (h.length() < 2) sb.append('0');
            sb.append(h);
        }
        return sb.toString();
    }

    // ---------------------------------------------------------------------
    // Feature proxy（Steam GetControllerInfo 握手；對齊 Qt featureRequestLocked）
    // ---------------------------------------------------------------------

    private void primeFeatureCache() {
        synchronized (featureLock) {
            if (stopped) {
                return;
            }
            byte[] q = new byte[SC_HID_WIRE_BYTES];
            q[0] = (byte) SC_FEATURE_RID;
            q[1] = (byte) SC_MSG_GET_ATTRIBUTES_VALUES;
            q[2] = 0; // length 0
            featureRequestLocked((byte) SC_FEATURE_RID, (byte) 2 /* SET */, (byte) 0, q, 3,
                    FEAT_WARMUP_BUDGET_MS, " (warm-up)");
        }
    }

    // May be invoked on an arbitrary native callback thread (common-c control
    // stream). featureLock serializes it against primeFeatureCache() and stop(),
    // so the connection can't be closed mid-transfer. Holding the lock through
    // the ≤100 ms poll is fine: feature ops only occur during Steam's handshake
    // (low rate) and input reads don't take this lock.
    @Override
    public void onScHidFeatureRequest(byte reportId, byte op, byte seq, byte[] query, byte queryLen) {
        synchronized (featureLock) {
            if (stopped) {
                LimeLog.warning("[SC-HID] Feature request after stop; dropping");
                return;
            }
            featureRequestLocked(reportId, op, seq, query, queryLen & 0xFF,
                    op == 2 ? FEAT_SET_POLL_MS : FEAT_GET_POLL_MS, "");
        }
    }

    // 回傳 1 = 已回覆 host（即時或快取），0 = 無回應（不回覆，host driver 400 ms 閘控
    // 逾時後退回 LastResponse）。呼叫端須持 featureLock。
    private int featureRequestLocked(byte reportId, byte op, byte seq, byte[] query, int queryLen,
                                     int pollBudgetMs, String tag) {
        final long t0 = SystemClock.uptimeMillis();
        featReq++;

        byte[] qbuf = new byte[SC_HID_WIRE_BYTES];
        if (query != null) {
            int qn = Math.min(queryLen, Math.min(query.length, SC_HID_WIRE_BYTES));
            System.arraycopy(query, 0, qbuf, 0, qn);
        }
        qbuf[0] = reportId; // ensure report id is correct in byte 0

        // 期望的回應 type：SET 用 query[1]；GET 用最近一次 SET 的 type（0 = 不輪詢）。
        int expectType;
        if (op == 2) {
            expectType = qbuf[1] & 0xFF;
            lastSetType = expectType;
            lastSetMillis = t0;
        } else {
            expectType = lastSetType;
        }

        // 候選介面：活躍 slot 優先；未知就全部（Puck 閒置 slot 無害）。
        int active = activeIface.get();
        ArrayList<Integer> cand = new ArrayList<>();
        if (active >= 0 && active < claimedIfaces.size()) {
            cand.add(active);
        } else {
            for (int i = 0; i < claimedIfaces.size(); i++) cand.add(i);
        }

        int sent = 0;
        if (op == 2) {
            for (int idx : cand) {
                UsbInterface iface = claimedIfaces.get(idx);
                // bmRequestType 0x21 = host-to-device | class | interface
                int r = connection.controlTransfer(0x21, HID_REQUEST_SET_REPORT,
                        HID_REPORT_TYPE_FEATURE | (reportId & 0xFF), iface.getId(),
                        qbuf, SC_HID_WIRE_BYTES, CONTROL_XFER_TIMEOUT_MS);
                if (r >= 0) sent++;
            }
        }

        byte[] resp = new byte[SC_HID_WIRE_BYTES];
        int respIface = -1;
        int stolen = 0;
        boolean got = false;

        // GET op：本場沒 SET 過或距上次 SET 太久（實體暫存器早已清空）就不輪詢。
        boolean shouldPoll = (op == 2 && sent > 0) ||
                (op != 2 && expectType != 0 && (t0 - lastSetMillis) < FEAT_GET_WINDOW_MS);

        if (shouldPoll) {
            long deadline = t0 + Math.max(1, pollBudgetMs);
            byte[] buf = new byte[SC_HID_WIRE_BYTES];
            outer:
            while (SystemClock.uptimeMillis() < deadline && !stopped) {
                for (int idx : cand) {
                    if (SystemClock.uptimeMillis() >= deadline) break outer;
                    Arrays.fill(buf, (byte) 0);
                    buf[0] = reportId;
                    int r = getFeatureReport(claimedIfaces.get(idx), reportId, buf);
                    if (r <= 0) continue; // STALL = 回應尚未就緒（一次性暫存器語義）
                    int t = buf[1] & 0xFF;
                    if (expectType != 0 && t != expectType) {
                        stolen++;
                        if (stolen == 1) {
                            LimeLog.info("[SC-HID] Feature poll: unexpected type 0x"+Integer.toHexString(t)+
                                    " (want 0x"+Integer.toHexString(expectType)+") - consumed, keep polling");
                        }
                        continue;
                    }
                    System.arraycopy(buf, 0, resp, 0, SC_HID_WIRE_BYTES);
                    respIface = idx;
                    got = true;
                    break outer;
                }
                try {
                    Thread.sleep(FEAT_POLL_STEP_MS);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }

        long lat = SystemClock.uptimeMillis() - t0;
        featStolen += stolen;
        int result;
        String src;
        if (got) {
            result = 1;
            src = "live";
            featOk++;
            featLatSumMs += lat;
            if (lat > featLatMaxMs) featLatMaxMs = lat;
            cacheValid = true;
            cacheType = resp[1] & 0xFF;
            cacheMillis = SystemClock.uptimeMillis();
            System.arraycopy(resp, 0, cacheResp, 0, SC_HID_WIRE_BYTES);
            if (op == 2) {
                System.arraycopy(qbuf, 0, cacheQuery, 0, SC_HID_WIRE_BYTES);
            } else {
                Arrays.fill(cacheQuery, (byte) 0); // GET 撿到的回應不對應任何 query
            }
            if (activeIface.get() < 0 && respIface >= 0) activeIface.set(respIface);
        } else {
            featEmpty++;
            boolean useCache = false;
            boolean cacheAllowed = !(op != 2 && expectType == 0);
            if (cacheAllowed && cacheValid && (SystemClock.uptimeMillis() - cacheMillis) <= FEAT_CACHE_TTL_MS) {
                useCache = (op == 2) ? Arrays.equals(cacheQuery, qbuf) : (cacheType == expectType);
            }
            if (useCache) {
                result = 1;
                src = "cache";
                featCache++;
                System.arraycopy(cacheResp, 0, resp, 0, SC_HID_WIRE_BYTES);
                resp[0] = reportId;
            } else {
                result = 0;
                src = "empty";
            }
        }

        if (result > 0) {
            MoonBridge.sendScHidFeatureReport(seq, reportId, resp);
            LimeLog.info("[SC-HID] Feature req"+tag+" id=0x"+Integer.toHexString(reportId & 0xFF)+
                    " op="+(op == 2 ? "SET" : "GET")+" seq="+(seq & 0xFF)+
                    " type=0x"+Integer.toHexString(expectType)+" -> resp type=0x"+Integer.toHexString(resp[1] & 0xFF)+
                    " lat="+lat+" ms iface="+respIface+" src="+src+" stolen="+stolen+
                    " cand="+cand.size()+" sent="+sent+" poll="+pollBudgetMs+" ms");
        } else {
            // 一律不回零：全零會被 host driver 忽略，回了只是白跑；讓 driver 以 400 ms
            // 閘控逾時退回 LastResponse。節流：控制器睡眠時 Steam 每秒重試會洗版。
            boolean quietPath = (op != 2 && expectType == 0);
            if (quietPath) {
                if (featEmpty == 1) {
                    LimeLog.info("[SC-HID] Feature req"+tag+" op=GET seq="+(seq & 0xFF)+
                            ": GET before any SET this session - not polled, not replied");
                }
            } else if (featEmpty == 1 || (featEmpty % 32) == 0) {
                LimeLog.warning("[SC-HID] Feature req"+tag+" id=0x"+Integer.toHexString(reportId & 0xFF)+
                        " op="+(op == 2 ? "SET" : "GET")+" seq="+(seq & 0xFF)+
                        " type=0x"+Integer.toHexString(expectType)+" -> no response (lat="+lat+
                        " ms stolen="+stolen+" cand="+cand.size()+" sent="+sent+" empty#"+featEmpty+
                        ") - not replied; host driver falls back after its 400 ms gate");
            }
        }
        return result;
    }

    // HID GET_REPORT(Feature) on the control pipe. Like hidapi's libusb backend
    // (which the Qt client uses on Linux), the device's response lands at
    // buf[0] with the report id prefix included. Returns bytes read or <0
    // (STALL while the one-shot response register is empty).
    private int getFeatureReport(UsbInterface iface, byte reportId, byte[] buf) {
        // bmRequestType 0xA1 = device-to-host | class | interface
        return connection.controlTransfer(0xA1, HID_REQUEST_GET_REPORT,
                HID_REPORT_TYPE_FEATURE | (reportId & 0xFF), iface.getId(),
                buf, SC_HID_WIRE_BYTES, CONTROL_XFER_TIMEOUT_MS);
    }
}
