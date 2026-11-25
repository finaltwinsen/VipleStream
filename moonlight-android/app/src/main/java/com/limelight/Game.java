package com.limelight;


import com.limelight.binding.PlatformBinding;
import com.limelight.computers.ComputerManagerService;
import com.limelight.binding.audio.AndroidAudioRenderer;
import com.limelight.binding.input.ControllerHandler;
import com.limelight.binding.input.KeyboardTranslator;
import com.limelight.binding.input.capture.InputCaptureManager;
import com.limelight.binding.input.capture.InputCaptureProvider;
import com.limelight.binding.input.touch.AbsoluteTouchContext;
import com.limelight.binding.input.touch.RelativeTouchContext;
import com.limelight.binding.input.driver.UsbDriverService;
import com.limelight.binding.input.evdev.EvdevListener;
import com.limelight.binding.input.touch.TouchContext;
import com.limelight.binding.input.virtual_controller.VirtualController;
import com.limelight.binding.video.CrashListener;
import com.limelight.binding.video.MediaCodecDecoderRenderer;
import com.limelight.binding.video.MediaCodecHelper;
import com.limelight.binding.video.PerfOverlayListener;
import com.limelight.nvstream.NvConnection;
import com.limelight.nvstream.NvConnectionListener;
import com.limelight.nvstream.StreamConfiguration;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.http.NvHTTP;
import com.limelight.nvstream.input.ControllerPacket;
import com.limelight.nvstream.input.KeyboardPacket;
import com.limelight.nvstream.input.MouseButtonPacket;
import com.limelight.nvstream.jni.MoonBridge;
import com.limelight.glasses.GlassesDisplaySwitcher;
import com.limelight.glasses.GlassesVrSession;
import com.limelight.glasses.GlassesWarpRenderer;
import com.limelight.glasses.GlassesWearMonitor;
import com.limelight.glasses.VitureGlasses;
import com.limelight.glasses.VitureUsb;
import com.limelight.glasses.VitureV2Protocol;
import com.limelight.nvstream.vr.VrLaunchConfig;
import com.limelight.preferences.GlPreferences;
import com.limelight.preferences.PreferenceConfiguration;
import com.limelight.ui.ExternalDisplayHelper;
import com.limelight.ui.GameGestures;
import com.limelight.ui.StreamPresentation;
import com.limelight.ui.StreamView;
import com.limelight.utils.CacheHelper;
import com.limelight.utils.Dialog;
import com.limelight.utils.LogDrop;
import com.limelight.utils.QuestSupport;
import com.limelight.utils.ServerHelper;
import com.limelight.utils.ShortcutHelper;
import com.limelight.utils.SpinnerDialog;
import com.limelight.utils.UiHelper;
import com.limelight.vr.QxrNative;
import com.limelight.vr.QxrVideoTest;
import com.limelight.vr.VrStreamDefaults;

import android.annotation.SuppressLint;
import android.annotation.TargetApi;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.PictureInPictureParams;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Rational;
import android.util.TypedValue;
import android.view.Display;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewGroup;
import android.view.View.OnGenericMotionListener;
import android.view.View.OnSystemUiVisibilityChangeListener;
import android.view.View.OnTouchListener;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.view.inputmethod.InputMethodManager;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayInputStream;
import java.io.StringReader;
import java.util.List;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;


