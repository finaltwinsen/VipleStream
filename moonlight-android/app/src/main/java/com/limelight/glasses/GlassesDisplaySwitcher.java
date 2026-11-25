package com.limelight.glasses;

import java.util.Locale;

/**
 * §VITURE：glasses_vr 與一般串流共用的顯示模式切換。
 * 位元組與還原旗標只放這裡，兩邊才不會各送一次不一樣的 0x32／0x31。
 */
public final class GlassesDisplaySwitcher {
    public static final int WIDTH_2D = 1920;
    public static final int WIDTH_3D = 3840;
    public static final int HEIGHT_SBS = 1080;
    /** §VITURE：Beast 的 3D 是 3840，2D 是 1920。3000 落在兩者中間，避免把 1920 看成 3D。 */
    public static final int WIDTH_3D_MIN = 3000;
    /** §VITURE：跟 glasses_vr 等 3D 面板的時間相同。指令被接受不代表面板真的換了。 */
    public static final int MODE_WAIT_MS = 10000;

    private final Object lock = new Object();
    private volatile boolean closing;
    private boolean restore2d;
    // §LOGDROP：結束摘要只記最後一次真的送出的模式。正在關閉而略過的指令不能算進去。
    private boolean sawMode;
    private int lastMode;
    private boolean lastModeOk;
    private int lastModeStatus;
    private boolean sawRestore;
    private boolean restoreOk;
    private int restoreStatus;

    public static boolean streamIsSbs(int width, int height) {
        return width == WIDTH_3D && height == HEIGHT_SBS;
    }

    public static boolean isStereoWidth(int width) {
        return width >= WIDTH_3D_MIN;
    }

    public boolean isClosing() {
        return closing;
    }

    Object lock() {
        return lock;
    }

    /**
     * §VITURE：呼叫端已持有 {@link #lock()}。
     * forceRestore 表示這次串流真的用過 3D 面板，結束時要送回 2D。
     * 0x32 成功也會另外把 restore2d 設起來；兩邊都沒有就不會多送 0x31。
     */
    void markClosingLocked(boolean forceRestore) {
        closing = true;
        if (forceRestore) {
            restore2d = true;
        }
    }

    /** 新的切換不再送出。還沒把 2D 送回去，配戴輪詢可以先停。 */
    public void beginClose() {
        synchronized (lock) {
            closing = true;
        }
    }

    public VitureGlasses.CommandResult setDisplayMode(VitureGlasses glasses, int mode) {
        synchronized (lock) {
            if (closing) {
                VitureUsb.logI("display mode 0x" + hexByte(mode) + " skipped; session closing");
                return new VitureGlasses.CommandResult(false, VitureGlasses.STATUS_IO, new byte[0]);
            }
            if (glasses != null && glasses.isBeast()) {
                // §VITURE：Beast 的 0x31／0x32／0x34 會黑屏，模式改看外接顯示器寬度。
                VitureUsb.logI("display mode 0x" + hexByte(mode) + " skipped; beast");
                return new VitureGlasses.CommandResult(false, VitureGlasses.STATUS_IO, new byte[0]);
            }
            VitureGlasses.CommandResult result = glasses.setDisplayMode(mode);
            // 這次 0x32 成功才代表指令把模式改掉。本來就在 3D、沒有成功的 0x32，
            // 要等呼叫端看到 3D 面板真的被這次串流用過，才在 markClosingLocked 要求送回 2D。
            if (result.ok && mode == VitureV2Protocol.DISPLAY_3D_SBS_60) {
                restore2d = true;
            }
            // §LOGDROP：失敗也要留下。上面因正在關閉而直接返回的那次沒有寫入，才不會蓋掉這筆。
            sawMode = true;
            lastMode = mode;
            lastModeOk = result.ok;
            lastModeStatus = result.status;
            return result;
        }
    }

