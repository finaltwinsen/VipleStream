#include "commandlineparser.h"
#include "streaming/video/bitstreamdump.h"  // §SF-PROBE：stream --dump-bitstream

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>

#include <Limelight.h>  // §VR：VIPLE_VR_* 範圍常數

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
// Windows uses leading-underscore _exit; POSIX has unprefixed _exit in <unistd.h>.
#  include <process.h>
#  define VS_FAST_EXIT(code) ::_exit(code)
#else
#  include <unistd.h>
#  define VS_FAST_EXIT(code) ::_exit(code)
#endif

static bool inRange(int value, int min, int max)
{
    return value >= min && value <= max;
}

// This method returns key's value from QMap where the key is a QString.
// Key matching is case insensitive.
template <typename T>
static T mapValue(QMap<QString, T> map, QString key)
{
    for(auto& item : map.toStdMap()) {
        if (QString::compare(item.first, key, Qt::CaseInsensitive) == 0) {
            return item.second;
        }
    }
    return T();
}

class CommandLineParser : public QCommandLineParser
{
public:
    enum MessageType {
        Info,
        Error
    };

    void setupCommonOptions()
    {
        setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        addHelpOption();
        addVersionOption();
    }

    void handleHelpAndVersionOptions()
    {
        if (isSet("help")) {
            showInfo(helpText());
        }
        if (isSet("version")) {
            showVersion();
        }
    }

    void handleUnknownOptions()
    {
        if (!unknownOptionNames().isEmpty()) {
            showError(QString("Unknown options: %1").arg(unknownOptionNames().join(", ")));
        }
    }

    void showMessage(QString message, MessageType type) const
    {
        message = message.endsWith('\n') ? message : message + '\n';
        fputs(qPrintable(message), type == Info ? stdout : stderr);
    }

    // VipleStream: --help / --version / parse-error paths bail out from
    // deep inside main() *after* QGuiApplication, SDL_InitSubSystem(VIDEO),
    // atexit(SDL_Quit), and file-scope static QRegularExpressions have all
    // been set up.  Plain exit() runs atexit handlers + static destructors,
    // and on Windows that hits an ordering trap: Qt's static cleanup logs
    // a few messages on the way out, those go through main.cpp's
    // logToLoggerStream() which calls QString::replace(k_RikeyRegex, ...),
    // but k_RikeyRegex's destructor has already fired -> "called on an
    // invalid QRegularExpression object" warning + a 0xC0000409 fail-fast
    // ~10% of the time.  Running back-to-back also occasionally leaves a
    // zombie process stuck inside SDL_Quit (see scripts/build_moonlight_
    // package.cmd line 28's "zombie VipleStream.exe" warning).
    //
    // _exit() skips atexit + static destructors entirely, which is exactly
    // what we want for these short fast-paths -- there's no per-process
    // state worth flushing, and the help/error text was already written
    // via fputs above so we just flush stdio buffers and bail.
    [[ noreturn ]] void fastExit(int code) const
    {
        std::fflush(stdout);
        std::fflush(stderr);
        VS_FAST_EXIT(code);
    }

    [[ noreturn ]] void showInfo(QString message) const
    {
        showMessage(message, Info);
        fastExit(0);
    }

    // VipleStream：QCommandLineParser::showVersion() 不是 virtual，且走一般的 ::exit()——
    // 正是上面那段註解描述的 static destructor / SDL_Quit 競態路徑。1.5.271 實測 `--version`
    // 三次裡兩次卡在 SDL_Quit 不結束、一次 0xC0000409。這裡用同名成員遮蔽它，讓 --version
    // 與 --help 走同一條 fastExit（輸出格式與 Qt 相同：「<name> <version>」）。
    [[ noreturn ]] void showVersion() const
    {
        showInfo(QCoreApplication::applicationName() + " " + QCoreApplication::applicationVersion());
    }

    [[ noreturn ]] void showError(QString message) const
    {
        showMessage(message + "\n\n" + helpText(), Error);
        fastExit(1);
    }

    int getIntOption(QString name) const
    {
        bool ok;
        int intValue = value(name).toInt(&ok);
        if (!ok) {
            showError(QString("Invalid %1 value: %2").arg(name, value(name)));
        }
        return intValue;
    }

    bool getToggleOptionValue(QString name, bool defaultValue) const
    {
        QRegularExpression re(QString("^(%1|no-%1)$").arg(name));
        QStringList options = optionNames().filter(re);
        if (options.isEmpty()) {
            return defaultValue;
        } else {
            return options.last() == name;
        }
    }

    QString getChoiceOptionValue(QString name) const
    {
        if (!m_Choices[name].contains(value(name), Qt::CaseInsensitive)) {
            showError(QString("Invalid %1 choice: %2").arg(name, value(name)));
        }
        return value(name);
    }

    QPair<int,int> getResolutionOptionValue(QString name) const
    {
        static QRegularExpression re("^(\\d+)x(\\d+)$", QRegularExpression::CaseInsensitiveOption);
        auto match = re.match(value(name));
        if (!match.hasMatch()) {
            showError(QString("Invalid %1 format: %2").arg(name, value(name)));
        }
        return qMakePair(match.captured(1).toInt(), match.captured(2).toInt());
    }

    void addFlagOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Use %1.").arg(descriptiveName)));
    }

    void addToggleOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Use %1.").arg(descriptiveName)));
        addOption(QCommandLineOption("no-" + name, QString("Do not use %1.").arg(descriptiveName)));
    }

    void addValueOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Specify %1 to use.").arg(descriptiveName), name));
    }

    void addChoiceOption(QString name, QString descriptiveName, QStringList choices)
    {
        addOption(QCommandLineOption(name, QString("Select %1: %2.").arg(descriptiveName, choices.join('/')), name));
        m_Choices[name] = choices;
    }

private:
    QMap<QString, QStringList> m_Choices;
};

// §SF-PROBE（M2a）：整數選項＋範圍檢查（超出範圍走 showError，結束碼 1）。
static int getBoundedIntOption(const CommandLineParser& parser, const QString& name, int min, int max)
{
    const int value = parser.getIntOption(name);
    if (!inRange(value, min, max)) {
        parser.showError(QString("%1 must be within %2-%3").arg(name).arg(min).arg(max));
    }
    return value;
}

