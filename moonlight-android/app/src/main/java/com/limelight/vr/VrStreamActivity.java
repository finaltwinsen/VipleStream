package com.limelight.vr;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Display;
import android.view.Surface;
import android.view.WindowManager;
import android.widget.Toast;

import com.limelight.Game;
import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.binding.PlatformBinding;
import com.limelight.binding.audio.AndroidAudioRenderer;
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
import com.limelight.nvstream.jni.MoonBridge;
import com.limelight.nvstream.vr.VrLaunchConfig;
import com.limelight.preferences.GlPreferences;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.ByteArrayInputStream;
import java.security.cert.CertificateException;
import java.util.Locale;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;

/**
 * §QUEST-VR：頭盔裡一塊平面畫面，走一般 2D 串流。不送 vr* 參數，也不處理輸入。
 * §QUEST-PCVR：extra {@code vr_pcvr} 為 true 才送 vr*。2D 路徑不讀它，行為維持原樣。
 */
public class VrStreamActivity extends Activity implements NvConnectionListener, QxrNative.SessionListener {
    // 供 AppUpdater 判斷 VR 串流是否進行中，避免串流期間檢查或下載更新搶用頻寬。
    private static volatile boolean sIsStreaming = false;

    public static boolean isStreaming() {
        return sIsStreaming;
    }

    private static final int PREFERRED_HZ_MIN = 72;
    private static final int PREFERRED_HZ_MAX = 120;
    private static final int VIEW_CONFIG_POLL_MS = 100;
    private static final int VIEW_CONFIG_TIMEOUT_MS = 10000;
    private static final int VIEW_FLOATS = 33;
    private static final int VIEW_EYE_W = 0;
    private static final int VIEW_EYE_H = 1;
    private static final int VIEW_HZ = 2;
    private static final int VIEW_LEFT_ANGLE = 3;
    private static final int VIEW_LEFT_POS = 7;
    private static final int VIEW_LEFT_ROT = 10;
    private static final int VIEW_RIGHT_ANGLE = 14;
    private static final int VIEW_RIGHT_POS = 18;
    private static final int VIEW_RIGHT_ROT = 21;
    private static final int VIEW_AREA_W = 25;
    private static final int VIEW_AREA_D = 26;
    private static final int VIEW_SEATED_VALID = 27;
    private static final int VIEW_SEATED_X = 28;
    private static final int VIEW_SEATED_Y = 29;
    private static final int VIEW_SEATED_Z = 30;
    private static final int VIEW_SEATED_YAW = 31;
    private static final int VIEW_TIME_CONV = 32;
    // VipleVr.h 的 VIPLE_VR_HZ_MIN／VIPLE_VR_HZ_MAX。
    // docs/vr_protocol.md §4.2：vrHz 與 vrDispHz 都是 60–144；超出或 vrDispHz < vrHz 回 400 VR_BAD_PARAMS。
    private static final int VR_HZ_MIN = 60;
    private static final int VR_HZ_MAX = 144;
    private static final int EYE_MAX = 2304;
    private static final int EYE_ALIGN = 16;
    // §QUEST-PCVR：nativeCreate 不接受 0。還沒有單眼尺寸時用這個把 runtime 拉起來，量完再換成打包尺寸。
    private static final int PCVR_PLACEHOLDER_WIDTH = 1280;
    private static final int PCVR_PLACEHOLDER_HEIGHT = 720;

    private final Object releaseLock = new Object();

    private PreferenceConfiguration prefConfig;
    private NvConnection conn;
    private MediaCodecDecoderRenderer decoderRenderer;
    private SharedPreferences tombstonePrefs;
    private Boolean cliAutoAdjustBitrate;
    private long handle;
    private Surface decoderSurface;
    private String streamHost;
    private int streamHttpsPort;
    private X509Certificate streamServerCert;
    // 非正常斷線時 host 會自己留著 app。connectionTerminated 會關掉這個旗標，releaseSession 才不會再送 /cancel。
    private volatile boolean releaseHostOwnership = true;
    private boolean connectionStartRequested;
    private boolean released;
    private volatile boolean reportedCrash;
    // §QUEST-PCVR：2D 維持 false，releaseSession 才不會多叫 nativePcvrStop。
    private volatile boolean pcvrMode;
    private final Handler pcvrPollHandler = new Handler(Looper.getMainLooper());
    private int pcvrPollGeneration;
    // §QUEST-PCVR：重建 session 時舊的結束回呼不能把新的這場關掉。兩者相等才處理。
    private volatile int sessionEpoch;
    private volatile int boundEpoch;
    private int pcvrCreateWidth;
    private int pcvrCreateHeight;
    // 解碼器的 AImageReader 尺寸在 nativeCreate 就定死，只為它重建一次。第二次還不合就停。
    private boolean pcvrRecreatedForSize;
    private boolean pcvrAwaitingRecreatedViews;
    private int requestedEyeW;
    private int requestedEyeH;
    private boolean hasRequestedEye;
    private int requestedHz;
    private int pcvrHz;
    private int pcvrPackedW;
    private int pcvrPackedH;
    private GlPreferences pcvrGlPrefs;
    private boolean pcvrMetered;
    private String pcvrHost;
    private int pcvrPort;
    private int pcvrHttpsPort;
    private NvApp pcvrApp;
    private String pcvrUniqueId;
    private X509Certificate pcvrCert;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sIsStreaming = true;
        LimeLog.info("[VIPLE-QXR-ACT] onCreate");

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // §QUEST-VR：音量鍵要調的是串流音訊。這個 Activity 沒有可見的 2D 控制項。
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        // §QUEST-VR：沉浸式 Activity 的 2D 視窗不會顯示，放 layout 沒有意義，也沒有 SurfaceView 可當解碼目標。

        if (!QxrNative.isAvailable()) {
            abort("OpenXR streaming component failed to load", true);
            return;
        }

