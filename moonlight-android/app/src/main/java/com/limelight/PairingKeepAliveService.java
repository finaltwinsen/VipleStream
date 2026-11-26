package com.limelight;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * §PAIR-BG（2026-11-26）：配對期間的前景服務。
 *
 * 使用者用手機配對、再切到 Chrome 遠端桌面去電腦上輸入 PIN：App 一離開前景，PIN 對話框就被 onStop 關掉，
 * 行程隨後被 Android 凍結（cached app freezer），server 送回的憑證回應收不到、下一步也送不出去，配對卡死
 * （server log：[PAIR-PIN] received → getservercert 之後再也沒有 clientchallenge）。
 * 配對期間掛這個前景服務讓行程不被凍結，通知顯示 PIN，切到別的 App 也看得到。配對一結束（成功、失敗、例外）就停。
 * Android 14 起用 shortService 類型（不需要額外權限，系統約 3 分鐘逾時；server 端等 PIN 的配對本來就只留 3 分鐘）。
 */
public class PairingKeepAliveService extends Service {
    private static final String CHANNEL_ID = "pairing";
    private static final int NOTIFICATION_ID = 0x5041;  // 'PA'
    private static final String EXTRA_TITLE = "title";
    private static final String EXTRA_TEXT = "text";

    public static void start(Context context, String title, String text) {
        try {
            Intent i = new Intent(context, PairingKeepAliveService.class);
            i.putExtra(EXTRA_TITLE, title);
            i.putExtra(EXTRA_TEXT, text);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(i);
            }
            else {
                context.startService(i);
            }
        } catch (RuntimeException e) {
            // 啟動不了（例如背景啟動限制）就照舊行為：配對照跑，只是切走時可能被凍結
            LimeLog.warning("[VIPLE-PAIR-BG] keep-alive service not started: " + e);
        }
    }

    public static void stop(Context context) {
        try {
            context.stopService(new Intent(context, PairingKeepAliveService.class));
        } catch (RuntimeException ignored) {
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String title = intent != null ? intent.getStringExtra(EXTRA_TITLE) : null;
        String text = intent != null ? intent.getStringExtra(EXTRA_TEXT) : null;
        if (title == null) {
            title = getString(R.string.pair_pairing_title);
        }

        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if (nm != null && nm.getNotificationChannel(CHANNEL_ID) == null) {
                // 高重要性：切到別的 App 的那一刻會跳出浮動通知，PIN 一眼就看得到
                NotificationChannel ch = new NotificationChannel(CHANNEL_ID, getString(R.string.pair_pairing_title),
                        NotificationManager.IMPORTANCE_HIGH);
                ch.setSound(null, null);
                ch.enableVibration(false);
                nm.createNotificationChannel(ch);
            }
            b = new Notification.Builder(this, CHANNEL_ID);
        }
        else {
            b = new Notification.Builder(this);
        }

        Intent open = new Intent(this, PcView.class);
        open.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        Notification n = b.setSmallIcon(R.drawable.ic_lock)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setContentIntent(pi)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .build();

        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SHORT_SERVICE);
            }
            else {
                startForeground(NOTIFICATION_ID, n);
            }
            LimeLog.info("[VIPLE-PAIR-BG] keep-alive started");
        } catch (RuntimeException e) {
            LimeLog.warning("[VIPLE-PAIR-BG] startForeground failed: " + e);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    // Android 14 shortService 逾時：一定要自己停，否則系統當成 ANR
    @Override
    public void onTimeout(int startId) {
        LimeLog.warning("[VIPLE-PAIR-BG] keep-alive timed out");
        stopSelf();
    }

    @Override
    public void onDestroy() {
        LimeLog.info("[VIPLE-PAIR-BG] keep-alive stopped");
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
