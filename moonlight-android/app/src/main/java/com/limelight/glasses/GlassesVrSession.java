package com.limelight.glasses;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.SystemClock;

import com.limelight.utils.LogDrop;
import com.limelight.vr.QxrNative;

import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * §VITURE：Game 只負責掛上這條路徑。USB、融合與 native push 都留在這裡，一般串流不會建它。
 */
public final class GlassesVrSession {
    private final VitureGlasses glasses;
    private final GlassesDisplaySwitcher switcher = new GlassesDisplaySwitcher();
    private final ImuFusion fusion = new ImuFusion();
    private final float[] quat = new float[4];
    private final float[] ang = new float[3];
    private final long nativeHandle;
    private final Runnable onDetached;
    private volatile boolean closing;
    private volatile boolean imuPushArmed;
    private volatile boolean imuGiveUp;
    // §LOGDROP：arm 之前的拆除是失敗的開啟，不是一次完成的工作階段。
    private volatile boolean sessionLive;
    private long sessionLiveElapsed;
    private boolean sessionEndSent;
    private final Object sessionEndLock = new Object();
    private long imuArmedNs;
    private Thread imuWatchThread;
    private int lastWear = -1;
    private GlassesWearMonitor wearMonitor;
    private Thread wearThread;
    private final GlassesWearMonitor.Listener wearListener;
    // 實機要一次看出陀螺儀單位與 deviceTick 步進。各只記一次，不改積分。
    private boolean loggedFirstRaw;
    private boolean loggedGyroScale;
    private boolean loggedTickStep;
    private long lastDeviceTick = -1L;
    private final float[] statusQuat = new float[4];
    private final float[] statusEuler = new float[3];

    private static final long IMU_BOOT_NS = 1_000_000_000L;
    private static final long IMU_GAP_NS = 500_000_000L;
    private static final int IMU_RETRY_LIMIT = 5;
    private static final long IMU_RETRY_BASE_MS = 250L;
    private static final long IMU_RETRY_MAX_MS = 4000L;
    private static final long READER_JOIN_MS = 1500L;

    /** 在讀取執行緒可以呼叫 nativeImuPush 之前，把 session 交進 Game 的生命週期鎖。 */
    public interface Attach {
        /** @return false 表示正在拆除，呼叫端必須自己停掉讀取執行緒，而且不能開始 push。 */
        boolean attach(GlassesVrSession session);
    }

    private GlassesVrSession(VitureGlasses glasses, long nativeHandle, Runnable onDetached,
                             GlassesWearMonitor.Listener wearListener) {
        this.glasses = glasses;
        this.nativeHandle = nativeHandle;
        this.onDetached = onDetached;
        this.wearListener = wearListener;
    }

