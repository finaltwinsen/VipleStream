package com.limelight.glasses;

import android.annotation.SuppressLint;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbManager;
import android.os.Build;

import com.limelight.vr.QxrNative;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * §VITURE：adb 與畫面選單要把同一組 extra 送給 Game，眼鏡 3DoF 才會啟動。
 */
public final class GlassesVrLaunch {
    /** §VITURE：跟 CLI 沒帶 vr_eye 時寫進 intent 的單眼尺寸相同。 */
    public static final String DEFAULT_EYE = "1920x1080";
    /** §VITURE：跟 CLI 沒帶 vr_hz 時寫進 intent 的更新率相同。 */
    public static final int DEFAULT_HZ = 60;

    private static final AtomicInteger presenceGeneration = new AtomicInteger();
    // 長按選單在主執行緒。getDeviceList 是跨程序呼叫，所以只讀這份快取。
    private static volatile boolean glassesPresent;

    private GlassesVrLaunch() {
    }

    /**
     * §VITURE：onResume 在背景掃一次，插上或拔除時再掃。選單只讀結果，不在主執行緒列 USB 裝置。
     * 回傳的 receiver 要用同一個 Context 解註冊。
     */
    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    public static BroadcastReceiver registerPresenceWatcher(Context context) {
        refreshPresenceAsync(context);
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context ctx, Intent intent) {
                refreshPresenceAsync(ctx != null ? ctx : context);
            }
        };
        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // 系統廣播仍送得到。標成不匯出，其他 App 不能對這個 receiver 灌插拔事件。
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            context.registerReceiver(receiver, filter);
        }
        return receiver;
    }

    public static void unregisterPresenceWatcher(Context context, BroadcastReceiver receiver) {
        if (context == null || receiver == null) {
            return;
        }
        try {
            context.unregisterReceiver(receiver);
        } catch (IllegalArgumentException ignored) {
            // 已經解註冊時再解一次會把 onPause 打掛。
        }
    }

    public static void refreshPresenceAsync(Context context) {
        if (context == null) {
            return;
        }
        final Context app = context.getApplicationContext();
        final int generation = presenceGeneration.incrementAndGet();
        new Thread(new Runnable() {
            @Override
            public void run() {
                boolean present = false;
                try {
                    UsbManager usb = (UsbManager) app.getSystemService(Context.USB_SERVICE);
                    present = VitureUsb.findGlassesDevice(usb) != null;
                } catch (RuntimeException error) {
                    VitureUsb.logW("glasses presence scan failed", error);
                }
                if (generation == presenceGeneration.get()) {
                    glassesPresent = present;
                }
            }
        }, "viple-glasses-scan").start();
    }

    public static void applyExtras(Intent gameIntent, String vrEye, int vrHz) {
        String eye = vrEye;
        if (eye == null || eye.isEmpty()) {
            eye = DEFAULT_EYE;
        }
        gameIntent.putExtra("glasses_vr", true);
        gameIntent.putExtra("vr_eye", eye);
        gameIntent.putExtra("vr_hz", vrHz);
    }

    /**
     * §VITURE：選單只在快取裡有眼鏡時出現。這裡不呼叫 getDeviceList，長按才不會在主執行緒做 Binder。
     * 快取還沒掃完時當成沒有眼鏡；下一次長按會讀到背景掃描的結果。還沒取得 USB 權限也能判斷，因為掃描不開啟裝置。
     */
    public static boolean canShowMenu(Context context) {
        if (context == null || !glassesPresent) {
            return false;
        }
        // §VITURE：先確認有眼鏡再載入程式庫。沒接眼鏡的裝置開選單時不會去載它。
        try {
            return QxrNative.isAvailable();
        } catch (Throwable error) {
            // §VITURE：程式庫初始化失敗不能往外丟，否則整份長按選單都開不起來。
            VitureUsb.logW("glasses menu native check failed", error);
            return false;
        }
    }
}
