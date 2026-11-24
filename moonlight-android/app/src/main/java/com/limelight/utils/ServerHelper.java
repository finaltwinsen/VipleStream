package com.limelight.utils;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.widget.Toast;

import com.limelight.AppView;
import com.limelight.Game;
import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.ShortcutTrampoline;
import com.limelight.glasses.GlassesVrLaunch;
import com.limelight.binding.PlatformBinding;
import com.limelight.computers.ComputerManagerService;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.HostHttpResponseException;
import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.http.NvHTTP;
import com.limelight.nvstream.jni.MoonBridge;

import org.xmlpull.v1.XmlPullParserException;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.net.UnknownHostException;
import java.security.cert.CertificateEncodingException;
import java.util.ArrayList;
import java.util.List;

public class ServerHelper {
    public static final String CONNECTION_TEST_SERVER = "android.conntest.moonlight-stream.org";

    public static ComputerDetails.AddressTuple getCurrentAddressFromComputer(ComputerDetails computer) throws IOException {
        if (computer.activeAddress == null) {
            throw new IOException("No active address for "+computer.name);
        }
        return computer.activeAddress;
    }

    public static Intent createPcShortcutIntent(Activity parent, ComputerDetails computer) {
        Intent i = new Intent(parent, ShortcutTrampoline.class);
        i.putExtra(AppView.NAME_EXTRA, computer.name);
        i.putExtra(AppView.UUID_EXTRA, computer.uuid);
        i.setAction(Intent.ACTION_DEFAULT);
        return i;
    }

    public static Intent createAppShortcutIntent(Activity parent, ComputerDetails computer, NvApp app) {
        Intent i = new Intent(parent, ShortcutTrampoline.class);
        i.putExtra(AppView.NAME_EXTRA, computer.name);
        i.putExtra(AppView.UUID_EXTRA, computer.uuid);
        i.putExtra(Game.EXTRA_APP_NAME, app.getAppName());
        i.putExtra(Game.EXTRA_APP_ID, ""+app.getAppId());
        i.putExtra(Game.EXTRA_APP_HDR, app.isHdrSupported());
        i.setAction(Intent.ACTION_DEFAULT);
        return i;
    }

    public static Intent createStartIntent(Activity parent, NvApp app, ComputerDetails computer,
                                           ComputerManagerService.ComputerManagerBinder managerBinder) {
        Intent intent = new Intent(parent, Game.class);
        intent.putExtra(Game.EXTRA_HOST, computer.activeAddress.address);
        intent.putExtra(Game.EXTRA_PORT, computer.activeAddress.port);
        intent.putExtra(Game.EXTRA_HTTPS_PORT, computer.httpsPort);
        intent.putExtra(Game.EXTRA_APP_NAME, app.getAppName());
        intent.putExtra(Game.EXTRA_APP_ID, app.getAppId());
        intent.putExtra(Game.EXTRA_APP_HDR, app.isHdrSupported());
        intent.putExtra(Game.EXTRA_UNIQUEID, managerBinder.getUniqueId());
        intent.putExtra(Game.EXTRA_PC_UUID, computer.uuid);
        intent.putExtra(Game.EXTRA_PC_NAME, computer.name);
        // VipleStream §M.parity W1: propagate server codec mode support
        // so Game can intersect supportedVideoFormats with host caps.
        intent.putExtra(Game.EXTRA_SERVER_CODEC_MODE_SUPPORT, computer.serverCodecModeSupport);
        // VipleStream: Pass STUN address for relay NAT traversal
        if (computer.stunAddress != null) {
            intent.putExtra(Game.EXTRA_STUN_ADDRESS, computer.stunAddress);
            intent.putExtra(Game.EXTRA_STUN_PUNCH_PORT, computer.stunPunchPort);  // §REVIEW-1104
        }
        try {
            if (computer.serverCert != null) {
                intent.putExtra(Game.EXTRA_SERVER_CERT, computer.serverCert.getEncoded());
            }
        } catch (CertificateEncodingException e) {
            e.printStackTrace();
        }
        return intent;
    }

