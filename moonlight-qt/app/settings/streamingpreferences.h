#pragma once

#include <QObject>
#include <QRect>
#include <QQmlEngine>

class StreamingPreferences : public QObject
{
    Q_OBJECT

public:
    static StreamingPreferences* get(QQmlEngine *qmlEngine = nullptr);

    // VipleStream: `fruc` reflects whether frame interpolation is
    // ON (enableFrameInterpolation). When ON, the host actually
    // encodes at fps/2 (see session.cpp — we request half frame rate
    // from the server and reconstruct locally via FRUC), so the
    // default bitrate must be computed against the *host-side*
    // frame rate, not the user's target frame rate. A small
    // headroom multiplier is then applied so the halved source
    // keeps enough detail for the interpolator to work from.
    // Defaulted to false so existing call sites that don't care
    // (legacy) behave as before.
    Q_INVOKABLE static int
    getDefaultBitrate(int width, int height, int fps, bool yuv444, bool fruc = false);

    Q_INVOKABLE void save();

    void reload();

    enum AudioConfig
    {
        AC_STEREO,
        AC_51_SURROUND,
        AC_71_SURROUND
    };
    Q_ENUM(AudioConfig)

    enum VideoCodecConfig
    {
        VCC_AUTO,
        VCC_FORCE_H264,
        VCC_FORCE_HEVC,
        VCC_FORCE_HEVC_HDR_DEPRECATED, // Kept for backwards compatibility
        VCC_FORCE_AV1
    };
    Q_ENUM(VideoCodecConfig)

    enum VideoDecoderSelection
    {
        VDS_AUTO,
        VDS_FORCE_HARDWARE,
        VDS_FORCE_SOFTWARE
    };
    Q_ENUM(VideoDecoderSelection)

    enum WindowMode
    {
        WM_FULLSCREEN,
        WM_FULLSCREEN_DESKTOP,
        WM_WINDOWED
    };
    Q_ENUM(WindowMode)

    enum UIDisplayMode
    {
        UI_WINDOWED,
        UI_MAXIMIZED,
        UI_FULLSCREEN
    };
    Q_ENUM(UIDisplayMode)

    // New entries must go at the end of the enum
    // to avoid renumbering existing entries (which
    // would affect existing user preferences).
    enum Language
    {
        LANG_AUTO,
        LANG_EN,
        LANG_FR,
        LANG_ZH_CN,
        LANG_DE,
        LANG_NB_NO,
        LANG_RU,
        LANG_ES,
        LANG_JA,
        LANG_VI,
        LANG_TH,
        LANG_KO,
        LANG_HU,
        LANG_NL,
        LANG_SV,
        LANG_TR,
        LANG_UK,
        LANG_ZH_TW,
        LANG_PT,
        LANG_PT_BR,
        LANG_EL,
        LANG_IT,
        LANG_HI,
        LANG_PL,
        LANG_CS,
        LANG_HE,
        LANG_CKB,
        LANG_LT,
        LANG_ET,
        LANG_BG,
        LANG_EO,
        LANG_TA,
    };
    Q_ENUM(Language);

    enum CaptureSysKeysMode
    {
        CSK_OFF,
        CSK_FULLSCREEN,
        CSK_ALWAYS,
    };
    Q_ENUM(CaptureSysKeysMode);

    enum FrucBackend
    {
        FB_GENERIC,     // D3D11/GLES compute shader (low latency, default)
        FB_NVIDIA_OF,   // NVIDIA Optical Flow via CUDA (higher quality, higher latency)
        FB_DIRECTML,    // DirectML-based ML interpolation (Windows D3D11, iGPU/dGPU agnostic)
        FB_NCNN,        // NCNN-Vulkan RIFE (cross-vendor: NV/AMD/Intel; v1.3.x)
    };
    Q_ENUM(FrucBackend);

