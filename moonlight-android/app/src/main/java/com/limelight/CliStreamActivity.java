package com.limelight;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Toast;

import com.limelight.binding.PlatformBinding;
import com.limelight.computers.ComputerDatabaseManager;
import com.limelight.computers.IdentityManager;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.http.NvHTTP;
import com.limelight.utils.CacheHelper;

import org.xmlpull.v1.XmlPullParserException;

import java.io.IOException;
import java.io.StringReader;
import java.security.cert.CertificateEncodingException;
import java.util.List;

/**
 * VipleStream CLI streaming entry point — launch streaming directly via adb.
 *
 * Usage:
 *   adb shell am start -n com.piinsta.debug/com.limelight.CliStreamActivity \
 *     --es host "192.168.51.226" \
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
 *
 * Prerequisites: the host must already be paired via the normal UI flow.
 */
public class CliStreamActivity extends Activity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Intent in = getIntent();
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
        ComputerDatabaseManager dbManager = new ComputerDatabaseManager(this);
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

        NvApp app = resolveApp(computer, appName, uniqueId);
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

        Intent gameIntent = new Intent(this, Game.class);
        gameIntent.putExtra(Game.EXTRA_HOST, addr.address);
        gameIntent.putExtra(Game.EXTRA_PORT, addr.port);
        gameIntent.putExtra(Game.EXTRA_HTTPS_PORT, computer.httpsPort);
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
        if (in.hasExtra("enableFruc"))
            gameIntent.putExtra("cli_enableFruc", in.getBooleanExtra("enableFruc", false));
        if (in.hasExtra("perfOverlay"))
            gameIntent.putExtra("cli_perfOverlay", in.getBooleanExtra("perfOverlay", false));

        LimeLog.info("[VIPLE-CLI] Launching stream: " + addr.address + " -> " + app.getAppName()
                     + " (appId=" + app.getAppId() + ")");

        startActivity(gameIntent);
        finish();
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

    private NvApp resolveApp(ComputerDetails computer, String appName, String uniqueId) {
        try {
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
            List<NvApp> apps = http.getAppList();
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

    private void showError(String msg) {
        LimeLog.severe(msg);
        runOnUiThread(() -> {
            Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
            finish();
        });
    }
}
