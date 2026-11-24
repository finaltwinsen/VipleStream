package com.limelight.glasses;

import android.content.Context;
import android.content.SharedPreferences;
import android.hardware.display.DisplayManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.util.DisplayMetrics;
import android.view.Display;

import com.limelight.BuildConfig;
import com.limelight.utils.LogDrop;

import java.lang.reflect.Field;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/**
 * §VITURE：報告會被公開貼出。這裡只放版本、機型、顯示器與 USB 描述，不讀序號、帳號、Wi-Fi、IP、已配對主機。
 */
public final class GlassesDiagnostics {
    public enum Stage {
        DISPLAYS,
        USB,
        PERMISSION,
        OPEN,
        QUERIES,
        POSE,
        RAW,
        DONE,
        NO_DEVICE
    }

    public interface Host {
        boolean isCancelled();

        /** 英文單行：granted、denied、cancelled，或 error 開頭。 */
        String ensurePermission(UsbDevice device);

        void onStage(Stage stage);
    }

    private GlassesDiagnostics() {
    }

    public static String collect(Context context, Host host) {
        StringBuilder report = new StringBuilder(8192);
        long startedNs = System.nanoTime();
        VitureUsb.logI("diagnostics start");
        try {
            appendInstallId(context, report);
            appendAxisMap(context, report);
            appendHeader(report);
            host.onStage(Stage.DISPLAYS);
            appendDisplays(context, report);
            host.onStage(Stage.USB);
            UsbManager usb = (UsbManager) context.getSystemService(Context.USB_SERVICE);
            VitureUsb.appendInventory(usb, report);
            List<UsbDevice> devices = VitureUsb.findDevices(usb);
            if (devices.isEmpty()) {
                report.append("\nPROBE\n");
                report.append("No VITURE USB device found.\n");
                host.onStage(Stage.NO_DEVICE);
            } else {
                VitureUsb.logI("viture devices=" + devices.size());
                for (int i = 0; i < devices.size(); i++) {
                    if (host.isCancelled()) {
                        report.append("cancelled\n");
                        break;
                    }
                    probeOne(usb, devices.get(i), i, devices.size(), host, report);
                }
                if (!host.isCancelled()) {
                    host.onStage(Stage.DONE);
                }
            }
        } catch (Exception error) {
            report.append("fatal=").append(VitureProbe.errorText(error)).append('\n');
            VitureUsb.logW("diagnostics failed", error);
        }
        long probeMs = (System.nanoTime() - startedNs) / 1_000_000L;
        report.append("probe_ms=").append(probeMs).append('\n');
        report.append("END OF REPORT\n");
        VitureUsb.logI("diagnostics done bytes=" + report.length());
        return report.toString();
    }

    private static void probeOne(UsbManager usb, UsbDevice device, int index, int count,
                                 Host host, StringBuilder report) {
        report.append("\nPROBE\n");
        report.append("index=").append(index + 1).append(" of ").append(count)
                .append(" dev=").append(VitureUsb.quoted(device.getDeviceName()))
                .append(" vid=").append(VitureV2Protocol.hex16(device.getVendorId()))
                .append(" pid=").append(VitureV2Protocol.hex16(device.getProductId()));
        String known = VitureUsb.knownLabel(device.getVendorId(), device.getProductId());
        if (known.length() > 0) {
            report.append(" known=").append(known);
        }
        report.append('\n');
        String permission = oneLine(host.ensurePermission(device));
        report.append("permission=").append(permission).append('\n');
        if (!"granted".equals(permission) || host.isCancelled()) {
            return;
        }
        host.onStage(Stage.OPEN);
        VitureUsb.Session session = null;
        try {
            session = VitureUsb.Session.open(usb, device, report);
            if (session == null) {
                return;
            }
            final Host outer = host;
            VitureProbe.run(session, new VitureProbe.Host() {
                @Override
                public boolean isCancelled() {
                    return outer.isCancelled();
                }

                @Override
                public void onStage(VitureProbe.Stage stage) {
                    if (stage == VitureProbe.Stage.POSE) {
                        outer.onStage(Stage.POSE);
                    } else if (stage == VitureProbe.Stage.RAW) {
                        outer.onStage(Stage.RAW);
                    } else {
                        outer.onStage(Stage.QUERIES);
                    }
                }
            }, report);
        } catch (Exception error) {
            report.append("probe_error=").append(VitureProbe.errorText(error)).append('\n');
            VitureUsb.logW("device probe failed", error);
        } finally {
            if (session != null) {
                session.close();
            }
        }
    }

