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

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
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

    public static final String PREF_UPDATE_MODE = "update_mode";
    public static final String MODE_OFF = "off";
    public static final String MODE_NOTIFY = "notify";
    public static final String MODE_AUTO = "auto";

    private static final String PREF_LAST_CHECK_TIME = "last_update_check_time";
    private static final String PREF_SKIPPED_VERSION = "skipped_update_version";
    private static final String PREF_SKIPPED_TIME = "skipped_update_time";

    private static final long CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000L; // 24 小時檢查間隔

    private static final ExecutorService sExecutor = Executors.newSingleThreadExecutor();
    private static boolean sDebugLogged = false;

    // 儲存目前顯示中的對話方塊，防止 Activity 銷毀時產生視窗洩漏（Window Leaks）
    private static Dialog sCurrentDialog = null;

    // 當前往系統設定開啟「安裝未知應用程式」權限時，暫存等待安裝的 APK 檔案
    private static File sPendingInstallApk = null;

    // 下載任務取消控制旗標
    private static volatile boolean sDownloadCancelled = false;

    /**
     * 當主畫面（PcView）恢復執行（onResume）時呼叫
     */
    public static void onActivityResume(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) {
            return;
        }

        // 1. 檢查是否有等待使用者授權後繼續安裝的 APK 檔案
        if (sPendingInstallApk != null && sPendingInstallApk.exists()) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                if (activity.getPackageManager().canRequestPackageInstalls()) {
                    Log.i(TAG, LOG_PREFIX + "使用者已授予安裝權限，繼續安裝更新。");
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
        // 1. 若為除錯建置（Debug Build）則完全跳過，僅記錄一次日誌
        if (BuildConfig.APPLICATION_ID.endsWith(".debug") || BuildConfig.DEBUG) {
            if (!sDebugLogged) {
                sDebugLogged = true;
                Log.i(TAG, LOG_PREFIX + "偵測到除錯版本（Debug build），自我更新功能已停用。");
            }
            return;
        }

        // 2. 若目前正在進行串流，絕不執行更新檢查
        if (isStreamRunning()) {
            Log.i(TAG, LOG_PREFIX + "目前正在進行遊戲串流，跳過更新檢查。");
            return;
        }

        final Context appContext = activity.getApplicationContext();
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(appContext);

        // 3. 檢查使用者的更新模式偏好設定
        String mode = prefs.getString(PREF_UPDATE_MODE, MODE_AUTO);
        if (MODE_OFF.equalsIgnoreCase(mode)) {
            Log.i(TAG, LOG_PREFIX + "使用者已將更新模式設為關閉（Off）。");
            return;
        }

        // 4. 檢查上次檢查時間，最多每 24 小時執行一次
        long lastCheck = prefs.getLong(PREF_LAST_CHECK_TIME, 0);
        long now = System.currentTimeMillis();
        if (now - lastCheck < CHECK_INTERVAL_MS && now >= lastCheck) {
            return;
        }

        // 更新最後檢查時間
        prefs.edit().putLong(PREF_LAST_CHECK_TIME, now).apply();

        // 在背景執行緒執行網路檢查
        sExecutor.execute(new Runnable() {
            @Override
            public void run() {
                performUpdateCheck(activity, appContext);
            }
        });
    }

    /**
     * 背景執行緒：執行 GitHub 標籤檢查與後續流程
     */
    private static void performUpdateCheck(final Activity activity, final Context context) {
        try {
            // 透過 HEAD 請求取得最新版本標籤（避免消耗 GitHub API 配額）
            String latestTag = fetchLatestReleaseTagHead();
            if (latestTag == null || latestTag.isEmpty()) {
                Log.i(TAG, LOG_PREFIX + "HEAD 檢查最新版本失敗，切換至 GET API 備援檢查。");
                latestTag = fetchLatestReleaseTagGet();
            }

            if (latestTag == null || latestTag.isEmpty()) {
                Log.w(TAG, LOG_PREFIX + "無法取得最新發行版本標籤。");
                return;
            }

            Log.i(TAG, LOG_PREFIX + "取得最新版本標籤：" + latestTag);

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
                Log.i(TAG, LOG_PREFIX + "軟體已是最新版本（目前版本：" + currentVersion + "，最新版本：" + remoteVersion + "）。");
                return;
            }

            Log.i(TAG, LOG_PREFIX + "發現新版本：" + remoteVersion + "（目前版本：" + currentVersion + "）。");

            SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(context);
            final String mode = prefs.getString(PREF_UPDATE_MODE, MODE_AUTO);
            final String finalTag = latestTag;

            if (MODE_AUTO.equalsIgnoreCase(mode)) {
                // 自動模式：僅在非計量網路（Unmetered Network）下自動下載
                ConnectivityManager cm = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
                if (cm != null && cm.isActiveNetworkMetered()) {
                    Log.i(TAG, LOG_PREFIX + "偵測到計量網路（行動數據），延後自動下載。");
                    return;
                }

                // 檢查是否先前已下載並驗證完成
                File updatesDir = new File(context.getCacheDir(), "updates");
                File targetApk = new File(updatesDir, "VipleStream-Android-" + remoteVersion + ".apk");
                if (targetApk.exists() && verifyApk(context, targetApk)) {
                    Log.i(TAG, LOG_PREFIX + "新版本 APK 先前已下載並驗證成功，顯示安裝提示對話方塊。");
                    showDownloadedDialog(activity, remoteVersion, targetApk);
                    return;
                }

                // 在背景下載並驗證（先清掉上一次使用者取消留下的旗標，否則自動下載會立刻中止）
                sDownloadCancelled = false;
                downloadAndVerify(activity, context, remoteVersion, finalTag, false, new DownloadCallback() {
                    @Override
                    public void onSuccess(File apkFile) {
                        showDownloadedDialog(activity, remoteVersion, apkFile);
                    }

                    @Override
                    public void onFailure(String errorMsg) {
                        Log.e(TAG, LOG_PREFIX + "自動下載或驗證失敗：" + errorMsg);
                    }
                });
            } else {
                // 通知模式（notify）：檢查使用者是否針對此版本點選過「稍後」
                String skippedVersion = prefs.getString(PREF_SKIPPED_VERSION, "");
                long skippedTime = prefs.getLong(PREF_SKIPPED_TIME, 0);
                if (remoteVersion.equals(skippedVersion)) {
                    long elapsed = System.currentTimeMillis() - skippedTime;
                    if (elapsed >= 0 && elapsed < CHECK_INTERVAL_MS) {
                        Log.i(TAG, LOG_PREFIX + "版本 " + remoteVersion + " 曾在 24 小時內被使用者延後，跳過通知。");
                        return;
                    }
                }

                // 在 UI 執行緒顯示更新提示對話方塊
                showUpdateAvailableDialog(activity, remoteVersion, finalTag);
            }
        } catch (Exception e) {
            Log.e(TAG, LOG_PREFIX + "更新檢查發生異常：" + e.getMessage(), e);
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
            Log.w(TAG, LOG_PREFIX + "HEAD 請求失敗：" + e.getMessage());
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
    private static void loadReleaseNotesInto(final Activity activity, final AlertDialog dialog, final String baseMessage, final String tag) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                HttpURLConnection conn = null;
                try {
                    URL url = new URL("https://api.github.com/repos/finaltwinsen/VipleStream/releases/tags/" + tag);
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
                    Log.w(TAG, LOG_PREFIX + "更新摘要抓取失敗：" + e.getMessage());
                } finally {
                    if (conn != null) {
                        conn.disconnect();
                    }
                }
            }
        }, "UpdateNotes").start();
    }

    /**
     * 備援方案：發送 GET 請求至 GitHub Releases API 取得 tag_name
     */
    private static String fetchLatestReleaseTagGet() {
        HttpURLConnection conn = null;
        try {
            URL url = new URL("https://api.github.com/repos/finaltwinsen/VipleStream/releases/latest");
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
            Log.w(TAG, LOG_PREFIX + "GET 備援請求失敗：" + e.getMessage());
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
    private static void showUpdateAvailableDialog(final Activity activity, final String remoteVersion, final String tag) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (activity.isFinishing() || activity.isDestroyed()) {
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
                                startUserInitiatedDownload(activity, remoteVersion, tag);
                            }
                        })
                        .setNegativeButton(R.string.update_button_later, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                // 記錄稍後延後狀態，24 小時內不再提示
                                SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(activity.getApplicationContext());
                                prefs.edit()
                                        .putString(PREF_SKIPPED_VERSION, remoteVersion)
                                        .putLong(PREF_SKIPPED_TIME, System.currentTimeMillis())
                                        .apply();
                                Log.i(TAG, LOG_PREFIX + "使用者選擇「稍後」，版本 " + remoteVersion + " 提示已延後 24 小時。");
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
                                    Log.e(TAG, LOG_PREFIX + "無法開啟版本資訊連結：" + e.getMessage());
                                }
                            }
                        });

                final AlertDialog shown = builder.show();
                sCurrentDialog = shown;
                // §UPDATE-NOTES：對話框先出來，更新摘要在背景抓，抓到了再補在訊息後面（抓不到就維持原樣）
                loadReleaseNotesInto(activity, shown, message, tag);
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
                if (activity.isFinishing() || activity.isDestroyed()) {
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
    private static void startUserInitiatedDownload(final Activity activity, final String remoteVersion, final String tag) {
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
                        Log.i(TAG, LOG_PREFIX + "使用者取消了更新下載。");
                    }
                })
                .setCancelable(true)
                .setOnCancelListener(new DialogInterface.OnCancelListener() {
                    @Override
                    public void onCancel(DialogInterface dialog) {
                        sDownloadCancelled = true;
                        Log.i(TAG, LOG_PREFIX + "使用者取消了更新下載。");
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
                downloadAndVerify(activity, activity.getApplicationContext(), remoteVersion, tag, true, new DownloadCallback() {
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

    private static void downloadAndVerify(Activity activity, Context context, String remoteVersion, String tag,
                                          boolean userInitiated, DownloadCallback callback) {
        downloadAndVerify(activity, context, remoteVersion, tag, userInitiated, callback, null);
    }

    /**
     * 從 GitHub Tags API 取得資產資訊，下載 APK 檔案並進行全面安全性驗證
     */
    private static void downloadAndVerify(Activity activity, Context context, String remoteVersion, String tag,
                                          boolean userInitiated, DownloadCallback callback, ProgressListener listener) {
        HttpURLConnection apiConn = null;
        try {
            // 1. 呼叫 Tags API 取得資產中繼資料
            URL apiUrl = new URL("https://api.github.com/repos/finaltwinsen/VipleStream/releases/tags/" + tag);
            apiConn = (HttpURLConnection) apiUrl.openConnection();
            apiConn.setRequestMethod("GET");
            apiConn.setConnectTimeout(15000);
            apiConn.setReadTimeout(15000);
            apiConn.setRequestProperty("User-Agent", "VipleStream-Android/" + BuildConfig.VERSION_NAME);
            apiConn.setRequestProperty("Accept", "application/vnd.github.v3+json");

            if (apiConn.getResponseCode() != HttpURLConnection.HTTP_OK) {
                callback.onFailure("Tags API 回傳 HTTP " + apiConn.getResponseCode());
                return;
            }

            String jsonStr = readStreamToString(apiConn.getInputStream());
            JSONObject releaseObj = new JSONObject(jsonStr);
            JSONArray assets = releaseObj.optJSONArray("assets");

            if (assets == null) {
                callback.onFailure("發行版本中無資產清單");
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

            // §SECURITY：預設拒絕（Fail Closed）原則，若缺少資產或缺少 digest 雜湊值則拒絕更新
            if (downloadUrl == null || expectedSize <= 0 || digest == null || !digest.toLowerCase(Locale.ROOT).startsWith("sha256:")) {
                Log.e(TAG, LOG_PREFIX + "找不到資產 " + targetAssetName + " 或資產缺少 sha256 digest。拒絕更新。");
                callback.onFailure("資產中缺少 digest 驗證資訊");
                return;
            }

            String expectedSha256 = digest.substring("sha256:".length()).trim();

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

            // 若目標檔案已存在且通過完整驗證，直接使用
            if (finalApkFile.exists() && finalApkFile.length() == expectedSize && verifyApk(context, finalApkFile)) {
                Log.i(TAG, LOG_PREFIX + "檔案已存在且通過驗證：" + finalApkFile.getAbsolutePath());
                callback.onSuccess(finalApkFile);
                return;
            }

            if (partApkFile.exists()) {
                partApkFile.delete();
            }

            // 3. 下載 APK 檔案（手動支援最多 5 次 HTTP 重定向）
            Log.i(TAG, LOG_PREFIX + "開始下載 APK：" + downloadUrl);
            HttpURLConnection dlConn = null;
            InputStream in = null;
            FileOutputStream out = null;
            String currentUrl = downloadUrl;
            int redirectCount = 0;

            try {
                boolean connected = false;
                while (redirectCount < 5) {
                    // §REVIEW-1111（第二輪）：只走 https（轉址也是）；轉址沒給位置就當失敗
                    if (currentUrl == null || !currentUrl.startsWith("https://")) {
                        throw new IOException("下載位址不是 https");
                    }
                    URL u = new URL(currentUrl);
                    dlConn = (HttpURLConnection) u.openConnection();
                    dlConn.setInstanceFollowRedirects(true);
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
                        currentUrl = newLoc;
                        redirectCount++;
                        continue;
                    }

                    if (code != HttpURLConnection.HTTP_OK) {
                        throw new IOException("下載連線回傳 HTTP " + code);
                    }
                    connected = true;
                    break;
                }
                if (!connected) {
                    // 轉址次數用完：dlConn 已經 disconnect，不能再拿來讀
                    throw new IOException("下載轉址次數過多");
                }

                in = dlConn.getInputStream();
                out = new FileOutputStream(partApkFile);
                MessageDigest sha256Digest = MessageDigest.getInstance("SHA-256");

                byte[] buffer = new byte[8192];
                long totalRead = 0;
                int read;

                while ((read = in.read(buffer)) != -1) {
                    if (sDownloadCancelled) {
                        partApkFile.delete();
                        callback.onFailure("下載已取消");
                        return;
                    }
                    out.write(buffer, 0, read);
                    sha256Digest.update(buffer, 0, read);
                    totalRead += read;
                    // §REVIEW-1111（第二輪）：超過 release 回報的大小就停（多給 1 MB 餘裕），不要把儲存空間寫滿
                    if (expectedSize > 0 && totalRead > expectedSize + (1 << 20)) {
                        throw new IOException("下載的檔案比預期大");
                    }

                    if (listener != null && expectedSize > 0) {
                        int percent = (int) ((totalRead * 100) / expectedSize);
                        listener.onProgress(Math.min(percent, 100));
                    }
                }
                out.flush();

                // 4. 驗證檔案大小與 SHA-256
                if (partApkFile.length() != expectedSize) {
                    partApkFile.delete();
                    Log.e(TAG, LOG_PREFIX + "檔案大小不符：預期 " + expectedSize + " 位元組，實際 " + partApkFile.length() + " 位元組。");
                    callback.onFailure("檔案大小不符");
                    return;
                }

                String actualSha256 = bytesToHex(sha256Digest.digest());
                if (!actualSha256.equalsIgnoreCase(expectedSha256)) {
                    partApkFile.delete();
                    Log.e(TAG, LOG_PREFIX + "SHA-256 雜湊值不符：預期 " + expectedSha256 + "，實際 " + actualSha256);
                    callback.onFailure("雜湊驗證失敗");
                    return;
                }

                if (finalApkFile.exists()) {
                    finalApkFile.delete();
                }
                if (!partApkFile.renameTo(finalApkFile)) {
                    callback.onFailure("無法重新命名暫存檔案");
                    return;
                }

                Log.i(TAG, LOG_PREFIX + "檔案下載與雜湊檢查成功，開始執行 APK 簽章與版本序號驗證。");

                // 5. 執行安全性驗證：Package Name、versionCode、簽章憑證
                if (!verifyApk(context, finalApkFile)) {
                    finalApkFile.delete();
                    callback.onFailure("APK 憑證或版本安全驗證失敗");
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
            Log.e(TAG, LOG_PREFIX + "下載與驗證程序失敗：" + e.getMessage(), e);
            callback.onFailure(e.getMessage());
        } finally {
            if (apiConn != null) {
                apiConn.disconnect();
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
                Log.e(TAG, LOG_PREFIX + "無法解析下載的 APK 封裝資訊，檔案可能已損毀。");
                return false;
            }

            // 1. 驗證套件名稱
            String expectedPackage = context.getPackageName();
            if (!expectedPackage.equals(archiveInfo.packageName)) {
                Log.e(TAG, LOG_PREFIX + "套件名稱不符：預期 " + expectedPackage + "，實際 " + archiveInfo.packageName);
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
                Log.e(TAG, LOG_PREFIX + "拒絕更新：下載的 APK versionCode (" + archiveVersionCode +
                        ") 未大於已安裝的 versionCode (" + installedVersionCode + ")。");
                return false;
            }

            // 4. 驗證簽章憑證
            SigningInfo installedSigning = installedInfo.signingInfo;
            SigningInfo archiveSigning = archiveInfo.signingInfo;

            if (installedSigning == null || archiveSigning == null) {
                Log.e(TAG, LOG_PREFIX + "SigningInfo 為空，無法驗證簽章。");
                return false;
            }

            // 檢查多簽章（multiple signers）結構是否一致
            if (installedSigning.hasMultipleSigners() != archiveSigning.hasMultipleSigners()) {
                Log.e(TAG, LOG_PREFIX + "簽章架構不相符（hasMultipleSigners 不一致）。");
                return false;
            }

            if (installedSigning.hasMultipleSigners()) {
                // 多簽章情況：比對所有目前簽章者的憑證 SHA-256 集合
                Set<String> installedSigners = getCertFingerprints(installedSigning.getApkContentsSigners());
                Set<String> archiveSigners = getCertFingerprints(archiveSigning.getApkContentsSigners());
                if (installedSigners.isEmpty() || !installedSigners.equals(archiveSigners)) {
                    Log.e(TAG, LOG_PREFIX + "多簽章憑證雜湊集合不相符。");
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
                    Log.e(TAG, LOG_PREFIX + "下載的 APK 簽章憑證與已安裝應用程式不相符。");
                    return false;
                }
            }

            Log.i(TAG, LOG_PREFIX + "APK 套件名稱、versionCode (" + archiveVersionCode + ") 與簽章憑證安全驗證全部通過。");
            return true;
        } catch (Exception e) {
            Log.e(TAG, LOG_PREFIX + "驗證 APK 時發生例外：" + e.getMessage(), e);
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
            Log.e(TAG, LOG_PREFIX + "欲安裝的 APK 檔案不存在。");
            return;
        }

        // Android 8.0+（API 26+）檢查是否允許安裝未知應用程式
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if (!activity.getPackageManager().canRequestPackageInstalls()) {
                Log.i(TAG, LOG_PREFIX + "應用程式尚未取得安裝未知應用程式權限，提示使用者前往設定。");
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
                                try {
                                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                                        Intent intent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES);
                                        intent.setData(Uri.parse("package:" + activity.getPackageName()));
                                        activity.startActivity(intent);
                                    }
                                } catch (Exception e) {
                                    Log.e(TAG, LOG_PREFIX + "無法開啟安裝未知應用程式設定畫面：" + e.getMessage());
                                }
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

            Log.i(TAG, LOG_PREFIX + "啟動套件安裝程式：" + contentUri);
            activity.startActivity(installIntent);
        } catch (Exception e) {
            Log.e(TAG, LOG_PREFIX + "啟動套件安裝程式失敗：" + e.getMessage(), e);
        }
    }

    private static boolean isStreamRunning() {
        return Game.isStreaming();
    }

    private static String readStreamToString(InputStream is) throws IOException {
        ByteArrayOutputStream baos = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int len;
        while ((len = is.read(buf)) != -1) {
            baos.write(buf, 0, len);
        }
        return baos.toString("UTF-8");
    }
}
