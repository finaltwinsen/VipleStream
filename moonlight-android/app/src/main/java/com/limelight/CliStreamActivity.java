package com.limelight;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.widget.Toast;

import com.limelight.binding.PlatformBinding;
import com.limelight.computers.ComputerDatabaseManager;
import com.limelight.computers.IdentityManager;
import com.limelight.glasses.GlassesVrLaunch;
import com.limelight.glasses.VitureAxisMap;
import com.limelight.glasses.VitureUsb;
import com.limelight.glasses.VitureV2Protocol;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.http.NvHTTP;
import com.limelight.utils.AppUpdater;
import com.limelight.utils.CacheHelper;
import com.limelight.utils.QuestSupport;
import com.limelight.vr.QxrNative;
import com.limelight.vr.VrStreamActivity;

import org.xmlpull.v1.XmlPullParserException;

import java.io.IOException;
import java.io.StringReader;
import java.security.cert.CertificateEncodingException;
import java.util.List;

/**
 * VipleStream CLI streaming entry point — launch streaming directly via adb.
 *
 * Usage:
 *   adb shell am start -n com.piinsta/com.limelight.CliStreamActivity \
 *     --es host "<host>" \
 *     --es app "Desktop" \
 *     [--ei bitrate 20000]           # kbps
 *     [--ei fps 30]                  # target FPS
 *     [--ei width 1920]              # resolution width
 *     [--ei height 1080]             # resolution height
 *     [--ei codec 1]                 # 0=H264 1=HEVC 2=AV1
 *     [--ez autoAdjustBitrate true]  # ABR on/off
 *     [--ez enableMpQuic true]       # QUIC multipath on/off
 *     [--ez enableFruc true]         # FRUC frame interpolation on/off
 *     [--ez perfOverlay true]        # performance overlay on/off
 *     [--ez external_display true]   # 有外接顯示器時把串流畫面放到外接顯示器；false 維持只畫在手機
 *     [--ez vr_screen false]         # true：Quest 且載入得到 viple-openxr 時改開沉浸式 2D 畫面
 *     [--ez vr_pcvr false]           # true：Quest 且載入得到 viple-openxr 時改開頭盔 PCVR。app 先查一般清單，找不到再查 applist?vr=1。沒帶 vr_eye／vr_hz 就用 runtime
 *     [--ez vr_emulate false]        # true 且載入得到 viple-openxr：手機上協商真的 PCVR，合成姿態上行，畫面仍是 2D
 *     [--es vr_eye 1440x1440]        # 單眼寬高；串流解析度是兩眼並排
 *     [--ei vr_hz 72]                # 顯示更新率，tracking 送出頻率是它的 4 倍
 *     [--es vr_motion sine]          # still / sine / yaw30 / tilt30yaw / fast；預設 sine
 *     [--ez glasses_vr false]        # true：VITURE 眼鏡當 3DoF PCVR。要載入得到 viple-openxr，而且找得到眼鏡 USB
 *     [--es glasses_axis +x,+y,+z]   # 覆寫並存下軸向對應。格式錯誤就改回預設
 *
 * 測試自我更新（§REVIEW-1127 android #1，原本帶在 PcView 上）：
 *   adb shell am start -n com.piinsta/com.limelight.CliStreamActivity --es dev_update_url "<base url>"
 *   帶了 dev_update_url 就只設定測試更新網址並回到主畫面，不開串流（不需要 host）。
 *
 * Prerequisites: the host must already be paired via the normal UI flow.
 */