// §SF-PROBE（M2a）：decode-bench 的幀號清單。"3,10,20-24" → 3,10,20,21,22,23,24（排序、去重）。
// 只接受非負整數與 a-b（a<=b）區間；總數上限 100000，防止打錯字展開成巨大清單。
static bool parseFrameList(const QString& text, QList<int>& out)
{
    static const qint64 k_MaxEntries = 100000;
    const QStringList parts = text.split(',', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }
    for (const QString& rawPart : parts) {
        const QString part = rawPart.trimmed();
        const int dash = part.indexOf('-');
        bool ok1 = false;
        bool ok2 = false;
        if (dash > 0) {
            const int first = part.left(dash).toInt(&ok1);
            const int last = part.mid(dash + 1).toInt(&ok2);
            if (!ok1 || !ok2 || first < 0 || last < first ||
                    (qint64)last - first + 1 + out.size() > k_MaxEntries) {
                return false;
            }
            for (qint64 i = first; i <= last; i++) {
                out.append((int)i);
            }
        }
        else {
            const int value = part.toInt(&ok1);
            if (!ok1 || value < 0 || out.size() + 1 > k_MaxEntries) {
                return false;
            }
            out.append(value);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

// §SF-PROBE（M2a）：探測動作只接受「動作名稱」這一個位置參數；多出來的多半是打錯
// （例如把 --device 的值直接寫在後面），直接報錯比默默忽略好。
static void rejectExtraPositionals(const CommandLineParser& parser, int expected)
{
    const QStringList posArgs = parser.positionalArguments();
    if (posArgs.size() > expected) {
        parser.showError(QString("Unexpected argument(s): %1").arg(posArgs.mid(expected).join(' ')));
    }
}

// 已知動作表：parse() 與 earlyProbeAction() 共用，兩者對同一組 argv 一定得出同一個動作。
namespace {
struct ActionEntry {
    const char* name;
    GlobalCommandLineParser::ParseResult result;
    bool earlyProbe;    // §SF-PROBE：main.cpp 在 QGuiApplication 之前以 QCoreApplication 派發
};

const ActionEntry k_Actions[] = {
    {"quit",         GlobalCommandLineParser::QuitRequested,        false},
    {"stream",       GlobalCommandLineParser::StreamRequested,      false},
    {"pair",         GlobalCommandLineParser::PairRequested,        false},
    {"list",         GlobalCommandLineParser::ListRequested,        false},
    {"fruc-offline", GlobalCommandLineParser::FrucOfflineRequested, false},
    {"xr-probe",     GlobalCommandLineParser::XrProbeRequested,     true},
    {"v4l2-probe",   GlobalCommandLineParser::V4l2ProbeRequested,   true},
    {"decode-bench", GlobalCommandLineParser::DecodeBenchRequested, true},
};
}

GlobalCommandLineParser::GlobalCommandLineParser()
{
}

GlobalCommandLineParser::~GlobalCommandLineParser()
{
}

GlobalCommandLineParser::ParseResult GlobalCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Starts Moonlight normally if no arguments are given.\n"
        "\n"
        "Available actions:\n"
        "  list            List the available apps on a host\n"
        "  quit            Quit the currently running app\n"
        "  stream          Start streaming an app\n"
        "  pair            Pair a new host\n"
        "  fruc-offline    Offline FRUC quality measurement (dev-only)\n"
        "  xr-probe        Probe the OpenXR runtime (Steam Frame bring-up)\n"
        "  v4l2-probe      Probe V4L2 stateful video decoders (Steam Frame bring-up)\n"
        "  decode-bench    Benchmark a video decoder on a recorded bitstream (dev)\n"
        "\n"
        "See 'moonlight <action> --help' for help of specific action."
    );
    parser.addPositionalArgument("action", "Action to execute", "<action>");
    parser.parse(args);
    auto posArgs = parser.positionalArguments();

    if (posArgs.isEmpty()) {
        // This method will not return and terminates the process if --version
        // or --help is specified
        parser.handleHelpAndVersionOptions();
        parser.handleUnknownOptions();
        return NormalStartRequested;
    }
    else {
        // If users supply arguments that accept values prior to the "quit"
        // or "stream" positional arguments, we will not be able to correctly
        // parse the value out of the input because this QCommandLineParser
        // doesn't know about all of the options that "quit" and "stream"
        // commands can accept. To work around this issue, we just look
        // for "quit" or "stream" positional arguments anywhere.
        for (int i = 0; i < posArgs.size(); i++) {
            QString action = posArgs.at(i).toLower();
            for (const ActionEntry& entry : k_Actions) {
                if (action == QLatin1String(entry.name)) {
                    return entry.result;
                }
            }
        }

        parser.showError(QString("Invalid action"));
    }
}

const char* GlobalCommandLineParser::earlyProbeAction(int argc, char* argv[])
{
    // 對齊 parse()：QCommandLineParser 把「-」開頭的字當選項（這個階段不認得的選項後面
    // 接的值會被當成位置參數，這裡同樣當成候選）、「--」之後全部是位置參數；位置參數
    // 依序比對已知動作（不分大小寫），第一個命中的決定動作。
    bool afterDoubleDash = false;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (arg == nullptr) {
            continue;
        }
        if (!afterDoubleDash && arg[0] == '-' && arg[1] != '\0') {
            if (strcmp(arg, "--") == 0) {
                afterDoubleDash = true;
            }
            continue;
        }
        for (const ActionEntry& entry : k_Actions) {
            if (qstricmp(arg, entry.name) == 0) {
                return entry.earlyProbe ? entry.name : nullptr;
            }
        }
    }
    return nullptr;
}

QuitCommandLineParser::QuitCommandLineParser()
{
}

QuitCommandLineParser::~QuitCommandLineParser()
{
}

void QuitCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Quit the currently running app on the given host."
    );
    parser.addPositionalArgument("quit", "quit running app");
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that host has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);
}

QString QuitCommandLineParser::getHost() const
{
    return m_Host;
}

PairCommandLineParser::PairCommandLineParser()
{
}

PairCommandLineParser::~PairCommandLineParser()
{
}

void PairCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Pair with the specified host."
    );
    parser.addPositionalArgument("pair", "pair host");
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");
    parser.addValueOption("pin", "4 digit pairing PIN");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that host has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);
    m_PredefinedPin = parser.value("pin");
    if (!m_PredefinedPin.isEmpty() && m_PredefinedPin.length() != 4) {
        parser.showError("PIN must be 4 digits");
    }
}

QString PairCommandLineParser::getHost() const
{
    return m_Host;
}

QString PairCommandLineParser::getPredefinedPin() const
{
    return m_PredefinedPin;
}