    // §J.3.e.2.i — VipleStream renderer backend.  使用者在 Settings 下拉
    // 選單裡切.
    //
    // RS_AUTO（v1.4.137+ 預設）= 走標準 hwaccel cascade（Windows 上 D3D11VA
    // 優先 → DXVA2 → Vulkan → SW），讓 ffmpeg 自己挑第一個能 init 的。等同
    // upstream Moonlight 行為.  RS_AUTO 跟 RS_D3D11 目前在 ffmpeg.cpp 的
    // helper 裡走同一條 path（shouldPreferVulkanDecoderCascade 都 false）—
    // 將來若加 GPU-detect 邏輯（譬如 RTX 40 系列偏好 Vulkan native decode），
    // 邏輯放在 helper 內判 RS_AUTO 即可，RS_D3D11 仍保 hard-pin D3D11 語意.
    //
    // RS_D3D11 = 跟 RS_AUTO 行為一樣，但使用者明確選了 D3D11；保留以後當
    // 「不允許 helper 自動切到 Vulkan」的 hard-pin 用.
    //
    // RS_VULKAN = 強制 Vulkan-first cascade + VkFrucRenderer (SW upload +
    // dual-present + Vulkan compute FRUC).  AMD APU / 部分機種 native vulkan
    // video decode 不穩，user 自願承擔.
    //
    // FRUC engine 對應：RS_AUTO/RS_D3D11 用 frucBackend 設定的 backend；
    // RS_VULKAN 因為還沒接 IFRUCBackend 抽象，固定用 VkFruc 內建的 ME→
    // median→warp compute（行為等同 FB_GENERIC），UI 會把 frucBackend
    // dropdown disable + 顯示提示.
    //
    // Enum 值穩定（QSettings backwards-compat）：RS_VULKAN=0 / RS_D3D11=1 /
    // RS_AUTO=2 是 v1.4.137 新增.
    enum RendererSelection
    {
        RS_VULKAN = 0,  // 強制 Vulkan-first cascade + VkFrucRenderer
        RS_D3D11  = 1,  // 標準 cascade（D3D11VA 優先），hard-pin 不切 Vulkan
        RS_AUTO   = 2,  // 預設：標準 cascade，未來可接 GPU-detect
    };
    Q_ENUM(RendererSelection);

    enum FrucQuality
    {
        FQ_QUALITY,      // Best visual quality, higher GPU load
        FQ_BALANCED,     // Recommended: quality/performance balance
        FQ_PERFORMANCE,  // Lowest latency, suitable for iGPU
    };
    Q_ENUM(FrucQuality);

    // §J.3.e.2.i.11 (v1.4.66) — cross-hardware FRUC auto-tier 分級。
    // VkFrucRenderer 啟動時根據 GPU 強度自動選 path + inferDim，避免
    // 使用者手動 tune 不同 HW 的 setting。Tier 由兩階段偵測決定：
    //   1. v1.4.66: deviceName + limits heuristic (粗判)
    //   2. v1.4.67: 1-shot Conv2D micro-benchmark (細修)
    // 各 tier 自動套用的 path:
    //   ENTRY        — 內顯 / 老卡 / Vulkan 1.1: 補幀全關
    //   PERFORMANCE  — GTX 10xx / RX 5xx: block-match only (~4ms chain)
    //   BALANCED     — RTX 30xx / RX 6700+: RIFE β.5.1 inferDim=128 (~10ms)
    //   QUALITY      — RTX 40xx / RX 7800+: RIFE β.5.1 inferDim=256 (~14ms)
    enum VkfrucGpuTier
    {
        VGT_UNKNOWN     = 0,
        VGT_ENTRY       = 1,
        VGT_PERFORMANCE = 2,
        VGT_BALANCED    = 3,
        VGT_QUALITY     = 4,
    };
    Q_ENUM(VkfrucGpuTier);

    // VipleStream editorial design variant selector.
    // DV_SAFE — quiet editorial grid (thin rules, monospace meta, 8pt-row layouts)
    // DV_BOLD — magazine-cover energy (oversized display type, wider mastheads)
    enum DesignVariant
    {
        DV_SAFE,
        DV_BOLD,
    };
    Q_ENUM(DesignVariant);

