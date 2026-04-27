package com.limelight.binding.video;

import android.os.Build;
import android.util.Log;

/**
 * VipleStream Phase A recon — verifies that the Vulkan extensions required
 * for the planned GLES → Vulkan FRUC migration (see docs/TODO.md §I) are
 * actually present on the running device.
 *
 * Logs everything to logcat under the {@code [VIPLE-VK-PROBE]} tag; the
 * native side enumerates {@code VK_GOOGLE_display_timing} (optional but
 * critical for our latency story) and
 * {@code VK_ANDROID_external_memory_android_hardware_buffer} (zero-copy
 * MediaCodec import). Throwaway-style — when Phase B starts, this code
 * either evolves into the real Vulkan helper or gets removed entirely.
 */
public final class VkProbe {
    private static final String TAG = "VIPLE-VK-PROBE";

    static {
        try {
            System.loadLibrary("moonlight-core");
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "moonlight-core not loadable from VkProbe: " + e.getMessage());
        }
    }

    private VkProbe() {}

    /**
     * Run the probe synchronously. Logs to logcat. Returns true iff the device
     * exposes both {@code VK_GOOGLE_display_timing} and
     * {@code VK_ANDROID_external_memory_android_hardware_buffer} on at least
     * one physical device. Safe to call on any API level: returns false fast
     * on pre-API-24 (Vulkan unavailable).
     */
    public static boolean run() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
            Log.i(TAG, "API < 24, Vulkan loader not present — skipping probe");
            return false;
        }
        try {
            boolean ok = runProbeNative();
            Log.i(TAG, "probe overall verdict: " + (ok ? "BOTH required extensions present"
                                                       : "at least one required extension missing"));
            return ok;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "native probe entry not linked: " + e.getMessage());
            return false;
        } catch (Throwable t) {
            Log.e(TAG, "probe threw: " + t, t);
            return false;
        }
    }

    private static native boolean runProbeNative();
}