    /** 阻塞。必須在主執行緒以外呼叫，USB 讀寫不能在主執行緒。 */
    public static GlassesVrSession open(Activity activity, long nativeHandle, Runnable onDetached,
                                        GlassesWearMonitor.Listener wearListener, Attach attach) {
        VitureUsb.checkNotMainThread();
        UsbManager usb = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
        if (usb == null) {
            VitureUsb.logI("glasses open failed: usb manager");
            return null;
        }
        UsbDevice device = VitureUsb.findGlassesDevice(usb);
        if (device == null) {
            VitureUsb.logI("glasses open failed: no device");
            return null;
        }
        if (!awaitPermission(activity, usb, device)) {
            VitureUsb.logI("glasses open failed: permission denied");
            return null;
        }
        if (activity.isFinishing() || activity.isDestroyed() || Thread.currentThread().isInterrupted()) {
            VitureUsb.logI("glasses open aborted");
            return null;
        }
        final GlassesVrSession[] holder = new GlassesVrSession[1];
        VitureGlasses.Listener listener = new VitureGlasses.Listener() {
            @Override
            public void onRawSample(float gx, float gy, float gz, float ax, float ay, float az,
                                    long sequence, long arrivalNs) {
                GlassesVrSession session = holder[0];
                if (session == null || !session.imuPushArmed || session.closing) {
                    return;
                }
                session.noteRawForDeviceCheck(gx, gy, gz, ax, ay, az, sequence);
                if (!session.fusion.update(gx, gy, gz, ax, ay, az, sequence, arrivalNs)) {
                    return;
                }
                session.fusion.copyQuaternion(session.quat);
                session.fusion.copyAngularVelocity(session.ang);
                if (nativeHandle != 0 && QxrNative.isAvailable()) {
                    QxrNative.nativeImuPush(nativeHandle,
                            session.quat[3], session.quat[0], session.quat[1], session.quat[2],
                            session.ang[0], session.ang[1], session.ang[2], arrivalNs);
                }
            }

            @Override
            public void onDisconnected() {
                // §VITURE：這條回呼可能在讀取執行緒。只轉交，不在這裡 close。
                GlassesVrSession session = holder[0];
                if (session == null || session.closing || session.onDetached == null) {
                    return;
                }
                session.onDetached.run();
            }
        };
        VitureGlasses glasses;
        try {
            glasses = VitureGlasses.open(activity, device, listener);
        } catch (Exception error) {
            VitureUsb.logW("glasses open failed", error);
            VitureUsb.logI("glasses open failed: " + error.getMessage());
            return null;
        }
        GlassesVrSession session = new GlassesVrSession(glasses, nativeHandle, onDetached, wearListener);
        // holder 先設好，但要等 attach 把 push 打開。否則拆除時 session 還不在 Game 手上。
        holder[0] = session;
        if (attach == null || !attach.attach(session)) {
            VitureUsb.logI("glasses open aborted before imu push");
            session.shutdownAndJoin(false, READER_JOIN_MS);
            return null;
        }
        session.startStreams(activity);
        return session;
    }

    /** 必須在 Game 持有生命週期鎖時呼叫，跟 glassesSession 的發布同一段。 */
    public void armImuPush() {
        // §LOGDROP：先寫時間再打開旗標。發布 session 之後，拆除才讀得到一組完整的開始時間。
        sessionLiveElapsed = SystemClock.elapsedRealtime();
        sessionLive = true;
        imuPushArmed = true;
    }

    public boolean imuSilent() {
        return imuGiveUp;
    }

    public void recenter() {
        fusion.recenter();
    }

    public boolean copyStatus(float[] hzYawPitchRoll) {
        if (hzYawPitchRoll == null || hzYawPitchRoll.length < 4 || !fusion.hasPose()) {
            return false;
        }
        fusion.copyQuaternion(statusQuat);
        ImuFusion.eulerDegrees(statusQuat[0], statusQuat[1], statusQuat[2], statusQuat[3], statusEuler);
        hzYawPitchRoll[0] = fusion.measuredHz();
        hzYawPitchRoll[1] = statusEuler[0];
        hzYawPitchRoll[2] = statusEuler[1];
        hzYawPitchRoll[3] = statusEuler[2];
        return true;
    }

    /**
     * §VITURE：跟 shutdown 共用切換器的同一把鎖。結束流程進鎖之後就不再送 0x32，避免關掉以後又被切回 3D。
     */
    public VitureGlasses.CommandResult setDisplayMode(int mode) {
        synchronized (switcher.lock()) {
            if (closing) {
                VitureUsb.logI("display mode 0x" + hexByte(mode) + " skipped; session closing");
                return new VitureGlasses.CommandResult(false, VitureGlasses.STATUS_IO, new byte[0]);
            }
            return switcher.setDisplayMode(glasses, mode);
        }
    }

    /** 送 0x32。還原旗標在共用的切換器裡；這行 log 跟以前寫在 Game 的那行相同。 */
    public VitureGlasses.CommandResult switchTo3d() {
        VitureGlasses.CommandResult result = setDisplayMode(VitureV2Protocol.DISPLAY_3D_SBS_60);
        VitureUsb.logI("SET display mode 0x32 status=" + result.status + " ok=" + result.ok);
        return result;
    }