        try {
            startStream();
        } catch (Exception | UnsatisfiedLinkError e) {
            LimeLog.severe("[VIPLE-QXR-ACT] onCreate failed: " + e);
            if (!isFinishing()) {
                abort("could not start OpenXR stream", true);
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        LimeLog.info("[VIPLE-QXR-ACT] onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        // 系統選單只會 onPause，OpenXR 停在 VISIBLE，畫面還要畫。只有正在結束才拆。
        LimeLog.info("[VIPLE-QXR-ACT] onPause finishing=" + isFinishing());
        if (isFinishing()) {
            releaseSession();
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        LimeLog.info("[VIPLE-QXR-ACT] onStop finishing=" + isFinishing());
        if (isFinishing()) {
            releaseSession();
            return;
        }
        // session 進 STOPPING 之後，這個 Activity 無法自己恢復。連線留著只會讓 host 空轉。
        // 這一階段不做暫停後恢復。
        releaseSession();
        finish();
    }

    @Override
    protected void onDestroy() {
        LimeLog.info("[VIPLE-QXR-ACT] onDestroy");
        sIsStreaming = false;
        releaseSession();
        super.onDestroy();
    }

    private void startStream() {
        // §QUEST-VR：relay 的 wss 釘選跟 Game 共用同一份儲存。沒初始化的話走 relay 會失敗。
        com.limelight.relay.RelayTls.init(this);

        prefConfig = PreferenceConfiguration.readPreferences(this);
        applyCliOverrides(prefConfig, getIntent());
        // §QUEST-VR：解碼器接到直接輸出 Surface 時不會做補幀。幀率若照 Game 砍半，畫面會以半速播放。
        if (prefConfig.enableFruc) {
            LimeLog.info("[VIPLE-QXR-ACT] ignore frame interpolation: OpenXR direct output has no FRUC");
            prefConfig.enableFruc = false;
        }
        if (prefConfig.enableHdr) {
            // §QUEST-VR：native 影像層這一階段只處理 8-bit。補幀路徑會讀這個欄位，這裡一併關掉。
            LimeLog.info("[VIPLE-QXR-ACT] ignore HDR");
            prefConfig.enableHdr = false;
        }
        // BALANCED 把已解出的緩衝扣在 outputBufferQueue，等 Choreographer 才 release。
        // directOutputSurface 非 null 時，其餘模式都是解出來就 releaseOutputBuffer(index, true)。
        // 選 MIN_LATENCY：OpenXR 自己留最新一張，不要在 Java 端為了 2D vsync 留幀。
        prefConfig.framePacing = PreferenceConfiguration.FRAME_PACING_MIN_LATENCY;

        String host = getIntent().getStringExtra(Game.EXTRA_HOST);
        int port = getIntent().getIntExtra(Game.EXTRA_PORT, NvHTTP.DEFAULT_HTTP_PORT);
        int httpsPort = getIntent().getIntExtra(Game.EXTRA_HTTPS_PORT, 0);
        int appId = getIntent().getIntExtra(Game.EXTRA_APP_ID, StreamConfiguration.INVALID_APP_ID);
        String appName = getIntent().getStringExtra(Game.EXTRA_APP_NAME);
        String uniqueId = getIntent().getStringExtra(Game.EXTRA_UNIQUEID);
        boolean appSupportsHdr = getIntent().getBooleanExtra(Game.EXTRA_APP_HDR, false);
        byte[] derCertData = getIntent().getByteArrayExtra(Game.EXTRA_SERVER_CERT);

        if (host == null || host.isEmpty()) {
            abort("missing host address", true);
            return;
        }
        if (appId == StreamConfiguration.INVALID_APP_ID) {
            LimeLog.severe("[VIPLE-QXR-ACT] missing app id");
            finish();
            return;
        }

        X509Certificate serverCert = readServerCertificate(derCertData);
        NvApp app = new NvApp(appName != null ? appName : "app", appId, appSupportsHdr);

        Display display = getWindowManager().getDefaultDisplay();
        float displayRefreshRate = display.getRefreshRate();
        LimeLog.info("[VIPLE-QXR-ACT] panel refresh " + displayRefreshRate);
        int chosenFrameRate = prefConfig.fps;
        int preferredHz = clampPreferredHz(chosenFrameRate);

        GlPreferences glPrefs = GlPreferences.readPreferences(this);
        // §QUEST-VR：解碼器建構式會問 MediaCodecHelper 有哪些 codec，沒先初始化會查到空的。
        MediaCodecHelper.initialize(this, glPrefs.glRenderer);

        ConnectivityManager connMgr = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        // §QUEST-VR：Quest 上 getSystemService 可以是 null。沒有服務就當成非計量網路。
        boolean metered = connMgr != null && connMgr.isActiveNetworkMetered();

        tombstonePrefs = getSharedPreferences("DecoderTombstone", MODE_PRIVATE);

        if (getIntent().getBooleanExtra("vr_pcvr", false)) {
            pcvrMode = true;
            VrStreamDefaults.applySessionLimits(prefConfig);
            pcvrGlPrefs = glPrefs;
            pcvrMetered = metered;
            pcvrHost = host;
            pcvrPort = port;
            pcvrHttpsPort = httpsPort;
            pcvrApp = app;
            pcvrUniqueId = uniqueId;
            pcvrCert = serverCert;
            beginPcvr();
            return;
        }

        handle = QxrNative.nativeCreate(this, prefConfig.width, prefConfig.height, preferredHz);
        if (handle == 0) {
            String error = QxrNative.nativeLastError();
            if (error == null || error.isEmpty()) {
                error = "nativeCreate returned 0";
            }
            abort(error, true);
            return;
        }
        LimeLog.info("[VIPLE-QXR-ACT] nativeCreate handle=" + handle
                + " " + prefConfig.width + "x" + prefConfig.height
                + " fps=" + chosenFrameRate + " preferredHz=" + preferredHz);

        decoderSurface = QxrNative.nativeGetDecoderSurface(handle);
        if (decoderSurface == null) {
            String error = QxrNative.nativeLastError();
            abort("no decoder Surface" + (error == null || error.isEmpty() ? "" : ": " + error), true);
            return;
        }

        decoderRenderer = new MediaCodecDecoderRenderer(
                this,
                prefConfig,
                new CrashListener() {
                    @Override
                    public void notifyCrash(Exception e) {
                        // §QUEST-VR：解碼器丟例外後行程可能跟著結束，計數要在返回前寫進磁碟。
                        reportedCrash = true;
                        tombstonePrefs.edit()
                                .putInt("CrashCount", tombstonePrefs.getInt("CrashCount", 0) + 1)
                                .commit();
                        LimeLog.severe("[VIPLE-QXR-ACT] decoder crash: " + e);
                    }
                },
                tombstonePrefs.getInt("CrashCount", 0),
                metered,
                // §QUEST-VR：requestedHdr。固定 false，不讓解碼器去找 10-bit profile。
                false,
                glPrefs.glRenderer,
                new PerfOverlayListener() {
                    @Override
                    public void onPerfUpdate(String text) {
                        // §QUEST-VR：沉浸式畫面沒有浮層，這一階段不顯示效能字串。
                    }
                });
        decoderRenderer.setDirectOutputSurface(decoderSurface);
        // 只改本機 PTS（frameNumber 乘固定步進），輸出端才找得回幀編號。不送 vr*，也不改 /launch。
        decoderRenderer.setVrSession(true);

        int supportedVideoFormats = buildSupportedVideoFormats();
        if (!decoderRenderer.isAvcSupported()) {
            abort("no H.264 hardware decoder", true);
            return;
        }

        // §QUEST-VR：不要 setVrLaunch。vrLaunch 不是 null 就會把 vr* 送進 /launch，這次只要一般 2D 串流。
        StreamConfiguration config = new StreamConfiguration.Builder()
                .setResolution(prefConfig.width, prefConfig.height)
                .setLaunchRefreshRate(prefConfig.fps)
                .setRefreshRate(chosenFrameRate)
                .setApp(app)
                .setBitrate(prefConfig.bitrate)
                .setEnableSops(prefConfig.enableSops)
                .enableLocalAudioPlayback(prefConfig.playHostAudio)
                .setMaxPacketSize(prefConfig.packetSize)
                .setRemoteConfiguration(StreamConfiguration.STREAM_CFG_AUTO)
                .setSupportedVideoFormats(supportedVideoFormats)
                // §QUEST-VR：這一階段不讀手把、不建虛擬手把，遮罩固定 0。
                .setAttachedGamepadMask(0)
                .setClientRefreshRateX100((int) (displayRefreshRate * 100))
                .setAudioConfiguration(prefConfig.audioConfiguration)
                .setColorSpace(decoderRenderer.getPreferredColorSpace())
                .setColorRange(decoderRenderer.getPreferredColorRange())
                .setPersistGamepadsAfterDisconnect(!prefConfig.multiController)
                .setMpQuic(prefConfig.enableMpQuic, prefConfig.mpQuicScheduler)
                .setAutoAdjustBitrate(cliAutoAdjustBitrate != null ? cliAutoAdjustBitrate : true)
                .build();

        conn = new NvConnection(getApplicationContext(),
                new ComputerDetails.AddressTuple(host, port),
                httpsPort, uniqueId, config,
                PlatformBinding.getCryptoProvider(this), serverCert);
        conn.setRelayConfig(prefConfig.relayUrl, prefConfig.relayPsk,
                getIntent().getStringExtra(Game.EXTRA_PC_UUID),
                getIntent().getStringExtra(Game.EXTRA_STUN_ADDRESS),
                getIntent().getIntExtra(Game.EXTRA_STUN_PUNCH_PORT, 0));

        QxrNative.setListener(handle, this);
        if (!QxrNative.nativeStart(handle)) {
            String error = QxrNative.nativeLastError();
            abort("OpenXR session start failed" + (error == null || error.isEmpty() ? "" : ": " + error), true);
            return;
        }
        // §QUEST-VR：nativeStart 若在這條執行緒同步結束 session，finish() 已經排進來，不能再 start。
        if (isFinishing()) {
            return;
        }

        streamHost = host;
        streamHttpsPort = httpsPort;
        streamServerCert = serverCert;
        connectionStartRequested = true;
        LimeLog.info("[VIPLE-QXR-ACT] conn.start");
        // §QUEST-VR：NvConnection.start 自己開執行緒。Surface 在 nativeCreate 時就有了，不必等 SurfaceView。
        conn.start(new AndroidAudioRenderer(this, prefConfig.enableAudioFx), decoderRenderer, this);
    }

    private int buildSupportedVideoFormats() {
        int supportedVideoFormats = MoonBridge.VIDEO_FORMAT_H264;
        if (decoderRenderer.isHevcSupported()) {
            supportedVideoFormats |= MoonBridge.VIDEO_FORMAT_H265;
        }

        // §QUEST-VR：探測照 Game 問過解碼器，但這一階段的影像層只收 8-bit H.264 / HEVC，結果不公告。
        // 探測自己丟例外時不能擋掉 H.264，否則這一階段沒有可以播的格式。
        boolean av1 = false;
        boolean hevc10 = false;
        boolean av1Main10 = false;
        boolean hevc444 = false;
        boolean hevc44410 = false;
        boolean av1444 = false;
        boolean av144410 = false;
        boolean pyroWave = false;
        try {
            av1 = decoderRenderer.isAv1Supported();
            hevc10 = decoderRenderer.isHevcMain10Hdr10Supported();
            av1Main10 = decoderRenderer.isAv1Main10Supported();
            hevc444 = decoderRenderer.isHevcRext8_444Supported();
            hevc44410 = decoderRenderer.isHevcRext10_444Supported();
            av1444 = decoderRenderer.isAv1High8_444Supported();
            av144410 = decoderRenderer.isAv1High10_444Supported();
            pyroWave = prefConfig.videoFormat == PreferenceConfiguration.FormatOption.FORCE_PYROWAVE
                    && MoonBridge.isPyroWaveSupported();
        } catch (RuntimeException | UnsatisfiedLinkError e) {
            LimeLog.warning("[VIPLE-QXR-ACT] format probe failed, advertising only H.264/HEVC: " + e);
        }
        if (av1 || hevc10 || av1Main10 || hevc444 || hevc44410 || av1444 || av144410 || pyroWave) {
            LimeLog.info("[VIPLE-QXR-ACT] probed but not advertised this stage"
                    + " av1=" + av1
                    + " hevc10=" + hevc10
                    + " av1Main10=" + av1Main10
                    + " hevc444=" + hevc444
                    + " hevc44410=" + hevc44410
                    + " av1444=" + av1444
                    + " av144410=" + av144410
                    + " pyro=" + pyroWave);
        }
        if (prefConfig.videoFormat == PreferenceConfiguration.FormatOption.FORCE_HEVC
                && !decoderRenderer.isHevcSupported()) {
            LimeLog.warning("[VIPLE-QXR-ACT] HEVC required but no decoder");
        }
        if (prefConfig.videoFormat == PreferenceConfiguration.FormatOption.FORCE_AV1) {
            LimeLog.warning("[VIPLE-QXR-ACT] AV1 required; this stage only advertises H.264/HEVC");
        }
        if (prefConfig.videoFormat == PreferenceConfiguration.FormatOption.FORCE_PYROWAVE) {
            LimeLog.warning("[VIPLE-QXR-ACT] PyroWave required; this stage only advertises H.264/HEVC");
        }

        supportedVideoFormats = maskToServerCodecs(supportedVideoFormats,
                getIntent().getIntExtra(Game.EXTRA_SERVER_CODEC_MODE_SUPPORT, 0));
        int probed = supportedVideoFormats;
        supportedVideoFormats &= MoonBridge.VIDEO_FORMAT_H264 | MoonBridge.VIDEO_FORMAT_H265;
        if (probed != supportedVideoFormats) {
            LimeLog.info("[VIPLE-QXR-ACT] keeping only 8-bit H.264/HEVC: 0x"
                    + Integer.toHexString(probed) + " -> 0x" + Integer.toHexString(supportedVideoFormats));
        }
        if ((supportedVideoFormats & MoonBridge.VIDEO_FORMAT_H264) == 0) {
            LimeLog.warning("[VIPLE-QXR-ACT] advertised formats have no H.264: 0x" + Integer.toHexString(supportedVideoFormats));
        }
        LimeLog.info("[VIPLE-QXR-ACT] supportedVideoFormats=0x" + Integer.toHexString(supportedVideoFormats));
        return supportedVideoFormats;
    }

    // §QUEST-VR：主機的 SCM_* 編號和 VIDEO_FORMAT_* 不同，要先對映再取交集，否則格式位元會被誤刪。
    private static int maskToServerCodecs(int supportedVideoFormats, int serverCodecModeSupport) {
        if (serverCodecModeSupport == 0) {
            return supportedVideoFormats;
        }
        int beforeMask = supportedVideoFormats;
        int serverFormatMask = 0;
        if ((serverCodecModeSupport & 0x00000001) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_H264;
        }
        if ((serverCodecModeSupport & 0x00040000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_H264_HIGH8_444;
        }
        if ((serverCodecModeSupport & 0x00000100) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_H265;
        }
        if ((serverCodecModeSupport & 0x00000200) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_H265_MAIN10;
        }
        if ((serverCodecModeSupport & 0x00080000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_H265_REXT8_444;
        }
        if ((serverCodecModeSupport & 0x00100000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_H265_REXT10_444;
        }
        if ((serverCodecModeSupport & 0x00010000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_AV1_MAIN8;
        }
        if ((serverCodecModeSupport & 0x00020000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_AV1_MAIN10;
        }
        if ((serverCodecModeSupport & 0x00200000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_AV1_HIGH8_444;
        }
        if ((serverCodecModeSupport & 0x00400000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_AV1_HIGH10_444;
        }
        if ((serverCodecModeSupport & 0x01000000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_PYROWAVE_MAIN8;
        }
        if ((serverCodecModeSupport & 0x02000000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_PYROWAVE_MAIN10;
        }
        if ((serverCodecModeSupport & 0x04000000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_PYROWAVE_444_8;
        }
        if ((serverCodecModeSupport & 0x08000000) != 0) {
            serverFormatMask |= MoonBridge.VIDEO_FORMAT_PYROWAVE_444_10;
        }
        supportedVideoFormats &= serverFormatMask;
        if (beforeMask != supportedVideoFormats) {
            LimeLog.info("[VIPLE-QXR-ACT] supportedVideoFormats masked to host codecs 0x"
                    + Integer.toHexString(beforeMask) + " -> 0x"
                    + Integer.toHexString(supportedVideoFormats)
                    + " (SCM=0x" + Integer.toHexString(serverCodecModeSupport) + ")");
        }
        return supportedVideoFormats;
    }

    private static int clampPreferredHz(int streamFps) {
        if (streamFps < PREFERRED_HZ_MIN) {
            return PREFERRED_HZ_MIN;
        }
        if (streamFps > PREFERRED_HZ_MAX) {
            return PREFERRED_HZ_MAX;
        }
        return streamFps;
    }

    private static X509Certificate readServerCertificate(byte[] derCertData) {
        if (derCertData == null) {
            return null;
        }
        try {
            return (X509Certificate) CertificateFactory.getInstance("X.509")
                    .generateCertificate(new ByteArrayInputStream(derCertData));
        } catch (CertificateException e) {
            LimeLog.warning("[VIPLE-QXR-ACT] server certificate parse failed: " + e);
            return null;
        }
    }

    // §QUEST-VR：Game.applyCliOverrides 是 private。CLI 的 extra 不在這裡套用的話，vr_screen 會忽略解析度與幀率。
    private void applyCliOverrides(PreferenceConfiguration pref, Intent intent) {
        if (intent.hasExtra("cli_bitrate")) {
            int v = intent.getIntExtra("cli_bitrate", 0);
            if (v > 0) {
                pref.bitrate = v;
                LimeLog.info("[VIPLE-QXR-ACT] bitrate=" + v);
            }
        }
        if (intent.hasExtra("cli_fps")) {
            int v = intent.getIntExtra("cli_fps", 0);
            if (v > 0) {
                pref.fps = v;
                LimeLog.info("[VIPLE-QXR-ACT] fps=" + v);
            }
        }
        if (intent.hasExtra("cli_width")) {
            int v = intent.getIntExtra("cli_width", 0);
            if (v > 0) {
                pref.width = v;
                LimeLog.info("[VIPLE-QXR-ACT] width=" + v);
            }
        }
        if (intent.hasExtra("cli_height")) {
            int v = intent.getIntExtra("cli_height", 0);
            if (v > 0) {
                pref.height = v;
                LimeLog.info("[VIPLE-QXR-ACT] height=" + v);
            }
        }
        if (intent.hasExtra("cli_codec")) {
            int v = intent.getIntExtra("cli_codec", -1);
            if (v >= 0) {
                switch (v) {
                    case 0:
                        pref.videoFormat = PreferenceConfiguration.FormatOption.FORCE_H264;
                        break;
                    case 1:
                        pref.videoFormat = PreferenceConfiguration.FormatOption.FORCE_HEVC;
                        break;
                    case 2:
                        pref.videoFormat = PreferenceConfiguration.FormatOption.FORCE_AV1;
                        break;
                    default:
                        break;
                }
                LimeLog.info("[VIPLE-QXR-ACT] codec=" + v);
            }
        }
        if (intent.hasExtra("cli_enableMpQuic")) {
            pref.enableMpQuic = intent.getBooleanExtra("cli_enableMpQuic", false);
            LimeLog.info("[VIPLE-QXR-ACT] enableMpQuic=" + pref.enableMpQuic);
        }
        if (intent.hasExtra("cli_enableFruc")) {
            pref.enableFruc = intent.getBooleanExtra("cli_enableFruc", false);
            LimeLog.info("[VIPLE-QXR-ACT] enableFruc=" + pref.enableFruc);
        }
        if (intent.hasExtra("cli_perfOverlay")) {
            pref.enablePerfOverlay = intent.getBooleanExtra("cli_perfOverlay", false);
            LimeLog.info("[VIPLE-QXR-ACT] perfOverlay=" + pref.enablePerfOverlay);
        }
        if (intent.hasExtra("cli_autoAdjustBitrate")) {
            cliAutoAdjustBitrate = intent.getBooleanExtra("cli_autoAdjustBitrate", true);
            LimeLog.info("[VIPLE-QXR-ACT] autoAdjustBitrate=" + cliAutoAdjustBitrate);
        }
    }

    private void beginPcvr() {
        if (getIntent().hasExtra("cli_width") || getIntent().hasExtra("cli_height") || getIntent().hasExtra("cli_fps")) {
            LimeLog.info("[VIPLE-QXR-ACT] pcvr ignores cli width, height, and fps");
        }
        int[] eye = parseEyeSize(getIntent().getStringExtra("vr_eye"));
        if (eye != null) {
            hasRequestedEye = true;
            requestedEyeW = eye[0];
            requestedEyeH = eye[1];
        } else if (getIntent().hasExtra("vr_eye")) {
            LimeLog.warning("[VIPLE-QXR-ACT] invalid vr_eye '" + getIntent().getStringExtra("vr_eye")
                    + "'; using recommended eye size");
        }
        if (getIntent().hasExtra("vr_hz")) {
            int hz = getIntent().getIntExtra("vr_hz", 0);
            if (hz > 0) {
                requestedHz = hz;
            } else {
                LimeLog.warning("[VIPLE-QXR-ACT] invalid vr_hz " + hz + "; using view refresh");
            }
        }
        if (requestedHz > 0) {
            int clamped = clampVrHz(requestedHz);
            if (clamped != requestedHz) {
                LimeLog.info("[VIPLE-QXR-ACT] requested vr_hz " + requestedHz + " clamped to " + clamped);
                requestedHz = clamped;
            }
        }
        int width;
        int height;
        if (hasRequestedEye) {
            // §QUEST-PCVR：打包寬是單眼寬的兩倍。溢位會變成負值，nativeCreate 直接失敗。
            if (requestedEyeW > Integer.MAX_VALUE / 2) {
                abortPcvr("eye size is unusable: " + requestedEyeW + "x" + requestedEyeH,
                        R.string.vr_pcvr_bad_view);
                return;
            }
            width = requestedEyeW * 2;
            height = requestedEyeH;
        } else {
            width = PCVR_PLACEHOLDER_WIDTH;
            height = PCVR_PLACEHOLDER_HEIGHT;
            LimeLog.info("[VIPLE-QXR-ACT] pcvr placeholder " + width + "x" + height);
        }
        if (!bringUpXr(width, height, requestedHz)) {
            return;
        }
        pollViewConfig();
    }

    private boolean bringUpXr(int width, int height, int preferredHz) {
        handle = QxrNative.nativeCreate(this, width, height, preferredHz);
        if (handle == 0) {
            String error = QxrNative.nativeLastError();
            if (error == null || error.isEmpty()) {
                error = "nativeCreate returned 0";
            }
            abort(error, true);
            return false;
        }
        LimeLog.info("[VIPLE-QXR-ACT] nativeCreate handle=" + handle
                + " " + width + "x" + height + " preferredHz=" + preferredHz);
        if (!QxrNative.nativeEnablePcvr(handle)) {
            abortPcvr("nativeEnablePcvr failed" + errorSuffix(QxrNative.nativeLastError()),
                    R.string.vr_pcvr_enable_failed);
            return false;
        }
        LimeLog.info("[VIPLE-QXR-ACT] nativeEnablePcvr");
        boundEpoch = sessionEpoch;
        QxrNative.setListener(handle, this);
        if (!QxrNative.nativeStart(handle)) {
            abort("OpenXR session start failed" + errorSuffix(QxrNative.nativeLastError()), true);
            return false;
        }
        LimeLog.info("[VIPLE-QXR-ACT] nativeStart");
        pcvrCreateWidth = width;
        pcvrCreateHeight = height;
        if (isFinishing()) {
            return false;
        }
        return true;
    }

    private void pollViewConfig() {
        final int generation = pcvrPollGeneration;
        final long deadline = SystemClock.uptimeMillis() + VIEW_CONFIG_TIMEOUT_MS;
        pcvrPollHandler.post(new Runnable() {
            @Override
            public void run() {
                if (generation != pcvrPollGeneration || released || isFinishing()) {
                    return;
                }
                float[] cfg = null;
                try {
                    cfg = QxrNative.nativeGetViewConfig(handle);
                } catch (Throwable t) {
                    LimeLog.warning("[VIPLE-QXR-ACT] nativeGetViewConfig failed: " + t);
                }
                if (cfg != null && cfg.length >= VIEW_FLOATS) {
                    LimeLog.info("[VIPLE-QXR-ACT] view config eye=" + cfg[VIEW_EYE_W] + "x" + cfg[VIEW_EYE_H]
                            + " hz=" + cfg[VIEW_HZ]);
                    try {
                        onPcvrViewConfig(cfg);
                    } catch (Exception | UnsatisfiedLinkError e) {
                        LimeLog.severe("[VIPLE-QXR-ACT] pcvr setup failed: " + e);
                        if (!isFinishing()) {
                            abort("could not start OpenXR stream", true);
                        }
                    }
                    return;
                }
                long now = SystemClock.uptimeMillis();
                if (now >= deadline) {
                    if (!isFinishing()) {
                        String why = pcvrAwaitingRecreatedViews
                                ? "recreated session view config timed out after " + VIEW_CONFIG_TIMEOUT_MS + " ms"
                                : "view config timed out after " + VIEW_CONFIG_TIMEOUT_MS + " ms";
                        abortPcvr(why, R.string.vr_pcvr_view_timeout);
                    }
                    return;
                }
                pcvrPollHandler.postDelayed(this, Math.min(VIEW_CONFIG_POLL_MS, deadline - now));
            }
        });
    }

    private void onPcvrViewConfig(float[] cfg) {
        pcvrPollGeneration++;
        pcvrAwaitingRecreatedViews = false;
        int eyeW;
        int eyeH;
        if (hasRequestedEye) {
            eyeW = requestedEyeW;
            eyeH = requestedEyeH;
        } else {
            int[] fit = fitRecommendedEye(Math.round(cfg[VIEW_EYE_W]), Math.round(cfg[VIEW_EYE_H]));
            if (fit == null) {
                abortPcvr("recommended eye size is unusable: " + cfg[VIEW_EYE_W] + "x" + cfg[VIEW_EYE_H],
                        R.string.vr_pcvr_bad_view);
                return;
            }
            eyeW = fit[0];
            eyeH = fit[1];
        }
        int measuredRaw = Math.round(cfg[VIEW_HZ]);
        int hz = requestedHz > 0 ? requestedHz : measuredRaw;
        if (hz <= 0) {
            abortPcvr("view refresh is unusable: " + cfg[VIEW_HZ], R.string.vr_pcvr_bad_view);
            return;
        }
        int streamHz = clampVrHz(hz);
        if (streamHz != hz) {
            LimeLog.info("[VIPLE-QXR-ACT] stream hz " + hz + " clamped to " + streamHz);
            hz = streamHz;
        }
        int displayHz = measuredRaw > 0 ? clampVrHz(measuredRaw) : hz;
        if (measuredRaw > 0 && displayHz != measuredRaw) {
            LimeLog.info("[VIPLE-QXR-ACT] display hz " + measuredRaw + " clamped to " + displayHz);
        }
        if (displayHz < hz) {
            // 協定不接受比 vrHz 低的 vrDispHz。不帶表示與串流更新率相同。
            LimeLog.info("[VIPLE-QXR-ACT] display hz " + displayHz + " is below stream hz " + hz
                    + "; omitting vrDispHz");
            displayHz = 0;
        }
        if (eyeW <= 0 || eyeH <= 0 || eyeW > Integer.MAX_VALUE / 2) {
            abortPcvr("eye size is unusable: " + eyeW + "x" + eyeH, R.string.vr_pcvr_bad_view);
            return;
        }
        int packedW = eyeW * 2;
        int packedH = eyeH;
        if (packedW != pcvrCreateWidth || packedH != pcvrCreateHeight) {
            // 重建是為了解碼器的 AImageReader，不是 projection swapchain。
            // 後者在 nativePcvrStart 才依打包尺寸建立。新 session 的視角還沒鎖，不能拿上一場繼續。
            if (pcvrRecreatedForSize) {
                abortPcvr("recreated session eye size still does not match the packed size",
                        R.string.vr_pcvr_bad_view);
                return;
            }
            pcvrRecreatedForSize = true;
            LimeLog.info("[VIPLE-QXR-ACT] recreate session packed=" + packedW + "x" + packedH
                    + " was " + pcvrCreateWidth + "x" + pcvrCreateHeight
                    + "; waiting for the new view config");
            invalidateSessionEnd();
            destroyXrHandle();
            if (!bringUpXr(packedW, packedH, hz)) {
                return;
            }
            if (isFinishing()) {
                return;
            }
            pcvrAwaitingRecreatedViews = true;
            pollViewConfig();
            return;
        }

        prefConfig.width = packedW;
        prefConfig.height = packedH;
        prefConfig.fps = hz;
        boolean bitrateFromCli = getIntent().hasExtra("cli_bitrate")
                && getIntent().getIntExtra("cli_bitrate", 0) > 0;
        if (!bitrateFromCli) {
            prefConfig.bitrate = VrStreamDefaults.defaultBitrateKbps(packedW, packedH, hz);
            LimeLog.info("[VIPLE-QXR-ACT] bitrate " + prefConfig.bitrate + " kbps (VR default; "
                    + packedW + "x" + packedH + "@" + hz + ")");
        }

        decoderSurface = QxrNative.nativeGetDecoderSurface(handle);
        if (decoderSurface == null) {
            String error = QxrNative.nativeLastError();
            abort("no decoder Surface" + errorSuffix(error), true);
            return;
        }

        decoderRenderer = new MediaCodecDecoderRenderer(
                this,
                prefConfig,
                new CrashListener() {
                    @Override
                    public void notifyCrash(Exception e) {
                        reportedCrash = true;
                        tombstonePrefs.edit()
                                .putInt("CrashCount", tombstonePrefs.getInt("CrashCount", 0) + 1)
                                .commit();
                        LimeLog.severe("[VIPLE-QXR-ACT] decoder crash: " + e);
                    }
                },
                tombstonePrefs.getInt("CrashCount", 0),
                pcvrMetered,
                false,
                pcvrGlPrefs.glRenderer,
                new PerfOverlayListener() {
                    @Override
                    public void onPerfUpdate(String text) {
                    }
                });
        decoderRenderer.setDirectOutputSurface(decoderSurface);
        // §QUEST-PCVR：PTS 用幀號乘固定步距，host 才對得回這一幀。
        decoderRenderer.setVrSession(true);
        if (!decoderRenderer.isAvcSupported()) {
            abort("no H.264 hardware decoder", true);
            return;
        }

        int supportedVideoFormats = VrStreamDefaults.retainVrCodecs(
                buildSupportedVideoFormats(),
                decoderRenderer.isHevcSupported(),
                prefConfig.videoFormat);
        LimeLog.info("[VIPLE-QXR-ACT] vr codecs mask=0x" + Integer.toHexString(supportedVideoFormats));

        VrLaunchConfig launch = new VrLaunchConfig();
        launch.eyeWidth = eyeW;
        launch.eyeHeight = eyeH;
        launch.refreshHz = hz;
        // §QUEST-RATE：Frame 只在顯示 Hz 和串流 Hz 不同時才把 vrDispHz 放進 /launch
        // （session.cpp 設 displayHz；VrLaunchConfig.toQuery 在兩者相等或 displayHz 為 0 時不帶）。
        // 這裡的 displayHz 已夾進協定範圍；比串流低時是 0，避免 400 VR_BAD_PARAMS。
        launch.displayHz = displayHz;
        // §QUEST-PCVR：toQuery 在 periodNs 為 0 時用 1e9/refreshHz 的整數除法。先寫上，避免兩處以後分叉。
        launch.periodNs = 1000000000L / hz;
        launch.controller = "touch";
        writeFov(launch, 0,
                cfg[VIEW_LEFT_ANGLE], cfg[VIEW_LEFT_ANGLE + 1],
                cfg[VIEW_LEFT_ANGLE + 2], cfg[VIEW_LEFT_ANGLE + 3]);
        writeFov(launch, 1,
                cfg[VIEW_RIGHT_ANGLE], cfg[VIEW_RIGHT_ANGLE + 1],
                cfg[VIEW_RIGHT_ANGLE + 2], cfg[VIEW_RIGHT_ANGLE + 3]);
        writeEyePose(launch, 0,
                cfg[VIEW_LEFT_POS], cfg[VIEW_LEFT_POS + 1], cfg[VIEW_LEFT_POS + 2],
                cfg[VIEW_LEFT_ROT], cfg[VIEW_LEFT_ROT + 1], cfg[VIEW_LEFT_ROT + 2], cfg[VIEW_LEFT_ROT + 3]);
        writeEyePose(launch, 7,
                cfg[VIEW_RIGHT_POS], cfg[VIEW_RIGHT_POS + 1], cfg[VIEW_RIGHT_POS + 2],
                cfg[VIEW_RIGHT_ROT], cfg[VIEW_RIGHT_ROT + 1], cfg[VIEW_RIGHT_ROT + 2], cfg[VIEW_RIGHT_ROT + 3]);
        // §QUEST-PCVR：setIpd 會把 eyeToHead 覆寫成只有水平偏移的單位四元數。IPD 只寫欄位。
        // 瞳距是兩眼的三維距離，不是只有水平差。
        float ipdX = cfg[VIEW_RIGHT_POS] - cfg[VIEW_LEFT_POS];
        float ipdY = cfg[VIEW_RIGHT_POS + 1] - cfg[VIEW_LEFT_POS + 1];
        float ipdZ = cfg[VIEW_RIGHT_POS + 2] - cfg[VIEW_LEFT_POS + 2];
        launch.ipd = (int) Math.round(Math.sqrt(ipdX * ipdX + ipdY * ipdY + ipdZ * ipdZ) * 100000.0);
        if (cfg[VIEW_SEATED_VALID] != 0f) {
            launch.hasSeated = true;
            launch.seatedPosMm[0] = (int) Math.round(cfg[VIEW_SEATED_X] * 1000.0);
            launch.seatedPosMm[1] = (int) Math.round(cfg[VIEW_SEATED_Y] * 1000.0);
            launch.seatedPosMm[2] = (int) Math.round(cfg[VIEW_SEATED_Z] * 1000.0);
            // 弧度換成毫度。57295.7795＝1000×180/π，跟 Frame session.cpp 同一係數。
            launch.seatedYawMdeg = (int) Math.round(cfg[VIEW_SEATED_YAW] * 57295.7795);
        } else {
            LimeLog.info("[VIPLE-QXR-ACT] seated zero unavailable; vrSeated omitted");
        }
        if (cfg[VIEW_TIME_CONV] == 0f) {
            // 沒有 XrTime 換算時，上行不能離開影格執行緒。toQuery 既有欄位的格式不變，只拿掉這一個位元。
            launch.caps &= ~VrLaunchConfig.CAP_TRACK_THREAD;
            LimeLog.info("[VIPLE-QXR-ACT] no XrTime conversion; clearing CAP_TRACK_THREAD");
        }
        if (cfg[VIEW_AREA_W] > 0f && cfg[VIEW_AREA_D] > 0f) {
            launch.playAreaWidthMm = (int) Math.round(cfg[VIEW_AREA_W] * 1000.0);
            launch.playAreaDepthMm = (int) Math.round(cfg[VIEW_AREA_D] * 1000.0);
        } else if (cfg[VIEW_AREA_W] > 0f || cfg[VIEW_AREA_D] > 0f) {
            LimeLog.info("[VIPLE-QXR-ACT] play area incomplete widthM=" + cfg[VIEW_AREA_W]
                    + " depthM=" + cfg[VIEW_AREA_D]);
        }
        launch.codecs = VrStreamDefaults.launchCodecMask(supportedVideoFormats);
        LimeLog.info("[VIPLE-QXR-ACT] pcvr eye=" + eyeW + "x" + eyeH
                + " packed=" + packedW + "x" + packedH
                + " hz=" + hz
                + " displayHz=" + launch.displayHz
                + " periodNs=" + launch.periodNs
                + " ipd=" + launch.ipd
                + " seated=" + (launch.hasSeated
                        ? (launch.seatedPosMm[0] + "," + launch.seatedPosMm[1] + "," + launch.seatedPosMm[2]
                        + "," + launch.seatedYawMdeg)
                        : "omitted")
                + " caps=0x" + Integer.toHexString(launch.caps)
                + " fov=" + launch.fovTan[0] + "," + launch.fovTan[1] + "," + launch.fovTan[2] + "," + launch.fovTan[3]
                + "," + launch.fovTan[4] + "," + launch.fovTan[5] + "," + launch.fovTan[6] + "," + launch.fovTan[7]
                + " play=" + launch.playAreaWidthMm + "x" + launch.playAreaDepthMm
                + " codecs=" + launch.codecs);

        // §QUEST-PCVR：這時候的 2D 面板更新率不是雙眼顯示的更新率。
        StreamConfiguration config = new StreamConfiguration.Builder()
                .setResolution(packedW, packedH)
                .setLaunchRefreshRate(hz)
                .setRefreshRate(hz)
                .setApp(pcvrApp)
                .setBitrate(prefConfig.bitrate)
                .setEnableSops(prefConfig.enableSops)
                .enableLocalAudioPlayback(prefConfig.playHostAudio)
                .setMaxPacketSize(prefConfig.packetSize)
                .setRemoteConfiguration(StreamConfiguration.STREAM_CFG_AUTO)
                .setSupportedVideoFormats(supportedVideoFormats)
                .setAttachedGamepadMask(0)
                .setClientRefreshRateX100(hz * 100)
                .setAudioConfiguration(prefConfig.audioConfiguration)
                .setColorSpace(decoderRenderer.getPreferredColorSpace())
                .setColorRange(decoderRenderer.getPreferredColorRange())
                .setPersistGamepadsAfterDisconnect(!prefConfig.multiController)
                .setMpQuic(prefConfig.enableMpQuic, prefConfig.mpQuicScheduler)
                .setAutoAdjustBitrate(cliAutoAdjustBitrate != null ? cliAutoAdjustBitrate : true)
                .setVrLaunch(launch)
                .build();

        conn = new NvConnection(getApplicationContext(),
                new ComputerDetails.AddressTuple(pcvrHost, pcvrPort),
                pcvrHttpsPort, pcvrUniqueId, config,
                PlatformBinding.getCryptoProvider(this), pcvrCert);
        conn.setRelayConfig(prefConfig.relayUrl, prefConfig.relayPsk,
                getIntent().getStringExtra(Game.EXTRA_PC_UUID),
                getIntent().getStringExtra(Game.EXTRA_STUN_ADDRESS),
                getIntent().getIntExtra(Game.EXTRA_STUN_PUNCH_PORT, 0));

        pcvrHz = hz;
        pcvrPackedW = packedW;
        pcvrPackedH = packedH;
        streamHost = pcvrHost;
        streamHttpsPort = pcvrHttpsPort;
        streamServerCert = pcvrCert;
        connectionStartRequested = true;
        LimeLog.info("[VIPLE-QXR-ACT] conn.start");
        conn.start(new AndroidAudioRenderer(this, prefConfig.enableAudioFx), decoderRenderer, this);
    }

    private void destroyXrHandle() {
        long session = handle;
        handle = 0;
        if (session == 0) {
            return;
        }
        try {
            QxrNative.setListener(session, null);
        } catch (Throwable t) {
            LimeLog.warning("[VIPLE-QXR-ACT] clear listener failed: " + t);
        }
        try {
            QxrNative.nativeStop(session);
        } catch (Throwable t) {
            LimeLog.warning("[VIPLE-QXR-ACT] nativeStop failed: " + t);
        }
        try {
            QxrNative.nativeDestroy(session);
        } catch (Throwable t) {
            LimeLog.warning("[VIPLE-QXR-ACT] nativeDestroy failed: " + t);
        }
    }

    private void invalidateSessionEnd() {
        sessionEpoch++;
    }

    private void startPcvrUplink() {
        long session = handle;
        if (session == 0 || pcvrPackedW <= 0 || pcvrPackedH <= 0) {
            LimeLog.severe("[VIPLE-QXR-ACT] nativePcvrStart skipped: session is not ready");
            failPcvrFromAnyThread(R.string.vr_pcvr_tracking_failed);
            return;
        }
        LimeLog.info("[VIPLE-QXR-ACT] nativePcvrStart hz=" + pcvrHz
                + " packed=" + pcvrPackedW + "x" + pcvrPackedH);
        boolean ok;
        try {
            ok = QxrNative.nativePcvrStart(session, pcvrHz, pcvrPackedW, pcvrPackedH);
        } catch (Throwable t) {
            LimeLog.severe("[VIPLE-QXR-ACT] nativePcvrStart threw: " + t);
            failPcvrFromAnyThread(R.string.vr_pcvr_tracking_failed);
            return;
        }
        if (!ok) {
            LimeLog.severe("[VIPLE-QXR-ACT] nativePcvrStart failed" + errorSuffix(QxrNative.nativeLastError()));
            failPcvrFromAnyThread(R.string.vr_pcvr_tracking_failed);
        }
    }

    private static int[] parseEyeSize(String eye) {
        if (eye == null || eye.isEmpty()) {
            return null;
        }
        String lowered = eye.toLowerCase(Locale.US);
        int split = lowered.indexOf('x');
        if (split <= 0 || split + 1 >= lowered.length()) {
            return null;
        }
        try {
            int width = Integer.parseInt(lowered.substring(0, split).trim());
            int height = Integer.parseInt(lowered.substring(split + 1).trim());
            if (width <= 0 || height <= 0) {
                return null;
            }
            return new int[] { width, height };
        } catch (NumberFormatException ignored) {
            return null;
        }
    }

    // §QUEST-PCVR：host 編碼與解碼器要 16 的倍數，預設碼率下單眼超過 2304 畫質也不划算。先縮進上限再對齊，對齊後仍不超過上限。
    private static int[] fitRecommendedEye(int recommendedW, int recommendedH) {
        if (recommendedW <= 0 || recommendedH <= 0) {
            return null;
        }
        double scale = 1.0;
        if (recommendedW > EYE_MAX) {
            scale = Math.min(scale, EYE_MAX / (double) recommendedW);
        }
        if (recommendedH > EYE_MAX) {
            scale = Math.min(scale, EYE_MAX / (double) recommendedH);
        }
        int width = align16(recommendedW * scale);
        int height = align16(recommendedH * scale);
        if (width > EYE_MAX) {
            width = EYE_MAX;
        }
        if (height > EYE_MAX) {
            height = EYE_MAX;
        }
        if (width < EYE_ALIGN) {
            width = EYE_ALIGN;
        }
        if (height < EYE_ALIGN) {
            height = EYE_ALIGN;
        }
        return new int[] { width, height };
    }

    private static int align16(double value) {
        return (int) Math.round(value / EYE_ALIGN) * EYE_ALIGN;
    }

    // §QUEST-PCVR：左、下在 OpenXR 是負角，協定存 tan 的絕對值。過大或過小的值夾到跟 Qt client 同一範圍，避免壞角度進 /launch。
    private static void writeFov(VrLaunchConfig launch, int eye,
                                 float angleLeft, float angleRight, float angleUp, float angleDown) {
        int base = eye * 4;
        launch.fovTan[base] = fovTanFromAngle(angleLeft, true);
        launch.fovTan[base + 1] = fovTanFromAngle(angleRight, false);
        launch.fovTan[base + 2] = fovTanFromAngle(angleUp, false);
        launch.fovTan[base + 3] = fovTanFromAngle(angleDown, true);
    }

    private static int fovTanFromAngle(float radians, boolean negate) {
        double tangent = Math.tan(negate ? -radians : radians);
        double magnitude = Math.abs(tangent);
        if (Double.isNaN(magnitude) || Double.isInfinite(magnitude) || magnitude < 0.01) {
            magnitude = 0.01;
        } else if (magnitude > 10.0) {
            magnitude = 10.0;
        }
        return (int) Math.round(magnitude * 10000.0);
    }

    private static void writeEyePose(VrLaunchConfig launch, int base,
                                     float x, float y, float z,
                                     float qx, float qy, float qz, float qw) {
        // §QUEST-PCVR：位置是公尺，欄位是 mm×100；四元數欄位是 ×10000。
        launch.eyeToHead[base] = (int) Math.round(x * 100000.0);
        launch.eyeToHead[base + 1] = (int) Math.round(y * 100000.0);
        launch.eyeToHead[base + 2] = (int) Math.round(z * 100000.0);
        launch.eyeToHead[base + 3] = (int) Math.round(qx * 10000.0);
        launch.eyeToHead[base + 4] = (int) Math.round(qy * 10000.0);
        launch.eyeToHead[base + 5] = (int) Math.round(qz * 10000.0);
        launch.eyeToHead[base + 6] = (int) Math.round(qw * 10000.0);
    }

    private static String errorSuffix(String error) {
        if (error == null || error.isEmpty()) {
            return "";
        }
        return ": " + error;
    }

    private void abort(String message, boolean toast) {
        LimeLog.severe("[VIPLE-QXR-ACT] " + message);
        if (toast) {
            Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        }
        finish();
    }

    private void abortPcvr(String logMessage, int toastRes) {
        LimeLog.severe("[VIPLE-QXR-ACT] " + logMessage);
        Toast.makeText(this, toastRes, Toast.LENGTH_LONG).show();
        finish();
    }

    private void failPcvrFromAnyThread(final int toastRes) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (!isFinishing() && !isDestroyed()) {
                    Toast.makeText(VrStreamActivity.this, toastRes, Toast.LENGTH_LONG).show();
                    finish();
                }
            }
        });
    }

    private void finishOnMainThread() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (!isFinishing() && !isDestroyed()) {
                    finish();
                }
            }
        });
    }

    private void releaseSession() {
        sIsStreaming = false;
        synchronized (releaseLock) {
            // onPause、onStop、onDestroy 都會進來。第二次直接返回，避免重複 stop 與重複 /cancel。
            if (released) {
                return;
            }
            released = true;
        }

        if (pcvrMode) {
            pcvrPollGeneration++;
            pcvrPollHandler.removeCallbacksAndMessages(null);
        }

        // §QUEST-VR：Game 在拆連線前先 prepareForStop。沒先做的話，拆連線期間解碼器的例外會被當成崩潰。
        if (decoderRenderer != null) {
            try {
                decoderRenderer.prepareForStop();
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] prepareForStop failed: " + t);
            }
        }

        // conn.stop() 可能在這條執行緒同步進 connectionTerminated 並改掉旗標。要先抄下來。
        final boolean ownership = releaseHostOwnership;
        final boolean started = connectionStartRequested;
        final String cancelHost = streamHost;
        final int cancelHttpsPort = streamHttpsPort;
        final X509Certificate cancelCert = streamServerCert;
        final Context appContext = getApplicationContext();

        if (pcvrMode && handle != 0) {
            try {
                // §QUEST-PCVR：上行要在拆連線前停。nativeStop 還在解碼器放掉之後，那時連線已經沒了。
                QxrNative.nativePcvrStop(handle);
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] nativePcvrStop failed: " + t);
            }
        }

        if (started && conn != null) {
            try {
                conn.stop();
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] conn.stop failed: " + t);
            }
        }

        // 跟 Game 正常離開一樣：conn.stop() 只拆串流，HTTPS /cancel 才放掉 host 上的 app。
        // quitApp() 沒有短逾時，不能放在主執行緒。這裡不等那條執行緒。
        if (started && !ownership) {
            LimeLog.info("[VIPLE-QXR-ACT] unexpected disconnect: not sending /cancel");
        } else if (started && cancelHost != null && cancelHttpsPort != 0 && cancelCert != null) {
            new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        NvHTTP cancelHttp = new NvHTTP(
                                new ComputerDetails.AddressTuple(cancelHost, NvHTTP.DEFAULT_HTTP_PORT),
                                cancelHttpsPort,
                                "0123456789ABCDEF",
                                cancelCert,
                                PlatformBinding.getCryptoProvider(appContext));
                        cancelHttp.quitApp();
                        LimeLog.info("[VIPLE-QXR-ACT] sent /cancel");
                    } catch (Exception e) {
                        LimeLog.warning("[VIPLE-QXR-ACT] /cancel failed: " + e);
                    }
                }
            }, "qxr-cancel").start();
        } else if (started) {
            LimeLog.info("[VIPLE-QXR-ACT] skip /cancel: missing host, https port, or cert");
        }

        if (decoderRenderer != null) {
            try {
                // §QUEST-VR：conn.stop 只在 MoonBridge 已接手時才會 stop/cleanup。連線沒建立就要自己放。
                // 已經放過的話 cleanup 看到 videoDecoder 為 null 會直接返回。
                decoderRenderer.stop();
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] decoder.stop failed: " + t);
            }
            try {
                decoderRenderer.cleanup();
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] decoder.cleanup failed: " + t);
            }
            decoderRenderer = null;
        }

        if (decoderSurface != null) {
            try {
                // ANativeWindow_toSurface 的 Java Surface 要由呼叫端 release。direct output 不會代放。
                // 解碼器 cleanup 之後才放，避免 MediaCodec 還握著這張 Surface。
                decoderSurface.release();
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] decoderSurface.release failed: " + t);
            }
            decoderSurface = null;
        }

        long session = handle;
        handle = 0;
        // 結束回呼若在 nativeStop 之後才清，拆 session 的同時還會進 onSessionEnded。
        // destroyXrHandle 也是先清 listener。nativePcvrStop 仍在 conn.stop 之前。
        try {
            QxrNative.setListener(session, null);
        } catch (Throwable t) {
            LimeLog.warning("[VIPLE-QXR-ACT] clear listener failed: " + t);
        }
        if (session != 0) {
            try {
                QxrNative.nativeStop(session);
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] nativeStop failed: " + t);
            }
            try {
                QxrNative.nativeDestroy(session);
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] nativeDestroy failed: " + t);
            }
        }

        // §QUEST-VR：跟 Game 共用 DecoderTombstone。正常結束不清掉的話，下次會一直關掉參考影格失效。
        if (tombstonePrefs != null && connectionStartRequested && !reportedCrash
                && tombstonePrefs.getInt("CrashCount", 0) != 0) {
            try {
                tombstonePrefs.edit()
                        .putInt("CrashCount", 0)
                        .putInt("LastNotifiedCrashCount", 0)
                        .apply();
            } catch (Throwable t) {
                LimeLog.warning("[VIPLE-QXR-ACT] clear decoder crash count failed: " + t);
            }
        }
    }

    @Override
    public void onSessionEnded(int reason) {
        if (boundEpoch != sessionEpoch) {
            // §QUEST-PCVR：拆掉暫定 session 時，舊的結束回呼不能把已經量到視角的那一場關掉。
            LimeLog.info("[VIPLE-QXR-ACT] onSessionEnded ignored reason=" + reason);
            return;
        }
        LimeLog.info("[VIPLE-QXR-ACT] onSessionEnded reason=" + reason);
        finishOnMainThread();
    }

    @Override
    public void onVrServerError(int code) {
        if (boundEpoch != sessionEpoch || !pcvrMode) {
            LimeLog.info("[VIPLE-QXR-ACT] onVrServerError ignored code=" + code);
            return;
        }
        LimeLog.severe("[VIPLE-QXR-ACT] host could not start VR code=" + code);
        // 這個回呼在 VR message sink 執行緒，bridge 還握著讀鎖。這裡不能呼叫 nativeStop，
        // 否則跟 stop 的寫鎖互等。先回主執行緒再 finish，releaseSession 才拆 session。
        final String text = hostVrError(code);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (isFinishing() || isDestroyed()) {
                    return;
                }
                Toast.makeText(VrStreamActivity.this, text, Toast.LENGTH_LONG).show();
                finish();
            }
        });
    }

    private static String hostVrError(int code) {
        // 編號是 VIPLE_VR_STATE_CODE_*。Java 不能 include 那個標頭。
        switch (code) {
            case 1:
            case 8:
                return "Another headset is using SteamVR on the host. Disconnect it, then try again.";
            case 2:
                return "SteamVR on the host started in safe mode and blocked the VipleStream driver. Restart SteamVR on the host, then try again.";
            case 5:
                return "SteamVR is not installed on the host.";
            case 6:
                return "Nobody is signed in on the host.";
            case 7:
            case 10:
                return "SteamVR on the host did not start in time. Try again.";
            case 9:
                return "A VR game is already running on the host without VipleStream. Quit it, then try again.";
            case 17:
                return "Steam is not signed in on the host. Sign in on the host, then try again.";
            case 19:
                return "SteamVR on the host has to restart, but a VR game is still running there. Quit the game, then try again.";
            case 22:
                return "The host is on the sign-in or lock screen. Sign in on the host, then try again.";
            default:
                return "The host could not start VR (error " + code + ").";
        }
    }

    private static int clampVrHz(int hz) {
        if (hz < VR_HZ_MIN) {
            return VR_HZ_MIN;
        }
        if (hz > VR_HZ_MAX) {
            return VR_HZ_MAX;
        }
        return hz;
    }

    @Override
    public void stageStarting(String stage) {
        // §QUEST-VR：這一階段沒有進度畫面，也不處理輸入。
    }

    @Override
    public void stageComplete(String stage) {
        // §QUEST-VR：這一階段沒有進度畫面，也不處理輸入。
    }

    @Override
    public void stageFailed(String stage, int portFlags, int errorCode) {
        LimeLog.severe("[VIPLE-QXR-ACT] stageFailed stage=" + stage
                + " portFlags=" + portFlags + " error=" + errorCode);
        finishOnMainThread();
    }

    @Override
    public void connectionStarted() {
        if (pcvrMode) {
            // §QUEST-PCVR：這次回呼在連線執行緒，而 nativePcvrStart 會等 frame thread 把 projection swapchain 建好才返回。
            // 先返回，改在主執行緒呼叫，避免連線啟動和 frame thread 互相等待。
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    if (isFinishing() || isDestroyed() || !pcvrMode) {
                        return;
                    }
                    startPcvrUplink();
                }
            });
            return;
        }
        // §QUEST-VR：螢幕喚醒在 onCreate 就設了。這一階段不處理輸入。
    }

    @Override
    public void connectionTerminated(int errorCode) {
        LimeLog.severe("[VIPLE-QXR-ACT] connectionTerminated error=" + errorCode);
        // 非正常結束不送 /cancel。host 會留著 app 一段時間，讓同一台裝置接回去。跟 Game 一樣。
        // 要在 finish 之前寫，onPause(isFinishing) 會同步進 releaseSession。
        if (errorCode != MoonBridge.ML_ERROR_GRACEFUL_TERMINATION) {
            releaseHostOwnership = false;
        }
        finishOnMainThread();
    }

    @Override
    public void connectionStatusUpdate(int connectionStatus) {
        // §QUEST-VR：這一階段不顯示連線品質，也不處理輸入。
    }

    @Override
    public void displayMessage(String message) {
        LimeLog.info("[VIPLE-QXR-ACT] displayMessage " + message);
    }

    @Override
    public void displayTransientMessage(String message) {
        LimeLog.info("[VIPLE-QXR-ACT] displayTransientMessage " + message);
    }

    @Override
    public void rumble(short controllerNumber, short lowFreqMotor, short highFreqMotor) {
        // §QUEST-VR：這一階段不處理輸入。
    }

    @Override
    public void rumbleTriggers(short controllerNumber, short leftTrigger, short rightTrigger) {
        // §QUEST-VR：這一階段不處理輸入。
    }

    @Override
    public void setHdrMode(boolean enabled, byte[] hdrMetadata) {
        // §QUEST-VR：這一階段不開 HDR，也不處理輸入。
    }

    @Override
    public void setMotionEventState(short controllerNumber, byte motionType, short reportRateHz) {
        // §QUEST-VR：這一階段不處理輸入。
    }

    @Override
    public void setControllerLED(short controllerNumber, byte r, byte g, byte b) {
        // §QUEST-VR：這一階段不處理輸入。
    }
}