    public static void doStart(Activity parent, NvApp app, ComputerDetails computer,
                               ComputerManagerService.ComputerManagerBinder managerBinder) {
        if (computer.state == ComputerDetails.State.OFFLINE || computer.activeAddress == null) {
            Toast.makeText(parent, parent.getResources().getString(R.string.pair_pc_offline), Toast.LENGTH_SHORT).show();
            return;
        }
        parent.startActivity(createStartIntent(parent, app, computer, managerBinder));
    }

    public static void doStartGlassesVr(Activity parent, NvApp app, ComputerDetails computer,
                                        ComputerManagerService.ComputerManagerBinder managerBinder) {
        if (computer == null || computer.state == ComputerDetails.State.OFFLINE || computer.activeAddress == null) {
            Toast.makeText(parent, parent.getResources().getString(R.string.pair_pc_offline), Toast.LENGTH_SHORT).show();
            return;
        }
        if (managerBinder == null) {
            Toast.makeText(parent, parent.getResources().getString(R.string.error_manager_not_running), Toast.LENGTH_LONG).show();
            return;
        }
        Intent intent = createStartIntent(parent, app, computer, managerBinder);
        // §VITURE：介面沒有 adb 的 vr_eye／vr_hz，跟 CLI 沒帶這兩個參數時用同一組預設。
        GlassesVrLaunch.applyExtras(intent, null, GlassesVrLaunch.DEFAULT_HZ);
        LimeLog.info("[VIPLE-VITURE] glasses_vr start appId=" + app.getAppId() + " name=" + app.getAppName());
        parent.startActivity(intent);
    }