StreamCommandLineParser::StreamCommandLineParser()
{
    m_WindowModeMap = {
        {"fullscreen", StreamingPreferences::WM_FULLSCREEN},
        {"windowed",   StreamingPreferences::WM_WINDOWED},
        {"borderless", StreamingPreferences::WM_FULLSCREEN_DESKTOP},
    };
    m_AudioConfigMap = {
        {"stereo",       StreamingPreferences::AC_STEREO},
        {"5.1-surround", StreamingPreferences::AC_51_SURROUND},
        {"7.1-surround", StreamingPreferences::AC_71_SURROUND},
    };
    m_VideoCodecMap = {
        {"auto",  StreamingPreferences::VCC_AUTO},
        {"H.264", StreamingPreferences::VCC_FORCE_H264},
        {"HEVC",  StreamingPreferences::VCC_FORCE_HEVC},
        {"AV1", StreamingPreferences::VCC_FORCE_AV1},
    };
    m_VideoDecoderMap = {
        {"auto",     StreamingPreferences::VDS_AUTO},
        {"software", StreamingPreferences::VDS_FORCE_SOFTWARE},
        {"hardware", StreamingPreferences::VDS_FORCE_HARDWARE},
    };
    m_CaptureSysKeysModeMap = {
        {"never",      StreamingPreferences::CSK_OFF},
        {"fullscreen", StreamingPreferences::CSK_FULLSCREEN},
        {"always",     StreamingPreferences::CSK_ALWAYS},
    };
    m_FrucBackendMap = {
        {"generic",  StreamingPreferences::FB_GENERIC},
        {"nvidia",   StreamingPreferences::FB_NVIDIA_OF},
        {"directml", StreamingPreferences::FB_DIRECTML},
        {"ncnn",     StreamingPreferences::FB_NCNN},
    };
    m_FrucQualityMap = {
        {"quality",     StreamingPreferences::FQ_QUALITY},
        {"balanced",    StreamingPreferences::FQ_BALANCED},
        {"performance", StreamingPreferences::FQ_PERFORMANCE},
    };
    // §J.3.e.2.i — D3D11 vs Vulkan renderer.  CLI override added 2026-05-07
    // for benchmark scripts that need to force RS_VULKAN regardless of saved
    // user setting (Phase 7E NVOF/TRIPLE 只在 Vulkan path 生效).
    m_RendererSelectionMap = {
        {"vulkan", StreamingPreferences::RS_VULKAN},
        {"d3d11",  StreamingPreferences::RS_D3D11},
    };
    // VipleStream §K — MP-QUIC scheduler. Values match QuicTransport.h
    // QUIC_SCHED_* constants (auto=0, min-rtt=1, aggregate=2, redundant=3,
    // ecf=4). Names mirror the GUI dropdown in Settings → Network.
    m_QuicSchedulerMap = {
        {"auto",      0},
        {"min-rtt",   1},
        {"aggregate", 2},
        {"redundant", 3},
        {"ecf",       4},
    };
}

StreamCommandLineParser::~StreamCommandLineParser()
{
}

