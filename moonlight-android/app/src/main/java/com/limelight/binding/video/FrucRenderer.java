package com.limelight.binding.video;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.opengl.*;
import android.util.Log;
import android.view.Surface;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * VipleStream: FRUC Frame Interpolation via OpenGL ES 3.1 compute shaders.
 * Intercepts MediaCodec output via SurfaceTexture, runs motion estimation + warping,
 * and renders interpolated + real frames to the display surface.
 */
public class FrucRenderer {
    private static final String TAG = "FRUC";
    private static final int BLOCK_SIZE = 64; // Full-res block size
    private static final int DOWNSCALE = 8;

    // Quality presets: 0=Quality, 1=Balanced, 2=Performance
    public static final int QUALITY_QUALITY = 0;
    public static final int QUALITY_BALANCED = 1;
    public static final int QUALITY_PERFORMANCE = 2;
    private int qualityLevel = QUALITY_BALANCED;

    private final Context context;
    private int width, height;

    // EGL
    private EGLDisplay eglDisplay;
    private EGLContext eglContext;
    private EGLSurface eglSurface;

    // Input from MediaCodec
    private int oesTextureId;
    private SurfaceTexture surfaceTexture;
    private Surface inputSurface;

    // GL textures
    private int prevFrameTex, currFrameTex;
    private int motionFieldTex;
    private int prevMotionFieldTex;  // VipleStream v17: temporal smoothing
    private int interpFrameTex;

    // Shaders
    private int oesToRgbaProgram;
    private int[] motionEstPrograms = new int[3];  // [0]=quality [1]=balanced [2]=performance
    private int[] warpPrograms = new int[3];
    private int motionEstProgram;  // alias for active quality
    private int warpProgram;       // alias for active quality
    private int blitProgram;

    // FBO for OES→RGBA conversion
    private int conversionFbo;

    // Fullscreen quad
    private FloatBuffer quadVertices;

