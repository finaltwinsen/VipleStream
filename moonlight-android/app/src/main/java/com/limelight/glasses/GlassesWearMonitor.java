package com.limelight.glasses;

import android.os.SystemClock;

/**
 * §VITURE：配戴狀態只留一條輪詢。glasses_vr 與一般串流不能各開一條，
 * 查詢會佔住同一把交易鎖，音量與模式指令會被第二條輪詢擋住。
 */
public final class GlassesWearMonitor {
    public interface Listener {
        /** wear 從 1 變成 0，而且連續維持 5 秒。在輪詢執行緒上呼叫。 */
        void onWornOffSustained();

        /** 摘下之後又戴上。在輪詢執行緒上呼叫。 */
        void onWorn();

        /** 連續失敗 5 次，輪詢已停。在輪詢執行緒上呼叫。 */
        void onPollStopped();
    }

    private static final int INTERVAL_MS = 2000;
    private static final int FAIL_LIMIT = 5;
    private static final int UNWORN_HOLD_MS = 5000;

    private final VitureGlasses glasses;
    private final Listener listener;
    private final java.util.concurrent.atomic.AtomicBoolean closing =
            new java.util.concurrent.atomic.AtomicBoolean();
    private int lastWear;
    private long unwornSinceMs = -1L;
    private boolean announced;
    private int consecutiveFailures;
    private Thread thread;

    public GlassesWearMonitor(VitureGlasses glasses, int initialWear, Listener listener) {
        this.glasses = glasses;
        this.lastWear = initialWear;
        this.listener = listener;
    }

    public Thread start() {
        thread = new Thread(new Runnable() {
            @Override
            public void run() {
                poll();
            }
        }, "viple-viture-wear");
        thread.start();
        return thread;
    }

    public void requestStop() {
        closing.set(true);
        Thread current = thread;
        if (current != null) {
            current.interrupt();
        }
    }

    public void join(long timeoutMs) {
        Thread current = thread;
        if (current == null || current == Thread.currentThread()) {
            return;
        }
        try {
            current.join(timeoutMs);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        }
    }

    private void poll() {
        // §VITURE：開機那次查詢已經做過。跟以前一樣先等一個間隔，避免剛開就連打兩次。
        while (!closing.get()) {
            try {
                Thread.sleep(INTERVAL_MS);
            } catch (InterruptedException error) {
                break;
            }
            if (closing.get()) {
                break;
            }
            VitureGlasses.CommandResult result;
            try {
                result = glasses.query(VitureV2Protocol.MSG_GET_WEAR);
            } catch (RuntimeException error) {
                if (closing.get()) {
                    break;
                }
                VitureUsb.logW("wear query failed", error);
                if (noteFailure("exception")) {
                    break;
                }
                continue;
            }
            if (closing.get()) {
                break;
            }
            if (result == null || !result.ok || result.body.length < 1) {
                int status = result == null ? VitureGlasses.STATUS_IO : result.status;
                if (noteFailure("status=" + status)) {
                    break;
                }
                continue;
            }
            consecutiveFailures = 0;
            int value = result.body[0] & 0xFF;
            if (lastWear >= 0 && value != lastWear) {
                VitureUsb.logI("wear " + lastWear + " -> " + value);
            }
            if (value == 0) {
                // §VITURE：要先看過戴著（1）再變成 0。一開始就是摘下的不當成這次串流才摘下來。
                if (lastWear == 1) {
                    unwornSinceMs = SystemClock.elapsedRealtime();
                    announced = false;
                }
                if (unwornSinceMs >= 0L && !announced
                        && SystemClock.elapsedRealtime() - unwornSinceMs >= UNWORN_HOLD_MS) {
                    announced = true;
                    VitureUsb.logI("wear stayed 0 for 5 s");
                    fireWornOff();
                }
            } else if (announced) {
                unwornSinceMs = -1L;
                announced = false;
                fireWorn();
            } else {
                unwornSinceMs = -1L;
            }
            lastWear = value;
        }
    }

    /** @return 輪詢該停了 */
    private boolean noteFailure(String detail) {
        consecutiveFailures++;
        VitureUsb.logI("wear query failed " + detail + " consecutive=" + consecutiveFailures);
        if (consecutiveFailures < FAIL_LIMIT) {
            return false;
        }
        // §VITURE：連續失敗多半是線已經拔了。再查只會一直佔著交易鎖。
        VitureUsb.logI("wear poll stopped after " + consecutiveFailures
                + " consecutive failures; glasses may be unplugged");
        if (!closing.get() && listener != null) {
            try {
                listener.onPollStopped();
            } catch (RuntimeException error) {
                VitureUsb.logW("wear poll listener failed", error);
            }
        }
        return true;
    }

    private void fireWornOff() {
        if (closing.get() || listener == null) {
            return;
        }
        try {
            listener.onWornOffSustained();
        } catch (RuntimeException error) {
            VitureUsb.logW("wear listener failed", error);
        }
    }

    private void fireWorn() {
        if (closing.get() || listener == null) {
            return;
        }
        try {
            listener.onWorn();
        } catch (RuntimeException error) {
            VitureUsb.logW("wear listener failed", error);
        }
    }
}
