package com.limelight.binding.video;

import android.content.Context;
import android.util.Log;
import android.view.Surface;

/**
 * VipleStream §I.B.1 — Vulkan FRUC backend skeleton.
 *
 * <p>This is the wiring + fallback validator. The actual Vulkan render path
 * (swapchain, AHardwareBuffer import, compute shaders, present) is built up
 * across B.2 → C → D. For now {@link #initialize} deliberately returns
 * {@code null} so the {@code MediaCodecDecoderRenderer} fallback chain
 * promotes to the GLES backend. The job of B.1 is to prove:</p>
 * <ul>
 *   <li>Backend selection via {@code debug.viplestream.vkprobe}=1
 *       system property reaches the right instantiation site,</li>
 *   <li>Vulkan instance + extension probe runs in the app's process
 *       (not just the standalone PcView probe path),</li>
 *   <li>Fallback to GLES is automatic and crash-free when this
 *       backend declines.</li>
 * </ul>
 *
 * <p>Activate from a shell:
 * {@code adb shell setprop debug.viplestream.vkprobe 1} then start a
 * stream. Default users (property unset / 0) see no change.</p>
 */
public final class VkBackend implements IFrucBackend {
    private static final String TAG = "VKBE";
    private static final String BACKEND = "vulkan";

    static {
        try {
            System.loadLibrary("moonlight-core");
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "moonlight-core not loadable from VkBackend: " + e.getMessage());
        }
    }

    private final Context context;
    private boolean initialized = false;
    private int qualityLevel = 1;

    /**
     * Native-side handle for the {@code vk_backend_t} struct (VkInstance +
     * VkDevice + VkQueue + the function pointer table). Zero means
     * "no backend allocated"; non-zero MUST be paired with exactly one
     * {@link #nativeDestroy(long)} call before this object is GC'd.
     */
    private long nativeHandle = 0;

    public VkBackend(Context context) {
        this.context = context;
    }

    @Override public String backendName() { return BACKEND; }
    @Override public boolean isInitialized() { return initialized; }
    @Override public int getInterpolatedCount() { return 0; }
    @Override public float getOutputFps() { return 0f; }
    @Override public void setConnectionPoor(boolean poor) {}
    @Override public void setQualityLevel(int level) {
        this.qualityLevel = Math.max(0, Math.min(2, level));
    }

    /**
     * B.2a contract: build a real VkInstance + VkDevice + graphics queue
     * via {@link #nativeInit()}. Hold the handle for the backend lifetime
     * so subsequent phases (B.2b swapchain, B.2c AHB import) can attach.
     * Still returns {@code null} so the GLES fallback engages — the actual
     * presentation path is not yet wired.
     */
    @Override
    public Surface initialize(Surface displaySurface, int w, int h) {
        Log.i(TAG, "init: probing + creating Vulkan resources for " + w + "x" + h);

        boolean extsOk;
        try {
            extsOk = VkProbe.run();
        } catch (Throwable t) {
            Log.e(TAG, "VkProbe.run() threw: " + t, t);
            extsOk = false;
        }
        if (!extsOk) {
            Log.w(TAG, "Vulkan extension check failed; declining backend → fallback to GLES");
            return null;
        }

        try {
            nativeHandle = nativeInit(displaySurface);
        } catch (Throwable t) {
            Log.e(TAG, "nativeInit threw: " + t, t);
            nativeHandle = 0;
        }
        if (nativeHandle == 0) {
            Log.w(TAG, "Vulkan resource creation failed; declining backend → fallback to GLES");
            return null;
        }

        // B.2b stops here intentionally — Vulkan instance/device/surface/
        // swapchain are all alive, but we have no MediaCodec→VkImage
        // import path yet (B.2c) and no acquire/blit/present loop. Tear
        // everything down so the displaySurface is free for GLES to
        // claim via eglCreateWindowSurface.
        Log.i(TAG, "B.2b — instance/device/surface/swapchain ready, but no MediaCodec→VkImage "
                 + "import yet. Tearing down and declining → GLES fallback.");
        nativeDestroy(nativeHandle);
        nativeHandle = 0;
        return null;
    }

    @Override
    public boolean onFrameAvailable() {
        // B.1 never reaches the active state.
        return false;
    }

    @Override
    public void destroy() {
        if (nativeHandle != 0) {
            try {
                nativeDestroy(nativeHandle);
            } catch (Throwable t) {
                Log.e(TAG, "nativeDestroy threw: " + t, t);
            }
            nativeHandle = 0;
        }
        initialized = false;
    }

    // ---------- native bridge ----------
    private static native long nativeInit(Surface displaySurface);
    private static native void nativeDestroy(long handle);

    /**
     * Read {@code debug.viplestream.vkprobe} via reflection so we don't
     * pull android.os.SystemProperties as a hidden-API dep. Returns true
     * iff the property is exactly "1" or "true".
     */
    public static boolean isOptedIn() {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            java.lang.reflect.Method get = sp.getMethod("get", String.class, String.class);
            String v = (String) get.invoke(null, "debug.viplestream.vkprobe", "0");
            boolean b = "1".equals(v) || "true".equalsIgnoreCase(v);
            Log.i(TAG, "isOptedIn: debug.viplestream.vkprobe='" + v + "' → " + b);
            return b;
        } catch (Throwable t) {
            Log.w(TAG, "isOptedIn reflection failed: " + t);
            return false;
        }
    }
}