void StreamCommandLineParser::parse(const QStringList &args, StreamingPreferences *preferences)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Starts directly streaming a given app."
    );
    parser.addPositionalArgument("stream", "Start stream");

    // Add other arguments and options
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");
    parser.addPositionalArgument("app", "App to stream", "\"<app>\"");

    parser.addFlagOption("720",  "1280x720 resolution");
    parser.addFlagOption("1080", "1920x1080 resolution");
    parser.addFlagOption("1440", "2560x1440 resolution");
    parser.addFlagOption("4K", "3840x2160 resolution");
    parser.addValueOption("resolution", "custom <width>x<height> resolution");
    parser.addToggleOption("vsync", "V-Sync");
    parser.addValueOption("fps", "FPS");
    parser.addValueOption("bitrate", "bitrate in Kbps");
    parser.addValueOption("packet-size", "video packet size");
    parser.addChoiceOption("display-mode", "display mode", m_WindowModeMap.keys());
    parser.addChoiceOption("audio-config", "audio config", m_AudioConfigMap.keys());
    parser.addToggleOption("multi-controller", "multiple controller support");
    parser.addToggleOption("quit-after", "quit app after session");
    parser.addToggleOption("absolute-mouse", "remote desktop optimized mouse control");
    parser.addToggleOption("mouse-buttons-swap", "left and right mouse buttons swap");
    parser.addToggleOption("touchscreen-trackpad", "touchscreen in trackpad mode");
    parser.addToggleOption("game-optimization", "game optimizations");
    parser.addToggleOption("audio-on-host", "audio on host PC");
    parser.addToggleOption("frame-pacing", "frame pacing");
    parser.addToggleOption("mute-on-focus-loss", "mute audio when Moonlight window loses focus");
    parser.addToggleOption("background-gamepad", "background gamepad input");
    parser.addToggleOption("reverse-scroll-direction", "inverted scroll direction");
    parser.addToggleOption("swap-gamepad-buttons", "swap A/B and X/Y gamepad buttons (Nintendo-style)");
    parser.addToggleOption("keep-awake", "prevent display sleep while streaming");
    parser.addToggleOption("performance-overlay", "show performance overlay");
    parser.addToggleOption("hdr", "HDR streaming");
    parser.addToggleOption("yuv444", "YUV 4:4:4 sampling, if supported");
    parser.addToggleOption("frame-interpolation", "frame interpolation (FRUC)");
    parser.addChoiceOption("fruc-backend", "FRUC backend", m_FrucBackendMap.keys());
    parser.addChoiceOption("fruc-quality", "FRUC quality preset", m_FrucQualityMap.keys());
    // §B-NVOF / §B2 — Vulkan-only 補幀進階開關 (RS_VULKAN 才生效;
    // RS_D3D11 設了會被忽略).  --vk-nvof / --no-vk-nvof 切換 NVIDIA
    // Optical Flow 硬體 ME (取代 software block-match);
    // --vk-triple / --no-vk-triple 切換 60→180 三倍補幀 mode (需 180Hz panel).
    parser.addToggleOption("vk-nvof", "NVIDIA Optical Flow Vulkan path (Vulkan renderer only)");
    parser.addToggleOption("vk-triple", "TRIPLE 60→180 frame interpolation (Vulkan renderer only, needs 180Hz panel)");
    parser.addChoiceOption("renderer", "renderer selection (vulkan / d3d11)", m_RendererSelectionMap.keys());
    parser.addToggleOption("auto-bitrate", "adaptive bitrate for lossy networks");
    parser.addChoiceOption("capture-system-keys", "capture system key combos", m_CaptureSysKeysModeMap.keys());
    parser.addChoiceOption("video-codec", "video codec", m_VideoCodecMap.keys());
    parser.addChoiceOption("video-decoder", "video decoder", m_VideoDecoderMap.keys());

    // VipleStream §K — MP-QUIC opt-in (capability-gated by server's
    // <VipleStreamMPQUIC>1</...>). Hard-coded congestion control (BBR);
    // expose --quic-cc only if observation reveals BBR underperforming.
    parser.addToggleOption("quic", "MP-QUIC multipath transport");
    parser.addChoiceOption("quic-scheduler", "MP-QUIC scheduler",
                           m_QuicSchedulerMap.keys());

    // VipleStream 2.0 §VR（M1a）— PCVR 協定驗證。這些值只存在這次行程，
    // 不寫入 QSettings（見 StreamingPreferences 的 §VR 欄位註解）。
    parser.addChoiceOption("display-target", "display target", {"window", "xr-desktop", "pcvr"});
    parser.addValueOption("xr-runtime-json", "(dev) OpenXR runtime manifest for --display-target xr-desktop (in-process only)");
    parser.addValueOption("xr-dump-frame", "(dev) save the 300th XR video frame as PNG (longest side <= 1280) for --display-target xr-desktop");
    parser.addValueOption("xr-test-stall-ms", "(dev) drop video frames for N ms starting 20 s after the first XR frame (stale-state test)");
    parser.addValueOption("xr-test-recenter-sec", "(dev) recenter the XR screen once N seconds after XR bring-up");
    parser.addFlagOption("xr-test-pointer", "(dev) synthetic XR pointer ray (4 s circle, trigger click at the top) instead of the controllers");
    parser.addValueOption("xr-test-keyboard", "(dev) open the XR virtual keyboard 5 s after the screen is placed and type this text with a synthetic ray");
    parser.addValueOption("xr-dump-keyboard", "(dev) save the XR virtual keyboard texture as PNG (longest side <= 1280)");
    parser.addChoiceOption("xr-test-fail", "(dev) XR failure injection: bringup (fail before /launch), loss (LOSS_PENDING 15 s in, rebuild succeeds), loss3 (loss, all rebuilds fail -> flat), exit (runtime exit 15 s in -> flat)",
                           {"bringup", "loss", "loss3", "exit"});
    parser.addFlagOption("vr-emulate", "synthetic head/controller pose for PCVR (no XR runtime needed)");
    parser.addChoiceOption("vr-synthetic-motion", "synthetic pose motion for --vr-emulate",
                           {"still", "sine", "yaw30"});
    parser.addValueOption("vr-eye", "per-eye <width>x<height> for PCVR (default 1728x1728)");
    parser.addValueOption("vr-hz", "HMD refresh rate for PCVR (default 90)");
    parser.addValueOption("vr-inject-drop", "(dev) N: drop every Nth decoded frame to test VR frame pairing");
    parser.addValueOption("vr-inject-loss", "(dev) N: inject a VR LOSS report every N seconds");

    // VipleStream 2.0 §SF-PROBE（M2a）dev-only：把 decoder 實際收到的 bitstream 錄成
    // decode-bench 的樣本。值只存在 BitstreamDump 的行程內全域，絕不進 StreamingPreferences
    // （CLI 覆寫會在 session 開始時被 save() 寫回 QSettings，下次一般啟動就會一直錄）。
    parser.addOption(QCommandLineOption("dump-bitstream",
                                        "(dev) Record the bitstream fed to the video decoder to <path> "
                                        "(Annex-B for H.264/HEVC, IVF for AV1) for decode-bench.",
                                        "path"));

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // Resolve display's width and height
    static QRegularExpression resolutionRexExp("^(720|1080|1440|4K|resolution)$");
    QStringList resoOptions = parser.optionNames().filter(resolutionRexExp);
    bool displaySet = !resoOptions.isEmpty();
    if (displaySet) {
        QString name = resoOptions.last();
        if (name == "720") {
            preferences->width  = 1280;
            preferences->height = 720;
        } else if (name == "1080") {
            preferences->width  = 1920;
            preferences->height = 1080;
        } else if (name == "1440") {
            preferences->width  = 2560;
            preferences->height = 1440;
        } else if (name == "4K") {
            preferences->width  = 3840;
            preferences->height = 2160;
        } else if (name == "resolution") {
            auto resolution = parser.getResolutionOptionValue(name);
            preferences->width  = resolution.first;
            preferences->height = resolution.second;
        }
    }

    // Resolve --fps option
    if (parser.isSet("fps")) {
        preferences->fps = parser.getIntOption("fps");
        if (!inRange(preferences->fps, 10, 480)) {
            fprintf(stderr, "Warning: FPS is out of the supported range (10 - 480 FPS). Performance may suffer!\n");
        }
    }

    // Resolve --bitrate option
    if (parser.isSet("bitrate")) {
        preferences->bitrateKbps = parser.getIntOption("bitrate");
        if (!inRange(preferences->bitrateKbps, 500, 500000)) {
            fprintf(stderr, "Warning: Bitrate is out of the supported range (500 - 500000 Kbps). Performance may suffer!\n");
        }
    } else if (displaySet || parser.isSet("fps")) {
        preferences->bitrateKbps = preferences->getDefaultBitrate(
            preferences->width, preferences->height, preferences->fps,
            preferences->enableYUV444, preferences->enableFrameInterpolation);
    }

    // Resolve --packet-size option
    if (parser.isSet("packet-size")) {
        preferences->packetSize = parser.getIntOption("packet-size");
        if (preferences->packetSize < 1024) {
            parser.showError("Packet size must be greater than 1024 bytes");
        }
    }

    // Resolve --display option
    if (parser.isSet("display-mode")) {
        preferences->windowMode = mapValue(m_WindowModeMap, parser.getChoiceOptionValue("display-mode"));
    }

    // Resolve --vsync and --no-vsync options
    preferences->enableVsync = parser.getToggleOptionValue("vsync", preferences->enableVsync);

    // Resolve --audio-config option
    if (parser.isSet("audio-config")) {
        preferences->audioConfig = mapValue(m_AudioConfigMap, parser.getChoiceOptionValue("audio-config"));
    }

    // Resolve --multi-controller and --no-multi-controller options
    preferences->multiController = parser.getToggleOptionValue("multi-controller", preferences->multiController);

    // Resolve --quit-after and --no-quit-after options
    preferences->quitAppAfter = parser.getToggleOptionValue("quit-after", preferences->quitAppAfter);

    // Resolve --absolute-mouse and --no-absolute-mouse options
    preferences->absoluteMouseMode = parser.getToggleOptionValue("absolute-mouse", preferences->absoluteMouseMode);

    // Resolve --mouse-buttons-swap and --no-mouse-buttons-swap options
    preferences->swapMouseButtons = parser.getToggleOptionValue("mouse-buttons-swap", preferences->swapMouseButtons);

    // Resolve --touchscreen-trackpad and --no-touchscreen-trackpad options
    preferences->absoluteTouchMode = !parser.getToggleOptionValue("touchscreen-trackpad", !preferences->absoluteTouchMode);

    // Resolve --game-optimization and --no-game-optimization options
    preferences->gameOptimizations = parser.getToggleOptionValue("game-optimization", preferences->gameOptimizations);

    // Resolve --audio-on-host and --no-audio-on-host options
    preferences->playAudioOnHost = parser.getToggleOptionValue("audio-on-host", preferences->playAudioOnHost);

    // Resolve --frame-pacing and --no-frame-pacing options
    preferences->framePacing = parser.getToggleOptionValue("frame-pacing", preferences->framePacing);

    // Resolve --mute-on-focus-loss and --no-mute-on-focus-loss options
    preferences->muteOnFocusLoss = parser.getToggleOptionValue("mute-on-focus-loss", preferences->muteOnFocusLoss);

    // Resolve --background-gamepad and --no-background-gamepad options
    preferences->backgroundGamepad = parser.getToggleOptionValue("background-gamepad", preferences->backgroundGamepad);

    // Resolve --reverse-scroll-direction and --no-reverse-scroll-direction options
    preferences->reverseScrollDirection = parser.getToggleOptionValue("reverse-scroll-direction", preferences->reverseScrollDirection);

    // Resolve --swap-gamepad-buttons and --no-swap-gamepad-buttons options
    preferences->swapFaceButtons = parser.getToggleOptionValue("swap-gamepad-buttons", preferences->swapFaceButtons);

    // Resolve --keep-awake and --no-keep-awake options
    preferences->keepAwake = parser.getToggleOptionValue("keep-awake", preferences->keepAwake);

    // Resolve --performance-overlay and --no-performance-overlay options
    preferences->showPerformanceOverlay = parser.getToggleOptionValue("performance-overlay", preferences->showPerformanceOverlay);

    // Resolve --hdr and --no-hdr options
    preferences->enableHdr = parser.getToggleOptionValue("hdr", preferences->enableHdr);

    // Resolve --yuv444 and --no-yuv444 options
    preferences->enableYUV444 = parser.getToggleOptionValue("yuv444", preferences->enableYUV444);

    // Resolve --frame-interpolation and --no-frame-interpolation options
    preferences->enableFrameInterpolation = parser.getToggleOptionValue("frame-interpolation", preferences->enableFrameInterpolation);

    // Resolve --fruc-backend option
    if (parser.isSet("fruc-backend")) {
        preferences->frucBackend = mapValue(m_FrucBackendMap, parser.getChoiceOptionValue("fruc-backend"));
    }

    // Resolve --fruc-quality option
    if (parser.isSet("fruc-quality")) {
        preferences->frucQuality = mapValue(m_FrucQualityMap, parser.getChoiceOptionValue("fruc-quality"));
    }

    // §B-NVOF / §B2 — Vulkan-only 補幀進階開關 CLI override (default=既有 prefs).
    preferences->vkfrucEnableNvOf =
        parser.getToggleOptionValue("vk-nvof",   preferences->vkfrucEnableNvOf);
    preferences->vkfrucEnableTriple =
        parser.getToggleOptionValue("vk-triple", preferences->vkfrucEnableTriple);

    // Resolve --renderer option (override saved RS_D3D11 / RS_VULKAN).
    if (parser.isSet("renderer")) {
        preferences->rendererSelection = mapValue(m_RendererSelectionMap,
                                                   parser.getChoiceOptionValue("renderer"));
    }

    // Resolve --auto-bitrate and --no-auto-bitrate options
    preferences->autoAdjustBitrate = parser.getToggleOptionValue("auto-bitrate", preferences->autoAdjustBitrate);

    // Resolve --capture-system-keys option
    if (parser.isSet("capture-system-keys")) {
        preferences->captureSysKeysMode = mapValue(m_CaptureSysKeysModeMap, parser.getChoiceOptionValue("capture-system-keys"));
    }

    // Resolve --video-codec option
    if (parser.isSet("video-codec")) {
        preferences->videoCodecConfig = mapValue(m_VideoCodecMap, parser.getChoiceOptionValue("video-codec"));
    }

    // Resolve --video-decoder option
    if (parser.isSet("video-decoder")) {
        preferences->videoDecoderSelection = mapValue(m_VideoDecoderMap, parser.getChoiceOptionValue("video-decoder"));
    }

    // VipleStream §K — Resolve --quic / --no-quic and --quic-scheduler.
    // session.cpp:2274 also requires m_Computer->isMpQuicCapable (i.e. host
    // advertised <VipleStreamMPQUIC>1) before flipping useQuicTransport in
    // the StreamConfig — so --quic on a vanilla Sunshine host is a no-op.
    preferences->enableMpQuic =
        parser.getToggleOptionValue("quic", preferences->enableMpQuic);
    if (parser.isSet("quic-scheduler")) {
        preferences->mpQuicScheduler =
            mapValue(m_QuicSchedulerMap, parser.getChoiceOptionValue("quic-scheduler"));
    }

    // VipleStream 2.0 §VR（M1a）
    if (parser.isSet("display-target")) {
        const QString dt = parser.getChoiceOptionValue("display-target");
        preferences->displayTarget =
            dt.compare("pcvr", Qt::CaseInsensitive) == 0 ? StreamingPreferences::DT_PCVR
            : dt.compare("xr-desktop", Qt::CaseInsensitive) == 0 ? StreamingPreferences::DT_XR_DESKTOP
                                                                  : StreamingPreferences::DT_WINDOW;
    }
    if (parser.isSet("xr-runtime-json")) {
        const QString path = parser.value("xr-runtime-json");
        if (path.isEmpty()) {
            parser.showError("xr-runtime-json requires a file path");
        }
        preferences->xrRuntimeJsonPath = QFileInfo(path).absoluteFilePath();
    }
    if (parser.isSet("xr-dump-frame")) {
        const QString path = parser.value("xr-dump-frame");
        if (path.isEmpty()) {
            parser.showError("xr-dump-frame requires a file path");
        }
        preferences->xrDumpFramePath = QFileInfo(path).absoluteFilePath();
    }
    if (parser.isSet("xr-test-stall-ms")) {
        bool ok = false;
        const int v = parser.value("xr-test-stall-ms").toInt(&ok);
        if (!ok || v < 0 || v > 600000) {
            parser.showError("xr-test-stall-ms must be 0..600000");
        }
        preferences->xrTestStallMs = v;
    }
    if (parser.isSet("xr-test-recenter-sec")) {
        bool ok = false;
        const int v = parser.value("xr-test-recenter-sec").toInt(&ok);
        if (!ok || v < 0 || v > 3600) {
            parser.showError("xr-test-recenter-sec must be 0..3600");
        }
        preferences->xrTestRecenterSec = v;
    }
    if (parser.isSet("xr-test-keyboard")) {
        preferences->xrTestKeyboard = parser.value("xr-test-keyboard");
    }
    if (parser.isSet("xr-dump-keyboard")) {
        const QString path = parser.value("xr-dump-keyboard");
        if (path.isEmpty()) {
            parser.showError("xr-dump-keyboard requires a file path");
        }
        preferences->xrDumpKeyboardPath = QFileInfo(path).absoluteFilePath();
    }
    if (parser.isSet("xr-test-pointer")) {
        preferences->xrTestPointer = true;
    }
    if (parser.isSet("xr-test-fail")) {
        preferences->xrTestFail = parser.getChoiceOptionValue("xr-test-fail").toLower();
    }
