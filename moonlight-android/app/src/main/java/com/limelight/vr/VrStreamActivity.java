package com.limelight.vr;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.os.Bundle;
import android.view.Display;
import android.view.Surface;
import android.view.WindowManager;
import android.widget.Toast;

import com.limelight.Game;
import com.limelight.LimeLog;
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
import com.limelight.preferences.GlPreferences;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.ByteArrayInputStream;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;

/**
 * §QUEST-VR：頭盔裡一塊平面畫面，走一般 2D 串流。不送 vr* 參數，也不處理輸入。
 */
public class VrStreamActivity extends Activity implements NvConnectionListener, QxrNative.SessionListener {
    private static final int PREFERRED_HZ_MIN = 72;
    private static final int PREFERRED_HZ_MAX = 120;

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
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

    private void abort(String message, boolean toast) {
        LimeLog.severe("[VIPLE-QXR-ACT] " + message);
        if (toast) {
            Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        }
        finish();
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
        synchronized (releaseLock) {
            // onPause、onStop、onDestroy 都會進來。第二次直接返回，避免重複 stop 與重複 /cancel。
            if (released) {
                return;
            }
            released = true;
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
        try {
            QxrNative.setListener(session, null);
        } catch (Throwable t) {
            LimeLog.warning("[VIPLE-QXR-ACT] clear listener failed: " + t);
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
        LimeLog.info("[VIPLE-QXR-ACT] onSessionEnded reason=" + reason);
        finishOnMainThread();
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
