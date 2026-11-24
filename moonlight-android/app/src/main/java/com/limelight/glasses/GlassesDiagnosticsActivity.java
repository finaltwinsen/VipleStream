package com.limelight.glasses;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.app.ProgressDialog;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputFilter;
import android.text.InputType;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.limelight.R;
import com.limelight.utils.LogDrop;
import com.limelight.utils.UiHelper;
import com.limelight.utils.UpdateFileProvider;

import java.io.File;
import java.lang.ref.WeakReference;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class GlassesDiagnosticsActivity extends Activity implements GlassesDiagnostics.Host, GlassesManualTest.Host {
    private static final String STATE_REPORT = "glasses_report";
    private static final String STATE_AXIS_INPUT = "glasses_axis_input";
    private static final String LOGDROP_TAG = "VipleLogDrop";

    private TextView progressView;
    private TextView reportView;
    private Button startButton;
    private Button shareButton;
    private Button uploadButton;
    private Button copyButton;
    private Dialog uploadDialog;
    private boolean uploading;
    private Button mode3dButton;
    private Button mode2dButton;
    private Button mode120Button;
    private Button axisButton;
    private TextView axisCurrentView;
    private EditText axisInput;
    private TextView axisErrorView;
    private volatile VitureGlasses manualGlasses;

    private volatile boolean cancelled;
    private volatile boolean uiGone;
    private volatile boolean running;
    private volatile String permissionResult;
    private volatile String pendingDeviceName;
    private volatile CountDownLatch permissionLatch;
    private volatile GlassesDiagnostics.Stage lastStage = GlassesDiagnostics.Stage.DISPLAYS;
    private String reportText;
    private Thread probeThread;
    private boolean receiverRegistered;

    private final BroadcastReceiver permissionReceiver = new BroadcastReceiver() {
        @Override
        @SuppressWarnings("deprecation")
        public void onReceive(Context context, Intent intent) {
            if (intent == null || !VitureUsb.ACTION_USB_PERMISSION.equals(intent.getAction())) {
                return;
            }
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (device == null || pendingDeviceName == null || !pendingDeviceName.equals(device.getDeviceName())) {
                return;
            }
            boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
            permissionResult = granted ? "granted" : "denied";
            VitureUsb.logI("permission result=" + permissionResult
                    + " vid=" + VitureV2Protocol.hex16(device.getVendorId())
                    + " pid=" + VitureV2Protocol.hex16(device.getProductId()));
            CountDownLatch latch = permissionLatch;
            if (latch != null) {
                latch.countDown();
            }
        }
    };

    @Override
    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout root = buildLayout();
        setContentView(root);
        UiHelper.notifyNewRootView(this);
        UiHelper.applyStatusBarPadding(root);
        if (savedInstanceState != null) {
            reportText = savedInstanceState.getString(STATE_REPORT);
            if (reportText != null) {
                reportView.setText(reportText);
                progressView.setText(R.string.glasses_diag_progress_done);
                lastStage = GlassesDiagnostics.Stage.DONE;
            }
            String axisDraft = savedInstanceState.getString(STATE_AXIS_INPUT);
            if (axisDraft != null) {
                axisInput.setText(axisDraft);
            }
        }
        // §VITURE：輸入框若一開始就取得焦點，鍵盤會蓋住下面的按鈕。只改狀態位元，避免清掉原本的 adjust。
        int inputMode = getWindow().getAttributes().softInputMode;
        inputMode = (inputMode & ~WindowManager.LayoutParams.SOFT_INPUT_MASK_STATE)
                | WindowManager.LayoutParams.SOFT_INPUT_STATE_HIDDEN;
        getWindow().setSoftInputMode(inputMode);
        root.setFocusableInTouchMode(true);
        root.requestFocus();
        updateButtons();

        IntentFilter filter = new IntentFilter(VitureUsb.ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(permissionReceiver, filter, RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(permissionReceiver, filter);
        }
        receiverRegistered = true;
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateButtons();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        if (reportText != null) {
            outState.putString(STATE_REPORT, reportText);
        }
        if (axisInput != null && axisInput.getText() != null) {
            outState.putString(STATE_AXIS_INPUT, axisInput.getText().toString());
        }
    }

    @Override
    protected void onDestroy() {
        uiGone = true;
        // §VITURE：權限回呼在主執行緒。先標記取消並放行 latch，探測執行緒才關得了 IMU。
        cancelled = true;
        CountDownLatch latch = permissionLatch;
        if (latch != null) {
            latch.countDown();
        }
        VitureGlasses manual = manualGlasses;
        if (manual != null) {
            manual.requestStop();
        }
        Thread thread = probeThread;
        if (thread != null && thread != Thread.currentThread()) {
            try {
                // §VITURE：等執行緒送完關閉串流。切片讀寫最多約 1 秒，2 秒仍低於 ANR。
                thread.join(2000);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
            }
        }
        VitureUsb.closeActive();
        if (receiverRegistered) {
            unregisterReceiver(permissionReceiver);
            receiverRegistered = false;
        }
        // §LOGDROP：不上傳執行緒不在這裡等（網路最長約兩分鐘，等會拖過 ANR）。
        // 對話框綁著這個 Activity，先關掉才不會漏視窗；結果若在結束後才回來就不再畫。
        dismissUploadDialog();
        super.onDestroy();
    }

    @Override
    public boolean isCancelled() {
        return cancelled;
    }

    @Override
    public void onStage(final GlassesDiagnostics.Stage stage) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (isDestroyed()) {
                    return;
                }
                lastStage = stage;
                progressView.setText(messageFor(stage));
            }
        });
    }

    @Override
    public String ensurePermission(final UsbDevice device) {
        if (isCancelled()) {
            return "cancelled";
        }
        final UsbManager usb = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (usb == null) {
            return "error UsbManager unavailable";
        }
        try {
            if (usb.hasPermission(device)) {
                return "granted";
            }
        } catch (SecurityException error) {
            return "error " + VitureProbe.errorText(error);
        }
        permissionResult = null;
        pendingDeviceName = device.getDeviceName();
        final CountDownLatch latch = new CountDownLatch(1);
        permissionLatch = latch;
        if (isCancelled()) {
            return "cancelled";
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (isDestroyed() || isCancelled()) {
                    permissionResult = "cancelled";
                    latch.countDown();
                    return;
                }
                progressView.setText(R.string.glasses_diag_progress_permission);
                try {
                    VitureUsb.requestPermission(GlassesDiagnosticsActivity.this, usb, device);
                    VitureUsb.logI("permission request vid=" + VitureV2Protocol.hex16(device.getVendorId())
                            + " pid=" + VitureV2Protocol.hex16(device.getProductId()));
                } catch (SecurityException error) {
                    VitureUsb.logW("permission request blocked", error);
                    Toast.makeText(GlassesDiagnosticsActivity.this, R.string.error_usb_prohibited, Toast.LENGTH_LONG).show();
                    permissionResult = "error " + VitureProbe.errorText(error);
                    latch.countDown();
                }
            }
        });
        while (true) {
            int state = awaitLatch(latch);
            if (state < 0 || isCancelled()) {
                return "cancelled";
            }
            if (state > 0) {
                break;
            }
        }
        String result = permissionResult;
        return result == null ? "denied" : result;
    }

    private LinearLayout buildLayout() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);
        root.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // §VITURE：AppTheme 關掉了標題列，診斷名稱要自己畫，不然測試者不知道身在哪一頁。
        TextView titleView = new TextView(this);
        titleView.setText(R.string.title_glasses_diagnostics);
        titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        titleView.setTypeface(titleView.getTypeface(), android.graphics.Typeface.BOLD);
        root.addView(titleView);

        TextView explainer = new TextView(this);
        explainer.setText(R.string.glasses_diag_explainer);
        explainer.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        LinearLayout.LayoutParams explainerParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        explainerParams.topMargin = dp(8);
        explainerParams.bottomMargin = dp(12);
        root.addView(explainer, explainerParams);

        startButton = new Button(this);
        startButton.setText(R.string.glasses_diag_start);
        startButton.setAllCaps(false);
        startButton.setMinHeight(dp(48));
        startButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startProbe();
            }
        });
        root.addView(startButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        progressView = new TextView(this);
        progressView.setText(R.string.glasses_diag_progress_idle);
        progressView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        LinearLayout.LayoutParams progressParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        progressParams.topMargin = dp(8);
        progressParams.bottomMargin = dp(8);
        root.addView(progressView, progressParams);

        TextView manualTitle = new TextView(this);
        manualTitle.setText(R.string.glasses_diag_manual_title);
        manualTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        manualTitle.setTypeface(manualTitle.getTypeface(), android.graphics.Typeface.BOLD);
        LinearLayout.LayoutParams manualTitleParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        manualTitleParams.topMargin = dp(16);
        root.addView(manualTitle, manualTitleParams);

        TextView manualWarning = new TextView(this);
        manualWarning.setText(R.string.glasses_diag_manual_warning);
        manualWarning.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        LinearLayout.LayoutParams warningParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        warningParams.topMargin = dp(8);
        warningParams.bottomMargin = dp(8);
        root.addView(manualWarning, warningParams);

        mode3dButton = manualButton(R.string.glasses_diag_mode_3d, VitureV2Protocol.DISPLAY_3D_SBS_60);
        mode2dButton = manualButton(R.string.glasses_diag_mode_2d, VitureV2Protocol.DISPLAY_2D_60);
        mode120Button = manualButton(R.string.glasses_diag_mode_120, VitureV2Protocol.DISPLAY_2D_120);
        axisButton = manualButton(R.string.glasses_diag_axis, -1);
        root.addView(mode3dButton, manualButtonParams(0));
        root.addView(mode2dButton, manualButtonParams(8));
        root.addView(mode120Button, manualButtonParams(8));
        root.addView(axisButton, manualButtonParams(8));

        reportView = new TextView(this);
        reportView.setTypeface(android.graphics.Typeface.MONOSPACE);
        reportView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        reportView.setTextIsSelectable(true);
        HorizontalScrollView horizontal = new HorizontalScrollView(this);
        horizontal.addView(reportView, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        // §VITURE：軸向設定放進這塊捲動區。固定在上面的話，小螢幕會把分享按鈕擠出畫面。
        LinearLayout belowManual = new LinearLayout(this);
        belowManual.setOrientation(LinearLayout.VERTICAL);
        addAxisMapSection(belowManual);
        LinearLayout.LayoutParams reportParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        reportParams.topMargin = dp(12);
        belowManual.addView(horizontal, reportParams);
        ScrollView vertical = new ScrollView(this);
        vertical.setFillViewport(true);
        vertical.addView(belowManual, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(vertical, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        shareButton = new Button(this);
        shareButton.setText(R.string.glasses_diag_share);
        shareButton.setAllCaps(false);
        shareButton.setMinHeight(dp(48));
        shareButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                shareReport();
            }
        });
        copyButton = new Button(this);
        copyButton.setText(R.string.glasses_diag_copy);
        copyButton.setAllCaps(false);
        copyButton.setMinHeight(dp(48));
        copyButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                copyReport();
            }
        });
        uploadButton = new Button(this);
        uploadButton.setText(R.string.glasses_diag_upload);
        uploadButton.setAllCaps(false);
        uploadButton.setMinHeight(dp(48));
        uploadButton.setMaxLines(2);
        uploadButton.setGravity(Gravity.CENTER);
        uploadButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                confirmUpload();
            }
        });
        // §LOGDROP：獨立一行。三個按鈕並排時，小螢幕會把「立即上傳」截掉。網址是空的仍留著按鈕。
        LinearLayout.LayoutParams uploadParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        uploadParams.topMargin = dp(8);
        root.addView(uploadButton, uploadParams);
        row.addView(shareButton, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        LinearLayout.LayoutParams copyParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        copyParams.leftMargin = dp(8);
        row.addView(copyButton, copyParams);
        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rowParams.topMargin = dp(8);
        root.addView(row, rowParams);
        return root;
    }

    private void startProbe() {
        if (running) {
            return;
        }
        running = true;
        cancelled = false;
        updateButtons();
        progressView.setText(R.string.glasses_diag_progress_running);
        final GlassesDiagnosticsActivity activity = this;
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                String text;
                try {
                    text = GlassesDiagnostics.collect(activity, activity);
                } catch (Exception error) {
                    VitureUsb.logW("collect failed", error);
                    text = "fatal=" + VitureProbe.errorText(error) + "\nEND OF REPORT\n";
                }
                final String done = text;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        running = false;
                        reportText = withInstallId(done);
                        // §LOGDROP：報告文字定稿後才送。旋轉還原走 onCreate，不會再進這裡。
                        LogDrop.autoUploadAsync(getApplicationContext(), "android-glasses", "glasses-diag", reportText);
                        if (uiGone || isDestroyed()) {
                            return;
                        }
                        reportView.setText(reportText);
                        if (lastStage != GlassesDiagnostics.Stage.NO_DEVICE) {
                            progressView.setText(R.string.glasses_diag_progress_done);
                            lastStage = GlassesDiagnostics.Stage.DONE;
                        }
                        updateButtons();
                    }
                });
            }
        }, "viple-viture-probe");
        probeThread = thread;
        thread.start();
    }

    private void shareReport() {
        if (reportText == null || running) {
            Toast.makeText(this, R.string.glasses_diag_need_report, Toast.LENGTH_SHORT).show();
            return;
        }
        try {
            File dir = new File(getCacheDir(), UpdateFileProvider.GLASSES_CACHE_DIR);
            if (!dir.isDirectory() && !dir.mkdirs()) {
                throw new java.io.IOException("mkdir");
            }
            // 檔名含到秒，每次分享都是新檔。不刪的話舊報告會一直留在快取。
            File[] stale = dir.listFiles();
            if (stale != null) {
                for (int i = 0; i < stale.length; i++) {
                    File old = stale[i];
                    String name = old.getName();
                    if (old.isFile() && name.startsWith("viplestream-glasses-") && name.endsWith(".txt")
                            && !old.delete()) {
                        VitureUsb.logI("kept old glasses report " + name);
                    }
                }
            }
            // §VITURE：檔名時間跟報告裡的 UTC 對得起來，測試者不用換算時區。
            SimpleDateFormat stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US);
            stamp.setTimeZone(TimeZone.getTimeZone("UTC"));
            File file = new File(dir, "viplestream-glasses-" + stamp.format(new Date()) + ".txt");
            FileOutputStream out = new FileOutputStream(file);
            try {
                out.write(reportText.getBytes(StandardCharsets.UTF_8));
            } finally {
                out.close();
            }
            String authority = getPackageName() + ".updates";
            Uri uri = UpdateFileProvider.getUriForFile(this, authority, file);
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.title_glasses_diagnostics));
            // §VITURE：chooser 不會把 EXTRA_STREAM 的讀取權限帶過去，要另外放 ClipData。
            send.setClipData(ClipData.newRawUri("glasses-report", uri));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            Intent chooser = Intent.createChooser(send, getString(R.string.glasses_diag_share));
            chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(chooser);
            VitureUsb.logI("share report bytes=" + file.length());
        } catch (Exception error) {
            VitureUsb.logW("share failed", error);
            Toast.makeText(this, R.string.glasses_diag_share_failed, Toast.LENGTH_LONG).show();
        }
    }

    private void copyReport() {
        if (reportText == null || running) {
            Toast.makeText(this, R.string.glasses_diag_need_report, Toast.LENGTH_SHORT).show();
            return;
        }
        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) {
            Toast.makeText(this, R.string.glasses_diag_copy_failed, Toast.LENGTH_LONG).show();
            return;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("glasses-report", reportText));
        Toast.makeText(this, R.string.glasses_diag_copied, Toast.LENGTH_SHORT).show();
    }

    private void updateButtons() {
        startButton.setEnabled(!running);
        boolean manualEnabled = !running;
        mode3dButton.setEnabled(manualEnabled);
        mode2dButton.setEnabled(manualEnabled);
        mode120Button.setEnabled(manualEnabled);
        axisButton.setEnabled(manualEnabled);
        boolean canExport = reportText != null && !running;
        shareButton.setEnabled(canExport);
        uploadButton.setEnabled(canExport && !uploading);
        uploadButton.setText("yes".equals(LogDrop.consent(this))
                ? R.string.glasses_diag_upload_now
                : R.string.glasses_diag_upload);
        copyButton.setEnabled(canExport);
    }

    @Override
    public void showPrompt(final int stringRes) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (!isDestroyed()) {
                    progressView.setText(stringRes);
                }
            }
        });
    }

    @Override
    public void setManualGlasses(VitureGlasses glasses) {
        manualGlasses = glasses;
    }

    private void addAxisMapSection(LinearLayout root) {
        TextView axisTitle = new TextView(this);
        axisTitle.setText(R.string.glasses_axis_map_title);
        axisTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        axisTitle.setTypeface(axisTitle.getTypeface(), android.graphics.Typeface.BOLD);
        LinearLayout.LayoutParams titleParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        titleParams.topMargin = dp(16);
        root.addView(axisTitle, titleParams);

        TextView axisHelp = new TextView(this);
        axisHelp.setText(R.string.glasses_axis_map_help);
        axisHelp.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        LinearLayout.LayoutParams helpParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        helpParams.topMargin = dp(8);
        root.addView(axisHelp, helpParams);

        String stored = readStoredAxis();
        axisCurrentView = new TextView(this);
        axisCurrentView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        axisCurrentView.setText(getString(R.string.glasses_axis_map_current, stored));
        LinearLayout.LayoutParams currentParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        currentParams.topMargin = dp(8);
        root.addView(axisCurrentView, currentParams);

        axisInput = new EditText(this);
        axisInput.setSingleLine(true);
        axisInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        axisInput.setTypeface(android.graphics.Typeface.MONOSPACE);
        axisInput.setMinHeight(dp(48));
        axisInput.setText(stored);
        LinearLayout.LayoutParams inputParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        inputParams.topMargin = dp(8);
        root.addView(axisInput, inputParams);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        Button saveButton = new Button(this);
        saveButton.setText(R.string.glasses_axis_map_save);
        saveButton.setAllCaps(false);
        saveButton.setMinHeight(dp(48));
        saveButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                saveAxisMap();
            }
        });
        Button resetButton = new Button(this);
        resetButton.setText(R.string.glasses_axis_map_reset);
        resetButton.setAllCaps(false);
        resetButton.setMinHeight(dp(48));
        resetButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                restoreAxisMap();
            }
        });
        row.addView(saveButton, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        LinearLayout.LayoutParams resetParams = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        resetParams.leftMargin = dp(8);
        row.addView(resetButton, resetParams);
        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rowParams.topMargin = dp(8);
        root.addView(row, rowParams);

        axisErrorView = new TextView(this);
        axisErrorView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        axisErrorView.setVisibility(View.GONE);
        LinearLayout.LayoutParams errorParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        errorParams.topMargin = dp(4);
        root.addView(axisErrorView, errorParams);
    }

    private String readStoredAxis() {
        SharedPreferences prefs = getSharedPreferences(VitureAxisMap.PREFS_NAME, MODE_PRIVATE);
        String saved = prefs.getString(VitureAxisMap.PREFS_KEY, null);
        if (saved == null || saved.length() == 0) {
            return VitureAxisMap.DEFAULT;
        }
        return saved;
    }

    private boolean writeAxisMap(String value) {
        SharedPreferences prefs = getSharedPreferences(VitureAxisMap.PREFS_NAME, MODE_PRIVATE);
        // §VITURE：接著就要開串流的人會立刻讀這個值，apply() 可能還沒寫完。
        boolean written = prefs.edit().putString(VitureAxisMap.PREFS_KEY, value).commit();
        if (!written) {
            VitureUsb.logW("axis map commit failed", null);
            axisErrorView.setText(R.string.glasses_axis_map_save_failed);
            axisErrorView.setVisibility(View.VISIBLE);
            return false;
        }
        String stored = readStoredAxis();
        axisInput.setText(stored);
        axisCurrentView.setText(getString(R.string.glasses_axis_map_current, stored));
        axisErrorView.setVisibility(View.GONE);
        return true;
    }

    private void saveAxisMap() {
        String raw = axisInput.getText() == null ? "" : axisInput.getText().toString();
        String canonical = VitureAxisMap.canonical(raw);
        if (canonical == null) {
            axisErrorView.setText(R.string.glasses_axis_map_invalid);
            axisErrorView.setVisibility(View.VISIBLE);
            VitureUsb.logI("axis map rejected");
            return;
        }
        if (writeAxisMap(canonical)) {
            VitureUsb.logI("axis map saved " + canonical);
        }
    }

    private void restoreAxisMap() {
        if (writeAxisMap(VitureAxisMap.DEFAULT)) {
            VitureUsb.logI("axis map restored " + VitureAxisMap.DEFAULT);
        }
    }

    private Button manualButton(int label, final int mode) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setMinHeight(dp(48));
        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startManual(mode);
            }
        });
        return button;
    }

    private LinearLayout.LayoutParams manualButtonParams(int topMargin) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(topMargin);
        return params;
    }

    private void startManual(final int mode) {
        if (running) {
            return;
        }
        running = true;
        cancelled = false;
        updateButtons();
        progressView.setText(R.string.glasses_diag_progress_manual);
        final GlassesDiagnosticsActivity activity = this;
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                String text;
                try {
                    if (mode < 0) {
                        text = GlassesManualTest.axisTest(activity, activity);
                    } else {
                        text = GlassesManualTest.displayMode(activity, activity, mode);
                    }
                } catch (Exception error) {
                    VitureUsb.logW("manual test failed", error);
                    text = "manual fatal=" + VitureProbe.errorText(error) + "\n";
                }
                final String done = text;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        running = false;
                        if (reportText == null) {
                            reportText = "";
                        }
                        if (reportText.length() > 0 && !reportText.endsWith("\n")) {
                            reportText = reportText + "\n";
                        }
                        reportText = withInstallId(reportText + done);
                        // §LOGDROP：手動測試與軸向測試結束才送，畫面刷新不進這裡。
                        LogDrop.autoUploadAsync(getApplicationContext(), "android-glasses", "glasses-diag", reportText);
                        if (uiGone || isDestroyed()) {
                            return;
                        }
                        reportView.setText(reportText);
                        progressView.setText(R.string.glasses_diag_progress_done);
                        updateButtons();
                    }
                });
            }
        }, "viple-viture-manual");
        probeThread = thread;
        thread.start();
    }

    private int messageFor(GlassesDiagnostics.Stage stage) {
        switch (stage) {
            case PERMISSION:
                return R.string.glasses_diag_progress_permission;
            case QUERIES:
                return R.string.glasses_diag_progress_queries;
            case POSE:
                return R.string.glasses_diag_progress_pose;
            case RAW:
                return R.string.glasses_diag_progress_raw;
            case NO_DEVICE:
                return R.string.glasses_diag_progress_no_device;
            case DONE:
                return R.string.glasses_diag_progress_done;
            case DISPLAYS:
            case USB:
            case OPEN:
            default:
                return R.string.glasses_diag_progress_running;
        }
    }

    private String withInstallId(String text) {
        if (text != null && text.startsWith("install_id:")) {
            return text;
        }
        // §LOGDROP：collect() 會自己寫這一行。手動測試與例外路徑沒有經過它。
        String body = text == null ? "" : text;
        return "install_id: " + LogDrop.installId(this) + "\n" + body;
    }

    private void confirmUpload() {
        final String report = reportText;
        if (uploading || uiGone) {
            return;
        }
        if (report == null || running) {
            Toast.makeText(this, R.string.glasses_diag_upload_need_report, Toast.LENGTH_SHORT).show();
            return;
        }
        if (!LogDrop.isConfigured()) {
            Toast.makeText(this, R.string.logdrop_not_configured, Toast.LENGTH_LONG).show();
            return;
        }
        // §LOGDROP：已經同意過就不要每次再問。還沒問或選過不要，仍先確認。
        if ("yes".equals(LogDrop.consent(this))) {
            startUpload(report, "");
            return;
        }
        final EditText noteInput = new EditText(this);
        noteInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        noteInput.setSingleLine(false);
        noteInput.setMinLines(3);
        noteInput.setMaxLines(6);
        noteInput.setGravity(Gravity.TOP | Gravity.START);
        noteInput.setFilters(new InputFilter[] { new InputFilter.LengthFilter(2000) });
        noteInput.setHint(R.string.glasses_diag_upload_note_hint);

        ScrollView scroll = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        box.setPadding(pad, dp(8), pad, 0);
        TextView explain = new TextView(this);
        explain.setText(R.string.glasses_diag_upload_confirm);
        explain.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        LinearLayout.LayoutParams wide = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        LinearLayout.LayoutParams noteParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        noteParams.topMargin = dp(12);
        box.addView(explain, wide);
        box.addView(noteInput, noteParams);
        scroll.addView(box, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.glasses_diag_upload)
                .setView(scroll)
                .setPositiveButton(R.string.glasses_diag_upload_send, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        final String note = noteInput.getText().toString();
                        // §LOGDROP：確認框還在關，同一拍就開進度框有時會漏視窗。
                        new Handler(Looper.getMainLooper()).post(new Runnable() {
                            @Override
                            public void run() {
                                startUpload(report, note);
                            }
                        });
                    }
                })
                .setNegativeButton(R.string.glasses_diag_upload_cancel, null)
                .create();
        // §LOGDROP：說明框在對話框底部，鍵盤會蓋住。
        if (dialog.getWindow() != null) {
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        }
        showUploadDialog(dialog);
    }

    private void startUpload(String report, String note) {
        if (uiGone || isDestroyed() || isFinishing() || report == null) {
            return;
        }
        if (uploading) {
            return;
        }
        uploading = true;
        updateButtons();
        ProgressDialog progress = new ProgressDialog(this);
        progress.setTitle(R.string.glasses_diag_upload);
        progress.setMessage(getString(R.string.glasses_diag_upload_progress));
        progress.setProgressStyle(ProgressDialog.STYLE_SPINNER);
        progress.setIndeterminate(true);
        progress.setCancelable(false);
        progress.setCanceledOnTouchOutside(false);
        if (!showUploadDialog(progress)) {
            uploading = false;
            if (!uiGone && !isDestroyed() && !isFinishing()) {
                updateButtons();
            }
            return;
        }
        Context app = getApplicationContext();
        if (app == null) {
            app = this;
        }
        new Thread(new UploadTask(new WeakReference<GlassesDiagnosticsActivity>(this), app, report, note),
                "viple-logdrop-upload").start();
    }

    private void showUploadResult(LogDrop.Result result) {
        uploading = false;
        if (uiGone || isDestroyed() || isFinishing()) {
            return;
        }
        updateButtons();
        if (result != null && result.ok && result.code != null && result.code.length() > 0) {
            showUploadCode(result.code);
        } else {
            showUploadFailure(result == null ? null : result.error);
        }
    }

    private void showUploadCode(final String code) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        TextView codeView = new TextView(this);
        codeView.setText(code);
        codeView.setGravity(Gravity.CENTER);
        codeView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 34);
        codeView.setTypeface(android.graphics.Typeface.MONOSPACE);
        codeView.setTextIsSelectable(true);
        codeView.setPadding(dp(16), dp(20), dp(16), dp(8));
        TextView message = new TextView(this);
        message.setText(R.string.glasses_diag_upload_code_message);
        message.setGravity(Gravity.CENTER);
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        message.setPadding(dp(16), dp(4), dp(16), dp(12));
        box.addView(codeView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        box.addView(message, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setView(box)
                .setNeutralButton(R.string.glasses_diag_copy, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        copyUploadCode(code);
                    }
                })
                .setPositiveButton(android.R.string.ok, null)
                .create();
        dialog.setOnShowListener(new DialogInterface.OnShowListener() {
            @Override
            public void onShow(DialogInterface d) {
                Button copy = dialog.getButton(AlertDialog.BUTTON_NEUTRAL);
                if (copy == null) {
                    return;
                }
                // §LOGDROP：預設按鈕會關掉對話框，代碼還沒看完就沒了。
                copy.setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        copyUploadCode(code);
                    }
                });
            }
        });
        showUploadDialog(dialog);
    }

    private void showUploadFailure(String error) {
        String message;
        if ("rate_limit".equals(error) || "daily_limit".equals(error) || "install_limit".equals(error)) {
            message = getString(R.string.glasses_diag_upload_busy);
        } else if ("network".equals(error)) {
            message = getString(R.string.glasses_diag_upload_network);
        } else {
            if (error == null || error.length() == 0) {
                error = "bad_response";
            }
            message = getString(R.string.glasses_diag_upload_failed, error);
        }
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setMessage(message)
                .setPositiveButton(android.R.string.ok, null)
                .create();
        showUploadDialog(dialog);
    }

    private void copyUploadCode(String code) {
        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) {
            Toast.makeText(this, R.string.glasses_diag_upload_copy_failed, Toast.LENGTH_LONG).show();
            return;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("logdrop-code", code));
        Toast.makeText(this, R.string.glasses_diag_upload_copied, Toast.LENGTH_SHORT).show();
    }

    private boolean showUploadDialog(Dialog dialog) {
        if (uiGone || isDestroyed() || isFinishing()) {
            return false;
        }
        dismissUploadDialog();
        try {
            dialog.show();
        } catch (WindowManager.BadTokenException error) {
            // §LOGDROP：畫面已經關掉時 show() 會失敗，不要再更新 UI。
            Log.w(LOGDROP_TAG, "[VIPLE-LOGDROP] dialog not shown", error);
            return false;
        }
        uploadDialog = dialog;
        return true;
    }

    private void dismissUploadDialog() {
        Dialog dialog = uploadDialog;
        uploadDialog = null;
        if (dialog == null) {
            return;
        }
        try {
            if (dialog.isShowing()) {
                dialog.dismiss();
            }
        } catch (RuntimeException error) {
            Log.w(LOGDROP_TAG, "[VIPLE-LOGDROP] dialog dismiss failed", error);
        }
    }

    private static final class UploadTask implements Runnable {
        private final WeakReference<GlassesDiagnosticsActivity> activity;
        private final Context app;
        private final String report;
        private final String note;

        private UploadTask(WeakReference<GlassesDiagnosticsActivity> activity, Context app,
                String report, String note) {
            this.activity = activity;
            this.app = app;
            this.report = report;
            this.note = note;
        }

        @Override
        public void run() {
            LogDrop.Result result;
            try {
                result = LogDrop.upload(app, "android-glasses", report, note, "manual");
            } catch (RuntimeException error) {
                Log.w(LOGDROP_TAG, "[VIPLE-LOGDROP] upload crashed", error);
                result = new LogDrop.Result(false, "", "exception");
            }
            final LogDrop.Result done = result;
            final GlassesDiagnosticsActivity ui = activity.get();
            if (ui == null) {
                return;
            }
            ui.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    GlassesDiagnosticsActivity current = activity.get();
                    if (current == null) {
                        return;
                    }
                    current.showUploadResult(done);
                }
            });
        }
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private static int awaitLatch(CountDownLatch latch) {
        try {
            return latch.await(200, TimeUnit.MILLISECONDS) ? 1 : 0;
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return -1;
        }
    }
}
