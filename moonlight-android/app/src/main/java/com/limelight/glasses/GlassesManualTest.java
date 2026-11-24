package com.limelight.glasses;

import android.content.Context;
import android.hardware.display.DisplayManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Display;

import com.limelight.R;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * §VITURE：診斷畫面的手動測試。每個動作都會改眼鏡設定，所以只能由按鈕觸發。
 */
public final class GlassesManualTest {
    private static final int[] AXIS_PROMPTS = {
            R.string.glasses_axis_still,
            R.string.glasses_axis_nod,
            R.string.glasses_axis_yaw,
            R.string.glasses_axis_roll
    };
    private static final String[] AXIS_NAMES = {"still", "nod", "yaw", "roll"};

    public interface Host {
        boolean isCancelled();

        String ensurePermission(UsbDevice device);

        void showPrompt(int stringRes);

        void setManualGlasses(VitureGlasses glasses);
    }

    private GlassesManualTest() {
    }

    public static String displayMode(Context context, Host host, int mode) {
        StringBuilder report = new StringBuilder();
        VitureGlasses glasses = open(context, host, report);
        if (glasses == null) {
            return report.toString();
        }
        DisplayWatch watch = new DisplayWatch(context);
        try {
            watch.start();
            VitureGlasses.CommandResult result = glasses.setDisplayMode(mode);
            report.append("SET display mode=0x").append(hexByte(mode))
                    .append(" status=").append(result.status)
                    .append(" ok=").append(result.ok)
                    .append('\n');
            watch.await(host, 10000);
            report.append(watch.dump());
            VitureGlasses.CommandResult query = glasses.query(VitureV2Protocol.MSG_GET_DISPLAY);
            report.append("GET display id=0x3141 status=").append(query.status)
                    .append(" value=").append(query.ok && query.body.length >= 1
                            ? String.format(Locale.US, "0x%02X", query.body[0] & 0xFF)
                            : "none")
                    .append('\n');
        } catch (RuntimeException error) {
            report.append("manual error=").append(VitureProbe.errorText(error)).append('\n');
        } finally {
            watch.stop();
            host.setManualGlasses(null);
            glasses.close();
        }
        return report.toString();
    }

