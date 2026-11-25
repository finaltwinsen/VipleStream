package com.limelight.utils;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.net.ConnectivityManager;
import android.net.Uri;
import android.os.Build;
import android.preference.PreferenceManager;
import android.provider.Settings;
import android.util.Log;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import com.limelight.BuildConfig;
import com.limelight.Game;
import com.limelight.R;
import com.limelight.vr.VrStreamActivity;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * VipleStream 軟體自動自我更新程式
 *
 * 負責透過 GitHub Releases 檢查、下載、多重安全驗證（雜湊值、套件名稱、簽章憑證、版本序號）與安裝最新版本的 APK。
 */
public class AppUpdater {

    private static final String TAG = "VipleUpdate";
    private static final String LOG_PREFIX = "[VIPLE-UPDATE] ";

    public static final String EXTRA_DEV_UPDATE_URL = "dev_update_url";

    public static final String PREF_UPDATE_MODE = "update_mode";
    public static final String MODE_OFF = "off";
    public static final String MODE_NOTIFY = "notify";
    public static final String MODE_AUTO = "auto";

    private static final String PREF_LAST_CHECK_TIME = "last_update_check_time";
    private static final String PREF_SKIPPED_VERSION = "skipped_update_version";
    private static final String PREF_SKIPPED_TIME = "skipped_update_time";

    private static final long CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000L; // 24 小時檢查間隔
    private static final long MAX_DEV_APK_BYTES = 512L * 1024L * 1024L;

    private static final ExecutorService sExecutor = Executors.newSingleThreadExecutor();
    private static boolean sDebugLogged = false;

    // 儲存目前顯示中的對話方塊，防止 Activity 銷毀時產生視窗洩漏（Window Leaks）
    private static Dialog sCurrentDialog = null;

    // 當前往系統設定開啟「安裝未知應用程式」權限時，暫存等待安裝的 APK 檔案
    private static File sPendingInstallApk = null;

    // 下載任務取消控制旗標
    private static volatile boolean sDownloadCancelled = false;

    // §REVIEW-1118：串流中途中止下載後補查一次；網路失敗或串流中斷時不寫入 24 小時戳記，確保回來能補查。
    private static volatile boolean sRetryAfterStream = false;

    // 僅在當次啟動生效的開發測試更新伺服器基底網址，不寫入 SharedPreferences。
    private static volatile String sPendingDevUpdateUrl = null;

    /**
     * 設定僅限本次啟動生效的開發測試更新伺服器網址
     */
    public static void setDevUpdateUrl(String rawUrl) {
        if (rawUrl == null || rawUrl.trim().isEmpty()) {
            return;
        }
        String normalized = normalizeAndValidateDevBaseUrl(rawUrl);
        if (normalized == null) {
            Log.w(TAG, LOG_PREFIX + "Rejected dev_update_url (only https:// or http:// localhost/private IP allowed): " + rawUrl);
            sPendingDevUpdateUrl = null;
            return;
        }
        Log.i(TAG, LOG_PREFIX + "Configured one-shot dev_update_url: " + normalized);
        sPendingDevUpdateUrl = normalized;
    }

    /**
     * 當主畫面（PcView）恢復執行（onResume）時呼叫
     */
    public static void onActivityResume(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) {
            return;
        }

        // 若本次啟動帶有 dev_update_url，優先跑測試更新流程，不要被上一輪殘留的待裝檔案擋住。
        if (sPendingDevUpdateUrl != null) {
            sPendingInstallApk = null;
            checkForUpdates(activity);
            return;
        }