    // VipleStream H Phase 2.2: Apps view sort mode.
    // - ASM_DEFAULT: manual entries from apps.json pinned at top (ordered
    //   alphabetically), then auto-imported (Steam) ordered alphabetically.
    //   This is the original post-H-Phase2 behaviour.
    // - ASM_RECENT: manual entries pinned at top, then Steam games ordered
    //   by LastPlayed descending (most recent first). Games that have
    //   NEVER been launched (LastPlayed==0) fall to the bottom, ordered
    //   alphabetically so they stay discoverable.
    // - ASM_PLAYTIME: manual entries pinned at top, then Steam games
    //   ordered by Playtime descending (most-played first). Unplayed
    //   games at bottom alphabetical — same tiebreaker as ASM_RECENT.
    // - ASM_NAME: pure alphabetical across all entries, ignoring source
    //   and playtime. For users who want upstream Moonlight's behaviour.
    enum AppSortMode
    {
        ASM_DEFAULT,
        ASM_RECENT,
        ASM_PLAYTIME,
        ASM_NAME,
    };
    Q_ENUM(AppSortMode);

    Q_PROPERTY(int width MEMBER width NOTIFY displayModeChanged)
    Q_PROPERTY(int height MEMBER height NOTIFY displayModeChanged)
    Q_PROPERTY(int fps MEMBER fps NOTIFY displayModeChanged)
    Q_PROPERTY(int bitrateKbps MEMBER bitrateKbps NOTIFY bitrateChanged)
    Q_PROPERTY(bool unlockBitrate MEMBER unlockBitrate NOTIFY unlockBitrateChanged)
    Q_PROPERTY(bool autoAdjustBitrate MEMBER autoAdjustBitrate NOTIFY autoAdjustBitrateChanged)
    Q_PROPERTY(bool enableVsync MEMBER enableVsync NOTIFY enableVsyncChanged)
    Q_PROPERTY(bool gameOptimizations MEMBER gameOptimizations NOTIFY gameOptimizationsChanged)
    Q_PROPERTY(bool playAudioOnHost MEMBER playAudioOnHost NOTIFY playAudioOnHostChanged)
    Q_PROPERTY(bool multiController MEMBER multiController NOTIFY multiControllerChanged)
    Q_PROPERTY(bool enableMdns MEMBER enableMdns NOTIFY enableMdnsChanged)
    Q_PROPERTY(bool autoWakeOnLan MEMBER autoWakeOnLan NOTIFY autoWakeOnLanChanged)
    Q_PROPERTY(bool enableFrameInterpolation MEMBER enableFrameInterpolation NOTIFY enableFrameInterpolationChanged)
    Q_PROPERTY(FrucBackend frucBackend MEMBER frucBackend NOTIFY frucBackendChanged)
    Q_PROPERTY(RendererSelection rendererSelection MEMBER rendererSelection NOTIFY rendererSelectionChanged)
    Q_PROPERTY(FrucQuality frucQuality MEMBER frucQuality NOTIFY frucQualityChanged)
    // §B-NVOF UI 整合 2026-05-07 — Vulkan-only 補幀進階開關，跟既有的
    // VIPLE_VKFRUC_NV_OF / VIPLE_VKFRUC_TRIPLE env var 平行：env var 優先
    // (escape hatch / dev override)，沒設 env var 才看 settings.
    // RS_D3D11 path 完全 ignore 這兩個 (UI 也只在 RS_VULKAN 時顯示).
    Q_PROPERTY(bool vkfrucEnableNvOf MEMBER vkfrucEnableNvOf NOTIFY vkfrucEnableNvOfChanged)
    Q_PROPERTY(bool vkfrucEnableTriple MEMBER vkfrucEnableTriple NOTIFY vkfrucEnableTripleChanged)
    // v1.4.153 §R2-γ — Vulkan FRUC 主動/被動模式切換.
    //   false (default): 主動補幀 — server 砍半推 + always-on dual-present.
    //                    可預期低 server 負載, 客戶端 GPU 必須夠強.
    //   true: 被動補幀 — server 全推 UI fps, 客戶端依 recv% 動態升 ratio
    //                    (recv ≥ 70% → 1x pass-through, 40-70% → 2x dual,
    //                    < 40% → 3x triple). 適合 server 端不一定能滿推時.
    Q_PROPERTY(bool vkfrucPassiveMode MEMBER vkfrucPassiveMode NOTIFY vkfrucPassiveModeChanged)
    // §J.3.e.X Path β — native RIFE Vulkan flow extraction + native-res warp.
    // Beta feature; opt-in default-OFF.  Quality much higher than block-match
    // (5/5 score 0.95 ≈ perfect midpoint vs block-match 0% effective).
    // 2026-05-08 β.6 stability fix: VK_ERROR_DEVICE_LOST after 30-60s was
    // overlay-resize use-after-free in drainOverlayStash; fixed with
    // vkDeviceWaitIdle before destroying old VkImage on resize.  Beta tag
    // retained pending multi-GPU / multi-driver validation.
    // RS_D3D11 path 完全 ignore (UI 也只在 RS_VULKAN 時顯示).
    Q_PROPERTY(bool vkfrucEnableNativeRife MEMBER vkfrucEnableNativeRife NOTIFY vkfrucEnableNativeRifeChanged)
    Q_PROPERTY(int  vkfrucNativeRifeInferDim MEMBER vkfrucNativeRifeInferDim NOTIFY vkfrucNativeRifeInferDimChanged)
    // §J.3.e.2.i.11 (v1.4.66) — auto-tier 偵測欄位。vkfrucDetectedTier 是
    // process 啟動時被 vkfruc.cpp 寫入的偵測結果（heuristic + benchmark），
    // 之後存進 QSettings 讓下一次啟動讀 cache。vkfrucDetectedGpuName 用來
    // 判斷 GPU 是否變更（變了就重跑 benchmark）。vkfrucBenchmarkNs 是
    // v1.4.67 micro-benchmark Conv2D dispatch 的實測 GPU time (ns)，給
    // log + diagnostic 顯示用。三個都 read-only from QML side。
    Q_PROPERTY(VkfrucGpuTier vkfrucDetectedTier MEMBER vkfrucDetectedTier NOTIFY vkfrucDetectedTierChanged)
    Q_PROPERTY(QString vkfrucDetectedGpuName MEMBER vkfrucDetectedGpuName NOTIFY vkfrucDetectedGpuNameChanged)
    Q_PROPERTY(qint64 vkfrucBenchmarkNs MEMBER vkfrucBenchmarkNs NOTIFY vkfrucBenchmarkNsChanged)
    // §J.3.e.2.i.11 (v1.4.69) — auto-tier 啟用開關。default=true：
    // 新使用者 / 升級者啟動 client 後 RIFE on/off + inferDim 都改由
    // vkfrucDetectedTier 自動決定，跳過 vkfrucEnableNativeRife /
    // vkfrucNativeRifeInferDim manual setting.  使用者可在 UI (v1.4.70)
    // 關掉 auto-tier 回 manual 控制.
    Q_PROPERTY(bool vkfrucRifeAutoTier MEMBER vkfrucRifeAutoTier NOTIFY vkfrucRifeAutoTierChanged)
    Q_PROPERTY(DesignVariant designVariant MEMBER designVariant NOTIFY designVariantChanged)
    Q_PROPERTY(AppSortMode appSortMode MEMBER appSortMode NOTIFY appSortModeChanged)
    Q_PROPERTY(QString relayUrl MEMBER relayUrl NOTIFY relayUrlChanged)
    Q_PROPERTY(QString relayPsk MEMBER relayPsk NOTIFY relayPskChanged)
    Q_PROPERTY(bool forceRelayStream MEMBER forceRelayStream NOTIFY forceRelayStreamChanged)
    Q_PROPERTY(bool quitAppAfter MEMBER quitAppAfter NOTIFY quitAppAfterChanged)
    Q_PROPERTY(bool absoluteMouseMode MEMBER absoluteMouseMode NOTIFY absoluteMouseModeChanged)
    Q_PROPERTY(bool absoluteTouchMode MEMBER absoluteTouchMode NOTIFY absoluteTouchModeChanged)
    Q_PROPERTY(bool framePacing MEMBER framePacing NOTIFY framePacingChanged)
    // §Q MP-QUIC multipath transport
    Q_PROPERTY(bool enableMpQuic MEMBER enableMpQuic NOTIFY enableMpQuicChanged)
    Q_PROPERTY(int mpQuicScheduler MEMBER mpQuicScheduler NOTIFY mpQuicSchedulerChanged)
    Q_PROPERTY(bool connectionWarnings MEMBER connectionWarnings NOTIFY connectionWarningsChanged)
    Q_PROPERTY(bool configurationWarnings MEMBER configurationWarnings NOTIFY configurationWarningsChanged)
    Q_PROPERTY(bool richPresence MEMBER richPresence NOTIFY richPresenceChanged)
    Q_PROPERTY(bool gamepadMouse MEMBER gamepadMouse NOTIFY gamepadMouseChanged)
    Q_PROPERTY(bool detectNetworkBlocking MEMBER detectNetworkBlocking NOTIFY detectNetworkBlockingChanged)
    Q_PROPERTY(bool showPerformanceOverlay MEMBER showPerformanceOverlay NOTIFY showPerformanceOverlayChanged)
    Q_PROPERTY(AudioConfig audioConfig MEMBER audioConfig NOTIFY audioConfigChanged)
    Q_PROPERTY(VideoCodecConfig videoCodecConfig MEMBER videoCodecConfig NOTIFY videoCodecConfigChanged)
    Q_PROPERTY(bool enableHdr MEMBER enableHdr NOTIFY enableHdrChanged)
    Q_PROPERTY(bool enableYUV444 MEMBER enableYUV444 NOTIFY enableYUV444Changed)
    Q_PROPERTY(VideoDecoderSelection videoDecoderSelection MEMBER videoDecoderSelection NOTIFY videoDecoderSelectionChanged)
    Q_PROPERTY(WindowMode windowMode MEMBER windowMode NOTIFY windowModeChanged)
    Q_PROPERTY(WindowMode recommendedFullScreenMode MEMBER recommendedFullScreenMode CONSTANT)
    Q_PROPERTY(UIDisplayMode uiDisplayMode MEMBER uiDisplayMode NOTIFY uiDisplayModeChanged)
    Q_PROPERTY(bool swapMouseButtons MEMBER swapMouseButtons NOTIFY mouseButtonsChanged)
    Q_PROPERTY(bool muteOnFocusLoss MEMBER muteOnFocusLoss NOTIFY muteOnFocusLossChanged)
    Q_PROPERTY(bool backgroundGamepad MEMBER backgroundGamepad NOTIFY backgroundGamepadChanged)
    Q_PROPERTY(bool reverseScrollDirection MEMBER reverseScrollDirection NOTIFY reverseScrollDirectionChanged)
    Q_PROPERTY(bool swapFaceButtons MEMBER swapFaceButtons NOTIFY swapFaceButtonsChanged)
    Q_PROPERTY(bool keepAwake MEMBER keepAwake NOTIFY keepAwakeChanged)
    Q_PROPERTY(CaptureSysKeysMode captureSysKeysMode MEMBER captureSysKeysMode NOTIFY captureSysKeysModeChanged)
    Q_PROPERTY(Language language MEMBER language NOTIFY languageChanged);