#ifndef HAVE_OPENXR
    if (preferences->displayTarget == StreamingPreferences::DT_XR_DESKTOP) {
        fprintf(stderr, "Warning: --display-target xr-desktop needs an OpenXR build (CONFIG+=openxr); "
                        "the stream will use a flat window.\n");
    }
#endif
    preferences->vrEmulate = parser.isSet("vr-emulate");
    if (parser.isSet("vr-synthetic-motion")) {
        const QString motion = parser.getChoiceOptionValue("vr-synthetic-motion").toLower();
        preferences->vrSyntheticMotion = motion == "still" ? 0 : (motion == "yaw30" ? 2 : 1);
    }
    if (parser.isSet("vr-eye")) {
        auto eye = parser.getResolutionOptionValue("vr-eye");
        if (!inRange(eye.first, VIPLE_VR_EYE_DIM_MIN, VIPLE_VR_EYE_DIM_MAX) ||
                !inRange(eye.second, VIPLE_VR_EYE_DIM_MIN, VIPLE_VR_EYE_DIM_MAX)) {
            parser.showError(QString("vr-eye must be within %1-%2 per dimension")
                                 .arg(VIPLE_VR_EYE_DIM_MIN).arg(VIPLE_VR_EYE_DIM_MAX));
        }
        preferences->vrEyeWidth = eye.first;
        preferences->vrEyeHeight = eye.second;
    }
    if (parser.isSet("vr-hz")) {
        preferences->vrRefreshHz = parser.getIntOption("vr-hz");
        if (!inRange(preferences->vrRefreshHz, VIPLE_VR_HZ_MIN, VIPLE_VR_HZ_MAX)) {
            parser.showError(QString("vr-hz must be within %1-%2").arg(VIPLE_VR_HZ_MIN).arg(VIPLE_VR_HZ_MAX));
        }
    }
    if (parser.isSet("vr-inject-drop")) {
        preferences->vrInjectDropEvery = parser.getIntOption("vr-inject-drop");
    }
    if (parser.isSet("vr-inject-loss")) {
        preferences->vrInjectLossSec = parser.getIntOption("vr-inject-loss");
    }
    if (preferences->displayTarget == StreamingPreferences::DT_PCVR && !preferences->vrEmulate) {
        fprintf(stderr, "Warning: --display-target pcvr needs --vr-emulate in this build (no XR runtime yet); "
                        "the stream will fall back to a flat window.\n");
    }

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that both host and app has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);

    if (posArgs.length() < 3) {
        parser.showError("App not provided");
    }
    m_AppName = parser.positionalArguments().at(2);

    // §SF-PROBE（M2a）：參數都驗過、確定要串流才開啟 dump（session 開始前）。
    if (parser.isSet("dump-bitstream")) {
        const QString dumpPath = parser.value("dump-bitstream");
        if (dumpPath.isEmpty()) {
            parser.showError("dump-bitstream requires a file path");
        }
        BitstreamDump::setPath(QFileInfo(dumpPath).absoluteFilePath());
    }
}