public class CliStreamActivity extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Intent in = getIntent();

        // §REVIEW-1127 android #1：dev_update_url 原本由 exported 的 PcView 接收，任何 App 都能觸發；
        // 改由這個需要 DUMP 權限的入口接收（adb shell 有、一般 App 沒有）。設好之後回到 PcView，
        // 由它的 onResume 照原流程跑一次測試更新；串流中就不切畫面，等回到 PcView 時再跑。
        if (in.hasExtra(AppUpdater.EXTRA_DEV_UPDATE_URL)) {
            AppUpdater.setDevUpdateUrl(in.getStringExtra(AppUpdater.EXTRA_DEV_UPDATE_URL));
            if (!Game.isStreaming() && !VrStreamActivity.isStreaming()) {
                Intent pcView = new Intent(this, PcView.class);
                pcView.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
                startActivity(pcView);
            }
            finish();
            return;
        }

        String host = in.getStringExtra("host");
        String appName = in.getStringExtra("app");

        if (host == null || host.isEmpty()) {
            Toast.makeText(this, "CLI: missing 'host' extra", Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        if (appName == null || appName.isEmpty()) {
            appName = "Desktop";
        }

        final String fHost = host;
        final String fAppName = appName;

        new Thread(() -> resolve(fHost, fAppName)).start();
    }

    private void resolve(String host, String appName) {
        storeGlassesAxis(getIntent());
        boolean forwardGlasses = resolveGlassesVr();
        ComputerDatabaseManager dbManager = new ComputerDatabaseManager(this);
        try {
        ComputerDetails computer = findComputerByHost(dbManager, host);

        if (computer == null) {
            showError("CLI: no paired computer found for host '" + host + "'");
            return;
        }

        if (computer.serverCert == null) {
            showError("CLI: computer '" + computer.name + "' has no server certificate (not paired?)");
            return;
        }

        IdentityManager idManager = new IdentityManager(this);
        String uniqueId = idManager.getUniqueId();

        // §QUEST-VR：VR 專用的 app 只在 applist?vr=1 裡，快取的 2D 清單找不到。
        // §VITURE：眼鏡 VR 同樣要開 PCVR session，所以也走 VR app 清單。條件不成立就不改原本的清單。
        // §QUEST-PCVR：條件不成立就不要改清單，後面仍照原本的方式開 Game。
        // 條件成立時先走一般清單；找不到才查 applist?vr=1，VR 專用的 app 才進得去。
        boolean vrPcvrRequested = getIntent().getBooleanExtra("vr_pcvr", false);
        boolean vrPcvr = false;
        if (vrPcvrRequested) {
            if (!QuestSupport.isQuest(this)) {
                LimeLog.info("[VIPLE-QXR-ACT] vr_pcvr=true but this is not a Quest; starting Game");
            } else if (!QxrNative.isAvailable()) {
                LimeLog.info("[VIPLE-QXR-ACT] vr_pcvr=true but viple-openxr is not loadable; starting Game");
            } else {
                vrPcvr = true;
            }
        }
        boolean wantVrApps = forwardGlasses || getIntent().getBooleanExtra("vr_emulate", false);
        NvApp app = vrPcvr
                ? resolveApp(computer, appName, uniqueId, false)
                : resolveApp(computer, appName, uniqueId, wantVrApps);
        if (app == null && vrPcvr) {
            LimeLog.info("[VIPLE-QXR-ACT] '" + appName + "' not in the flat app list; querying applist?vr=1");
            app = resolveApp(computer, appName, uniqueId, true);
        }
        if (app == null) {
            showError("CLI: app '" + appName + "' not found on '" + computer.name + "'");
            return;
        }

        ComputerDetails.AddressTuple addr = computer.activeAddress;
        if (addr == null) addr = computer.localAddress;
        if (addr == null) addr = computer.remoteAddress;
        if (addr == null) addr = computer.manualAddress;
        if (addr == null) {
            addr = new ComputerDetails.AddressTuple(host, NvHTTP.DEFAULT_HTTP_PORT);
        }

        // §N.5 fix (2026-06-18): computer.httpsPort loaded from the local DB is
        // often 0 — the normal UI populates it via a serverinfo poll before
        // launching Game, but the CLI path skipped that. httpsPort==0 makes
        // Game.startFileTransferClient() bail with "missing host/port/cert".
        // Poll serverinfo here so the in-stream file transfer client can start.
        int httpsPort = computer.httpsPort;
        if (httpsPort == 0) {
            try {
                NvHTTP probe = new NvHTTP(addr, 0, uniqueId, computer.serverCert,
                        PlatformBinding.getCryptoProvider(this));
                httpsPort = probe.getComputerDetails(true).httpsPort;
                LimeLog.info("[VIPLE-CLI] resolved httpsPort=" + httpsPort + " via serverinfo");
            } catch (Exception e) {
                LimeLog.warning("[VIPLE-CLI] httpsPort serverinfo fetch failed: " + e.getMessage());
            }
        }

        boolean forwardVrEmulate = getIntent().getBooleanExtra("vr_emulate", false);
        if (forwardGlasses) {
            // §VITURE：眼鏡路徑自己帶姿態。不要再把合成的 vr_emulate 傳進 Game。
            forwardVrEmulate = false;
        }
        if (forwardVrEmulate && !QxrNative.isAvailable()) {
            // §QUEST-VR：.so 載入失敗就不要把 vr_emulate 傳進 Game，後面照原本的方式開。
            LimeLog.info("[VIPLE-QXR-CLI] vr_emulate=true but viple-openxr is not loadable; starting a normal stream");
            forwardVrEmulate = false;
        }
        boolean vrScreen = getIntent().getBooleanExtra("vr_screen", false);
        Class<?> streamActivity = Game.class;
        if (vrPcvr) {
            // §QUEST-PCVR：條件已經在上面擋過。這裡成立才進頭盔，不成立時下面的分支跟以前一樣。
            streamActivity = VrStreamActivity.class;
            LimeLog.info("[VIPLE-QXR-ACT] vr_pcvr=true; starting VrStreamActivity");
        } else if (forwardVrEmulate) {
            // §QUEST-VR：模擬模式留在 Game，手機螢幕照常解碼。不要另開 VrStreamActivity 的 OpenXR。
            LimeLog.info("[VIPLE-QXR-CLI] vr_emulate=true; starting Game");
        } else if (vrScreen) {
            // §QUEST-VR：條件不成立就維持原本的 Game，只多一行 log 說明原因。
            if (!QuestSupport.isQuest(this)) {
                LimeLog.info("[VIPLE-QXR-CLI] vr_screen=true but this is not a Quest; starting Game");
            } else if (!QxrNative.isAvailable()) {
                LimeLog.info("[VIPLE-QXR-CLI] vr_screen=true but viple-openxr is not loadable; starting Game");
            } else {
                streamActivity = VrStreamActivity.class;
            }
        }
        Intent gameIntent = new Intent(this, streamActivity);
        gameIntent.putExtra(Game.EXTRA_HOST, addr.address);
        gameIntent.putExtra(Game.EXTRA_PORT, addr.port);
        gameIntent.putExtra(Game.EXTRA_HTTPS_PORT, httpsPort);
        gameIntent.putExtra(Game.EXTRA_APP_NAME, app.getAppName());
        gameIntent.putExtra(Game.EXTRA_APP_ID, app.getAppId());
        gameIntent.putExtra(Game.EXTRA_APP_HDR, app.isHdrSupported());
        gameIntent.putExtra(Game.EXTRA_UNIQUEID, uniqueId);
        gameIntent.putExtra(Game.EXTRA_PC_UUID, computer.uuid);
        gameIntent.putExtra(Game.EXTRA_PC_NAME, computer.name);
        try {
            if (computer.serverCert != null) {
                gameIntent.putExtra(Game.EXTRA_SERVER_CERT, computer.serverCert.getEncoded());
            }
        } catch (CertificateEncodingException e) {
            e.printStackTrace();
        }

        Intent in = getIntent();

        if (in.hasExtra("bitrate"))
            gameIntent.putExtra("cli_bitrate", in.getIntExtra("bitrate", 0));
        if (in.hasExtra("fps"))
            gameIntent.putExtra("cli_fps", in.getIntExtra("fps", 0));
        if (in.hasExtra("width"))
            gameIntent.putExtra("cli_width", in.getIntExtra("width", 0));
        if (in.hasExtra("height"))
            gameIntent.putExtra("cli_height", in.getIntExtra("height", 0));
        if (in.hasExtra("codec"))
            gameIntent.putExtra("cli_codec", in.getIntExtra("codec", -1));
        if (in.hasExtra("autoAdjustBitrate"))
            gameIntent.putExtra("cli_autoAdjustBitrate", in.getBooleanExtra("autoAdjustBitrate", true));
        if (in.hasExtra("enableMpQuic"))
            gameIntent.putExtra("cli_enableMpQuic", in.getBooleanExtra("enableMpQuic", false));
        // §QXR-VIDEO-TEST：開發用，見 Game.startQxrVideoTestIfRequested。
        if (in.hasExtra("qxr_video_test"))
            gameIntent.putExtra("qxr_video_test", in.getBooleanExtra("qxr_video_test", false));
        if (in.hasExtra("qxr_video_test_srgb"))
            gameIntent.putExtra("qxr_video_test_srgb", in.getBooleanExtra("qxr_video_test_srgb", false));
        if (in.hasExtra("enableFruc"))
            gameIntent.putExtra("cli_enableFruc", in.getBooleanExtra("enableFruc", false));
        if (in.hasExtra("perfOverlay"))
            gameIntent.putExtra("cli_perfOverlay", in.getBooleanExtra("perfOverlay", false));
        if (in.hasExtra("external_display"))
            gameIntent.putExtra("cli_externalDisplay", in.getBooleanExtra("external_display", true));
        if (vrPcvr) {
            // §QUEST-PCVR：沒帶 vr_eye／vr_hz 就不要塞預設值，Activity 才會改用 runtime 的建議尺寸與更新率。
            gameIntent.putExtra("vr_pcvr", true);
            if (in.hasExtra("vr_eye")) {
                String eye = in.getStringExtra("vr_eye");
                if (eye != null && !eye.isEmpty()) {
                    gameIntent.putExtra("vr_eye", eye);
                }
            }
            if (in.hasExtra("vr_hz")) {
                gameIntent.putExtra("vr_hz", in.getIntExtra("vr_hz", 0));
            }
        } else if (forwardGlasses) {
            GlassesVrLaunch.applyExtras(gameIntent, in.getStringExtra("vr_eye"),
                    in.getIntExtra("vr_hz", GlassesVrLaunch.DEFAULT_HZ));
        } else if (forwardVrEmulate) {
            String eye = in.getStringExtra("vr_eye");
            if (eye == null || eye.isEmpty()) {
                eye = "1440x1440";
            }
            String motion = in.getStringExtra("vr_motion");
            if (motion == null || motion.isEmpty()) {
                motion = "sine";
            }
            gameIntent.putExtra("vr_emulate", true);
            gameIntent.putExtra("vr_eye", eye);
            gameIntent.putExtra("vr_hz", in.getIntExtra("vr_hz", 72));
            gameIntent.putExtra("vr_motion", motion);
        }

        LimeLog.info("[VIPLE-CLI] Launching stream: " + addr.address + " -> " + app.getAppName()
                     + " (appId=" + app.getAppId() + ")");

        startActivity(gameIntent);
        finish();
        } finally {
            // §REVIEW-1104：CLI 路徑打開 computers4.db 後沒關，連線留到 finalizer 才關並出現 SQLite leaked
            dbManager.close();
        }
    }

    private ComputerDetails findComputerByHost(ComputerDatabaseManager dbManager, String host) {
        List<ComputerDetails> allComputers = dbManager.getAllComputers();
        for (ComputerDetails c : allComputers) {
            if (matchesHost(c, host)) {
                return c;
            }
        }
        return dbManager.getComputerByName(host);
    }

    private boolean matchesHost(ComputerDetails c, String host) {
        if (matchesAddress(c.localAddress, host)) return true;
        if (matchesAddress(c.remoteAddress, host)) return true;
        if (matchesAddress(c.manualAddress, host)) return true;
        if (matchesAddress(c.ipv6Address, host)) return true;
        if (matchesAddress(c.activeAddress, host)) return true;
        return false;
    }

    private boolean matchesAddress(ComputerDetails.AddressTuple addr, String host) {
        return addr != null && host.equals(addr.address);
    }

    private NvApp resolveApp(ComputerDetails computer, String appName, String uniqueId, boolean vrApps) {
        if (!vrApps) try {
            String rawAppList = CacheHelper.readInputStreamToString(
                    CacheHelper.openCacheFileForInput(getCacheDir(), "applist", computer.uuid));
            if (rawAppList != null && !rawAppList.isEmpty()) {
                List<NvApp> apps = NvHTTP.getAppListByReader(new StringReader(rawAppList));
                for (NvApp a : apps) {
                    if (a.getAppName().equalsIgnoreCase(appName)) {
                        return a;
                    }
                }
            }
        } catch (IOException | XmlPullParserException e) {
            LimeLog.warning("[VIPLE-CLI] Cached app list unavailable: " + e.getMessage());
        }

        try {
            ComputerDetails.AddressTuple addr = computer.activeAddress;
            if (addr == null) addr = computer.localAddress;
            if (addr == null) addr = computer.remoteAddress;
            if (addr == null) return null;

            NvHTTP http = new NvHTTP(addr, computer.httpsPort, uniqueId,
                    computer.serverCert, PlatformBinding.getCryptoProvider(this));
            List<NvApp> apps = vrApps ? http.getVrAppList() : http.getAppList();
            for (NvApp a : apps) {
                if (a.getAppName().equalsIgnoreCase(appName)) {
                    return a;
                }
            }
        } catch (Exception e) {
            LimeLog.warning("[VIPLE-CLI] Server app list query failed: " + e.getMessage());
        }

        return null;
    }

    private boolean resolveGlassesVr() {
        if (!getIntent().getBooleanExtra("glasses_vr", false)) {
            return false;
        }
        if (!QxrNative.isAvailable()) {
            LimeLog.info("[VIPLE-VITURE] glasses_vr=true but viple-openxr is not loadable; starting a normal stream");
            return false;
        }
        UsbManager usb = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (usb == null) {
            LimeLog.info("[VIPLE-VITURE] glasses_vr=true but usb manager is unavailable; starting a normal stream");
            return false;
        }
        UsbDevice device = VitureUsb.findGlassesDevice(usb);
        if (device == null) {
            LimeLog.info("[VIPLE-VITURE] glasses_vr=true but no VITURE glasses USB device; starting a normal stream");
            return false;
        }
        LimeLog.info("[VIPLE-VITURE] glasses_vr=true vid=" + VitureV2Protocol.hex16(device.getVendorId())
                + " pid=" + VitureV2Protocol.hex16(device.getProductId()) + "; starting Game");
        return true;
    }

    private void storeGlassesAxis(Intent in) {
        if (in == null || !in.hasExtra("glasses_axis")) {
            return;
        }
        String raw = in.getStringExtra("glasses_axis");
        String canonical = VitureAxisMap.canonical(raw);
        SharedPreferences prefs = getSharedPreferences(VitureAxisMap.PREFS_NAME, MODE_PRIVATE);
        if (canonical == null) {
            LimeLog.info("[VIPLE-VITURE] glasses_axis invalid '" + raw + "'; using " + VitureAxisMap.DEFAULT);
            prefs.edit().putString(VitureAxisMap.PREFS_KEY, VitureAxisMap.DEFAULT).commit();
            return;
        }
        prefs.edit().putString(VitureAxisMap.PREFS_KEY, canonical).commit();
        LimeLog.info("[VIPLE-VITURE] glasses_axis=" + canonical);
    }

    private void showError(String msg) {
        LimeLog.severe(msg);
        runOnUiThread(() -> {
            Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
            finish();
        });
    }
}