        // 1. 檢查是否有等待使用者授權後繼續安裝的 APK 檔案
        if (sPendingInstallApk != null && sPendingInstallApk.exists()) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                if (activity.getPackageManager().canRequestPackageInstalls()) {
                    Log.i(TAG, LOG_PREFIX + "Install permission granted; resuming pending APK installation.");
                    File apkToInstall = sPendingInstallApk;
                    sPendingInstallApk = null;
                    doInstall(activity, apkToInstall);
                    return;
                }
            } else {
                File apkToInstall = sPendingInstallApk;
                sPendingInstallApk = null;
                doInstall(activity, apkToInstall);
                return;
            }
        }

        // 2. 執行定期的 24 小時更新檢查
        checkForUpdates(activity);
    }

    /**
     * 當主畫面停止（onStop）時呼叫，關閉任何顯示中的對話方塊以避免洩漏
     */
    public static void onActivityStop(Activity activity) {
        dismissCurrentDialog();
    }

    /**
     * 關閉目前顯示的對話方塊
     */
    private static synchronized void dismissCurrentDialog() {
        if (sCurrentDialog != null) {
            try {
                if (sCurrentDialog.isShowing()) {
                    sCurrentDialog.dismiss();
                }
            } catch (Exception ignored) {
            }
            sCurrentDialog = null;
        }
    }

    /**
     * 檢查是否有新版本可供更新
     */
    public static void checkForUpdates(final Activity activity) {
        final String devBaseUrl = sPendingDevUpdateUrl;
        sPendingDevUpdateUrl = null;
        final boolean isDevOverride = (devBaseUrl != null);

        // 1. 若為除錯建置（Debug Build）且未帶 dev_update_url 則完全跳過，僅記錄一次日誌
        if (!isDevOverride && (BuildConfig.APPLICATION_ID.endsWith(".debug") || BuildConfig.DEBUG)) {
            if (!sDebugLogged) {
                sDebugLogged = true;
                Log.i(TAG, LOG_PREFIX + "Debug build detected; self-update check is disabled.");
            }
            return;
        }

        // 2. 若目前正在進行 2D 或 VR 串流，絕不執行更新檢查
        if (isStreamRunning()) {
            if (isDevOverride) {
                // 串流結束回到主畫面時再執行這次測試檢查。
                sPendingDevUpdateUrl = devBaseUrl;
                sRetryAfterStream = true;
            }
            Log.i(TAG, LOG_PREFIX + "Stream is active; deferring update check.");
            return;
        }

        final Context appContext = activity.getApplicationContext();
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(appContext);

        // 3. 檢查使用者的更新模式偏好設定（dev_update_url 測試時不受 Off 擋下）
        String mode = prefs.getString(PREF_UPDATE_MODE, MODE_AUTO);
        if (!isDevOverride && MODE_OFF.equalsIgnoreCase(mode)) {
            Log.i(TAG, LOG_PREFIX + "Update mode is set to Off.");
            return;
        }

        // 4. 檢查上次成功檢查時間，最多每 24 小時執行一次（網路失敗不寫入，故下次啟動可重試）
        long lastCheck = prefs.getLong(PREF_LAST_CHECK_TIME, 0);
        long now = System.currentTimeMillis();
        boolean retryAfterStream = sRetryAfterStream;
        if (retryAfterStream) {
            sRetryAfterStream = false;
        } else if (!isDevOverride && now - lastCheck < CHECK_INTERVAL_MS && now >= lastCheck) {
            return;
        }

        // 在背景執行緒執行網路檢查
        sExecutor.execute(new Runnable() {
            @Override
            public void run() {
                performUpdateCheck(activity, appContext, devBaseUrl);
            }
        });
    }

    private static void recordLastCheckTimeIfNormal(Context context, String devBaseUrl) {
        // 開發測試入口不可寫入偏好設定；一般模式只在成功取得 release 資訊或確定無新版後才寫入。
        if (devBaseUrl != null) {
            return;
        }
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
        prefs.edit().putLong(PREF_LAST_CHECK_TIME, System.currentTimeMillis()).apply();
    }

    /**
     * 背景執行緒：執行 GitHub 標籤檢查與後續流程
     */
    private static void performUpdateCheck(final Activity activity, final Context context, final String devBaseUrl) {
        try {
            if (abortDownloadBecauseStreaming(false, null)) {
                if (devBaseUrl != null) {
                    sPendingDevUpdateUrl = devBaseUrl;
                }
                return;
            }

            final boolean isDevOverride = (devBaseUrl != null);
            String latestTag;
            if (isDevOverride) {
                // dev_update_url 固定提供 GitHub API 格式 JSON 的 <url>/releases/latest。
                latestTag = fetchLatestReleaseTagGet(devBaseUrl + "/releases/latest");
            } else {
                // 透過 HEAD 請求取得最新版本標籤（避免消耗 GitHub API 配額）
                latestTag = fetchLatestReleaseTagHead();
                if (latestTag == null || latestTag.isEmpty()) {
                    Log.i(TAG, LOG_PREFIX + "HEAD release check failed; falling back to GET API.");
                    latestTag = fetchLatestReleaseTagGet("https://api.github.com/repos/finaltwinsen/VipleStream/releases/latest");
                }
            }

            if (latestTag == null || latestTag.isEmpty()) {
                Log.w(TAG, LOG_PREFIX + "Could not fetch latest release tag.");
                return;
            }

            Log.i(TAG, LOG_PREFIX + "Fetched latest release tag: " + latestTag);

            // 解析版本號碼（例如 "v2.0.73" -> "2.0.73"）
            final String remoteVersion = latestTag.startsWith("v") || latestTag.startsWith("V")
                    ? latestTag.substring(1) : latestTag;
            final String currentVersion = BuildConfig.VERSION_NAME;

            // VipleStream REVIEW-1111 (round 2): the tag becomes part of a file name and of an API URL; accept plain
            // dotted numbers only (a tag such as "999.0/../x" passed the numeric comparison below).
            if (!remoteVersion.matches("[0-9]+([.][0-9]+){0,3}")) {
                Log.w(TAG, LOG_PREFIX + "unexpected release tag, ignoring: " + latestTag);
                return;
            }

            // 依據點號數值比對版本號
            if (!isNewerVersion(remoteVersion, currentVersion)) {
                Log.i(TAG, LOG_PREFIX + "App is up to date (current: " + currentVersion + ", latest: " + remoteVersion + ").");
                recordLastCheckTimeIfNormal(context, devBaseUrl);
                return;
            }

            Log.i(TAG, LOG_PREFIX + "New version available: " + remoteVersion + " (current: " + currentVersion + ").");

            SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
            String configuredMode = prefs.getString(PREF_UPDATE_MODE, MODE_AUTO);
            if (isDevOverride && MODE_OFF.equalsIgnoreCase(configuredMode)) {
                configuredMode = MODE_AUTO;
            }
            final String mode = configuredMode;
            final String finalTag = latestTag;

            if (MODE_AUTO.equalsIgnoreCase(mode)) {
                // 自動模式：一般情況僅在非計量網路下自動下載；計量網路時不寫時間戳，讓切回 Wi-Fi 後可再試。
                if (!isDevOverride) {
                    ConnectivityManager cm = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
                    if (cm != null && cm.isActiveNetworkMetered()) {
                        Log.i(TAG, LOG_PREFIX + "Metered network detected; deferring automatic update download.");
                        return;
                    }
                }

                // §REVIEW-1118：查版本的這段時間使用者可能已經進遊戲，不要在串流中才開始傳 APK。
                if (abortDownloadBecauseStreaming(false, null)) {
                    if (isDevOverride) {
                        sPendingDevUpdateUrl = devBaseUrl;
                    }
                    return;
                }

                // 一律透過 downloadAndVerify 取得 release digest 並核對 SHA-256 與 APK 簽章（快取命中亦須驗過 digest）。
                sDownloadCancelled = false;
                downloadAndVerify(activity, context, remoteVersion, finalTag, devBaseUrl, false, new DownloadCallback() {
                    @Override
                    public void onSuccess(File apkFile) {
                        recordLastCheckTimeIfNormal(context, devBaseUrl);
                        showDownloadedDialog(activity, remoteVersion, apkFile);
                    }

                    @Override
                    public void onFailure(String errorMsg) {
                        Log.e(TAG, LOG_PREFIX + "Automatic download or verification failed: " + errorMsg);
                    }
                }, null);
            } else {
                // 已成功取得新版資訊，通知模式下寫入檢查時間戳。
                recordLastCheckTimeIfNormal(context, devBaseUrl);

                // 通知模式（notify）：檢查使用者是否針對此版本點選過「稍後」
                if (!isDevOverride) {
                    String skippedVersion = prefs.getString(PREF_SKIPPED_VERSION, "");
                    long skippedTime = prefs.getLong(PREF_SKIPPED_TIME, 0);
                    if (remoteVersion.equals(skippedVersion)) {
                        long elapsed = System.currentTimeMillis() - skippedTime;
                        if (elapsed >= 0 && elapsed < CHECK_INTERVAL_MS) {
                            Log.i(TAG, LOG_PREFIX + "Version " + remoteVersion + " was snoozed within 24h; skipping prompt.");
                            return;
                        }
                    }
                }

                if (abortDownloadBecauseStreaming(false, null)) {
                    return;
                }

                // 在 UI 執行緒顯示更新提示對話方塊
                showUpdateAvailableDialog(activity, remoteVersion, finalTag, devBaseUrl);
            }
        } catch (Exception e) {
            Log.e(TAG, LOG_PREFIX + "Update check failed with exception: " + e.getMessage(), e);
        }
    }

    /**
     * 發送 HEAD 請求至 GitHub release latest（停用重定向）以取得 Location 標頭中的標籤名稱
     */
    private static String fetchLatestReleaseTagHead() {
        HttpURLConnection conn = null;
        try {
            URL url = new URL("https://github.com/finaltwinsen/VipleStream/releases/latest");
            conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod("HEAD");
            conn.setInstanceFollowRedirects(false); // 停用自動重定向以擷取 Location 標頭
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(10000);
            conn.setRequestProperty("User-Agent", "VipleStream-Android/" + BuildConfig.VERSION_NAME);

            int code = conn.getResponseCode();
            if (code >= 300 && code < 400) {
                String location = conn.getHeaderField("Location");
                if (location != null) {
                    int tagIdx = location.indexOf("/releases/tag/");
                    if (tagIdx != -1) {
                        String tag = location.substring(tagIdx + "/releases/tag/".length());
                        int queryIdx = tag.indexOf('?');
                        if (queryIdx != -1) {
                            tag = tag.substring(0, queryIdx);
                        }
                        while (tag.endsWith("/")) {
                            tag = tag.substring(0, tag.length() - 1);
                        }
                        return tag;
                    }
                }
            }
        } catch (Exception e) {
            Log.w(TAG, LOG_PREFIX + "HEAD request failed: " + e.getMessage());
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
        return null;
    }

    /**
     * §UPDATE-NOTES：抓這個 tag 的 release notes，取摘要（規則同電腦版，見 UpdateNotes），補進還開著的對話框。
     */
    private static void loadReleaseNotesInto(final Activity activity, final AlertDialog dialog,
                                             final String baseMessage, final String tag, final String devBaseUrl) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                HttpURLConnection conn = null;
                try {
                    String notesApiUrl = (devBaseUrl != null)
                            ? (devBaseUrl + "/releases/latest")
                            : ("https://api.github.com/repos/finaltwinsen/VipleStream/releases/tags/" + tag);
                    URL url = new URL(notesApiUrl);
                    conn = (HttpURLConnection) url.openConnection();
                    conn.setRequestMethod("GET");
                    conn.setConnectTimeout(10000);
                    conn.setReadTimeout(10000);
                    conn.setRequestProperty("User-Agent", "VipleStream-Android/" + BuildConfig.VERSION_NAME);
                    conn.setRequestProperty("Accept", "application/vnd.github.v3+json");
                    if (conn.getResponseCode() != HttpURLConnection.HTTP_OK) {
                        return;
                    }
                    JSONObject json = new JSONObject(readStreamToString(conn.getInputStream()));
                    final String notes = UpdateNotes.summarize(json.optString("body", ""), Locale.getDefault().toLanguageTag());
                    if (notes.isEmpty()) {
                        return;
                    }
                    activity.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            if (!activity.isFinishing() && !activity.isDestroyed() && dialog.isShowing()) {
                                dialog.setMessage(baseMessage + "\n\n" + notes);
                            }
                        }
                    });
                } catch (Exception e) {
                    Log.w(TAG, LOG_PREFIX + "Failed to load release notes summary: " + e.getMessage());
                } finally {
                    if (conn != null) {
                        conn.disconnect();
                    }
                }
            }
        }, "UpdateNotes").start();
    }

    /**
     * 發送 GET 請求至 Releases API（或 dev_update_url/releases/latest）取得 tag_name
     */
    private static String fetchLatestReleaseTagGet(String apiUrl) {
        HttpURLConnection conn = null;
        try {
            URL url = new URL(apiUrl);
            conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(10000);
            conn.setRequestProperty("User-Agent", "VipleStream-Android/" + BuildConfig.VERSION_NAME);
            conn.setRequestProperty("Accept", "application/vnd.github.v3+json");

            if (conn.getResponseCode() == HttpURLConnection.HTTP_OK) {
                String response = readStreamToString(conn.getInputStream());
                JSONObject json = new JSONObject(response);
                return json.optString("tag_name", null);
            }
        } catch (Exception e) {
            Log.w(TAG, LOG_PREFIX + "GET release check failed: " + e.getMessage());
        } finally {
            if (conn != null) {
                conn.disconnect();
            }
        }
        return null;
    }

    /**
     * 數值點號比對版本號碼，例如 2.0.73 > 2.0.72
     */
    public static boolean isNewerVersion(String remote, String current) {
        if (remote == null || current == null) {
            return false;
        }
        String[] rParts = remote.split("\\.");
        String[] cParts = current.split("\\.");
        int len = Math.max(rParts.length, cParts.length);
        for (int i = 0; i < len; i++) {
            int rVal = i < rParts.length ? parseLeadingInt(rParts[i]) : 0;
            int cVal = i < cParts.length ? parseLeadingInt(cParts[i]) : 0;
            if (rVal > cVal) {
                return true;
            }
            if (rVal < cVal) {
                return false;
            }
        }
        return false;
    }

    private static int parseLeadingInt(String part) {
        if (part == null || part.isEmpty()) {
            return 0;
        }
        int idx = 0;
        while (idx < part.length() && Character.isDigit(part.charAt(idx))) {
            idx++;
        }
        if (idx == 0) {
            return 0;
        }
        try {
            return Integer.parseInt(part.substring(0, idx));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /**
     * 顯示「發現新版本」提示對話方塊（Notify 模式）
     */
    private static void showUpdateAvailableDialog(final Activity activity, final String remoteVersion,
                                                  final String tag, final String devBaseUrl) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (activity.isFinishing() || activity.isDestroyed() || isStreamRunning()) {
                    return;
                }
                dismissCurrentDialog();

                String message = String.format(Locale.getDefault(),
                        activity.getString(R.string.update_available_message), remoteVersion);

                AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                        .setTitle(R.string.update_available_title)
                        .setMessage(message)
                        .setPositiveButton(R.string.update_button_update, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                // 使用者主動點選更新，啟動帶有進度條的下載程序
                                startUserInitiatedDownload(activity, remoteVersion, tag, devBaseUrl);
                            }
                        })
                        .setNegativeButton(R.string.update_button_later, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                // 開發測試入口不寫入偏好；一般模式下記錄延後狀態，24 小時內不再提示。
                                if (devBaseUrl == null) {
                                    SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(activity.getApplicationContext());
                                    prefs.edit()
                                            .putString(PREF_SKIPPED_VERSION, remoteVersion)
                                            .putLong(PREF_SKIPPED_TIME, System.currentTimeMillis())
                                            .apply();
                                }
                                Log.i(TAG, LOG_PREFIX + "User deferred update for version " + remoteVersion + ".");
                            }
                        })
                        .setNeutralButton(R.string.update_button_release_notes, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                try {
                                    Intent browserIntent = new Intent(Intent.ACTION_VIEW,
                                            Uri.parse("https://github.com/finaltwinsen/VipleStream/releases/tag/" + tag));
                                    activity.startActivity(browserIntent);
                                } catch (Exception e) {
                                    Log.e(TAG, LOG_PREFIX + "Failed to open release notes URL: " + e.getMessage());
                                }
                            }
                        });

                final AlertDialog shown = builder.show();
                sCurrentDialog = shown;
                // §UPDATE-NOTES：對話框先出來，更新摘要在背景抓，抓到了再補在訊息後面（抓不到就維持原樣）
                loadReleaseNotesInto(activity, shown, message, tag, devBaseUrl);
            }
        });
    }

    /**
     * 顯示「更新已下載」對話方塊（Auto 模式完成時）
     */
    private static void showDownloadedDialog(final Activity activity, final String remoteVersion, final File apkFile) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (activity.isFinishing() || activity.isDestroyed() || isStreamRunning()) {
                    return;
                }
                dismissCurrentDialog();

                String message = String.format(Locale.getDefault(),
                        activity.getString(R.string.update_downloaded_message), remoteVersion);

                AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                        .setTitle(R.string.update_downloaded_title)
                        .setMessage(message)
                        .setPositiveButton(R.string.update_button_install, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                installApk(activity, apkFile);
                            }
                        })
                        .setNegativeButton(R.string.update_button_later, null);

                sCurrentDialog = builder.show();
            }
        });
    }

    /**
     * 使用者主動觸發下載，建立進度對話方塊並在背景下載驗證
     */
    private static void startUserInitiatedDownload(final Activity activity, final String remoteVersion,
                                                   final String tag, final String devBaseUrl) {
        dismissCurrentDialog();

        // 建立簡單水平進度條自訂介面
        LinearLayout layout = new LinearLayout(activity);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(60, 40, 60, 20);

        final ProgressBar progressBar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setIndeterminate(false);
        progressBar.setMax(100);
        progressBar.setProgress(0);

        final TextView progressText = new TextView(activity);
        progressText.setText("0%");
        progressText.setPadding(0, 16, 0, 0);

        layout.addView(progressBar);
        layout.addView(progressText);

        sDownloadCancelled = false;

        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle(activity.getString(R.string.update_downloading_title))
                .setView(layout)
                .setNegativeButton(android.R.string.cancel, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        sDownloadCancelled = true;
                        Log.i(TAG, LOG_PREFIX + "User cancelled update download.");
                    }
                })
                .setCancelable(true)
                .setOnCancelListener(new DialogInterface.OnCancelListener() {
                    @Override
                    public void onCancel(DialogInterface dialog) {
                        sDownloadCancelled = true;
                        Log.i(TAG, LOG_PREFIX + "User cancelled update download.");
                    }
                });

        final AlertDialog progressDialog = builder.create();
        sCurrentDialog = progressDialog;
        progressDialog.show();

        final ProgressListener progressListener = new ProgressListener() {
            @Override
            public void onProgress(final int percent) {
                activity.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (!activity.isFinishing() && !activity.isDestroyed() && progressDialog.isShowing()) {
                            progressBar.setProgress(percent);
                            progressText.setText(percent + "%");
                        }
                    }
                });
            }
        };

        sExecutor.execute(new Runnable() {
            @Override
            public void run() {
                downloadAndVerify(activity, activity.getApplicationContext(), remoteVersion, tag, devBaseUrl, true, new DownloadCallback() {
                    @Override
                    public void onSuccess(final File apkFile) {
                        activity.runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                dismissCurrentDialog();
                                installApk(activity, apkFile);
                            }
                        });
                    }

                    @Override
                    public void onFailure(final String errorMsg) {
                        activity.runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                dismissCurrentDialog();
                                if (!sDownloadCancelled && !activity.isFinishing() && !activity.isDestroyed()) {
                                    showErrorDialog(activity, errorMsg);
                                }
                            }
                        });
                    }
                }, progressListener);
            }
        });
    }

    /**
     * 顯示錯誤訊息對話方塊
     */
    private static void showErrorDialog(Activity activity, String errorMsg) {
        if (activity.isFinishing() || activity.isDestroyed()) {
            return;
        }
        dismissCurrentDialog();

        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                .setTitle(R.string.update_error_title)
                .setMessage(activity.getString(R.string.update_error_message))
                .setPositiveButton(android.R.string.ok, null);
        sCurrentDialog = builder.show();
    }

    private interface DownloadCallback {
        void onSuccess(File apkFile);
        void onFailure(String errorMsg);
    }

    private interface ProgressListener {
        void onProgress(int percent);
    }

    /**
     * 從 GitHub Tags API（或 dev_update_url/releases/latest）取得資產資訊，下載 APK 檔案並進行全面安全性驗證
     */
    private static void downloadAndVerify(Activity activity, Context context, String remoteVersion, String tag,
                                          String devBaseUrl, boolean userInitiated,
                                          DownloadCallback callback, ProgressListener listener) {
        if (abortDownloadBecauseStreaming(userInitiated, callback)) {
            if (devBaseUrl != null) {
                sPendingDevUpdateUrl = devBaseUrl;
            }
            return;
        }

        final boolean isDevOverride = (devBaseUrl != null);
        HttpURLConnection apiConn = null;
        try {
            // 1. 取得資產中繼資料（dev_update_url 模式下統一從 <url>/releases/latest 取）
            String releaseEndpoint = isDevOverride
                    ? (devBaseUrl + "/releases/latest")
                    : ("https://api.github.com/repos/finaltwinsen/VipleStream/releases/tags/" + tag);
            URL apiUrl = new URL(releaseEndpoint);
            apiConn = (HttpURLConnection) apiUrl.openConnection();
            apiConn.setRequestMethod("GET");
            apiConn.setConnectTimeout(15000);
            apiConn.setReadTimeout(15000);
            apiConn.setRequestProperty("User-Agent", "VipleStream-Android/" + BuildConfig.VERSION_NAME);
            apiConn.setRequestProperty("Accept", "application/vnd.github.v3+json");

            if (apiConn.getResponseCode() != HttpURLConnection.HTTP_OK) {
                callback.onFailure("Release API returned HTTP " + apiConn.getResponseCode());
                return;
            }

            String jsonStr = readStreamToString(apiConn.getInputStream());
            JSONObject releaseObj = new JSONObject(jsonStr);
            JSONArray assets = releaseObj.optJSONArray("assets");

            if (assets == null) {
                callback.onFailure("No assets array in release metadata");
                return;
            }

            String targetAssetName = "VipleStream-Android-" + remoteVersion + ".apk";
            String downloadUrl = null;
            long expectedSize = -1;
            String digest = null;

            for (int i = 0; i < assets.length(); i++) {
                JSONObject asset = assets.getJSONObject(i);
                if (targetAssetName.equals(asset.optString("name"))) {
                    downloadUrl = asset.optString("browser_download_url", null);
                    expectedSize = asset.optLong("size", -1);
                    digest = asset.optString("digest", null);
                    break;
                }
            }

            // §SECURITY：預設拒絕（Fail Closed）原則，若缺少資產或缺少有效 64 字元 sha256 digest 則拒絕更新
            if (downloadUrl == null || (!isDevOverride && expectedSize <= 0)
                    || digest == null || !digest.toLowerCase(Locale.ROOT).startsWith("sha256:")) {
                Log.e(TAG, LOG_PREFIX + "Asset " + targetAssetName + " missing or lacks sha256 digest; refusing update.");
                callback.onFailure("Asset missing valid sha256 digest metadata");
                return;
            }

            String expectedSha256 = digest.substring("sha256:".length()).trim();
            if (!expectedSha256.matches("[0-9a-fA-F]{64}")) {
                Log.e(TAG, LOG_PREFIX + "Invalid sha256 digest format for " + targetAssetName + ": " + digest);
                callback.onFailure("Malformed sha256 digest");
                return;
            }

            // 2. 準備下載目錄 getCacheDir()/updates/，清理該目錄下的舊檔案
            File updatesDir = new File(context.getCacheDir(), "updates");
            if (!updatesDir.exists()) {
                updatesDir.mkdirs();
            }

            File[] existingFiles = updatesDir.listFiles();
            if (existingFiles != null) {
                for (File f : existingFiles) {
                    if (!f.getName().equals(targetAssetName)) {
                        f.delete();
                    }
                }
            }

            File finalApkFile = new File(updatesDir, targetAssetName);
            File partApkFile = new File(updatesDir, targetAssetName + ".part");

            // 若目標檔案已存在，必須同時通過檔案大小、release sha256 digest 與 APK 簽章套件驗證才可重用。
            if (finalApkFile.exists()) {
                if ((expectedSize <= 0 || finalApkFile.length() == expectedSize)
                        && verifyFileSha256(finalApkFile, expectedSha256)
                        && verifyApk(context, finalApkFile)) {
                    Log.i(TAG, LOG_PREFIX + "Cached APK verified (SHA-256 and package signature): " + finalApkFile.getAbsolutePath());
                    callback.onSuccess(finalApkFile);
                    return;
                }
                finalApkFile.delete();
            }

            if (partApkFile.exists()) {
                partApkFile.delete();
            }

            // 3. 下載 APK 檔案（手動支援最多 5 次 HTTP 重定向）
            Log.i(TAG, LOG_PREFIX + "Starting APK download: " + downloadUrl);
            HttpURLConnection dlConn = null;
            InputStream in = null;
            FileOutputStream out = null;
            String currentUrl = downloadUrl;
            int redirectCount = 0;

            try {
                boolean connected = false;
                while (redirectCount < 5) {
                    // §REVIEW-1118：轉址期間若已進串流，不要接著把整包抓下來。
                    if (abortDownloadBecauseStreaming(userInitiated, callback)) {
                        if (isDevOverride) {
                            sPendingDevUpdateUrl = devBaseUrl;
                        }
                        return;
                    }
                    // §REVIEW-1111（第二輪）：一般模式只走 https；開發測試模式額外允許 http 的 localhost 與私有位址。
                    if (!isAllowedDownloadUrl(currentUrl, isDevOverride)) {
                        throw new IOException("Disallowed download URL: " + currentUrl);
                    }
                    URL u = new URL(currentUrl);
                    dlConn = (HttpURLConnection) u.openConnection();
                    // §REVIEW-1119：為什麼關掉自動跟隨——getResponseCode() 會自行吃掉同協定轉址，
                    // 上面的 https 檢查、5 次上限、串流中止都看不到。絕對網址維持原字串，避免簽名參數被改寫。
                    dlConn.setInstanceFollowRedirects(false);
                    dlConn.setConnectTimeout(15000);
                    dlConn.setReadTimeout(30000);
                    dlConn.setRequestProperty("User-Agent", "VipleStream-Android/" + BuildConfig.VERSION_NAME);

                    int code = dlConn.getResponseCode();
                    if (code == HttpURLConnection.HTTP_MOVED_TEMP ||
                            code == HttpURLConnection.HTTP_MOVED_PERM ||
                            code == HttpURLConnection.HTTP_SEE_OTHER ||
                            code == 307 || code == 308) {
                        String newLoc = dlConn.getHeaderField("Location");
                        dlConn.disconnect();
                        if (newLoc == null) {
                            throw new IOException("Redirect response missing Location header");
                        }
                        if (newLoc.startsWith("https://") || newLoc.startsWith("http://")) {
                            currentUrl = newLoc;
                        } else {
                            currentUrl = new URL(u, newLoc).toString();
                        }
                        redirectCount++;
                        continue;
                    }

                    if (code != HttpURLConnection.HTTP_OK) {
                        throw new IOException("Download connection returned HTTP " + code);
                    }
                    connected = true;
                    break;
                }
                if (!connected) {
                    // 轉址次數用完：dlConn 已經 disconnect，不能再拿來讀
                    throw new IOException("Too many download redirects");
                }
                if (abortDownloadBecauseStreaming(userInitiated, callback)) {
                    if (isDevOverride) {
                        sPendingDevUpdateUrl = devBaseUrl;
                    }
                    return;
                }

                long contentLen = dlConn.getContentLengthLong();
                long progressTotal = expectedSize > 0 ? expectedSize : contentLen;

                in = dlConn.getInputStream();
                out = new FileOutputStream(partApkFile);
                MessageDigest sha256Digest = MessageDigest.getInstance("SHA-256");

                byte[] buffer = new byte[8192];
                long totalRead = 0;
                int read;

                while ((read = in.read(buffer)) != -1) {
                    // §REVIEW-1118：資料在流時下一個 8KB 就停；對端卡住要等這次 read 逾時（30 秒）。
                    if (abortDownloadBecauseStreaming(userInitiated, callback)) {
                        if (isDevOverride) {
                            sPendingDevUpdateUrl = devBaseUrl;
                        }
                        return;
                    }
                    if (sDownloadCancelled) {
                        callback.onFailure("Download cancelled");
                        return;
                    }
                    out.write(buffer, 0, read);
                    sha256Digest.update(buffer, 0, read);
                    totalRead += read;
                    // §REVIEW-1111（第二輪）：超過 release 回報的大小就停（多給 1 MB 餘裕），不要把儲存空間寫滿
                    if (expectedSize > 0 && totalRead > expectedSize + (1 << 20)) {
                        throw new IOException("Downloaded file exceeds expected size");
                    }
                    if (expectedSize <= 0 && totalRead > MAX_DEV_APK_BYTES) {
                        throw new IOException("Downloaded file exceeds maximum allowed size");
                    }

                    if (listener != null && progressTotal > 0) {
                        int percent = (int) ((totalRead * 100) / progressTotal);
                        listener.onProgress(Math.min(percent, 100));
                    }
                }
                out.flush();
                out.close();
                out = null;

                // 4. 驗證檔案大小與 SHA-256
                if (partApkFile.length() <= 0 || (expectedSize > 0 && partApkFile.length() != expectedSize)) {
                    partApkFile.delete();
                    Log.e(TAG, LOG_PREFIX + "File size mismatch: expected " + expectedSize + ", actual " + partApkFile.length());
                    callback.onFailure("File size mismatch");
                    return;
                }

                String actualSha256 = bytesToHex(sha256Digest.digest());
                if (!actualSha256.equalsIgnoreCase(expectedSha256)) {
                    partApkFile.delete();
                    Log.e(TAG, LOG_PREFIX + "SHA-256 mismatch: expected " + expectedSha256 + ", actual " + actualSha256);
                    callback.onFailure("SHA-256 verification failed");
                    return;
                }

                if (finalApkFile.exists()) {
                    finalApkFile.delete();
                }
                if (!partApkFile.renameTo(finalApkFile)) {
                    callback.onFailure("Failed to rename temporary APK file");
                    return;
                }

                // 對落盤後的最終 APK 再次核對 SHA-256，防範重命名前後的檔案替換。
                if (!verifyFileSha256(finalApkFile, expectedSha256)) {
                    finalApkFile.delete();
                    callback.onFailure("Final APK SHA-256 verification failed");
                    return;
                }

                Log.i(TAG, LOG_PREFIX + "Download and SHA-256 verification succeeded; verifying APK package and signature.");

                // 5. 執行安全性驗證：Package Name、versionCode、簽章憑證
                if (!verifyApk(context, finalApkFile)) {
                    finalApkFile.delete();
                    callback.onFailure("APK signature or package verification failed");
                    return;
                }

                if (abortDownloadBecauseStreaming(userInitiated, callback)) {
                    if (isDevOverride) {
                        sPendingDevUpdateUrl = devBaseUrl;
                    }
                    return;
                }

                callback.onSuccess(finalApkFile);
            } finally {
                if (in != null) {
                    try { in.close(); } catch (Exception ignored) {}
                }
                if (out != null) {
                    try { out.close(); } catch (Exception ignored) {}
                }
                if (dlConn != null) {
                    dlConn.disconnect();
                }
                // §REVIEW-1111（第二輪）：中途失敗時不留半個暫存檔（成功時已經改名，這裡刪不到東西）
                if (partApkFile.exists()) {
                    partApkFile.delete();
                }
            }
        } catch (Exception e) {
            Log.e(TAG, LOG_PREFIX + "Download and verification failed: " + e.getMessage(), e);
            callback.onFailure(e.getMessage());
        } finally {
            if (apiConn != null) {
                apiConn.disconnect();
            }
        }
    }

    /**
     * 計算磁碟上 APK 檔案的 SHA-256 並與 release digest 比對
     */
    private static boolean verifyFileSha256(File file, String expectedSha256) {
        if (file == null || !file.exists() || expectedSha256 == null || !expectedSha256.matches("[0-9a-fA-F]{64}")) {
            return false;
        }
        FileInputStream fis = null;
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            fis = new FileInputStream(file);
            byte[] buf = new byte[8192];
            int n;
            while ((n = fis.read(buf)) != -1) {
                md.update(buf, 0, n);
            }
            String actual = bytesToHex(md.digest());
            if (!actual.equalsIgnoreCase(expectedSha256)) {
                Log.e(TAG, LOG_PREFIX + "File SHA-256 mismatch for " + file.getName()
                        + ": expected " + expectedSha256 + ", actual " + actual);
                return false;
            }
            return true;
        } catch (Exception e) {
            Log.e(TAG, LOG_PREFIX + "Failed to compute SHA-256 for " + file.getName() + ": " + e.getMessage());
            return false;
        } finally {
            if (fis != null) {
                try { fis.close(); } catch (IOException ignored) {}
            }
        }
    }

    /**
     * 對已下載的 APK 進行安全性檢查（Fail Closed）：
     * 1. 解析封裝資訊（PackageManager.getPackageArchiveInfo）
     * 2. 驗證套件名稱（Package Name）與目前應用程式一致
     * 3. 驗證 versionCode 大於目前已安裝版本（防範版本降級攻擊 Downgrade Attack）
     * 4. 驗證簽章憑證（Signing Certificates）與已安裝版本相符（支援憑證旋轉歷史與多簽章）
     */
    private static boolean verifyApk(Context context, File apkFile) {
        try {
            PackageManager pm = context.getPackageManager();

            // 取得已下載 APK 封裝資訊（minSdk 28 使用 GET_SIGNING_CERTIFICATES）
            PackageInfo archiveInfo = pm.getPackageArchiveInfo(
                    apkFile.getAbsolutePath(),
                    PackageManager.GET_SIGNING_CERTIFICATES
            );

            if (archiveInfo == null) {
                Log.e(TAG, LOG_PREFIX + "Failed to parse downloaded APK archive info.");
                return false;
            }

            // 1. 驗證套件名稱
            String expectedPackage = context.getPackageName();
            if (!expectedPackage.equals(archiveInfo.packageName)) {
                Log.e(TAG, LOG_PREFIX + "Package name mismatch: expected " + expectedPackage + ", actual " + archiveInfo.packageName);
                return false;
            }

            // 2. 取得目前已安裝應用的封裝資訊
            PackageInfo installedInfo = pm.getPackageInfo(
                    expectedPackage,
                    PackageManager.GET_SIGNING_CERTIFICATES
            );

            // 3. 驗證 versionCode：新版本必須嚴格大於目前安裝版本
            long installedVersionCode = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) ?
                    installedInfo.getLongVersionCode() : installedInfo.versionCode;
            long archiveVersionCode = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) ?
                    archiveInfo.getLongVersionCode() : archiveInfo.versionCode;

            if (archiveVersionCode <= installedVersionCode) {
                Log.e(TAG, LOG_PREFIX + "Refusing update: archive versionCode (" + archiveVersionCode +
                        ") is not greater than installed versionCode (" + installedVersionCode + ").");
                return false;
            }

            // 4. 驗證簽章憑證
            SigningInfo installedSigning = installedInfo.signingInfo;
            SigningInfo archiveSigning = archiveInfo.signingInfo;

            if (installedSigning == null || archiveSigning == null) {
                Log.e(TAG, LOG_PREFIX + "SigningInfo is null; cannot verify APK signature.");
                return false;
            }

            // 檢查多簽章（multiple signers）結構是否一致
            if (installedSigning.hasMultipleSigners() != archiveSigning.hasMultipleSigners()) {
                Log.e(TAG, LOG_PREFIX + "Signing structure mismatch (hasMultipleSigners differs).");
                return false;
            }

            if (installedSigning.hasMultipleSigners()) {
                // 多簽章情況：比對所有目前簽章者的憑證 SHA-256 集合
                Set<String> installedSigners = getCertFingerprints(installedSigning.getApkContentsSigners());
                Set<String> archiveSigners = getCertFingerprints(archiveSigning.getApkContentsSigners());
                if (installedSigners.isEmpty() || !installedSigners.equals(archiveSigners)) {
                    Log.e(TAG, LOG_PREFIX + "Multi-signer certificate fingerprint set mismatch.");
                    return false;
                }
            } else {
                // 單一簽章情況：比對當前有效簽章憑證與簽章歷史憑證鏈
                Signature[] installedPast = installedSigning.getSigningCertificateHistory();
                Signature[] archivePast = archiveSigning.getSigningCertificateHistory();

                Set<String> installedCurrent = getCertFingerprints(installedSigning.getApkContentsSigners());
                Set<String> archiveCurrent = getCertFingerprints(archiveSigning.getApkContentsSigners());

                Set<String> installedHistory = getCertFingerprints(installedPast != null ? installedPast : installedSigning.getApkContentsSigners());
                Set<String> archiveHistory = getCertFingerprints(archivePast != null ? archivePast : archiveSigning.getApkContentsSigners());

                // 當前簽章者必須一致，或下載 APK 的歷史憑證鏈包含已安裝的原始根憑證
                // VipleStream REVIEW-1111 (round 2): an empty set makes containsAll() true, which let any signer through
                if (installedCurrent.isEmpty() || archiveCurrent.isEmpty() || installedHistory.isEmpty()
                        || (!installedCurrent.equals(archiveCurrent) && !archiveHistory.containsAll(installedHistory))) {
                    Log.e(TAG, LOG_PREFIX + "Downloaded APK signing certificate does not match installed app.");
                    return false;
                }
            }

            Log.i(TAG, LOG_PREFIX + "APK package name, versionCode (" + archiveVersionCode + "), and signing certificate verified.");
            return true;
        } catch (Exception e) {
            Log.e(TAG, LOG_PREFIX + "Exception while verifying APK: " + e.getMessage(), e);
            return false;
        }
    }

    private static Set<String> getCertFingerprints(Signature[] signatures) {
        Set<String> set = new HashSet<>();
        if (signatures != null) {
            for (Signature sig : signatures) {
                set.add(sha256Hex(sig.toByteArray()));
            }
        }
        return set;
    }

    private static String sha256Hex(byte[] data) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            return bytesToHex(md.digest(data));
        } catch (NoSuchAlgorithmException e) {
            throw new RuntimeException(e);
        }
    }

    private static String bytesToHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes) {
            sb.append(String.format("%02x", b));
        }
        return sb.toString();
    }

    /**
     * 安裝 APK：檢查安裝未知應用程式權限，必要時開啟系統設定，否則呼叫系統套件安裝程式
     */
    public static void installApk(final Activity activity, final File apkFile) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) {
            return;
        }
        if (apkFile == null || !apkFile.exists()) {
            Log.e(TAG, LOG_PREFIX + "APK file to install does not exist.");
            return;
        }

        // Android 8.0+（API 26+）檢查是否允許安裝未知應用程式
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if (!activity.getPackageManager().canRequestPackageInstalls()) {
                Log.i(TAG, LOG_PREFIX + "App lacks permission to install unknown apps; prompting user.");
                sPendingInstallApk = apkFile;
                showUnknownAppsPermissionDialog(activity);
                return;
            }
        }

        doInstall(activity, apkFile);
    }

    /**
     * 提示使用者開啟「安裝未知應用程式」權限
     */
    private static void showUnknownAppsPermissionDialog(final Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (activity.isFinishing() || activity.isDestroyed()) {
                    return;
                }
                dismissCurrentDialog();

                AlertDialog.Builder builder = new AlertDialog.Builder(activity)
                        .setTitle(R.string.update_permission_title)
                        .setMessage(R.string.update_permission_message)
                        .setPositiveButton(R.string.update_button_settings, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                openUnknownAppsSettings(activity);
                            }
                        })
                        .setNegativeButton(android.R.string.cancel, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                sPendingInstallApk = null;
                            }
                        })
                        // VipleStream §REVIEW-1111: dismissing the dialog (back key, tap outside) left the pending APK set
                        // with nothing to prompt again, so the update was stuck until the next 24 h check.
                        .setOnCancelListener(new DialogInterface.OnCancelListener() {
                            @Override
                            public void onCancel(DialogInterface dialog) {
                                sPendingInstallApk = null;
                            }
                        });

                sCurrentDialog = builder.show();
            }
        });
    }

    private static void openUnknownAppsSettings(Activity activity) {
        Uri packageUri = Uri.parse("package:" + activity.getPackageName());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Intent managePkgIntent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, packageUri);
            if (tryStartSettingsIntent(activity, managePkgIntent)) {
                return;
            }
            // Horizon OS（Meta Quest）精簡過的系統設定可能不支援帶 package URI 的未知來源頁面，退回通用未知來源頁或應用程式詳細資料頁。
            Intent manageAllIntent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES);
            if (tryStartSettingsIntent(activity, manageAllIntent)) {
                android.widget.Toast.makeText(activity, R.string.update_permission_fallback_hint, android.widget.Toast.LENGTH_LONG).show();
                return;
            }
        }
        Intent detailsIntent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, packageUri);
        if (tryStartSettingsIntent(activity, detailsIntent)) {
            android.widget.Toast.makeText(activity, R.string.update_permission_fallback_hint, android.widget.Toast.LENGTH_LONG).show();
            return;
        }
        Intent generalSettingsIntent = new Intent(Settings.ACTION_SETTINGS);
        if (tryStartSettingsIntent(activity, generalSettingsIntent)) {
            android.widget.Toast.makeText(activity, R.string.update_permission_fallback_hint, android.widget.Toast.LENGTH_LONG).show();
            return;
        }
        sPendingInstallApk = null;
        android.widget.Toast.makeText(activity, R.string.update_permission_fallback_hint, android.widget.Toast.LENGTH_LONG).show();
    }

    private static boolean tryStartSettingsIntent(Activity activity, Intent intent) {
        try {
            activity.startActivity(intent);
            return true;
        } catch (Exception e) {
            Log.w(TAG, LOG_PREFIX + "Failed to start settings intent " + intent.getAction() + ": " + e.getMessage());
            return false;
        }
    }

    /**
     * 呼叫系統 ACTION_VIEW 啟動套件安裝程式
     */
    private static void doInstall(Activity activity, File apkFile) {
        try {
            String authority = activity.getPackageName() + ".updates";
            Uri contentUri = UpdateFileProvider.getUriForFile(activity, authority, apkFile);

            Intent installIntent = new Intent(Intent.ACTION_VIEW);
            installIntent.setDataAndType(contentUri, "application/vnd.android.package-archive");
            installIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            installIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

            Log.i(TAG, LOG_PREFIX + "Launching package installer for: " + contentUri);
            activity.startActivity(installIntent);
        } catch (Exception e) {
            Log.e(TAG, LOG_PREFIX + "Failed to launch package installer: " + e.getMessage(), e);
        }
    }

    private static boolean isStreamRunning() {
        return Game.isStreaming() || VrStreamActivity.isStreaming();
    }

    // §REVIEW-1118：為什麼在這裡停——檢查只在下載開始前看串流旗標，進遊戲後 APK 仍跟畫面搶同一條 Wi-Fi。
    // 部分檔案由呼叫端 finally 刪掉。使用者手動取消不走這裡，不會被當成「等一下再查」。
    private static boolean abortDownloadBecauseStreaming(boolean userInitiated, DownloadCallback callback) {
        if (!isStreamRunning()) {
            return false;
        }
        sRetryAfterStream = true;
        sDownloadCancelled = true;
        Log.i(TAG, LOG_PREFIX + "Stream started; aborting update download and deferring until stream ends.");
        if (userInitiated && callback != null) {
            callback.onFailure("Download cancelled due to active stream");
        }
        return true;
    }

    private static String normalizeAndValidateDevBaseUrl(String rawUrl) {
        if (rawUrl == null) {
            return null;
        }
        String trimmed = rawUrl.trim();
        while (trimmed.endsWith("/")) {
            trimmed = trimmed.substring(0, trimmed.length() - 1);
        }
        if (trimmed.isEmpty()) {
            return null;
        }
        if (!isAllowedDownloadUrl(trimmed, true)) {
            return null;
        }
        return trimmed;
    }

    private static boolean isAllowedDownloadUrl(String rawUrl, boolean allowDevCleartext) {
        if (rawUrl == null || rawUrl.isEmpty()) {
            return false;
        }
        try {
            URL parsed = new URL(rawUrl);
            if (parsed.getUserInfo() != null) {
                return false;
            }
            String protocol = parsed.getProtocol();
            String host = parsed.getHost();
            if (host == null || host.isEmpty()) {
                return false;
            }
            if ("https".equalsIgnoreCase(protocol)) {
                return true;
            }
            if (allowDevCleartext && "http".equalsIgnoreCase(protocol)) {
                return isLocalOrPrivateHost(host);
            }
            return false;
        } catch (Exception e) {
            return false;
        }
    }

    private static boolean isLocalOrPrivateHost(String host) {
        String h = host.trim().toLowerCase(Locale.ROOT);
        if (h.startsWith("[") && h.endsWith("]")) {
            h = h.substring(1, h.length() - 1);
        }
        if ("localhost".equals(h) || "::1".equals(h) || "0:0:0:0:0:0:0:1".equals(h)) {
            return true;
        }
        // 只用字面量判斷 IPv4 私有與迴路網段，不觸發 DNS 解析，避免主執行緒網路例外與 DNS rebinding。
        String[] parts = h.split("\\.");
        if (parts.length != 4) {
            return false;
        }
        int[] octets = new int[4];
        for (int i = 0; i < 4; i++) {
            String p = parts[i];
            if (p.isEmpty() || p.length() > 3) {
                return false;
            }
            for (int j = 0; j < p.length(); j++) {
                char c = p.charAt(j);
                if (c < '0' || c > '9') {
                    return false;
                }
            }
            int val = Integer.parseInt(p);
            if (val < 0 || val > 255) {
                return false;
            }
            octets[i] = val;
        }
        // 127.0.0.0/8 (loopback)
        if (octets[0] == 127) {
            return true;
        }
        // 10.0.0.0/8 (RFC 1918)
        if (octets[0] == 10) {
            return true;
        }
        // 172.16.0.0/12 (RFC 1918)
        if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) {
            return true;
        }
        // 192.168.0.0/16 (RFC 1918)
        if (octets[0] == 192 && octets[1] == 168) {
            return true;
        }
        return false;
    }

    // §REVIEW-1119：為什麼設上限並關掉串流——這段把對端的 HTTP 本文整份讀進記憶體，
    // 沒有上限時超長回應會把行程撐爆；呼叫端只 disconnect，輸入串流沒關。
    private static String readStreamToString(InputStream is) throws IOException {
        ByteArrayOutputStream baos = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int len;
        int total = 0;
        try {
            while ((len = is.read(buf)) != -1) {
                total += len;
                if (total > 1024 * 1024) {
                    throw new IOException("HTTP response exceeds maximum allowed size");
                }
                baos.write(buf, 0, len);
            }
            return baos.toString("UTF-8");
        } finally {
            try {
                is.close();
            } catch (IOException ignored) {
            }
        }
    }
}
