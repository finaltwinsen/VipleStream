package com.limelight.binding.video;

import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
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

    // §I.B.2c.2: ImageReader receives MediaCodec output as AHardwareBuffer-backed
    // {@link Image} objects. Phase B.2c.3 imports each one as a VkImage via
    // VK_ANDROID_external_memory_android_hardware_buffer and blits to the
    // swapchain. Right now this scaffolding only validates the lifecycle —
    // initialize() still returns null, so MediaCodec writes to the GLES
    // SurfaceTexture path and onImageAvailable never fires.
    private ImageReader imageReader;
    private HandlerThread imageReaderThread;
    private Handler imageReaderHandler;
    private int imageReaderFramesSeen;

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

        if (!setupImageReader(w, h)) {
            Log.w(TAG, "ImageReader setup failed; declining backend → fallback to GLES");
            destroyImageReader();
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
            return null;
        }

        // §I.B.2c.3a: ACTIVE MODE. Return ImageReader's surface so MediaCodec
        // writes its decoded frames into our buffer queue. onImageAvailable
        // currently triggers a clear-colour render; B.2c.3b imports the
        // HardwareBuffer and B.2c.3c blits actual video. Visible effect for
        // an opt-in user (setprop=1): rapidly cycling colour on screen at
        // input fps, NOT the streamed game video. Default users (setprop=0)
        // are not affected.
        Surface input = imageReader.getSurface();
        if (input == null) {
            Log.w(TAG, "imageReader.getSurface() returned null; declining → GLES fallback");
            destroyImageReader();
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
            return null;
        }
        initialized = true;
        Log.i(TAG, "Active Vulkan FRUC backend (B.2c.3a clear-colour mode)");
        return input;
    }

    /**
     * §I.B.2c.2 scaffold: build the {@link ImageReader} that B.2c.3+ will
     * have MediaCodec write into. Format is PRIVATE + USAGE_GPU_SAMPLED_IMAGE
     * so the underlying buffer is an {@link HardwareBuffer} importable by
     * Vulkan via VK_ANDROID_external_memory_android_hardware_buffer.
     *
     * <p>Requires API 28+ for {@link ImageReader#newInstance(int, int, int,
     * int, long)} with the usage flag. Pixel 5 / LineageOS 22.1 is API 35,
     * so the gate is mostly defensive against minSdk 21 builds.</p>
     */
    private boolean setupImageReader(int w, int h) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            Log.w(TAG, "ImageReader (with usage) requires API 28+; current="
                       + Build.VERSION.SDK_INT);
            return false;
        }
        try {
            // 3 buffers: matches MediaCodec's typical output queue depth.
            // Larger eats memory; smaller can stall the encoder.
            imageReader = ImageReader.newInstance(
                w, h, ImageFormat.PRIVATE, /*maxImages=*/3,
                HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE);

            imageReaderThread = new HandlerThread("VipleStream-VkImageReader");
            imageReaderThread.start();
            imageReaderHandler = new Handler(imageReaderThread.getLooper());

            imageReader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
                @Override
                public void onImageAvailable(ImageReader reader) {
                    Image img = null;
                    try {
                        img = reader.acquireLatestImage();
                        if (img == null) return;
                        imageReaderFramesSeen++;
                        // B.2c.3a: native renders a per-frame animated clear
                        // colour to the swapchain image — does NOT yet read
                        // the HardwareBuffer (B.2c.3b adds AHB→VkImage import,
                        // B.2c.3c adds the YCbCr→RGB sampler so actual video
                        // shows). The visible cycling colour proves the
                        // per-frame Vulkan present loop is running at the
                        // MediaCodec frame rate.
                        if (nativeHandle != 0) {
                            nativeRenderClearFrame(nativeHandle);
                        }
                        if (imageReaderFramesSeen <= 3 || imageReaderFramesSeen % 60 == 0) {
                            Log.i(TAG, "ImageReader frame #" + imageReaderFramesSeen
                                       + " size=" + img.getWidth() + "x" + img.getHeight()
                                       + " hwBuffer=" + (img.getHardwareBuffer() != null));
                        }
                    } catch (Throwable t) {
                        Log.e(TAG, "onImageAvailable threw: " + t, t);
                    } finally {
                        if (img != null) img.close();
                    }
                }
            }, imageReaderHandler);

            Log.i(TAG, "ImageReader created: " + w + "x" + h
                       + " ImageFormat.PRIVATE maxImages=3 usage=GPU_SAMPLED_IMAGE");
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "ImageReader init threw: " + t, t);
            destroyImageReader();
            return false;
        }
    }

    private void destroyImageReader() {
        if (imageReader != null) {
            try { imageReader.setOnImageAvailableListener(null, null); } catch (Throwable ignored) {}
            try { imageReader.close(); } catch (Throwable t) {
                Log.w(TAG, "ImageReader close threw: " + t);
            }
            imageReader = null;
        }
        if (imageReaderThread != null) {
            try { imageReaderThread.quitSafely(); } catch (Throwable ignored) {}
            imageReaderThread = null;
            imageReaderHandler = null;
        }
        Log.i(TAG, "ImageReader destroyed (frames seen during this session: "
                   + imageReaderFramesSeen + ")");
        imageReaderFramesSeen = 0;
    }

    @Override
    public boolean onFrameAvailable() {
        // B.1 never reaches the active state.
        return false;
    }

    @Override
    public void destroy() {
        destroyImageReader();
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
    private static native int  nativeRenderClearFrame(long handle);

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