    Q_INVOKABLE bool retranslate();

    // Directly accessible members for preferences
    int width;
    int height;
    int fps;
    int bitrateKbps;
    bool unlockBitrate;
    bool autoAdjustBitrate;
    bool enableVsync;
    bool gameOptimizations;
    bool playAudioOnHost;
    bool multiController;
    bool enableMdns;
    bool autoWakeOnLan;
    bool enableFrameInterpolation;
    FrucBackend frucBackend;
    RendererSelection rendererSelection;  // §J.3.e.2.i — D3D11 vs Vulkan renderer
    FrucQuality frucQuality;
    bool vkfrucEnableNvOf;     // §B-NVOF — VK_NV_optical_flow 取代 software block-match ME
    bool vkfrucEnableTriple;   // §B2 — TRIPLE 60→180 (兩 interp / server frame)
    bool vkfrucPassiveMode;    // §R2-γ — false=主動補幀, true=被動補幀 (依 recv% 動態 ratio)
    bool vkfrucEnableNativeRife; // §J.3.e.X Path β — native RIFE flow + native warp (beta)
    int  vkfrucNativeRifeInferDim; // 128/256/384/512 — must be /128 aligned for RIFE-v4.25-lite
    // §J.3.e.2.i.11 (v1.4.66) — auto-tier 偵測結果 cache 欄位。
    VkfrucGpuTier vkfrucDetectedTier;
    QString       vkfrucDetectedGpuName;
    qint64        vkfrucBenchmarkNs;
    // §J.3.e.2.i.11 (v1.4.69) — auto-tier 啟用開關 (default true)
    bool          vkfrucRifeAutoTier;
    DesignVariant designVariant;
    AppSortMode appSortMode;
    QString relayUrl;      // VipleStream: signaling relay WebSocket URL
    QString relayPsk;      // VipleStream: relay pre-shared key
    bool forceRelayStream; // VipleStream: always stream via relay (bypass direct /launch)
    bool quitAppAfter;
    bool absoluteMouseMode;
    bool absoluteTouchMode;
    bool framePacing;
    bool connectionWarnings;
    bool configurationWarnings;
    bool richPresence;
    bool gamepadMouse;
    bool detectNetworkBlocking;
    bool enableMpQuic;
    int mpQuicScheduler;
    bool showPerformanceOverlay;
    bool swapMouseButtons;
    bool muteOnFocusLoss;
    bool backgroundGamepad;
    bool reverseScrollDirection;
    bool swapFaceButtons;
    bool keepAwake;
    int packetSize;
    AudioConfig audioConfig;
    VideoCodecConfig videoCodecConfig;
    bool enableHdr;
    bool enableYUV444;
    VideoDecoderSelection videoDecoderSelection;
    WindowMode windowMode;
    WindowMode recommendedFullScreenMode;
    UIDisplayMode uiDisplayMode;
    Language language;
    CaptureSysKeysMode captureSysKeysMode;

signals:
    void displayModeChanged();
    void bitrateChanged();
    void unlockBitrateChanged();
    void autoAdjustBitrateChanged();
    void enableVsyncChanged();
    void gameOptimizationsChanged();
    void playAudioOnHostChanged();
    void multiControllerChanged();
    void unsupportedFpsChanged();
    void enableMdnsChanged();
    void autoWakeOnLanChanged();
    void enableFrameInterpolationChanged();
    void frucBackendChanged();
    void rendererSelectionChanged();
    void frucQualityChanged();
    void vkfrucEnableNvOfChanged();
    void vkfrucEnableTripleChanged();
    void vkfrucPassiveModeChanged();
    void vkfrucEnableNativeRifeChanged();
    void vkfrucNativeRifeInferDimChanged();
    // §J.3.e.2.i.11 (v1.4.66) — auto-tier 偵測欄位 NOTIFY signals。
    void vkfrucDetectedTierChanged();
    void vkfrucDetectedGpuNameChanged();
    void vkfrucBenchmarkNsChanged();
    void vkfrucRifeAutoTierChanged();
    void designVariantChanged();
    void appSortModeChanged();
    void relayUrlChanged();
    void relayPskChanged();
    void forceRelayStreamChanged();
    void quitAppAfterChanged();
    void absoluteMouseModeChanged();
    void absoluteTouchModeChanged();
    void audioConfigChanged();
    void videoCodecConfigChanged();
    void enableHdrChanged();
    void enableYUV444Changed();
    void videoDecoderSelectionChanged();
    void uiDisplayModeChanged();
    void windowModeChanged();
    void framePacingChanged();
    void connectionWarningsChanged();
    void configurationWarningsChanged();
    void richPresenceChanged();
    void gamepadMouseChanged();
    void detectNetworkBlockingChanged();
    void enableMpQuicChanged();
    void mpQuicSchedulerChanged();
    void showPerformanceOverlayChanged();
    void mouseButtonsChanged();
    void muteOnFocusLossChanged();
    void backgroundGamepadChanged();
    void reverseScrollDirectionChanged();
    void swapFaceButtonsChanged();
    void captureSysKeysModeChanged();
    void keepAwakeChanged();
    void languageChanged();

private:
    explicit StreamingPreferences(QQmlEngine *qmlEngine);

    QString getSuffixFromLanguage(Language lang);

    QQmlEngine* m_QmlEngine;
};