QString StreamCommandLineParser::getHost() const
{
    return m_Host;
}

QString StreamCommandLineParser::getAppName() const
{
    return m_AppName;
}

ListCommandLineParser::ListCommandLineParser()
{
}

ListCommandLineParser::~ListCommandLineParser()
{
}

void ListCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "List the available apps on the given host."
    );
    parser.addPositionalArgument("list", "list available apps");
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");

    parser.addFlagOption("csv",     "Print as CSV with additional information");
    parser.addFlagOption("verbose", "Displays additional information");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();


    m_PrintCSV = parser.isSet("csv");
    m_Verbose = parser.isSet("verbose");

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that host has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);
}

QString ListCommandLineParser::getHost() const
{
    return m_Host;
}

bool ListCommandLineParser::isPrintCSV() const
{
    return m_PrintCSV;
}

bool ListCommandLineParser::isVerbose() const
{
    return m_Verbose;
}

// VipleStream §FRUC-VALIDATE — 離線 FRUC 品質量測（dev-only）。
FrucOfflineCommandLineParser::FrucOfflineCommandLineParser()
{
}

FrucOfflineCommandLineParser::~FrucOfflineCommandLineParser()
{
}

void FrucOfflineCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Offline FRUC quality measurement (dev-only).\n"
        "Feeds a PNG sequence through the real VkFrucRenderer (software upload +\n"
        "FRUC + frame dump) and writes real/interp BMPs to <dump_dir> for\n"
        "comparison against ground-truth frames by fruc_metrics.py."
    );
    parser.addPositionalArgument("fruc-offline", "offline FRUC measurement");
    parser.addPositionalArgument("input_dir", "directory of input PNG frames", "<input_dir>");
    parser.addPositionalArgument("dump_dir", "output directory for real/interp dumps", "<dump_dir>");
    parser.addPositionalArgument("width", "frame width in pixels", "<width>");
    parser.addPositionalArgument("height", "frame height in pixels", "<height>");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 5) {
        parser.showError("Usage: fruc-offline <input_dir> <dump_dir> <width> <height>");
    }
    m_InputDir = posArgs.at(1);
    m_DumpDir  = posArgs.at(2);
    m_Width    = posArgs.at(3).toInt();
    m_Height   = posArgs.at(4).toInt();

    if (m_Width <= 0 || m_Height <= 0) {
        parser.showError("Invalid width/height (must be positive integers)");
    }
}

QString FrucOfflineCommandLineParser::getInputDir() const { return m_InputDir; }
QString FrucOfflineCommandLineParser::getDumpDir() const { return m_DumpDir; }
int FrucOfflineCommandLineParser::getWidth() const { return m_Width; }
int FrucOfflineCommandLineParser::getHeight() const { return m_Height; }

