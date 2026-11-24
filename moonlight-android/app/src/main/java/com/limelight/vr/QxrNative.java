package com.limelight.vr;

import android.app.Activity;
import android.view.Surface;

import com.limelight.LimeLog;

/**
 * §QUEST-VR：libviple-openxr.so 的 JNI 介面。方法名稱已定案。
 * 簽名除了 nativePcvrStart 多了打包寬高，不能改。
 */
public final class QxrNative {
    // §QUEST-VR：native 在自己的執行緒呼叫。實作端要自己回主執行緒，這裡不切。
    public interface SessionListener {
        void onSessionEnded(int reason);

        // host 的 STATE ERROR。在 message sink 執行緒呼叫，實作端要自己回主執行緒，不可同步拆 session。
        void onVrServerError(int code);
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

    // §QUEST-PCVR：nativeCreate 之後、nativeStart 之前。沒呼叫時維持原本的 2D 浮空螢幕。
    public static native boolean nativeEnablePcvr(long handle);

    // §QUEST-PCVR：還沒量到兩眼視角時回 null。長度固定 33。0..26 的索引不變，27 起是尾端追加：
    // [0] eyeW、[1] eyeH（建議的單眼像素）、[2] refreshHz、
    // [3..6] 左眼 angleLeft / angleRight / angleUp / angleDown（弧度）、
    // [7..9] 左眼相對頭部的位置 xyz（公尺）、[10..13] 左眼方向 xyzw、
    // [14..17] 右眼四個角（弧度）、[18..20] 右眼位置 xyz、[21..24] 右眼方向 xyzw、
    // [25] playAreaWidthM（0＝未知）、[26] playAreaDepthM（0＝未知）、
    // [27] seatedValid（0＝不要送 vrSeated）、[28..30] 坐姿零點 xyz（公尺）、[31] seatedYawRad、
    // [32] timeConversion（1＝可離開影格執行緒，0＝要清掉 CAP_TRACK_THREAD）。
    public static native float[] nativeGetViewConfig(long handle);

    // §QUEST-PCVR：連線建立且 VIPLE_VR_SF_ENABLED 之後才呼叫。refreshHz <= 0 時 native 用 72。
    // packedW、packedH 是串流雙眼並排的寬高（左眼在左半）。projection swapchain 用這一組，不用建議的單眼解析度。
    public static native boolean nativePcvrStart(long handle, int refreshHz, int packedW, int packedH);

    public static native void nativePcvrStop(long handle);

    // §QUEST-VR：不建立 OpenXR。呼叫端要先看 isAvailable()，這個 .so 沒載入時不能呼叫。
    public static native long nativeEmuCreate(int refreshHz, String motion);

    public static native void nativeEmuStart(long handle);

    public static native void nativeEmuStop(long handle);

    public static native void nativeEmuDestroy(long handle);

    // §VITURE：Start／Stop／Destroy 沿用 nativeEmu*。push 約 400 Hz，native 端不能配置記憶體。
    public static native long nativeImuCreate(int refreshHz);

    public static native void nativeImuPush(long handle, float qw, float qx, float qy, float qz,
                                            float wx, float wy, float wz, long sourceTimeNs);

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

    // §QUEST-PCVR：native 用這個名字找靜態方法。簽名 (JI)V。改名的話回呼會直接找不到。
    public static void onVrServerError(long handle, int code) {
        SessionListener target;
        synchronized (LISTENER_LOCK) {
            if (listener == null || handle != listenerHandle) {
                LimeLog.info("[VIPLE-QXR-JNI] onVrServerError has no listener handle="
                        + handle + " code=" + code);
                return;
            }
            target = listener;
        }
        target.onVrServerError(code);
    }
}