    // SurfaceTexture transform matrix
    private final float[] stMatrix = new float[16];
    private static final float[] IDENTITY_MATRIX = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };

    private int frameCount = 0;
    private int interpolatedCount = 0;
    private boolean initialized = false;
    private final AtomicBoolean frameAvailable = new AtomicBoolean(false);
    private final Object frameSyncLock = new Object();
    private java.io.PrintWriter logWriter;

    // FPS tracking
    private long fpsWindowStart = 0;
    private int fpsWindowPresents = 0;
    private float currentOutputFps = 0;

    // VipleStream v17: frame gap detection + connection quality
    private long lastRenderTimeMs = 0;
    private int renderFrameCount = 0;
    private volatile boolean connectionPoor = false;  // set by Game.java

    public FrucRenderer(Context context) {
        this.context = context;
        // Try writing log to Downloads (user-visible), fallback to app-private dir
        try {
            java.io.File logDir = android.os.Environment.getExternalStoragePublicDirectory(
                android.os.Environment.DIRECTORY_DOWNLOADS);
            if (logDir == null || !logDir.canWrite()) {
                logDir = context.getExternalFilesDir(null);
            }
            if (logDir != null) {
                java.io.File logFile = new java.io.File(logDir, "fruc_log.txt");
                logWriter = new java.io.PrintWriter(new java.io.FileWriter(logFile, false));
                logWriter.println("FRUC log started: " + new java.util.Date());
                logWriter.flush();
                Log.i(TAG, "FRUC log: " + logFile.getAbsolutePath());
            }
        } catch (Exception e) {
            Log.w(TAG, "Could not create FRUC log file: " + e.getMessage());
        }
    }

    public int getInterpolatedCount() { return interpolatedCount; }
    public float getOutputFps() { return currentOutputFps; }
    public void setConnectionPoor(boolean poor) { this.connectionPoor = poor; }
    public void setQualityLevel(int level) { this.qualityLevel = Math.max(0, Math.min(2, level)); }

    /**
     * Initialize the FRUC pipeline. Must be called from the GL thread.
     * @param displaySurface The Surface to render to (from SurfaceHolder)
     * @param w Video width
     * @param h Video height
     * @return The Surface that MediaCodec should output to
     */
    public Surface initialize(Surface displaySurface, int w, int h) {
        this.width = w;
        this.height = h;

        try {
            initEGL(displaySurface);
            initTextures();
            initShaders();
            initQuad();

            initialized = true;
            String msg = "FRUC initialized: " + w + "x" + h + " (GLES 3.1 compute)";
            Log.i(TAG, msg);
            logToFile(msg);
            logToFile("GL_RENDERER: " + GLES31.glGetString(GLES31.GL_RENDERER));
            logToFile("GL_VERSION: " + GLES31.glGetString(GLES31.GL_VERSION));
        } catch (Exception e) {
            String msg = "FRUC init failed: " + e.getMessage();
            Log.e(TAG, msg, e);
            logToFile("INIT FAILED: " + msg);
            if (e.getCause() != null) logToFile("  Cause: " + e.getCause().getMessage());
            destroy();
            return null;
        }

        return inputSurface;
    }

    private void initEGL(Surface displaySurface) {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        EGL14.eglInitialize(eglDisplay, version, 0, version, 1);

        int[] configAttribs = {
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGLExt.EGL_OPENGL_ES3_BIT_KHR,
            EGL14.EGL_NONE
        };
        EGLConfig[] configs = new EGLConfig[1];
        int[] numConfigs = new int[1];
        EGL14.eglChooseConfig(eglDisplay, configAttribs, 0, configs, 0, 1, numConfigs, 0);

        int[] contextAttribs = { EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE };
        eglContext = EGL14.eglCreateContext(eglDisplay, configs[0], EGL14.EGL_NO_CONTEXT, contextAttribs, 0);

        int[] surfaceAttribs = { EGL14.EGL_NONE };
        eglSurface = EGL14.eglCreateWindowSurface(eglDisplay, configs[0], displaySurface, surfaceAttribs, 0);

        EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

        // Force V-sync: each eglSwapBuffers blocks until the frame is displayed.
        // Critical for FRUC 2x: without this, both presents queue instantly
        // and the buffer queue discards the interpolated frame.
        EGL14.eglSwapInterval(eglDisplay, 1);
    }

    private void initTextures() {
        // OES texture for MediaCodec input
        int[] texIds = new int[1];
        GLES31.glGenTextures(1, texIds, 0);
        oesTextureId = texIds[0];
        GLES31.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId);
        GLES31.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES31.GL_TEXTURE_MIN_FILTER, GLES31.GL_LINEAR);
        GLES31.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES31.GL_TEXTURE_MAG_FILTER, GLES31.GL_LINEAR);

        surfaceTexture = new SurfaceTexture(oesTextureId);
        surfaceTexture.setDefaultBufferSize(width, height);
        surfaceTexture.setOnFrameAvailableListener(new SurfaceTexture.OnFrameAvailableListener() {
            @Override
            public void onFrameAvailable(SurfaceTexture st) {
                synchronized (frameSyncLock) {
                    frameAvailable.set(true);
                    frameSyncLock.notifyAll();
                }
            }
        });
        inputSurface = new Surface(surfaceTexture);

        // RGBA textures for prev/curr frames
        prevFrameTex = createRgbaTexture(width, height);
        currFrameTex = createRgbaTexture(width, height);

        // Motion field (RG16I)
        int mvW = width / BLOCK_SIZE;
        int mvH = height / BLOCK_SIZE;
        int[] tex = new int[1];
        GLES31.glGenTextures(1, tex, 0);
        motionFieldTex = tex[0];
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, motionFieldTex);
        GLES31.glTexStorage2D(GLES31.GL_TEXTURE_2D, 1, GLES31.GL_R32I, mvW, mvH);
        GLES31.glTexParameteri(GLES31.GL_TEXTURE_2D, GLES31.GL_TEXTURE_MIN_FILTER, GLES31.GL_NEAREST);
        GLES31.glTexParameteri(GLES31.GL_TEXTURE_2D, GLES31.GL_TEXTURE_MAG_FILTER, GLES31.GL_NEAREST);

        // VipleStream v17: Previous MV field for temporal smoothing
        {
            int[] tex2 = new int[1];
            GLES31.glGenTextures(1, tex2, 0);
            prevMotionFieldTex = tex2[0];
            GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, prevMotionFieldTex);
            GLES31.glTexStorage2D(GLES31.GL_TEXTURE_2D, 1, GLES31.GL_R32I, mvW, mvH);
            GLES31.glTexParameteri(GLES31.GL_TEXTURE_2D, GLES31.GL_TEXTURE_MIN_FILTER, GLES31.GL_NEAREST);
            GLES31.glTexParameteri(GLES31.GL_TEXTURE_2D, GLES31.GL_TEXTURE_MAG_FILTER, GLES31.GL_NEAREST);
        }

        // Interpolated output
        interpFrameTex = createRgbaTexture(width, height);

        // FBO for OES → RGBA conversion
        int[] fbo = new int[1];
        GLES31.glGenFramebuffers(1, fbo, 0);
        conversionFbo = fbo[0];

        Log.i(TAG, "Textures: " + width + "x" + height + ", MV " + mvW + "x" + mvH);
    }

    private int createRgbaTexture(int w, int h) {
        int[] tex = new int[1];
        GLES31.glGenTextures(1, tex, 0);
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, tex[0]);
        GLES31.glTexStorage2D(GLES31.GL_TEXTURE_2D, 1, GLES31.GL_RGBA8, w, h);
        GLES31.glTexParameteri(GLES31.GL_TEXTURE_2D, GLES31.GL_TEXTURE_MIN_FILTER, GLES31.GL_LINEAR);
        GLES31.glTexParameteri(GLES31.GL_TEXTURE_2D, GLES31.GL_TEXTURE_MAG_FILTER, GLES31.GL_LINEAR);
        GLES31.glTexParameteri(GLES31.GL_TEXTURE_2D, GLES31.GL_TEXTURE_WRAP_S, GLES31.GL_CLAMP_TO_EDGE);
        GLES31.glTexParameteri(GLES31.GL_TEXTURE_2D, GLES31.GL_TEXTURE_WRAP_T, GLES31.GL_CLAMP_TO_EDGE);
        return tex[0];
    }

    private void initShaders() {
        oesToRgbaProgram = loadProgram("shaders/fullscreen.vert", "shaders/oes_to_rgba.frag");
        blitProgram = loadProgram("shaders/fullscreen.vert", "shaders/blit.frag");

        // Compile 3 quality variants of each compute shader
        String[] qualityNames = {"Quality", "Balanced", "Performance"};
        for (int q = 0; q < 3; q++) {
            motionEstPrograms[q] = loadComputeProgramWithQuality("shaders/motionest_compute.glsl", q);
            warpPrograms[q] = loadComputeProgramWithQuality("shaders/warp_compute.glsl", q);
            Log.i(TAG, "Shader variant " + qualityNames[q] + " compiled OK");
        }
        // Set active shader pair
        motionEstProgram = motionEstPrograms[qualityLevel];
        warpProgram = warpPrograms[qualityLevel];
        Log.i(TAG, "Active quality: " + qualityNames[qualityLevel]);
    }

    private void initQuad() {
        float[] quad = {
            -1f, -1f, 0f, 1f,
            -1f,  1f, 0f, 0f,
             1f, -1f, 1f, 1f,
             1f,  1f, 1f, 0f,
        };
        quadVertices = ByteBuffer.allocateDirect(quad.length * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer();
        quadVertices.put(quad).position(0);
    }

    /**
     * Process a new frame from MediaCodec. Call this after releaseOutputBuffer.
     * @return true if an interpolated frame was produced and presented
     */
    public boolean onFrameAvailable() {
        if (!initialized) {
            if (frameCount == 0) logToFile("onFrameAvailable: not initialized");
            return false;
        }

        // Ensure our EGL context is current on the calling thread
        if (EGL14.eglGetCurrentContext() != eglContext) {
            EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
        }

        // Wait for frame from MediaCodec (SurfaceTexture callback)
        synchronized (frameSyncLock) {
            if (!frameAvailable.get()) {
                try {
                    frameSyncLock.wait(50); // 50ms timeout
                } catch (InterruptedException e) {
                    return false;
                }
            }
            if (!frameAvailable.getAndSet(false)) {
                if (frameCount == 0) logToFile("onFrameAvailable: timeout waiting for frame");
                return false;
            }
        }

        try {
            surfaceTexture.updateTexImage();
        } catch (Exception e) {
            logToFile("updateTexImage failed: " + e.getMessage());
            return false;
        }
        frameCount++;

        // Convert OES → RGBA into currFrameTex
        convertOesToRgba(currFrameTex);

        if (frameCount <= 1) {
            copyTexture(currFrameTex, prevFrameTex);
            blitToScreen(currFrameTex);
            EGL14.eglSwapBuffers(eglDisplay, eglSurface);
            lastRenderTimeMs = android.os.SystemClock.elapsedRealtime();
            logToFile("frame=1: first frame stored, no interp");
            return false;
        }

        // VipleStream v17: Frame gap detection
        long now = android.os.SystemClock.elapsedRealtime();
        long gap = now - lastRenderTimeMs;
        // Expected interval in ms (30fps=33ms, 60fps=16ms)
        long expectedMs = 33; // Conservative default for 30fps FRUC input
        boolean frameLate = (renderFrameCount > 10) && (gap > expectedMs * 2);
        lastRenderTimeMs = now;
        renderFrameCount++;

        // VipleStream v17: Skip FRUC on late frame or poor connection
        if (frameLate || connectionPoor) {
            // Skip ME/warp, just present real frame immediately and update state
            copyTexture(currFrameTex, prevFrameTex);
            blitToScreen(currFrameTex);
            EGL14.eglSwapBuffers(eglDisplay, eglSurface);
            if (frameLate && frameCount % 100 == 0) {
                logToFile("frame=" + frameCount + " SKIPPED (late=" + gap + "ms, poor=" + connectionPoor + ")");
            }
            return false;
        }

        // Check GL errors before compute
        int glErr = GLES31.glGetError();
        if (glErr != GLES31.GL_NO_ERROR && frameCount <= 5) {
            logToFile("frame=" + frameCount + " GL error before compute: 0x" + Integer.toHexString(glErr));
        }

        // Motion estimation (v17: with temporal predictor)
        runMotionEstimation();

        glErr = GLES31.glGetError();
        if (glErr != GLES31.GL_NO_ERROR && frameCount <= 10) {
            logToFile("frame=" + frameCount + " GL error after motionEst: 0x" + Integer.toHexString(glErr));
        }

        // Warp + blend → interpFrameTex (v17: adaptive blend)
        runWarp();

        glErr = GLES31.glGetError();
        if (glErr != GLES31.GL_NO_ERROR && frameCount <= 10) {
            logToFile("frame=" + frameCount + " GL error after warp: 0x" + Integer.toHexString(glErr));
        }

        // Present interpolated frame (eglSwapInterval=1 ensures V-sync blocking)
        blitToScreen(interpFrameTex);
        EGL14.eglSwapBuffers(eglDisplay, eglSurface);

        // Present real frame (blocks at next V-sync)
        blitToScreen(currFrameTex);
        EGL14.eglSwapBuffers(eglDisplay, eglSurface);

        // Save current as previous (frame + MV field)
        copyTexture(currFrameTex, prevFrameTex);
        copyMVTexture();

        interpolatedCount++;

        // Track output FPS (2 presents per input frame)
        long fpsNow = System.nanoTime();
        fpsWindowPresents += 2;
        if (fpsWindowStart == 0) {
            fpsWindowStart = fpsNow;
        } else {
            long elapsed = fpsNow - fpsWindowStart;
            if (elapsed >= 1_000_000_000L) { // 1 second window
                currentOutputFps = fpsWindowPresents * 1_000_000_000f / elapsed;
                fpsWindowPresents = 0;
                fpsWindowStart = fpsNow;
            }
        }

        if (frameCount <= 5 || frameCount % 300 == 0) {
            logToFile("frame=" + frameCount + " interpolated (total=" + interpolatedCount + ") fps=" + currentOutputFps);
        }

        return true;
    }

    private void convertOesToRgba(int targetTex) {
        GLES31.glBindFramebuffer(GLES31.GL_FRAMEBUFFER, conversionFbo);
        GLES31.glFramebufferTexture2D(GLES31.GL_FRAMEBUFFER, GLES31.GL_COLOR_ATTACHMENT0, GLES31.GL_TEXTURE_2D, targetTex, 0);
        GLES31.glViewport(0, 0, width, height);

        GLES31.glUseProgram(oesToRgbaProgram);
        GLES31.glActiveTexture(GLES31.GL_TEXTURE0);
        GLES31.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId);
        GLES31.glUniform1i(GLES31.glGetUniformLocation(oesToRgbaProgram, "sTexture"), 0);

        // Apply SurfaceTexture transform matrix to fix orientation
        surfaceTexture.getTransformMatrix(stMatrix);
        GLES31.glUniformMatrix4fv(GLES31.glGetUniformLocation(oesToRgbaProgram, "uTexMatrix"), 1, false, stMatrix, 0);

        drawQuad(oesToRgbaProgram);

        GLES31.glBindFramebuffer(GLES31.GL_FRAMEBUFFER, 0);
    }

    private void runMotionEstimation() {
        GLES31.glUseProgram(motionEstProgram);

        GLES31.glActiveTexture(GLES31.GL_TEXTURE0);
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, prevFrameTex);
        GLES31.glUniform1i(GLES31.glGetUniformLocation(motionEstProgram, "prevFrame"), 0);

        GLES31.glActiveTexture(GLES31.GL_TEXTURE1);
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, currFrameTex);
        GLES31.glUniform1i(GLES31.glGetUniformLocation(motionEstProgram, "currFrame"), 1);

        // VipleStream v17: bind previous MV field for temporal predictor
        GLES31.glActiveTexture(GLES31.GL_TEXTURE2);
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, prevMotionFieldTex);
        GLES31.glUniform1i(GLES31.glGetUniformLocation(motionEstProgram, "prevMotionField"), 2);

        GLES31.glBindImageTexture(0, motionFieldTex, 0, false, 0, GLES31.GL_WRITE_ONLY, GLES31.GL_R32I);

        GLES31.glUniform1ui(GLES31.glGetUniformLocation(motionEstProgram, "frameWidth"), width);
        GLES31.glUniform1ui(GLES31.glGetUniformLocation(motionEstProgram, "frameHeight"), height);
        GLES31.glUniform1ui(GLES31.glGetUniformLocation(motionEstProgram, "blockSize"), BLOCK_SIZE);

        int mvW = width / BLOCK_SIZE;
        int mvH = height / BLOCK_SIZE;
        GLES31.glDispatchCompute((mvW + 7) / 8, (mvH + 7) / 8, 1);
        GLES31.glMemoryBarrier(GLES31.GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    private void runWarp() {
        GLES31.glUseProgram(warpProgram);

        GLES31.glActiveTexture(GLES31.GL_TEXTURE0);
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, prevFrameTex);
        GLES31.glUniform1i(GLES31.glGetUniformLocation(warpProgram, "prevFrame"), 0);

        GLES31.glActiveTexture(GLES31.GL_TEXTURE1);
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, currFrameTex);
        GLES31.glUniform1i(GLES31.glGetUniformLocation(warpProgram, "currFrame"), 1);

        GLES31.glActiveTexture(GLES31.GL_TEXTURE2);
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, motionFieldTex);
        GLES31.glUniform1i(GLES31.glGetUniformLocation(warpProgram, "motionField"), 2);

        GLES31.glBindImageTexture(0, interpFrameTex, 0, false, 0, GLES31.GL_WRITE_ONLY, GLES31.GL_RGBA8);

        GLES31.glUniform1ui(GLES31.glGetUniformLocation(warpProgram, "frameWidth"), width);
        GLES31.glUniform1ui(GLES31.glGetUniformLocation(warpProgram, "frameHeight"), height);
        GLES31.glUniform1ui(GLES31.glGetUniformLocation(warpProgram, "mvBlockSize"), BLOCK_SIZE);
        GLES31.glUniform1f(GLES31.glGetUniformLocation(warpProgram, "blendFactor"), 0.5f);

        GLES31.glDispatchCompute((width + 7) / 8, (height + 7) / 8, 1);
        GLES31.glMemoryBarrier(GLES31.GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    private void blitToScreen(int texId) {
        GLES31.glBindFramebuffer(GLES31.GL_FRAMEBUFFER, 0);
        GLES31.glViewport(0, 0, width, height);

        GLES31.glUseProgram(blitProgram);
        GLES31.glActiveTexture(GLES31.GL_TEXTURE0);
        GLES31.glBindTexture(GLES31.GL_TEXTURE_2D, texId);
        GLES31.glUniform1i(GLES31.glGetUniformLocation(blitProgram, "sTexture"), 0);
        GLES31.glUniformMatrix4fv(GLES31.glGetUniformLocation(blitProgram, "uTexMatrix"), 1, false, IDENTITY_MATRIX, 0);

        drawQuad(blitProgram);
    }

    private void drawQuad(int program) {
        int posLoc = GLES31.glGetAttribLocation(program, "aPosition");
        int texLoc = GLES31.glGetAttribLocation(program, "aTexCoord");

        quadVertices.position(0);
        GLES31.glVertexAttribPointer(posLoc, 2, GLES31.GL_FLOAT, false, 16, quadVertices);
        GLES31.glEnableVertexAttribArray(posLoc);

        quadVertices.position(2);
        GLES31.glVertexAttribPointer(texLoc, 2, GLES31.GL_FLOAT, false, 16, quadVertices);
        GLES31.glEnableVertexAttribArray(texLoc);

        GLES31.glDrawArrays(GLES31.GL_TRIANGLE_STRIP, 0, 4);

        GLES31.glDisableVertexAttribArray(posLoc);
        GLES31.glDisableVertexAttribArray(texLoc);
    }

    private void copyTexture(int src, int dst) {
        // Use FBO blit instead of glCopyImageSubData (requires GLES 3.2)
        int[] tmpFbo = new int[2];
        GLES31.glGenFramebuffers(2, tmpFbo, 0);

        GLES31.glBindFramebuffer(GLES31.GL_READ_FRAMEBUFFER, tmpFbo[0]);
        GLES31.glFramebufferTexture2D(GLES31.GL_READ_FRAMEBUFFER, GLES31.GL_COLOR_ATTACHMENT0, GLES31.GL_TEXTURE_2D, src, 0);

        GLES31.glBindFramebuffer(GLES31.GL_DRAW_FRAMEBUFFER, tmpFbo[1]);
        GLES31.glFramebufferTexture2D(GLES31.GL_DRAW_FRAMEBUFFER, GLES31.GL_COLOR_ATTACHMENT0, GLES31.GL_TEXTURE_2D, dst, 0);

        GLES31.glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GLES31.GL_COLOR_BUFFER_BIT, GLES31.GL_NEAREST);

        GLES31.glBindFramebuffer(GLES31.GL_FRAMEBUFFER, 0);
        GLES31.glDeleteFramebuffers(2, tmpFbo, 0);
    }

    // VipleStream v17: copy current MV field to previous for temporal smoothing
    private void copyMVTexture() {
        int mvW = width / BLOCK_SIZE;
        int mvH = height / BLOCK_SIZE;
        int[] tmpFbo = new int[2];
        GLES31.glGenFramebuffers(2, tmpFbo, 0);

        GLES31.glBindFramebuffer(GLES31.GL_READ_FRAMEBUFFER, tmpFbo[0]);
        GLES31.glFramebufferTexture2D(GLES31.GL_READ_FRAMEBUFFER, GLES31.GL_COLOR_ATTACHMENT0, GLES31.GL_TEXTURE_2D, motionFieldTex, 0);

        GLES31.glBindFramebuffer(GLES31.GL_DRAW_FRAMEBUFFER, tmpFbo[1]);
        GLES31.glFramebufferTexture2D(GLES31.GL_DRAW_FRAMEBUFFER, GLES31.GL_COLOR_ATTACHMENT0, GLES31.GL_TEXTURE_2D, prevMotionFieldTex, 0);

        GLES31.glBlitFramebuffer(0, 0, mvW, mvH, 0, 0, mvW, mvH, GLES31.GL_COLOR_BUFFER_BIT, GLES31.GL_NEAREST);

        GLES31.glBindFramebuffer(GLES31.GL_FRAMEBUFFER, 0);
        GLES31.glDeleteFramebuffers(2, tmpFbo, 0);
    }

    public void destroy() {
        logToFile("destroy: frames=" + frameCount + " interpolated=" + interpolatedCount);
        if (logWriter != null) { logWriter.close(); logWriter = null; }
        if (inputSurface != null) { inputSurface.release(); inputSurface = null; }
        if (surfaceTexture != null) { surfaceTexture.release(); surfaceTexture = null; }
        if (eglSurface != null && eglSurface != EGL14.EGL_NO_SURFACE) {
            EGL14.eglDestroySurface(eglDisplay, eglSurface);
        }
        if (eglContext != null && eglContext != EGL14.EGL_NO_CONTEXT) {
            EGL14.eglDestroyContext(eglDisplay, eglContext);
        }
        if (eglDisplay != null && eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglTerminate(eglDisplay);
        }
        initialized = false;
    }

    private void logToFile(String msg) {
        Log.i(TAG, msg);
        if (logWriter != null) {
            logWriter.println(System.currentTimeMillis() + " " + msg);
            logWriter.flush();
        }
    }

    public boolean isInitialized() { return initialized; }

    // --- Shader loading helpers ---

    private int loadProgram(String vertAsset, String fragAsset) {
        String vertSrc = loadAsset(vertAsset);
        String fragSrc = loadAsset(fragAsset);
        int vert = compileShader(GLES31.GL_VERTEX_SHADER, vertSrc);
        int frag = compileShader(GLES31.GL_FRAGMENT_SHADER, fragSrc);
        int prog = GLES31.glCreateProgram();
        GLES31.glAttachShader(prog, vert);
        GLES31.glAttachShader(prog, frag);
        GLES31.glLinkProgram(prog);
        return prog;
    }

    private int loadComputeProgram(String asset) {
        return loadComputeProgramWithQuality(asset, -1);
    }

    private int loadComputeProgramWithQuality(String asset, int quality) {
        String src = loadAsset(asset);
        if (quality >= 0) {
            // Inject #define QUALITY_LEVEL after #version line
            int versionEnd = src.indexOf('\n');
            if (versionEnd > 0) {
                src = src.substring(0, versionEnd + 1)
                    + "#define QUALITY_LEVEL " + quality + "\n"
                    + src.substring(versionEnd + 1);
            }
        }
        int shader = compileShader(GLES31.GL_COMPUTE_SHADER, src);
        int prog = GLES31.glCreateProgram();
        GLES31.glAttachShader(prog, shader);
        GLES31.glLinkProgram(prog);
        return prog;
    }

    private int compileShader(int type, String source) {
        int shader = GLES31.glCreateShader(type);
        GLES31.glShaderSource(shader, source);
        GLES31.glCompileShader(shader);
        int[] status = new int[1];
        GLES31.glGetShaderiv(shader, GLES31.GL_COMPILE_STATUS, status, 0);
        if (status[0] == 0) {
            String log = GLES31.glGetShaderInfoLog(shader);
            Log.e(TAG, "Shader compile failed: " + log);
            GLES31.glDeleteShader(shader);
            throw new RuntimeException("Shader compile failed: " + log);
        }
        return shader;
    }

    private String loadAsset(String path) {
        try {
            InputStream is = context.getAssets().open(path);
            BufferedReader reader = new BufferedReader(new InputStreamReader(is));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append('\n');
            }
            reader.close();
            return sb.toString();
        } catch (IOException e) {
            throw new RuntimeException("Failed to load asset: " + path, e);
        }
    }
}
