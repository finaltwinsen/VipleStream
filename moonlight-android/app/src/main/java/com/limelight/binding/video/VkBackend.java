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

    private final Context context;
    private boolean initialized = false;
    private int qualityLevel = 1;

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
     * B.1 contract: validate Vulkan availability inside the app process,
     * then bow out so GLES takes over. Returns {@code null} unconditionally
     * — the MediaCodec init wiring treats null as "this backend declined".
     */
    @Override
    public Surface initialize(Surface displaySurface, int w, int h) {
        Log.i(TAG, "B.1 init: probing Vulkan in app process for " + w + "x" + h);

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

        // B.1 stops here intentionally. Phase B.2 will:
        //   * create AImageReader + AHardwareBuffer plumbing on the
        //     decoder output side,
        //   * import as VkImage via VK_ANDROID_external_memory_*,
        //   * create VkSwapchainKHR on the displaySurface,
        //   * acquire/blit/present per frame.
        Log.i(TAG, "B.1 skeleton — Vulkan available, real path not yet wired. "
                 + "Returning null so GLES fallback engages.");
        return null;
    }

    @Override
    public boolean onFrameAvailable() {
        // B.1 never reaches the active state.
        return false;
    }

    @Override
    public void destroy() {
        initialized = false;
    }

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
            return "1".equals(v) || "true".equalsIgnoreCase(v);
        } catch (Throwable t) {
            return false;
        }
    }
}