    /** 送 0x32，並留下跟 glasses_vr 相同的那行 log。Beast 不送，避免下面那行被看成有送出。 */
    public VitureGlasses.CommandResult switchTo3d(VitureGlasses glasses) {
        if (glasses != null && glasses.isBeast()) {
            VitureUsb.logI("SET display mode 0x32 skipped; beast");
            return new VitureGlasses.CommandResult(false, VitureGlasses.STATUS_IO, new byte[0]);
        }
        VitureGlasses.CommandResult result = setDisplayMode(glasses, VitureV2Protocol.DISPLAY_3D_SBS_60);
        VitureUsb.logI("SET display mode 0x32 status=" + result.status + " ok=" + result.ok);
        return result;
    }

    /**
     * §VITURE：按鈕已把 2D 送成功。結束時再送一次會讓外接畫面再消失一輪。
     */
    public void noteUserReturnedTo2d() {
        synchronized (lock) {
            restore2d = false;
        }
    }

    public void restore2d(VitureGlasses glasses) {
        String summary;
        synchronized (lock) {
            restore2dLocked(glasses);
            summary = buildSummaryLocked();
        }
        // §LOGDROP：一般串流只從公開的 restore2d 結束。手動測試的 close() 沒有先走這裡。
        glasses.markPlainSessionEnd(summary);
    }

    /** 呼叫端已持有 {@link #lock()}。沒切過 3D 就什麼都不送。 */
    void restore2dLocked(VitureGlasses glasses) {
        if (glasses != null && glasses.isBeast()) {
            // §VITURE：Beast 沒有對得上的 2D 代碼。0x31 照樣會黑屏，還原留給眼鏡上的按鈕。
            if (restore2d) {
                VitureUsb.logI("restore display mode skipped; beast");
            }
            restore2d = false;
            return;
        }
        if (!restore2d) {
            return;
        }
        restore2d = false;
        try {
            VitureGlasses.CommandResult result = glasses.setDisplayMode(VitureV2Protocol.DISPLAY_2D_60);
            // §LOGDROP：還原直接打眼鏡，不經過 setDisplayMode，才不會蓋掉最後一次使用者切換。
            noteRestoreLocked(result.ok, result.status);
            if (result.ok) {
                VitureUsb.logI("restore display mode 0x31 status=" + result.status + " ok=true");
            } else {
                VitureUsb.logI("restore display mode 0x31 status=" + result.status + " ok=false");
            }
        } catch (RuntimeException error) {
            noteRestoreLocked(false, VitureGlasses.STATUS_IO);
            VitureUsb.logW("restore 2d failed", error);
        }
    }

    /** §LOGDROP：glasses_vr 的結束不走公開的 restore2d，摘要要另外拿。 */
    String sessionEndSummary() {
        synchronized (lock) {
            return buildSummaryLocked();
        }
    }

    private void noteRestoreLocked(boolean ok, int status) {
        sawRestore = true;
        restoreOk = ok;
        restoreStatus = status;
    }

    private String buildSummaryLocked() {
        StringBuilder summary = new StringBuilder();
        if (!sawMode) {
            summary.append("display_mode=none");
        } else {
            summary.append("display_mode=0x").append(hexByte(lastMode))
                    .append(" ok=").append(lastModeOk)
                    .append(" status=").append(lastModeStatus);
        }
        if (!sawRestore) {
            summary.append(" restore_2d=skipped");
        } else {
            summary.append(" restore_2d=").append(restoreOk ? "ok" : "failed")
                    .append(" status=").append(restoreStatus);
        }
        return summary.toString();
    }

    public static void logModeRejected(VitureGlasses.CommandResult result) {
        VitureUsb.logI("display mode 0x32 rejected status=" + result.status
                + "; waiting for the mode button");
    }

    public static void logModeTimeout(VitureGlasses.CommandResult result, long elapsedMs) {
        VitureUsb.logI("display mode 0x32 timed out after " + elapsedMs + " ms status=" + result.status
                + "; waiting for the mode button");
    }

    private static String hexByte(int value) {
        return String.format(Locale.US, "%02X", value & 0xFF);
    }
}