    /**
     * 先停 nativeImuPush，送完結束指令，再等讀取執行緒停。
     *
     * @return 讀取執行緒已停。false 時呼叫端不能 destroy native handle。
     */
    public boolean shutdownAndJoin(boolean restore, long readerJoinMs) {
        VitureUsb.checkNotMainThread();
        // 讀取執行緒看到這面旗標就不再 push。USB 指令還要等它收 ACK，所以先不要停它。
        closing = true;
        Thread watch = imuWatchThread;
        if (watch != null) {
            watch.interrupt();
        }
        if (wearMonitor != null) {
            wearMonitor.requestStop();
        }
        synchronized (switcher.lock()) {
            switcher.markClosingLocked(restore);
            try {
                VitureGlasses.CommandResult imu = glasses.setImuRaw(false);
                VitureUsb.logI("imu stream off status=" + imu.status + " ok=" + imu.ok
                        + " body=" + imu.body.length);
            } catch (RuntimeException error) {
                VitureUsb.logW("imu off failed", error);
            }
            switcher.restore2dLocked(glasses);
        }
        if (watch != null) {
            try {
                watch.join(READER_JOIN_MS);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
            }
        }
        if (wearMonitor != null) {
            wearMonitor.join(500);
        }
        boolean stopped;
        try {
            stopped = glasses.stopReader(readerJoinMs);
        } finally {
            // §LOGDROP：讀取執行緒停不下來也是一次結束，否則這段摘要不會送出。
            notifySessionEnd();
        }
        return stopped;
    }

    private void notifySessionEnd() {
        synchronized (sessionEndLock) {
            // §LOGDROP：open() 在 arm 之前就會 shutdown。那次 sessionLive 仍是 false，不是一次完成的工作階段。
            if (!sessionLive || sessionEndSent) {
                return;
            }
            sessionEndSent = true;
        }
        long duration = SystemClock.elapsedRealtime() - sessionLiveElapsed;
        if (duration < 0L) {
            duration = 0L;
        }
        String summary = "duration_ms=" + duration
                + " imu_samples=" + glasses.rawSampleTotal()
                + " " + switcher.sessionEndSummary();
        LogDrop.autoUploadAsync(glasses.applicationContext(), "android-glasses", "session-end", summary);
    }

    private void noteRawForDeviceCheck(float gx, float gy, float gz, float ax, float ay, float az,
                                       long sequence) {
        long tick = sequence & 0xFFFFFFFFL;
        if (!loggedFirstRaw) {
            loggedFirstRaw = true;
            VitureUsb.logI(String.format(Locale.US,
                    "imu first raw gyro=%.5f,%.5f,%.5f accel=%.5f,%.5f,%.5f seq=%d",
                    gx, gy, gz, ax, ay, az, tick));
        }
        // 靜止時 rad/s 與 deg/s 都接近 0。一般轉頭若是 deg/s 會超過 5，rad/s 通常不會。
        float peak = Math.abs(gx);
        if (Math.abs(gy) > peak) {
            peak = Math.abs(gy);
        }
        if (Math.abs(gz) > peak) {
            peak = Math.abs(gz);
        }
        if (!loggedGyroScale && peak >= 5f) {
            loggedGyroScale = true;
            VitureUsb.logI(String.format(Locale.US,
                    "imu motion raw gyro=%.5f,%.5f,%.5f accel=%.5f,%.5f,%.5f seq=%d",
                    gx, gy, gz, ax, ay, az, tick));
        }
        if (lastDeviceTick >= 0L && !loggedTickStep) {
            loggedTickStep = true;
            long delta = (tick - lastDeviceTick) & 0xFFFFFFFFL;
            VitureUsb.logI("imu deviceTick delta=" + delta + " seq=" + tick + " prev=" + lastDeviceTick);
        }
        lastDeviceTick = tick;
    }

