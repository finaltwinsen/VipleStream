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
    };
    Q_ENUM(FrucBackend);

    enum FrucQuality
    {
        FQ_QUALITY,      // Best visual quality, higher GPU load
        FQ_BALANCED,     // Recommended: quality/performance balance
        FQ_PERFORMANCE,  // Lowest latency, suitable for iGPU
    };
    Q_ENUM(FrucQuality);

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
    Q_PROPERTY(FrucQuality frucQuality MEMBER frucQuality NOTIFY frucQualityChanged)
    Q_PROPERTY(DesignVariant designVariant MEMBER designVariant NOTIFY designVariantChanged)
    Q_PROPERTY(AppSortMode appSortMode MEMBER appSortMode NOTIFY appSortModeChanged)
    Q_PROPERTY(QString relayUrl MEMBER relayUrl NOTIFY relayUrlChanged)
    Q_PROPERTY(QString relayPsk MEMBER relayPsk NOTIFY relayPskChanged)
    Q_PROPERTY(bool forceRelayStream MEMBER forceRelayStream NOTIFY forceRelayStreamChanged)
    Q_PROPERTY(bool quitAppAfter MEMBER quitAppAfter NOTIFY quitAppAfterChanged)
    Q_PROPERTY(bool absoluteMouseMode MEMBER absoluteMouseMode NOTIFY absoluteMouseModeChanged)
    Q_PROPERTY(bool absoluteTouchMode MEMBER absoluteTouchMode NOTIFY absoluteTouchModeChanged)
    Q_PROPERTY(bool framePacing MEMBER framePacing NOTIFY framePacingChanged)
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
    FrucQuality frucQuality;
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
    void frucQualityChanged();
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