// §SF-PROBE（M2a）：--json 的共用處理（相對路徑以目前目錄解析成絕對路徑，結尾印的就是實際位置）。
static QString getJsonPathOption(const CommandLineParser& parser)
{
    if (!parser.isSet("json")) {
        return QString();
    }
    const QString path = parser.value("json");
    if (path.isEmpty()) {
        parser.showError("json requires a file path");
    }
    return QFileInfo(path).absoluteFilePath();
}

// VipleStream 2.0 §SF-PROBE（M2a）— xr-probe
void XrProbeCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Probe the OpenXR runtime (Steam Frame bring-up).\n"
        "Loads the OpenXR loader, creates an XrInstance, queries extensions, the HMD\n"
        "system, the stereo view configuration and the Vulkan requirements, then\n"
        "destroys the instance. With --session it then creates a Vulkan device and an\n"
        "XrSession, runs the frame loop for --duration seconds (a gray loading quad)\n"
        "and reports the highest session state, frame count, missed frames, refresh\n"
        "rate, per-eye FOV and reference spaces. A summary goes to stdout; the full\n"
        "result is written as JSON (its path is printed on stderr).\n"
        "\n"
        "Exit codes: 0 runtime loaded (and, with --session, frames were submitted),\n"
        "1 command line error, 10 not built with OpenXR, 13 no usable runtime,\n"
        "14 runtime error (session failed), 15 JSON write failed."
    );
    parser.addPositionalArgument("xr-probe", "probe the OpenXR runtime");
    parser.addOption(QCommandLineOption("session",
                                        "Also create an XrSession and run the frame loop (session state, refresh rate, FOV, reference spaces)."));
    parser.addOption(QCommandLineOption("duration",
                                        "Seconds to run the frame loop with --session (default 10, 1-600).",
                                        "seconds"));
    parser.addOption(QCommandLineOption("xr-runtime-json",
                                        "(dev) Load the runtime from this manifest (sets XR_RUNTIME_JSON inside this process only).",
                                        "path"));
    parser.addOption(QCommandLineOption("loader-debug",
                                        "(dev) Print OpenXR loader diagnostics to stderr (XR_LOADER_DEBUG=all inside this process only)."));
    parser.addOption(QCommandLineOption("selftest-ray",
                                        "Only run the XR pointer math self-test (ray vs quad/cylinder, UV round trip, One-Euro); "
                                        "no runtime needed. Exit 0 all pass, 14 on failure."));
    parser.addOption(QCommandLineOption("json",
                                        "Write the JSON report to <path> (default: probe-xr-probe-<ms>-<pid>.json in the log directory).",
                                        "path"));

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // 指定 --version 或 --help 時，這個呼叫會直接結束行程、不會返回
    parser.handleHelpAndVersionOptions();

    rejectExtraPositionals(parser, 1);

    m_Options.session = parser.isSet("session");
    if (parser.isSet("duration")) {
        bool ok = false;
        const int d = parser.value("duration").toInt(&ok);
        if (!ok || d < 1 || d > 600) {
            parser.showError("duration must be an integer between 1 and 600");
        }
        m_Options.durationSec = d;
    }
    m_Options.loaderDebug = parser.isSet("loader-debug");
    m_Options.selftestRay = parser.isSet("selftest-ray");
    if (parser.isSet("xr-runtime-json")) {
        const QString path = parser.value("xr-runtime-json");
        if (path.isEmpty()) {
            parser.showError("xr-runtime-json requires a file path");
        }
        m_Options.runtimeJson = QFileInfo(path).absoluteFilePath();
    }
    m_Options.jsonPath = getJsonPathOption(parser);
}

const XrProbeOptions& XrProbeCommandLineParser::options() const
{
    return m_Options;
}

// VipleStream 2.0 §SF-PROBE（M2a）— v4l2-probe
void V4l2ProbeCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Probe V4L2 stateful (memory-to-memory) video decoders (Steam Frame bring-up).\n"
        "Lists /dev/video* and, for every m2m decoder, its formats, frame sizes,\n"
        "profile/level controls and minimum buffer counts. --header-test submits a\n"
        "720p test frame, reads the capture format after SOURCE_CHANGE and waits for\n"
        "the first decoded frame (this briefly uses the hardware decoder). A summary\n"
        "goes to stdout; the full result is written as JSON (its path is printed on\n"
        "stderr).\n"
        "\n"
        "Exit codes: 0 at least one m2m decoder, 1 command line error, 10 not a Linux\n"
        "build, 13 no m2m decoder visible, 14 runtime error, 15 JSON write failed."
    );
    parser.addPositionalArgument("v4l2-probe", "probe V4L2 decoders");
    parser.addOption(QCommandLineOption("device",
                                        "Probe only this device node, e.g. /dev/video0 (default: every /dev/video*).",
                                        "path"));
    parser.addChoiceOption("header-test", "codec(s) for the header test", {"h264", "hevc", "all"});
    parser.addOption(QCommandLineOption("expbuf",
                                        "During --header-test, also test VIDIOC_EXPBUF on the capture buffers."));
    parser.addOption(QCommandLineOption("json",
                                        "Write the JSON report to <path> (default: probe-v4l2-probe-<ms>-<pid>.json in the log directory).",
                                        "path"));

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // 指定 --version 或 --help 時，這個呼叫會直接結束行程、不會返回
    parser.handleHelpAndVersionOptions();

    rejectExtraPositionals(parser, 1);

    if (parser.isSet("device")) {
        m_Options.device = parser.value("device");
        if (m_Options.device.isEmpty()) {
            parser.showError("device requires a device node path");
        }
    }
    if (parser.isSet("header-test")) {
        m_Options.headerTest = parser.getChoiceOptionValue("header-test").toLower();
    }
    m_Options.expbuf = parser.isSet("expbuf");
    if (m_Options.expbuf && m_Options.headerTest.isEmpty()) {
        parser.showError("expbuf only applies together with --header-test");
    }
    m_Options.jsonPath = getJsonPathOption(parser);
}

const V4l2ProbeOptions& V4l2ProbeCommandLineParser::options() const
{
    return m_Options;
}

