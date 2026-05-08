package com.limelight.binding.video;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.graphics.Bitmap;
import android.opengl.GLES30;
import android.opengl.GLES31;
import android.os.Build;
import android.os.HandlerThread;
import android.util.Log;

import java.io.BufferedOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * VipleStream §B-DUMP-ANDROID: Android port of the PC §B-DUMP frame
 * comparison mechanism (vkfruc.cpp). Captures real (decoder output) +
 * interp (warp output) + MV field at the end of FRUC pipeline cycles
 * to disk for cross-platform quality analysis with the same Python
 * tools (scripts/benchmark/verify_dump_interp.py, analyze_fruc_compare.py).
 *
 * <p>Hook into {@link FrucRenderer#onFrameAvailable()}:
 * <ul>
 *   <li>Init at end of {@code initialize()} (after width/height stamped).</li>
 *   <li>Kick real-frame readback after OES→RGBA blit.</li>
 *   <li>Kick MV readback after {@code runMvMedian()}.</li>
 *   <li>Kick interp readback after {@code runWarp()}, before blit.</li>
 *   <li>Call {@link #commitCycleAndDrainReady} at cycle end (return true).</li>
 *   <li>Call {@link #commitFirstCycleReal} at end of frameCount==1 path.</li>
 *   <li>Call {@link #shutdown()} from {@link FrucRenderer#destroy()}.</li>
 * </ul>
 *
 * <p>Trigger via system property (matches the
 * {@code debug.viplestream.vkprobe} pattern in {@code VkBackend.isOptedIn}):
 * <pre>
 *   adb shell setprop debug.viplestream.frucdump.dir &lt;path&gt;
 *   adb shell setprop debug.viplestream.frucdump.frames 10
 *   adb shell setprop debug.viplestream.frucdump.delay_ms 10000
 * </pre>
 *
 * <p>Output layout (matches PC flat-dir naming):
 * <pre>
 *   &lt;dir&gt;/fruc_dump_&lt;yyyyMMdd_HHmmss&gt;/
 *     frame_0000_real.png        ← cycle 0 (frame 1 first-cycle real)
 *     frame_0001_interp.png      ← cycle 1 interp(real_0, real_1)
 *     frame_0002_real.png        ← cycle 1 real (= curr at chain time)
 *     frame_0003_interp.png      ← cycle 2 …
 *     frame_0004_real.png
 *     mv_frame_0001.bin          ← MV used to produce frame_0001_interp
 *     mv_frame_0003.bin
 *     manifest.json
 * </pre>
 *
 * <p>MV format: byte-for-byte compatible with PC {@code mv_frame_NNNN.bin}.
 * Each block writes 2 × int32 LE (mvX, mvY) in Q1 units (1 LSB = 0.5 px).
 * Source on Android is a packed R32I texture {@code (x<<16) | (y & 0xFFFF)};
 * the writer thread unpacks before writing.
 *
 * <p>PNG (vs PC BMP): chosen for native Android encode + ~4× smaller files.
 * The PC analysis scripts have been updated to accept both extensions.
 */
final class FrucDumpWriter {
    private static final String TAG = "FrucDump";

    // Slot count = 3, mirrors PC kFrucFramesInFlight. Allows up to 3
    // in-flight GPU readbacks before back-pressure kicks in.
    private static final int RING = 3;

    // Writer queue cap = 12 (PC kDumpQueueCap). On overflow we drop the
    // entire cycle atomically (matches PC's commit-or-drop semantics).
    private static final int QUEUE_CAP = 12;

    // ---- Property keys ----
    private static final String PROP_DIR = "debug.viplestream.frucdump.dir";
    private static final String PROP_FRAMES = "debug.viplestream.frucdump.frames";
    private static final String PROP_DELAY = "debug.viplestream.frucdump.delay_ms";

    private final Context context;
    private final int width, height;
    private final int mvW, mvH;
    private final int qualityLevel;
    private final long vsyncPeriodNs;

    private final File dumpDir;
    private final int framesTotal;
    private final long delayMs;
    private final long startedAtMs;

    // ---- GL resources (allocated on GL thread in initGlResources) ----
    private boolean glReady = false;
    private final int[] pboReal = new int[RING];     // RGBA8: width*height*4
    private final int[] pboInterp = new int[RING];   // RGBA8
    private final int[] pboMv = new int[RING];       // R32I:  mvW*mvH*4
    private final long[] slotFence = new long[RING]; // 0 = free
    private final boolean[] slotHasInterp = new boolean[RING];
    private final boolean[] slotHasMv = new boolean[RING];
    private final int[] slotCycleIdx = new int[RING];  // matches displayCounter at commit
    private int dumpReadFbo = 0;
    private int writeSlot = 0;  // next slot to fill
    private int readSlot = 0;   // next slot to drain

    // ---- Counters ----
    // displayCounter = next file index. After first cycle: 1 (real_0 written).
    // After cycle N (N>=1): 2N+1 (cycle wrote real_N and interp_(N-1,N)).
    private int displayCounter = 0;
    private int framesQueued = 0;        // commits that actually landed on disk
    private int commitsDispatched = 0;   // commit attempts (queued or dropped)
    private int droppedCycles = 0;       // commit attempts that hit a full queue
    private boolean doneLogged = false;
    private boolean delayReportedReady = false;
    // Hard cap on commit attempts. PNG encode is the bottleneck on Pixel 5
    // (~50 ms / 1080p frame), so at 60+ fps cycle rate the writer queue
    // saturates fast. Without this cap, shouldCapture would keep returning
    // true and produce hundreds of "queue full" drops per session.
    private static final int MAX_DISPATCH_MULTIPLIER = 3;

    // ---- Writer thread ----
    private HandlerThread writerThread;
    private final LinkedBlockingQueue<DumpJob> writerQueue =
            new LinkedBlockingQueue<>(QUEUE_CAP);
    private volatile boolean writerStop = false;

    private FrucDumpWriter(Context ctx, int w, int h, int mvW, int mvH,
                           int qualityLevel, long vsyncPeriodNs,
                           File dir, int framesTotal, long delayMs) {
        this.context = ctx;
        this.width = w;
        this.height = h;
        this.mvW = mvW;
        this.mvH = mvH;
        this.qualityLevel = qualityLevel;
        this.vsyncPeriodNs = vsyncPeriodNs;
        this.dumpDir = dir;
        this.framesTotal = framesTotal;
        this.delayMs = delayMs;
        this.startedAtMs = System.currentTimeMillis();
    }

    /**
     * Factory: returns a configured writer iff the dump-dir system
     * property is set, else null. The caller short-circuits all
     * dump-related work on null.
     *
     * <p>Reads {@code debug.viplestream.frucdump.dir} via reflection to
     * avoid pulling {@code android.os.SystemProperties} as a hidden-API
     * dep — same shape as {@link VkBackend#isOptedIn}.
     */
    static FrucDumpWriter tryCreate(Context ctx, int w, int h, int mvW, int mvH,
                                    int qualityLevel, long vsyncPeriodNs) {
        String baseDir = sysGet(PROP_DIR, "");
        if (baseDir.isEmpty()) return null;
        int frames = parseIntOr(sysGet(PROP_FRAMES, "10"), 10);
        long delay = parseLongOr(sysGet(PROP_DELAY, "10000"), 10000L);

        // Per-session subdir so back-to-back captures don't overwrite.
        String stamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US)
                .format(new Date());
        File dir = new File(baseDir, "fruc_dump_" + stamp);
        if (!dir.mkdirs() && !dir.isDirectory()) {
            Log.w(TAG, "tryCreate: failed to create " + dir.getAbsolutePath());
            return null;
        }
        Log.i(TAG, "tryCreate: enabled, dir=" + dir.getAbsolutePath()
                + " frames=" + frames + " delay_ms=" + delay
                + " (" + w + "x" + h + ", mv=" + mvW + "x" + mvH + ")");
        return new FrucDumpWriter(ctx, w, h, mvW, mvH, qualityLevel,
                                  vsyncPeriodNs, dir, frames, delay);
    }

    /** GL-thread: alloc PBOs + FBO + start writer thread. */
    void initGlResources() {
        // 9 PBOs total (3 slots × 3 kinds).
        GLES31.glGenBuffers(RING, pboReal, 0);
        GLES31.glGenBuffers(RING, pboInterp, 0);
        GLES31.glGenBuffers(RING, pboMv, 0);
        int rgbaBytes = width * height * 4;
        int mvBytes = mvW * mvH * 4;
        for (int s = 0; s < RING; s++) {
            initPbo(pboReal[s], rgbaBytes);
            initPbo(pboInterp[s], rgbaBytes);
            initPbo(pboMv[s], mvBytes);
        }
        int[] fbo = new int[1];
        GLES31.glGenFramebuffers(1, fbo, 0);
        dumpReadFbo = fbo[0];
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, 0);

        writeManifest();

        writerThread = new HandlerThread("FrucDumpWriter");
        writerThread.start();
        Thread t = new Thread(this::writerLoop, "FrucDumpWriterDrain");
        t.setDaemon(true);
        t.start();

        glReady = true;
        Log.i(TAG, "initGlResources: ring=" + RING + " rgba_bytes_per_slot="
                + rgbaBytes + " mv_bytes_per_slot=" + mvBytes);
    }

    private void initPbo(int pbo, int size) {
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, pbo);
        GLES31.glBufferData(GLES31.GL_PIXEL_PACK_BUFFER, size, null,
                            GLES30.GL_STREAM_READ);
    }

    /**
     * Should this cycle be captured? Caller checks this at the
     * top of each fully-rendered cycle (after the late-skip path is
     * confirmed taken or not). Burns no CPU once framesTotal hit.
     *
     * <p>Two stop conditions:
     * <ol>
     *   <li>{@code framesQueued >= framesTotal}: the happy path. We've
     *       successfully landed enough frames on disk.</li>
     *   <li>{@code commitsDispatched >= framesTotal * MAX_DISPATCH_MULTIPLIER}:
     *       writer can't keep up — give up rather than spam the log
     *       with "queue full" drops indefinitely.</li>
     * </ol>
     */
    boolean shouldCapture() {
        if (!glReady) return false;
        if (framesQueued >= framesTotal) return false;
        if (commitsDispatched >= framesTotal * MAX_DISPATCH_MULTIPLIER) {
            return false;
        }
        long elapsed = System.currentTimeMillis() - startedAtMs;
        if (elapsed < delayMs) return false;
        if (!delayReportedReady) {
            delayReportedReady = true;
            Log.i(TAG, "warmup elapsed (" + elapsed + "ms >= " + delayMs
                    + "ms) — capture armed");
        }
        // Need a free write-slot, i.e. a slot whose last fence was
        // either never set or has since been fully drained. We treat
        // writeSlot wrapping back to readSlot with both pending as
        // queue-full and skip this cycle.
        if (slotFence[writeSlot] != 0L) {
            // Try to drain so we can free space.
            tryDrainOne();
        }
        return slotFence[writeSlot] == 0L;
    }

    /** GL-thread: queue a glReadPixels of {@code srcTex} into the real PBO of the current write slot. */
    void kickReadbackReal(int srcTex) {
        kickRgba(srcTex, pboReal[writeSlot]);
    }

    /** GL-thread: queue a glReadPixels of {@code srcTex} into the interp PBO. */
    void kickReadbackInterp(int srcTex) {
        kickRgba(srcTex, pboInterp[writeSlot]);
        slotHasInterp[writeSlot] = true;
    }

    /** GL-thread: queue an R32I readback of the MV texture into the mv PBO. */
    void kickReadbackMv(int srcTex) {
        GLES31.glBindFramebuffer(GLES31.GL_READ_FRAMEBUFFER, dumpReadFbo);
        GLES31.glFramebufferTexture2D(GLES31.GL_READ_FRAMEBUFFER,
                GLES31.GL_COLOR_ATTACHMENT0, GLES31.GL_TEXTURE_2D, srcTex, 0);
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, pboMv[writeSlot]);
        GLES31.glReadPixels(0, 0, mvW, mvH,
                GLES30.GL_RED_INTEGER, GLES31.GL_INT, 0);
        int err = GLES31.glGetError();
        if (err != GLES31.GL_NO_ERROR) {
            // Some Mali drivers reject GL_RED_INTEGER+GL_INT to PBO. We
            // graceful-degrade by skipping MV (PNG dumps still work).
            Log.w(TAG, "MV readback rejected (gl_err=0x" + Integer.toHexString(err)
                    + ") — disabling MV dump for remainder of session");
            slotHasMv[writeSlot] = false;
        } else {
            slotHasMv[writeSlot] = true;
        }
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, 0);
        GLES31.glBindFramebuffer(GLES31.GL_READ_FRAMEBUFFER, 0);
    }

    private void kickRgba(int srcTex, int pbo) {
        GLES31.glBindFramebuffer(GLES31.GL_READ_FRAMEBUFFER, dumpReadFbo);
        GLES31.glFramebufferTexture2D(GLES31.GL_READ_FRAMEBUFFER,
                GLES31.GL_COLOR_ATTACHMENT0, GLES31.GL_TEXTURE_2D, srcTex, 0);
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, pbo);
        GLES31.glReadPixels(0, 0, width, height,
                GLES31.GL_RGBA, GLES31.GL_UNSIGNED_BYTE, 0);
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, 0);
        GLES31.glBindFramebuffer(GLES31.GL_READ_FRAMEBUFFER, 0);
    }

    /**
     * Cycle 0 only — frameCount==1, no warp ran. We dumped only a real
     * frame; commit it as file 0 and advance counter to 1.
     */
    void commitFirstCycleReal() {
        slotHasInterp[writeSlot] = false;
        slotHasMv[writeSlot] = false;
        commitWriteSlot();
    }

    /** Normal cycle: real + interp + MV all kicked. Commit them as a unit. */
    void commitCycleAndDrainReady() {
        commitWriteSlot();
        // Best-effort: drain any slots that have signaled.
        for (int i = 0; i < RING; i++) tryDrainOne();
    }

    private void commitWriteSlot() {
        slotCycleIdx[writeSlot] = displayCounter;
        slotFence[writeSlot] = GLES30.glFenceSync(
                GLES30.GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        // Counter advance — must match PC layout convention exactly:
        //   - First-capture cycle (entry counter == 0):  +1 (file 0 = real_0)
        //   - Normal cycle (interp + real):              +2 (interp@2N-1, real@2N)
        //   - Real-only non-first (defensive):           +1
        //
        // The first-capture cycle CAN have hasInterp=true if warmup ended
        // mid-stream (drainer will still emit only real_0 because the
        // firstCycle branch detects idx==0). Counter must still advance
        // by 1 there so file 1 is reserved for the next cycle's interp.
        if (displayCounter == 0) {
            displayCounter = 1;
        } else if (slotHasInterp[writeSlot]) {
            displayCounter += 2;
        } else {
            displayCounter += 1;
        }
        commitsDispatched++;
        writeSlot = (writeSlot + 1) % RING;
    }

    private void tryDrainOne() {
        if (slotFence[readSlot] == 0L) return;
        int status = GLES30.glClientWaitSync(slotFence[readSlot], 0, 0L);
        if (status != GLES30.GL_ALREADY_SIGNALED
                && status != GLES30.GL_CONDITION_SATISFIED) {
            return;
        }
        // GPU done — map and copy out, then enqueue.
        int idx = slotCycleIdx[readSlot];
        boolean firstCycle = (idx == 0);
        boolean hasInterp = slotHasInterp[readSlot];
        boolean hasMv = slotHasMv[readSlot];

        // Real always present.
        byte[] realRgba = mapPboToBytes(pboReal[readSlot], width * height * 4);
        byte[] interpRgba = null;
        int[] mvPacked = null;
        if (hasInterp) {
            interpRgba = mapPboToBytes(pboInterp[readSlot], width * height * 4);
        }
        if (hasMv) {
            mvPacked = mapPboToInts(pboMv[readSlot], mvW * mvH);
        }

        // Build jobs and atomically commit all-or-nothing.
        //
        // PC layout convention (vkfruc.cpp L1888-1917, also documented in
        // analyze_fruc_compare.py): file 2N = real_N, file 2N+1 = interp.
        // Entry counter values:
        //   First cycle:  entry=0, write real_0 at file 0,    counter→1
        //   Cycle N≥1:    entry=2N-1 (odd), write interp at  file 2N-1,
        //                                   write real_N at  file 2N,
        //                                   counter += 2
        DumpJob[] jobs;
        if (firstCycle) {
            jobs = new DumpJob[] {
                DumpJob.png(filePathReal(idx), width, height, realRgba)
            };
        } else if (hasInterp) {
            int interpIdx = idx;        // odd: 1, 3, 5, …
            int realIdx   = idx + 1;    // even: 2, 4, 6, …
            DumpJob.Builder b = new DumpJob.Builder();
            b.add(DumpJob.png(filePathInterp(interpIdx), width, height, interpRgba));
            b.add(DumpJob.png(filePathReal(realIdx), width, height, realRgba));
            if (hasMv) {
                b.add(DumpJob.mvBin(filePathMv(interpIdx), mvW, mvH, mvPacked));
            }
            jobs = b.build();
        } else {
            // Should never happen: late-skip path doesn't commit. If we
            // do see it, log + emit real to keep file 0 invariant intact.
            Log.w(TAG, "drain: non-first cycle with no interp at idx=" + idx);
            jobs = new DumpJob[] {
                DumpJob.png(filePathReal(idx), width, height, realRgba)
            };
        }

        // Atomic commit-or-drop to writer queue.
        if (writerQueue.size() + jobs.length > QUEUE_CAP) {
            // Rate-limit: log first, last, and every 10th drop. With the
            // dispatch cap (framesTotal × 3) the absolute count is bounded
            // anyway, but spelling each one out adds noise to the trace.
            droppedCycles++;
            if (droppedCycles == 1 || droppedCycles % 10 == 0) {
                Log.w(TAG, "writer queue full (" + writerQueue.size() + "/"
                        + QUEUE_CAP + "), dropping cycle idx=" + idx
                        + " (total drops=" + droppedCycles + ")");
            }
        } else {
            for (DumpJob j : jobs) writerQueue.offer(j);
            framesQueued++;
            if (framesQueued >= framesTotal && !doneLogged) {
                doneLogged = true;
                Log.i(TAG, "capture complete — " + framesQueued
                        + " server frames queued (writer thread continues "
                        + "to drain queue in background)");
            }
        }

        // Free slot.
        GLES30.glDeleteSync(slotFence[readSlot]);
        slotFence[readSlot] = 0L;
        slotHasInterp[readSlot] = false;
        slotHasMv[readSlot] = false;
        readSlot = (readSlot + 1) % RING;
    }

    private byte[] mapPboToBytes(int pbo, int size) {
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, pbo);
        ByteBuffer buf = (ByteBuffer) GLES30.glMapBufferRange(
                GLES31.GL_PIXEL_PACK_BUFFER, 0, size, GLES30.GL_MAP_READ_BIT);
        byte[] out;
        if (buf == null) {
            Log.w(TAG, "glMapBufferRange returned null — skipping slot");
            out = new byte[size];
        } else {
            out = new byte[size];
            buf.position(0);
            buf.get(out);
            GLES30.glUnmapBuffer(GLES31.GL_PIXEL_PACK_BUFFER);
        }
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, 0);
        return out;
    }

    private int[] mapPboToInts(int pbo, int count) {
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, pbo);
        ByteBuffer buf = (ByteBuffer) GLES30.glMapBufferRange(
                GLES31.GL_PIXEL_PACK_BUFFER, 0, count * 4, GLES30.GL_MAP_READ_BIT);
        int[] out = new int[count];
        if (buf == null) {
            Log.w(TAG, "glMapBufferRange (mv) returned null");
        } else {
            buf.order(ByteOrder.nativeOrder());
            IntBuffer ib = buf.asIntBuffer();
            ib.get(out);
            GLES30.glUnmapBuffer(GLES31.GL_PIXEL_PACK_BUFFER);
        }
        GLES31.glBindBuffer(GLES31.GL_PIXEL_PACK_BUFFER, 0);
        return out;
    }

    private String filePathReal(int idx) {
        return new File(dumpDir, String.format(Locale.US,
                "frame_%04d_real.png", idx)).getAbsolutePath();
    }

    private String filePathInterp(int idx) {
        return new File(dumpDir, String.format(Locale.US,
                "frame_%04d_interp.png", idx)).getAbsolutePath();
    }

    private String filePathMv(int idx) {
        return new File(dumpDir, String.format(Locale.US,
                "mv_frame_%04d.bin", idx)).getAbsolutePath();
    }

    /**
     * GL-thread: signal shutdown. Stops writer thread cleanly. Caller
     * MUST be on GL thread for the GL resource teardown.
     */
    void shutdown() {
        if (!glReady) return;
        glReady = false;
        // Drain any signaled slots one last time.
        for (int i = 0; i < RING; i++) tryDrainOne();
        // Free remaining fences (slots that didn't drain).
        for (int s = 0; s < RING; s++) {
            if (slotFence[s] != 0L) {
                GLES30.glDeleteSync(slotFence[s]);
                slotFence[s] = 0L;
            }
        }
        if (dumpReadFbo != 0) {
            GLES31.glDeleteFramebuffers(1, new int[]{dumpReadFbo}, 0);
            dumpReadFbo = 0;
        }
        GLES31.glDeleteBuffers(RING, pboReal, 0);
        GLES31.glDeleteBuffers(RING, pboInterp, 0);
        GLES31.glDeleteBuffers(RING, pboMv, 0);

        writerStop = true;
        // Sentinel to wake the writer.
        writerQueue.offer(DumpJob.sentinel());
        if (writerThread != null) {
            writerThread.quitSafely();
        }
        Log.i(TAG, "shutdown: framesQueued=" + framesQueued + "/" + framesTotal
                + " dispatched=" + commitsDispatched + " dropped=" + droppedCycles
                + " displayCounter=" + displayCounter);
    }

    String describe() {
        return dumpDir.getAbsolutePath() + " (frames=" + framesTotal
                + " delay_ms=" + delayMs + ")";
    }

    // ---- Writer loop (background thread) ----

    private void writerLoop() {
        while (!writerStop || !writerQueue.isEmpty()) {
            DumpJob job;
            try {
                job = writerQueue.poll(100, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                continue;
            }
            if (job == null || job.path == null) continue;  // sentinel
            try {
                if (job.isMv) {
                    writeMvBin(job.path, job.mvPacked);
                } else {
                    writePng(job.path, job.w, job.h, job.rgba);
                }
            } catch (Throwable t) {
                Log.w(TAG, "writer failed for " + job.path + ": " + t);
            }
        }
        Log.i(TAG, "writer loop exited (queueRemaining=" + writerQueue.size() + ")");
    }

    private static void writePng(String path, int w, int h, byte[] rgba) {
        Bitmap bm = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        // glReadPixels returned RGBA. Bitmap ARGB_8888 in memory is
        // little-endian RGBA8888 on Android (native order matches), so
        // copyPixelsFromBuffer with the raw byte buffer works.
        ByteBuffer bb = ByteBuffer.wrap(rgba);
        bb.order(ByteOrder.nativeOrder());
        bm.copyPixelsFromBuffer(bb);
        try (FileOutputStream fos = new FileOutputStream(path);
             BufferedOutputStream bos = new BufferedOutputStream(fos)) {
            bm.compress(Bitmap.CompressFormat.PNG, 100, bos);
        } catch (Exception e) {
            Log.w(TAG, "PNG write failed: " + path + " err=" + e);
        } finally {
            bm.recycle();
        }
    }

    /**
     * Writes int32 pairs (mvX, mvY) little-endian, one pair per block.
     * Source on Android is packed R32I {@code (x<<16) | (y & 0xFFFF)};
     * unpack here into the same on-disk format PC produces (PC writes
     * 2 × int32 per block via fwrite).
     */
    private static void writeMvBin(String path, int[] packed) {
        try (FileOutputStream fos = new FileOutputStream(path);
             BufferedOutputStream bos = new BufferedOutputStream(fos);
             DataOutputStream dos = new DataOutputStream(bos)) {
            byte[] tmp = new byte[8];
            ByteBuffer bb = ByteBuffer.wrap(tmp).order(ByteOrder.LITTLE_ENDIAN);
            for (int p : packed) {
                int x = p >> 16;                   // sign-extend high
                int y = (p << 16) >> 16;           // sign-extend low
                bb.clear();
                bb.putInt(x);
                bb.putInt(y);
                dos.write(tmp);
            }
        } catch (Exception e) {
            Log.w(TAG, "MV write failed: " + path + " err=" + e);
        }
    }

    private void writeManifest() {
        File mf = new File(dumpDir, "manifest.json");
        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
        kvStr(sb, "schema_version", "1");
        kvStr(sb, "platform", "\"android\"");
        kvStr(sb, "app_version_name", "\"" + appVersionName() + "\"");
        kvStr(sb, "app_version_code", String.valueOf(appVersionCode()));
        kvStr(sb, "device_model", "\"" + jsonEsc(Build.MODEL) + "\"");
        kvStr(sb, "device_brand", "\"" + jsonEsc(Build.BRAND) + "\"");
        kvStr(sb, "android_sdk", String.valueOf(Build.VERSION.SDK_INT));
        kvStr(sb, "gl_renderer", "\"" + jsonEsc(safeGlString(GLES31.GL_RENDERER)) + "\"");
        kvStr(sb, "gl_version", "\"" + jsonEsc(safeGlString(GLES31.GL_VERSION)) + "\"");
        kvStr(sb, "stream_width", String.valueOf(width));
        kvStr(sb, "stream_height", String.valueOf(height));
        kvStr(sb, "mv_width", String.valueOf(mvW));
        kvStr(sb, "mv_height", String.valueOf(mvH));
        kvStr(sb, "block_size", "64");
        kvStr(sb, "quality_preset", String.valueOf(qualityLevel));
        kvStr(sb, "vsync_period_ns", String.valueOf(vsyncPeriodNs));
        kvStr(sb, "frames_requested", String.valueOf(framesTotal));
        kvStr(sb, "delay_ms", String.valueOf(delayMs));
        kvStr(sb, "started_at_unix_ms", String.valueOf(startedAtMs));
        SimpleDateFormat iso = new SimpleDateFormat(
                "yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US);
        iso.setTimeZone(TimeZone.getTimeZone("UTC"));
        kvStr(sb, "started_at_iso", "\"" + iso.format(new Date(startedAtMs)) + "\"");
        kvStr(sb, "image_format", "\"png\"");
        kvStr(sb, "mv_format", "\"int32_pair_le\"");
        kvStr(sb, "mv_unit", "\"Q1\"");
        // Last entry — no trailing comma.
        sb.append("  \"index_convention\": \"file_2N=real_N, file_2N+1=interp(real_N, real_N+1)\"\n");
        sb.append("}\n");
        try (FileOutputStream fos = new FileOutputStream(mf);
             OutputStreamWriter w = new OutputStreamWriter(fos, "UTF-8")) {
            w.write(sb.toString());
        } catch (Exception e) {
            Log.w(TAG, "manifest write failed: " + e);
        }
    }

    private static void kvStr(StringBuilder sb, String k, String v) {
        sb.append("  \"").append(k).append("\": ").append(v).append(",\n");
    }

    private static String jsonEsc(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    private static String safeGlString(int name) {
        try {
            String s = GLES31.glGetString(name);
            return s == null ? "" : s;
        } catch (Throwable t) {
            return "";
        }
    }

    private String appVersionName() {
        try {
            PackageInfo pi = context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0);
            return pi.versionName == null ? "" : pi.versionName;
        } catch (Throwable t) {
            return "";
        }
    }

    private long appVersionCode() {
        try {
            PackageInfo pi = context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0);
            if (Build.VERSION.SDK_INT >= 28) return pi.getLongVersionCode();
            return pi.versionCode;
        } catch (Throwable t) {
            return 0L;
        }
    }

    // ---- Property reflection helpers (mirror VkBackend.isOptedIn) ----

    private static String sysGet(String key, String defVal) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            Method get = sp.getMethod("get", String.class, String.class);
            Object v = get.invoke(null, key, defVal);
            return v == null ? defVal : v.toString();
        } catch (Throwable t) {
            return defVal;
        }
    }

    private static int parseIntOr(String s, int defVal) {
        try { return Integer.parseInt(s.trim()); } catch (Throwable t) { return defVal; }
    }

    private static long parseLongOr(String s, long defVal) {
        try { return Long.parseLong(s.trim()); } catch (Throwable t) { return defVal; }
    }

    // ---- Job record ----

    private static final class DumpJob {
        final String path;
        final int w, h;
        final byte[] rgba;
        final int[] mvPacked;
        final boolean isMv;

        private DumpJob(String path, int w, int h, byte[] rgba, int[] mv, boolean isMv) {
            this.path = path; this.w = w; this.h = h;
            this.rgba = rgba; this.mvPacked = mv; this.isMv = isMv;
        }
        static DumpJob png(String path, int w, int h, byte[] rgba) {
            return new DumpJob(path, w, h, rgba, null, false);
        }
        static DumpJob mvBin(String path, int w, int h, int[] packed) {
            return new DumpJob(path, w, h, null, packed, true);
        }
        static DumpJob sentinel() {
            return new DumpJob(null, 0, 0, null, null, false);
        }

        static final class Builder {
            private final DumpJob[] arr = new DumpJob[3];
            private int n = 0;
            void add(DumpJob j) { arr[n++] = j; }
            DumpJob[] build() {
                DumpJob[] out = new DumpJob[n];
                System.arraycopy(arr, 0, out, 0, n);
                return out;
            }
        }
    }
}
