package com.limelight.vr;

import android.app.Activity;
import android.view.Surface;

import com.limelight.LimeLog;

/**
 * §QUEST-VR：libviple-openxr.so 的 JNI 介面。方法名稱與簽名已定案，不能改。
 */
public final class QxrNative {
    // §QUEST-VR：native 在自己的執行緒呼叫。實作端要自己回主執行緒，這裡不切。
    public interface SessionListener {
        void onSessionEnded(int reason);
    }

    private static final Object LISTENER_LOCK = new Object();
    private static long listenerHandle;
    private static SessionListener listener;

    private static final boolean available;

    static {
        boolean loaded = false;
        try {
            // §QUEST-VR：MoonBridge 可能已經載過 moonlight-core，再載一次是安全的。
            // 必須先於 viple-openxr，後者才找得到它匯出的符號。
            System.loadLibrary("moonlight-core");
            System.loadLibrary("viple-openxr");
            loaded = true;
        } catch (UnsatisfiedLinkError error) {
            // §QUEST-VR：非 arm64 的套件沒有 libviple-openxr.so。不能讓靜態初始化把整個 App 打掛。
            LimeLog.warning("[VIPLE-QXR-JNI] library load failed: " + error);
        }
        available = loaded;
    }

    private QxrNative() {
    }

    public static boolean isAvailable() {
        return available;
    }

    public static native long nativeCreate(Activity activity, int streamWidth, int streamHeight, int preferredHz);

    public static native String nativeLastError();

    public static native Surface nativeGetDecoderSurface(long handle);

    public static native boolean nativeStart(long handle);

    public static native void nativeStop(long handle);

    public static native void nativeDestroy(long handle);

    // §QUEST-VR：介面已定案。這一階段還沒用，先留著給更新率與單眼解析度。
    public static native float[] nativeGetRefreshRates(long handle);

    public static native int[] nativeGetRecommendedEyeSize(long handle);

    // §QUEST-VR：不建立 OpenXR。呼叫端要先看 isAvailable()，這個 .so 沒載入時不能呼叫。
    public static native long nativeEmuCreate(int refreshHz, String motion);

    public static native void nativeEmuStart(long handle);

    public static native void nativeEmuStop(long handle);

    public static native void nativeEmuDestroy(long handle);

    // §QUEST-VR：native 回呼只帶 handle，沒有 Activity。同一時間只留一筆，用 handle 對上。
    public static void setListener(long handle, SessionListener sessionListener) {
        synchronized (LISTENER_LOCK) {
            listenerHandle = handle;
            listener = sessionListener;
        }
    }

    // §QUEST-VR：native 用這個名字找靜態方法。改名的話回呼會直接找不到。
    public static void onSessionEnded(long handle, int reason) {
        SessionListener target;
        synchronized (LISTENER_LOCK) {
            if (listener == null || handle != listenerHandle) {
                LimeLog.info("[VIPLE-QXR-JNI] onSessionEnded has no listener handle="
                        + handle + " reason=" + reason);
                return;
            }
            target = listener;
        }
        target.onSessionEnded(reason);
    }
}