    private static void appendInstallId(Context context, StringBuilder report) {
        report.append("install_id: ").append(LogDrop.installId(context)).append('\n');
    }

    private static void appendAxisMap(Context context, StringBuilder report) {
        // §VITURE：緊接在匿名 ID 後面，開發者打開就能核對測試者設的軸向。
        try {
            SharedPreferences prefs = context.getSharedPreferences(VitureAxisMap.PREFS_NAME, Context.MODE_PRIVATE);
            String saved = prefs.getString(VitureAxisMap.PREFS_KEY, null);
            if (saved == null || saved.length() == 0) {
                report.append("axis_map=").append(VitureAxisMap.DEFAULT).append(" (default)\n");
                return;
            }
            String canonical = VitureAxisMap.canonical(saved);
            if (canonical == null) {
                String flat = oneLine(saved).trim();
                if (flat.length() == 0) {
                    flat = "(empty)";
                } else if (flat.length() > 80) {
                    // §VITURE：貼進來的錯誤內容可能很長，第一行只留前段。
                    flat = flat.substring(0, 80);
                }
                report.append("axis_map=").append(flat).append(" (invalid)\n");
                return;
            }
            report.append("axis_map=").append(canonical).append('\n');
        } catch (RuntimeException error) {
            report.append("axis_map=unavailable\n");
            VitureUsb.logW("axis map read failed", error);
        }
    }

    private static void appendHeader(StringBuilder report) {
        report.append("VipleStream glasses diagnostics\n");
        String version = BuildConfig.VERSION_NAME;
        report.append("app=").append(version == null || version.length() == 0 ? "unknown" : version).append('\n');
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        report.append("time_utc=").append(format.format(new Date())).append('\n');
        report.append("manufacturer=").append(oneLine(Build.MANUFACTURER)).append('\n');
        report.append("model=").append(oneLine(Build.MODEL)).append('\n');
        report.append("android=").append(oneLine(Build.VERSION.RELEASE)).append('\n');
        report.append("sdk=").append(Build.VERSION.SDK_INT).append('\n');
        report.append("soc=").append(socModel()).append('\n');
        report.append('\n');
    }

    private static void appendDisplays(Context context, StringBuilder report) {
        report.append("DISPLAYS\n");
        DisplayManager manager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        if (manager == null) {
            report.append("count=0\n");
            report.append("display_manager=unavailable\n\n");
            return;
        }
        Display[] displays;
        try {
            // §VITURE：診斷要的是 Android 看到的全部顯示器，不能沿用串流那個 presentation 過濾。
            displays = manager.getDisplays();
        } catch (RuntimeException error) {
            report.append("error=").append(VitureProbe.errorText(error)).append("\n\n");
            return;
        }
        if (displays == null) {
            displays = new Display[0];
        }
        report.append("count=").append(displays.length).append('\n');
        for (int i = 0; i < displays.length; i++) {
            appendOneDisplay(displays[i], report);
        }
        report.append('\n');
    }

    private static void appendOneDisplay(Display display, StringBuilder report) {
        if (display == null) {
            report.append("display missing\n");
            return;
        }
        try {
            report.append("display id=").append(display.getDisplayId())
                    .append(" name=").append(VitureUsb.quoted(display.getName()))
                    .append(" flags=").append(formatFlags(display.getFlags()))
                    .append(" state=").append(display.getState())
                    .append(' ').append(stateName(display.getState()))
                    .append(" current=").append(formatMode(display.getMode()))
                    .append('\n');
            Display.Mode[] modes = display.getSupportedModes();
            if (modes == null || modes.length == 0) {
                report.append("  mode none\n");
            } else {
                for (int i = 0; i < modes.length; i++) {
                    report.append("  mode ").append(formatMode(modes[i])).append('\n');
                }
            }
            report.append("  hdr=").append(hdrTypes(display)).append('\n');
            appendMetrics(display, report);
        } catch (RuntimeException error) {
            report.append("display id=").append(display.getDisplayId())
                    .append(" error=").append(VitureProbe.errorText(error)).append('\n');
        }
    }

