package com.limelight.binding.video;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
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
    // §I.E.b — caller stashes prefs.enableHdr here BEFORE initialize() so
    // nativeInit can decide instance/device ext enable up-front. Setting
    // post-init is still legal (forwarded as a runtime flag for telemetry)
    // but cannot retroactively change the ext list.
    private boolean userEnableHdr = false;

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

    // Output FPS counter (delta-based since v1.2.184). Reads
    // nativeGetDisplayedCount() — which returns single + 2*dual — and
    // divides delta by elapsed time. Reports actual on-screen present
    // rate, not the input/decoder rate. volatile because the perf
    // overlay polls from the UI thread while we update from the
    // ImageReader handler thread.
    private long fpsWindowStartNs;
    private int  fpsLastDisplayedCount;
    private volatile float currentOutputFps;

    public VkBackend(Context context) {
        this.context = context;
    }

    @Override public String backendName() { return BACKEND; }
    @Override public boolean isInitialized() { return initialized; }
    // §I.C.4.b dual present: native side increments fInterpolatedCount once
    // per render_ahb_frame whenever the FRUC compute pipeline is up
    // (pass 1 = synthesized interp, pass 2 = real). When init is partial
    // and we fall back to single-present AHB, the counter stays at 0.
    @Override public int getInterpolatedCount() {
        return nativeHandle != 0 ? nativeGetInterpolatedCount(nativeHandle) : 0;
    }
    @Override public float getOutputFps() { return currentOutputFps; }
    @Override public void setConnectionPoor(boolean poor) {}
    @Override public void setQualityLevel(int level) {
        this.qualityLevel = Math.max(0, Math.min(2, level));
        // §I.C.5.a: push down so dispatch_fruc picks motionest_q<N> + warp_q<N>
        // matching the user's preset. nativeHandle 0 == backend not init'd
        // yet — ignore; init_compute_pipelines defaults to Q1, the new value
        // takes effect on the next setQualityLevel call after init.
        if (nativeHandle != 0) {
            nativeSetQualityLevel(nativeHandle, this.qualityLevel);
        }
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

        // Set up file-based status logger for diagnostics on devices
        // where adb isn't available (e.g. Pixel 9 without USB debugging).
        //
        // Android 14+ (Files by Google) hides /sdcard/Android/data/<pkg>/
        // — even the owning app's user can't browse it. So write to
        // public Downloads/ via MediaStore.Downloads (API 29+), which is
        // accessible without any storage permission and visible in Files
        // app's Downloads section. Native gets a raw fd and fdopen()s it.
        // Falls back to external-files-dir on API < 29.
        boolean logSetUp = false;
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
            try {
                android.content.ContentResolver cr = context.getContentResolver();
                String name = "viple_vkbe_status.log";
                // Delete any prior file with the same name so we overwrite (MediaStore
                // doesn't replace by name — leaves old entries hanging around).
                cr.delete(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI,
                          android.provider.MediaStore.MediaColumns.DISPLAY_NAME + "=?",
                          new String[]{name});
                android.content.ContentValues cv = new android.content.ContentValues();
                cv.put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, name);
                cv.put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "text/plain");
                cv.put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH,
                       android.os.Environment.DIRECTORY_DOWNLOADS);
                android.net.Uri uri = cr.insert(
                        android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv);
                if (uri != null) {
                    android.os.ParcelFileDescriptor pfd =
                            cr.openFileDescriptor(uri, "w");
                    if (pfd != null) {
                        int fd = pfd.detachFd();    // native owns it now
                        nativeSetLogFd(fd);
                        Log.i(TAG, "diagnostic log -> Downloads/" + name + " (fd=" + fd + ")");
                        logSetUp = true;
                    }
                }
            } catch (Throwable t) {
                Log.w(TAG, "MediaStore log setup failed (will fallback): " + t);
            }
        }
        if (!logSetUp) {
            // Fallback: app's external-files dir (visible only on API < 30 or
            // pre-Files-by-Google blocks). Better than nothing on old Android.
            try {
                java.io.File logDir = context.getExternalFilesDir(null);
                if (logDir != null) {
                    java.io.File logFile = new java.io.File(logDir, "viple_vkbe_status.log");
                    nativeSetLogPath(logFile.getAbsolutePath());
                    Log.i(TAG, "diagnostic log -> " + logFile.getAbsolutePath() + " (fallback)");
                }
            } catch (Throwable t) {
                Log.w(TAG, "couldn't set diagnostic log path: " + t);
            }
        }

        // §I.D.c v2 — read device's max supported refresh rate so native
        // can both hint compositor AND drive smart-mode dual-vs-single
        // decision against the real panel. Hardcoded 90 was Pixel-5-only;
        // Pixel 9 (120 Hz) needs 120 to trigger 60→120 dual present.
        float maxRefreshHz = 60.0f;
        try {
            android.view.Display d = (android.os.Build.VERSION.SDK_INT >= 30)
                    ? context.getDisplay()
                    : ((android.view.WindowManager) context.getSystemService(
                            android.content.Context.WINDOW_SERVICE)).getDefaultDisplay();
            if (d != null) {
                android.view.Display.Mode[] modes = d.getSupportedModes();
                if (modes != null) {
                    for (android.view.Display.Mode m : modes) {
                        float r = m.getRefreshRate();
                        if (r > maxRefreshHz) maxRefreshHz = r;
                    }
                }
                Log.i(TAG, "device display: current=" + d.getRefreshRate()
                        + " Hz, max-supported=" + maxRefreshHz + " Hz");
            }
        } catch (Throwable t) {
            Log.w(TAG, "display rate query failed: " + t + " (defaulting to 60 Hz)");
        }

        try {
            nativeHandle = nativeInit(displaySurface, w, h, maxRefreshHz, userEnableHdr);
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
                        // §I.B.2c.3c.3: hand the HardwareBuffer to native,
                        // which imports it as a VkImage with the YCbCr
                        // sampler view and renders the fullscreen video
                        // sample shader. Falls back to clear-render if the
                        // import or pipeline isn't ready.
                        if (nativeHandle != 0) {
                            HardwareBuffer hb = img.getHardwareBuffer();
                            int rc = nativeRenderFrame(nativeHandle, hb);
                            if (rc != 0 && imageReaderFramesSeen <= 5) {
                                Log.w(TAG, "nativeRenderFrame returned " + rc
                                           + " on frame " + imageReaderFramesSeen);
                            }
                            if (rc == 0) updateOutputFps();
                            if (hb != null) hb.close();
                        }
                        if (imageReaderFramesSeen <= 3 || imageReaderFramesSeen % 60 == 0) {
                            Log.i(TAG, "ImageReader frame #" + imageReaderFramesSeen
                                       + " size=" + img.getWidth() + "x" + img.getHeight()
                                       + " hwBuffer=" + (img.getHardwareBuffer() != null)
                                       + " outputFps=" + currentOutputFps);
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

    /**
     * Delta-based 1-second sliding window over the native displayed
     * count (single + 2*dual). Reports the actual swapchain present
     * rate so the perf overlay shows ~2× input in dual mode and ~1×
     * in single mode, matching the GLES FRUC path's semantics.
     *
     * Pre-v1.2.184 this incremented by 1 per input frame, which made
     * the overlay's "Output FPS" always equal "Input FPS" regardless
     * of how many presents the dual-mode pipeline submitted — that
     * was the source of the user-reported "1:1 even with FRUC active"
     * appearance.
     */
    private void updateOutputFps() {
        long now = System.nanoTime();
        int displayed = (nativeHandle != 0) ? nativeGetDisplayedCount(nativeHandle) : 0;
        if (fpsWindowStartNs == 0) {
            fpsWindowStartNs      = now;
            fpsLastDisplayedCount = displayed;
            return;
        }
        long elapsedNs = now - fpsWindowStartNs;
        if (elapsedNs >= 1_000_000_000L) {
            int delta = displayed - fpsLastDisplayedCount;
            if (delta < 0) delta = 0;   // shouldn't happen; counters monotonic
            currentOutputFps      = delta * 1_000_000_000f / elapsedNs;
            fpsLastDisplayedCount = displayed;
            fpsWindowStartNs      = now;
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
        fpsWindowStartNs      = 0;
        fpsLastDisplayedCount = 0;
        currentOutputFps      = 0f;
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
    private static native long nativeInit(Surface displaySurface, int videoWidth, int videoHeight,
                                          float maxRefreshHz, boolean enableHdr);
    private static native void nativeDestroy(long handle);
    private static native int  nativeRenderClearFrame(long handle);
    private static native int  nativeImportAhb(long handle, HardwareBuffer hwBuffer);
    private static native int  nativeRenderFrame(long handle, HardwareBuffer hwBuffer);
    private static native int  nativeGetInterpolatedCount(long handle);
    private static native int  nativeGetDisplayedCount(long handle);
    private static native void nativeSetQualityLevel(long handle, int level);
    private static native void nativeSetHdrEnabled(long handle, boolean enabled);
    private static native void nativeSetLogPath(String path);
    private static native void nativeSetLogFd(int fd);

    /**
     * Pass through prefs.enableHdr ("Enable HDR (Experimental)" checkbox).
     *
     * <p>Pre-{@link #initialize}: stashes the flag so {@code nativeInit}
     * can gate VK_EXT_swapchain_colorspace (instance) + VK_EXT_hdr_metadata
     * (device) — both must be decided before vkCreateInstance/vkCreateDevice
     * because Vulkan ext lists are immutable post-creation.</p>
     *
     * <p>Post-init: still forwarded to native for telemetry / future
     * pipeline-rebuild logic, but cannot retroactively change ext lists.</p>
     */
    public void setHdrEnabled(boolean enabled) {
        this.userEnableHdr = enabled;
        if (nativeHandle != 0) {
            nativeSetHdrEnabled(nativeHandle, enabled);
        }
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
            boolean b = "1".equals(v) || "true".equalsIgnoreCase(v);
            Log.i(TAG, "isOptedIn: debug.viplestream.vkprobe='" + v + "' → " + b);
            return b;
        } catch (Throwable t) {
            Log.w(TAG, "isOptedIn reflection failed: " + t);
            return false;
        }
    }

    // ---------- SIGSEGV canary (v1.2.185) ----------
    //
    // Java try/catch can recover from RuntimeException / native init
    // returning null, but it CANNOT recover from a SIGSEGV inside
    // native code (e.g. driver bugs in vkCreateInstance /
    // vkCreateDevice / vkCreateSwapchainKHR — confirmed on Pixel 9
    // v1.2.180-182 with VK_EXT_hdr_metadata, VK_EXT_swapchain_colorspace,
    // ANativeWindow_setFrameRate). When that happens, the kernel kills
    // the JVM before any catch block runs, and re-launching the app
    // hits the same code path and crashes again — the user is locked
    // out forever with no fallback.
    //
    // This canary persists "we are about to call into native Vulkan
    // init" to disk synchronously BEFORE the call. If the call SIGSEGVs,
    // the flag stays armed across process death. Next launch reads it
    // and forces GLES fallback. App upgrades (versionCode bump) auto-
    // clear the canary so a fixed build gets a fresh retry.
    private static final String CANARY_PREFS_NAME      = "vipleStream_vk_canary";
    private static final String CANARY_KEY_ARMED       = "init_in_progress";
    private static final String CANARY_KEY_VERSION     = "last_version_code";

    /**
     * @return true if the previous Vulkan FRUC init attempt SIGSEGV'd
     *         the process. When true the caller MUST skip Vulkan and
     *         fall back to GLES. Auto-clears on app upgrade.
     */
    public static boolean isCanaryActive(Context context) {
        SharedPreferences sp = context.getSharedPreferences(
                CANARY_PREFS_NAME, Context.MODE_PRIVATE);
        int curVer = readVersionCode(context);
        int lastVer = sp.getInt(CANARY_KEY_VERSION, -1);
        if (lastVer != curVer) {
            // App upgraded (or first run after canary feature added).
            // Stale canary may be from an older build whose bug is now
            // fixed — clear and retry on this fresh version.
            sp.edit()
              .putBoolean(CANARY_KEY_ARMED, false)
              .putInt(CANARY_KEY_VERSION, curVer)
              .apply();
            Log.i(TAG, "canary: version changed " + lastVer + " → " + curVer
                       + ", cleared stale canary");
            return false;
        }
        boolean armed = sp.getBoolean(CANARY_KEY_ARMED, false);
        if (armed) {
            Log.w(TAG, "canary: ARMED — previous Vulkan init likely SIGSEGV'd, "
                       + "forcing GLES fallback. Update the app to retry.");
        }
        return armed;
    }

    /** Persist "about to call native Vulkan init" synchronously. */
    public static void armCanary(Context context) {
        // commit() not apply() — must be on disk before native call,
        // otherwise a SIGSEGV before the async write completes would
        // leave the canary unarmed and we'd crash again next launch.
        context.getSharedPreferences(CANARY_PREFS_NAME, Context.MODE_PRIVATE)
               .edit()
               .putBoolean(CANARY_KEY_ARMED, true)
               .commit();
    }

    /** Native init returned (success or graceful null) — clear canary. */
    public static void disarmCanary(Context context) {
        context.getSharedPreferences(CANARY_PREFS_NAME, Context.MODE_PRIVATE)
               .edit()
               .putBoolean(CANARY_KEY_ARMED, false)
               .apply();
    }

    private static int readVersionCode(Context context) {
        try {
            PackageInfo pi = context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0);
            return pi.versionCode;
        } catch (Throwable t) {
            return -1;
        }
    }
}