public class Game extends Activity implements SurfaceHolder.Callback,
        OnGenericMotionListener, OnTouchListener, NvConnectionListener, EvdevListener,
        OnSystemUiVisibilityChangeListener, GameGestures, StreamView.InputCallbacks,
        PerfOverlayListener, UsbDriverService.UsbDriverStateListener, View.OnKeyListener {
    // 記錄目前是否有串流正在進行（供自我更新程式 AppUpdater 檢查）
    private static volatile boolean sIsStreaming = false;

    public static boolean isStreaming() {
        return sIsStreaming;
    }

    private int lastButtonState = 0;
    // §QUEST-2D：實機只留前幾筆指標 log，避免 hover 洗版
    private int questPointerLogsLeft = 12;
    private int questLoggedButtonState = -1;

    // Only 2 touches are supported
    private final TouchContext[] touchContextMap = new TouchContext[2];
    private long threeFingerDownTime = 0;

    private static final int REFERENCE_HORIZ_RES = 1280;
    private static final int REFERENCE_VERT_RES = 720;

    private static final int STYLUS_DOWN_DEAD_ZONE_DELAY = 100;
    private static final int STYLUS_DOWN_DEAD_ZONE_RADIUS = 20;

    private static final int STYLUS_UP_DEAD_ZONE_DELAY = 150;
    private static final int STYLUS_UP_DEAD_ZONE_RADIUS = 50;

    private static final int THREE_FINGER_TAP_THRESHOLD = 300;

    private ControllerHandler controllerHandler;
    private KeyboardTranslator keyboardTranslator;
    private VirtualController virtualController;

    private PreferenceConfiguration prefConfig;
    private SharedPreferences tombstonePrefs;

    private NvConnection conn;
    private SpinnerDialog spinner;
    // VipleStream §N — in-stream file transfer client
    private com.limelight.transfer.FileTransferClient fileTransferClient;
    private String xferHost;
    private int xferHttpsPort;
    private X509Certificate xferServerCert;
    private boolean displayedFailureDialog = false;
    // §CONSOLE-LOGIN：host 拒絕訊息先到 displayMessage，對話框在 stageFailed 才畫。
    private volatile boolean hostNeedsLogin;
    private AlertDialog consoleLoginDialog;
    private boolean consoleLoginStarting;
    private boolean consoleLoginServiceBound;
    private ServiceConnection consoleLoginServiceConnection;
    private boolean connecting = false;
    private boolean connected = false;
    // §LOGDROP：MIN_VALUE 代表 connectionTerminated／stageFailed 都還沒給結束碼。0 是正常結束碼，不能拿來當空值。
    private volatile int streamEndError = Integer.MIN_VALUE;
    private volatile String streamEndStage;
    // §LOGDROP：connectionTerminated 會先拆眼鏡再 stopConnection。旗標在物件被清掉之前寫上，同一場才不會送兩次。
    private volatile boolean glassesSessionEndOwned;
    private volatile long streamAttemptElapsed;
    private volatile long streamLiveElapsed;
    private boolean autoEnterPip = false;
    private boolean surfaceCreated = false;
    private boolean attemptedConnection = false;
    private int suppressPipRefCount = 0;
    private String pcName;
    private String appName;
    private NvApp app;
    private float desiredRefreshRate;

    private InputCaptureProvider inputCaptureProvider;
    private int modifierFlags = 0;
    private boolean grabbedInput = true;
    private boolean cursorVisible = false;
    private boolean waitingForAllModifiersUp = false;
    private int specialKeyCode = KeyEvent.KEYCODE_UNKNOWN;
    private StreamView streamView;
    // §EXT-DISPLAY：沒有外接顯示器時這兩個是同一個物件，holder 與尺寸跟以前一樣。
    // 有外接時 videoView 改指眼鏡上的 StreamView，streamView 仍留在手機收觸控。
    private StreamView videoView;
    private StreamPresentation externalPresentation;
    private Display externalDisplay;
    private Display.Mode externalChosenMode;
    private int externalDisplayId = Display.INVALID_DISPLAY;
    private boolean externalDisplayActive;
    private boolean externalDisplayLossHandled;
    private boolean externalDismissPending;
    private DisplayManager.DisplayListener externalDisplayListener;
    // §EXT-DISPLAY：眼鏡換模式會讓外接顯示器短暫消失。立刻結束的話，每次切模式都斷線。
    // 實機量到眼鏡重新出現要約 7 秒（另一位開發者在 Galaxy S25＋Beast 上的紀錄），8 秒太貼近。
    private static final int EXTERNAL_DISPLAY_GRACE_MS = 15000;
    // 結果字與摘下提示是白字。亮度 0 時先交回系統亮度，這段時間過了才回到調暗。
    private static final int EXTERNAL_DIM_HINT_MS = 3000;
    private final Handler externalDisplayHandler = new Handler(Looper.getMainLooper());
    // glasses_vr 會清掉這個旗標。留著的話，摘下提示的計時結束會把視窗再寫回 0。
    private boolean externalTouchpadDimmed;
    private final Runnable restoreExternalTouchpadDimRunnable = new Runnable() {
        @Override
        public void run() {
            if (isFinishing() || isDestroyed() || !externalTouchpadDimmed) {
                return;
            }
            WindowManager.LayoutParams params = getWindow().getAttributes();
            params.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_OFF;
            getWindow().setAttributes(params);
        }
    };
    private boolean externalDisplayAwaiting;
    private boolean externalReconnectPending;
    private boolean externalReconnectRunning;
    private int externalReconnectGeneration;
    private long externalDisplayLostUptimeMs;
    private Display.Mode externalDisplayLostMode;
    private SurfaceTexture externalHoldTexture;
    private Surface externalHoldSurface;
    private final Runnable externalDisplayGraceRunnable = new Runnable() {
        @Override
        public void run() {
            if (!externalDisplayAwaiting || externalDisplayLossHandled || isFinishing() || isDestroyed()) {
                return;
            }
            LimeLog.info("[VIPLE-EXTDISP] grace period elapsed after " + EXTERNAL_DISPLAY_GRACE_MS + " ms; ending stream");
            finishExternalDisplayLoss();
        }
    };
    private long lastAbsTouchUpTime = 0;
    private long lastAbsTouchDownTime = 0;
    private float lastAbsTouchUpX, lastAbsTouchUpY;
    private float lastAbsTouchDownX, lastAbsTouchDownY;

    private boolean isHidingOverlays;
    private TextView notificationOverlayView;
    private int requestedNotificationOverlayVisibility = View.GONE;

    // VipleStream §N.5 — File transfer progress overlay (left-bottom).
    // 替代原 Toast 不打擾 stream，整個 transfer 期間持續顯示 0-100%.
    private TextView fileTransferOverlayView;
    private final java.util.concurrent.atomic.AtomicReference<Runnable> fileTransferHideTask =
            new java.util.concurrent.atomic.AtomicReference<>(null);
    private TextView performanceOverlayView;

    private MediaCodecDecoderRenderer decoderRenderer;
    private boolean reportedCrash;

    private WifiManager.WifiLock highPerfWifiLock;
    private WifiManager.WifiLock lowLatencyWifiLock;

    private boolean connectedToUsbDriverService = false;

    // §QUEST-VR：vr_emulate 沒開時這些維持預設，一般串流不讀它們。
    private boolean vrEmulate;
    private int vrEyeWidth = 1440;
    private int vrEyeHeight = 1440;
    private int vrHz = 72;
    private String vrMotion = "sine";
    private VrLaunchConfig vrLaunch;
    private long vrEmuHandle;
    // §QXR-VIDEO-TEST：開發用。解碼輸出改進 native 的 AImageReader→Vulkan，驗 Quest 的影像路徑；一般串流永遠是 0。
    private long qxrVideoTestHandle;
    private boolean vrEmuReleased;
    // §VITURE-WARP：解碼器改接到 SurfaceTexture。釋放必須在 conn.stop() 之後，不能跟 native handle 一起拆。
    private GlassesWarpRenderer glassesWarp;
    private final Object glassesWarpLock = new Object();
    private boolean glassesWarpReleaseOnStopThread;
    // §VITURE：只有 glasses_vr 會設。vr_emulate 維持 false，下面的組態分支才不會改到合成姿態。
    private boolean glassesVr;
    private volatile boolean vitureBeast;
    private boolean glassesBeastPausedFor2d;
    private boolean glassesWaitFor3d;
    private boolean glassesBringupStarted;
    private Thread glassesBringupThread;
    private volatile boolean glassesShutdown;
    private volatile boolean glassesPresented;
    private volatile GlassesVrSession glassesSession;
    private final Object glassesLife = new Object();
    private LinearLayout glassesControls;
    private TextView glassesStatusView;
    private static final int GLASSES_3D_HEIGHT = 1080;
    private static final long BEAST_3D_WAIT_MS = 60000L;
    // 權限等待可被 interrupt 立刻返回。bulkTransfer 最久約 1 秒，多留幾次指令的時間。
    private static final long GLASSES_BRINGUP_JOIN_MS = 8000L;
    private static final long GLASSES_READER_JOIN_MS = 1500L;
    // §VITURE：一般串流的眼鏡。glasses_vr 不開這組，避免兩條路同時 claim 同一條 USB。
    private final Object plainLife = new Object();
    private final GlassesDisplaySwitcher plainSwitcher = new GlassesDisplaySwitcher();
    private volatile VitureGlasses plainGlasses;
    private volatile boolean plainReleaseRequested;
    private volatile boolean plainDetached;
    private boolean plainOpenStarted;
    private boolean sbsAutoAttempted;
    private GlassesWearMonitor plainWearMonitor;
    private HandlerThread plainCommandThread;
    private Handler plainCommandHandler;
    private LinearLayout plainGlassesBar;
    private Button plainModeButton;
    private Button plainVolumeDownButton;
    private Button plainVolumeUpButton;
    private TextView plainModeResult;
    private TextView plainBeastModeNote;
    private TextView plainVolumeResult;
    private TextView glassesRemovedNotice;
    private int modeWatchBaselineWidth;
    private int modeWatchTarget;
    private long modeWatchStartedElapsed;
    private VitureGlasses.CommandResult modeWatchResult;
    private boolean modeWatchHintShown;
    private final Runnable modeWatchRunnable = new Runnable() {
        @Override
        public void run() {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            int width = externalPhysicalWidth();
            int baseline = modeWatchBaselineWidth;
            long elapsed = SystemClock.elapsedRealtime() - modeWatchStartedElapsed;
            if (baseline > 0 && width > 0 && width != baseline) {
                modeWatchBaselineWidth = 0;
                VitureUsb.logI("display width " + baseline + " -> " + width);
                return;
            }
            VitureGlasses.CommandResult result = modeWatchResult;
            if (result != null && result.ok && modeWatchTarget == VitureV2Protocol.DISPLAY_3D_SBS_60) {
                GlassesDisplaySwitcher.logModeTimeout(result, elapsed);
            } else {
                VitureUsb.logI("display width unchanged after " + elapsed + " ms mode=0x"
                        + Integer.toHexString(modeWatchTarget & 0xFF));
            }
            showGlassesPressModeHint();
        }
    };
    // §VITURE：查詢得到的音量範圍未知。送出去的值先夾在這裡，避免一次寫進規格沒寫的大數字。
    private static final int PLAIN_VOLUME_MIN = 0;
    private static final int PLAIN_VOLUME_MAX = 15;
    private final GlassesWearMonitor.Listener glassesWearListener = new GlassesWearMonitor.Listener() {
        @Override
        public void onWornOffSustained() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    showGlassesRemovedNotice();
                }
            });
        }

        @Override
        public void onWorn() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    hideGlassesRemovedNotice();
                }
            });
        }

        @Override
        public void onPollStopped() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    // §VITURE：輪詢停了代表眼鏡可能已拔。按鈕失效，串流繼續。
                    disablePlainGlassesControls();
                }
            });
        }
    };
    private ServiceConnection usbDriverServiceConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName componentName, IBinder iBinder) {
            UsbDriverService.UsbDriverBinder binder = (UsbDriverService.UsbDriverBinder) iBinder;
            binder.setListener(controllerHandler);
            binder.setStateListener(Game.this);
            binder.start();
            connectedToUsbDriverService = true;
        }

        @Override
        public void onServiceDisconnected(ComponentName componentName) {
            connectedToUsbDriverService = false;
        }
    };

    public static final String EXTRA_HOST = "Host";
    public static final String EXTRA_PORT = "Port";
    public static final String EXTRA_HTTPS_PORT = "HttpsPort";
    public static final String EXTRA_APP_NAME = "AppName";
    public static final String EXTRA_APP_ID = "AppId";
    public static final String EXTRA_UNIQUEID = "UniqueId";
    public static final String EXTRA_PC_UUID = "UUID";
    public static final String EXTRA_PC_NAME = "PcName";
    public static final String EXTRA_APP_HDR = "HDR";
    public static final String EXTRA_SERVER_CERT = "ServerCert";
    public static final String EXTRA_STUN_ADDRESS = "StunAddress"; // VipleStream: STUN endpoint for relay
    public static final String EXTRA_STUN_PUNCH_PORT = "StunPunchPort"; // §REVIEW-1104：STUN 回報的實際控制埠
    // VipleStream §M.parity W1 (2026-05-20). Host codec capability bit
    // field (SCM_* flags from Sunshine /serverinfo). 0 means the host
    // did not advertise the field (vanilla GFE / older Sunshine) — in
    // that case we don't AND the supportedVideoFormats so we match
    // existing upstream behaviour.
    public static final String EXTRA_SERVER_CODEC_MODE_SUPPORT = "ServerCodecModeSupport";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // 串流活動建立，設定串流進行中旗標
        sIsStreaming = true;

        // §REVIEW-1103：relay wss 的自簽憑證釘選需要儲存位置
        com.limelight.relay.RelayTls.init(this);

        UiHelper.setLocale(this);

        // We don't want a title bar
        requestWindowFeature(Window.FEATURE_NO_TITLE);

        // Full-screen
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);

        // If we're going to use immersive mode, we want to have
        // the entire screen
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                        View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                        View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN);

        // Listen for UI visibility events
        getWindow().getDecorView().setOnSystemUiVisibilityChangeListener(this);

        // Change volume button behavior
        setVolumeControlStream(AudioManager.STREAM_MUSIC);

        // Inflate the content
        setContentView(R.layout.activity_game);

        // Start the spinner
        spinner = SpinnerDialog.displayDialog(this, getResources().getString(R.string.conn_establishing_title),
                getResources().getString(R.string.conn_establishing_msg), true);

        // Read the stream preferences
        prefConfig = PreferenceConfiguration.readPreferences(this);
        tombstonePrefs = Game.this.getSharedPreferences("DecoderTombstone", 0);

        // VipleStream CLI mode: override prefConfig with intent extras from CliStreamActivity
        applyCliOverrides(prefConfig, getIntent());

        // §QUEST-VR：只有帶 vr_emulate 才改偏好。沒有這個 extra 時，後面的組態跟以前相同。
        // §VITURE：glasses_vr 走同一段組態，姿態來源改成 IMU。兩邊都帶時以眼鏡為準。
        if (getIntent().getBooleanExtra("glasses_vr", false)) {
            applyGlassesVrPrefs(prefConfig, getIntent());
        } else if (getIntent().getBooleanExtra("vr_emulate", false)) {
            applyVrEmulatePrefs(prefConfig, getIntent());
        }

        // Enter landscape unless we're on a square screen
        setPreferredOrientationForCurrentDisplay();

        if (prefConfig.stretchVideo || shouldIgnoreInsetsForResolution(prefConfig.width, prefConfig.height)) {
            // Allow the activity to layout under notches if the fill-screen option
            // was turned on by the user or it's a full-screen native resolution
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                getWindow().getAttributes().layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            }
            else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                getWindow().getAttributes().layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            }
        }

        // Listen for non-touch events on the game surface
        streamView = findViewById(R.id.surfaceView);
        videoView = streamView;
        streamView.setOnGenericMotionListener(this);
        streamView.setOnKeyListener(this);
        streamView.setInputCallbacks(this);

        // Listen for touch events on the background touch view to enable trackpad mode
        // to work on areas outside of the StreamView itself. We use a separate View
        // for this rather than just handling it at the Activity level, because that
        // allows proper touch splitting, which the OSC relies upon.
        View backgroundTouchView = findViewById(R.id.backgroundTouchView);
        backgroundTouchView.setOnTouchListener(this);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Request unbuffered input event dispatching for all input classes we handle here.
            // Without this, input events are buffered to be delivered in lock-step with VBlank,
            // artificially increasing input latency while streaming.
            streamView.requestUnbufferedDispatch(
                    InputDevice.SOURCE_CLASS_BUTTON | // Keyboards
                    InputDevice.SOURCE_CLASS_JOYSTICK | // Gamepads
                    InputDevice.SOURCE_CLASS_POINTER | // Touchscreens and mice (w/o pointer capture)
                    InputDevice.SOURCE_CLASS_POSITION | // Touchpads
                    InputDevice.SOURCE_CLASS_TRACKBALL // Mice (pointer capture)
            );
            backgroundTouchView.requestUnbufferedDispatch(
                    InputDevice.SOURCE_CLASS_BUTTON | // Keyboards
                    InputDevice.SOURCE_CLASS_JOYSTICK | // Gamepads
                    InputDevice.SOURCE_CLASS_POINTER | // Touchscreens and mice (w/o pointer capture)
                    InputDevice.SOURCE_CLASS_POSITION | // Touchpads
                    InputDevice.SOURCE_CLASS_TRACKBALL // Mice (pointer capture)
            );
        }

        notificationOverlayView = findViewById(R.id.notificationOverlay);

        // VipleStream §N.5 — file transfer overlay (left-bottom)
        fileTransferOverlayView = findViewById(R.id.fileTransferOverlay);
        // VipleStream §M.parity U6 (2026-05-20): tap the overlay to cancel
        // the in-flight transfer (mobile-friendly equivalent of the PC
        // moonlight-qt Ctrl+Alt+Shift+T hotkey, v1.4.103). cancelCurrent()
        // is already idempotent and safe to call when no transfer is
        // active.
        if (fileTransferOverlayView != null) {
            fileTransferOverlayView.setOnClickListener(v -> promptCancelFileTransfer());
        }

        performanceOverlayView = findViewById(R.id.performanceOverlay);

        inputCaptureProvider = InputCaptureManager.getInputCaptureProvider(this, this);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            streamView.setOnCapturedPointerListener(new View.OnCapturedPointerListener() {
                @Override
                public boolean onCapturedPointer(View view, MotionEvent motionEvent) {
                    return handleMotionEvent(view, motionEvent);
                }
            });
        }

        // Warn the user if they're on a metered connection
        ConnectivityManager connMgr = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        // §QUEST-2D：getSystemService 允許回 null，直接 isActiveNetworkMetered 會在開串流時 NPE。
        // 有服務時條件與原本相同。
        if (connMgr != null && connMgr.isActiveNetworkMetered()) {
            displayTransientMessage(getResources().getString(R.string.conn_metered));
        }

        // Make sure Wi-Fi is fully powered up
        WifiManager wifiMgr = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        // §QUEST-2D：沒有 Wi-Fi 服務時這裡是 null。下面的 try 只接 SecurityException，接不到 NPE。
        if (wifiMgr != null) {
            try {
                highPerfWifiLock = wifiMgr.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "Moonlight High Perf Lock");
                highPerfWifiLock.setReferenceCounted(false);
                highPerfWifiLock.acquire();

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    lowLatencyWifiLock = wifiMgr.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "Moonlight Low Latency Lock");
                    lowLatencyWifiLock.setReferenceCounted(false);
                    lowLatencyWifiLock.acquire();
                }
            } catch (SecurityException e) {
                // Some Samsung Galaxy S10+/S10e devices throw a SecurityException from
                // WifiLock.acquire() even though we have android.permission.WAKE_LOCK in our manifest.
                e.printStackTrace();
            }
        }
        else {
            LimeLog.warning("[VIPLE-QUEST-2D] WifiManager is null; skip wifi lock");
        }

        appName = Game.this.getIntent().getStringExtra(EXTRA_APP_NAME);
        pcName = Game.this.getIntent().getStringExtra(EXTRA_PC_NAME);

        String host = Game.this.getIntent().getStringExtra(EXTRA_HOST);
        int port = Game.this.getIntent().getIntExtra(EXTRA_PORT, NvHTTP.DEFAULT_HTTP_PORT);
        int httpsPort = Game.this.getIntent().getIntExtra(EXTRA_HTTPS_PORT, 0); // 0 is treated as unknown
        int appId = Game.this.getIntent().getIntExtra(EXTRA_APP_ID, StreamConfiguration.INVALID_APP_ID);
        String uniqueId = Game.this.getIntent().getStringExtra(EXTRA_UNIQUEID);
        boolean appSupportsHdr = Game.this.getIntent().getBooleanExtra(EXTRA_APP_HDR, false);
        byte[] derCertData = Game.this.getIntent().getByteArrayExtra(EXTRA_SERVER_CERT);

        // VipleStream §N — 記下 host info 給 FileTransferClient 用（連線建好後在
        // connectionStarted 內 instantiate）
        xferHost = host;
        xferHttpsPort = httpsPort;

        app = new NvApp(appName != null ? appName : "app", appId, appSupportsHdr);

        X509Certificate serverCert = null;
        try {
            if (derCertData != null) {
                serverCert = (X509Certificate) CertificateFactory.getInstance("X.509")
                        .generateCertificate(new ByteArrayInputStream(derCertData));
            }
        } catch (CertificateException e) {
            e.printStackTrace();
        }
        xferServerCert = serverCert;  // VipleStream §N — 給 FileTransferClient 認證用

        if (appId == StreamConfiguration.INVALID_APP_ID) {
            finish();
            return;
        }

        // Initialize the MediaCodec helper before creating the decoder
        GlPreferences glPrefs = GlPreferences.readPreferences(this);
        MediaCodecHelper.initialize(this, glPrefs.glRenderer);

        // §EXT-DISPLAY：HDR 要看真正出畫面的那台。模式沒定之前不能先查手機內建螢幕。
        maybeStartExternalDisplay();
        if (externalDisplayActive && prefConfig.enableFruc) {
            // 換輸出 Surface 時補幀還抓著解碼器的輸出，切過去會失敗並把串流拆掉。
            prefConfig.enableFruc = false;
            LimeLog.info("[VIPLE-EXTDISP] FRUC disabled; swapping the output surface does not support frame interpolation");
        }

        // Check if the user has enabled HDR
        boolean willStreamHdr = false;
        if (prefConfig.enableHdr) {
            // Start our HDR checklist
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                Display display;
                if (externalDisplayActive && externalDisplay != null) {
                    display = externalDisplay;
                }
                else {
                    display = getWindowManager().getDefaultDisplay();
                }
                Display.HdrCapabilities hdrCaps = display != null ? display.getHdrCapabilities() : null;

                // We must now ensure our display is compatible with HDR10
                if (hdrCaps != null) {
                    // getHdrCapabilities() returns null on Lenovo Lenovo Mirage Solo (vega), Android 8.0
                    for (int hdrType : hdrCaps.getSupportedHdrTypes()) {
                        if (hdrType == Display.HdrCapabilities.HDR_TYPE_HDR10) {
                            willStreamHdr = true;
                            break;
                        }
                    }
                }

                if (externalDisplayActive) {
                    int hdrDisplayId = display != null ? display.getDisplayId() : Display.INVALID_DISPLAY;
                    LimeLog.info("[VIPLE-EXTDISP] hdr displayId=" + hdrDisplayId + " hdr10=" + willStreamHdr);
                }

                if (!willStreamHdr) {
                    // Nope, no HDR for us :(
                    Toast.makeText(this, "Display does not support HDR10", Toast.LENGTH_LONG).show();
                }
            }
            else {
                Toast.makeText(this, "HDR requires Android 7.0 or later", Toast.LENGTH_LONG).show();
            }
        }

        // Check if the user has enabled performance stats overlay
        if (prefConfig.enablePerfOverlay) {
            performanceOverlayView.setVisibility(View.VISIBLE);
        }

        decoderRenderer = new MediaCodecDecoderRenderer(
                this,
                prefConfig,
                new CrashListener() {
                    @Override
                    public void notifyCrash(Exception e) {
                        // The MediaCodec instance is going down due to a crash
                        // let's tell the user something when they open the app again

                        // We must use commit because the app will crash when we return from this function
                        tombstonePrefs.edit().putInt("CrashCount", tombstonePrefs.getInt("CrashCount", 0) + 1).commit();
                        reportedCrash = true;
                    }
                },
                tombstonePrefs.getInt("CrashCount", 0),
                connMgr.isActiveNetworkMetered(),
                willStreamHdr,
                glPrefs.glRenderer,
                this);
        if (externalDisplayActive || glassesVr) {
            // 熱路徑只在這面旗標為真時才進鎖。要在任何 setOutputSurface 與解碼執行緒之前設上，串流結束才在 cleanup 清掉。
            decoderRenderer.armOutputSurfaceSwap();
        }

        // Don't stream HDR if the decoder can't support it
        boolean pyroWaveSupported = (prefConfig.videoFormat == PreferenceConfiguration.FormatOption.FORCE_PYROWAVE && MoonBridge.isPyroWaveSupported());
        if (willStreamHdr && !decoderRenderer.isHevcMain10Hdr10Supported() && !decoderRenderer.isAv1Main10Supported() && !pyroWaveSupported) {
            willStreamHdr = false;
            Toast.makeText(this, "Decoder does not support HDR10 profile", Toast.LENGTH_LONG).show();
        }

        // Display a message to the user if HEVC was forced on but we still didn't find a decoder
        if (prefConfig.videoFormat == PreferenceConfiguration.FormatOption.FORCE_HEVC && !decoderRenderer.isHevcSupported()) {
            Toast.makeText(this, "No HEVC decoder found", Toast.LENGTH_LONG).show();
        }

        // Display a message to the user if AV1 was forced on but we still didn't find a decoder
        if (prefConfig.videoFormat == PreferenceConfiguration.FormatOption.FORCE_AV1 && !decoderRenderer.isAv1Supported()) {
            Toast.makeText(this, "No AV1 decoder found", Toast.LENGTH_LONG).show();
        }
        if (prefConfig.videoFormat == PreferenceConfiguration.FormatOption.FORCE_PYROWAVE && !pyroWaveSupported) {
            Toast.makeText(this, "PyroWave (Vulkan 1.1+) is not supported on this device", Toast.LENGTH_LONG).show();
        }

        // H.264 is always supported
        int supportedVideoFormats = MoonBridge.VIDEO_FORMAT_H264;
        // VipleStream PyroWave: only negotiated when explicitly selected (never in Auto)
        if (pyroWaveSupported) {
            supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_PYROWAVE_MAIN8;
            if (willStreamHdr) {
                supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_PYROWAVE_MAIN10;
            }
            if (prefConfig.enableYuv444) {
                supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_PYROWAVE_444_8;
                if (willStreamHdr) {
                    supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_PYROWAVE_444_10;
                }
            }
        }
        if (decoderRenderer.isHevcSupported()) {
            supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_H265;
            if (willStreamHdr && decoderRenderer.isHevcMain10Hdr10Supported()) {
                supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_H265_MAIN10;
            }
        }
        if (decoderRenderer.isAv1Supported()) {
            supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_AV1_MAIN8;
            if (willStreamHdr && decoderRenderer.isAv1Main10Supported()) {
                supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_AV1_MAIN10;
            }
        }
        // VipleStream §M.parity W3/W5: opt-in 4:4:4 chroma variants
        // when the user requested it AND the MediaCodec decoder
        // actually advertises the corresponding 4:4:4 profile. Mobile
        // decoders usually don't, so this is a no-op for them — the
        // codec_*_Supported() probes return false. AV1 4:4:4 has no
        // Android profile constant through API 35 so the AV1 helpers
        // hardcode false; we keep the bits wired so adding them later
        // is a pure decoder-probe change.
        if (prefConfig.enableYuv444) {
            if (decoderRenderer.isHevcSupported()) {
                if (decoderRenderer.isHevcRext8_444Supported()) {
                    supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_H265_REXT8_444;
                }
                if (willStreamHdr && decoderRenderer.isHevcRext10_444Supported()) {
                    supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_H265_REXT10_444;
                }
            }
            if (decoderRenderer.isAv1Supported()) {
                if (decoderRenderer.isAv1High8_444Supported()) {
                    supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_AV1_HIGH8_444;
                }
                if (willStreamHdr && decoderRenderer.isAv1High10_444Supported()) {
                    supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_AV1_HIGH10_444;
                }
            }
        }
        // VipleStream §M.parity W1: intersect with host-advertised
        // SCM_* bit field if Sunshine sent one. Skipped when the host
        // omitted ServerCodecModeSupport (vanilla GFE / older builds)
        // so we don't accidentally rule out every codec.
        int serverCodecModeSupport = Game.this.getIntent().getIntExtra(EXTRA_SERVER_CODEC_MODE_SUPPORT, 0);
        if (serverCodecModeSupport != 0) {
            int beforeMask = supportedVideoFormats;
            // §REVIEW-1104：將主機回報的 SCM_* 位元映射為 VIDEO_FORMAT_* 遮罩後再取交集，避免 AV1、4:4:4 與 PyroWave 格式位元因編號不同遭誤刪
            int serverFormatMask = 0;
            if ((serverCodecModeSupport & 0x00000001) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_H264;
            if ((serverCodecModeSupport & 0x00040000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_H264_HIGH8_444;
            if ((serverCodecModeSupport & 0x00000100) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_H265;
            if ((serverCodecModeSupport & 0x00000200) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_H265_MAIN10;
            if ((serverCodecModeSupport & 0x00080000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_H265_REXT8_444;
            if ((serverCodecModeSupport & 0x00100000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_H265_REXT10_444;
            if ((serverCodecModeSupport & 0x00010000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_AV1_MAIN8;
            if ((serverCodecModeSupport & 0x00020000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_AV1_MAIN10;
            if ((serverCodecModeSupport & 0x00200000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_AV1_HIGH8_444;
            if ((serverCodecModeSupport & 0x00400000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_AV1_HIGH10_444;
            if ((serverCodecModeSupport & 0x01000000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_PYROWAVE_MAIN8;
            if ((serverCodecModeSupport & 0x02000000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_PYROWAVE_MAIN10;
            if ((serverCodecModeSupport & 0x04000000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_PYROWAVE_444_8;
            if ((serverCodecModeSupport & 0x08000000) != 0) serverFormatMask |= MoonBridge.VIDEO_FORMAT_PYROWAVE_444_10;
            supportedVideoFormats &= serverFormatMask;
            if (beforeMask != supportedVideoFormats) {
                LimeLog.info("[VIPLE-PARITY] supportedVideoFormats masked by server caps 0x"
                             + Integer.toHexString(beforeMask) + " -> 0x"
                             + Integer.toHexString(supportedVideoFormats)
                             + " (SCM=0x" + Integer.toHexString(serverCodecModeSupport) + ")");
            }
        }

        if (vrEmulate) {
            // §QUEST-VR：VR profile 是 SDR 4:2:0（session.cpp 約 2534 行把 AV1、4:4:4、沒開 vr10bit 的 10-bit 拿掉）。
            // 模擬另外關掉 PyroWave。FORCE_H264 時解碼器不會接 HEVC，遮罩裡也不能留。
            supportedVideoFormats = VrStreamDefaults.retainVrCodecs(
                    supportedVideoFormats,
                    decoderRenderer.isHevcSupported(),
                    prefConfig.videoFormat);
            LimeLog.info("[VIPLE-QXR-EMU] vr codecs mask=0x" + Integer.toHexString(supportedVideoFormats));
        }

        int gamepadMask = ControllerHandler.getAttachedControllerMask(this);
        if (!prefConfig.multiController) {
            // Always set gamepad 1 present for when multi-controller is
            // disabled for games that don't properly support detection
            // of gamepads removed and replugged at runtime.
            gamepadMask = 1;
        }
        if (prefConfig.onscreenController) {
            // If we're using OSC, always set at least gamepad 1.
            gamepadMask |= 1;
        }

        // 外接模式在 HDR 之前就決定了。這裡只補監聽與手機上的觸控板；沒有外接畫面時不註冊 DisplayListener。
        if (externalDisplayActive || glassesVr) {
            registerExternalDisplayListener((DisplayManager) getSystemService(Context.DISPLAY_SERVICE));
        }
        if (externalDisplayActive) {
            showExternalTouchpadUi();
        }
        if (glassesVr && externalDisplayActive) {
            // §VITURE：觸控板那一頁要能看到回正與結束。偏好若把亮度拉到 0，按鈕會消失。
            keepGlassesScreenReadable();
        }
        if (!glassesVr && externalDisplayActive) {
            // §VITURE：沒有外接畫面就不開 USB。手機自己播的時候行為跟以前一樣。
            maybeOpenPlainGlasses();
        }

        // Set to the optimal mode for streaming
        float displayRefreshRate = prepareDisplayForRendering();
        LimeLog.info("Display refresh rate: "+displayRefreshRate);

        // If the user requested frame pacing using a capped FPS, we will need to change our
        // desired FPS setting here in accordance with the active display refresh rate.
        int roundedRefreshRate = Math.round(displayRefreshRate);
        int chosenFrameRate = prefConfig.fps;

        // VipleStream: FRUC auto-adapt FPS to display refresh rate, then halve for server.
        // Bitrate strategy (a): total stream bitrate stays at the user-configured value
        // (prefConfig.bitrate), passed unmodified to setBitrate() below. Because the server
        // is asked for half the FPS, this implicitly doubles the per-frame bitrate budget,
        // which is exactly what FRUC wants — better source frame quality means better
        // motion vectors and warps. Do NOT scale prefConfig.bitrate here without re-deriving
        // strategy (a/b/c) from docs/TODO.md §F? — see logged effective per-frame bitrate.
        if (prefConfig.enableFruc) {
            // Auto-cap to display refresh rate (FRUC will double it locally)
            int frucTargetFps = Math.min(prefConfig.fps, roundedRefreshRate);
            // Server gets half — FRUC doubles it back
            chosenFrameRate = frucTargetFps / 2;
            int perFrameKbits = chosenFrameRate > 0 ? prefConfig.bitrate / chosenFrameRate : 0;
            int vanillaPerFrameKbits = frucTargetFps > 0 ? prefConfig.bitrate / frucTargetFps : 0;
            LimeLog.info("[FRUC] Display=" + roundedRefreshRate + "Hz, target=" + frucTargetFps
                         + "FPS, requesting " + chosenFrameRate + "FPS from server (FRUC 2x)");
            LimeLog.info("[FRUC] bitrate strategy (a): total=" + prefConfig.bitrate + "kbps unchanged, "
                         + "effective per-frame=" + perFrameKbits + "kbits "
                         + "(2x vanilla " + vanillaPerFrameKbits + "kbits @ " + frucTargetFps + "FPS)");
        }
        if (prefConfig.framePacing == PreferenceConfiguration.FRAME_PACING_CAP_FPS) {
            if (prefConfig.fps >= roundedRefreshRate) {
                if (prefConfig.fps > roundedRefreshRate + 3) {
                    // Use frame drops when rendering above the screen frame rate
                    prefConfig.framePacing = PreferenceConfiguration.FRAME_PACING_BALANCED;
                    LimeLog.info("Using drop mode for FPS > Hz");
                } else if (roundedRefreshRate <= 49) {
                    // Let's avoid clearly bogus refresh rates and fall back to legacy rendering
                    prefConfig.framePacing = PreferenceConfiguration.FRAME_PACING_BALANCED;
                    LimeLog.info("Bogus refresh rate: " + roundedRefreshRate);
                }
                else {
                    // §REVIEW-1104：FRUC 已把 chosenFrameRate 砍半，cap-fps 的 −1 不可再覆寫，否則 server 不會以半幀率送流
                    if (!prefConfig.enableFruc) {
                        chosenFrameRate = roundedRefreshRate - 1;
                        LimeLog.info("Adjusting FPS target for screen to " + chosenFrameRate);
                    }
                }
            }
        }

        if (vrEmulate) {
            prefConfig.width = vrEyeWidth * 2;
            prefConfig.height = vrEyeHeight;
            prefConfig.fps = vrHz;
            chosenFrameRate = vrHz;
            boolean bitrateFromCli = getIntent().hasExtra("cli_bitrate")
                    && getIntent().getIntExtra("cli_bitrate", 0) > 0;
            if (!bitrateFromCli) {
                // §QUEST-VR：公式出自 moonlight-qt session.cpp 的 VR default（約 2196–2201 行）。
                // 模擬關掉 PyroWave，所以不用那邊的 250000／350000。上下限在共用公式裡。
                int vrDefault = VrStreamDefaults.defaultBitrateKbps(
                        prefConfig.width, prefConfig.height, prefConfig.fps);
                prefConfig.bitrate = vrDefault;
                LimeLog.info("[VIPLE-VR-SESSION] bitrate " + vrDefault + " kbps (VR default; "
                        + prefConfig.width + "x" + prefConfig.height + "@" + prefConfig.fps + ")");
            }
            vrLaunch = new VrLaunchConfig();
            vrLaunch.eyeWidth = vrEyeWidth;
            vrLaunch.eyeHeight = vrEyeHeight;
            vrLaunch.refreshHz = vrHz;
            vrLaunch.periodNs = 1000000000L / vrHz;
            vrLaunch.controller = "touch";
            vrLaunch.codecs = VrStreamDefaults.launchCodecMask(supportedVideoFormats);
            if (glassesVr) {
                applyBeastFov(vrLaunch);
            }
        }

        StreamConfiguration.Builder streamConfigBuilder = new StreamConfiguration.Builder()
                .setResolution(prefConfig.width, prefConfig.height)
                // §REVIEW-1104（使用者決策 B）：開補幀時 host 顯示器／遊戲跟實際串流幀率（半幀）對齊，與 Qt 一致——
                // 全幀時 host 從 120 張挑 60 張，間隔不均會讓補出來的動作抖，GPU 也白畫一半
                .setLaunchRefreshRate(prefConfig.enableFruc ? chosenFrameRate : prefConfig.fps)
                .setRefreshRate(chosenFrameRate)
                .setApp(app)
                .setBitrate(prefConfig.bitrate)
                .setEnableSops(prefConfig.enableSops)
                .enableLocalAudioPlayback(prefConfig.playHostAudio)
                .setMaxPacketSize(prefConfig.packetSize)
                .setRemoteConfiguration(StreamConfiguration.STREAM_CFG_AUTO) // NvConnection will perform LAN and VPN detection
                .setSupportedVideoFormats(supportedVideoFormats)
                .setAttachedGamepadMask(gamepadMask)
                .setClientRefreshRateX100((int)(displayRefreshRate * 100))
                .setAudioConfiguration(prefConfig.audioConfiguration)
                .setColorSpace(decoderRenderer.getPreferredColorSpace())
                .setColorRange(decoderRenderer.getPreferredColorRange())
                .setPersistGamepadsAfterDisconnect(!prefConfig.multiController)
                .setMpQuic(prefConfig.enableMpQuic, prefConfig.mpQuicScheduler)
                .setAutoAdjustBitrate(cliAutoAdjustBitrate != null ? cliAutoAdjustBitrate : true);
        if (vrEmulate) {
            streamConfigBuilder.setVrLaunch(vrLaunch);
        }
        StreamConfiguration config = streamConfigBuilder.build();

        // Initialize the connection
        conn = new NvConnection(getApplicationContext(),
                new ComputerDetails.AddressTuple(host, port),
                httpsPort, uniqueId, config,
                PlatformBinding.getCryptoProvider(this), serverCert);

        // VipleStream: Pass relay settings to connection context
        String pcUuid = Game.this.getIntent().getStringExtra(EXTRA_PC_UUID);
        String stunAddr = Game.this.getIntent().getStringExtra(EXTRA_STUN_ADDRESS);
        conn.setRelayConfig(prefConfig.relayUrl, prefConfig.relayPsk, pcUuid, stunAddr,
                Game.this.getIntent().getIntExtra(EXTRA_STUN_PUNCH_PORT, 0));

        controllerHandler = new ControllerHandler(this, conn, this, prefConfig);
        keyboardTranslator = new KeyboardTranslator();

        InputManager inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        // §QUEST-2D：服務缺失時不要註冊，否則開串流直接 NPE。有服務時仍是原本那一行。
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(keyboardTranslator, null);
        }
        else {
            LimeLog.warning("[VIPLE-QUEST-2D] InputManager is null; skip keyboard device listener");
        }

        // Initialize touch contexts
        // §EXT-DISPLAY：外接時手指看不到畫面對應的位置，絕對觸控沒有意義，一律走相對模式。參考 view 仍是手機上的 streamView。
        boolean useRelativeTouch = prefConfig.touchscreenTrackpad || externalDisplayActive;
        for (int i = 0; i < touchContextMap.length; i++) {
            if (!useRelativeTouch) {
                touchContextMap[i] = new AbsoluteTouchContext(conn, i, streamView);
            }
            else {
                touchContextMap[i] = new RelativeTouchContext(conn, i,
                        REFERENCE_HORIZ_RES, REFERENCE_VERT_RES,
                        streamView, prefConfig);
            }
        }

        if (prefConfig.onscreenController) {
            // create virtual onscreen controller
            virtualController = new VirtualController(controllerHandler,
                    (FrameLayout)streamView.getParent(),
                    this);
            virtualController.refreshLayout();
            virtualController.show();
        }

        if (prefConfig.usbDriver) {
            // Start the USB driver
            bindService(new Intent(this, UsbDriverService.class),
                    usbDriverServiceConnection, Service.BIND_AUTO_CREATE);
        }

        if (!decoderRenderer.isAvcSupported()) {
            if (spinner != null) {
                spinner.dismiss();
                spinner = null;
            }

            // If we can't find an AVC decoder, we can't proceed
            Dialog.displayDialog(this, getResources().getString(R.string.conn_error_title),
                    "This device or ROM doesn't support hardware accelerated H.264 playback.", true);
            return;
        }

        if (glassesVr && !QxrNative.isAvailable()) {
            if (spinner != null) {
                spinner.dismiss();
                spinner = null;
            }
            LimeLog.info("[VIPLE-VITURE] glasses_vr but viple-openxr is not loadable; ending");
            Dialog.displayDialog(this, getResources().getString(R.string.conn_error_title),
                    getResources().getString(R.string.glasses_vr_no_native), true);
            return;
        }

        if (glassesVr && !externalDisplayActive) {
            // §VITURE：沒有外接顯示器就結束。不能把雙眼並排畫面留在手機上。
            if (spinner != null) {
                spinner.dismiss();
                spinner = null;
            }
            LimeLog.info("[VIPLE-VITURE] glasses_vr requires an external display; ending without the phone surface");
            Dialog.displayDialog(this, getResources().getString(R.string.conn_error_title),
                    getResources().getString(R.string.glasses_vr_no_display), true);
            return;
        }

        if (vrEmulate && QxrNative.isAvailable()) {
            // §QUEST-VR：sink 要在 surface 回呼裡 conn.start 之前就登記好。沒有 H.264 的裝置走不到這裡。
            if (glassesVr) {
                vrEmuHandle = QxrNative.nativeImuCreate(vrHz);
                if (vrEmuHandle == 0) {
                    LimeLog.warning("[VIPLE-VITURE] nativeImuCreate failed: " + QxrNative.nativeLastError());
                }
                else {
                    LimeLog.info("[VIPLE-VITURE] nativeImuCreate handle=" + vrEmuHandle);
                }
            }
            else {
                vrEmuHandle = QxrNative.nativeEmuCreate(vrHz, vrMotion);
                if (vrEmuHandle == 0) {
                    LimeLog.warning("[VIPLE-QXR-EMU] nativeEmuCreate failed: " + QxrNative.nativeLastError());
                }
                else {
                    LimeLog.info("[VIPLE-QXR-EMU] emulation handle=" + vrEmuHandle);
                }
            }
        }

        // The connection will be started when the surface gets created
        if (glassesVr) {
            // 就算已經是 3840×1080，Presentation 也還沒建立。active 留著的話，
            // 這段時間的 display 事件會走進寬限期，可能把串流綁上 2D。
            glassesWaitFor3d = externalDisplay == null || !isGlasses3d(externalDisplay);
            if (glassesWaitFor3d) {
                // §VITURE：2D 面板先不要開 Presentation。切模式時顯示器會消失再出現，
                // 這段時間不能讓 §EXT-DISPLAY 把串流綁上 1920 的畫面。
                externalDisplay = null;
                externalChosenMode = null;
            }
            externalDisplayActive = false;
            externalDisplayId = Display.INVALID_DISPLAY;
            startGlassesBringup();
        }
        else if (externalDisplayActive) {
            // §EXT-DISPLAY：onCreate 時 window token 常常還沒綁上，這時 Presentation.show() 會 BadTokenException。
            // 等 decor 接上再 show。callback 不能先掛手機的 holder，否則連線會從手機 Surface 啟動，就切不過去。
            getWindow().getDecorView().post(this::showExternalPresentationAndStart);
        }
        else {
            videoView.getHolder().addCallback(this);
        }
    }

    private void setPreferredOrientationForCurrentDisplay() {
        // §QUEST-2D：Quest 的 2D 面板方向由系統合成器決定。要求 USER_LANDSCAPE 可能讓面板被重建，或被系統拒絕。
        // 未在實機確認。非 Quest 不進這裡，下面仍會 setRequestedOrientation。
        if (QuestSupport.isQuest(this)) {
            LimeLog.info("[VIPLE-QUEST-2D] skip setRequestedOrientation");
            return;
        }
        Display display = getWindowManager().getDefaultDisplay();

        // For semi-square displays, we use more complex logic to determine which orientation to use (if any)
        if (PreferenceConfiguration.isSquarishScreen(display)) {
            int desiredOrientation = Configuration.ORIENTATION_UNDEFINED;

            // OSC doesn't properly support portrait displays, so don't use it in portrait mode by default
            if (prefConfig.onscreenController) {
                desiredOrientation = Configuration.ORIENTATION_LANDSCAPE;
            }

            // For native resolution, we will lock the orientation to the one that matches the specified resolution
            if (PreferenceConfiguration.isNativeResolution(prefConfig.width, prefConfig.height)) {
                if (prefConfig.width > prefConfig.height) {
                    desiredOrientation = Configuration.ORIENTATION_LANDSCAPE;
                }
                else {
                    desiredOrientation = Configuration.ORIENTATION_PORTRAIT;
                }
            }

            if (desiredOrientation == Configuration.ORIENTATION_LANDSCAPE) {
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE);
            }
            else if (desiredOrientation == Configuration.ORIENTATION_PORTRAIT) {
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT);
            }
            else {
                // If we don't have a reason to lock to portrait or landscape, allow any orientation
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_USER);
            }
        }
        else {
            // For regular displays, we always request landscape
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE);
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);

        // Set requested orientation for possible new screen size
        setPreferredOrientationForCurrentDisplay();

        if (virtualController != null) {
            // Refresh layout of OSC for possible new screen size
            virtualController.refreshLayout();
        }

        // Hide on-screen overlays in PiP mode
        // §QUEST-2D：這裡只查 isInPictureInPictureMode，不會呼叫 enterPictureInPictureMode，查詢本身不丟例外。
        // Quest 的進入路徑已跳過，所以這個分支維持走不到。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if (isInPictureInPictureMode()) {
                isHidingOverlays = true;

                if (virtualController != null) {
                    virtualController.hide();
                }

                performanceOverlayView.setVisibility(View.GONE);
                notificationOverlayView.setVisibility(View.GONE);
                // VipleStream §N.5 — file transfer overlay 也 PiP hide
                if (fileTransferOverlayView != null) {
                    fileTransferOverlayView.setVisibility(View.GONE);
                }

                // Disable sensors while in PiP mode
                controllerHandler.disableSensors();

                // Update GameManager state to indicate we're in PiP (still gaming, but interruptible)
                UiHelper.notifyStreamEnteringPiP(this);
            }
            else {
                isHidingOverlays = false;

                // Restore overlays to previous state when leaving PiP

                if (virtualController != null) {
                    virtualController.show();
                }

                if (prefConfig.enablePerfOverlay) {
                    performanceOverlayView.setVisibility(View.VISIBLE);
                }

                notificationOverlayView.setVisibility(requestedNotificationOverlayVisibility);

                // §REVIEW-1104：進入 PiP 會把檔案傳輸 overlay 藏起來，離開時若還有進度文字且尚未終態就設回可見
                if (fileTransferOverlayView != null) {
                    CharSequence transferText = fileTransferOverlayView.getText();
                    String transferMessage = transferText == null ? null : transferText.toString();
                    if (transferMessage != null && transferMessage.length() > 0
                            && !isFileTransferTerminalMessage(transferMessage)) {
                        fileTransferOverlayView.setVisibility(View.VISIBLE);
                    }
                }

                // Enable sensors again after exiting PiP
                controllerHandler.enableSensors();

                // Update GameManager state to indicate we're out of PiP (gaming, non-interruptible)
                UiHelper.notifyStreamExitingPiP(this);
            }
        }
    }

    @TargetApi(Build.VERSION_CODES.O)
    private PictureInPictureParams getPictureInPictureParams(boolean autoEnter) {
        PictureInPictureParams.Builder builder =
                new PictureInPictureParams.Builder()
                        .setAspectRatio(pipAspectRatio())
                        .setSourceRectHint(new Rect(
                                streamView.getLeft(), streamView.getTop(),
                                streamView.getRight(), streamView.getBottom()));

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            builder.setAutoEnterEnabled(autoEnter);
            builder.setSeamlessResizeEnabled(true);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (appName != null) {
                builder.setTitle(appName);
                if (pcName != null) {
                    builder.setSubtitle(pcName);
                }
            }
            else if (pcName != null) {
                builder.setTitle(pcName);
            }
        }

        return builder.build();
    }

    private Rational pipAspectRatio() {
        int width = prefConfig.width;
        int height = prefConfig.height;
        // 框架只接受約 1:2.39 到 2.39:1。glasses_vr 的 3840×1080 與其他雙眼並排會直接丟例外。
        if (width > 0 && height > 0 && width * 100L > height * 239L) {
            width = 239;
            height = 100;
        } else if (width > 0 && height > 0 && height * 100L > width * 239L) {
            width = 100;
            height = 239;
        }
        return new Rational(width, height);
    }

    private boolean skipPipForExternalOrVr() {
        return externalDisplayActive || vrEmulate;
    }

    private void updatePipAutoEnter() {
        // §QUEST-2D：Android 12 以後是靠 setPictureInPictureParams 的 auto-enter 進 PiP，不是 onUserLeaveHint。
        // Quest 面板沒有手機那種子母畫面，這個呼叫可能丟 IllegalStateException，或把 2D 面板收成系統不支援的狀態。
        // 未在實機確認。非 Quest 仍從下面的 enablePip 判斷開始。
        if (QuestSupport.isQuest(this)) {
            LimeLog.info("[VIPLE-QUEST-2D] skip PiP auto-enter");
            return;
        }
        if (skipPipForExternalOrVr()) {
            // 畫面在外接顯示器或是雙眼並排（含 glasses_vr 3840×1080）時，手機視窗進子母畫面沒有對應的畫面。
            return;
        }
        if (!prefConfig.enablePip) {
            return;
        }

        boolean autoEnter = connected && suppressPipRefCount == 0;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            setPictureInPictureParams(getPictureInPictureParams(autoEnter));
        }
        else {
            autoEnterPip = autoEnter;
        }
    }

    public void setMetaKeyCaptureState(boolean enabled) {
        // This uses custom APIs present on some Samsung devices to allow capture of
        // meta key events while streaming.
        try {
            Class<?> semWindowManager = Class.forName("com.samsung.android.view.SemWindowManager");
            Method getInstanceMethod = semWindowManager.getMethod("getInstance");
            Object manager = getInstanceMethod.invoke(null);

            if (manager != null) {
                Class<?>[] parameterTypes = new Class<?>[2];
                parameterTypes[0] = ComponentName.class;
                parameterTypes[1] = boolean.class;
                Method requestMetaKeyEventMethod = semWindowManager.getDeclaredMethod("requestMetaKeyEvent", parameterTypes);
                requestMetaKeyEventMethod.invoke(manager, this.getComponentName(), enabled);
            }
            else {
                LimeLog.warning("SemWindowManager.getInstance() returned null");
            }
        } catch (ClassNotFoundException e) {
            e.printStackTrace();
        } catch (NoSuchMethodException e) {
            e.printStackTrace();
        } catch (InvocationTargetException e) {
            e.printStackTrace();
        } catch (IllegalAccessException e) {
            e.printStackTrace();
        }
    }

    @Override
    public void onUserLeaveHint() {
        super.onUserLeaveHint();

        // §QUEST-2D：離開前景不該把 Quest 面板收進子母畫面。
        // 下面的 enter 原本就只在 API 26–29；Quest 是 12L 以上，其實走不到。仍先返回，
        // 避免以後把版本判斷放寬時誤進。未在實機確認。非 Quest 仍走原本的版本判斷。
        if (QuestSupport.isQuest(this)) {
            LimeLog.info("[VIPLE-QUEST-2D] onUserLeaveHint skip PiP");
            return;
        }
        if (skipPipForExternalOrVr()) {
            return;
        }

        // PiP is only supported on Oreo and later, and we don't need to manually enter PiP on
        // Android S and later. On Android R, we will use onPictureInPictureRequested() instead.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            if (autoEnterPip) {
                try {
                    // This has thrown all sorts of weird exceptions on Samsung devices
                    // running Oreo. Just eat them and close gracefully on leave, rather
                    // than crashing.
                    enterPictureInPictureMode(getPictureInPictureParams(false));
                } catch (Exception e) {
                    e.printStackTrace();
                }
            }
        }
    }

    @Override
    @TargetApi(Build.VERSION_CODES.R)
    public boolean onPictureInPictureRequested() {
        // §QUEST-2D：不要呼叫 enterPictureInPictureMode。API 31 以上這支原本就不會 enter
        // （條件是 SDK < S），但回傳 true 表示已處理，系統就不會再代為進入。
        // 若改回傳 false，框架可能自己再進一次。未在實機確認 Horizon 是否仍會送這個 callback。
        // 非 Quest 仍走原本的 enter 判斷，最後一樣 return true。
        if (QuestSupport.isQuest(this)) {
            LimeLog.info("[VIPLE-QUEST-2D] onPictureInPictureRequested ignored");
            return true;
        }
        if (skipPipForExternalOrVr()) {
            return true;
        }
        // Enter PiP when requested unless we're on Android 12 which supports auto-enter.
        if (autoEnterPip && Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            enterPictureInPictureMode(getPictureInPictureParams(false));
        }
        return true;
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);

        // We can't guarantee the state of modifiers keys which may have
        // lifted while focus was not on us. Clear the modifier state.
        this.modifierFlags = 0;

        // With Android native pointer capture, capture is lost when focus is lost,
        // so it must be requested again when focus is regained.
        inputCaptureProvider.onWindowFocusChanged(hasFocus);
    }

    private boolean isRefreshRateEqualMatch(float refreshRate) {
        return refreshRate >= prefConfig.fps &&
                refreshRate <= prefConfig.fps + 3;
    }

    private boolean isRefreshRateGoodMatch(float refreshRate) {
        return refreshRate >= prefConfig.fps &&
                Math.round(refreshRate) % prefConfig.fps <= 3;
    }

    private boolean shouldIgnoreInsetsForResolution(int width, int height) {
        // Never ignore insets for non-native resolutions
        if (!PreferenceConfiguration.isNativeResolution(width, height)) {
            return false;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Display display = getWindowManager().getDefaultDisplay();
            for (Display.Mode candidate : display.getSupportedModes()) {
                // Ignore insets if this is an exact match for the display resolution
                if ((width == candidate.getPhysicalWidth() && height == candidate.getPhysicalHeight()) ||
                        (height == candidate.getPhysicalWidth() && width == candidate.getPhysicalHeight())) {
                    return true;
                }
            }
        }

        return false;
    }

    private boolean mayReduceRefreshRate() {
        return prefConfig.framePacing == PreferenceConfiguration.FRAME_PACING_CAP_FPS ||
                prefConfig.framePacing == PreferenceConfiguration.FRAME_PACING_MAX_SMOOTHNESS ||
                (prefConfig.framePacing == PreferenceConfiguration.FRAME_PACING_BALANCED && prefConfig.reduceRefreshRate);
    }

    private float prepareDisplayForRendering() {
        if (externalDisplayActive) {
            // §EXT-DISPLAY：fps 上限與 frame pacing 跟外接顯示器走。手機的 preferredDisplayModeId 不能動。
            return prepareExternalDisplayForRendering();
        }
        Display display = getWindowManager().getDefaultDisplay();
        WindowManager.LayoutParams windowLayoutParams = getWindow().getAttributes();
        float displayRefreshRate;

        // On M, we can explicitly set the optimal display mode
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Display.Mode bestMode = display.getMode();
            boolean isNativeResolutionStream = PreferenceConfiguration.isNativeResolution(prefConfig.width, prefConfig.height);
            boolean refreshRateIsGood = isRefreshRateGoodMatch(bestMode.getRefreshRate());
            boolean refreshRateIsEqual = isRefreshRateEqualMatch(bestMode.getRefreshRate());

            LimeLog.info("Current display mode: "+bestMode.getPhysicalWidth()+"x"+
                    bestMode.getPhysicalHeight()+"x"+bestMode.getRefreshRate());

            for (Display.Mode candidate : display.getSupportedModes()) {
                boolean refreshRateReduced = candidate.getRefreshRate() < bestMode.getRefreshRate();
                boolean resolutionReduced = candidate.getPhysicalWidth() < bestMode.getPhysicalWidth() ||
                        candidate.getPhysicalHeight() < bestMode.getPhysicalHeight();
                boolean resolutionFitsStream = candidate.getPhysicalWidth() >= prefConfig.width &&
                        candidate.getPhysicalHeight() >= prefConfig.height;

                LimeLog.info("Examining display mode: "+candidate.getPhysicalWidth()+"x"+
                        candidate.getPhysicalHeight()+"x"+candidate.getRefreshRate());

                if (candidate.getPhysicalWidth() > 4096 && prefConfig.width <= 4096) {
                    // Avoid resolutions options above 4K to be safe
                    continue;
                }

                // On non-4K streams, we force the resolution to never change unless it's above
                // 60 FPS, which may require a resolution reduction due to HDMI bandwidth limitations,
                // or it's a native resolution stream.
                if (prefConfig.width < 3840 && prefConfig.fps <= 60 && !isNativeResolutionStream) {
                    if (display.getMode().getPhysicalWidth() != candidate.getPhysicalWidth() ||
                            display.getMode().getPhysicalHeight() != candidate.getPhysicalHeight()) {
                        continue;
                    }
                }

                // Make sure the resolution doesn't regress unless if it's over 60 FPS
                // where we may need to reduce resolution to achieve the desired refresh rate.
                if (resolutionReduced && !(prefConfig.fps > 60 && resolutionFitsStream)) {
                    continue;
                }

                if (mayReduceRefreshRate() && refreshRateIsEqual && !isRefreshRateEqualMatch(candidate.getRefreshRate())) {
                    // If we had an equal refresh rate and this one is not, skip it. In min latency
                    // mode, we want to always prefer the highest frame rate even though it may cause
                    // microstuttering.
                    continue;
                }
                else if (refreshRateIsGood) {
                    // We've already got a good match, so if this one isn't also good, it's not
                    // worth considering at all.
                    if (!isRefreshRateGoodMatch(candidate.getRefreshRate())) {
                        continue;
                    }

                    if (mayReduceRefreshRate()) {
                        // User asked for the lowest possible refresh rate, so don't raise it if we
                        // have a good match already
                        if (candidate.getRefreshRate() > bestMode.getRefreshRate()) {
                            continue;
                        }
                    }
                    else {
                        // User asked for the highest possible refresh rate, so don't reduce it if we
                        // have a good match already
                        if (refreshRateReduced) {
                            continue;
                        }
                    }
                }
                else if (!isRefreshRateGoodMatch(candidate.getRefreshRate())) {
                    // We didn't have a good match and this match isn't good either, so just don't
                    // reduce the refresh rate.
                    if (refreshRateReduced) {
                        continue;
                    }
                } else {
                    // We didn't have a good match and this match is good. Prefer this refresh rate
                    // even if it reduces the refresh rate. Lowering the refresh rate can be beneficial
                    // when streaming a 60 FPS stream on a 90 Hz device. We want to select 60 Hz to
                    // match the frame rate even if the active display mode is 90 Hz.
                }

                bestMode = candidate;
                refreshRateIsGood = isRefreshRateGoodMatch(candidate.getRefreshRate());
                refreshRateIsEqual = isRefreshRateEqualMatch(candidate.getRefreshRate());
            }

            LimeLog.info("Best display mode: "+bestMode.getPhysicalWidth()+"x"+
                    bestMode.getPhysicalHeight()+"x"+bestMode.getRefreshRate());

            // Only apply new window layout parameters if we've actually changed the display mode
            if (display.getMode().getModeId() != bestMode.getModeId()) {
                // If we only changed refresh rate and we're on an OS that supports Surface.setFrameRate()
                // use that instead of using preferredDisplayModeId to avoid the possibility of triggering
                // bugs that can cause the system to switch from 4K60 to 4K24 on Chromecast 4K.
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
                        display.getMode().getPhysicalWidth() != bestMode.getPhysicalWidth() ||
                        display.getMode().getPhysicalHeight() != bestMode.getPhysicalHeight()) {
                    // Apply the display mode change
                    // §QUEST-2D：Quest 也留著這兩行。preferredDisplayModeId 只是提示，
                    // Window.setAttributes 不會因為合成器拒絕該模式而丟例外。未在實機確認是否真的切換。
                    // 看既有的 "Best display mode" log。
                    windowLayoutParams.preferredDisplayModeId = bestMode.getModeId();
                    getWindow().setAttributes(windowLayoutParams);
                }
                else {
                    LimeLog.info("Using setFrameRate() instead of preferredDisplayModeId due to matching resolution");
                }
            }
            else {
                LimeLog.info("Current display mode is already the best display mode");
            }

            displayRefreshRate = bestMode.getRefreshRate();
        }
        // On L, we can at least tell the OS that we want a refresh rate
        else {
            float bestRefreshRate = display.getRefreshRate();
            for (float candidate : display.getSupportedRefreshRates()) {
                LimeLog.info("Examining refresh rate: "+candidate);

                if (candidate > bestRefreshRate) {
                    // Ensure the frame rate stays around 60 Hz for <= 60 FPS streams
                    if (prefConfig.fps <= 60) {
                        if (candidate >= 63) {
                            continue;
                        }
                    }

                    bestRefreshRate = candidate;
                }
            }

            LimeLog.info("Selected refresh rate: "+bestRefreshRate);
            windowLayoutParams.preferredRefreshRate = bestRefreshRate;
            displayRefreshRate = bestRefreshRate;

            // Apply the refresh rate change
            getWindow().setAttributes(windowLayoutParams);
        }

        applyPhoneStreamLayout();

        // Set the desired refresh rate that will get passed into setFrameRate() later
        desiredRefreshRate = displayRefreshRate;

        if (getPackageManager().hasSystemFeature(PackageManager.FEATURE_TELEVISION) ||
                getPackageManager().hasSystemFeature(PackageManager.FEATURE_LEANBACK)) {
            // TVs may take a few moments to switch refresh rates, and we can probably assume
            // it will be eventually activated.
            // TODO: Improve this
            return displayRefreshRate;
        }
        else {
            // Use the lower of the current refresh rate and the selected refresh rate.
            // The preferred refresh rate may not actually be applied (ex: Battery Saver mode).
            return Math.min(getWindowManager().getDefaultDisplay().getRefreshRate(), displayRefreshRate);
        }
    }

    // §EXT-DISPLAY：vr_emulate 要到後面才寫進 prefConfig，但單眼尺寸這時候已經算好。模式必須跟最終串流尺寸走。
    private int externalStreamWidth() {
        if (vrEmulate) {
            return vrEyeWidth * 2;
        }
        return prefConfig.width;
    }

    private int externalStreamHeight() {
        if (vrEmulate) {
            return vrEyeHeight;
        }
        return prefConfig.height;
    }

    private int externalStreamFps() {
        if (vrEmulate) {
            return vrHz;
        }
        return prefConfig.fps;
    }

    // DeX、Chromebook、桌面模式會把 Activity 放在非 0 的顯示器上。視窗還沒掛上時 getDisplay() 可能是 null。
    private int windowActivityDisplayId() {
        Display display = getWindowManager().getDefaultDisplay();
        if (display == null) {
            return Display.DEFAULT_DISPLAY;
        }
        return display.getDisplayId();
    }

    private int attachedActivityDisplayId() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Display display = getDisplay();
            if (display != null) {
                return display.getDisplayId();
            }
        }
        return Display.INVALID_DISPLAY;
    }

    private int currentActivityDisplayId() {
        int attached = attachedActivityDisplayId();
        if (attached != Display.INVALID_DISPLAY) {
            return attached;
        }
        return windowActivityDisplayId();
    }

    private void logActivityDisplayMismatch() {
        int attached = attachedActivityDisplayId();
        int window = windowActivityDisplayId();
        if (attached != Display.INVALID_DISPLAY && attached != window) {
            LimeLog.info("[VIPLE-EXTDISP] activity display mismatch getDisplay=" + attached
                    + " defaultDisplay=" + window);
        }
    }

    private boolean vitureGlassesUsbPresent() {
        try {
            UsbManager usb = (UsbManager) getSystemService(Context.USB_SERVICE);
            // findGlassesDevice 只看裝置清單，不需要 USB 權限，也不開啟裝置。
            return VitureUsb.findGlassesDevice(usb) != null;
        } catch (RuntimeException error) {
            // 判斷失敗就當成沒接眼鏡，畫面留在這台裝置，不要把開串流程打掛。
            VitureUsb.logW("glasses check for external display failed", error);
            return false;
        }
    }

    private void maybeStartExternalDisplay() {
        noteVitureBeast();
        boolean glassesUsb = false;
        if (!prefConfig.externalDisplay && !glassesVr && prefConfig.externalDisplayAutoGlasses) {
            // 偏好與 glasses_vr 都沒開才掃。結果要在 HDR 之前用到，所以這次不能丟到背景。
            glassesUsb = vitureGlassesUsbPresent();
        }
        boolean want = prefConfig.externalDisplay || glassesUsb || glassesVr;
        int activityDisplayId = currentActivityDisplayId();
        int alsoExcludedDisplayId = windowActivityDisplayId();
        logActivityDisplayMismatch();
        if (!want) {
            LimeLog.info("[VIPLE-EXTDISP] external mode off preference=" + prefConfig.externalDisplay
                    + " glassesUsb=" + glassesUsb
                    + " activityDisplayId=" + activityDisplayId);
            return;
        }
        LimeLog.info("[VIPLE-EXTDISP] external mode requested preference=" + prefConfig.externalDisplay
                + " glassesUsb=" + glassesUsb
                + " glassesVr=" + glassesVr
                + " activityDisplayId=" + activityDisplayId);
        DisplayManager displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        ExternalDisplayHelper.Candidate candidate = ExternalDisplayHelper.find(
                displayManager, externalStreamWidth(), externalStreamHeight(), externalStreamFps(),
                activityDisplayId, alsoExcludedDisplayId);
        if (candidate == null) {
            return;
        }
        externalDisplay = candidate.display;
        externalChosenMode = candidate.chosen;
        externalDisplayId = candidate.display.getDisplayId();
        externalDisplayActive = true;
        LimeLog.info("[VIPLE-EXTDISP] external mode active displayId=" + externalDisplayId
                + " activityDisplayId=" + activityDisplayId);
    }

    private float prepareExternalDisplayForRendering() {
        int streamWidth = externalStreamWidth();
        int streamHeight = externalStreamHeight();
        int streamFps = externalStreamFps();
        Display.Mode mode = ExternalDisplayHelper.pickMode(
                externalDisplay.getSupportedModes(), streamWidth, streamHeight, streamFps);
        externalChosenMode = mode;
        float displayRefreshRate = mode != null ? mode.getRefreshRate() : externalDisplay.getRefreshRate();
        desiredRefreshRate = displayRefreshRate;
        // 不跟手機更新率取較小值。手機模式沒被切換，那個數字不能拿來當眼鏡的 fps 上限。
        LimeLog.info("[VIPLE-EXTDISP] using external refresh " + displayRefreshRate + " for fps cap and frame pacing");
        return displayRefreshRate;
    }

    private void showExternalPresentationAndStart() {
        if (isFinishing() || isDestroyed() || externalDisplayLossHandled || externalDisplayAwaiting || !externalDisplayActive) {
            return;
        }
        if (externalPresentation != null || attemptedConnection) {
            // DecorView.post 可能比 DisplayListener 的重連還晚。已經有 Presentation 或已經開連就不能再蓋一層。
            return;
        }
        applyPhoneTouchpadAspect();
        try {
            externalPresentation = new StreamPresentation(this, externalDisplay, externalChosenMode, this::onExternalDisplayLost);
            externalPresentation.setOnDismissListener(dialog -> onExternalPresentationDismissed());
            externalPresentation.show();
            StreamView presented = externalPresentation.getStreamView();
            if (presented == null) {
                throw new IllegalStateException("missing stream view");
            }
            videoView = presented;
            applyExternalVideoLayout();
            videoView.getHolder().addCallback(this);
            LimeLog.info("[VIPLE-EXTDISP] presenting on displayId=" + externalDisplayId);
            refreshBeast2dHint();
        } catch (RuntimeException e) {
            // 串流組態已經依外接更新率算過。這裡只能改畫回手機，fps 上限不會重算。
            LimeLog.warning("[VIPLE-EXTDISP] presentation failed: " + e);
            StreamPresentation failed = externalPresentation;
            externalPresentation = null;
            dismissOwnedExternalPresentation(failed);
            if (externalDisplayAwaiting) {
                // §EXT-DISPLAY：show 途中螢幕被拆掉時，寬限期已經在等下一次接上，不能改從手機 Surface 啟動。
                externalReconnectPending = false;
                return;
            }
            if (glassesVr) {
                // §VITURE：外接畫面開不起來就結束，不要改把雙眼並排畫到手機。
                externalDisplayActive = false;
                externalDisplay = null;
                externalChosenMode = null;
                externalDisplayId = Display.INVALID_DISPLAY;
                videoView = streamView;
                LimeLog.info("[VIPLE-VITURE] presentation failed; ending without the phone surface");
                Dialog.displayDialog(this, getResources().getString(R.string.conn_error_title),
                        getResources().getString(R.string.glasses_vr_no_display), true);
                return;
            }
            externalDisplayActive = false;
            externalDisplay = null;
            externalChosenMode = null;
            externalDisplayId = Display.INVALID_DISPLAY;
            videoView = streamView;
            hideExternalTouchpadUi();
            applyPhoneStreamLayout();
            videoView.getHolder().addCallback(this);
        }
    }

    private void applyPhoneStreamLayout() {
        // Marshmallow 之前不能指定 4K 模式，長寬比已經一致時改用固定緩衝區大小。
        boolean aspectRatioMatch = false;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            Display display = getWindowManager().getDefaultDisplay();
            Point screenSize = new Point(0, 0);
            display.getSize(screenSize);
            double screenAspectRatio = ((double) screenSize.y) / screenSize.x;
            double streamAspectRatio = ((double) prefConfig.height) / prefConfig.width;
            if (Math.abs(screenAspectRatio - streamAspectRatio) < 0.001) {
                LimeLog.info("Stream has compatible aspect ratio with output display");
                aspectRatioMatch = true;
            }
        }
        if (prefConfig.stretchVideo || aspectRatioMatch) {
            streamView.getHolder().setFixedSize(prefConfig.width, prefConfig.height);
        } else {
            streamView.setDesiredAspectRatio((double) prefConfig.width / (double) prefConfig.height);
        }
    }

    private void applyPhoneTouchpadAspect() {
        int width = externalStreamWidth();
        int height = externalStreamHeight();
        if (streamView == null || width <= 0 || height <= 0) {
            return;
        }
        // 手機這張 view 要跟外接畫面同一長寬比，絕對座標才不會把手機全螢幕線性拉到影像上。
        // glasses_vr 的 3840×1080 也走 externalStreamWidth/Height。
        streamView.setDesiredAspectRatio((double) width / (double) height);
        streamView.requestLayout();
    }

    private void applyExternalVideoLayout() {
        int streamWidth = externalStreamWidth();
        int streamHeight = externalStreamHeight();
        // API 28 以上原本的長寬比比對不會成立。延展就固定緩衝區大小，否則只鎖長寬比，畫面比例（含雙眼並排）才不會被裁掉。
        if (prefConfig.stretchVideo) {
            videoView.getHolder().setFixedSize(streamWidth, streamHeight);
        }
        else {
            videoView.setDesiredAspectRatio((double) streamWidth / (double) streamHeight);
            // §VITURE-WARP：EGL window 要跟解碼緩衝同一尺寸。callback 之前設，才不會在 surfaceChanged 裡重建 Surface。
            if (glassesVr && prefConfig.glassesTimewarp) {
                videoView.getHolder().setFixedSize(streamWidth, streamHeight);
            }
        }
        String layout = "video layout stream=" + streamWidth + "x" + streamHeight
                + " stretch=" + prefConfig.stretchVideo
                + " mode=" + ExternalDisplayHelper.formatMode(externalChosenMode);
        if (glassesVr) {
            VitureUsb.logI(layout);
        } else {
            LimeLog.info("[VIPLE-EXTDISP] " + layout);
        }
    }

    private void showExternalTouchpadUi() {
        View backgroundTouchView = findViewById(R.id.backgroundTouchView);
        backgroundTouchView.setBackgroundColor(Color.BLACK);
        getWindow().getDecorView().setBackgroundColor(Color.BLACK);
        TextView hint = findViewById(R.id.externalDisplayHint);
        // §VITURE：glasses_vr 的手機頁只放眼鏡狀態、回正與結束；觸控板說明跟狀態文字會疊在同一塊（實機截圖）。
        hint.setVisibility(glassesVr ? View.GONE : View.VISIBLE);
        if (prefConfig.externalDisplayDim) {
            // 只改這個視窗。0 是最暗；-1 才是交回系統亮度。Activity 結束後系統會自己還原。
            externalTouchpadDimmed = true;
            WindowManager.LayoutParams params = getWindow().getAttributes();
            params.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_OFF;
            getWindow().setAttributes(params);
        }
        applyPhoneTouchpadAspect();
        showExternalExitButton();
    }

    // §EXT-DISPLAY：觸控板頁整個手機都在吃觸控，返回鍵在全螢幕手勢導覽下不好按。放一顆固定的結束鈕。
    // glasses_vr 的控制列已經有「結束」，不重複放。
    private Button externalExitButton;

    private void showExternalExitButton() {
        if (glassesVr) {
            return;
        }
        if (externalExitButton == null) {
            Button button = new Button(this);
            button.setText(R.string.external_display_exit);
            button.setAllCaps(false);
            button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
            button.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    LimeLog.info("[VIPLE-EXTDISP] end requested from touchpad");
                    finish();
                }
            });
            FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END);
            params.topMargin = dp(24);
            params.rightMargin = dp(16);
            params.setMarginEnd(dp(16));
            View hint = findViewById(R.id.externalDisplayHint);
            ViewGroup parent = hint == null ? null : (ViewGroup) hint.getParent();
            if (parent == null) {
                parent = findViewById(android.R.id.content);
            }
            parent.addView(button, params);
            externalExitButton = button;
        }
        externalExitButton.setVisibility(View.VISIBLE);
    }

    private void hideExternalTouchpadUi() {
        if (externalExitButton != null) {
            externalExitButton.setVisibility(View.GONE);
        }
        TextView hint = findViewById(R.id.externalDisplayHint);
        if (hint != null) {
            hint.setVisibility(View.GONE);
        }
        TextView beastHint = findViewById(R.id.beastModeHint);
        if (beastHint != null) {
            beastHint.setVisibility(View.GONE);
        }
        if (externalPresentation != null) {
            externalPresentation.setTopHint(null);
        }
        View backgroundTouchView = findViewById(R.id.backgroundTouchView);
        if (backgroundTouchView != null) {
            backgroundTouchView.setBackgroundColor(Color.TRANSPARENT);
        }
        getWindow().getDecorView().setBackgroundColor(Color.TRANSPARENT);
        // 已經交回系統亮度。計時留著的話，幾秒後會把手機畫面再調成 0。
        externalTouchpadDimmed = false;
        cancelExternalDimHintTimer();
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        getWindow().setAttributes(params);
        hidePlainGlassesBar();
    }

    private void cancelExternalDimHintTimer() {
        externalDisplayHandler.removeCallbacks(restoreExternalTouchpadDimRunnable);
    }

    // 亮度 0 時白字結果與摘下提示會消失。連續提示要重算這 3 秒，不能讓上一次先把亮度拉回去。
    private void revealExternalDimForHint() {
        if (!externalTouchpadDimmed || isFinishing() || isDestroyed()) {
            return;
        }
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        getWindow().setAttributes(params);
        externalDisplayHandler.removeCallbacks(restoreExternalTouchpadDimRunnable);
        externalDisplayHandler.postDelayed(restoreExternalTouchpadDimRunnable, EXTERNAL_DIM_HINT_MS);
    }

    private void showExternalWaitingHint() {
        TextView hint = findViewById(R.id.externalDisplayHint);
        if (hint != null) {
            hint.setText(R.string.external_display_waiting);
            hint.setVisibility(View.VISIBLE);
        }
    }

    private void restoreExternalTouchpadHint() {
        TextView hint = findViewById(R.id.externalDisplayHint);
        if (hint == null) {
            return;
        }
        int width = externalPhysicalWidth();
        if (modeWatchBaselineWidth > 0 && width > 0 && width != modeWatchBaselineWidth) {
            // §VITURE：寬度已經換了，10 秒提示不必再跳出來蓋掉觸控板說明。
            externalDisplayHandler.removeCallbacks(modeWatchRunnable);
            modeWatchBaselineWidth = 0;
            modeWatchHintShown = false;
        }
        if (modeWatchHintShown) {
            hint.setText(R.string.glasses_press_mode);
            hint.setVisibility(View.VISIBLE);
            return;
        }
        hint.setText(R.string.external_display_touchpad_hint);
        // §VITURE：glasses_vr 的手機頁只放眼鏡狀態、回正與結束，觸控板說明會跟狀態文字疊在一起。
        hint.setVisibility(glassesVr ? View.GONE : View.VISIBLE);
    }

    private void cancelExternalDisplayGraceTimer() {
        externalDisplayHandler.removeCallbacks(externalDisplayGraceRunnable);
    }

    private void logExternalModeRefreshGap() {
        if (externalDisplay == null) {
            return;
        }
        float refresh = externalChosenMode != null
                ? externalChosenMode.getRefreshRate()
                : externalDisplay.getRefreshRate();
        int streamFps = externalStreamFps();
        if (refresh < streamFps) {
            LimeLog.info("[VIPLE-EXTDISP] mode refresh " + refresh
                    + " is below stream fps " + streamFps + "; stream fps unchanged");
        }
    }

    private void registerExternalDisplayListener(DisplayManager displayManager) {
        if (displayManager == null || externalDisplayListener != null) {
            return;
        }
        externalDisplayListener = new DisplayManager.DisplayListener() {
            @Override
            public void onDisplayAdded(int displayId) {
                if (externalDisplayLossHandled || isFinishing() || isDestroyed()) {
                    return;
                }
                if (externalDisplayAwaiting) {
                    LimeLog.info("[VIPLE-EXTDISP] display added during grace id=" + displayId);
                    tryReconnectExternalDisplay();
                    return;
                }
                if (glassesVr && vitureBeast && !glassesPresented && !attemptedConnection) {
                    // §VITURE：Beast 切到 3D 會先拆掉再加回。這次 added 交給等待迴圈，不能當成開串後才插上。
                    LimeLog.info("[VIPLE-EXTDISP] beast panel added before present id=" + displayId);
                    return;
                }
                // 開串之後才插上的不接管。解碼器已經綁在手機的 Surface 上。
                if (!externalDisplayActive
                        && ExternalDisplayHelper.isPresentationCandidate(displayManager, displayId,
                        currentActivityDisplayId(), windowActivityDisplayId())) {
                    LimeLog.info("[VIPLE-EXTDISP] external display added after stream start; staying on the phone");
                }
            }

            @Override
            public void onDisplayRemoved(int displayId) {
                if (externalDisplayLossHandled || isFinishing() || isDestroyed()) {
                    return;
                }
                if (glassesVr && vitureBeast && !glassesPresented && !attemptedConnection) {
                    // §VITURE：切模式的 removed 不是拔線。還沒開始畫之前就結束的話，按了按鈕也等不到 3840。
                    LimeLog.info("[VIPLE-EXTDISP] beast panel removed before present id=" + displayId);
                    return;
                }
                if (glassesVr && vitureBeast && glassesBeastPausedFor2d) {
                    if (beast2dPanelPresent(displayId)) {
                        // §VITURE：拆掉的是別台。2D 眼鏡還在，不能把暫停當成拔線。
                        LimeLog.info("[VIPLE-EXTDISP] beast 2d pause ignored removal id=" + displayId);
                        return;
                    }
                    LimeLog.info("[VIPLE-EXTDISP] beast 2d panel removed id=" + displayId);
                    onBeast2dPanelRemoved();
                    return;
                }
                if (displayId == externalDisplayId && (externalDisplayActive || externalDisplayAwaiting)) {
                    LimeLog.info("[VIPLE-EXTDISP] onDisplayRemoved id=" + displayId);
                    onExternalDisplayLost();
                }
            }

            @Override
            public void onDisplayChanged(int displayId) {
                if (externalDisplayLossHandled || isFinishing() || isDestroyed()) {
                    return;
                }
                if (externalDisplayAwaiting) {
                    if (displayId == externalDisplayId
                            || ExternalDisplayHelper.isPresentationCandidate(displayManager, displayId,
                            currentActivityDisplayId(), windowActivityDisplayId())) {
                        LimeLog.info("[VIPLE-EXTDISP] display changed during grace id=" + displayId);
                        tryReconnectExternalDisplay();
                    }
                    return;
                }
                if (glassesVr && vitureBeast && !glassesPresented && !attemptedConnection) {
                    LimeLog.info("[VIPLE-EXTDISP] beast panel changed before present id=" + displayId);
                    return;
                }
                if (!externalDisplayActive || displayId != externalDisplayId) {
                    return;
                }
                StreamPresentation presentation = externalPresentation;
                if (presentation == null || !presentation.isShowing()) {
                    // §EXT-DISPLAY：同一個 display 沒被移除，但系統已經把 Presentation 關掉。要重開一扇。
                    LimeLog.info("[VIPLE-EXTDISP] presentation gone on display change id=" + displayId);
                    beginExternalDisplayGrace();
                    tryReconnectExternalDisplay();
                    return;
                }
                Display display = displayManager.getDisplay(displayId);
                if (display == null) {
                    return;
                }
                if (glassesVr && vitureBeast && !isGlasses3d(display)) {
                    // §VITURE：同一台沒被拔掉卻變成 1920 時，不能繼續把雙眼畫面留在上面。
                    LimeLog.info("[VIPLE-EXTDISP] beast panel changed to 2d id=" + displayId);
                    beginExternalDisplayGrace();
                    if (externalDisplayAwaiting) {
                        pauseBeastGlassesOn2d();
                    }
                    return;
                }
                externalDisplay = display;
                Display.Mode previous = externalChosenMode;
                Display.Mode mode = ExternalDisplayHelper.pickMode(
                        display.getSupportedModes(), externalStreamWidth(), externalStreamHeight(), externalStreamFps());
                externalChosenMode = mode;
                presentation.setPreferredMode(mode);
                LimeLog.info("[VIPLE-EXTDISP] display changed id=" + displayId
                        + " old=" + ExternalDisplayHelper.formatMode(previous)
                        + " new=" + ExternalDisplayHelper.formatMode(mode));
                logExternalModeRefreshGap();
                if (!glassesVr && vitureBeast) {
                    applyExternalVideoLayout();
                    refreshBeast2dHint();
                }
            }
        };
        displayManager.registerDisplayListener(externalDisplayListener, externalDisplayHandler);
    }

    private void unregisterExternalDisplayListener() {
        if (externalDisplayListener == null) {
            return;
        }
        DisplayManager displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        if (displayManager != null) {
            displayManager.unregisterDisplayListener(externalDisplayListener);
        }
        externalDisplayListener = null;
    }

    private void onExternalDisplayLost() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            runOnUiThread(this::onExternalDisplayLost);
            return;
        }
        if (externalDisplayLossHandled || isFinishing() || isDestroyed()) {
            return;
        }
        if (externalDisplayAwaiting) {
            // §EXT-DISPLAY：寬限期內又消失不要重設 8 秒，否則一直閃爍就永遠不會結束。
            if (externalReconnectPending) {
                externalReconnectGeneration++;
                externalReconnectPending = false;
                LimeLog.info("[VIPLE-EXTDISP] external display removed again during grace");
            }
            return;
        }
        if (!externalDisplayActive) {
            return;
        }
        beginExternalDisplayGrace();
    }

    private void onExternalPresentationDismissed() {
        if (externalDisplayLossHandled || isFinishing() || isDestroyed()) {
            return;
        }
        if (!externalDisplayActive && !externalDisplayAwaiting) {
            return;
        }
        // §EXT-DISPLAY：系統把 Presentation 關掉時 Surface 跟著沒有。自己 dismiss 前會先拿掉這個 listener。
        if (!externalDisplayAwaiting) {
            beginExternalDisplayGrace();
        }
        if (externalDisplayAwaiting && !externalDisplayLossHandled) {
            tryReconnectExternalDisplay();
        }
    }

    private void beginExternalDisplayGrace() {
        if (externalDisplayLossHandled || isFinishing() || isDestroyed() || !externalDisplayActive) {
            return;
        }
        externalDisplayAwaiting = true;
        externalDisplayLostUptimeMs = SystemClock.uptimeMillis();
        externalDisplayLostMode = externalChosenMode;
        LimeLog.info("[VIPLE-EXTDISP] external display removed; waiting " + EXTERNAL_DISPLAY_GRACE_MS
                + " ms old=" + ExternalDisplayHelper.formatMode(externalDisplayLostMode));
        if (attemptedConnection && !attachExternalHoldSurface()) {
            finishExternalDisplayLoss();
            return;
        }
        if (!attemptedConnection) {
            LimeLog.info("[VIPLE-EXTDISP] stream not started; waiting without parking the decoder");
        }
        // 解碼器已經離開這張 Surface（或根本還沒用它）才拆。callback 留著的話，寬限期結束後銷毀事件會把連線停掉。
        dismissExternalPresentation();
        showExternalWaitingHint();
        cancelExternalDisplayGraceTimer();
        externalDisplayHandler.postDelayed(externalDisplayGraceRunnable, EXTERNAL_DISPLAY_GRACE_MS);
        refreshBeast2dHint();
    }

    private boolean attachExternalHoldSurface() {
        GlassesWarpRenderer warp;
        synchronized (glassesWarpLock) {
            warp = glassesWarp;
        }
        if (warp != null) {
            // §VITURE-WARP：direct surface 時 setOutputSurface 會被拒絕。解碼器留在 SurfaceTexture，只拆眼鏡上的 EGL window。
            warp.detachOutput();
            LimeLog.info("[VIPLE-VITURE] warp window detached; decoder stays on the texture");
            return true;
        }
        if (externalHoldSurface != null) {
            return true;
        }
        int width = Math.max(externalStreamWidth(), 1);
        int height = Math.max(externalStreamHeight(), 1);
        // §EXT-DISPLAY：不確定「完全不 render、但輸出 Surface 已死」時解碼器會不會丟例外，所以改接到這張不顯示的暫存 Surface。
        // SurfaceTexture(false) 是 detached，不需要 GL context。
        SurfaceTexture texture = new SurfaceTexture(false);
        texture.setDefaultBufferSize(width, height);
        Surface surface = new Surface(texture);
        if (decoderRenderer == null || !decoderRenderer.setOutputSurface(surface, null)) {
            surface.release();
            texture.release();
            LimeLog.warning("[VIPLE-EXTDISP] could not move the decoder to the hold surface; ending stream");
            return false;
        }
        externalHoldTexture = texture;
        externalHoldSurface = surface;
        LimeLog.info("[VIPLE-EXTDISP] decoder parked on hold surface " + width + "x" + height);
        return true;
    }

    private void releaseExternalHoldSurface() {
        Surface surface = externalHoldSurface;
        SurfaceTexture texture = externalHoldTexture;
        externalHoldSurface = null;
        externalHoldTexture = null;
        if (surface != null) {
            surface.release();
        }
        if (texture != null) {
            texture.release();
        }
    }

    private void tryReconnectExternalDisplay() {
        if (externalReconnectRunning || !externalDisplayAwaiting || externalDisplayLossHandled
                || isFinishing() || isDestroyed()) {
            return;
        }
        if (attemptedConnection && !connecting && !connected) {
            return;
        }
        externalReconnectRunning = true;
        try {
            DisplayManager displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
            ExternalDisplayHelper.Candidate candidate = ExternalDisplayHelper.find(
                    displayManager, externalStreamWidth(), externalStreamHeight(), externalStreamFps(),
                    currentActivityDisplayId(), windowActivityDisplayId());
            if (candidate == null || (glassesVr && !isGlasses3d(candidate.display))) {
                // glasses_vr 退回最大面積時會挑到 1920×1080。3D 串流不能綁上那張 2D 面板。
                if (glassesVr && vitureBeast && candidate != null) {
                    pauseBeastGlassesOn2d();
                    LimeLog.info("[VIPLE-EXTDISP] reconnect: beast 2d panel "
                            + ExternalDisplayHelper.formatMode(candidate.chosen));
                } else {
                    LimeLog.info("[VIPLE-EXTDISP] reconnect: no usable display yet");
                }
                return;
            }
            boolean resumeFrom2d = glassesBeastPausedFor2d;
            glassesBeastPausedFor2d = false;
            if (externalPresentation != null && externalPresentation.isShowing()
                    && externalDisplay != null
                    && candidate.display.getDisplayId() == externalDisplay.getDisplayId()) {
                return;
            }
            int generation = ++externalReconnectGeneration;
            externalReconnectPending = true;
            StreamPresentation previous = externalPresentation;
            externalPresentation = null;
            dismissOwnedExternalPresentation(previous);
            if (generation != externalReconnectGeneration || !externalDisplayAwaiting || externalDisplayLossHandled) {
                externalReconnectPending = false;
                return;
            }
            externalDisplay = candidate.display;
            externalChosenMode = candidate.chosen;
            externalDisplayId = candidate.display.getDisplayId();
            if (!attemptedConnection) {
                // 還沒開串流時，新 Surface 的 surfaceCreated 會用這個值呼叫 setFrameRate。
                // 已經連上的重連改走 completeExternalReconnect，不在這裡改連線用過的更新率。
                desiredRefreshRate = externalChosenMode != null
                        ? externalChosenMode.getRefreshRate()
                        : externalDisplay.getRefreshRate();
            }
            externalDisplayActive = true;
            if (resumeFrom2d) {
                // §VITURE：暫停時把寬限期取消了。接回 3D 的這段若失敗，仍要能結束，不能一直停在暫存 Surface。
                cancelExternalDisplayGraceTimer();
                externalDisplayHandler.postDelayed(externalDisplayGraceRunnable, EXTERNAL_DISPLAY_GRACE_MS);
            }
            externalPresentation = new StreamPresentation(this, externalDisplay, externalChosenMode, this::onExternalDisplayLost);
            externalPresentation.setOnDismissListener(dialog -> onExternalPresentationDismissed());
            externalPresentation.show();
            if (generation != externalReconnectGeneration || externalPresentation == null
                    || !externalDisplayAwaiting || externalDisplayLossHandled) {
                externalReconnectPending = false;
                return;
            }
            StreamView presented = externalPresentation.getStreamView();
            if (presented == null) {
                throw new IllegalStateException("missing stream view");
            }
            videoView = presented;
            applyExternalVideoLayout();
            LimeLog.info("[VIPLE-EXTDISP] reconnect presenting on displayId=" + externalDisplayId
                    + " mode=" + ExternalDisplayHelper.formatMode(externalChosenMode));
            videoView.getHolder().addCallback(this);
        } catch (RuntimeException e) {
            LimeLog.warning("[VIPLE-EXTDISP] reconnect presentation failed: " + e);
            StreamPresentation failed = externalPresentation;
            externalPresentation = null;
            dismissOwnedExternalPresentation(failed);
            externalReconnectPending = false;
        } finally {
            externalReconnectRunning = false;
        }
    }

    private void completeExternalReconnect(SurfaceHolder holder) {
        if (!externalReconnectPending || externalDisplayLossHandled || isFinishing() || isDestroyed()) {
            return;
        }
        if (videoView == null || holder != videoView.getHolder()) {
            return;
        }
        Surface surface = holder.getSurface();
        if (surface == null || !surface.isValid()) {
            LimeLog.warning("[VIPLE-EXTDISP] reconnect surface is not valid");
            return;
        }
        GlassesWarpRenderer warp;
        synchronized (glassesWarpLock) {
            warp = glassesWarp;
        }
        if (warp != null) {
            // §VITURE-WARP：解碼器沒換輸出，不要求 IDR。視窗建不起來就無法退回直接顯示。
            if (!warp.attachOutput(surface, videoView)) {
                LimeLog.warning("[VIPLE-VITURE] warp output reattach failed; ending stream");
                externalReconnectPending = false;
                finishExternalDisplayLoss();
                return;
            }
            externalReconnectPending = false;
            externalDisplayAwaiting = false;
            cancelExternalDisplayGraceTimer();
            long elapsedMs = SystemClock.uptimeMillis() - externalDisplayLostUptimeMs;
            LimeLog.info("[VIPLE-EXTDISP] external display back after " + elapsedMs
                    + " ms old=" + ExternalDisplayHelper.formatMode(externalDisplayLostMode)
                    + " new=" + ExternalDisplayHelper.formatMode(externalChosenMode));
            logExternalModeRefreshGap();
            LimeLog.info("[VIPLE-VITURE] warp output reattached; decoder surface unchanged");
            glassesBeastPausedFor2d = false;
            restoreExternalTouchpadHint();
            refreshBeast2dHint();
            return;
        }
        if (decoderRenderer == null || !decoderRenderer.setOutputSurface(surface, holder)) {
            LimeLog.warning("[VIPLE-EXTDISP] failed to move decoder back to the external display; ending stream");
            externalReconnectPending = false;
            finishExternalDisplayLoss();
            return;
        }
        // 解碼器已經離開暫存 Surface，才可以放掉。
        releaseExternalHoldSurface();
        externalReconnectPending = false;
        externalDisplayAwaiting = false;
        cancelExternalDisplayGraceTimer();
        long elapsedMs = SystemClock.uptimeMillis() - externalDisplayLostUptimeMs;
        LimeLog.info("[VIPLE-EXTDISP] external display back after " + elapsedMs
                + " ms old=" + ExternalDisplayHelper.formatMode(externalDisplayLostMode)
                + " new=" + ExternalDisplayHelper.formatMode(externalChosenMode));
        logExternalModeRefreshGap();
        // §EXT-DISPLAY：換了輸出 Surface，舊的參考畫面對新 Surface 沒有意義，要一張 IDR 重新開始。
        MoonBridge.requestIdrFrame();
        LimeLog.info("[VIPLE-EXTDISP] requested an IDR frame after switching the output surface");
        glassesBeastPausedFor2d = false;
        restoreExternalTouchpadHint();
        refreshBeast2dHint();
    }

    private void noteExternalDisplayBackBeforeStart() {
        externalReconnectPending = false;
        externalDisplayAwaiting = false;
        cancelExternalDisplayGraceTimer();
        releaseExternalHoldSurface();
        long elapsedMs = SystemClock.uptimeMillis() - externalDisplayLostUptimeMs;
        LimeLog.info("[VIPLE-EXTDISP] external display back after " + elapsedMs
                + " ms old=" + ExternalDisplayHelper.formatMode(externalDisplayLostMode)
                + " new=" + ExternalDisplayHelper.formatMode(externalChosenMode)
                + "; stream had not started");
        logExternalModeRefreshGap();
        glassesBeastPausedFor2d = false;
        restoreExternalTouchpadHint();
        refreshBeast2dHint();
    }

    private void finishExternalDisplayLoss() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            runOnUiThread(this::finishExternalDisplayLoss);
            return;
        }
        if (externalDisplayLossHandled || isFinishing() || isDestroyed()) {
            return;
        }
        externalDisplayLossHandled = true;
        externalDisplayAwaiting = false;
        externalReconnectPending = false;
        cancelExternalDisplayGraceTimer();
        // 擋掉 connectionTerminated 的另一個錯誤對話框。結束本身仍走 stopConnection（會送 /cancel）。
        displayedFailureDialog = true;
        if (spinner != null) {
            spinner.dismiss();
            spinner = null;
        }
        hideExternalTouchpadUi();
        releaseVrEmulation();
        if (decoderRenderer != null) {
            decoderRenderer.prepareForStop();
        }
        if (connecting || connected) {
            stopConnection();
        } else {
            dismissExternalPresentation();
            releaseExternalHoldSurface();
        }
        Dialog.displayDialog(this,
                getResources().getString(R.string.title_external_display_lost),
                getResources().getString(R.string.message_external_display_lost),
                true);
    }

    private void dismissExternalPresentation() {
        externalDismissPending = false;
        StreamPresentation presentation = externalPresentation;
        externalPresentation = null;
        dismissOwnedExternalPresentation(presentation);
    }

    private void dismissOwnedExternalPresentation(StreamPresentation presentation) {
        if (presentation == null) {
            return;
        }
        StreamView presentedView = presentation.getStreamView();
        if (presentedView != null) {
            presentedView.getHolder().removeCallback(this);
        }
        try {
            // §EXT-DISPLAY：自己關掉時不能再進 onDismiss，否則會把正常結束誤判成螢幕消失。
            presentation.setOnDismissListener(null);
            presentation.dismiss();
        } catch (RuntimeException e) {
            LimeLog.warning("[VIPLE-EXTDISP] dismiss failed: " + e);
        }
    }

    @SuppressLint("InlinedApi")
    private final Runnable hideSystemUi = new Runnable() {
            @Override
            public void run() {
                // TODO: Do we want to use WindowInsetsController here on R+ instead of
                // SYSTEM_UI_FLAG_IMMERSIVE_STICKY? They seem to do the same thing as of S...

                // In multi-window mode on N+, we need to drop our layout flags or we'll
                // be drawing underneath the system UI.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && isInMultiWindowMode()) {
                    Game.this.getWindow().getDecorView().setSystemUiVisibility(
                            View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
                }
                else {
                    // Use immersive mode
                    Game.this.getWindow().getDecorView().setSystemUiVisibility(
                            View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                            View.SYSTEM_UI_FLAG_FULLSCREEN |
                            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
                }
            }
    };

    private void hideSystemUi(int delay) {
        Handler h = getWindow().getDecorView().getHandler();
        if (h != null) {
            h.removeCallbacks(hideSystemUi);
            h.postDelayed(hideSystemUi, delay);
        }
    }

    @Override
    @TargetApi(Build.VERSION_CODES.N)
    public void onMultiWindowModeChanged(boolean isInMultiWindowMode) {
        super.onMultiWindowModeChanged(isInMultiWindowMode);

        // In multi-window, we don't want to use the full-screen layout
        // flag. It will cause us to collide with the system UI.
        // This function will also be called for PiP so we can cover
        // that case here too.
        if (isInMultiWindowMode) {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            decoderRenderer.notifyVideoBackground();
        }
        else {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            decoderRenderer.notifyVideoForeground();
        }

        // Correct the system UI visibility flags
        hideSystemUi(50);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();

        // 串流活動銷毀，清除串流進行中旗標
        sIsStreaming = false;

        releaseVrEmulation();
        releaseQxrVideoTest();
        // §VITURE-WARP：stop 執行緒會在 conn.stop() 之後釋放 SurfaceTexture。這裡只拆視窗，避免跟解碼器搶。
        if (glassesWarpReleaseOnStopThread) {
            detachGlassesWarpOutput();
        }
        else {
            releaseGlassesWarp();
        }

        if (controllerHandler != null) {
            controllerHandler.destroy();
        }
        if (keyboardTranslator != null) {
            InputManager inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
            // §QUEST-2D：跟註冊時同一種 null。沒註冊過就不需要反註冊，有服務時仍是原本那一行。
            if (inputManager != null) {
                inputManager.unregisterInputDeviceListener(keyboardTranslator);
            }
        }

        if (lowLatencyWifiLock != null) {
            lowLatencyWifiLock.release();
        }
        if (highPerfWifiLock != null) {
            highPerfWifiLock.release();
        }

        if (connectedToUsbDriverService) {
            // Unbind from the discovery service
            unbindService(usbDriverServiceConnection);
        }

        // §CONSOLE-LOGIN：按了「用串流登入」會綁 ComputerManagerService。銷毀時一定要解綁，否則 ServiceConnection 留在已結束的 Activity。
        unbindConsoleLoginService();

        // §EXT-DISPLAY：有排程時由 stopConnection 的執行緒在 conn.stop() 之後才 dismiss。
        // 這裡先拆的話，解碼器可能還抓著外接 Surface。沒排程代表解碼器沒有輸出，直接拆以免視窗洩漏。
        cancelExternalDisplayGraceTimer();
        // Activity 結束後這個 Handler 還在主執行緒上。計時留著會再改已經關掉的視窗。
        cancelExternalDimHintTimer();
        externalDisplayAwaiting = false;
        externalReconnectPending = false;
        if (!externalDismissPending) {
            dismissExternalPresentation();
            releaseExternalHoldSurface();
        }
        unregisterExternalDisplayListener();

        // Destroy the capture provider
        inputCaptureProvider.destroy();
    }

    @Override
    protected void onPause() {
        if (isFinishing()) {
            // Stop any further input device notifications before we lose focus (and pointer capture)
            if (controllerHandler != null) {
                controllerHandler.stop();
            }

            // Ungrab input to prevent further input device notifications
            setInputGrabState(false);
        }

        super.onPause();
    }

    @Override
    protected void onStop() {
        super.onStop();

        SpinnerDialog.closeDialogs(this);
        Dialog.closeDialogs();
        // §CONSOLE-LOGIN：這個對話框不在 Dialog 的清單裡。onStop 結尾會 finish，這裡先拆掉以免視窗洩漏。
        if (consoleLoginDialog != null) {
            consoleLoginDialog.setOnDismissListener(null);
            consoleLoginDialog.dismiss();
            consoleLoginDialog = null;
        }

        if (virtualController != null) {
            virtualController.hide();
        }

        if (conn != null) {
            int videoFormat = decoderRenderer.getActiveVideoFormat();

            displayedFailureDialog = true;
            stopConnection();

            if (prefConfig.enableLatencyToast) {
                int averageEndToEndLat = decoderRenderer.getAverageEndToEndLatency();
                int averageDecoderLat = decoderRenderer.getAverageDecoderLatency();
                String message = null;
                if (averageEndToEndLat > 0) {
                    message = getResources().getString(R.string.conn_client_latency)+" "+averageEndToEndLat+" ms";
                    if (averageDecoderLat > 0) {
                        message += " ("+getResources().getString(R.string.conn_client_latency_hw)+" "+averageDecoderLat+" ms)";
                    }
                }
                else if (averageDecoderLat > 0) {
                    message = getResources().getString(R.string.conn_hardware_latency)+" "+averageDecoderLat+" ms";
                }

                // Add the video codec to the post-stream toast
                if (message != null) {
                    message += " [";

                    if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_H264) != 0) {
                        message += "H.264";
                    }
                    else if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_H265) != 0) {
                        message += "HEVC";
                    }
                    else if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_PYROWAVE) != 0) {
                        message += "PyroWave";
                    }
                    else if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_AV1) != 0) {
                        message += "AV1";
                    }
                    else {
                        message += "UNKNOWN";
                    }

                    if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_10BIT) != 0) {
                        message += " HDR";
                    }

                    message += "]";
                }

                if (message != null) {
                    Toast.makeText(this, message, Toast.LENGTH_LONG).show();
                }
            }

            // Clear the tombstone count if we terminated normally
            if (!reportedCrash && tombstonePrefs.getInt("CrashCount", 0) != 0) {
                tombstonePrefs.edit()
                        .putInt("CrashCount", 0)
                        .putInt("LastNotifiedCrashCount", 0)
                        .apply();
            }
        }

        finish();
    }

    private void setInputGrabState(boolean grab) {
        // Grab/ungrab the mouse cursor
        if (grab) {
            inputCaptureProvider.enableCapture();

            // Enabling capture may hide the cursor again, so
            // we will need to show it again.
            if (cursorVisible) {
                inputCaptureProvider.showCursor();
            }
        }
        else {
            inputCaptureProvider.disableCapture();
        }

        // Grab/ungrab system keyboard shortcuts
        setMetaKeyCaptureState(grab);

        grabbedInput = grab;
    }

    private final Runnable toggleGrab = new Runnable() {
        @Override
        public void run() {
            setInputGrabState(!grabbedInput);
        }
    };

    // Returns true if the key stroke was consumed
    private boolean handleSpecialKeys(int androidKeyCode, boolean down) {
        int modifierMask = 0;
        int nonModifierKeyCode = KeyEvent.KEYCODE_UNKNOWN;

        if (androidKeyCode == KeyEvent.KEYCODE_CTRL_LEFT ||
            androidKeyCode == KeyEvent.KEYCODE_CTRL_RIGHT) {
            modifierMask = KeyboardPacket.MODIFIER_CTRL;
        }
        else if (androidKeyCode == KeyEvent.KEYCODE_SHIFT_LEFT ||
                 androidKeyCode == KeyEvent.KEYCODE_SHIFT_RIGHT) {
            modifierMask = KeyboardPacket.MODIFIER_SHIFT;
        }
        else if (androidKeyCode == KeyEvent.KEYCODE_ALT_LEFT ||
                 androidKeyCode == KeyEvent.KEYCODE_ALT_RIGHT) {
            modifierMask = KeyboardPacket.MODIFIER_ALT;
        }
        else if (androidKeyCode == KeyEvent.KEYCODE_META_LEFT ||
                androidKeyCode == KeyEvent.KEYCODE_META_RIGHT) {
            modifierMask = KeyboardPacket.MODIFIER_META;
        }
        else {
            nonModifierKeyCode = androidKeyCode;
        }

        if (down) {
            this.modifierFlags |= modifierMask;
        }
        else {
            this.modifierFlags &= ~modifierMask;
        }

        // Handle the special combos on the key up
        if (waitingForAllModifiersUp || specialKeyCode != KeyEvent.KEYCODE_UNKNOWN) {
            if (specialKeyCode == androidKeyCode) {
                // If this is a key up for the special key itself, eat that because the host never saw the original key down
                return true;
            }
            else if (modifierFlags != 0) {
                // While we're waiting for modifiers to come up, eat all key downs and allow all key ups to pass
                return down;
            }
            else {
                // When all modifiers are up, perform the special action
                switch (specialKeyCode) {
                    // Toggle input grab
                    case KeyEvent.KEYCODE_Z:
                        Handler h = getWindow().getDecorView().getHandler();
                        if (h != null) {
                            h.postDelayed(toggleGrab, 250);
                        }
                        break;

                    // Quit
                    case KeyEvent.KEYCODE_Q:
                        finish();
                        break;

                    // Toggle cursor visibility
                    case KeyEvent.KEYCODE_C:
                        if (!grabbedInput) {
                            inputCaptureProvider.enableCapture();
                            grabbedInput = true;
                        }
                        cursorVisible = !cursorVisible;
                        if (cursorVisible) {
                            inputCaptureProvider.showCursor();
                        } else {
                            inputCaptureProvider.hideCursor();
                        }
                        break;

                    default:
                        break;
                }

                // Reset special key state
                specialKeyCode = KeyEvent.KEYCODE_UNKNOWN;
                waitingForAllModifiersUp = false;
            }
        }
        // Check if Ctrl+Alt+Shift is down when a non-modifier key is pressed
        else if ((modifierFlags & (KeyboardPacket.MODIFIER_CTRL | KeyboardPacket.MODIFIER_ALT | KeyboardPacket.MODIFIER_SHIFT)) ==
                (KeyboardPacket.MODIFIER_CTRL | KeyboardPacket.MODIFIER_ALT | KeyboardPacket.MODIFIER_SHIFT) &&
                (down && nonModifierKeyCode != KeyEvent.KEYCODE_UNKNOWN)) {
            switch (androidKeyCode) {
                case KeyEvent.KEYCODE_Z:
                case KeyEvent.KEYCODE_Q:
                case KeyEvent.KEYCODE_C:
                    // Remember that a special key combo was activated, so we can consume all key
                    // events until the modifiers come up
                    specialKeyCode = androidKeyCode;
                    waitingForAllModifiersUp = true;
                    return true;

                default:
                    // This isn't a special combo that we consume on the client side
                    return false;
            }
        }

        // Not a special combo
        return false;
    }

    // We cannot simply use modifierFlags for all key event processing, because
    // some IMEs will not generate real key events for pressing Shift. Instead
    // they will simply send key events with isShiftPressed() returning true,
    // and we will need to send the modifier flag ourselves.
    private byte getModifierState(KeyEvent event) {
        // Start with the global modifier state to ensure we cover the case
        // detailed in https://github.com/moonlight-stream/moonlight-android/issues/840
        byte modifier = getModifierState();
        if (event.isShiftPressed()) {
            modifier |= KeyboardPacket.MODIFIER_SHIFT;
        }
        if (event.isCtrlPressed()) {
            modifier |= KeyboardPacket.MODIFIER_CTRL;
        }
        if (event.isAltPressed()) {
            modifier |= KeyboardPacket.MODIFIER_ALT;
        }
        if (event.isMetaPressed()) {
            modifier |= KeyboardPacket.MODIFIER_META;
        }
        return modifier;
    }

    private byte getModifierState() {
        return (byte) modifierFlags;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        return handleKeyDown(event) || super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean handleKeyDown(KeyEvent event) {
        // Pass-through virtual navigation keys
        if ((event.getFlags() & KeyEvent.FLAG_VIRTUAL_HARD_KEY) != 0) {
            return false;
        }

        // Handle a synthetic back button event that some Android OS versions
        // create as a result of a right-click. This event WILL repeat if
        // the right mouse button is held down, so we ignore those.
        int eventSource = event.getSource();
        if ((eventSource == InputDevice.SOURCE_MOUSE ||
                eventSource == InputDevice.SOURCE_MOUSE_RELATIVE) &&
                event.getKeyCode() == KeyEvent.KEYCODE_BACK) {

            // Send the right mouse button event if mouse back and forward
            // are disabled. If they are enabled, handleMotionEvent() will take
            // care of this.
            if (!prefConfig.mouseNavButtons) {
                conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_RIGHT);
            }

            // Always return true, otherwise the back press will be propagated
            // up to the parent and finish the activity.
            return true;
        }

        boolean handled = false;

        if (ControllerHandler.isGameControllerDevice(event.getDevice())) {
            // Always try the controller handler first, unless it's an alphanumeric keyboard device.
            // Otherwise, controller handler will eat keyboard d-pad events.
            handled = controllerHandler.handleButtonDown(event);
        }

        // Try the keyboard handler if it wasn't handled as a game controller
        if (!handled) {
            // Let this method take duplicate key down events
            if (handleSpecialKeys(event.getKeyCode(), true)) {
                return true;
            }

            // Pass through keyboard input if we're not grabbing
            if (!grabbedInput) {
                return false;
            }

            // We'll send it as a raw key event if we have a key mapping, otherwise we'll send it
            // as UTF-8 text (if it's a printable character).
            short translated = keyboardTranslator.translate(event.getKeyCode(), event.getDeviceId());
            if (translated == 0) {
                // Make sure it has a valid Unicode representation and it's not a dead character
                // (which we don't support). If those are true, we can send it as UTF-8 text.
                //
                // NB: We need to be sure this happens before the getRepeatCount() check because
                // UTF-8 events don't auto-repeat on the host side.
                int unicodeChar = event.getUnicodeChar();
                if ((unicodeChar & KeyCharacterMap.COMBINING_ACCENT) == 0 && (unicodeChar & KeyCharacterMap.COMBINING_ACCENT_MASK) != 0) {
                    conn.sendUtf8Text(""+(char)unicodeChar);
                    return true;
                }

                return false;
            }

            // Eat repeat down events
            if (event.getRepeatCount() > 0) {
                return true;
            }

            conn.sendKeyboardInput(translated, KeyboardPacket.KEY_DOWN, getModifierState(event),
                    keyboardTranslator.hasNormalizedMapping(event.getKeyCode(), event.getDeviceId()) ? 0 : MoonBridge.SS_KBE_FLAG_NON_NORMALIZED);
        }

        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return handleKeyUp(event) || super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean handleKeyUp(KeyEvent event) {
        // Pass-through virtual navigation keys
        if ((event.getFlags() & KeyEvent.FLAG_VIRTUAL_HARD_KEY) != 0) {
            return false;
        }

        // Handle a synthetic back button event that some Android OS versions
        // create as a result of a right-click.
        int eventSource = event.getSource();
        if ((eventSource == InputDevice.SOURCE_MOUSE ||
                eventSource == InputDevice.SOURCE_MOUSE_RELATIVE) &&
                event.getKeyCode() == KeyEvent.KEYCODE_BACK) {

            // Send the right mouse button event if mouse back and forward
            // are disabled. If they are enabled, handleMotionEvent() will take
            // care of this.
            if (!prefConfig.mouseNavButtons) {
                conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_RIGHT);
            }

            // Always return true, otherwise the back press will be propagated
            // up to the parent and finish the activity.
            return true;
        }

        boolean handled = false;
        if (ControllerHandler.isGameControllerDevice(event.getDevice())) {
            // Always try the controller handler first, unless it's an alphanumeric keyboard device.
            // Otherwise, controller handler will eat keyboard d-pad events.
            handled = controllerHandler.handleButtonUp(event);
        }

        // Try the keyboard handler if it wasn't handled as a game controller
        if (!handled) {
            if (handleSpecialKeys(event.getKeyCode(), false)) {
                return true;
            }

            // Pass through keyboard input if we're not grabbing
            if (!grabbedInput) {
                return false;
            }

            short translated = keyboardTranslator.translate(event.getKeyCode(), event.getDeviceId());
            if (translated == 0) {
                // If we sent this event as UTF-8 on key down, also report that it was handled
                // when we get the key up event for it.
                int unicodeChar = event.getUnicodeChar();
                return (unicodeChar & KeyCharacterMap.COMBINING_ACCENT) == 0 && (unicodeChar & KeyCharacterMap.COMBINING_ACCENT_MASK) != 0;
            }

            conn.sendKeyboardInput(translated, KeyboardPacket.KEY_UP, getModifierState(event),
                    keyboardTranslator.hasNormalizedMapping(event.getKeyCode(), event.getDeviceId()) ? 0 : MoonBridge.SS_KBE_FLAG_NON_NORMALIZED);
        }

        return true;
    }

    @Override
    public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) {
        return handleKeyMultiple(event) || super.onKeyMultiple(keyCode, repeatCount, event);
    }

    private boolean handleKeyMultiple(KeyEvent event) {
        // We can receive keys from a software keyboard that don't correspond to any existing
        // KEYCODE value. Android will give those to us as an ACTION_MULTIPLE KeyEvent.
        //
        // Despite the fact that the Android docs say this is unused since API level 29, these
        // events are still sent as of Android 13 for the above case.
        //
        // For other cases of ACTION_MULTIPLE, we will not report those as handled so hopefully
        // they will be passed to us again as regular singular key events.
        if (event.getKeyCode() != KeyEvent.KEYCODE_UNKNOWN || event.getCharacters() == null) {
            return false;
        }

        conn.sendUtf8Text(event.getCharacters());
        return true;
    }

    private TouchContext getTouchContext(int actionIndex)
    {
        if (actionIndex < touchContextMap.length) {
            return touchContextMap[actionIndex];
        }
        else {
            return null;
        }
    }

    @Override
    public void toggleKeyboard() {
        LimeLog.info("Toggling keyboard overlay");
        InputMethodManager inputManager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        // §QUEST-2D：沒有輸入法服務時不要解參考。有服務時仍是原本的 toggleSoftInput。
        if (inputManager != null) {
            inputManager.toggleSoftInput(0, 0);
        }
    }

    private byte getLiTouchTypeFromEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                return MoonBridge.LI_TOUCH_EVENT_DOWN;

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                if ((event.getFlags() & MotionEvent.FLAG_CANCELED) != 0) {
                    return MoonBridge.LI_TOUCH_EVENT_CANCEL;
                }
                else {
                    return MoonBridge.LI_TOUCH_EVENT_UP;
                }

            case MotionEvent.ACTION_MOVE:
                return MoonBridge.LI_TOUCH_EVENT_MOVE;

            case MotionEvent.ACTION_CANCEL:
                // ACTION_CANCEL applies to *all* pointers in the gesture, so it maps to CANCEL_ALL
                // rather than CANCEL. For a single pointer cancellation, that's indicated via
                // FLAG_CANCELED on a ACTION_POINTER_UP.
                // https://developer.android.com/develop/ui/views/touch-and-input/gestures/multi
                return MoonBridge.LI_TOUCH_EVENT_CANCEL_ALL;

            case MotionEvent.ACTION_HOVER_ENTER:
            case MotionEvent.ACTION_HOVER_MOVE:
                return MoonBridge.LI_TOUCH_EVENT_HOVER;

            case MotionEvent.ACTION_HOVER_EXIT:
                return MoonBridge.LI_TOUCH_EVENT_HOVER_LEAVE;

            case MotionEvent.ACTION_BUTTON_PRESS:
            case MotionEvent.ACTION_BUTTON_RELEASE:
                return MoonBridge.LI_TOUCH_EVENT_BUTTON_ONLY;

            default:
               return -1;
        }
    }

    private float[] getStreamViewRelativeNormalizedXY(View view, MotionEvent event, int pointerIndex) {
        float normalizedX = event.getX(pointerIndex);
        float normalizedY = event.getY(pointerIndex);

        // For the containing background view, we must subtract the origin
        // of the StreamView to get video-relative coordinates.
        if (view != streamView) {
            normalizedX -= streamView.getX();
            normalizedY -= streamView.getY();
        }

        normalizedX = Math.max(normalizedX, 0.0f);
        normalizedY = Math.max(normalizedY, 0.0f);

        normalizedX = Math.min(normalizedX, streamView.getWidth());
        normalizedY = Math.min(normalizedY, streamView.getHeight());

        normalizedX /= streamView.getWidth();
        normalizedY /= streamView.getHeight();

        return new float[] { normalizedX, normalizedY };
    }

    private static float normalizeValueInRange(float value, InputDevice.MotionRange range) {
        return (value - range.getMin()) / range.getRange();
    }

    private static float getPressureOrDistance(MotionEvent event, int pointerIndex) {
        InputDevice dev = event.getDevice();
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_HOVER_ENTER:
            case MotionEvent.ACTION_HOVER_MOVE:
            case MotionEvent.ACTION_HOVER_EXIT:
                // Hover events report distance
                if (dev != null) {
                    InputDevice.MotionRange distanceRange = dev.getMotionRange(MotionEvent.AXIS_DISTANCE, event.getSource());
                    if (distanceRange != null) {
                        return normalizeValueInRange(event.getAxisValue(MotionEvent.AXIS_DISTANCE, pointerIndex), distanceRange);
                    }
                }
                return 0.0f;

            default:
                // Other events report pressure
                return event.getPressure(pointerIndex);
        }
    }

    private static short getRotationDegrees(MotionEvent event, int pointerIndex) {
        InputDevice dev = event.getDevice();
        if (dev != null) {
            if (dev.getMotionRange(MotionEvent.AXIS_ORIENTATION, event.getSource()) != null) {
                short rotationDegrees = (short) Math.toDegrees(event.getOrientation(pointerIndex));
                if (rotationDegrees < 0) {
                    rotationDegrees += 360;
                }
                return rotationDegrees;
            }
        }
        return MoonBridge.LI_ROT_UNKNOWN;
    }

    private static float[] polarToCartesian(float r, float theta) {
        return new float[] { (float)(r * Math.cos(theta)), (float)(r * Math.sin(theta)) };
    }

    private static float cartesianToR(float[] point) {
        return (float)Math.sqrt(Math.pow(point[0], 2) + Math.pow(point[1], 2));
    }

    private float[] getStreamViewNormalizedContactArea(MotionEvent event, int pointerIndex) {
        float orientation;

        // If the orientation is unknown, we'll just assume it's at a 45 degree angle and scale it by
        // X and Y scaling factors evenly.
        if (event.getDevice() == null || event.getDevice().getMotionRange(MotionEvent.AXIS_ORIENTATION, event.getSource()) == null) {
            orientation = (float)(Math.PI / 4);
        }
        else {
            orientation = event.getOrientation(pointerIndex);
        }

        float contactAreaMajor, contactAreaMinor;
        switch (event.getActionMasked()) {
            // Hover events report the tool size
            case MotionEvent.ACTION_HOVER_ENTER:
            case MotionEvent.ACTION_HOVER_MOVE:
            case MotionEvent.ACTION_HOVER_EXIT:
                contactAreaMajor = event.getToolMajor(pointerIndex);
                contactAreaMinor = event.getToolMinor(pointerIndex);
                break;

            // Other events report contact area
            default:
                contactAreaMajor = event.getTouchMajor(pointerIndex);
                contactAreaMinor = event.getTouchMinor(pointerIndex);
                break;
        }

        // The contact area major axis is parallel to the orientation, so we simply convert
        // polar to cartesian coordinates using the orientation as theta.
        float[] contactAreaMajorCartesian = polarToCartesian(contactAreaMajor, orientation);

        // The contact area minor axis is perpendicular to the contact area major axis (and thus
        // the orientation), so rotate the orientation angle by 90 degrees.
        float[] contactAreaMinorCartesian = polarToCartesian(contactAreaMinor, (float)(orientation + (Math.PI / 2)));

        // Normalize the contact area to the stream view size
        contactAreaMajorCartesian[0] = Math.min(Math.abs(contactAreaMajorCartesian[0]), streamView.getWidth()) / streamView.getWidth();
        contactAreaMinorCartesian[0] = Math.min(Math.abs(contactAreaMinorCartesian[0]), streamView.getWidth()) / streamView.getWidth();
        contactAreaMajorCartesian[1] = Math.min(Math.abs(contactAreaMajorCartesian[1]), streamView.getHeight()) / streamView.getHeight();
        contactAreaMinorCartesian[1] = Math.min(Math.abs(contactAreaMinorCartesian[1]), streamView.getHeight()) / streamView.getHeight();

        // Convert the normalized values back into polar coordinates
        return new float[] { cartesianToR(contactAreaMajorCartesian), cartesianToR(contactAreaMinorCartesian) };
    }

    private boolean sendPenEventForPointer(View view, MotionEvent event, byte eventType, byte toolType, int pointerIndex) {
        byte penButtons = 0;
        if ((event.getButtonState() & MotionEvent.BUTTON_STYLUS_PRIMARY) != 0) {
            penButtons |= MoonBridge.LI_PEN_BUTTON_PRIMARY;
        }
        if ((event.getButtonState() & MotionEvent.BUTTON_STYLUS_SECONDARY) != 0) {
            penButtons |= MoonBridge.LI_PEN_BUTTON_SECONDARY;
        }

        byte tiltDegrees = MoonBridge.LI_TILT_UNKNOWN;
        InputDevice dev = event.getDevice();
        if (dev != null) {
            if (dev.getMotionRange(MotionEvent.AXIS_TILT, event.getSource()) != null) {
                tiltDegrees = (byte)Math.toDegrees(event.getAxisValue(MotionEvent.AXIS_TILT, pointerIndex));
            }
        }

        float[] normalizedCoords = getStreamViewRelativeNormalizedXY(view, event, pointerIndex);
        float[] normalizedContactArea = getStreamViewNormalizedContactArea(event, pointerIndex);
        return conn.sendPenEvent(eventType, toolType, penButtons,
                normalizedCoords[0], normalizedCoords[1],
                getPressureOrDistance(event, pointerIndex),
                normalizedContactArea[0], normalizedContactArea[1],
                getRotationDegrees(event, pointerIndex), tiltDegrees) != MoonBridge.LI_ERR_UNSUPPORTED;
    }

    private static byte convertToolTypeToStylusToolType(MotionEvent event, int pointerIndex) {
        switch (event.getToolType(pointerIndex)) {
            case MotionEvent.TOOL_TYPE_ERASER:
                return MoonBridge.LI_TOOL_TYPE_ERASER;
            case MotionEvent.TOOL_TYPE_STYLUS:
                return MoonBridge.LI_TOOL_TYPE_PEN;
            default:
                return MoonBridge.LI_TOOL_TYPE_UNKNOWN;
        }
    }

    private boolean trySendPenEvent(View view, MotionEvent event) {
        byte eventType = getLiTouchTypeFromEvent(event);
        if (eventType < 0) {
            return false;
        }

        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            // Move events may impact all active pointers
            boolean handledStylusEvent = false;
            for (int i = 0; i < event.getPointerCount(); i++) {
                byte toolType = convertToolTypeToStylusToolType(event, i);
                if (toolType == MoonBridge.LI_TOOL_TYPE_UNKNOWN) {
                    // Not a stylus pointer, so skip it
                    continue;
                }
                else {
                    // This pointer is a stylus, so we'll report that we handled this event
                    handledStylusEvent = true;
                }

                if (!sendPenEventForPointer(view, event, eventType, toolType, i)) {
                    // Pen events aren't supported by the host
                    return false;
                }
            }
            return handledStylusEvent;
        }
        else if (event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
            // Cancel impacts all active pointers
            return conn.sendPenEvent(MoonBridge.LI_TOUCH_EVENT_CANCEL_ALL, MoonBridge.LI_TOOL_TYPE_UNKNOWN, (byte)0,
                    0, 0, 0, 0, 0,
                    MoonBridge.LI_ROT_UNKNOWN, MoonBridge.LI_TILT_UNKNOWN) != MoonBridge.LI_ERR_UNSUPPORTED;
        }
        else {
            // Up, Down, and Hover events are specific to the action index
            byte toolType = convertToolTypeToStylusToolType(event, event.getActionIndex());
            if (toolType == MoonBridge.LI_TOOL_TYPE_UNKNOWN) {
                // Not a stylus event
                return false;
            }
            return sendPenEventForPointer(view, event, eventType, toolType, event.getActionIndex());
        }
    }

    private boolean sendTouchEventForPointer(View view, MotionEvent event, byte eventType, int pointerIndex) {
        float[] normalizedCoords = getStreamViewRelativeNormalizedXY(view, event, pointerIndex);
        float[] normalizedContactArea = getStreamViewNormalizedContactArea(event, pointerIndex);
        return conn.sendTouchEvent(eventType, event.getPointerId(pointerIndex),
                normalizedCoords[0], normalizedCoords[1],
                getPressureOrDistance(event, pointerIndex),
                normalizedContactArea[0], normalizedContactArea[1],
                getRotationDegrees(event, pointerIndex)) != MoonBridge.LI_ERR_UNSUPPORTED;
    }

    private boolean trySendTouchEvent(View view, MotionEvent event) {
        byte eventType = getLiTouchTypeFromEvent(event);
        if (eventType < 0) {
            return false;
        }

        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            // Move events may impact all active pointers
            for (int i = 0; i < event.getPointerCount(); i++) {
                if (!sendTouchEventForPointer(view, event, eventType, i)) {
                    return false;
                }
            }
            return true;
        }
        else if (event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
            // Cancel impacts all active pointers
            return conn.sendTouchEvent(MoonBridge.LI_TOUCH_EVENT_CANCEL_ALL, 0,
                    0, 0, 0, 0, 0,
                    MoonBridge.LI_ROT_UNKNOWN) != MoonBridge.LI_ERR_UNSUPPORTED;
        }
        else {
            // Up, Down, and Hover events are specific to the action index
            return sendTouchEventForPointer(view, event, eventType, event.getActionIndex());
        }
    }

    // Returns true if the event was consumed
    // NB: View is only present if called from a view callback
    private boolean handleMotionEvent(View view, MotionEvent event) {
        // Pass through mouse/touch/joystick input if we're not grabbing
        if (!grabbedInput) {
            return false;
        }

        int eventSource = event.getSource();
        int deviceSources = event.getDevice() != null ? event.getDevice().getSources() : 0;
        if ((eventSource & InputDevice.SOURCE_CLASS_JOYSTICK) != 0) {
            if (controllerHandler.handleMotionEvent(event)) {
                return true;
            }
        }
        else if ((deviceSources & InputDevice.SOURCE_CLASS_JOYSTICK) != 0 && controllerHandler.tryHandleTouchpadEvent(event)) {
            return true;
        }
        else if ((eventSource & InputDevice.SOURCE_CLASS_POINTER) != 0 ||
                 (eventSource & InputDevice.SOURCE_CLASS_POSITION) != 0 ||
                 eventSource == InputDevice.SOURCE_MOUSE_RELATIVE)
        {
            // This case is for mice and non-finger touch devices
            if (eventSource == InputDevice.SOURCE_MOUSE ||
                    (eventSource & InputDevice.SOURCE_CLASS_POSITION) != 0 || // SOURCE_TOUCHPAD
                    eventSource == InputDevice.SOURCE_MOUSE_RELATIVE ||
                    (event.getPointerCount() >= 1 &&
                            (event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE ||
                                    event.getToolType(0) == MotionEvent.TOOL_TYPE_STYLUS ||
                                    event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER)) ||
                    eventSource == 12290) // 12290 = Samsung DeX mode desktop mouse
            {
                int buttonState = event.getButtonState();
                int changedButtons = buttonState ^ lastButtonState;

                // The DeX touchpad on the Fold 4 sends proper right click events using BUTTON_SECONDARY,
                // but doesn't send BUTTON_PRIMARY for a regular click. Instead it sends ACTION_DOWN/UP,
                // so we need to fix that up to look like a sane input event to process it correctly.
                if (eventSource == 12290) {
                    if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                        buttonState |= MotionEvent.BUTTON_PRIMARY;
                    }
                    else if (event.getAction() == MotionEvent.ACTION_UP) {
                        buttonState &= ~MotionEvent.BUTTON_PRIMARY;
                    }
                    else {
                        // We may be faking the primary button down from a previous event,
                        // so be sure to add that bit back into the button state.
                        buttonState |= (lastButtonState & MotionEvent.BUTTON_PRIMARY);
                    }

                    changedButtons = buttonState ^ lastButtonState;
                }

                // Ignore mouse input if we're not capturing from our input source
                if (!inputCaptureProvider.isCapturingActive()) {
                    // We return true here because otherwise the events may end up causing
                    // Android to synthesize d-pad events.
                    return true;
                }

                // Always update the position before sending any button events. If we're
                // dealing with a stylus without hover support, our position might be
                // significantly different than before.
                if (inputCaptureProvider.eventHasRelativeMouseAxes(event)) {
                    // Send the deltas straight from the motion event
                    short deltaX = (short)inputCaptureProvider.getRelativeAxisX(event);
                    short deltaY = (short)inputCaptureProvider.getRelativeAxisY(event);

                    if (deltaX != 0 || deltaY != 0) {
                        if (prefConfig.absoluteMouseMode) {
                            // NB: view may be null, but we can unconditionally use streamView because we don't need to adjust
                            // relative axis deltas for the position of the streamView within the parent's coordinate system.
                            if (externalDisplayActive && videoView.getWidth() > 0 && videoView.getHeight() > 0) {
                                // §EXT-DISPLAY：相對位移換算成絕對位置時，參考矩形是正在顯示影像的 view。
                                // 影像 view 還沒 layout 時寬高是 0，退回手機 view，避免送出 0 尺寸。
                                conn.sendMouseMoveAsMousePosition(deltaX, deltaY, (short)videoView.getWidth(), (short)videoView.getHeight());
                            }
                            else {
                                conn.sendMouseMoveAsMousePosition(deltaX, deltaY, (short)streamView.getWidth(), (short)streamView.getHeight());
                            }
                        }
                        else {
                            conn.sendMouseMove(deltaX, deltaY);
                        }
                    }
                }
                else if ((eventSource & InputDevice.SOURCE_CLASS_POSITION) != 0) {
                    // If this input device is not associated with the view itself (like a trackpad),
                    // we'll convert the device-specific coordinates to use to send the cursor position.
                    // This really isn't ideal but it's probably better than nothing.
                    //
                    // Trackpad on newer versions of Android (Oreo and later) should be caught by the
                    // relative axes case above. If we get here, we're on an older version that doesn't
                    // support pointer capture.
                    InputDevice device = event.getDevice();
                    if (device != null) {
                        InputDevice.MotionRange xRange = device.getMotionRange(MotionEvent.AXIS_X, eventSource);
                        InputDevice.MotionRange yRange = device.getMotionRange(MotionEvent.AXIS_Y, eventSource);

                        // All touchpads coordinate planes should start at (0, 0)
                        if (xRange != null && yRange != null && xRange.getMin() == 0 && yRange.getMin() == 0) {
                            int xMax = (int)xRange.getMax();
                            int yMax = (int)yRange.getMax();

                            // Touchpads must be smaller than (65535, 65535)
                            if (xMax <= Short.MAX_VALUE && yMax <= Short.MAX_VALUE) {
                                conn.sendMousePosition((short)event.getX(), (short)event.getY(),
                                                       (short)xMax, (short)yMax);
                            }
                        }
                    }
                }
                else if (view != null && trySendPenEvent(view, event)) {
                    // If our host supports pen events, send it directly
                    return true;
                }
                else if (view != null) {
                    // Otherwise send absolute position based on the view for SOURCE_CLASS_POINTER
                    updateMousePosition(view, event);
                }
                else if (QuestSupport.isQuest(this)) {
                    // §QUEST-2D：Activity.onGenericMotionEvent 傳進來的 view 是 null，上面那支不會送絕對座標。
                    // 非 Quest 不會進這裡，維持原本不送。
                    updateMousePosition(null, event);
                }

                if (event.getActionMasked() == MotionEvent.ACTION_SCROLL) {
                    // Send the vertical scroll packet
                    conn.sendMouseHighResScroll((short)(event.getAxisValue(MotionEvent.AXIS_VSCROLL) * 120));
                    conn.sendMouseHighResHScroll((short)(event.getAxisValue(MotionEvent.AXIS_HSCROLL) * 120));
                }

                if ((changedButtons & MotionEvent.BUTTON_PRIMARY) != 0) {
                    if ((buttonState & MotionEvent.BUTTON_PRIMARY) != 0) {
                        conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_LEFT);
                    }
                    else {
                        conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_LEFT);
                    }
                }

                // Mouse secondary or stylus primary is right click (stylus down is left click)
                if ((changedButtons & (MotionEvent.BUTTON_SECONDARY | MotionEvent.BUTTON_STYLUS_PRIMARY)) != 0) {
                    if ((buttonState & (MotionEvent.BUTTON_SECONDARY | MotionEvent.BUTTON_STYLUS_PRIMARY)) != 0) {
                        conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_RIGHT);
                    }
                    else {
                        conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_RIGHT);
                    }
                }

                // Mouse tertiary or stylus secondary is middle click
                if ((changedButtons & (MotionEvent.BUTTON_TERTIARY | MotionEvent.BUTTON_STYLUS_SECONDARY)) != 0) {
                    if ((buttonState & (MotionEvent.BUTTON_TERTIARY | MotionEvent.BUTTON_STYLUS_SECONDARY)) != 0) {
                        conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_MIDDLE);
                    }
                    else {
                        conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_MIDDLE);
                    }
                }

                if (prefConfig.mouseNavButtons) {
                    if ((changedButtons & MotionEvent.BUTTON_BACK) != 0) {
                        if ((buttonState & MotionEvent.BUTTON_BACK) != 0) {
                            conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_X1);
                        }
                        else {
                            conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_X1);
                        }
                    }

                    if ((changedButtons & MotionEvent.BUTTON_FORWARD) != 0) {
                        if ((buttonState & MotionEvent.BUTTON_FORWARD) != 0) {
                            conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_X2);
                        }
                        else {
                            conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_X2);
                        }
                    }
                }

                // Handle stylus presses
                if (event.getPointerCount() == 1 && event.getActionIndex() == 0) {
                    if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                        if (event.getToolType(0) == MotionEvent.TOOL_TYPE_STYLUS) {
                            lastAbsTouchDownTime = event.getEventTime();
                            lastAbsTouchDownX = event.getX(0);
                            lastAbsTouchDownY = event.getY(0);

                            // Stylus is left click
                            conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_LEFT);
                        } else if (event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER) {
                            lastAbsTouchDownTime = event.getEventTime();
                            lastAbsTouchDownX = event.getX(0);
                            lastAbsTouchDownY = event.getY(0);

                            // Eraser is right click
                            conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_RIGHT);
                        }
                    }
                    else if (event.getActionMasked() == MotionEvent.ACTION_UP || event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
                        if (event.getToolType(0) == MotionEvent.TOOL_TYPE_STYLUS) {
                            lastAbsTouchUpTime = event.getEventTime();
                            lastAbsTouchUpX = event.getX(0);
                            lastAbsTouchUpY = event.getY(0);

                            // Stylus is left click
                            conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_LEFT);
                        } else if (event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER) {
                            lastAbsTouchUpTime = event.getEventTime();
                            lastAbsTouchUpX = event.getX(0);
                            lastAbsTouchUpY = event.getY(0);

                            // Eraser is right click
                            conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_RIGHT);
                        }
                    }
                }

                lastButtonState = buttonState;
            }
            // This case is for fingers
            else
            {
                // §QUEST-2D：下面的 switch 沒有 HOVER／BUTTON，default 直接回 false，絕對位置與左鍵都不會送。
                // Quest 雷射可能是 SOURCE_TOUCHSCREEN + TOOL_TYPE_FINGER 的 HOVER。未在實機確認。
                // 抓取旗標還沒立起時不送，否則連線中的 hover 會先推遠端游標。
                // 非 Quest 不呼叫；Quest 上不是這兩類事件時回 false，仍走下面原本的手指路徑。
                if (QuestSupport.isQuest(this) && inputCaptureProvider.isCapturingActive()
                        && handleQuestFingerHover(view, event)) {
                    return true;
                }

                if (virtualController != null &&
                        (virtualController.getControllerMode() == VirtualController.ControllerMode.MoveButtons ||
                         virtualController.getControllerMode() == VirtualController.ControllerMode.ResizeButtons)) {
                    // Ignore presses when the virtual controller is being configured
                    return true;
                }

                // If this is the parent view, we'll offset our coordinates to appear as if they
                // are relative to the StreamView like our StreamView touch events are.
                float xOffset, yOffset;
                if (view != streamView && !prefConfig.touchscreenTrackpad && !externalDisplayActive) {
                    xOffset = -streamView.getX();
                    yOffset = -streamView.getY();
                }
                else {
                    xOffset = 0.f;
                    yOffset = 0.f;
                }

                int actionIndex = event.getActionIndex();

                int eventX = (int)(event.getX(actionIndex) + xOffset);
                int eventY = (int)(event.getY(actionIndex) + yOffset);

                // Special handling for 3 finger gesture
                if (event.getActionMasked() == MotionEvent.ACTION_POINTER_DOWN &&
                        event.getPointerCount() == 3) {
                    // Three fingers down
                    threeFingerDownTime = event.getEventTime();

                    // Cancel the first and second touches to avoid
                    // erroneous events
                    for (TouchContext aTouchContext : touchContextMap) {
                        aTouchContext.cancelTouch();
                    }

                    return true;
                }

                // TODO: Re-enable native touch when have a better solution for handling
                // cancelled touches from Android gestures and 3 finger taps to activate
                // the software keyboard.
                /*if (!prefConfig.touchscreenTrackpad && trySendTouchEvent(view, event)) {
                    // If this host supports touch events and absolute touch is enabled,
                    // send it directly as a touch event.
                    return true;
                }*/

                TouchContext context = getTouchContext(actionIndex);
                if (context == null) {
                    return false;
                }

                switch (event.getActionMasked())
                {
                case MotionEvent.ACTION_POINTER_DOWN:
                case MotionEvent.ACTION_DOWN:
                    for (TouchContext touchContext : touchContextMap) {
                        touchContext.setPointerCount(event.getPointerCount());
                    }
                    context.touchDownEvent(eventX, eventY, event.getEventTime(), true);
                    break;
                case MotionEvent.ACTION_POINTER_UP:
                case MotionEvent.ACTION_UP:
                    if (event.getPointerCount() == 1 &&
                            (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU || (event.getFlags() & MotionEvent.FLAG_CANCELED) == 0)) {
                        // All fingers up
                        if (event.getEventTime() - threeFingerDownTime < THREE_FINGER_TAP_THRESHOLD) {
                            // This is a 3 finger tap to bring up the keyboard
                            toggleKeyboard();
                            return true;
                        }
                    }

                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && (event.getFlags() & MotionEvent.FLAG_CANCELED) != 0) {
                        context.cancelTouch();
                    }
                    else {
                        context.touchUpEvent(eventX, eventY, event.getEventTime());
                    }

                    for (TouchContext touchContext : touchContextMap) {
                        touchContext.setPointerCount(event.getPointerCount() - 1);
                    }
                    if (actionIndex == 0 && event.getPointerCount() > 1 && !context.isCancelled()) {
                        // The original secondary touch now becomes primary
                        context.touchDownEvent(
                                (int)(event.getX(1) + xOffset),
                                (int)(event.getY(1) + yOffset),
                                event.getEventTime(), false);
                    }
                    break;
                case MotionEvent.ACTION_MOVE:
                    // ACTION_MOVE is special because it always has actionIndex == 0
                    // We'll call the move handlers for all indexes manually

                    // First process the historical events
                    for (int i = 0; i < event.getHistorySize(); i++) {
                        for (TouchContext aTouchContextMap : touchContextMap) {
                            if (aTouchContextMap.getActionIndex() < event.getPointerCount())
                            {
                                aTouchContextMap.touchMoveEvent(
                                        (int)(event.getHistoricalX(aTouchContextMap.getActionIndex(), i) + xOffset),
                                        (int)(event.getHistoricalY(aTouchContextMap.getActionIndex(), i) + yOffset),
                                        event.getHistoricalEventTime(i));
                            }
                        }
                    }

                    // Now process the current values
                    for (TouchContext aTouchContextMap : touchContextMap) {
                        if (aTouchContextMap.getActionIndex() < event.getPointerCount())
                        {
                            aTouchContextMap.touchMoveEvent(
                                    (int)(event.getX(aTouchContextMap.getActionIndex()) + xOffset),
                                    (int)(event.getY(aTouchContextMap.getActionIndex()) + yOffset),
                                    event.getEventTime());
                        }
                    }
                    break;
                case MotionEvent.ACTION_CANCEL:
                    for (TouchContext aTouchContext : touchContextMap) {
                        aTouchContext.cancelTouch();
                        aTouchContext.setPointerCount(0);
                    }
                    break;
                default:
                    return false;
                }
            }

            // Handled a known source
            return true;
        }

        // Unknown class
        return false;
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        return handleMotionEvent(null, event) || super.onGenericMotionEvent(event);

    }

    // §QUEST-2D：手指分支的 default 會把 hover 丟掉，BUTTON 也不在 switch 裡。手機的 DOWN／MOVE 不進這裡。
    private boolean handleQuestFingerHover(View view, MotionEvent event) {
        int action = event.getActionMasked();
        boolean hoverOrButton = action == MotionEvent.ACTION_HOVER_ENTER
                || action == MotionEvent.ACTION_HOVER_MOVE
                || action == MotionEvent.ACTION_BUTTON_PRESS
                || action == MotionEvent.ACTION_BUTTON_RELEASE;
        if (!hoverOrButton) {
            return false;
        }

        // 只送按鍵的話，主機游標還停在按下前的位置。
        updateMousePosition(view, event);

        int buttonState = event.getButtonState();
        int actionButton = event.getActionButton();
        // 有的裝置 ACTION_BUTTON_PRESS 的 getButtonState 還沒含這個鍵，放開時則已經拿掉。用 actionButton 補。
        if (action == MotionEvent.ACTION_BUTTON_PRESS) {
            buttonState |= actionButton;
        }
        else if (action == MotionEvent.ACTION_BUTTON_RELEASE) {
            buttonState &= ~actionButton;
        }

        int changedButtons = buttonState ^ lastButtonState;
        if ((changedButtons & MotionEvent.BUTTON_PRIMARY) != 0) {
            if ((buttonState & MotionEvent.BUTTON_PRIMARY) != 0) {
                conn.sendMouseButtonDown(MouseButtonPacket.BUTTON_LEFT);
            }
            else {
                conn.sendMouseButtonUp(MouseButtonPacket.BUTTON_LEFT);
            }
        }
        lastButtonState = buttonState;
        return true;
    }

    private void updateMousePosition(View touchedView, MotionEvent event) {
        // X and Y are already relative to the provided view object
        float eventX, eventY;

        // For our StreamView itself, we can use the coordinates unmodified.
        if (touchedView == streamView) {
            eventX = event.getX(0);
            eventY = event.getY(0);
        }
        else if (touchedView == null) {
            // §QUEST-2D：只有 Quest 會傳 null。假設 Activity.onGenericMotionEvent 的 getX/getY
            // 是視窗座標（子 view 沒吃掉事件時，分發過程的位移已還原，原點是視窗而不是 streamView）。
            // streamView.getX() 只相對父層，面板有黑邊時會偏，所以改扣 getLocationInWindow。
            // 未在實機確認。若游標整體平移，比對 [VIPLE-QUEST-2D] pointer 的 x,y、raw、streamWin。
            int[] locationInWindow = new int[2];
            streamView.getLocationInWindow(locationInWindow);
            eventX = event.getX(0) - locationInWindow[0];
            eventY = event.getY(0) - locationInWindow[1];
        }
        else {
            // For the containing background view, we must subtract the origin
            // of the StreamView to get video-relative coordinates.
            eventX = event.getX(0) - streamView.getX();
            eventY = event.getY(0) - streamView.getY();
        }

        if (event.getPointerCount() == 1 && event.getActionIndex() == 0 &&
                (event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER ||
                event.getToolType(0) == MotionEvent.TOOL_TYPE_STYLUS))
        {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_HOVER_ENTER:
                case MotionEvent.ACTION_HOVER_EXIT:
                case MotionEvent.ACTION_HOVER_MOVE:
                    if (event.getEventTime() - lastAbsTouchUpTime <= STYLUS_UP_DEAD_ZONE_DELAY &&
                            Math.sqrt(Math.pow(eventX - lastAbsTouchUpX, 2) + Math.pow(eventY - lastAbsTouchUpY, 2)) <= STYLUS_UP_DEAD_ZONE_RADIUS) {
                        // Enforce a small deadzone between touch up and hover or touch down to allow more precise double-clicking
                        return;
                    }
                    break;

                case MotionEvent.ACTION_MOVE:
                case MotionEvent.ACTION_UP:
                    if (event.getEventTime() - lastAbsTouchDownTime <= STYLUS_DOWN_DEAD_ZONE_DELAY &&
                            Math.sqrt(Math.pow(eventX - lastAbsTouchDownX, 2) + Math.pow(eventY - lastAbsTouchDownY, 2)) <= STYLUS_DOWN_DEAD_ZONE_RADIUS) {
                        // Enforce a small deadzone between touch down and move or touch up to allow more precise double-clicking
                        return;
                    }
                    break;
            }
        }

        // We may get values slightly outside our view region on ACTION_HOVER_ENTER and ACTION_HOVER_EXIT.
        // Normalize these to the view size. We can't just drop them because we won't always get an event
        // right at the boundary of the view, so dropping them would result in our cursor never really
        // reaching the sides of the screen.
        eventX = Math.min(Math.max(eventX, 0), streamView.getWidth());
        eventY = Math.min(Math.max(eventY, 0), streamView.getHeight());

        // §QUEST-2D：實機要靠這幾筆對事件來源。移動會洗版，所以有額度；點擊不能被額度吃掉。
        if (QuestSupport.isQuest(this)) {
            logQuestPointer(event, touchedView, eventX, eventY);
        }

        if (externalDisplayActive) {
            // §EXT-DISPLAY：手機 view 已鎖成跟串流一樣的長寬比。正規化後再映射，游標在兩邊的比例才一致。
            int phoneWidth = streamView.getWidth();
            int phoneHeight = streamView.getHeight();
            int videoWidth = videoView.getWidth();
            int videoHeight = videoView.getHeight();
            if (phoneWidth > 0 && phoneHeight > 0 && videoWidth > 0 && videoHeight > 0) {
                eventX = eventX / phoneWidth * videoWidth;
                eventY = eventY / phoneHeight * videoHeight;
                conn.sendMousePosition((short)eventX, (short)eventY, (short)videoWidth, (short)videoHeight);
            }
            else {
                conn.sendMousePosition((short)eventX, (short)eventY, (short)streamView.getWidth(), (short)streamView.getHeight());
            }
        }
        else {
            conn.sendMousePosition((short)eventX, (short)eventY, (short)streamView.getWidth(), (short)streamView.getHeight());
        }
    }

    // §QUEST-2D：主鍵若只出現在 HOVER_MOVE 的 buttonState，移動額度用完後會看不到點擊。
    private void logQuestPointer(MotionEvent event, View touchedView, float sentX, float sentY) {
        int action = event.getActionMasked();
        boolean buttonEdge = event.getActionButton() != 0 || event.getButtonState() != questLoggedButtonState;
        if (buttonEdge) {
            questLoggedButtonState = event.getButtonState();
        }
        boolean always = action == MotionEvent.ACTION_HOVER_ENTER
                || action == MotionEvent.ACTION_BUTTON_PRESS
                || action == MotionEvent.ACTION_BUTTON_RELEASE
                || buttonEdge;
        if (!always) {
            if (questPointerLogsLeft <= 0) {
                return;
            }
            questPointerLogsLeft--;
        }
        int[] locationInWindow = new int[2];
        streamView.getLocationInWindow(locationInWindow);
        String viewName = touchedView == null ? "null" : (touchedView == streamView ? "stream" : "other");
        LimeLog.info("[VIPLE-QUEST-2D] pointer source=0x" + Integer.toHexString(event.getSource())
                + " action=" + action
                + " tool=" + (event.getPointerCount() > 0 ? event.getToolType(0) : -1)
                + " buttons=0x" + Integer.toHexString(event.getButtonState())
                + " actionButton=0x" + Integer.toHexString(event.getActionButton())
                + " view=" + viewName
                + " xy=" + event.getX() + "," + event.getY()
                + " raw=" + event.getRawX() + "," + event.getRawY()
                + " streamWin=" + locationInWindow[0] + "," + locationInWindow[1]
                + " sent=" + sentX + "," + sentY
                + " size=" + streamView.getWidth() + "x" + streamView.getHeight()
                + (questPointerLogsLeft == 0 && !always ? " (further moves suppressed)" : ""));
    }

    @Override
    public boolean onGenericMotion(View view, MotionEvent event) {
        return handleMotionEvent(view, event);
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override
    public boolean onTouch(View view, MotionEvent event) {
        if (event.getAction() == MotionEvent.ACTION_DOWN) {
            // Tell the OS not to buffer input events for us
            //
            // NB: This is still needed even when we call the newer requestUnbufferedDispatch()!
            view.requestUnbufferedDispatch(event);
        }

        return handleMotionEvent(view, event);
    }

    @Override
    public void stageStarting(final String stage) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (spinner != null) {
                    spinner.setMessage(getResources().getString(R.string.conn_starting) + " " + stage);
                }
            }
        });
    }

    @Override
    public void stageComplete(String stage) {
    }

    /**
     * VipleStream §N — 啟動 in-stream file transfer client（每 2s polling
     * server /transfer/poll）。必須在 stream 已連上且 NvHTTP 可用之後呼叫，
     * 因此放在 connectionStarted 的 UI thread 區塊。
     */
    private void startFileTransferClient() {
        if (fileTransferClient != null) return;
        if (xferHost == null || xferHttpsPort == 0 || xferServerCert == null) {
            android.util.Log.w("VipleXfer", "[VIPLE-XFER] missing host/port/cert — skipping");
            return;
        }
        try {
            String uniqueId = "0123456789ABCDEF";  // 跟 NvHTTP 一致
            com.limelight.LimeLog.info("[VIPLE-XFER] starting client for " + xferHost + ":" + xferHttpsPort);
            // 借 NvHTTP 那份 SSL-pinned OkHttpClient + HttpUrl
            com.limelight.nvstream.http.NvHTTP http = new com.limelight.nvstream.http.NvHTTP(
                    new com.limelight.nvstream.http.ComputerDetails.AddressTuple(xferHost, com.limelight.nvstream.http.NvHTTP.DEFAULT_HTTP_PORT),
                    xferHttpsPort,
                    uniqueId,
                    xferServerCert,
                    com.limelight.binding.PlatformBinding.getCryptoProvider(this));
            okhttp3.HttpUrl base = http.getHttpsUrl(true);
            // §N.5.bug fix (2026-05-14): supplier 仍是 getLongConnectClient
            // （每次呼叫都是新的 SSLContext）。Pixel 5 的 Conscrypt 在同一顆
            // client 累積太多次握手後會丟 TLSV1_ALERT_INTERNAL_ERROR。
            // §REVIEW-1118B：不要在這裡把回傳值抓成 final。FileTransferClient
            // 會自己留一顆讓 /transfer/poll 走 keep-alive，只有握手失敗才再要一顆。
            //
            // §N.5 UX (2026-05-14): emit() 改走 fileTransferOverlay 替代原 Toast,
            // 整個 transfer 期間持續可見 (0-100%), 完成或失敗 3 秒後自動 hide.
            fileTransferClient = new com.limelight.transfer.FileTransferClient(
                    getApplicationContext(), http::getLongConnectClient, base,
                    this::showFileTransferOverlay);
            fileTransferClient.start();
        } catch (Exception e) {
            android.util.Log.w("VipleXfer", "[VIPLE-XFER] startFileTransferClient failed", e);
        }
    }

    private void stopFileTransferClient() {
        if (fileTransferClient != null) {
            fileTransferClient.stop();
            fileTransferClient = null;
        }
    }

    private void startQxrVideoTestIfRequested() {
        if (!getIntent().getBooleanExtra("qxr_video_test", false)) {
            return;
        }
        if (!QxrVideoTest.isAvailable()) {
            LimeLog.warning("[VIPLE-QXR-VTEST] viple-openxr is not loadable; running a normal stream");
            return;
        }
        boolean srgb = getIntent().getBooleanExtra("qxr_video_test_srgb", false);
        long handle = QxrVideoTest.nativeCreate(prefConfig.width, prefConfig.height,
                qxrVideoTestOutDir(), srgb);
        if (handle == 0) {
            LimeLog.warning("[VIPLE-QXR-VTEST] nativeCreate failed; running a normal stream");
            return;
        }
        qxrVideoTestHandle = handle;
        decoderRenderer.setDirectOutputSurface(QxrVideoTest.nativeGetSurface(handle));
        QxrVideoTest.nativeStart(handle);
        LimeLog.info("[VIPLE-QXR-VTEST] decoder output goes to the Vulkan test path "
                + prefConfig.width + "x" + prefConfig.height + " srgb=" + srgb);
    }

    // release 版不能 run-as；Android 11 起 adb 也讀不到 Android/data。測試機用 adb 授與「管理所有檔案」後，
    // 截圖改寫到 Download，adb pull 才拿得到。沒授權就退回 app 自己的目錄（只看 log 也能判讀）。
    private String qxrVideoTestOutDir() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && android.os.Environment.isExternalStorageManager()) {
            return android.os.Environment.getExternalStoragePublicDirectory(
                    android.os.Environment.DIRECTORY_DOWNLOADS).getAbsolutePath() + "/viplestream_qxr_vtest";
        }
        java.io.File base = getExternalFilesDir(null);
        if (base == null) {
            base = getCacheDir();
        }
        return base.getAbsolutePath() + "/qxr_vtest";
    }

    private synchronized void releaseQxrVideoTest() {
        long handle = qxrVideoTestHandle;
        if (handle == 0) {
            return;
        }
        qxrVideoTestHandle = 0;
        QxrVideoTest.nativeStop(handle);
        QxrVideoTest.nativeDestroy(handle);
    }

    // §VITURE-WARP：只在 glasses_vr、偏好開啟、而且 EGL 真的起來時才把解碼器改接過去。失敗就留在原本的直接顯示。
    private void startGlassesWarpIfEnabled(SurfaceHolder holder) {
        if (!glassesVr) {
            return;
        }
        if (!prefConfig.glassesTimewarp) {
            LimeLog.info("[VIPLE-VITURE] timewarp preference off; direct display");
            return;
        }
        if (qxrVideoTestHandle != 0) {
            LimeLog.info("[VIPLE-VITURE] timewarp skipped; video test owns the decoder surface");
            return;
        }
        if (vrEmuHandle == 0 || !QxrNative.isAvailable()) {
            LimeLog.info("[VIPLE-VITURE] timewarp skipped; imu pose source is not ready");
            return;
        }
        Surface surface = holder.getSurface();
        if (surface == null || !surface.isValid() || decoderRenderer == null) {
            LimeLog.warning("[VIPLE-VITURE] timewarp skipped; output surface is not valid");
            return;
        }
        float tanX = 0.4831f;
        float tanY = 0.2718f;
        if (vrLaunch != null && vrLaunch.fovTan[0] > 0 && vrLaunch.fovTan[2] > 0) {
            tanX = vrLaunch.fovTan[0] / 10000f;
            tanY = vrLaunch.fovTan[2] / 10000f;
        }
        int width = externalStreamWidth();
        int height = externalStreamHeight();
        if (width <= 0) {
            width = 3840;
        }
        if (height <= 0) {
            height = 1080;
        }
        GlassesWarpRenderer warp;
        try {
            warp = GlassesWarpRenderer.start(vrEmuHandle, tanX, tanY, width, height);
        } catch (RuntimeException error) {
            LimeLog.warning("[VIPLE-VITURE] timewarp init failed: " + error.getMessage());
            return;
        }
        if (warp == null) {
            LimeLog.warning("[VIPLE-VITURE] timewarp unavailable; direct display");
            return;
        }
        Surface decoderSurface = warp.getDecoderSurface();
        if (decoderSurface == null || !warp.attachOutput(surface, videoView)) {
            warp.release();
            LimeLog.warning("[VIPLE-VITURE] timewarp output attach failed; direct display");
            return;
        }
        synchronized (glassesWarpLock) {
            glassesWarp = warp;
        }
        decoderRenderer.setDirectOutputSurface(decoderSurface);
        LimeLog.info("[VIPLE-VITURE] timewarp output " + width + "x" + height
                + " tanX=" + tanX + " tanY=" + tanY);
    }

    private void detachGlassesWarpOutput() {
        GlassesWarpRenderer warp;
        synchronized (glassesWarpLock) {
            warp = glassesWarp;
        }
        if (warp != null) {
            warp.detachOutput();
        }
    }

    private void releaseGlassesWarp() {
        GlassesWarpRenderer warp;
        synchronized (glassesWarpLock) {
            warp = glassesWarp;
            glassesWarp = null;
        }
        if (warp != null) {
            warp.release();
        }
    }

    private void stopConnection() {
        stopConnection(true);
    }

    // §REVIEW-1111：releaseOwnership＝要不要送 /cancel。/cancel 在 host 上不只拆串流，還會把遊戲關掉。
    // 使用者自己離開、或 host 正常結束時才送；意外斷線（沒畫面、控制通道斷、Wi-Fi 切換）不送——
    // host 會留著遊戲約 60 秒讓同一台裝置接回去（桌面 client 一直是這樣做的）。
    private void stopConnection(final boolean releaseOwnership) {
        // §EXT-DISPLAY：離開或結束時要把寬限計時拿掉，否則 Activity 關了還會再彈一次中斷對話框。
        // 調暗提示的計時也要一起拿掉，否則結束後還會改這個視窗的亮度。
        cancelExternalDisplayGraceTimer();
        cancelExternalDimHintTimer();
        externalDisplayAwaiting = false;
        externalReconnectPending = false;

        // §QUEST-VR：連線還沒標成 connecting 也要停。下面的 if 沒進去時，模擬執行緒不能留著。
        releaseVrEmulation();

        // VipleStream §N — stop transfer polling before tearing down stream
        stopFileTransferClient();

        if (connecting || connected) {
            connecting = connected = false;
            updatePipAutoEnter();
            // §LOGDROP：摘要在 conn.stop() 之前抓。解碼器拆掉之後就讀不到 codec。
            // releaseVrEmulation 已經先跑過。眼鏡會送的那場旗標已寫上，這裡就不再送。
            final String sessionEndSummary = glassesSessionEndOwned ? null : streamEndSummary();
            final String sessionEndSource = sessionEndSummary == null ? null : LogDrop.sourceFor(this);

            controllerHandler.stop();

            synchronized (glassesWarpLock) {
                if (glassesWarp != null) {
                    // §VITURE-WARP：SurfaceTexture 要等解碼器停掉。onDestroy 看到這個旗標就不再提早釋放。
                    glassesWarpReleaseOnStopThread = true;
                }
            }

            // Update GameManager state to indicate we're no longer in game
            UiHelper.notifyStreamEnded(this);

            // Stop may take a few hundred ms to do some network I/O to tell
            // the server we're going away and clean up. Let it run in a separate
            // thread to keep things smooth for the UI. Inside moonlight-common,
            // we prevent another thread from starting a connection before and
            // during the process of stopping this one.
            if (externalPresentation != null || externalHoldSurface != null) {
                // §EXT-DISPLAY：等這條執行緒裡的 conn.stop() 回來再拆 Presentation 與暫存 Surface。
                // 解碼器還抓著暫存 Surface 時先 release，各家解碼器行為不一致。
                externalDismissPending = true;
            }
            new Thread() {
                public void run() {
                    // §REVIEW-1104：先 conn.stop() 釋放 semaphore；quitApp() 沒有讀取逾時，卡住不能擋住 teardown
                    try {
                        conn.stop();
                    } catch (Exception e) {
                        android.util.Log.w("VipleStream", "[§M.1.f] conn.stop failed: " + e.getMessage());
                    }
                    // §QXR-VIDEO-TEST：解碼器已經停了，才能拆它輸出的 AImageReader。
                    releaseQxrVideoTest();
                    // §VITURE-WARP：同一個時機。解碼器還抓著 SurfaceTexture 時先 release，驅動行為不一致。
                    releaseGlassesWarp();

                    if (externalDismissPending) {
                        // §EXT-DISPLAY：conn.stop() 會把解碼器停掉。Surface 比解碼器早消失時，各家解碼器行為不一致。
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                dismissExternalPresentation();
                                releaseExternalHoldSurface();
                            }
                        });
                    }

                    // §M.1.f：/cancel 釋放 server 端 _owner_uuid。conn.stop() 只拆串流，
                    // 不放 host 擁有權。這步失敗不可擋住已經做完的 teardown。
                    try {
                        if (!releaseOwnership) {
                            android.util.Log.i("VipleStream", "[§M.1.f] unexpected disconnect: not sending /cancel (the host keeps the app for a resume)");
                        }
                        else if (xferHost != null && xferHttpsPort != 0 && xferServerCert != null) {
                            String uniqueId = "0123456789ABCDEF";  // same value FileTransferClient uses
                            com.limelight.nvstream.http.NvHTTP cancelHttp =
                                new com.limelight.nvstream.http.NvHTTP(
                                    new com.limelight.nvstream.http.ComputerDetails.AddressTuple(
                                        xferHost, com.limelight.nvstream.http.NvHTTP.DEFAULT_HTTP_PORT),
                                    xferHttpsPort,
                                    uniqueId,
                                    xferServerCert,
                                    com.limelight.binding.PlatformBinding.getCryptoProvider(Game.this));
                            cancelHttp.quitApp();
                            android.util.Log.i("VipleStream", "[§M.1.f] /cancel sent to release server ownership");
                        }
                    } catch (Exception e) {
                        // Best-effort — server may already be unreachable etc.
                        // Defensive layer covers via RTSP teardown when implemented.
                        android.util.Log.w("VipleStream", "[§M.1.f] /cancel failed: " + e.getMessage());
                    }
                    if (sessionEndSummary != null) {
                        LogDrop.autoUploadAsync(getApplicationContext(), sessionEndSource, "session-end", sessionEndSummary);
                    }
                }
            }.start();
        }
    }

    private String streamEndSummary() {
        long now = SystemClock.elapsedRealtime();
        long origin = streamLiveElapsed > 0L ? streamLiveElapsed : streamAttemptElapsed;
        long duration = origin > 0L ? now - origin : 0L;
        if (duration < 0L) {
            duration = 0L;
        }
        int width = prefConfig != null ? prefConfig.width : 0;
        int height = prefConfig != null ? prefConfig.height : 0;
        int fps = prefConfig != null ? prefConfig.fps : 0;
        int format = decoderRenderer != null ? decoderRenderer.getActiveVideoFormat() : 0;
        StringBuilder out = new StringBuilder(128);
        out.append("duration_ms=").append(duration);
        out.append(" resolution=").append(width).append('x').append(height);
        out.append(" fps=").append(fps);
        out.append(" codec=").append(codecLabel(format));
        boolean failed = streamEndStage != null
                || (streamEndError != Integer.MIN_VALUE
                && streamEndError != MoonBridge.ML_ERROR_GRACEFUL_TERMINATION);
        if (!failed) {
            out.append(" end=normal");
            return out.toString();
        }
        out.append(" end=error");
        if (streamEndError != Integer.MIN_VALUE) {
            out.append(" error_code=").append(streamEndError);
        }
        if (streamEndStage != null) {
            out.append(" stage=").append(safeStage(streamEndStage));
        }
        return out.toString();
    }

    private static String codecLabel(int videoFormat) {
        String name;
        if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_H264) != 0) {
            name = "H.264";
        } else if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_H265) != 0) {
            name = "HEVC";
        } else if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_PYROWAVE) != 0) {
            name = "PyroWave";
        } else if ((videoFormat & MoonBridge.VIDEO_FORMAT_MASK_AV1) != 0) {
            name = "AV1";
        } else {
            name = "unknown";
        }
        if (!"unknown".equals(name) && (videoFormat & MoonBridge.VIDEO_FORMAT_MASK_10BIT) != 0) {
            return name + " 10bit";
        }
        return name;
    }

    // §LOGDROP：摘要維持一行。stage 來自連線層，不把換行放進報告開頭。
    private static String safeStage(String stage) {
        StringBuilder out = new StringBuilder(Math.min(stage.length(), 32));
        for (int i = 0; i < stage.length() && out.length() < 32; i++) {
            char c = stage.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                out.append(c);
            }
        }
        return out.length() == 0 ? "unknown" : out.toString();
    }

    @Override
    public void stageFailed(final String stage, final int portFlags, final int errorCode) {
        // §LOGDROP：先記結束原因。對話框關掉之後才會 onStop → stopConnection。
        if (streamEndError == Integer.MIN_VALUE) {
            streamEndError = errorCode;
            streamEndStage = stage;
        }
        // §CONSOLE-LOGIN：host 已用 HTTP 拒絕。這時去做連接埠測試會把登入對話框拖慢，而且失敗原因不是連接埠被擋。
        if (hostNeedsLogin) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    showConsoleLoginFailure(stage, errorCode);
                }
            });
            return;
        }
        // Perform a connection test if the failure could be due to a blocked port
        // This does network I/O, so don't do it on the main thread.
        final int portTestResult = MoonBridge.testClientConnectivity(ServerHelper.CONNECTION_TEST_SERVER, 443, portFlags);

        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (spinner != null) {
                    spinner.dismiss();
                    spinner = null;
                }
                revealPhoneForFailureDialog();

                if (!displayedFailureDialog) {
                    displayedFailureDialog = true;
                    LimeLog.severe(stage + " failed: " + errorCode);

                    // If video initialization failed and the surface is still valid, display extra information for the user
                    if (stage.contains("video") && videoView.getHolder().getSurface().isValid()) {
                        Toast.makeText(Game.this, getResources().getText(R.string.video_decoder_init_failed), Toast.LENGTH_LONG).show();
                    }

                    String dialogText = getResources().getString(R.string.conn_error_msg) + " " + stage +" (error "+errorCode+")";

                    if (portFlags != 0) {
                        dialogText += "\n\n" + getResources().getString(R.string.check_ports_msg) + "\n" +
                                MoonBridge.stringifyPortFlags(portFlags, "\n");
                    }

                    if (portTestResult != MoonBridge.ML_TEST_RESULT_INCONCLUSIVE && portTestResult != 0)  {
                        dialogText += "\n\n" + getResources().getString(R.string.nettest_text_blocked);
                    }

                    Dialog.displayDialog(Game.this, getResources().getString(R.string.conn_error_title), dialogText, true);
                }
            }
        });
    }

    @Override
    public void connectionTerminated(final int errorCode) {
        // §LOGDROP：先記結束碼。onStop 可能在這個回呼的 UI 工作之前就 stopConnection。
        if (streamEndError == Integer.MIN_VALUE) {
            streamEndError = errorCode;
        }
        // Perform a connection test if the failure could be due to a blocked port
        // This does network I/O, so don't do it on the main thread.
        final int portFlags = MoonBridge.getPortFlagsFromTerminationErrorCode(errorCode);
        final int portTestResult = MoonBridge.testClientConnectivity(ServerHelper.CONNECTION_TEST_SERVER,443, portFlags);

        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                releaseVrEmulation();
                revealPhoneForFailureDialog();

                // Let the display go to sleep now
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

                // Stop processing controller input
                controllerHandler.stop();

                // Ungrab input
                setInputGrabState(false);

                if (!displayedFailureDialog) {
                    displayedFailureDialog = true;
                    LimeLog.severe("Connection terminated: " + errorCode);
                    stopConnection(errorCode == MoonBridge.ML_ERROR_GRACEFUL_TERMINATION);

                    // Display the error dialog if it was an unexpected termination.
                    // Otherwise, just finish the activity immediately.
                    if (errorCode != MoonBridge.ML_ERROR_GRACEFUL_TERMINATION) {
                        String message;

                        if (portTestResult != MoonBridge.ML_TEST_RESULT_INCONCLUSIVE && portTestResult != 0) {
                            // If we got a blocked result, that supersedes any other error message
                            message = getResources().getString(R.string.nettest_text_blocked);
                        }
                        else {
                            switch (errorCode) {
                                case MoonBridge.ML_ERROR_NO_VIDEO_TRAFFIC:
                                    message = getResources().getString(R.string.no_video_received_error);
                                    break;

                                case MoonBridge.ML_ERROR_NO_VIDEO_FRAME:
                                    message = getResources().getString(R.string.no_frame_received_error);
                                    break;

                                case MoonBridge.ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
                                case MoonBridge.ML_ERROR_PROTECTED_CONTENT:
                                    message = getResources().getString(R.string.early_termination_error);
                                    break;

                                case MoonBridge.ML_ERROR_FRAME_CONVERSION:
                                    message = getResources().getString(R.string.frame_conversion_error);
                                    break;

                                default:
                                    String errorCodeString;
                                    // We'll assume large errors are hex values
                                    if (Math.abs(errorCode) > 1000) {
                                        errorCodeString = Integer.toHexString(errorCode);
                                    }
                                    else {
                                        errorCodeString = Integer.toString(errorCode);
                                    }
                                    message = getResources().getString(R.string.conn_terminated_msg) + "\n\n" +
                                            getResources().getString(R.string.error_code_prefix) + " " + errorCodeString;
                                    break;
                            }
                        }

                        if (portFlags != 0) {
                            message += "\n\n" + getResources().getString(R.string.check_ports_msg) + "\n" +
                                    MoonBridge.stringifyPortFlags(portFlags, "\n");
                        }

                        Dialog.displayDialog(Game.this, getResources().getString(R.string.conn_terminated_title),
                                message, true);
                    }
                    else {
                        finish();
                    }
                }
            }
        });
    }

    @Override
    public void connectionStatusUpdate(final int connectionStatus) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (prefConfig.disableWarnings) {
                    return;
                }

                if (connectionStatus == MoonBridge.CONN_STATUS_POOR) {
                    if (prefConfig.bitrate > 5000) {
                        notificationOverlayView.setText(getResources().getString(R.string.slow_connection_msg));
                    }
                    else {
                        notificationOverlayView.setText(getResources().getString(R.string.poor_connection_msg));
                    }

                    requestedNotificationOverlayVisibility = View.VISIBLE;

                    // VipleStream v17: notify FRUC to pause interpolation
                    if (decoderRenderer != null) {
                        decoderRenderer.setConnectionPoor(true);
                    }
                }
                else if (connectionStatus == MoonBridge.CONN_STATUS_OKAY) {
                    requestedNotificationOverlayVisibility = View.GONE;

                    // VipleStream v17: resume FRUC interpolation
                    if (decoderRenderer != null) {
                        decoderRenderer.setConnectionPoor(false);
                    }
                }

                if (!isHidingOverlays) {
                    notificationOverlayView.setVisibility(requestedNotificationOverlayVisibility);
                }
            }
        });
    }

    // §SC-HID（2026-09-07）：gen-2 Steam Controller 走藍牙時是標準 HID over GATT，
    // Android 的 Bluetooth 堆疊把 HID 服務 0x1812 的 Report 特徵列為受限（非系統 App
    // 讀寫被拒），且它的描述符只有滑鼠＋鍵盤＋vendor collection、沒有 gamepad——
    // 系統只會建出 KEYBOARD|MOUSE 的 InputDevice（Pixel 5 實測）。所以藍牙 SC
    // 只能以 lizard mode 的滑鼠鍵盤被轉發，原生手把轉發需要 USB-C 直連或
    // Puck/Nereid 接收器經 OTG（UsbDriverService → SteamControllerDriver）。
    private boolean btSteamControllerWarned;
    private void warnIfBluetoothSteamController() {
        if (btSteamControllerWarned) {
            return;
        }
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice dev = InputDevice.getDevice(id);
            if (dev == null) {
                continue;
            }
            if (dev.getVendorId() == 0x28de && dev.getProductId() == 0x1303) {
                btSteamControllerWarned = true;
                LimeLog.warning("[SC-HID] Bluetooth Steam Controller detected as InputDevice '"+dev.getName()+
                        "' (sources=0x"+Integer.toHexString(dev.getSources())+") - Android blocks raw HID over "+
                        "GATT for apps; native passthrough needs USB-C or the Puck receiver via OTG");
                Toast.makeText(this, getString(R.string.sc_hid_bt_unsupported), Toast.LENGTH_LONG).show();
                break;
            }
        }
    }

    @Override
    public void connectionStarted() {
        // §LOGDROP：時長從串流真正起來算。這個回呼不一定在主執行緒。
        if (streamLiveElapsed == 0L) {
            streamLiveElapsed = SystemClock.elapsedRealtime();
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (spinner != null) {
                    spinner.dismiss();
                    spinner = null;
                }

                // VipleStream §REVIEW-1111: this runnable can run after the activity was told to finish (the user left
                // while connecting). Marking the stream connected then re-grabbed the pointer and restarted the file
                // transfer poller on a dead activity.
                if (isFinishing() || isDestroyed()) {
                    return;
                }

                connected = true;
                connecting = false;
                if (!glassesVr) {
                    // §VITURE：串流已經起來才切 3D。更早送的話，外接畫面會在連線前消失。
                    maybeAutoSwitchSbs();
                    refreshBeast2dHint();
                }
                if (vrEmulate && vrEmuHandle != 0) {
                    QxrNative.nativeEmuStart(vrEmuHandle);
                }
                updatePipAutoEnter();

                // Hide the mouse cursor now after a short delay.
                // Doing it before dismissing the spinner seems to be undone
                // when the spinner gets displayed. On Android Q, even now
                // is too early to capture. We will delay a second to allow
                // the spinner to dismiss before capturing.
                Handler h = new Handler();
                h.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (connected && !isFinishing() && !isDestroyed()) {
                            setInputGrabState(true);
                        }
                    }
                }, 500);

                // Keep the display on
                getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

                // Update GameManager state to indicate we're in game
                UiHelper.notifyStreamConnected(Game.this);

                hideSystemUi(1000);

                // VipleStream §N — 連線建立後啟動 in-stream file transfer client
                startFileTransferClient();

                // §SC-HID：藍牙 Steam Controller 在 Android 無法原生轉發，明講
                warnIfBluetoothSteamController();
            }
        });

        // Report this shortcut being used (off the main thread to prevent ANRs)
        ComputerDetails computer = new ComputerDetails();
        computer.name = pcName;
        computer.uuid = Game.this.getIntent().getStringExtra(EXTRA_PC_UUID);
        ShortcutHelper shortcutHelper = new ShortcutHelper(this);
        shortcutHelper.reportComputerShortcutUsed(computer);
        if (appName != null) {
            // This may be null if launched from the "Resume Session" PC context menu item
            shortcutHelper.reportGameLaunched(computer, app);
        }
    }

    @Override
    public void displayMessage(final String message) {
        // §CONSOLE-LOGIN：拒絕字串比 stageFailed 早到。旗標要在這條連線執行緒先寫上，
        // 後面的 stageFailed 才看得到，不會先畫出一般錯誤對話框。
        if (isConsoleLoginMessage(message)) {
            hostNeedsLogin = true;
            return;
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(Game.this, message, Toast.LENGTH_LONG).show();
            }
        });
    }

    // §CONSOLE-LOGIN：host 的 status_message 會包在 "Host PC returned error: ..." 裡。
    private static boolean isConsoleLoginMessage(String message) {
        return message != null && (message.contains("VR_NEEDS_LOGIN") || message.contains("NEEDS_LOGIN"));
    }

    private void showConsoleLoginFailure(String stage, int errorCode) {
        if (spinner != null) {
            spinner.dismiss();
            spinner = null;
        }
        revealPhoneForFailureDialog();
        if (displayedFailureDialog || isFinishing() || isDestroyed()) {
            return;
        }
        displayedFailureDialog = true;
        LimeLog.severe(stage + " failed: " + errorCode);
        LimeLog.info("[VIPLE-CONSOLE] host asked for Windows sign-in");
        showConsoleLoginDialog();
    }

    private void showConsoleLoginDialog() {
        if (consoleLoginDialog != null && consoleLoginDialog.isShowing()) {
            return;
        }
        if (isFinishing() || isDestroyed()) {
            return;
        }
        consoleLoginDialog = new AlertDialog.Builder(this)
                .setTitle(R.string.pc_needs_login_title)
                .setMessage(R.string.pc_needs_login_msg)
                .setCancelable(false)
                .setPositiveButton(R.string.pc_needs_login_yes, (dialog, which) -> {
                    // 對話框關掉時的 onDismiss 不能再 finish，否則非同步找 app 的途中 Activity 會先被拆掉。
                    consoleLoginStarting = true;
                    beginConsoleLoginStream();
                })
                .setNegativeButton(R.string.pc_needs_login_no, (dialog, which) -> finish())
                .create();
        // Builder.setOnDismissListener 是 API 33 才有。minSdk 28 要設在 Dialog 上。
        consoleLoginDialog.setOnDismissListener(dialog -> {
            consoleLoginDialog = null;
            if (!consoleLoginStarting && !isFinishing()) {
                finish();
            }
        });
        consoleLoginDialog.setCanceledOnTouchOutside(false);
        consoleLoginDialog.show();
    }

    private void beginConsoleLoginStream() {
        if (consoleLoginServiceBound || isFinishing() || isDestroyed()) {
            return;
        }
        consoleLoginServiceConnection = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder service) {
                if (!consoleLoginServiceBound || isFinishing() || isDestroyed()) {
                    return;
                }
                final ComputerManagerService.ComputerManagerBinder binder =
                        (ComputerManagerService.ComputerManagerBinder) service;
                // waitForReady 會等到探索服務接上，不能佔著主執行緒。
                new Thread(() -> {
                    binder.waitForReady();
                    final ComputerDetails computer = computerForConsoleLogin(binder);
                    final NvApp target = pickConsoleLoginApp(computer);
                    runOnUiThread(() -> launchConsoleLoginApp(binder, computer, target));
                }).start();
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
            }
        };
        consoleLoginServiceBound = bindService(new Intent(this, ComputerManagerService.class),
                consoleLoginServiceConnection, Context.BIND_AUTO_CREATE);
        if (!consoleLoginServiceBound) {
            LimeLog.warning("[VIPLE-CONSOLE] computer manager bind failed");
            finish();
        }
    }

    private void launchConsoleLoginApp(ComputerManagerService.ComputerManagerBinder binder,
                                       ComputerDetails computer, NvApp target) {
        if (isFinishing() || isDestroyed()) {
            unbindConsoleLoginService();
            return;
        }
        if (binder == null || target == null || computer == null || computer.activeAddress == null) {
            LimeLog.warning("[VIPLE-CONSOLE] no Desktop app to sign in with");
            unbindConsoleLoginService();
            finish();
            return;
        }
        LimeLog.info("[VIPLE-CONSOLE] starting sign-in stream app=" + target.getAppName()
                + " id=" + target.getAppId());
        // 這次 /launch 在 HTTP 就被拒，連線執行緒已經返回，MoonBridge 還沒開始。
        // onStop 看到 connecting 仍為 true 會非同步送 /cancel，剛好打到下一筆 Desktop /launch。
        connecting = false;
        connected = false;
        // Game 是 singleTask，而且沒有 onNewIntent。還活著時 doStart 會送回這個 Activity，接著被 finish 一起關掉。
        // 先標成 finishing，系統才會另建一個 Game。createStartIntent 不會帶 glasses_vr，登入串流維持一般畫面。
        finish();
        ServerHelper.doStart(this, target, computer, binder);
        unbindConsoleLoginService();
    }

    private void unbindConsoleLoginService() {
        if (!consoleLoginServiceBound) {
            return;
        }
        consoleLoginServiceBound = false;
        try {
            unbindService(consoleLoginServiceConnection);
        } catch (IllegalArgumentException e) {
            LimeLog.warning("[VIPLE-CONSOLE] unbind failed");
        }
    }

    // §CONSOLE-LOGIN：資料庫那份沒有這次連線的位址，狀態也常是 UNKNOWN。doStart 會把 OFFLINE 或沒有位址當成離線。
    private ComputerDetails computerForConsoleLogin(ComputerManagerService.ComputerManagerBinder binder) {
        Intent intent = getIntent();
        String uuid = intent.getStringExtra(EXTRA_PC_UUID);
        ComputerDetails computer = null;
        if (binder != null && uuid != null) {
            ComputerDetails live = binder.getComputer(uuid);
            if (live != null) {
                computer = new ComputerDetails(live);
            }
        }
        if (computer == null) {
            computer = new ComputerDetails();
            computer.uuid = uuid;
            computer.name = intent.getStringExtra(EXTRA_PC_NAME);
        }
        if (computer.activeAddress == null) {
            String host = intent.getStringExtra(EXTRA_HOST);
            int port = intent.getIntExtra(EXTRA_PORT, 0);
            if (host != null && port > 0) {
                computer.activeAddress = new ComputerDetails.AddressTuple(host, port);
            }
        }
        if (computer.httpsPort == 0) {
            computer.httpsPort = intent.getIntExtra(EXTRA_HTTPS_PORT, 0);
        }
        if (computer.name == null) {
            computer.name = intent.getStringExtra(EXTRA_PC_NAME);
        }
        if (computer.uuid == null) {
            computer.uuid = uuid;
        }
        if (computer.serverCert == null) {
            byte[] der = intent.getByteArrayExtra(EXTRA_SERVER_CERT);
            if (der != null) {
                try {
                    computer.serverCert = (X509Certificate) CertificateFactory.getInstance("X.509")
                            .generateCertificate(new ByteArrayInputStream(der));
                } catch (CertificateException e) {
                    LimeLog.warning("[VIPLE-CONSOLE] server cert decode failed");
                }
            }
        }
        if (computer.stunAddress == null && intent.hasExtra(EXTRA_STUN_ADDRESS)) {
            computer.stunAddress = intent.getStringExtra(EXTRA_STUN_ADDRESS);
            computer.stunPunchPort = intent.getIntExtra(EXTRA_STUN_PUNCH_PORT, 0);
        }
        if (computer.serverCodecModeSupport == 0) {
            computer.serverCodecModeSupport = intent.getIntExtra(EXTRA_SERVER_CODEC_MODE_SUPPORT, 0);
        }
        if (computer.activeAddress != null) {
            computer.state = ComputerDetails.State.ONLINE;
        }
        return computer;
    }

    private NvApp pickConsoleLoginApp(ComputerDetails computer) {
        if (computer == null) {
            return null;
        }
        List<NvApp> apps = appsFromRaw(computer.rawAppList);
        if (apps == null || apps.isEmpty()) {
            apps = appsFromCache(computer.uuid);
        }
        if (apps == null || apps.isEmpty()) {
            return null;
        }
        for (NvApp candidate : apps) {
            if (candidate != null && "Desktop".equalsIgnoreCase(candidate.getAppName())) {
                return candidate;
            }
        }
        for (NvApp candidate : apps) {
            if (candidate != null && !candidate.isVr()) {
                LimeLog.info("[VIPLE-CONSOLE] Desktop app missing; using first non-VR app id=" + candidate.getAppId());
                return candidate;
            }
        }
        return null;
    }

    private List<NvApp> appsFromRaw(String raw) {
        if (raw == null || raw.isEmpty()) {
            return null;
        }
        try {
            return NvHTTP.getAppListByReader(new StringReader(raw));
        } catch (Exception e) {
            LimeLog.warning("[VIPLE-CONSOLE] applist parse failed");
            return null;
        }
    }

    private List<NvApp> appsFromCache(String uuid) {
        if (uuid == null || uuid.isEmpty()) {
            return null;
        }
        try {
            String raw = CacheHelper.readInputStreamToString(
                    CacheHelper.openCacheFileForInput(getCacheDir(), "applist", uuid));
            return appsFromRaw(raw);
        } catch (Exception e) {
            LimeLog.info("[VIPLE-CONSOLE] cached applist unavailable");
            return null;
        }
    }

    @Override
    public void displayTransientMessage(final String message) {
        if (!prefConfig.disableWarnings) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    Toast.makeText(Game.this, message, Toast.LENGTH_LONG).show();
                }
            });
        }
    }

    // §REVIEW-1104：下載成功是 Received:，要算終態，否則左下角進度條留到串流結束
    private static boolean isFileTransferTerminalMessage(String message) {
        return message != null && (
                message.startsWith("Saved")
             || message.startsWith("Sent")
             || message.startsWith("Received:")
             || message.startsWith("File transfer cancelled")
             || message.contains("failed"));
    }

    /**
     * VipleStream §N.5 (2026-05-14) — File transfer 進度 overlay 顯示.
     *
     * 替代原 Toast / Notification 不打擾 stream 視覺. Pattern：
     *   - 整個 transfer 期間 ("Receiving file: ... 5%" / "Sending: ... 50%")
     *     持續可見在左下角，使用者可以隨時看進度
     *   - 完成/失敗訊息 ("Saved: ..." / "Sent: ..." / "File transfer cancelled")
     *     顯示後 3 秒自動 fade-out 避免長期擋畫面
     *   - 進度中如再收到 message (next 5% bucket) 取消既有 fade-out timer 重置
     *
     * 不跟 notificationOverlay (connection poor/slow) 衝突 — 獨立 view 左下 vs
     * 右上.
     */
    private void showFileTransferOverlay(final String message) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (fileTransferOverlayView == null) return;
                fileTransferOverlayView.setText(message);
                if (!isHidingOverlays) {
                    fileTransferOverlayView.setVisibility(View.VISIBLE);
                }
                // 取消既有 hide timer (進度更新時重置)
                Runnable prev = fileTransferHideTask.getAndSet(null);
                if (prev != null) fileTransferOverlayView.removeCallbacks(prev);
                // 終態 (完成 / 取消 / 失敗) 3 秒後 hide；進行中保持顯示
                // §REVIEW-1104：下載成功的文案是 Received:，原本對不上，進度條會留到串流結束
                boolean isTerminal = isFileTransferTerminalMessage(message);
                if (isTerminal) {
                    Runnable hideTask = () -> fileTransferOverlayView.setVisibility(View.GONE);
                    fileTransferHideTask.set(hideTask);
                    fileTransferOverlayView.postDelayed(hideTask, 3000);
                }
            }
        });
    }

    // VipleStream §M.parity U6 (2026-05-20). Confirm-dialog wrapped around
    // FileTransferClient.cancelCurrent(). PC parity: moonlight-qt v1.4.103
    // hotkey Ctrl+Alt+Shift+T does the same thing without confirmation,
    // but mobile UX should guard accidental taps on the overlay.
    private void promptCancelFileTransfer() {
        if (fileTransferClient == null) return;
        new AlertDialog.Builder(this)
                .setTitle(R.string.title_cancel_file_transfer)
                .setMessage(R.string.message_cancel_file_transfer)
                .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                    if (fileTransferClient != null) {
                        fileTransferClient.cancelCurrent();
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    @Override
    public void rumble(short controllerNumber, short lowFreqMotor, short highFreqMotor) {
        LimeLog.info(String.format((Locale)null, "Rumble on gamepad %d: %04x %04x", controllerNumber, lowFreqMotor, highFreqMotor));

        controllerHandler.handleRumble(controllerNumber, lowFreqMotor, highFreqMotor);
    }

    @Override
    public void rumbleTriggers(short controllerNumber, short leftTrigger, short rightTrigger) {
        LimeLog.info(String.format((Locale)null, "Rumble on gamepad triggers %d: %04x %04x", controllerNumber, leftTrigger, rightTrigger));

        controllerHandler.handleRumbleTriggers(controllerNumber, leftTrigger, rightTrigger);
    }

    @Override
    public void setHdrMode(boolean enabled, byte[] hdrMetadata) {
        LimeLog.info("Display HDR mode: " + (enabled ? "enabled" : "disabled"));
        decoderRenderer.setHdrMode(enabled, hdrMetadata);
    }

    @Override
    public void setMotionEventState(short controllerNumber, byte motionType, short reportRateHz) {
        controllerHandler.handleSetMotionEventState(controllerNumber, motionType, reportRateHz);
    }

    @Override
    public void setControllerLED(short controllerNumber, byte r, byte g, byte b) {
        controllerHandler.handleSetControllerLED(controllerNumber, r, g, b);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (externalDisplayLossHandled) {
            // §EXT-DISPLAY：眼鏡已經拔掉。這個 Surface 正在被銷毀，不能再拿它開連線。
            return;
        }
        if (externalDisplayAwaiting && externalReconnectPending && !attemptedConnection
                && videoView != null && holder == videoView.getHolder()) {
            // §EXT-DISPLAY：連線還沒綁過 Surface。這張就是重新接上的，下面照舊啟動，解析度與 fps 不變。
            noteExternalDisplayBackBeforeStart();
        }
        if (!surfaceCreated) {
            throw new IllegalStateException("Surface changed before creation!");
        }

        if (!attemptedConnection) {
            attemptedConnection = true;

            // Update GameManager state to indicate we're "loading" while connecting
            UiHelper.notifyStreamConnecting(Game.this);

            decoderRenderer.setRenderTarget(holder);
            startQxrVideoTestIfRequested();
            startGlassesWarpIfEnabled(holder);
            // §REVIEW-1104：connecting 原本從來沒被設成 true，連線建立前離開時 stopConnection() 什麼都不做，
            // 連線照樣在背景建起來。NvConnection.stop() 已處理「還沒拿到 permit 就被停」的情況
            connecting = true;
            if (streamAttemptElapsed == 0L) {
                streamAttemptElapsed = SystemClock.elapsedRealtime();
            }
            conn.start(new AndroidAudioRenderer(Game.this, prefConfig.enableAudioFx),
                    decoderRenderer, Game.this);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        float desiredFrameRate;

        surfaceCreated = true;

        if (externalReconnectPending && attemptedConnection
                && videoView != null && holder == videoView.getHolder()) {
            // §EXT-DISPLAY：串流 fps 是連線參數。這裡再 setFrameRate 會用舊的 desiredRefreshRate 去推新的面板模式。
            completeExternalReconnect(holder);
            return;
        }

        // Android will pick the lowest matching refresh rate for a given frame rate value, so we want
        // to report the true FPS value if refresh rate reduction is enabled. We also report the true
        // FPS value if there's no suitable matching refresh rate. In that case, Android could try to
        // select a lower refresh rate that avoids uneven pull-down (ex: 30 Hz for a 60 FPS stream on
        // a display that maxes out at 50 Hz).
        if (mayReduceRefreshRate() || desiredRefreshRate < prefConfig.fps) {
            desiredFrameRate = prefConfig.fps;
        }
        else {
            // Otherwise, we will pretend that our frame rate matches the refresh rate we picked in
            // prepareDisplayForRendering(). This will usually be the highest refresh rate that our
            // frame rate evenly divides into, which ensures the lowest possible display latency.
            desiredFrameRate = desiredRefreshRate;
        }

        // Tell the OS about our frame rate to allow it to adapt the display refresh rate appropriately
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // We want to change frame rate even if it's not seamless, since prepareDisplayForRendering()
            // will not set the display mode on S+ if it only differs by the refresh rate. It depends
            // on us to trigger the frame rate switch here.
            // §QUEST-2D：Quest（12L，API 32+）走這支，不走下面的 R。參數合法時這只是提示，
            // 合成器不支援會忽略；非法參數才 IllegalArgumentException，surface 已釋放才 IllegalStateException。
            // Horizon 未在實機確認，所以 Quest 多接這兩個例外，避免 surfaceCreated 把串流打掛。
            // 非 Quest 仍是下面原本那一行。
            if (QuestSupport.isQuest(this)) {
                try {
                    LimeLog.info("[VIPLE-QUEST-2D] setFrameRate " + desiredFrameRate);
                    holder.getSurface().setFrameRate(desiredFrameRate,
                            Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                            Surface.CHANGE_FRAME_RATE_ALWAYS);
                } catch (IllegalArgumentException | IllegalStateException e) {
                    LimeLog.warning("[VIPLE-QUEST-2D] setFrameRate failed: " + e);
                }
            }
            else {
                holder.getSurface().setFrameRate(desiredFrameRate,
                        Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                        Surface.CHANGE_FRAME_RATE_ALWAYS);
            }
        }
        else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // §QUEST-2D：Quest 是 API 32 以上，不會走到這支。參數規則與上面相同，原本就不會因「裝置沒這個能力」而丟例外。
            holder.getSurface().setFrameRate(desiredFrameRate,
                    Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (externalDisplayActive && videoView != null && holder != videoView.getHolder()) {
            // §EXT-DISPLAY：重新接上之後，舊的外接 Surface 才被系統拆掉。那張已經不是解碼器的輸出。
            LimeLog.info("[VIPLE-EXTDISP] ignoring destruction of a previous external surface");
            return;
        }
        if (externalDisplayAwaiting || externalReconnectPending) {
            // §EXT-DISPLAY：寬限期內這張 Surface 會消失。連線要留著，輸出已經改到暫存 Surface。
            if (!surfaceCreated) {
                LimeLog.warning("[VIPLE-EXTDISP] surface destroyed before creation");
            }
            else {
                LimeLog.info("[VIPLE-EXTDISP] external surface destroyed during grace; stream stays up");
            }
            return;
        }
        if (!surfaceCreated) {
            if (externalDisplayActive || externalDisplayLossHandled) {
                // §EXT-DISPLAY：外接 Surface 可能在 surfaceCreated 之前就被拆掉。手機那條路仍要丟例外。
                LimeLog.warning("[VIPLE-EXTDISP] surface destroyed before creation");
                return;
            }
            throw new IllegalStateException("Surface destroyed before creation!");
        }

        if (attemptedConnection) {
            // §VITURE-WARP：這張 Surface 真的消失。先拆 EGL window，SurfaceTexture 仍留給解碼器直到 conn.stop()。
            detachGlassesWarpOutput();
            // Let the decoder know immediately that the surface is gone
            decoderRenderer.prepareForStop();

            if (connected) {
                stopConnection();
            }
        }
    }

    @Override
    public void mouseMove(int deltaX, int deltaY) {
        conn.sendMouseMove((short) deltaX, (short) deltaY);
    }

    @Override
    public void mouseButtonEvent(int buttonId, boolean down) {
        byte buttonIndex;

        switch (buttonId)
        {
        case EvdevListener.BUTTON_LEFT:
            buttonIndex = MouseButtonPacket.BUTTON_LEFT;
            break;
        case EvdevListener.BUTTON_MIDDLE:
            buttonIndex = MouseButtonPacket.BUTTON_MIDDLE;
            break;
        case EvdevListener.BUTTON_RIGHT:
            buttonIndex = MouseButtonPacket.BUTTON_RIGHT;
            break;
        case EvdevListener.BUTTON_X1:
            buttonIndex = MouseButtonPacket.BUTTON_X1;
            break;
        case EvdevListener.BUTTON_X2:
            buttonIndex = MouseButtonPacket.BUTTON_X2;
            break;
        default:
            LimeLog.warning("Unhandled button: "+buttonId);
            return;
        }

        if (down) {
            conn.sendMouseButtonDown(buttonIndex);
        }
        else {
            conn.sendMouseButtonUp(buttonIndex);
        }
    }

    @Override
    public void mouseVScroll(byte amount) {
        conn.sendMouseScroll(amount);
    }

    @Override
    public void mouseHScroll(byte amount) {
        conn.sendMouseHScroll(amount);
    }

    @Override
    public void keyboardEvent(boolean buttonDown, short keyCode) {
        short keyMap = keyboardTranslator.translate(keyCode, -1);
        if (keyMap != 0) {
            // handleSpecialKeys() takes the Android keycode
            if (handleSpecialKeys(keyCode, buttonDown)) {
                return;
            }

            if (buttonDown) {
                conn.sendKeyboardInput(keyMap, KeyboardPacket.KEY_DOWN, getModifierState(), (byte)0);
            }
            else {
                conn.sendKeyboardInput(keyMap, KeyboardPacket.KEY_UP, getModifierState(), (byte)0);
            }
        }
    }

    @Override
    public void onSystemUiVisibilityChange(int visibility) {
        // Don't do anything if we're not connected
        if (!connected) {
            return;
        }

        // This flag is set for all devices
        if ((visibility & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0) {
            hideSystemUi(2000);
        }
        else if ((visibility & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION) == 0) {
            hideSystemUi(2000);
        }
    }

    @Override
    public void onPerfUpdate(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                performanceOverlayView.setText(text);
            }
        });
    }

    @Override
    public void onUsbPermissionPromptStarting() {
        // Disable PiP auto-enter while the USB permission prompt is on-screen. This prevents
        // us from entering PiP while the user is interacting with the OS permission dialog.
        suppressPipRefCount++;
        updatePipAutoEnter();
    }

    @Override
    public void onUsbPermissionPromptCompleted() {
        suppressPipRefCount--;
        updatePipAutoEnter();
    }

    @Override
    public boolean onKey(View view, int keyCode, KeyEvent keyEvent) {
        switch (keyEvent.getAction()) {
            case KeyEvent.ACTION_DOWN:
                return handleKeyDown(keyEvent);
            case KeyEvent.ACTION_UP:
                return handleKeyUp(keyEvent);
            case KeyEvent.ACTION_MULTIPLE:
                return handleKeyMultiple(keyEvent);
            default:
                return false;
        }
    }

    private void applyVrEmulatePrefs(PreferenceConfiguration pref, Intent intent) {
        applySharedVrPrefs(pref, intent, "1440x1440", 72, false);
    }

    private void applyGlassesVrPrefs(PreferenceConfiguration pref, Intent intent) {
        applySharedVrPrefs(pref, intent, "1920x1080", 60, true);
    }

    /**
     * §VITURE：vr_emulate 與 glasses_vr 共用這段。imuPose 只換預設單眼、更新率與姿態來源，
     * 串流尺寸、HDR／補幀與 codec 的處理維持同一條路。
     */
    private void applySharedVrPrefs(PreferenceConfiguration pref, Intent intent,
                                    String defaultEye, int defaultHz, boolean imuPose) {
        vrEmulate = true;
        glassesVr = imuPose;
        String eye = intent.getStringExtra("vr_eye");
        if (eye == null || eye.isEmpty()) {
            eye = defaultEye;
        }
        int width = 0;
        int height = 0;
        boolean parsed = false;
        String lowered = eye.toLowerCase(Locale.US);
        int split = lowered.indexOf('x');
        if (split > 0 && split + 1 < lowered.length()) {
            try {
                width = Integer.parseInt(lowered.substring(0, split).trim());
                height = Integer.parseInt(lowered.substring(split + 1).trim());
                parsed = width > 0 && height > 0;
            } catch (NumberFormatException ignored) {
                parsed = false;
            }
        }
        String logTag = imuPose ? "[VIPLE-VITURE]" : "[VIPLE-QXR-EMU]";
        if (!parsed) {
            LimeLog.warning(logTag + " invalid vr_eye '" + eye + "'; using " + defaultEye);
            String fallback = defaultEye.toLowerCase(Locale.US);
            int fallbackSplit = fallback.indexOf('x');
            width = Integer.parseInt(fallback.substring(0, fallbackSplit).trim());
            height = Integer.parseInt(fallback.substring(fallbackSplit + 1).trim());
        }
        vrEyeWidth = width;
        vrEyeHeight = height;
        // 外接模式挑選與手機長寬比都在這之後才跑。拖到後面才寫的話，會鎖到一般 2D 的比例。
        pref.width = width * 2;
        pref.height = height;
        int hz = intent.getIntExtra("vr_hz", defaultHz);
        if (hz <= 0) {
            LimeLog.warning(logTag + " invalid vr_hz " + hz + "; using " + defaultHz);
            hz = defaultHz;
        }
        vrHz = hz;
        pref.fps = hz;
        if (imuPose) {
            vrMotion = "imu";
        }
        else {
            String motion = intent.getStringExtra("vr_motion");
            if (motion == null || motion.trim().isEmpty()) {
                motion = "sine";
            }
            else {
                motion = motion.trim();
            }
            vrMotion = motion;
        }
        // 模擬沒有頭盔 HDR，也不做補幀。AV1／PyroWave 不在這次的 codec 交集裡，先改回 AUTO，避免原本的提示把使用者帶去那條路。
        VrStreamDefaults.applySessionLimits(pref);
        if (imuPose) {
            // 雙眼並排只能畫在眼鏡上。偏好關掉外接顯示器時，後面會以為沒有畫面而結束。
            pref.externalDisplay = true;
        }
        if (imuPose) {
            LimeLog.info("[VIPLE-VITURE] glasses_vr eye=" + width + "x" + height + " hz=" + hz + " pose=imu");
        }
        else {
            LimeLog.info("[VIPLE-QXR-EMU] vr_emulate eye=" + width + "x" + height + " hz=" + hz + " motion=" + vrMotion);
        }
    }

    /**
     * §VITURE：單眼對角 58°、16:9。半對角 29°。
     * tan(水平半角) / tan(垂直半角) = 16/9，兩者平方和 = tan(29°)^2。
     * 填進 fovTan 的單位是 tan×10000。左右眼相同：left＝right＝水平，up＝down＝垂直。ipd 沿用建構子預設。
     */
    private static void applyBeastFov(VrLaunchConfig launch) {
        double ratio = 16.0 / 9.0;
        double tanHalfDiag = Math.tan(Math.toRadians(29.0));
        double tanVertical = Math.sqrt((tanHalfDiag * tanHalfDiag) / (1.0 + ratio * ratio));
        double tanHorizontal = tanVertical * ratio;
        int horizontal = (int) Math.round(tanHorizontal * 10000.0);
        int vertical = (int) Math.round(tanVertical * 10000.0);
        for (int eye = 0; eye < 2; eye++) {
            launch.fovTan[eye * 4] = horizontal;
            launch.fovTan[eye * 4 + 1] = horizontal;
            launch.fovTan[eye * 4 + 2] = vertical;
            launch.fovTan[eye * 4 + 3] = vertical;
        }
        LimeLog.info("[VIPLE-VITURE] fovTan horizontal=" + horizontal + " vertical=" + vertical);
    }

    private void revealPhoneForFailureDialog() {
        if (!externalDisplayActive && !externalDisplayAwaiting && !glassesVr) {
            return;
        }
        hideExternalTouchpadUi();
    }

    private void keepGlassesScreenReadable() {
        if (!glassesVr) {
            return;
        }
        // 按鈕要留在系統亮度。調暗旗標留著的話，摘下提示計時結束會再把這個視窗寫回 0。
        externalTouchpadDimmed = false;
        cancelExternalDimHintTimer();
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE;
        getWindow().setAttributes(params);
    }

    private boolean isGlasses3d(Display display) {
        if (display == null || display.getDisplayId() == Display.DEFAULT_DISPLAY) {
            return false;
        }
        if ((display.getFlags() & Display.FLAG_PRIVATE) != 0) {
            return false;
        }
        if (display.getState() != Display.STATE_ON) {
            return false;
        }
        Display.Mode mode = display.getMode();
        // §VITURE：3D 以寬度 >= 3000 判斷。高度仍要 1080，4K 電視不該被當成眼鏡面板。
        return mode != null
                && GlassesDisplaySwitcher.isStereoWidth(mode.getPhysicalWidth())
                && mode.getPhysicalHeight() == GLASSES_3D_HEIGHT;
    }

    private Display findGlasses3d() {
        DisplayManager displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        if (displayManager == null) {
            return null;
        }
        Display[] displays = displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (displays == null) {
            return null;
        }
        for (int i = 0; i < displays.length; i++) {
            if (isGlasses3d(displays[i])) {
                return displays[i];
            }
        }
        return null;
    }

    private void startGlassesBringup() {
        if (glassesBringupStarted) {
            return;
        }
        glassesBringupStarted = true;
        noteVitureBeast();
        showGlassesVrControls();
        final boolean waitFor3d = glassesWaitFor3d;
        final long handle = vrEmuHandle;
        glassesBringupThread = new Thread(new Runnable() {
            @Override
            public void run() {
                GlassesVrSession session = GlassesVrSession.open(Game.this, handle, new Runnable() {
                    @Override
                    public void run() {
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                onGlassesDetached();
                            }
                        });
                    }
                }, glassesWearListener, new GlassesVrSession.Attach() {
                    @Override
                    public boolean attach(GlassesVrSession opening) {
                        synchronized (glassesLife) {
                            if (glassesShutdown || isFinishing() || isDestroyed()) {
                                return false;
                            }
                            // push 與 glassesSession 要同一段鎖寫入，拆除才不會看到 null 卻已經在 push。
                            opening.armImuPush();
                            glassesSession = opening;
                            return true;
                        }
                    }
                });
                if (session == null) {
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            if (!glassesShutdown && !isFinishing() && !isDestroyed()) {
                                Dialog.displayDialog(Game.this, getResources().getString(R.string.conn_error_title),
                                        getResources().getString(R.string.glasses_vr_open_failed), true);
                            }
                        }
                    });
                    return;
                }
                if (glassesShutdown || isFinishing() || isDestroyed() || Thread.currentThread().isInterrupted()) {
                    return;
                }
                if (session.isBeast()) {
                    vitureBeast = true;
                    bringUpBeastDisplay(waitFor3d);
                    return;
                }
                if (!waitFor3d) {
                    VitureUsb.logI("display already 3840x1080; skip SET_DISPLAY_MODE");
                    if (!publishGlassesDisplay(0L)) {
                        // §VITURE：開串流前顯示器又離開 3D。不要改畫到手機，留在這裡等它回來。
                        VitureUsb.logI("display left 3840x1080 before present; waiting for the mode button");
                        runOnUiThread(new Runnable() {
                            @Override
                            public void run() {
                                showGlassesModePrompt();
                            }
                        });
                        waitForGlasses3d(SystemClock.elapsedRealtime(), true);
                    }
                    return;
                }
                long startedMs = SystemClock.elapsedRealtime();
                VitureGlasses.CommandResult result = session.switchTo3d();
                if (!result.ok) {
                    GlassesDisplaySwitcher.logModeRejected(result);
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            showGlassesModePrompt();
                        }
                    });
                    waitForGlasses3d(startedMs, true);
                    return;
                }
                if (waitForGlasses3d(startedMs, false)) {
                    return;
                }
                long elapsed = SystemClock.elapsedRealtime() - startedMs;
                GlassesDisplaySwitcher.logModeTimeout(result, elapsed);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        showGlassesModePrompt();
                    }
                });
                waitForGlasses3d(startedMs, true);
            }
        }, "viple-viture-bringup");
        glassesBringupThread.start();
    }

    private void bringUpBeastDisplay(boolean waitFor3d) {
        if (!waitFor3d) {
            VitureUsb.logI("display already 3840x1080; skip SET_DISPLAY_MODE");
            if (!publishGlassesDisplay(0L)) {
                // §VITURE：開串流前面板又離開 3D。不要改畫到手機，也不要送 0x32。
                VitureUsb.logI("display left 3840x1080 before present; waiting for the mode button");
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        showBeastPress3dPrompt();
                    }
                });
                if (!waitForBeast3d(SystemClock.elapsedRealtime())) {
                    failBeast3dWait();
                }
            }
            return;
        }
        VitureUsb.logI("beast panel is 2d; waiting for the glasses button");
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                showBeastPress3dPrompt();
            }
        });
        if (!waitForBeast3d(SystemClock.elapsedRealtime())) {
            failBeast3dWait();
        }
    }

    private boolean waitForBeast3d(long startedMs) {
        long deadline = startedMs + BEAST_3D_WAIT_MS;
        while (!glassesShutdown && !isFinishing() && !isDestroyed() && !glassesPresented) {
            if (findGlasses3d() != null && publishGlassesDisplay(startedMs)) {
                return true;
            }
            if (SystemClock.elapsedRealtime() >= deadline) {
                return false;
            }
            try {
                Thread.sleep(200);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return glassesPresented;
    }

    private void failBeast3dWait() {
        VitureUsb.logI("beast 3d wait timed out after " + BEAST_3D_WAIT_MS + " ms");
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (glassesShutdown || isFinishing() || isDestroyed() || glassesPresented) {
                    return;
                }
                Dialog.displayDialog(Game.this, getResources().getString(R.string.conn_error_title),
                        getResources().getString(R.string.glasses_vr_beast_timeout), true);
            }
        });
    }

    private void showBeastPress3dPrompt() {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        TextView hint = findViewById(R.id.externalDisplayHint);
        if (hint == null) {
            return;
        }
        hint.setText(R.string.glasses_vr_beast_press_3d);
        hint.setVisibility(View.VISIBLE);
    }

    private void pauseBeastGlassesOn2d() {
        if (!glassesVr || !vitureBeast || glassesShutdown || isFinishing() || isDestroyed()) {
            return;
        }
        if (!glassesBeastPausedFor2d) {
            VitureUsb.logI("beast glasses_vr paused on 2d panel; waiting for 3840");
        }
        glassesBeastPausedFor2d = true;
        // §VITURE：2D 面板還在。寬限期若繼續倒數，使用者還沒按回 3D 就會被結束。
        cancelExternalDisplayGraceTimer();
        showBeastPress3dPrompt();
    }

    private boolean beast2dPanelPresent(int removedDisplayId) {
        DisplayManager displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        if (displayManager == null) {
            return false;
        }
        Display[] displays = displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (displays == null) {
            return false;
        }
        for (int i = 0; i < displays.length; i++) {
            Display display = displays[i];
            if (display == null || display.getDisplayId() == removedDisplayId || isGlasses3d(display)) {
                continue;
            }
            if (display.getDisplayId() == Display.DEFAULT_DISPLAY
                    || (display.getFlags() & Display.FLAG_PRIVATE) != 0
                    || display.getState() != Display.STATE_ON) {
                continue;
            }
            Display.Mode mode = display.getMode();
            if (mode != null && mode.getPhysicalWidth() > 0) {
                return true;
            }
        }
        return false;
    }

    private void onBeast2dPanelRemoved() {
        glassesBeastPausedFor2d = false;
        if (externalDisplayLossHandled || isFinishing() || isDestroyed()) {
            return;
        }
        if (!externalDisplayAwaiting) {
            beginExternalDisplayGrace();
            return;
        }
        // §VITURE：2D 面板被拆是切回 3D 的前一步，不是拔線。重新計時，沒有再出現才結束。
        cancelExternalDisplayGraceTimer();
        externalDisplayHandler.postDelayed(externalDisplayGraceRunnable, EXTERNAL_DISPLAY_GRACE_MS);
        showExternalWaitingHint();
        LimeLog.info("[VIPLE-EXTDISP] beast 2d panel removed; waiting for the next display");
    }

    private boolean waitForGlasses3d(long startedMs, boolean forever) {
        long deadline = forever ? Long.MAX_VALUE : startedMs + GlassesDisplaySwitcher.MODE_WAIT_MS;
        while (!glassesShutdown && !isFinishing() && !isDestroyed() && !glassesPresented) {
            if (findGlasses3d() != null && publishGlassesDisplay(startedMs)) {
                return true;
            }
            if (!forever && SystemClock.elapsedRealtime() >= deadline) {
                return false;
            }
            try {
                Thread.sleep(200);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return glassesPresented;
    }

    private boolean publishGlassesDisplay(final long startedMs) {
        final boolean[] ok = new boolean[1];
        final Object gate = new Object();
        final boolean[] done = new boolean[1];
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                ok[0] = presentGlassesDisplay(startedMs);
                synchronized (gate) {
                    done[0] = true;
                    gate.notifyAll();
                }
            }
        });
        synchronized (gate) {
            long deadline = SystemClock.elapsedRealtime() + 2000L;
            while (!done[0]) {
                long left = deadline - SystemClock.elapsedRealtime();
                if (left <= 0L || glassesShutdown) {
                    break;
                }
                try {
                    gate.wait(left);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    return false;
                }
            }
        }
        return ok[0];
    }

    private boolean presentGlassesDisplay(long startedMs) {
        if (glassesPresented) {
            return true;
        }
        if (glassesShutdown || isFinishing() || isDestroyed() || attemptedConnection) {
            return false;
        }
        // 真正顯示前先保持未啟用。post 之前若已經把 active 設上，佇列裡的 DisplayListener 會誤開重連。
        externalDisplayActive = false;
        externalDisplayId = Display.INVALID_DISPLAY;
        if (externalDisplay == null || !isGlasses3d(externalDisplay)) {
            DisplayManager displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
            ExternalDisplayHelper.Candidate candidate = ExternalDisplayHelper.find(
                    displayManager, externalStreamWidth(), externalStreamHeight(), externalStreamFps(),
                    currentActivityDisplayId(), windowActivityDisplayId());
            if (candidate == null || !isGlasses3d(candidate.display)) {
                return false;
            }
            externalDisplay = candidate.display;
            externalChosenMode = candidate.chosen;
        }
        desiredRefreshRate = externalChosenMode != null
                ? externalChosenMode.getRefreshRate()
                : externalDisplay.getRefreshRate();
        synchronized (glassesLife) {
            // 跟 release 讀這個旗標同一把鎖。先拆掉的那一端不能再把「用過 3D」補記上。
            if (glassesShutdown) {
                return false;
            }
            glassesPresented = true;
        }
        if (startedMs > 0L) {
            VitureUsb.logI("display mode 3840x1080 after " + (SystemClock.elapsedRealtime() - startedMs) + " ms");
        }
        TextView hint = findViewById(R.id.externalDisplayHint);
        if (hint != null) {
            hint.setText(R.string.external_display_touchpad_hint);
            hint.setVisibility(glassesVr ? View.GONE : View.VISIBLE);
        }
        keepGlassesScreenReadable();
        applyPhoneTouchpadAspect();
        View decor = getWindow().getDecorView();
        if (decor.getWindowToken() != null) {
            showGlassesPresentation();
        } else {
            decor.post(this::showGlassesPresentation);
        }
        return true;
    }

    private void showGlassesPresentation() {
        if (glassesShutdown || isFinishing() || isDestroyed() || externalDisplayLossHandled
                || externalDisplayAwaiting || attemptedConnection) {
            return;
        }
        if (externalPresentation != null) {
            return;
        }
        if (externalDisplay == null || !isGlasses3d(externalDisplay)) {
            return;
        }
        externalDisplayId = externalDisplay.getDisplayId();
        externalDisplayActive = true;
        showExternalPresentationAndStart();
    }

    private void showGlassesModePrompt() {
        if (isFinishing() || isDestroyed() || glassesPresented) {
            return;
        }
        TextView hint = findViewById(R.id.externalDisplayHint);
        if (hint != null) {
            hint.setText(R.string.glasses_vr_press_mode);
            hint.setVisibility(View.VISIBLE);
        }
    }

    private void showGlassesVrControls() {
        if (!glassesVr || isFinishing() || isDestroyed()) {
            return;
        }
        if (spinner != null) {
            spinner.dismiss();
            spinner = null;
        }
        keepGlassesScreenReadable();
        if (glassesWaitFor3d) {
            TextView hint = findViewById(R.id.externalDisplayHint);
            if (hint != null) {
                // §VITURE：Beast 不會送切換指令。一開始就請使用者按眼鏡，而不是顯示「正在切換」。
                hint.setText(vitureBeast ? R.string.glasses_vr_beast_press_3d : R.string.glasses_vr_switching);
                hint.setVisibility(View.VISIBLE);
            }
        }
        if (glassesControls == null) {
            LinearLayout panel = new LinearLayout(this);
            panel.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(16);
            panel.setPadding(pad, pad, pad, pad);
            // §VITURE：這塊蓋在觸控板上。不吃掉觸控的話，按按鈕會同時送出游標移動。
            panel.setClickable(true);
            glassesStatusView = new TextView(this);
            glassesStatusView.setText(R.string.glasses_vr_status_waiting);
            glassesStatusView.setTextColor(Color.WHITE);
            glassesStatusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
            glassesStatusView.setGravity(Gravity.CENTER);
            panel.addView(glassesStatusView, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            // §VITURE-TRIGGER：眼鏡沒有實體手把。按住＝扳機按下，放開＝鬆開；右手跟著頭的方向，看著選項按就是點選。
            Button triggerButton = glassesActionButton(R.string.glasses_vr_trigger, null);
            triggerButton.setMinHeight(dp(140));
            triggerButton.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
            triggerButton.setOnTouchListener(new View.OnTouchListener() {
                @Override
                public boolean onTouch(View v, MotionEvent event) {
                    GlassesVrSession session = glassesSession;
                    int action = event.getActionMasked();
                    if (action == MotionEvent.ACTION_DOWN) {
                        v.setPressed(true);
                        if (session != null) {
                            session.setTrigger(true);
                        }
                        return true;
                    }
                    if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                        v.setPressed(false);
                        if (session != null) {
                            session.setTrigger(false);
                        }
                        return true;
                    }
                    return true;
                }
            });
            panel.addView(triggerButton);
            panel.addView(glassesActionButton(R.string.glasses_vr_recenter, new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    GlassesVrSession session = glassesSession;
                    if (session != null) {
                        session.recenter();
                        VitureUsb.logI("recenter");
                    }
                }
            }));
            panel.addView(glassesActionButton(R.string.glasses_vr_end, new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    VitureUsb.logI("end requested");
                    finish();
                }
            }));
            FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM);
            View hint = findViewById(R.id.externalDisplayHint);
            ViewGroup parent = hint == null ? null : (ViewGroup) hint.getParent();
            if (parent == null) {
                parent = findViewById(android.R.id.content);
            }
            parent.addView(panel, panelParams);
            glassesControls = panel;
        }
        else {
            glassesControls.setVisibility(View.VISIBLE);
        }
        externalDisplayHandler.removeCallbacks(glassesStatusTick);
        externalDisplayHandler.post(glassesStatusTick);
    }

    private Button glassesActionButton(int label, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        button.setMinHeight(dp(64));
        button.setOnClickListener(listener);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(12);
        button.setLayoutParams(params);
        return button;
    }

    private final Runnable glassesStatusTick = new Runnable() {
        @Override
        public void run() {
            if (!glassesVr || isFinishing() || isDestroyed()) {
                return;
            }
            TextView status = glassesStatusView;
            GlassesVrSession session = glassesSession;
            if (status != null) {
                float[] sample = new float[4];
                if (session != null && session.imuSilent()) {
                    status.setText(R.string.glasses_vr_imu_silent);
                }
                else if (session != null && session.copyStatus(sample)) {
                    status.setText(getString(R.string.glasses_vr_status, sample[0], sample[1], sample[2], sample[3]));
                }
                else {
                    status.setText(R.string.glasses_vr_status_waiting);
                }
            }
            externalDisplayHandler.postDelayed(this, 200);
        }
    };

    private void onGlassesDetached() {
        if (glassesShutdown || isFinishing() || isDestroyed()) {
            return;
        }
        VitureUsb.logI("glasses detached; ending stream");
        finish();
    }

    private void releaseGlassesVr() {
        if (vrEmuReleased) {
            return;
        }
        vrEmuReleased = true;
        externalDisplayHandler.removeCallbacks(glassesStatusTick);
        final GlassesVrSession session;
        final Thread bringup;
        final boolean restore2d;
        synchronized (glassesLife) {
            glassesShutdown = true;
            session = glassesSession;
            glassesSession = null;
            bringup = glassesBringupThread;
            boolean beast = vitureBeast || (session != null && session.isBeast());
            restore2d = glassesPresented && !beast;
            // §LOGDROP：arm 之後 shutdown 才會送。這裡還看得到 session，拆掉之後 stopConnection 就判斷不到了。
            if (session != null && session.willUploadSessionEnd()) {
                glassesSessionEndOwned = true;
            }
        }
        if (bringup != null) {
            bringup.interrupt();
        }
        final long handle = vrEmuHandle;
        vrEmuHandle = 0;
        Runnable shutdown = new Runnable() {
            @Override
            public void run() {
                Thread opening = bringup;
                if (opening != null && opening != Thread.currentThread()) {
                    try {
                        opening.join(GLASSES_BRINGUP_JOIN_MS);
                    } catch (InterruptedException error) {
                        Thread.currentThread().interrupt();
                    }
                }
                boolean readerStopped = true;
                if (session != null) {
                    // 停 push、等讀取執行緒，之後才可以動 native handle。
                    readerStopped = session.shutdownAndJoin(restore2d, GLASSES_READER_JOIN_MS);
                } else if (opening != null && opening.isAlive()) {
                    // open() 可能還沒把 session 交出來，但讀取執行緒已經在跑。
                    readerStopped = false;
                }
                if (handle == 0 || !QxrNative.isAvailable()) {
                    return;
                }
                if (!readerStopped) {
                    // join 逾時就留著 handle。這時 destroy 會跟 nativeImuPush 重疊。
                    VitureUsb.logI("imu thread still running; leaking native handle=" + handle);
                    return;
                }
                QxrNative.nativeEmuStop(handle);
                QxrNative.nativeEmuDestroy(handle);
            }
        };
        // §VITURE：close() 拒絕在主執行緒做 USB。結束路徑多半在主執行緒，所以把關串流與還原 2D 放到背景。
        if (Looper.myLooper() == Looper.getMainLooper()) {
            new Thread(shutdown, "viple-viture-shutdown").start();
        }
        else {
            shutdown.run();
        }
    }

    private void maybeOpenPlainGlasses() {
        if (glassesVr || plainOpenStarted || plainReleaseRequested) {
            return;
        }
        plainOpenStarted = true;
        new Thread(new Runnable() {
            @Override
            public void run() {
                openPlainGlasses();
            }
        }, "viple-viture-plain").start();
    }

    private void openPlainGlasses() {
        try {
            UsbManager usb = (UsbManager) getSystemService(Context.USB_SERVICE);
            if (usb == null) {
                VitureUsb.logI("plain glasses skipped: usb manager");
                return;
            }
            UsbDevice device = VitureUsb.findGlassesDevice(usb);
            if (device == null) {
                VitureUsb.logI("plain glasses skipped: no device");
                return;
            }
            if (VitureUsb.isBeast(device)) {
                vitureBeast = true;
                logBeastDisplayLine();
            }
            boolean permitted;
            try {
                permitted = usb.hasPermission(device);
            } catch (SecurityException error) {
                VitureUsb.logW("plain glasses permission check failed", error);
                return;
            }
            if (!permitted) {
                if (VitureUsb.wasPermissionDenied(device)) {
                    VitureUsb.logI("plain glasses usb permission denied earlier; not requesting");
                    return;
                }
                // §VITURE：系統對話框畫在這顆 Activity 的手機視窗。外接 Presentation 是另一個視窗，不會被蓋住。
                permitted = awaitPlainUsbPermission(usb, device);
                if (!permitted) {
                    return;
                }
            }
            final VitureGlasses glasses;
            try {
                // §VITURE：不開 IMU。一般串流沒有人讀姿態，原始封包只會佔住同一條 IN 端點。
                glasses = VitureGlasses.open(Game.this, device, new VitureGlasses.Listener() {
                    @Override
                    public void onRawSample(float gx, float gy, float gz, float ax, float ay, float az,
                                            long sequence, long arrivalNs) {
                    }

                    @Override
                    public void onDisconnected() {
                        plainDetached = true;
                        onPlainGlassesDetached();
                    }
                });
            } catch (Exception error) {
                VitureUsb.logW("plain glasses open failed", error);
                VitureUsb.logI("plain glasses open failed: " + error.getMessage());
                return;
            }
            if (glasses.isBeast()) {
                queryBeastDisplayCode(glasses);
            }
            int initialWear = -1;
            try {
                VitureGlasses.CommandResult wear = glasses.query(VitureV2Protocol.MSG_GET_WEAR);
                if (wear.ok && wear.body.length >= 1) {
                    initialWear = wear.body[0] & 0xFF;
                }
                VitureUsb.logI("plain wear=" + (initialWear < 0 ? "unknown" : Integer.toString(initialWear)));
            } catch (RuntimeException error) {
                VitureUsb.logW("plain wear query failed", error);
            }
            boolean owned;
            synchronized (plainLife) {
                if (plainReleaseRequested || plainDetached || isFinishing() || isDestroyed()) {
                    owned = false;
                } else {
                    plainGlasses = glasses;
                    plainWearMonitor = new GlassesWearMonitor(glasses, initialWear, glassesWearListener);
                    plainWearMonitor.start();
                    plainCommandThread = new HandlerThread("viple-viture-cmd");
                    plainCommandThread.start();
                    plainCommandHandler = new Handler(plainCommandThread.getLooper());
                    owned = true;
                }
            }
            if (!owned) {
                try {
                    glasses.close();
                } catch (RuntimeException error) {
                    VitureUsb.logW("plain glasses close failed", error);
                }
                return;
            }
            VitureUsb.logI("plain glasses open");
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    showPlainGlassesControls();
                    maybeAutoSwitchSbs();
                    refreshBeast2dHint();
                }
            });
        } catch (RuntimeException error) {
            VitureUsb.logW("plain glasses open failed", error);
        }
    }

    private void onPlainGlassesDetached() {
        // §VITURE：拔掉只讓按鈕失效。這裡結束串流的話，換模式時畫面閃一下就會被當成拔線。
        VitureUsb.logI("glasses detached during plain stream; controls disabled, stream continues");
        GlassesWearMonitor monitor = plainWearMonitor;
        if (monitor != null) {
            monitor.requestStop();
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                disablePlainGlassesControls();
            }
        });
    }

    private void maybeAutoSwitchSbs() {
        if (glassesVr || sbsAutoAttempted || plainReleaseRequested || plainDetached
                || !connected || !externalDisplayActive || plainGlasses == null) {
            return;
        }
        sbsAutoAttempted = true;
        if (!GlassesDisplaySwitcher.streamIsSbs(prefConfig.width, prefConfig.height)) {
            VitureUsb.logI("sbs auto skipped stream=" + prefConfig.width + "x" + prefConfig.height);
            return;
        }
        postPlainCommand(new Runnable() {
            @Override
            public void run() {
                autoSwitchSbsBody();
            }
        });
    }

    private void autoSwitchSbsBody() {
        VitureGlasses glasses = plainGlasses;
        if (glasses == null || plainDetached || plainSwitcher.isClosing()) {
            return;
        }
        if (vitureBeast || glasses.isBeast()) {
            int width = externalPhysicalWidth();
            VitureUsb.logI("sbs auto skipped; beast mode="
                    + (GlassesDisplaySwitcher.isStereoWidth(width) ? "3d" : "2d"));
            return;
        }
        final int width = externalPhysicalWidth();
        if (width != GlassesDisplaySwitcher.WIDTH_2D) {
            if (width == GlassesDisplaySwitcher.WIDTH_3D) {
                VitureUsb.logI("display already 3840x1080; skip SET_DISPLAY_MODE");
            } else {
                VitureUsb.logI("sbs auto skipped displayWidth=" + width);
            }
            return;
        }
        final VitureGlasses.CommandResult result = plainSwitcher.switchTo3d(glasses);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                showPlainModeResult(result);
            }
        });
        if (!result.ok) {
            GlassesDisplaySwitcher.logModeRejected(result);
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    showGlassesPressModeHint();
                }
            });
            return;
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                armModeWidthWatch(width, result, VitureV2Protocol.DISPLAY_3D_SBS_60);
            }
        });
    }

    private void releasePlainGlasses() {
        final VitureGlasses toClose;
        final Handler handler;
        final GlassesWearMonitor monitor;
        synchronized (plainLife) {
            if (plainReleaseRequested) {
                return;
            }
            plainReleaseRequested = true;
            toClose = plainGlasses;
            plainGlasses = null;
            handler = plainCommandHandler;
            monitor = plainWearMonitor;
            // §LOGDROP：後面的 restore2d 一定會標記，close 才會送 session-end。stopConnection 可能在這次返回之後才跑。
            if (toClose != null) {
                glassesSessionEndOwned = true;
            }
        }
        if (handler == null && toClose == null && monitor == null) {
            return;
        }
        externalDisplayHandler.removeCallbacks(modeWatchRunnable);
        if (Looper.myLooper() == Looper.getMainLooper()) {
            disablePlainGlassesControls();
            hidePlainGlassesBar();
        } else {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    disablePlainGlassesControls();
                    hidePlainGlassesBar();
                }
            });
        }
        Runnable task = new Runnable() {
            @Override
            public void run() {
                if (monitor != null) {
                    monitor.requestStop();
                    monitor.join(500);
                }
                plainSwitcher.beginClose();
                if (toClose != null) {
                    plainSwitcher.restore2d(toClose);
                    try {
                        toClose.close();
                    } catch (RuntimeException error) {
                        VitureUsb.logW("plain glasses close failed", error);
                    }
                }
            }
        };
        if (handler != null) {
            handler.post(new Runnable() {
                @Override
                public void run() {
                    task.run();
                    if (plainCommandThread != null) {
                        plainCommandThread.quitSafely();
                    }
                }
            });
        } else {
            new Thread(task, "viple-viture-plain-shutdown").start();
        }
    }

    private void postPlainCommand(Runnable command) {
        if (command == null || plainReleaseRequested || plainDetached) {
            return;
        }
        Handler handler;
        synchronized (plainLife) {
            handler = plainCommandHandler;
        }
        if (handler == null) {
            return;
        }
        handler.post(command);
    }

    private void noteVitureBeast() {
        try {
            UsbManager usb = (UsbManager) getSystemService(Context.USB_SERVICE);
            if (VitureUsb.isBeast(VitureUsb.findGlassesDevice(usb))) {
                vitureBeast = true;
            }
        } catch (RuntimeException error) {
            // §VITURE：判斷失敗就維持舊款那條路，避免沒接眼鏡時誤把顯示指令拿掉。
            VitureUsb.logW("beast check failed", error);
        }
    }

    private void logBeastDisplayLine() {
        int width = -1;
        int height = -1;
        Display display = externalDisplay;
        try {
            if (display != null) {
                Display.Mode mode = display.getMode();
                if (mode != null) {
                    width = mode.getPhysicalWidth();
                    height = mode.getPhysicalHeight();
                }
            }
        } catch (RuntimeException error) {
            VitureUsb.logW("beast display size failed", error);
        }
        String panel = GlassesDisplaySwitcher.isStereoWidth(width) ? "3d" : "2d";
        VitureUsb.logI("beast display=" + width + "x" + height + " mode=" + panel);
    }

    private void queryBeastDisplayCode(VitureGlasses glasses) {
        try {
            VitureGlasses.CommandResult displayMode = glasses.query(VitureV2Protocol.MSG_GET_DISPLAY);
            String value = "none";
            if (displayMode.ok && displayMode.body.length >= 1) {
                value = String.format(Locale.US, "0x%02X", displayMode.body[0] & 0xFF);
            }
            VitureUsb.logI("beast GET 0x3141 status=" + displayMode.status + " value=" + value);
        } catch (RuntimeException error) {
            // §VITURE：這次查詢只為了記下 Beast 的代碼。失敗不能把一般串流一起停掉。
            VitureUsb.logW("beast GET 0x3141 failed", error);
        }
    }

    @SuppressWarnings("deprecation")
    private boolean awaitPlainUsbPermission(final UsbManager usb, final UsbDevice device) {
        final CountDownLatch latch = new CountDownLatch(1);
        final AtomicBoolean granted = new AtomicBoolean(false);
        final AtomicBoolean denied = new AtomicBoolean(false);
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                if (intent == null || !VitureUsb.ACTION_USB_PERMISSION.equals(intent.getAction())) {
                    return;
                }
                UsbDevice extra = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                if (extra == null || device.getDeviceName() == null
                        || !device.getDeviceName().equals(extra.getDeviceName())) {
                    return;
                }
                boolean ok = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                granted.set(ok);
                if (!ok) {
                    denied.set(true);
                }
                latch.countDown();
            }
        };
        IntentFilter filter = new IntentFilter(VitureUsb.ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(receiver, filter);
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (isFinishing() || isDestroyed()) {
                    latch.countDown();
                    return;
                }
                try {
                    VitureUsb.requestPermission(Game.this, usb, device);
                    VitureUsb.logI("plain glasses usb permission requested");
                } catch (RuntimeException error) {
                    VitureUsb.logW("plain glasses permission request failed", error);
                    latch.countDown();
                }
            }
        });
        boolean timedOut = false;
        try {
            timedOut = !latch.await(30, TimeUnit.SECONDS);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            try {
                unregisterReceiver(receiver);
            } catch (RuntimeException ignored) {
                // 已經解註冊。
            }
            return false;
        }
        try {
            unregisterReceiver(receiver);
        } catch (RuntimeException ignored) {
            // 已經解註冊。
        }
        if (isFinishing() || isDestroyed()) {
            return false;
        }
        if (denied.get()) {
            VitureUsb.notePermissionDenied(device);
            VitureUsb.logI("plain glasses usb permission denied");
            return false;
        }
        if (timedOut || !granted.get()) {
            VitureUsb.logI("plain glasses usb permission timed out");
            return false;
        }
        try {
            return usb.hasPermission(device);
        } catch (SecurityException error) {
            VitureUsb.logW("plain glasses permission check failed", error);
            return false;
        }
    }

    private void refreshBeast2dHint() {
        if (isFinishing() || isDestroyed() || glassesVr) {
            return;
        }
        boolean show = vitureBeast
                && externalDisplayActive
                && !externalDisplayAwaiting
                && isGlasses3d(externalDisplay)
                && prefConfig != null
                && !GlassesDisplaySwitcher.streamIsSbs(prefConfig.width, prefConfig.height);
        TextView phone = findViewById(R.id.beastModeHint);
        if (phone != null) {
            phone.setClickable(false);
            phone.setFocusable(false);
            if (show) {
                phone.setText(R.string.glasses_beast_switch_to_2d);
                phone.setVisibility(View.VISIBLE);
            } else {
                phone.setVisibility(View.GONE);
            }
        }
        StreamPresentation presentation = externalPresentation;
        if (presentation != null) {
            presentation.setTopHint(show ? getString(R.string.glasses_beast_switch_to_2d) : null);
        }
    }

    private int externalPhysicalWidth() {
        Display display = externalDisplay;
        if (display == null) {
            return -1;
        }
        try {
            Display.Mode mode = display.getMode();
            if (mode == null) {
                return -1;
            }
            return mode.getPhysicalWidth();
        } catch (RuntimeException error) {
            return -1;
        }
    }

    private void togglePlainDisplayMode() {
        VitureGlasses glasses = plainGlasses;
        if (glasses == null || plainDetached || plainSwitcher.isClosing()) {
            return;
        }
        if (vitureBeast || glasses.isBeast()) {
            VitureUsb.logI("display mode skipped; beast");
            return;
        }
        int width = externalPhysicalWidth();
        int mode;
        if (width == GlassesDisplaySwitcher.WIDTH_2D) {
            mode = VitureV2Protocol.DISPLAY_3D_SBS_60;
        } else if (width == GlassesDisplaySwitcher.WIDTH_3D) {
            mode = VitureV2Protocol.DISPLAY_2D_60;
        } else {
            VitureGlasses.CommandResult query;
            try {
                query = glasses.query(VitureV2Protocol.MSG_GET_DISPLAY);
            } catch (RuntimeException error) {
                VitureUsb.logW("display query failed", error);
                deliverModeResult(null);
                return;
            }
            if (!query.ok || query.body.length < 1) {
                deliverModeResult(query);
                return;
            }
            int current = query.body[0] & 0xFF;
            mode = current == VitureV2Protocol.DISPLAY_3D_SBS_60
                    ? VitureV2Protocol.DISPLAY_2D_60
                    : VitureV2Protocol.DISPLAY_3D_SBS_60;
        }
        final VitureGlasses.CommandResult result;
        if (mode == VitureV2Protocol.DISPLAY_3D_SBS_60) {
            result = plainSwitcher.switchTo3d(glasses);
            if (!result.ok) {
                GlassesDisplaySwitcher.logModeRejected(result);
            }
        } else {
            result = plainSwitcher.setDisplayMode(glasses, mode);
            VitureUsb.logI("SET display mode 0x31 status=" + result.status + " ok=" + result.ok);
            if (result.ok) {
                plainSwitcher.noteUserReturnedTo2d();
            }
        }
        final int baseline = width;
        final int sent = mode;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                showPlainModeResult(result);
                if (baseline > 0) {
                    armModeWidthWatch(baseline, result, sent);
                }
            }
        });
    }

    private void adjustPlainVolume(int delta) {
        VitureGlasses glasses = plainGlasses;
        if (glasses == null || plainDetached || plainSwitcher.isClosing()) {
            return;
        }
        VitureGlasses.CommandResult query;
        try {
            query = glasses.query(VitureV2Protocol.MSG_GET_VOLUME);
        } catch (RuntimeException error) {
            VitureUsb.logW("volume query failed", error);
            deliverVolumeText(R.string.glasses_mode_rejected);
            return;
        }
        if (!query.ok || query.body.length < 1) {
            VitureUsb.logI("volume query failed status=" + query.status);
            deliverVolumeText(query.status == VitureGlasses.STATUS_TIMEOUT
                    ? R.string.glasses_mode_timeout
                    : R.string.glasses_mode_rejected);
            return;
        }
        int current = query.body[0] & 0xFF;
        int next = current + delta;
        if (next < PLAIN_VOLUME_MIN) {
            next = PLAIN_VOLUME_MIN;
        }
        if (next > PLAIN_VOLUME_MAX) {
            next = PLAIN_VOLUME_MAX;
        }
        if (next == current) {
            VitureUsb.logI("volume unchanged level=" + current);
            deliverVolumeLevel(current, false);
            return;
        }
        VitureGlasses.CommandResult set;
        try {
            set = glasses.setVolume(next);
        } catch (RuntimeException error) {
            VitureUsb.logW("volume set failed", error);
            deliverVolumeLevel(current, true);
            return;
        }
        if (!set.ok) {
            VitureUsb.logI("volume set rejected status=" + set.status + " level=" + current);
            if (set.status == VitureGlasses.STATUS_TIMEOUT) {
                deliverVolumeText(R.string.glasses_mode_timeout);
            } else {
                deliverVolumeLevel(current, true);
            }
            return;
        }
        VitureUsb.logI("volume " + current + " -> " + next);
        deliverVolumeLevel(next, false);
    }

    private void deliverModeResult(final VitureGlasses.CommandResult result) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                showPlainModeResult(result);
            }
        });
    }

    private void deliverVolumeLevel(final int level, final boolean rejected) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (plainVolumeResult == null) {
                    return;
                }
                if (rejected) {
                    plainVolumeResult.setText(getString(R.string.glasses_volume_rejected, level));
                } else {
                    plainVolumeResult.setText(getString(R.string.glasses_volume_level, level));
                }
                revealExternalDimForHint();
            }
        });
    }

    private void deliverVolumeText(final int text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (plainVolumeResult != null) {
                    plainVolumeResult.setText(text);
                    revealExternalDimForHint();
                }
            }
        });
    }

    private void showPlainModeResult(VitureGlasses.CommandResult result) {
        if (plainModeResult == null) {
            return;
        }
        if (result != null && result.ok) {
            plainModeResult.setText(R.string.glasses_mode_ok);
        } else if (result != null && result.status == VitureGlasses.STATUS_TIMEOUT) {
            plainModeResult.setText(R.string.glasses_mode_timeout);
        } else {
            plainModeResult.setText(R.string.glasses_mode_rejected);
        }
        revealExternalDimForHint();
    }

    private void armModeWidthWatch(int baseline, VitureGlasses.CommandResult result, int targetMode) {
        modeWatchBaselineWidth = baseline;
        modeWatchResult = result;
        modeWatchTarget = targetMode;
        modeWatchStartedElapsed = SystemClock.elapsedRealtime();
        modeWatchHintShown = false;
        externalDisplayHandler.removeCallbacks(modeWatchRunnable);
        externalDisplayHandler.postDelayed(modeWatchRunnable, GlassesDisplaySwitcher.MODE_WAIT_MS);
    }

    private void showGlassesPressModeHint() {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        modeWatchHintShown = true;
        TextView hint = findViewById(R.id.externalDisplayHint);
        if (hint == null) {
            return;
        }
        hint.setText(R.string.glasses_press_mode);
        hint.setVisibility(View.VISIBLE);
    }

    private void showGlassesRemovedNotice() {
        if (isFinishing() || isDestroyed()) {
            return;
        }
        if (!externalDisplayActive && !glassesVr) {
            return;
        }
        if (glassesRemovedNotice == null) {
            TextView notice = new TextView(this);
            notice.setText(R.string.glasses_removed);
            notice.setTextColor(Color.WHITE);
            notice.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
            notice.setGravity(Gravity.CENTER);
            notice.setBackgroundColor(0xCC000000);
            int pad = dp(12);
            notice.setPadding(pad, pad, pad, pad);
            // §VITURE：這條提示蓋在觸控板上。不吃掉觸控的話，點到字會移動遠端游標。
            notice.setClickable(true);
            ViewGroup parent = externalTouchpadParent();
            if (parent == null) {
                return;
            }
            parent.addView(notice, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP));
            glassesRemovedNotice = notice;
        }
        glassesRemovedNotice.setVisibility(View.VISIBLE);
        revealExternalDimForHint();
    }

    private void hideGlassesRemovedNotice() {
        if (glassesRemovedNotice != null) {
            glassesRemovedNotice.setVisibility(View.GONE);
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private void showPlainGlassesControls() {
        if (glassesVr || plainReleaseRequested || plainDetached || plainGlasses == null
                || !externalDisplayActive || isFinishing() || isDestroyed()) {
            return;
        }
        if (plainGlassesBar == null) {
            LinearLayout panel = new LinearLayout(this);
            panel.setOrientation(LinearLayout.VERTICAL);
            panel.setBackgroundColor(0xF0181818);
            // §VITURE：這塊蓋在觸控板上。不吃掉觸控的話，在按鈕上滑會同時移動遠端游標。
            panel.setClickable(true);
            panel.setFocusable(false);
            panel.setOnTouchListener(new View.OnTouchListener() {
                @Override
                public boolean onTouch(View v, MotionEvent event) {
                    return true;
                }
            });
            plainModeResult = glassesResultText();
            panel.addView(plainModeResult);
            plainModeButton = glassesBarButton(R.string.glasses_mode_toggle, new Runnable() {
                @Override
                public void run() {
                    togglePlainDisplayMode();
                }
            });
            plainBeastModeNote = new TextView(this);
            plainBeastModeNote.setText(R.string.glasses_beast_use_button);
            plainBeastModeNote.setTextColor(Color.WHITE);
            plainBeastModeNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
            plainBeastModeNote.setGravity(Gravity.CENTER);
            plainBeastModeNote.setMinHeight(dp(72));
            // §VITURE：說明放在原本按鈕的位置。這塊控制列本來就吃觸控，字本身不要再變成按鈕。
            plainBeastModeNote.setClickable(false);
            plainBeastModeNote.setFocusable(false);
            plainBeastModeNote.setVisibility(View.GONE);
            LinearLayout.LayoutParams noteParams = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(72));
            noteParams.setMargins(dp(4), dp(4), dp(4), dp(4));
            panel.addView(plainBeastModeNote, noteParams);
            LinearLayout.LayoutParams modeParams = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(72));
            modeParams.setMargins(dp(4), dp(4), dp(4), dp(4));
            panel.addView(plainModeButton, modeParams);
            plainVolumeResult = glassesResultText();
            panel.addView(plainVolumeResult);
            LinearLayout volumeRow = new LinearLayout(this);
            volumeRow.setOrientation(LinearLayout.HORIZONTAL);
            plainVolumeDownButton = glassesBarButton(R.string.glasses_volume_down, new Runnable() {
                @Override
                public void run() {
                    adjustPlainVolume(-1);
                }
            });
            plainVolumeUpButton = glassesBarButton(R.string.glasses_volume_up, new Runnable() {
                @Override
                public void run() {
                    adjustPlainVolume(1);
                }
            });
            LinearLayout.LayoutParams half = new LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
            half.setMargins(dp(4), 0, dp(4), 0);
            volumeRow.addView(plainVolumeDownButton, half);
            volumeRow.addView(plainVolumeUpButton, half);
            // §VITURE：音量放在螢幕最下緣，左右各半，戴著眼鏡摸得到。
            LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(72));
            rowParams.setMargins(0, 0, 0, dp(4));
            panel.addView(volumeRow, rowParams);
            ViewGroup parent = externalTouchpadParent();
            if (parent == null) {
                return;
            }
            parent.addView(panel, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.BOTTOM));
            plainGlassesBar = panel;
        }
        if (vitureBeast) {
            plainModeButton.setVisibility(View.GONE);
            if (plainBeastModeNote != null) {
                plainBeastModeNote.setVisibility(View.VISIBLE);
            }
            if (plainModeResult != null) {
                plainModeResult.setVisibility(View.GONE);
            }
        } else {
            plainModeButton.setVisibility(View.VISIBLE);
            if (plainBeastModeNote != null) {
                plainBeastModeNote.setVisibility(View.GONE);
            }
        }
        plainModeButton.setEnabled(!vitureBeast);
        plainVolumeDownButton.setEnabled(true);
        plainVolumeUpButton.setEnabled(true);
        plainGlassesBar.setVisibility(View.VISIBLE);
        reserveTouchpadForGlassesBar(true);
    }

    private TextView glassesResultText() {
        TextView text = new TextView(this);
        text.setTextColor(Color.WHITE);
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        text.setGravity(Gravity.CENTER);
        text.setMinHeight(dp(32));
        return text;
    }

    @SuppressLint("ClickableViewAccessibility")
    private Button glassesBarButton(int label, final Runnable command) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        button.setMinHeight(dp(72));
        // §VITURE：搶走焦點的話，點過音量之後鍵盤事件會進按鈕而不是遊戲。
        button.setFocusable(false);
        button.setFocusableInTouchMode(false);
        button.setHapticFeedbackEnabled(true);
        button.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                    v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                }
                return false;
            }
        });
        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                postPlainCommand(command);
            }
        });
        return button;
    }

    private void disablePlainGlassesControls() {
        if (plainModeButton != null) {
            plainModeButton.setEnabled(false);
        }
        if (plainVolumeDownButton != null) {
            plainVolumeDownButton.setEnabled(false);
        }
        if (plainVolumeUpButton != null) {
            plainVolumeUpButton.setEnabled(false);
        }
    }

    private void hidePlainGlassesBar() {
        if (plainGlassesBar != null) {
            plainGlassesBar.setVisibility(View.GONE);
            // §VITURE：只有真的加過控制列才改邊距。沒有眼鏡時觸控板版面跟以前一樣。
            reserveTouchpadForGlassesBar(false);
        }
        hideGlassesRemovedNotice();
    }

    private void reserveTouchpadForGlassesBar(boolean reserve) {
        TextView hint = findViewById(R.id.externalDisplayHint);
        if (hint != null && hint.getLayoutParams() instanceof FrameLayout.LayoutParams) {
            FrameLayout.LayoutParams hintParams = (FrameLayout.LayoutParams) hint.getLayoutParams();
            hintParams.bottomMargin = reserve ? dp(240) : 0;
            hint.setLayoutParams(hintParams);
        }
        if (fileTransferOverlayView != null
                && fileTransferOverlayView.getLayoutParams() instanceof FrameLayout.LayoutParams) {
            FrameLayout.LayoutParams overlayParams =
                    (FrameLayout.LayoutParams) fileTransferOverlayView.getLayoutParams();
            overlayParams.bottomMargin = reserve ? dp(240) : dp(10);
            fileTransferOverlayView.setLayoutParams(overlayParams);
        }
    }

    private ViewGroup externalTouchpadParent() {
        View hint = findViewById(R.id.externalDisplayHint);
        if (hint != null && hint.getParent() instanceof ViewGroup) {
            return (ViewGroup) hint.getParent();
        }
        return findViewById(android.R.id.content);
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private void releaseVrEmulation() {
        // §VITURE：一般串流的眼鏡不走 glasses_vr。沒開過時立刻返回，手機串流的結束流程不變。
        releasePlainGlasses();
        if (glassesVr) {
            releaseGlassesVr();
            return;
        }
        if (!vrEmulate || vrEmuReleased) {
            return;
        }
        vrEmuReleased = true;
        if (vrEmuHandle != 0 && QxrNative.isAvailable()) {
            QxrNative.nativeEmuStop(vrEmuHandle);
            QxrNative.nativeEmuDestroy(vrEmuHandle);
            vrEmuHandle = 0;
        }
    }

    // VipleStream CLI mode: override stream preferences with intent extras
    private Boolean cliAutoAdjustBitrate = null;

    private void applyCliOverrides(PreferenceConfiguration pref, Intent intent) {
        if (intent.hasExtra("cli_bitrate")) {
            int v = intent.getIntExtra("cli_bitrate", 0);
            if (v > 0) { pref.bitrate = v; LimeLog.info("[VIPLE-CLI] bitrate=" + v); }
        }
        if (intent.hasExtra("cli_fps")) {
            int v = intent.getIntExtra("cli_fps", 0);
            if (v > 0) { pref.fps = v; LimeLog.info("[VIPLE-CLI] fps=" + v); }
        }
        if (intent.hasExtra("cli_width")) {
            int v = intent.getIntExtra("cli_width", 0);
            if (v > 0) { pref.width = v; LimeLog.info("[VIPLE-CLI] width=" + v); }
        }
        if (intent.hasExtra("cli_height")) {
            int v = intent.getIntExtra("cli_height", 0);
            if (v > 0) { pref.height = v; LimeLog.info("[VIPLE-CLI] height=" + v); }
        }
        if (intent.hasExtra("cli_codec")) {
            int v = intent.getIntExtra("cli_codec", -1);
            if (v >= 0) {
                switch (v) {
                    case 0: pref.videoFormat = PreferenceConfiguration.FormatOption.FORCE_H264; break;
                    case 1: pref.videoFormat = PreferenceConfiguration.FormatOption.FORCE_HEVC; break;
                    case 2: pref.videoFormat = PreferenceConfiguration.FormatOption.FORCE_AV1; break;
                }
                LimeLog.info("[VIPLE-CLI] codec=" + v);
            }
        }
        if (intent.hasExtra("cli_enableMpQuic")) {
            pref.enableMpQuic = intent.getBooleanExtra("cli_enableMpQuic", false);
            LimeLog.info("[VIPLE-CLI] enableMpQuic=" + pref.enableMpQuic);
        }
        if (intent.hasExtra("cli_enableFruc")) {
            pref.enableFruc = intent.getBooleanExtra("cli_enableFruc", false);
            LimeLog.info("[VIPLE-CLI] enableFruc=" + pref.enableFruc);
        }
        if (intent.hasExtra("cli_perfOverlay")) {
            pref.enablePerfOverlay = intent.getBooleanExtra("cli_perfOverlay", false);
            LimeLog.info("[VIPLE-CLI] perfOverlay=" + pref.enablePerfOverlay);
        }
        if (intent.hasExtra("cli_externalDisplay")) {
            pref.externalDisplay = intent.getBooleanExtra("cli_externalDisplay", true);
            // CLI 明確說 false＝只畫在手機，接著眼鏡也不要自動搬走。
            pref.externalDisplayAutoGlasses = pref.externalDisplay;
            LimeLog.info("[VIPLE-EXTDISP] cli external_display=" + pref.externalDisplay);
        }
        if (intent.hasExtra("cli_autoAdjustBitrate")) {
            cliAutoAdjustBitrate = intent.getBooleanExtra("cli_autoAdjustBitrate", true);
            LimeLog.info("[VIPLE-CLI] autoAdjustBitrate=" + cliAutoAdjustBitrate);
        }
    }
}
