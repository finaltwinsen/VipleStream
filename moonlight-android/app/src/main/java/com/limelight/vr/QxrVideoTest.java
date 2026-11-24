package com.limelight.vr;

import android.view.Surface;

/**
 * §QXR-VIDEO-TEST：手機上的影像路徑測試。不建立 OpenXR。
 * native 方法在 libviple-openxr.so，載入順序由 QxrNative 負責。
 */
public final class QxrVideoTest {
    // §QXR-VIDEO-TEST：類別初始化就觸發 QxrNative 的 loadLibrary。
    // 這裡再 load 一次會打亂 moonlight-core 必須先載入的順序。
    private static final boolean available = QxrNative.isAvailable();

    private QxrVideoTest() {
    }

    public static boolean isAvailable() {
        return available;
    }

    // 目標格式在建立時決定。解碼器拒換 Surface，測試不能中途改成另一種格式。
    public static native long nativeCreate(int streamW, int streamH, String outDir, boolean srgb);

    public static native Surface nativeGetSurface(long handle);

    public static native void nativeStart(long handle);

    public static native void nativeStop(long handle);

    public static native void nativeDestroy(long handle);
}
