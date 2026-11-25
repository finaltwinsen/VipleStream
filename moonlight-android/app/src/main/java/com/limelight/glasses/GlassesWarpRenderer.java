package com.limelight.glasses;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Process;
import android.util.Log;
import android.view.Choreographer;
import android.view.Surface;
import android.view.View;

import com.limelight.vr.QxrNative;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * §VITURE-WARP：眼鏡沒有 compositor。解碼器寫進 SurfaceTexture，每一格用
 * 「算圖姿態 → 現在姿態」的旋轉差把雙眼並排畫面扭正後再送到眼鏡。
 */
public final class GlassesWarpRenderer {
    private static final String TAG = "VipleViture";
    // 到眼鏡實際亮起大約還有一格。currentPose 用這段往前外插。
    private static final long DISPLAY_AHEAD_NS = 8_000_000L;
    private static final long LOG_INTERVAL_NS = 10_000_000_000L;

    // §VITURE-WARP：vClip 是 NDC（y 朝上）。視線的 v=0 在畫面上方，所以 (1-2v) 才是 +Y。
    // OES 的 getTransformMatrix 吃的是底邊為 0 的材質座標，取樣前把 v 翻成 t=1-v'。
    private static final String VERT_ES3 =
            "#version 300 es\n"
                    + "layout(location = 0) in vec2 aPos;\n"
                    + "out vec2 vClip;\n"
                    + "void main() {\n"
                    + "  vClip = aPos;\n"
                    + "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
                    + "}\n";
    private static final String FRAG_ES3 =
            "#version 300 es\n"
                    + "#extension GL_OES_EGL_image_external_essl3 : require\n"
                    + "precision highp float;\n"
                    + "in vec2 vClip;\n"
                    + "out vec4 fragColor;\n"
                    + "uniform samplerExternalOES uTex;\n"
                    + "uniform mat4 uTexMatrix;\n"
                    + "uniform vec4 uQRender;\n"
                    + "uniform vec4 uQCurrent;\n"
                    + "uniform vec2 uTan;\n"
                    + "vec4 qconj(vec4 q) { return vec4(-q.x, -q.y, -q.z, q.w); }\n"
                    + "vec4 qmul(vec4 a, vec4 b) {\n"
                    + "  return vec4(\n"
                    + "    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,\n"
                    + "    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,\n"
                    + "    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,\n"
                    + "    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z);\n"
                    + "}\n"
                    + "vec3 qrotate(vec4 q, vec3 v) {\n"
                    + "  float tx = 2.0 * (q.y * v.z - q.z * v.y);\n"
                    + "  float ty = 2.0 * (q.z * v.x - q.x * v.z);\n"
                    + "  float tz = 2.0 * (q.x * v.y - q.y * v.x);\n"
                    + "  return vec3(\n"
                    + "    v.x + q.w * tx + (q.y * tz - q.z * ty),\n"
                    + "    v.y + q.w * ty + (q.z * tx - q.x * tz),\n"
                    + "    v.z + q.w * tz + (q.x * ty - q.y * tx));\n"
                    + "}\n"
                    + "void main() {\n"
                    + "  float right = vClip.x >= 0.0 ? 1.0 : 0.0;\n"
                    + "  float u = right > 0.5 ? vClip.x : (vClip.x + 1.0);\n"
                    + "  float v = (1.0 - vClip.y) * 0.5;\n"
                    + "  vec3 d = normalize(vec3((2.0 * u - 1.0) * uTan.x, (1.0 - 2.0 * v) * uTan.y, -1.0));\n"
                    + "  vec3 dp = qrotate(qmul(qconj(uQRender), uQCurrent), d);\n"
                    + "  if (dp.z >= 0.0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
                    + "  float nz = -dp.z;\n"
                    + "  float up = (dp.x / nz / uTan.x + 1.0) * 0.5;\n"
                    + "  float vp = (1.0 - dp.y / nz / uTan.y) * 0.5;\n"
                    + "  if (up < 0.0 || up > 1.0 || vp < 0.0 || vp > 1.0) {\n"
                    + "    fragColor = vec4(0.0, 0.0, 0.0, 1.0); return;\n"
                    + "  }\n"
                    + "  float s = (right > 0.5 ? 0.5 : 0.0) + up * 0.5;\n"
                    + "  float t = 1.0 - vp;\n"
                    + "  vec2 st = (uTexMatrix * vec4(s, t, 0.0, 1.0)).xy;\n"
                    + "  fragColor = texture(uTex, st);\n"
                    + "}\n";
    private static final String VERT_ES2 =
            "attribute vec2 aPos;\n"
                    + "varying vec2 vClip;\n"
                    + "void main() {\n"
                    + "  vClip = aPos;\n"
                    + "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
                    + "}\n";
    private static final String FRAG_ES2 =
            "#extension GL_OES_EGL_image_external : require\n"
                    + "precision highp float;\n"
                    + "varying vec2 vClip;\n"
                    + "uniform samplerExternalOES uTex;\n"
                    + "uniform mat4 uTexMatrix;\n"
                    + "uniform vec4 uQRender;\n"
                    + "uniform vec4 uQCurrent;\n"
                    + "uniform vec2 uTan;\n"
                    + "vec4 qconj(vec4 q) { return vec4(-q.x, -q.y, -q.z, q.w); }\n"
                    + "vec4 qmul(vec4 a, vec4 b) {\n"
                    + "  return vec4(\n"
                    + "    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,\n"
                    + "    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,\n"
                    + "    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,\n"
                    + "    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z);\n"
                    + "}\n"
                    + "vec3 qrotate(vec4 q, vec3 v) {\n"
                    + "  float tx = 2.0 * (q.y * v.z - q.z * v.y);\n"
                    + "  float ty = 2.0 * (q.z * v.x - q.x * v.z);\n"
                    + "  float tz = 2.0 * (q.x * v.y - q.y * v.x);\n"
                    + "  return vec3(\n"
                    + "    v.x + q.w * tx + (q.y * tz - q.z * ty),\n"
                    + "    v.y + q.w * ty + (q.z * tx - q.x * tz),\n"
                    + "    v.z + q.w * tz + (q.x * ty - q.y * tx));\n"
                    + "}\n"
                    + "void main() {\n"
                    + "  float right = vClip.x >= 0.0 ? 1.0 : 0.0;\n"
                    + "  float u = right > 0.5 ? vClip.x : (vClip.x + 1.0);\n"
                    + "  float v = (1.0 - vClip.y) * 0.5;\n"
                    + "  vec3 d = normalize(vec3((2.0 * u - 1.0) * uTan.x, (1.0 - 2.0 * v) * uTan.y, -1.0));\n"
                    + "  vec3 dp = qrotate(qmul(qconj(uQRender), uQCurrent), d);\n"
                    + "  if (dp.z >= 0.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
                    + "  float nz = -dp.z;\n"
                    + "  float up = (dp.x / nz / uTan.x + 1.0) * 0.5;\n"
                    + "  float vp = (1.0 - dp.y / nz / uTan.y) * 0.5;\n"
                    + "  if (up < 0.0 || up > 1.0 || vp < 0.0 || vp > 1.0) {\n"
                    + "    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return;\n"
                    + "  }\n"
                    + "  float s = (right > 0.5 ? 0.5 : 0.0) + up * 0.5;\n"
                    + "  float t = 1.0 - vp;\n"
                    + "  vec2 st = (uTexMatrix * vec4(s, t, 0.0, 1.0)).xy;\n"
                    + "  gl_FragColor = texture2D(uTex, st);\n"
                    + "}\n";
    private static final String FRAG_ES2_MEDIUM = FRAG_ES2.replace("precision highp float;", "precision mediump float;");

