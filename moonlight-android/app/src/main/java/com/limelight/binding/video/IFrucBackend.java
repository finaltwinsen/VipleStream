package com.limelight.binding.video;

import android.view.Surface;

/**
 * VipleStream — abstract interface for FRUC (frame interpolation) backends.
 *
 * <p>Introduced as part of the §I Vulkan migration (see docs/TODO.md):
 * the existing GLES 3.1 compute-shader implementation lives behind this
 * interface as {@link FrucRenderer} (backendName = "gles"), and Phase B+
 * grow a Vulkan-based sibling alongside. {@code MediaCodecDecoderRenderer}
 * holds a reference of this type so it can swap implementations without
 * structural change.</p>
 *
 * <p>Method contract is intentionally identical to the original
 * {@code FrucRenderer} public surface — this commit is a pure refactor,
 * no behavior change.</p>
 */
public interface IFrucBackend {

    /** Quality preset (0 = Quality, 1 = Balanced, 2 = Performance). */
    void setQualityLevel(int level);

    /** Hint that the network is currently degraded; backend may skip work
     *  for late frames so the user gets the real frame promptly. */
    void setConnectionPoor(boolean poor);

    /**
     * Initialize the backend, attach to {@code displaySurface}, and return
     * the {@link Surface} that should be handed to {@code MediaCodec} as
     * its decoder output. Returns {@code null} on init failure — caller
     * MUST treat that as "FRUC unavailable, fall back".
     */
    Surface initialize(Surface displaySurface, int w, int h);

    boolean isInitialized();

    /**
     * Called from the MediaCodec output thread after a decoded frame has
     * been released into the input Surface chain. The backend reads,
     * processes, and presents.
     *
     * @return {@code true} if an interpolated frame was actually produced
     *         and presented (purely for stats/log usage).
     */
    boolean onFrameAvailable();

    /** Release all GPU / window resources. Idempotent. */
    void destroy();

    // ---------- stats for perf overlay / fruc_log.txt ----------

    int getInterpolatedCount();
    float getOutputFps();

    /**
     * Short identifier of the backend (e.g. {@code "gles"}, {@code "vulkan"}).
     * Used in logcat tags and perf overlay so it's unambiguous which path
     * actually ran for a given streaming session.
     */
    String backendName();
}