    private void applySavedAxis(Activity activity) {
        SharedPreferences prefs = activity.getSharedPreferences(VitureAxisMap.PREFS_NAME, Context.MODE_PRIVATE);
        String saved = prefs.getString(VitureAxisMap.PREFS_KEY, VitureAxisMap.DEFAULT);
        if (!fusion.applyAxisText(saved)) {
            VitureUsb.logI("viture_axis_map invalid '" + saved + "'; using " + VitureAxisMap.DEFAULT);
            fusion.applyAxisText(VitureAxisMap.DEFAULT);
            VitureUsb.logI("axis=" + VitureAxisMap.DEFAULT);
            return;
        }
        String canonical = VitureAxisMap.canonical(saved);
        VitureUsb.logI("axis=" + (canonical == null ? VitureAxisMap.DEFAULT : canonical));
    }

    private void logQueries() {
        VitureGlasses.CommandResult firmware = glasses.query(VitureV2Protocol.MSG_GET_VERSION);
        VitureUsb.logI("firmware=" + ascii(firmware));
        VitureGlasses.CommandResult wear = glasses.query(VitureV2Protocol.MSG_GET_WEAR);
        VitureGlasses.CommandResult display = glasses.query(VitureV2Protocol.MSG_GET_DISPLAY);
        VitureGlasses.CommandResult volume = glasses.query(VitureV2Protocol.MSG_GET_VOLUME);
        VitureGlasses.CommandResult duty = glasses.query(VitureV2Protocol.MSG_GET_DUTY_CYCLE);
        if (wear.ok && wear.body.length >= 1) {
            lastWear = wear.body[0] & 0xFF;
        }
        VitureUsb.logI("wear=" + u8(wear)
                + " display=" + hexU8(display)
                + " volume=" + u8(volume)
                + " duty=" + u8(duty));
    }

    private void startStreams(Activity activity) {
        applySavedAxis(activity);
        synchronized (switcher.lock()) {
            if (closing) {
                return;
            }
            logQueries();
            VitureGlasses.CommandResult imu = glasses.setImuRaw(true);
            VitureUsb.logI("imu stream on status=" + imu.status + " ok=" + imu.ok
                    + " body=" + imu.body.length);
            if (closing) {
                return;
            }
            startWearPoll();
            startImuWatch();
        }
    }

    private void startImuWatch() {
        imuArmedNs = System.nanoTime();
        imuGiveUp = false;
        imuWatchThread = new Thread(new Runnable() {
            @Override
            public void run() {
                watchImu();
            }
        }, "viple-viture-imu-watch");
        imuWatchThread.start();
    }

    private void watchImu() {
        int retries = 0;
        long backoffMs = IMU_RETRY_BASE_MS;
        while (!closing) {
            long now = System.nanoTime();
            long last = fusion.arrivalNs();
            long deadline = last == 0L ? imuArmedNs + IMU_BOOT_NS : last + IMU_GAP_NS;
            if (now < deadline) {
                retries = 0;
                backoffMs = IMU_RETRY_BASE_MS;
                imuGiveUp = false;
                long waitMs = (deadline - now) / 1_000_000L;
                if (waitMs > 200L) {
                    waitMs = 200L;
                }
                if (waitMs < 20L) {
                    waitMs = 20L;
                }
                if (!sleepWatch(waitMs)) {
                    return;
                }
                continue;
            }
            if (retries >= IMU_RETRY_LIMIT) {
                imuGiveUp = true;
                if (!sleepWatch(500L)) {
                    return;
                }
                continue;
            }
            retries++;
            long ageMs = (now - (last == 0L ? imuArmedNs : last)) / 1_000_000L;
            VitureUsb.logI("imu watchdog retry " + retries + "/" + IMU_RETRY_LIMIT + " ageMs=" + ageMs);
            resendImu();
            if (retries >= IMU_RETRY_LIMIT) {
                imuGiveUp = true;
                VitureUsb.logI("imu watchdog gave up after " + IMU_RETRY_LIMIT + " retries");
            }
            if (!sleepWatch(backoffMs)) {
                return;
            }
            if (backoffMs < IMU_RETRY_MAX_MS) {
                backoffMs *= 2L;
            }
        }
    }