    private final long nativeHandle;
    private final float tanX;
    private final float tanY;
    private final int bufferW;
    private final int bufferH;
    private final float[] qRender = new float[] {0f, 0f, 0f, 1f};
    private final float[] qCurrent = new float[] {0f, 0f, 0f, 1f};
    private final float[] texMatrix = new float[16];
    private final AtomicBoolean alive = new AtomicBoolean(true);
    private final AtomicBoolean drawQueued = new AtomicBoolean(false);
    private final AtomicBoolean vsyncArmed = new AtomicBoolean(false);
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private HandlerThread glThread;
    private Handler glHandler;
    private volatile boolean running;
    private volatile View vsyncView;

    private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
    private EGLContext context = EGL14.EGL_NO_CONTEXT;
    private EGLSurface pbuffer = EGL14.EGL_NO_SURFACE;
    private EGLSurface window = EGL14.EGL_NO_SURFACE;
    private EGLConfig config;
    private int program;
    private int vbo;
    private int oesTex;
    private int tanLoc = -1;
    private int qRenderLoc = -1;
    private int qCurrentLoc = -1;
    private int texMatLoc = -1;
    private int texLoc = -1;
    private SurfaceTexture surfaceTexture;
    private Surface decoderSurface;
    private Surface outputSurface;
    private boolean outputReady;
    private boolean haveImage;
    private boolean nativeOk = true;
    private boolean loggedFirstImage;
    private boolean loggedTexError;
    private boolean loggedDrawGl;
    private boolean loggedSwap;
    private long ptsUs = -1L;
    private int viewW;
    private int viewH;
    private int clientVersion;
    private int statDrawn;
    private int statFresh;
    private int statPose;
    private int statAngleCount;
    private double statAngleSum;
    private long statWindowNs;