// VipleStream 2.0 §SF-PROBE（M2a）— decode-bench
void DecodeBenchCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Benchmark a video decoder on a recorded bitstream (dev; Steam Frame PoC-3/3b/3c/4).\n"
        "The decoder is configured exactly like a streaming session. Record samples with\n"
        "\"stream --dump-bitstream <path>\". A summary goes to stdout; the full result is\n"
        "written as JSON (its path is printed on stderr).\n"
        "\n"
        "Exit codes: 0 done, 1 command line error, 10 not built with FFmpeg,\n"
        "11 input file unreadable or malformed, 13 the requested decoder cannot be\n"
        "opened, 14 decoding failed, 15 JSON write failed."
    );
    parser.addPositionalArgument("decode-bench", "benchmark a video decoder");
    parser.addPositionalArgument("file",
                                 "Annex-B H.264/HEVC (.h264 .264 .h265 .265 .hevc) or AV1 IVF (.ivf) bitstream",
                                 "<file>");
    // h265 是 hevc 的別名（decode-bench 內部會正規化）
    parser.addChoiceOption("codec", "bitstream codec (default auto = from the file extension)",
                           {"auto", "h264", "hevc", "h265", "av1"});
    parser.addOption(QCommandLineOption("decoder",
                                        "Decoder: auto (default), sw, an FFmpeg decoder name such as hevc_v4l2m2m, "
                                        "or hwaccel:<type> such as hwaccel:vaapi.",
                                        "decoder"));
    parser.addChoiceOption("out", "output frames (default drm_prime; sw = system memory)",
                           {"drm_prime", "sw"});
    parser.addOption(QCommandLineOption("fps",
                                        "Submit input at N frames per second like a stream (default 0: as fast as possible).",
                                        "N"));
    parser.addOption(QCommandLineOption("frames",
                                        "Decode at most N frames per pass (default 0: the whole file).",
                                        "N"));
    parser.addOption(QCommandLineOption("loop", "Decode the file N times (default 1).", "N"));
    parser.addOption(QCommandLineOption("warmup",
                                        "Leave the first N frames out of the statistics (default 0).",
                                        "N"));
    parser.addOption(QCommandLineOption("capture-buffers",
                                        "v4l2m2m capture buffer count (default: same as a streaming session).",
                                        "N"));
    parser.addOption(QCommandLineOption("output-buffers",
                                        "v4l2m2m output buffer count (default: same as a streaming session).",
                                        "N"));
    parser.addOption(QCommandLineOption("hold",
                                        "Keep every decoded frame referenced for N more frames (renderer/GPU hold).",
                                        "N"));
    // 單數寫法（--drop-frame／--corrupt-frame）也收：decode-bench 的交接說明與文件草稿用的是單數。
    // 幀號是檔內的第 k 幀（0 起算），--loop 時每一輪都套用（decodebench.cpp）。
    parser.addOption(QCommandLineOption(QStringList{QStringLiteral("drop-frames"), QStringLiteral("drop-frame")},
                                        "Do not submit these frame numbers (numbered within the file, applied on every loop), "
                                        "e.g. 100,200-204 (recovery test).",
                                        "list"));
    parser.addOption(QCommandLineOption("drop-every", "Drop every Nth frame (recovery test).", "N"));
    parser.addOption(QCommandLineOption(QStringList{QStringLiteral("corrupt-frames"), QStringLiteral("corrupt-frame")},
                                        "Flip bytes in the middle of these frames before submitting them, e.g. 100,300.",
                                        "list"));
    parser.addOption(QCommandLineOption("flush-on-error", "Call avcodec_flush_buffers() after a decode error."));
    parser.addOption(QCommandLineOption("compare-sw",
                                        "Also decode the file with the software decoder and report per-frame luma PSNR."));
    parser.addOption(QCommandLineOption("map-vulkan",
                                        "Map every frame into Vulkan through libplacebo and time it (libplacebo builds only)."));
    parser.addOption(QCommandLineOption("verbose", "Raise FFmpeg logging to debug."));
    parser.addOption(QCommandLineOption("json",
                                        "Write the JSON report to <path> (default: probe-decode-bench-<ms>-<pid>.json in the log directory).",
                                        "path"));

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // 指定 --version 或 --help 時，這個呼叫會直接結束行程、不會返回
    parser.handleHelpAndVersionOptions();

    const QStringList posArgs = parser.positionalArguments();
    if (posArgs.size() < 2) {
        parser.showError("Input file not provided");
    }
    rejectExtraPositionals(parser, 2);
    m_Options.file = QFileInfo(posArgs.at(1)).absoluteFilePath();

    if (parser.isSet("codec")) {
        m_Options.codec = parser.getChoiceOptionValue("codec").toLower();
    }
    if (parser.isSet("decoder")) {
        m_Options.decoder = parser.value("decoder").trimmed();
        if (m_Options.decoder.isEmpty()) {
            parser.showError("decoder requires a value");
        }
    }
    if (parser.isSet("out")) {
        m_Options.out = parser.getChoiceOptionValue("out").toLower();
    }
    if (parser.isSet("fps")) {
        m_Options.fps = getBoundedIntOption(parser, "fps", 0, 1000);
    }
    if (parser.isSet("frames")) {
        m_Options.frames = getBoundedIntOption(parser, "frames", 0, 100000000);
    }
    if (parser.isSet("loop")) {
        m_Options.loop = getBoundedIntOption(parser, "loop", 1, 100000);
    }
    if (parser.isSet("warmup")) {
        m_Options.warmup = getBoundedIntOption(parser, "warmup", 0, 100000000);
    }
    // FFmpeg v4l2m2m 的 num_capture_buffers／num_output_buffers 下限都是 2。
    if (parser.isSet("capture-buffers")) {
        m_Options.captureBuffers = getBoundedIntOption(parser, "capture-buffers", 2, 256);
    }
    if (parser.isSet("output-buffers")) {
        m_Options.outputBuffers = getBoundedIntOption(parser, "output-buffers", 2, 256);
    }
    if (parser.isSet("hold")) {
        m_Options.hold = getBoundedIntOption(parser, "hold", 0, 64);
    }
    if (parser.isSet("drop-frames") && !parseFrameList(parser.value("drop-frames"), m_Options.dropFrames)) {
        parser.showError(QString("Invalid drop-frames list: %1 (expected e.g. 100,200-204)").arg(parser.value("drop-frames")));
    }
    if (parser.isSet("drop-every")) {
        m_Options.dropEvery = getBoundedIntOption(parser, "drop-every", 0, 100000000);
        if (m_Options.dropEvery == 1) {
            parser.showError("drop-every must be 0 (off) or at least 2");
        }
    }
    if (parser.isSet("corrupt-frames") && !parseFrameList(parser.value("corrupt-frames"), m_Options.corruptFrames)) {
        parser.showError(QString("Invalid corrupt-frames list: %1 (expected e.g. 100,300)").arg(parser.value("corrupt-frames")));
    }
    m_Options.flushOnError = parser.isSet("flush-on-error");
    m_Options.compareSw = parser.isSet("compare-sw");
    m_Options.mapVulkan = parser.isSet("map-vulkan");
    m_Options.verbose = parser.isSet("verbose");
    m_Options.jsonPath = getJsonPathOption(parser);
}

const DecodeBenchOptions& DecodeBenchCommandLineParser::options() const
{
    return m_Options;
}