    private void resendImu() {
        synchronized (switcher.lock()) {
            if (closing) {
                return;
            }
            try {
                VitureGlasses.CommandResult result = glasses.setImuRaw(true);
                VitureUsb.logI("imu watchdog resend status=" + result.status + " ok=" + result.ok);
            } catch (RuntimeException error) {
                VitureUsb.logW("imu watchdog resend failed", error);
            }
        }
    }

    private boolean sleepWatch(long ms) {
        if (closing) {
            return false;
        }
        try {
            Thread.sleep(ms);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return false;
        }
        return !closing;
    }

    private void startWearPoll() {
        // §VITURE：跟一般串流共用這一條。glasses_vr 不會跟一般串流同時開，所以不會有兩條一起查。
        wearMonitor = new GlassesWearMonitor(glasses, lastWear, wearListener);
        wearThread = wearMonitor.start();
    }

    private static boolean awaitPermission(Activity activity, UsbManager usb, UsbDevice device) {
        try {
            if (usb.hasPermission(device)) {
                return true;
            }
        } catch (SecurityException error) {
            VitureUsb.logW("usb permission check failed", error);
            return false;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final AtomicBoolean granted = new AtomicBoolean(false);
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                if (intent == null || !VitureUsb.ACTION_USB_PERMISSION.equals(intent.getAction())) {
                    return;
                }
                UsbDevice extra = extraDevice(intent);
                if (extra == null || device.getDeviceName() == null
                        || !device.getDeviceName().equals(extra.getDeviceName())) {
                    return;
                }
                granted.set(intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false));
                latch.countDown();
            }
        };
        IntentFilter filter = new IntentFilter(VitureUsb.ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            activity.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            activity.registerReceiver(receiver, filter);
        }
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    VitureUsb.requestPermission(activity, usb, device);
                } catch (RuntimeException error) {
                    VitureUsb.logW("permission request failed", error);
                    latch.countDown();
                }
            }
        });
        try {
            latch.await(30, TimeUnit.SECONDS);
        } catch (InterruptedException error) {
            // 拆除會 interrupt 這條執行緒。權限對話框不能再把 Activity 留到逾時。
            Thread.currentThread().interrupt();
            try {
                activity.unregisterReceiver(receiver);
            } catch (RuntimeException ignored) {
                // 已經解註冊。
            }
            return false;
        }
        if (Thread.currentThread().isInterrupted()
                || activity.isFinishing() || activity.isDestroyed()) {
            try {
                activity.unregisterReceiver(receiver);
            } catch (RuntimeException ignored) {
                // 已經解註冊。
            }
            return false;
        }
        try {
            activity.unregisterReceiver(receiver);
        } catch (RuntimeException ignored) {
            // 已經解註冊。
        }
        return granted.get();
    }

    @SuppressWarnings("deprecation")
    private static UsbDevice extraDevice(Intent intent) {
        return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    }

    private static String ascii(VitureGlasses.CommandResult result) {
        if (result == null || !result.ok) {
            return "status:" + (result == null ? "null" : result.status);
        }
        int end = result.body.length;
        while (end > 0) {
            byte value = result.body[end - 1];
            if (value != 0 && value != ' ') {
                break;
            }
            end--;
        }
        return new String(result.body, 0, end, StandardCharsets.US_ASCII).replace('\n', ' ').replace('\r', ' ');
    }

    private static String u8(VitureGlasses.CommandResult result) {
        if (result == null || !result.ok || result.body.length < 1) {
            return "status:" + (result == null ? "null" : result.status);
        }
        return Integer.toString(result.body[0] & 0xFF);
    }

    private static String hexU8(VitureGlasses.CommandResult result) {
        if (result == null || !result.ok || result.body.length < 1) {
            return "status:" + (result == null ? "null" : result.status);
        }
        return String.format(Locale.US, "0x%02X", result.body[0] & 0xFF);
    }

    private static String hexByte(int value) {
        return String.format(Locale.US, "%02X", value & 0xFF);
    }
}