    /**
     * §VITURE：跟 Frame 版的 VR 模式一樣，列出 host 標成 VR 的 app（VR 遊戲與 SteamVR Home）讓使用者選。
     * host 那邊照常由 server 帶起 SteamVR 與遊戲，使用者不必自己操作 SteamVR。
     */
    public static void doPickGlassesVrApp(final Activity parent, final ComputerDetails computer,
                                          final ComputerManagerService.ComputerManagerBinder managerBinder) {
        if (computer == null || computer.state == ComputerDetails.State.OFFLINE || computer.activeAddress == null) {
            Toast.makeText(parent, parent.getResources().getString(R.string.pair_pc_offline), Toast.LENGTH_SHORT).show();
            return;
        }
        if (managerBinder == null) {
            Toast.makeText(parent, parent.getResources().getString(R.string.error_manager_not_running), Toast.LENGTH_LONG).show();
            return;
        }
        final SpinnerDialog spinner = SpinnerDialog.displayDialog(parent,
                parent.getResources().getString(R.string.glasses_vr_pick_title),
                parent.getResources().getString(R.string.glasses_vr_pick_loading),
                false);
        final ComputerManagerService.ComputerManagerBinder binder = managerBinder;
        // §VITURE：applist?vr=1 會打到主機，不能在主執行緒上等。
        new Thread(new Runnable() {
            @Override
            public void run() {
                final List<NvApp> vrApps = new ArrayList<>();
                boolean lookupFailed = false;
                try {
                    NvHTTP http = new NvHTTP(getCurrentAddressFromComputer(computer), computer.httpsPort,
                            binder.getUniqueId(), computer.serverCert, PlatformBinding.getCryptoProvider(parent));
                    List<NvApp> apps = http.getVrAppList();
                    if (apps != null) {
                        for (NvApp app : apps) {
                            if (app != null && app.isVr() && app.getAppName() != null) {
                                vrApps.add(app);
                            }
                        }
                    }
                    LimeLog.info("[VIPLE-VITURE] applist?vr=1 total=" + (apps == null ? 0 : apps.size())
                            + " vr=" + vrApps.size());
                } catch (Exception error) {
                    lookupFailed = true;
                    LimeLog.warning("[VIPLE-VITURE] applist?vr=1 failed: " + error.getMessage());
                }
                final boolean failed = lookupFailed;
                spinner.dismiss();
                parent.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (parent.isFinishing() || parent.isDestroyed()) {
                            return;
                        }
                        if (failed) {
                            Toast.makeText(parent, parent.getResources().getString(R.string.glasses_vr_pick_failed), Toast.LENGTH_LONG).show();
                            return;
                        }
                        if (vrApps.isEmpty()) {
                            Toast.makeText(parent, parent.getResources().getString(R.string.glasses_vr_pick_none), Toast.LENGTH_LONG).show();
                            return;
                        }
                        CharSequence[] names = new CharSequence[vrApps.size()];
                        for (int i = 0; i < names.length; i++) {
                            names[i] = vrApps.get(i).getAppName();
                        }
                        new AlertDialog.Builder(parent)
                                .setTitle(R.string.glasses_vr_pick_title)
                                .setItems(names, new DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(DialogInterface dialog, int which) {
                                        doStartGlassesVr(parent, vrApps.get(which), computer, binder);
                                    }
                                })
                                .setNegativeButton(android.R.string.cancel, null)
                                .show();
                    }
                });
            }
        }, "viple-viture-vrapps").start();
    }

    public static void doNetworkTest(final Activity parent) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                SpinnerDialog spinnerDialog = SpinnerDialog.displayDialog(parent,
                        parent.getResources().getString(R.string.nettest_title_waiting),
                        parent.getResources().getString(R.string.nettest_text_waiting),
                        false);

                int ret = MoonBridge.testClientConnectivity(CONNECTION_TEST_SERVER, 443, MoonBridge.ML_PORT_FLAG_ALL);
                spinnerDialog.dismiss();

                String dialogSummary;
                if (ret == MoonBridge.ML_TEST_RESULT_INCONCLUSIVE) {
                    dialogSummary = parent.getResources().getString(R.string.nettest_text_inconclusive);
                }
                else if (ret == 0) {
                    dialogSummary = parent.getResources().getString(R.string.nettest_text_success);
                }
                else {
                    dialogSummary = parent.getResources().getString(R.string.nettest_text_failure);
                    dialogSummary += MoonBridge.stringifyPortFlags(ret, "\n");
                }

                Dialog.displayDialog(parent,
                        parent.getResources().getString(R.string.nettest_title_done),
                        dialogSummary,
                        false);
            }
        }).start();
    }

    public static void doQuit(final Activity parent,
                              final ComputerDetails computer,
                              final NvApp app,
                              final ComputerManagerService.ComputerManagerBinder managerBinder,
                              final Runnable onComplete) {
        Toast.makeText(parent, parent.getResources().getString(R.string.applist_quit_app) + " " + app.getAppName() + "...", Toast.LENGTH_SHORT).show();
        new Thread(new Runnable() {
            @Override
            public void run() {
                NvHTTP httpConn;
                String message;
                try {
                    httpConn = new NvHTTP(ServerHelper.getCurrentAddressFromComputer(computer), computer.httpsPort,
                            managerBinder.getUniqueId(), computer.serverCert, PlatformBinding.getCryptoProvider(parent));
                    if (httpConn.quitApp()) {
                        message = parent.getResources().getString(R.string.applist_quit_success) + " " + app.getAppName();
                    } else {
                        message = parent.getResources().getString(R.string.applist_quit_fail) + " " + app.getAppName();
                    }
                } catch (HostHttpResponseException e) {
                    if (e.getErrorCode() == 599) {
                        message = "This session wasn't started by this device," +
                                " so it cannot be quit. End streaming on the original " +
                                "device or the PC itself. (Error code: "+e.getErrorCode()+")";
                    }
                    else {
                        message = e.getMessage();
                    }
                } catch (UnknownHostException e) {
                    message = parent.getResources().getString(R.string.error_unknown_host);
                } catch (FileNotFoundException e) {
                    message = parent.getResources().getString(R.string.error_404);
                } catch (IOException | XmlPullParserException e) {
                    message = e.getMessage();
                    e.printStackTrace();
                } finally {
                    if (onComplete != null) {
                        onComplete.run();
                    }
                }

                final String toastMessage = message;
                parent.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        Toast.makeText(parent, toastMessage, Toast.LENGTH_LONG).show();
                    }
                });
            }
        }).start();
    }
}