    @SuppressWarnings("deprecation")
    private static void appendMetrics(Display display, StringBuilder report) {
        // §VITURE：getMetrics 會扣掉系統裝飾，外接眼鏡的真實像素要用 getRealMetrics。
        DisplayMetrics metrics = new DisplayMetrics();
        display.getRealMetrics(metrics);
        report.append("  metrics=").append(metrics.widthPixels).append('x').append(metrics.heightPixels)
                .append(" xdpi=").append(String.format(Locale.US, "%.1f", metrics.xdpi))
                .append(" ydpi=").append(String.format(Locale.US, "%.1f", metrics.ydpi))
                .append(" densityDpi=").append(metrics.densityDpi)
                .append('\n');
    }

    private static String formatMode(Display.Mode mode) {
        if (mode == null) {
            return "none";
        }
        return mode.getPhysicalWidth() + "x" + mode.getPhysicalHeight()
                + "@" + formatHz(mode.getRefreshRate())
                + " modeId=" + mode.getModeId();
    }

    private static String formatHz(float hz) {
        String text = String.format(Locale.US, "%.3f", hz);
        int end = text.length();
        while (end > 0 && text.charAt(end - 1) == '0') {
            end--;
        }
        if (end > 0 && text.charAt(end - 1) == '.') {
            end--;
        }
        if (end <= 0) {
            return "0";
        }
        return text.substring(0, end);
    }

    private static String formatFlags(int flags) {
        StringBuilder names = new StringBuilder();
        addFlag(names, flags, Display.FLAG_SUPPORTS_PROTECTED_BUFFERS, "PROTECTED_BUFFERS");
        addFlag(names, flags, Display.FLAG_SECURE, "SECURE");
        addFlag(names, flags, Display.FLAG_PRIVATE, "PRIVATE");
        addFlag(names, flags, Display.FLAG_PRESENTATION, "PRESENTATION");
        addFlag(names, flags, Display.FLAG_ROUND, "ROUND");
        String hex = String.format(Locale.US, "0x%08X", flags);
        if (names.length() == 0) {
            return hex;
        }
        return hex + " " + names;
    }

    private static void addFlag(StringBuilder names, int flags, int mask, String name) {
        if ((flags & mask) == 0) {
            return;
        }
        if (names.length() > 0) {
            names.append(' ');
        }
        names.append(name);
    }

    private static String stateName(int state) {
        switch (state) {
            case Display.STATE_OFF:
                return "OFF";
            case Display.STATE_ON:
                return "ON";
            case Display.STATE_DOZE:
                return "DOZE";
            case Display.STATE_DOZE_SUSPEND:
                return "DOZE_SUSPEND";
            case Display.STATE_VR:
                return "VR";
            case Display.STATE_ON_SUSPEND:
                return "ON_SUSPEND";
            default:
                return "STATE_" + state;
        }
    }

    private static String hdrTypes(Display display) {
        Display.HdrCapabilities hdr = display.getHdrCapabilities();
        if (hdr == null) {
            return "NONE";
        }
        int[] types = hdr.getSupportedHdrTypes();
        if (types == null || types.length == 0) {
            return "NONE";
        }
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < types.length; i++) {
            if (i > 0) {
                out.append(',');
            }
            out.append(hdrName(types[i]));
        }
        return out.toString();
    }

    private static String hdrName(int type) {
        switch (type) {
            case Display.HdrCapabilities.HDR_TYPE_DOLBY_VISION:
                return "DOLBY_VISION";
            case Display.HdrCapabilities.HDR_TYPE_HDR10:
                return "HDR10";
            case Display.HdrCapabilities.HDR_TYPE_HLG:
                return "HLG";
            case 4:
                // §VITURE：HDR10_PLUS 的常數在 API 29 才加進 SDK，數值 4 從那時就固定，舊系統直接比數字。
                return "HDR10_PLUS";
            default:
                return "TYPE_" + type;
        }
    }

    private static String socModel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return "unavailable";
        }
        try {
            // §VITURE：SOC_MODEL 是 API 31 才有的欄位，直接引用在 Android 9/10 驗證方法時會失敗。
            Field field = Build.class.getField("SOC_MODEL");
            Object value = field.get(null);
            if (!(value instanceof String) || ((String) value).length() == 0) {
                return "unknown";
            }
            return oneLine((String) value);
        } catch (Exception error) {
            return "unavailable";
        }
    }

    private static String oneLine(String value) {
        if (value == null) {
            return "";
        }
        return value.replace('\n', ' ').replace('\r', ' ');
    }
}