    public static String axisTest(Context context, Host host) {
        StringBuilder report = new StringBuilder();
        final List<RawPoint> samples = new ArrayList<>();
        VitureGlasses glasses = open(context, host, report);
        if (glasses == null) {
            return report.toString();
        }
        glasses.setListener(new VitureGlasses.Listener() {
            @Override
            public void onRawSample(float gx, float gy, float gz, float ax, float ay, float az,
                                    long sequence, long arrivalNs) {
                RawPoint point = new RawPoint();
                point.gx = gx;
                point.gy = gy;
                point.gz = gz;
                point.ax = ax;
                point.ay = ay;
                point.az = az;
                point.arrivalNs = arrivalNs;
                synchronized (samples) {
                    samples.add(point);
                }
            }

            @Override
            public void onDisconnected() {
                // 測試結束會自己關。拔除時讓迴圈看到 cancelled 以外的空樣本即可。
            }
        });
        long t0Nano = 0L;
        try {
            VitureGlasses.CommandResult imu = glasses.setImuRaw(true);
            report.append("imu stream on status=").append(imu.status)
                    .append(" ok=").append(imu.ok).append('\n');
            t0Nano = System.nanoTime();
            long t0Elapsed = SystemClock.elapsedRealtime();
            int shown = -1;
            while (!host.isCancelled() && !Thread.currentThread().isInterrupted()) {
                long elapsed = SystemClock.elapsedRealtime() - t0Elapsed;
                if (elapsed >= 20000L) {
                    break;
                }
                int segment = (int) (elapsed / 5000L);
                if (segment > 3) {
                    segment = 3;
                }
                if (segment != shown) {
                    host.showPrompt(AXIS_PROMPTS[segment]);
                    shown = segment;
                }
                try {
                    Thread.sleep(100);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            try {
                glasses.setImuRaw(false);
            } catch (RuntimeException error) {
                report.append("imu off error=").append(VitureProbe.errorText(error)).append('\n');
            }
            List<RawPoint> copy;
            synchronized (samples) {
                copy = new ArrayList<>(samples);
            }
            appendAxisReport(report, copy, t0Nano);
        } catch (RuntimeException error) {
            report.append("manual error=").append(VitureProbe.errorText(error)).append('\n');
        } finally {
            host.setManualGlasses(null);
            glasses.close();
        }
        return report.toString();
    }

    private static void appendAxisReport(StringBuilder report, List<RawPoint> samples, long t0Nano) {
        report.append("AXIS_TEST duration_ms=20000 segments=still,nod,yaw,roll\n");
        report.append("note=raw sensor axes, no axis mapping\n");
        double[][] accelSum = new double[4][3];
        double[][] integ = new double[4][3];
        float[][] peak = new float[4][3];
        boolean[][] havePeak = new boolean[4][3];
        float[][] prevGyro = new float[4][3];
        long[] prevArrival = new long[4];
        int[] count = new int[4];
        long windowNs = 20_000_000_000L;
        for (int i = 0; i < samples.size(); i++) {
            RawPoint point = samples.get(i);
            if (point.arrivalNs < t0Nano || point.arrivalNs >= t0Nano + windowNs) {
                continue;
            }
            int segment = (int) ((point.arrivalNs - t0Nano) / 5_000_000_000L);
            if (segment < 0 || segment > 3) {
                continue;
            }
            if (count[segment] > 0) {
                double dt = (point.arrivalNs - prevArrival[segment]) * 1.0e-9;
                if (dt < 0d) {
                    dt = 0d;
                } else if (dt > 0.05d) {
                    // §VITURE：中間停太久時，用那段間隔積分會把角度灌爆。
                    dt = 0.05d;
                }
                integ[segment][0] += prevGyro[segment][0] * dt;
                integ[segment][1] += prevGyro[segment][1] * dt;
                integ[segment][2] += prevGyro[segment][2] * dt;
            }
            prevGyro[segment][0] = point.gx;
            prevGyro[segment][1] = point.gy;
            prevGyro[segment][2] = point.gz;
            prevArrival[segment] = point.arrivalNs;
            accelSum[segment][0] += point.ax;
            accelSum[segment][1] += point.ay;
            accelSum[segment][2] += point.az;
            considerPeak(peak[segment], havePeak[segment], 0, point.gx);
            considerPeak(peak[segment], havePeak[segment], 1, point.gy);
            considerPeak(peak[segment], havePeak[segment], 2, point.gz);
            count[segment]++;
        }
        for (int segment = 0; segment < 4; segment++) {
            int start = segment * 5000;
            int end = start + 5000;
            double inv = count[segment] > 0 ? 1d / count[segment] : 0d;
            report.append("segment=").append(AXIS_NAMES[segment])
                    .append(" t_ms=").append(start).append('-').append(end)
                    .append(" samples=").append(count[segment]).append('\n');
            report.append(String.format(Locale.US, "accel_mean_g=%.6f,%.6f,%.6f\n",
                    accelSum[segment][0] * inv, accelSum[segment][1] * inv, accelSum[segment][2] * inv));
            report.append(String.format(Locale.US, "gyro_integ_deg=%.6f,%.6f,%.6f\n",
                    Math.toDegrees(integ[segment][0]), Math.toDegrees(integ[segment][1]),
                    Math.toDegrees(integ[segment][2])));
            report.append(String.format(Locale.US, "gyro_peak_rad_s=%.6f,%.6f,%.6f\n",
                    peak[segment][0], peak[segment][1], peak[segment][2]));
        }
    }

    private static void considerPeak(float[] peak, boolean[] havePeak, int axis, float value) {
        if (!havePeak[axis] || Math.abs(value) > Math.abs(peak[axis])) {
            peak[axis] = value;
            havePeak[axis] = true;
        }
    }

    private static VitureGlasses open(Context context, Host host, StringBuilder report) {
        UsbManager usb = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        if (usb == null) {
            report.append("manual usb_manager=unavailable\n");
            return null;
        }
        UsbDevice device = VitureUsb.findGlassesDevice(usb);
        if (device == null) {
            report.append("manual no_device\n");
            return null;
        }
        String permission = host.ensurePermission(device);
        if (!"granted".equals(permission)) {
            report.append("manual permission=").append(permission).append('\n');
            return null;
        }
        if (host.isCancelled()) {
            report.append("manual cancelled\n");
            return null;
        }
        try {
            VitureGlasses glasses = VitureGlasses.open(context, device, null);
            host.setManualGlasses(glasses);
            return glasses;
        } catch (IOException error) {
            report.append("manual open failed: ").append(VitureProbe.errorText(error)).append('\n');
            return null;
        }
    }

    private static String hexByte(int value) {
        return String.format(Locale.US, "%02X", value & 0xFF);
    }

    private static final class RawPoint {
        float gx;
        float gy;
        float gz;
        float ax;
        float ay;
        float az;
        long arrivalNs;
    }

    private static final class DisplayWatch {
        private final Context context;
        private final List<String> lines = new ArrayList<>();
        private final long t0 = SystemClock.elapsedRealtime();
        private DisplayManager manager;
        private DisplayManager.DisplayListener listener;

        DisplayWatch(Context context) {
            this.context = context;
        }

        void start() {
            manager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
            if (manager == null) {
                synchronized (lines) {
                    lines.add("t_ms=0 event=unavailable id=none mode=none");
                }
                return;
            }
            listener = new DisplayManager.DisplayListener() {
                @Override
                public void onDisplayAdded(int displayId) {
                    record("added", displayId);
                }

                @Override
                public void onDisplayRemoved(int displayId) {
                    record("removed", displayId);
                }

                @Override
                public void onDisplayChanged(int displayId) {
                    record("changed", displayId);
                }
            };
            manager.registerDisplayListener(listener, new Handler(Looper.getMainLooper()));
        }

        void await(Host host, long ms) {
            long end = SystemClock.elapsedRealtime() + ms;
            while (SystemClock.elapsedRealtime() < end) {
                if (host.isCancelled() || Thread.currentThread().isInterrupted()) {
                    return;
                }
                long slice = Math.min(200L, end - SystemClock.elapsedRealtime());
                if (slice <= 0L) {
                    return;
                }
                try {
                    Thread.sleep(slice);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }

        void stop() {
            if (manager != null && listener != null) {
                try {
                    manager.unregisterDisplayListener(listener);
                } catch (RuntimeException ignored) {
                    // 已經解註冊。
                }
            }
            listener = null;
        }

        String dump() {
            StringBuilder out = new StringBuilder();
            out.append("display_watch begin\n");
            synchronized (lines) {
                for (int i = 0; i < lines.size(); i++) {
                    out.append(lines.get(i)).append('\n');
                }
            }
            out.append("display_watch end\n");
            return out.toString();
        }

        private void record(String event, int displayId) {
            long tMs = SystemClock.elapsedRealtime() - t0;
            String mode = "gone";
            if (manager != null && !"removed".equals(event)) {
                Display display = manager.getDisplay(displayId);
                if (display != null) {
                    Display.Mode current = display.getMode();
                    if (current != null) {
                        mode = current.getPhysicalWidth() + "x" + current.getPhysicalHeight()
                                + "@" + Math.round(current.getRefreshRate());
                    }
                }
            }
            String line = "t_ms=" + tMs + " event=" + event + " id=" + displayId + " mode=" + mode;
            synchronized (lines) {
                lines.add(line);
            }
        }
    }
}