    private final Runnable viewVsync = new Runnable() {
        @Override
        public void run() {
            requestDraw();
            scheduleNext();
        }
    };

    private final Choreographer.FrameCallback mainVsync = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            requestDraw();
            scheduleNext();
        }
    };

    private final Runnable drawTask = new Runnable() {
        @Override
        public void run() {
            try {
                if (running) {
                    drawFrame();
                }
            } catch (RuntimeException error) {
                // 畫一格失敗不能讓 GL 執行緒停掉，否則解碼緩衝會塞滿。
                if (!loggedDrawGl) {
                    loggedDrawGl = true;
                    Log.w(TAG, "[VIPLE-VITURE] warp draw exception", error);
                }
            } finally {
                drawQueued.set(false);
            }
        }
    };

    private GlassesWarpRenderer(long nativeHandle, float tanX, float tanY, int width, int height) {
        this.nativeHandle = nativeHandle;
        // 不是正數就用 Beast 58° 對角那組（fovTan log 的 4831／2718），避免 shader 除以 0。
        this.tanX = tanX > 0.01f ? tanX : 0.4831f;
        this.tanY = tanY > 0.01f ? tanY : 0.2718f;
        this.bufferW = width;
        this.bufferH = height;
        texMatrix[0] = 1f;
        texMatrix[5] = 1f;
        texMatrix[10] = 1f;
        texMatrix[15] = 1f;
    }

    /**
     * 失敗回 null。呼叫端退回解碼器直接畫上眼鏡。
     */
    public static GlassesWarpRenderer start(long nativeHandle, float tanX, float tanY, int width, int height) {
        if (nativeHandle == 0 || width <= 0 || height <= 0) {
            logW("warp start rejected handle=" + nativeHandle + " " + width + "x" + height);
            return null;
        }
        GlassesWarpRenderer renderer = new GlassesWarpRenderer(nativeHandle, tanX, tanY, width, height);
        if (!renderer.boot()) {
            renderer.release();
            return null;
        }
        return renderer;
    }

    public Surface getDecoderSurface() {
        return decoderSurface;
    }

    public boolean attachOutput(Surface surface, View vsync) {
        if (!alive.get() || glHandler == null || surface == null || !surface.isValid()) {
            logW("warp attach rejected");
            return false;
        }
        vsyncView = vsync;
        final boolean[] ok = new boolean[1];
        final CountDownLatch latch = new CountDownLatch(1);
        glHandler.post(new Runnable() {
            @Override
            public void run() {
                ok[0] = attachOnGl(surface);
                latch.countDown();
            }
        });
        if (!await(latch, 1000, "attach")) {
            return false;
        }
        if (ok[0]) {
            ensureVsync();
        }
        return ok[0];
    }

    public void detachOutput() {
        vsyncView = null;
        if (!alive.get() || glHandler == null) {
            return;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        glHandler.post(new Runnable() {
            @Override
            public void run() {
                detachOnGl();
                latch.countDown();
            }
        });
        await(latch, 500, "detach");
    }

    public void release() {
        if (!alive.compareAndSet(true, false)) {
            return;
        }
        running = false;
        final View view = vsyncView;
        vsyncView = null;
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                Choreographer.getInstance().removeFrameCallback(mainVsync);
                if (view != null) {
                    view.removeCallbacks(viewVsync);
                }
            }
        });
        if (glHandler == null) {
            if (glThread != null) {
                glThread.quitSafely();
            }
            return;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        glHandler.post(new Runnable() {
            @Override
            public void run() {
                destroyOnGl();
                latch.countDown();
            }
        });
        await(latch, 1500, "release");
        glThread.quitSafely();
        try {
            glThread.join(500);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
        }
    }

    private boolean boot() {
        glThread = new HandlerThread("viple-viture-warp", Process.THREAD_PRIORITY_URGENT_DISPLAY);
        glThread.start();
        glHandler = new Handler(glThread.getLooper());
        final boolean[] ok = new boolean[1];
        final CountDownLatch latch = new CountDownLatch(1);
        glHandler.post(new Runnable() {
            @Override
            public void run() {
                try {
                    ok[0] = initOnGl();
                } catch (RuntimeException error) {
                    logW("warp init exception: " + error.getMessage());
                    try {
                        destroyOnGl();
                    } catch (RuntimeException ignored) {
                        // 初始化已經失敗，這裡再丟會讓 latch 停不住。
                    }
                    ok[0] = false;
                }
                latch.countDown();
            }
        });
        if (!await(latch, 2000, "init") || !ok[0]) {
            return false;
        }
        running = true;
        ensureVsync();
        return true;
    }

    private boolean initOnGl() {
        drainEglError();
        if (!initDisplay()) {
            destroyOnGl();
            return false;
        }
        if (tryGl(3) || tryGl(2)) {
            statWindowNs = System.nanoTime();
            VitureUsb.logI("warp gl ready es=" + clientVersion + " buffer=" + bufferW + "x" + bufferH
                    + " tanX=" + tanX + " tanY=" + tanY);
            return true;
        }
        destroyOnGl();
        return false;
    }

    private boolean tryGl(int version) {
        releaseGlObjects();
        if (!createContext(version)) {
            return false;
        }
        if (!makePbufferCurrent()) {
            return false;
        }
        if (!buildProgram(version >= 3)) {
            return false;
        }
        if (!buildGeometry()) {
            return false;
        }
        if (!buildTexture()) {
            return false;
        }
        clientVersion = version;
        GLES20.glDisable(GLES20.GL_DEPTH_TEST);
        GLES20.glDisable(GLES20.GL_BLEND);
        GLES20.glDisable(GLES20.GL_CULL_FACE);
        GLES20.glClearColor(0f, 0f, 0f, 1f);
        return glOk("ready");
    }

    private boolean initDisplay() {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (display == EGL14.EGL_NO_DISPLAY || eglFailed("getDisplay")) {
            display = EGL14.EGL_NO_DISPLAY;
            return false;
        }
        int[] version = new int[2];
        if (!EGL14.eglInitialize(display, version, 0, version, 1) || eglFailed("initialize")) {
            return false;
        }
        if (!EGL14.eglBindAPI(EGL14.EGL_OPENGL_ES_API) || eglFailed("bindApi")) {
            return false;
        }
        return true;
    }

    private boolean createContext(int version) {
        int renderable = version >= 3 ? EGLExt.EGL_OPENGL_ES3_BIT_KHR : EGL14.EGL_OPENGL_ES2_BIT;
        int[] attribs = new int[] {
                EGL14.EGL_RED_SIZE, 8,
                EGL14.EGL_GREEN_SIZE, 8,
                EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_DEPTH_SIZE, 0,
                EGL14.EGL_RENDERABLE_TYPE, renderable,
                // 視窗拆掉之後還要 updateTexImage，所以同一組 config 要能做 pbuffer。
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT | EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE
        };
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        if (!EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, count, 0)
                || count[0] < 1 || eglFailed("chooseConfig")) {
            logW("warp no egl config es=" + version);
            return false;
        }
        config = configs[0];
        int[] ctxAttrib = new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, version, EGL14.EGL_NONE};
        context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT, ctxAttrib, 0);
        int ctxErr = EGL14.eglGetError();
        if (context == null || context == EGL14.EGL_NO_CONTEXT || ctxErr != EGL14.EGL_SUCCESS) {
            logW("warp egl createContext error=0x" + Integer.toHexString(ctxErr));
            context = EGL14.EGL_NO_CONTEXT;
            return false;
        }
        int[] pbAttrib = new int[] {EGL14.EGL_WIDTH, 16, EGL14.EGL_HEIGHT, 16, EGL14.EGL_NONE};
        pbuffer = EGL14.eglCreatePbufferSurface(display, config, pbAttrib, 0);
        int pbErr = EGL14.eglGetError();
        if (pbuffer == null || pbuffer == EGL14.EGL_NO_SURFACE || pbErr != EGL14.EGL_SUCCESS) {
            logW("warp egl pbuffer error=0x" + Integer.toHexString(pbErr));
            pbuffer = EGL14.EGL_NO_SURFACE;
            return false;
        }
        return true;
    }

    private boolean makePbufferCurrent() {
        if (!EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context) || eglFailed("makeCurrent pbuffer")) {
            return false;
        }
        return true;
    }

    private boolean buildProgram(boolean es3) {
        int vs = compile(GLES20.GL_VERTEX_SHADER, es3 ? VERT_ES3 : VERT_ES2);
        int fs = compile(GLES20.GL_FRAGMENT_SHADER, es3 ? FRAG_ES3 : FRAG_ES2);
        if (!es3 && fs == 0) {
            // 有的 GPU 片段 shader 沒有 highp。精度差一點仍比黑屏好。
            fs = compile(GLES20.GL_FRAGMENT_SHADER, FRAG_ES2_MEDIUM);
        }
        if (vs == 0 || fs == 0) {
            if (vs != 0) {
                GLES20.glDeleteShader(vs);
            }
            if (fs != 0) {
                GLES20.glDeleteShader(fs);
            }
            return false;
        }
        program = GLES20.glCreateProgram();
        if (program == 0) {
            logW("warp glCreateProgram failed");
            GLES20.glDeleteShader(vs);
            GLES20.glDeleteShader(fs);
            return false;
        }
        GLES20.glAttachShader(program, vs);
        GLES20.glAttachShader(program, fs);
        GLES20.glBindAttribLocation(program, 0, "aPos");
        GLES20.glLinkProgram(program);
        GLES20.glDeleteShader(vs);
        GLES20.glDeleteShader(fs);
        int[] linked = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linked, 0);
        if (linked[0] == 0) {
            logW("warp program link: " + GLES20.glGetProgramInfoLog(program));
            GLES20.glDeleteProgram(program);
            program = 0;
            return false;
        }
        tanLoc = GLES20.glGetUniformLocation(program, "uTan");
        qRenderLoc = GLES20.glGetUniformLocation(program, "uQRender");
        qCurrentLoc = GLES20.glGetUniformLocation(program, "uQCurrent");
        texMatLoc = GLES20.glGetUniformLocation(program, "uTexMatrix");
        texLoc = GLES20.glGetUniformLocation(program, "uTex");
        if (tanLoc < 0 || qRenderLoc < 0 || qCurrentLoc < 0 || texMatLoc < 0 || texLoc < 0) {
            logW("warp missing uniform");
            return false;
        }
        return glOk("program");
    }

    private int compile(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        if (shader == 0) {
            logW("warp glCreateShader failed type=" + type);
            return 0;
        }
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) {
            logW("warp shader: " + GLES20.glGetShaderInfoLog(shader));
            GLES20.glDeleteShader(shader);
            return 0;
        }
        if (!glOk("compile")) {
            GLES20.glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    private boolean buildGeometry() {
        int[] ids = new int[1];
        GLES20.glGenBuffers(1, ids, 0);
        vbo = ids[0];
        if (vbo == 0) {
            logW("warp glGenBuffers failed");
            return false;
        }
        float[] xy = new float[] {-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f};
        FloatBuffer buffer = ByteBuffer.allocateDirect(xy.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        buffer.put(xy).position(0);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo);
        GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, xy.length * 4, buffer, GLES20.GL_STATIC_DRAW);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0);
        return glOk("vbo");
    }

    private boolean buildTexture() {
        int[] ids = new int[1];
        GLES20.glGenTextures(1, ids, 0);
        oesTex = ids[0];
        if (oesTex == 0) {
            logW("warp glGenTextures failed");
            return false;
        }
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        // 外接材質有的驅動不讓改 wrap。過濾失敗才算這條路不能用。
        int err = GLES20.glGetError();
        if (err != GLES20.GL_NO_ERROR && err != GLES20.GL_INVALID_ENUM) {
            logW("warp tex param error=0x" + Integer.toHexString(err));
            return false;
        }
        surfaceTexture = new SurfaceTexture(oesTex);
        surfaceTexture.setDefaultBufferSize(bufferW, bufferH);
        surfaceTexture.setOnFrameAvailableListener(new SurfaceTexture.OnFrameAvailableListener() {
            @Override
            public void onFrameAvailable(SurfaceTexture surface) {
                consumeDecoderImage();
            }
        }, glHandler);
        decoderSurface = new Surface(surfaceTexture);
        if (!decoderSurface.isValid()) {
            logW("warp decoder surface invalid");
            return false;
        }
        return true;
    }

    private void consumeDecoderImage() {
        if (!alive.get() || surfaceTexture == null) {
            return;
        }
        try {
            surfaceTexture.updateTexImage();
            surfaceTexture.getTransformMatrix(texMatrix);
            long tsNs = surfaceTexture.getTimestamp();
            // releaseOutputBuffer(render=true) 留下的是 PTS 微秒 × 1000。幀號要先除回微秒。
            if (tsNs >= 0) {
                ptsUs = tsNs / 1000L;
            }
            haveImage = true;
            statFresh++;
            if (!loggedFirstImage) {
                loggedFirstImage = true;
                VitureUsb.logI("warp first image ptsUs=" + ptsUs + " tsNs=" + tsNs);
            }
        } catch (RuntimeException error) {
            if (!loggedTexError) {
                loggedTexError = true;
                Log.w(TAG, "[VIPLE-VITURE] warp updateTexImage failed", error);
            }
        }
    }

    private boolean attachOnGl(Surface surface) {
        if (display == EGL14.EGL_NO_DISPLAY || context == EGL14.EGL_NO_CONTEXT || config == null) {
            return false;
        }
        detachOnGl();
        int[] attrib = new int[] {EGL14.EGL_NONE};
        window = EGL14.eglCreateWindowSurface(display, config, surface, attrib, 0);
        int windowErr = EGL14.eglGetError();
        if (window == null || window == EGL14.EGL_NO_SURFACE || windowErr != EGL14.EGL_SUCCESS) {
            logW("warp egl window error=0x" + Integer.toHexString(windowErr));
            window = EGL14.EGL_NO_SURFACE;
            makePbufferCurrent();
            return false;
        }
        if (!EGL14.eglMakeCurrent(display, window, window, context) || eglFailed("makeCurrent window")) {
            EGL14.eglDestroySurface(display, window);
            window = EGL14.EGL_NO_SURFACE;
            makePbufferCurrent();
            return false;
        }
        // 節奏交給 Choreographer。swap 再等一次會跟另一個 vsync 疊起來，拆視窗時也會卡在 swap。
        EGL14.eglSwapInterval(display, 0);
        eglFailed("swapInterval");
        int[] w = new int[1];
        int[] h = new int[1];
        EGL14.eglQuerySurface(display, window, EGL14.EGL_WIDTH, w, 0);
        EGL14.eglQuerySurface(display, window, EGL14.EGL_HEIGHT, h, 0);
        if (eglFailed("querySurface") || w[0] <= 0 || h[0] <= 0) {
            detachOnGl();
            return false;
        }
        viewW = w[0];
        viewH = h[0];
        outputSurface = surface;
        outputReady = true;
        VitureUsb.logI("warp window " + viewW + "x" + viewH + " stream=" + bufferW + "x" + bufferH);
        GLES20.glViewport(0, 0, viewW, viewH);
        if (haveImage) {
            drawFrame();
        } else {
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            EGL14.eglSwapBuffers(display, window);
        }
        return glOk("attach");
    }

    private void detachOnGl() {
        outputReady = false;
        outputSurface = null;
        if (display == EGL14.EGL_NO_DISPLAY || window == EGL14.EGL_NO_SURFACE) {
            return;
        }
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(display, window);
        window = EGL14.EGL_NO_SURFACE;
        drainEglError();
        if (pbuffer != EGL14.EGL_NO_SURFACE && context != EGL14.EGL_NO_CONTEXT) {
            EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context);
            drainEglError();
        }
    }

    private void drawFrame() {
        if (!outputReady || window == EGL14.EGL_NO_SURFACE || !haveImage || program == 0) {
            return;
        }
        if (!EGL14.eglMakeCurrent(display, window, window, context)) {
            if (!loggedSwap) {
                loggedSwap = true;
                logW("warp makeCurrent draw error=0x" + Integer.toHexString(EGL14.eglGetError()));
            }
            return;
        }
        boolean renderOk = false;
        boolean currentOk = false;
        if (nativeOk) {
            try {
                renderOk = QxrNative.nativeWarpRenderPose(nativeHandle, ptsUs, qRender);
                currentOk = QxrNative.nativeWarpCurrentPose(nativeHandle, DISPLAY_AHEAD_NS, qCurrent);
            } catch (UnsatisfiedLinkError error) {
                nativeOk = false;
                logW("warp native missing: " + error.getMessage());
            }
        }
        if (renderOk) {
            statPose++;
        }
        if (renderOk && currentOk) {
            statAngleSum += correctionDegrees(qRender, qCurrent);
            statAngleCount++;
        } else {
            // 缺一邊就不要用絕對姿態去扭，否則畫面會整顆轉走。單位旋轉＝原圖。
            setIdentity(qRender);
            setIdentity(qCurrent);
        }
        GLES20.glViewport(0, 0, viewW, viewH);
        GLES20.glUseProgram(program);
        GLES20.glUniform2f(tanLoc, tanX, tanY);
        GLES20.glUniform4fv(qRenderLoc, 1, qRender, 0);
        GLES20.glUniform4fv(qCurrentLoc, 1, qCurrent, 0);
        GLES20.glUniformMatrix4fv(texMatLoc, 1, false, texMatrix, 0);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex);
        GLES20.glUniform1i(texLoc, 0);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo);
        GLES20.glEnableVertexAttribArray(0);
        GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 8, 0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0);
        if (!EGL14.eglSwapBuffers(display, window)) {
            if (!loggedSwap) {
                loggedSwap = true;
                logW("warp swap error=0x" + Integer.toHexString(EGL14.eglGetError()));
            }
        }
        statDrawn++;
        int glErr = GLES20.glGetError();
        if (glErr != GLES20.GL_NO_ERROR && !loggedDrawGl) {
            loggedDrawGl = true;
            logW("warp draw error=0x" + Integer.toHexString(glErr));
        }
        long now = System.nanoTime();
        if (statWindowNs == 0) {
            statWindowNs = now;
        } else if (now - statWindowNs >= LOG_INTERVAL_NS) {
            float posePct = statDrawn > 0 ? (100f * statPose / statDrawn) : 0f;
            float angle = statAngleCount > 0 ? (float) (statAngleSum / statAngleCount) : 0f;
            VitureUsb.logI(String.format(Locale.US,
                    "warp 10s drawn=%d fresh=%d pose=%.1f%% angleAvg=%.2f deg",
                    statDrawn, statFresh, posePct, angle));
            statDrawn = 0;
            statFresh = 0;
            statPose = 0;
            statAngleCount = 0;
            statAngleSum = 0;
            statWindowNs = now;
        }
    }

    private void requestDraw() {
        if (!running || glHandler == null) {
            return;
        }
        if (drawQueued.compareAndSet(false, true)) {
            glHandler.post(drawTask);
        }
    }

    private void ensureVsync() {
        if (!running) {
            return;
        }
        if (vsyncArmed.compareAndSet(false, true)) {
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    scheduleNext();
                }
            });
        }
    }

    private void scheduleNext() {
        if (!running) {
            vsyncArmed.set(false);
            return;
        }
        if (Looper.myLooper() != Looper.getMainLooper()) {
            mainHandler.post(new Runnable() {
                @Override
                public void run() {
                    scheduleNext();
                }
            });
            return;
        }
        View view = vsyncView;
        // Presentation 上的 view 跟著那扇視窗的 vsync。拆掉時退回主執行緒，頭還在轉也能繼續排重畫；沒有視窗時 draw 直接返回。
        if (view != null && view.isAttachedToWindow()) {
            view.postOnAnimation(viewVsync);
        } else {
            Choreographer.getInstance().postFrameCallback(mainVsync);
        }
    }

    private void releaseGlObjects() {
        // SurfaceTexture 要在建立它的 context 還 current 時放。先刪 GL 物件再拆 context。
        if (surfaceTexture != null) {
            surfaceTexture.setOnFrameAvailableListener(null);
        }
        boolean current = false;
        if (display != EGL14.EGL_NO_DISPLAY && context != EGL14.EGL_NO_CONTEXT) {
            EGLSurface draw = pbuffer != EGL14.EGL_NO_SURFACE ? pbuffer : window;
            if (draw != EGL14.EGL_NO_SURFACE) {
                current = EGL14.eglMakeCurrent(display, draw, draw, context);
            }
        }
        if (decoderSurface != null) {
            decoderSurface.release();
            decoderSurface = null;
        }
        if (surfaceTexture != null) {
            try {
                surfaceTexture.release();
            } catch (RuntimeException error) {
                logW("warp SurfaceTexture release: " + error.getMessage());
            }
            surfaceTexture = null;
        }
        if (current) {
            if (oesTex != 0) {
                GLES20.glDeleteTextures(1, new int[] {oesTex}, 0);
            }
            if (vbo != 0) {
                GLES20.glDeleteBuffers(1, new int[] {vbo}, 0);
            }
            if (program != 0) {
                GLES20.glDeleteProgram(program);
            }
            int guard = 0;
            while (GLES20.glGetError() != GLES20.GL_NO_ERROR && guard++ < 8) {
                // teardown 的錯誤不再往下帶。
            }
        }
        oesTex = 0;
        vbo = 0;
        program = 0;
        haveImage = false;
        outputReady = false;
        outputSurface = null;
        if (display != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (window != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(display, window);
                window = EGL14.EGL_NO_SURFACE;
            }
            if (pbuffer != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(display, pbuffer);
                pbuffer = EGL14.EGL_NO_SURFACE;
            }
            if (context != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(display, context);
                context = EGL14.EGL_NO_CONTEXT;
            }
            drainEglError();
        }
        config = null;
    }

    private void destroyOnGl() {
        releaseGlObjects();
        if (display != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglTerminate(display);
            display = EGL14.EGL_NO_DISPLAY;
            drainEglError();
        }
        EGL14.eglReleaseThread();
    }

    private static void setIdentity(float[] q) {
        q[0] = 0f;
        q[1] = 0f;
        q[2] = 0f;
        q[3] = 1f;
    }

    private static float correctionDegrees(float[] render, float[] current) {
        float iw = render[3];
        float ix = -render[0];
        float iy = -render[1];
        float iz = -render[2];
        float dw = iw * current[3] - ix * current[0] - iy * current[1] - iz * current[2];
        float aw = Math.abs(dw);
        if (aw > 1f) {
            aw = 1f;
        }
        return (float) Math.toDegrees(2.0 * Math.acos(aw));
    }

    private boolean await(CountDownLatch latch, long timeoutMs, String step) {
        try {
            if (!latch.await(timeoutMs, TimeUnit.MILLISECONDS)) {
                logW("warp " + step + " timed out");
                return false;
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            logW("warp " + step + " interrupted");
            return false;
        }
        return true;
    }

    private void drainEglError() {
        int guard = 0;
        while (EGL14.eglGetError() != EGL14.EGL_SUCCESS && guard++ < 8) {
            // 清掉上一階段留下的錯誤，下一動的檢查才對得起來。
        }
    }

    private boolean eglFailed(String step) {
        int err = EGL14.eglGetError();
        if (err != EGL14.EGL_SUCCESS) {
            logW("warp egl " + step + " error=0x" + Integer.toHexString(err));
            return true;
        }
        return false;
    }

    private boolean glOk(String step) {
        int err = GLES20.glGetError();
        if (err != GLES20.GL_NO_ERROR) {
            logW("warp gl " + step + " error=0x" + Integer.toHexString(err));
            while (GLES20.glGetError() != GLES20.GL_NO_ERROR) {
                // 把這一動後面的錯誤一次清掉。
            }
            return false;
        }
        return true;
    }

    private static void logW(String message) {
        Log.w(TAG, "[VIPLE-VITURE] " + message);
    }
}
