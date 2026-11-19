#include "vrlauncher.h"
#include "streaming/vr/vrnetbench.h"  // §VR-NETBENCH
#include "streaming/framehotspot.h"   // §FRAME-HOTSPOT-PAUSE
#include "streaming/session.h"        // §FRAME-HOTSPOT-EARLY：readVrHotspotCreds

#include "sfenv.h"
#include "vrpreflightcheck.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QFile>
#include <QMutex>
#include <QProcessEnvironment>

#include <atomic>
#include <memory>
#include <chrono>
#include <climits>

#include <cstdio>

#if defined(Q_OS_LINUX)
#include <csignal>
#include <sys/prctl.h>
#endif

#include "SDL_compat.h"

namespace {

#if defined(Q_OS_LINUX) && defined(HAVE_OPENXR)
// Flatpak 的 bwrap 預設會開新的 PID namespace（沙盒裡 PID 1 是 bwrap）。這時 OpenXR 回報給 Frame
// SteamVR 的是沙盒內的 PID，SteamVR 對不上（2026-09-30 實測：VRInitError_Init_Internal、SteamVR 重啟）。
// 開發環境以不開 PID namespace 的 bwrap 包裝繞過（Steam 捷徑的 wrapper 會設），那時 PID 1 是 systemd。
bool inPidNamespacedSandbox()
{
    if (!QFile::exists(QStringLiteral("/.flatpak-info"))) {
        return false;
    }
    QFile comm(QStringLiteral("/proc/1/comm"));
    if (!comm.open(QIODevice::ReadOnly)) {
        return false;
    }
    return comm.readAll().trimmed() == "bwrap";
}
#endif

// 子行程（main.cpp 的 CLI stream 路徑）印到 stderr、給使用者看的錯誤行
QString userFacingError(const QString& line)
{
    static const char* const k_Prefixes[] = {"Stream failed: ", "Stream error: ", "Play error: ", "Error: "};
    for (const char* p : k_Prefixes) {
        const QString prefix = QString::fromLatin1(p);
        if (line.startsWith(prefix)) {
            return line.mid(prefix.size()).trimmed();
        }
    }
    return QString();
}

// §REVIEW-1119：app 名稱、錯誤與子行程狀態會進日誌。控制字元會把一列拆開，看起來像另一條系統日誌。
QString logSafe(const QString& text)
{
    QString cleaned;
    const int n = qMin(text.size(), 200);
    cleaned.reserve(n);
    for (int i = 0; i < n; ++i) {
        const ushort u = text.at(i).unicode();
        cleaned.append((u < 0x20 || u == 0x7F) ? QLatin1Char(' ') : text.at(i));
    }
    return cleaned;
}

// §REVIEW-1119：開熱點的背景工作可能超過 10 秒。結束流程先設旗標，再跟「要不要丟回主執行緒」互斥。
std::atomic<bool> s_HotspotClosing{false};
QMutex s_HotspotDecisionMu;

std::function<void(VrLauncher::ChildEvent)> s_ChildHook;  // §VR-SHELL
std::function<bool(const VrLauncher::PlayRequest&)> s_PlayerHandler;  // §VR-SHELL（A2）
VrLauncher* s_Instance = nullptr;  // QML singleton（playerFinished 用）
bool s_ShellFocused = true;        // §PERF-1113：見 VrLauncher::setShellFocused
bool s_LanVrHostSeen = false;      // §FRAME-HOTSPOT-EARLY：VrLauncher 建立之前就看到區網上的 host 了

}  // namespace

void VrLauncher::setChildHook(std::function<void(ChildEvent)> hook)
{
    s_ChildHook = std::move(hook);
    if (s_Instance != nullptr) {
        emit s_Instance->shellActiveChanged();
    }
}

// §FRAME-HOTSPOT-EARLY：見 framehotspot.h。開與關都要等 softapmanager 回覆，放到執行緒池；等裝置連上的輪詢在介面執行緒
void VrLauncher::lanVrHostOnline()
{
#if defined(Q_OS_LINUX)
    static bool s_Tried = false;  // 這個行程只試一次（等不到就交給場次開始時的那條路徑）
    if (s_Tried) {
        return;
    }
    if (s_Instance == nullptr) {
        // 沒有 VrLauncher＝沒有人更新標記檔、結束時也沒有人關：先記著，它建立時再開
        s_LanVrHostSeen = true;
        return;
    }
    s_Tried = true;
    QPointer<VrLauncher> self(s_Instance);
    QThreadPool::globalInstance()->start([self]() {
        std::string ssid, key;
        if (!Session::readVrHotspotCreds(&ssid, &key) || !FrameHotspot::openAtAppStart(ssid, key)) {
            return;
        }
        // §REVIEW-1119：openAtAppStart 返回時程式可能已在結束。旗標在解構與 aboutToQuit 先設。
        // 鎖只包這次決定，避免看完旗標之後 qApp 才被解構、invokeMethod 用到已釋放的 qApp。
        // 熱點是這條工作開的、主執行緒又不會再排等待計時器，就在這裡關掉。
        QMutexLocker decision(&s_HotspotDecisionMu);
        if (s_HotspotClosing.load(std::memory_order_acquire) || QCoreApplication::closingDown() ||
                QCoreApplication::instance() == nullptr) {
            FrameHotspot::closeIfOwner();
            return;
        }
        QMetaObject::invokeMethod(qApp, [self]() {
            if (self.isNull() || s_HotspotClosing.load(std::memory_order_acquire)) {
                FrameHotspot::closeIfOwner();
                return;
            }
            auto* wait = new QTimer(self.data());
            wait->setInterval(5000);
            auto waited = std::make_shared<int>(0);
            connect(wait, &QTimer::timeout, self.data(), [wait, waited]() {
                *waited += 5;
                const FrameHotspot::AppWait st = FrameHotspot::appStartState();
                if (st == FrameHotspot::AppWait::Waiting && *waited < FrameHotspot::kAppJoinWaitS &&
                        !s_HotspotClosing.load(std::memory_order_acquire)) {
                    return;
                }
                wait->stop();
                wait->deleteLater();
                if (s_HotspotClosing.load(std::memory_order_acquire)) {
                    return;
                }
                if (st == FrameHotspot::AppWait::Joined) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-HOTSPOT] app: a device joined the hotspot after about %d s - keeping it on", *waited);
                }
                else if (st == FrameHotspot::AppWait::Waiting) {
                    QThreadPool::globalInstance()->start([]() {
                        // §REVIEW-1119：和 aboutToQuit 的 closeIfOwner 搶同一顆 socket。先看旗標，結束流程優先。
                        QMutexLocker decision(&s_HotspotDecisionMu);
                        if (s_HotspotClosing.load(std::memory_order_acquire)) {
                            return;
                        }
                        FrameHotspot::closeAppStart();
                    });
                }
            });
            wait->start();
        }, Qt::QueuedConnection);
    });
#endif
}

void VrLauncher::setPlayerHandler(std::function<bool(const PlayRequest&)> handler)
{
    s_PlayerHandler = std::move(handler);
}

void VrLauncher::playerFinished(int exitCode, const QString& error)
{
    if (s_Instance != nullptr) {
        s_Instance->inProcessPlayerEnded(exitCode, error);
    }
}

void VrLauncher::inProcessPlayerEnded(int exitCode, const QString& error)
{
    if (!m_InProcessPlayer) {
        return;
    }
    m_InProcessPlayer = false;
    m_LastError = error;
    const QString shown = playerError(exitCode, false);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] video player (in the VR interface) finished exit=%d%s%s", exitCode,
                shown.isEmpty() ? "" : " error=", qPrintable(logSafe(shown)));
    emit runningChanged();
    emit finished(exitCode, shown);
}

VrLauncher::VrLauncher(QObject* parent)
    : QObject(parent),
      m_Preflight(new VrPreflightCheck(this))
{
    s_Instance = this;
    // §FRAME-HOTSPOT-PAUSE：上一次的串流行程暫停了頭盔的串流轉接器熱點、沒有還原就結束（被強制結束、當掉）時，在這裡補做。
    // 沒有那個標記檔時只是一次開檔失敗；Frame 以外的平台什麼都不做
    FrameHotspot::recoverStale();
    // §FRAME-HOTSPOT-AUTO：區網場次自動開的熱點記在這個行程（App）名下，連著玩幾個遊戲時留著。這裡負責兩件事：
    // 定期更新標記檔的時間（沒有人更新＝App 不在了，別人會補關），以及 App 正常結束時關掉
    {
        auto* hotspotBeat = new QTimer(this);
        hotspotBeat->setInterval(15000);
        connect(hotspotBeat, &QTimer::timeout, this, []() { FrameHotspot::heartbeat(); });
        hotspotBeat->start();
        connect(qApp, &QCoreApplication::aboutToQuit, this, []() {
            // §REVIEW-1119：先擋住背景工作再丟回 qApp，再關熱點。closeIfOwner 與那個決定共用一把鎖。
            s_HotspotClosing.store(true, std::memory_order_release);
            QMutexLocker decision(&s_HotspotDecisionMu);
            FrameHotspot::closeIfOwner();
        });
        // §FRAME-HOTSPOT-EARLY
        if (s_LanVrHostSeen) {
            QTimer::singleShot(0, this, []() { lanVrHostOnline(); });
        }
    }
    // §VR-PREFLIGHT：檢查的結果。proceed＝可以開了（夠快、量不到、連續夠快、使用者堅持）
    connect(m_Preflight, &VrPreflightCheck::changed, this, &VrLauncher::preflightChanged);
    connect(m_Preflight, &VrPreflightCheck::proceed, this, &VrLauncher::preflightPassed);
    connect(m_Preflight, &VrPreflightCheck::cancelled, this, [this]() {
        m_PendingAddress.clear();
        m_PendingApp.clear();
        m_PendingTakeover = false;
        setLaunching(false, QString(), "preflight cancelled");
    });
}

void VrLauncher::setLaunching(bool on, const QString& app, const char* why)
{
    if (on == m_Launching && app == m_LaunchingApp) {
        return;
    }
    m_Launching = on;
    m_LaunchingApp = app;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] launching=%d (%s)", on ? 1 : 0, why);
    emit launchingChanged();
}

VrLauncher::~VrLauncher()
{
    if (s_Instance == this) {
        s_Instance = nullptr;
    }
    // §REVIEW-1119：同 aboutToQuit。背景的 openAtAppStart 若這時才成功，看到旗標就自己關，不再碰 qApp。
    s_HotspotClosing.store(true, std::memory_order_release);
    {
        QMutexLocker decision(&s_HotspotDecisionMu);
        FrameHotspot::closeIfOwner();  // §FRAME-HOTSPOT-AUTO：App 結束
    }
    if (m_Proc == nullptr) {
        return;
    }
    m_Proc->disconnect(this);
    if (m_Proc->state() != QProcess::NotRunning) {
        // GUI 要結束時（例如 Frame 的「結束遊戲」對整個行程群組送 SIGTERM），子行程多半也正在收尾（送 /cancel、
        // 拆 XR）。先等它自己結束，逾時才 SIGTERM，再逾時才交給 ~QProcess 的 SIGKILL。子行程收到第二個 SIGTERM
        // 會直接 _Exit，所以一開始不送。
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] waiting for the VR stream process to finish");
        if (!m_Proc->waitForFinished(8000)) {
            m_Proc->terminate();
            if (!m_Proc->waitForFinished(4000)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] VR stream process did not exit; killing it");
            }
        }
    }
}

bool VrLauncher::vrShellByDefault()
{
#if defined(Q_OS_LINUX) && defined(HAVE_OPENXR)
    return !qEnvironmentVariableIsEmpty("GAMESCOPE_WAYLAND_DISPLAY") && !inPidNamespacedSandbox();
#else
    return false;
#endif
}

bool VrLauncher::available() const
{
#if defined(Q_OS_LINUX) && defined(HAVE_OPENXR)
    // Frame 從收藏庫開的 app 都跑在 gamescope 裡；一般 Linux 桌面不跳模式選擇
    return !qEnvironmentVariableIsEmpty("GAMESCOPE_WAYLAND_DISPLAY");
#else
    return false;
#endif
}

namespace {
struct DevVideo {
    int tab = -1;
    QString url;
    int grabSec = 0;
    QString grabPath;
    int playSec = 0;
    QString host;
    QString dir;
    QString hide;
    bool stripAppEnv = false;
} s_DevVideo;
}  // namespace

void VrLauncher::setDevVideo(const QString& spec)
{
    if (spec.isEmpty()) {
        return;
    }
    const QStringList parts = spec.split(QLatin1Char(','));
    s_DevVideo.tab = parts.value(0).toInt();
    for (const QString& p : parts.mid(1)) {
        const QString k = p.section(QLatin1Char('='), 0, 0), v = p.section(QLatin1Char('='), 1);
        if (k == QLatin1String("url")) {
            s_DevVideo.url = v;
        }
        else if (k == QLatin1String("grab")) {
            s_DevVideo.grabSec = v.section(QLatin1Char(':'), 0, 0).toInt();
            s_DevVideo.grabPath = v.section(QLatin1Char(':'), 1);
        }
        else if (k == QLatin1String("play")) {
            s_DevVideo.playSec = v.toInt();
        }
        else if (k == QLatin1String("host")) {
            s_DevVideo.host = v;
        }
        else if (k == QLatin1String("dir")) {
            s_DevVideo.dir = v;
        }
        else if (k == QLatin1String("hide")) {
            s_DevVideo.hide = v;
        }
        else if (k == QLatin1String("appenv")) {
            s_DevVideo.stripAppEnv = v == QLatin1String("strip");
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] dev video page: tab=%d url=%s grab=%d play=%d host=%s dir=%s",
                s_DevVideo.tab, qPrintable(s_DevVideo.url), s_DevVideo.grabSec, s_DevVideo.playSec, qPrintable(s_DevVideo.host),
                qPrintable(s_DevVideo.dir));
}

int VrLauncher::devVideoTab() const { return s_DevVideo.tab; }
QString VrLauncher::devVideoUrl() const { return s_DevVideo.url; }
int VrLauncher::devVideoGrabSec() const { return s_DevVideo.grabSec; }
QString VrLauncher::devVideoGrabPath() const { return s_DevVideo.grabPath; }
int VrLauncher::devVideoPlaySec() const { return s_DevVideo.playSec; }
QString VrLauncher::devVideoHost() const { return s_DevVideo.host; }
QString VrLauncher::devVideoDir() const { return s_DevVideo.dir; }
QString VrLauncher::devLaunchHide() const { return s_DevVideo.hide; }

bool VrLauncher::shellActive() const
{
    return static_cast<bool>(s_ChildHook);
}

bool VrLauncher::shellFocused() const
{
    return s_ShellFocused;
}

void VrLauncher::setShellFocused(bool focused)
{
    if (focused == s_ShellFocused) {
        return;
    }
    s_ShellFocused = focused;
    if (s_Instance != nullptr) {
        emit s_Instance->shellFocusedChanged();
    }
}

bool VrLauncher::launchDesktop(const QString& hostAddress, const QString& appName, bool takeover)
{
    if (!s_ChildHook || running() || hostAddress.isEmpty() || appName.isEmpty()) {
        return false;
    }
    QStringList args{QStringLiteral("stream"), QStringLiteral("--display-target"), QStringLiteral("xr-desktop")};
    if (takeover) {
        args << QStringLiteral("--takeover");
    }
    args << QStringLiteral("--xr-handback") << QStringLiteral("--") << hostAddress << appName;
    m_ChildIsPlayer = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] launching desktop stream (XR screen): app=\"%s\"%s",
                qPrintable(logSafe(appName)), takeover ? " (takeover)" : "");
    if (!startChild(args, QStringLiteral("desktop stream"))) {
        return false;
    }
    setLaunching(true, appName, "desktop stream started");
    return true;
}

bool VrLauncher::playerAvailable() const
{
#ifdef HAVE_VR_PLAYER
    return available();
#else
    return false;
#endif
}

QString VrLauncher::blockedReason() const
{
#if defined(Q_OS_LINUX) && defined(HAVE_OPENXR)
    if (inPidNamespacedSandbox()) {
        // 正式的入口是 install-steamframe.sh 裝的啟動器（Steam 收藏庫的捷徑、選單裡的 VipleStream）；直接 flatpak run 才會走到這裡
        return tr("VR and video mode are not available when VipleStream is started this way (Flatpak PID sandbox). "
                  "Start it from the VipleStream shortcut in your Steam library, which the Steam Frame installer "
                  "(install-steamframe.sh) adds.");
    }
    return QString();
#else
    return tr("VR mode is not supported on this platform.");
#endif
}

bool VrLauncher::running() const
{
    return m_InProcessPlayer || (m_Proc != nullptr && m_Proc->state() != QProcess::NotRunning);
}

bool VrLauncher::streamRunning() const
{
    return !m_ChildIsPlayer && m_Proc != nullptr && m_Proc->state() != QProcess::NotRunning;
}

bool VrLauncher::setVrMode(bool on)
{
    if (on && !blockedReason().isEmpty()) {
        on = false;
    }
    if (m_VrMode == on) {
        return false;
    }
    // §VR-PREFLIGHT：模式換了，還在檢查或等使用者決定的那一場就不開了
    m_Preflight->cancel("mode changed");
    m_VrMode = on;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] mode=%s", on ? "vr" : "desktop");
    emit vrModeChanged();
    return true;
}

bool VrLauncher::preflightChecking() const
{
    return m_Preflight->checking();
}

bool VrLauncher::preflightWaiting() const
{
    return m_Preflight->waiting();
}

int VrLauncher::preflightMeasuredMs() const
{
    return m_Preflight->measuredRttMs();
}

int VrLauncher::preflightRttMs() const
{
    return m_Preflight->lastRttMs();
}

int VrLauncher::preflightGoodStreak() const
{
    return m_Preflight->goodStreak();
}

int VrLauncher::preflightThresholdMs() const
{
    return vrpreflight::kThresholdMs;
}

int VrLauncher::preflightGoodNeeded() const
{
    return vrpreflight::kGoodStreakToStart;
}

int VrLauncher::preflightIntervalSec() const
{
    return vrpreflight::kProbeIntervalMs / 1000;
}

bool VrLauncher::setTestPreflightScript(const QString& spec, QString* error)
{
    return VrPreflightCheck::setTestScript(spec, error);
}

bool VrLauncher::launch(const QString& hostAddress, const QString& probeHost, int probePort,
                        const QString& appName, bool takeover)
{
    if (!m_VrMode || running() || hostAddress.isEmpty() || appName.isEmpty() || !blockedReason().isEmpty()) {
        return false;
    }
    // 上一次按下去的還在檢查、或對話框正開著等使用者決定：不接第二次（對話框是 modal，這裡是最後一道）
    if (m_Preflight->busy()) {
        return false;
    }

    m_PendingAddress = hostAddress;
    m_PendingApp = appName;
    m_PendingTakeover = takeover;
    // 夠快或量不到時 proceed() 幾乎立刻發出（沒有可以量的目標時就在這個呼叫裡），接著 preflightPassed() 開子行程。
    // 先設 launching：begin() 可能在這個呼叫裡就開了子行程，之後的取消／丟棄會把它清掉
    setLaunching(true, appName, "VR stream requested");
    if (!m_Preflight->begin(probeHost, probePort)) {
        setLaunching(false, QString(), "preflight not started");
        return false;
    }
    return true;
}

void VrLauncher::preflightStartAnyway()
{
    m_Preflight->startAnyway();
}

void VrLauncher::preflightCancel()
{
    m_Preflight->cancel("the user");
}

void VrLauncher::preflightPassed()
{
    const QString hostAddress = m_PendingAddress;
    const QString appName = m_PendingApp;
    const bool takeover = m_PendingTakeover;
    m_PendingAddress.clear();
    m_PendingApp.clear();
    m_PendingTakeover = false;

    // 檢查期間狀態可能變了（開頭那幾次還在量的時候介面沒有被擋住）
    if (!m_VrMode || running() || hostAddress.isEmpty() || appName.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-LAUNCHER] preflight: launch dropped (no longer in VR mode, or a stream is already running)");
        setLaunching(false, QString(), "launch dropped");
        return;
    }
    if (!startProcess(hostAddress, appName, takeover)) {
        setLaunching(false, QString(), "process not started");
    }
}

bool VrLauncher::launchPlayer(const QString& source, const QString& format, const QString& title, const QString& audioSource)
{
    if (running() || source.trimmed().isEmpty() || !available() || !blockedReason().isEmpty()) {
        return false;
    }
    if (s_PlayerHandler) {
        // §VR-SHELL（A2）：在 VR 介面的 XR session 裡播，SteamVR 不必換程式
        const PlayRequest req{source.trimmed(), format, title, audioSource};
        if (!s_PlayerHandler(req)) {
            return false;
        }
        m_ChildIsPlayer = true;
        m_InProcessPlayer = true;
        m_LastError.clear();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] video player started in the VR interface");
        emit runningChanged();
        return true;
    }
    QStringList args{QStringLiteral("play")};
    if (!format.isEmpty() && format != QLatin1String("auto")) {
        args << QStringLiteral("--vr-format") << format;
    }
    if (!title.isEmpty()) {
        args << QStringLiteral("--title") << title;
    }
    if (!audioSource.isEmpty()) {
        args << QStringLiteral("--audio-url") << audioSource;
    }
    if (s_ChildHook) {
        args << QStringLiteral("--xr-handback");  // §VR-SHELL
    }
    // 來源放在 "--" 之後：以 '-' 開頭的檔名不會被當成選項
    args << QStringLiteral("--") << source.trimmed();
    m_ChildIsPlayer = true;
    return startChild(args, QStringLiteral("video player"));
}

void VrLauncher::killChild()
{
    if (m_Proc != nullptr && m_Proc->state() != QProcess::NotRunning) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] killing the child process (did not end on its own)");
        m_Proc->kill();
    }
}

bool VrLauncher::launchNetBenchStep(const QString& hostAddress, int eye, int seconds, const QString& resultPath)
{
    if (running() || hostAddress.isEmpty() || resultPath.isEmpty() || eye <= 0) {
        return false;
    }
    const QString eyeArg = QStringLiteral("%1x%1").arg(eye);
    QStringList args{QStringLiteral("stream"), QStringLiteral("--display-target"), QStringLiteral("pcvr"),
                     QStringLiteral("--vr-eye"), eyeArg, QStringLiteral("--vr-hz"), QStringLiteral("144"),
                     QStringLiteral("--vr-netbench"), resultPath, QStringLiteral("--vr-netbench-secs"), QString::number(seconds),
                     // §REVIEW-1111：測試以 144 張／秒算缺額。使用者開著「半速」時每秒只有 72 張新影像，
                     // 每一階都會被判成撐不住、階梯停在最低一階。測試一律全速。
                     QStringLiteral("--no-vr-half-rate")};
    if (s_ChildHook) {
        args << QStringLiteral("--xr-handback");  // §VR-SHELL
    }
    args << QStringLiteral("--") << hostAddress << NetBench::appName();
    m_ChildIsPlayer = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] launching network test step: eye=%s secs=%d host=%s",
                qPrintable(eyeArg), seconds, qPrintable(hostAddress));
    if (!startChild(args, QStringLiteral("network test"))) {
        return false;
    }
    setLaunching(true, NetBench::appName(), "network test step started");
    return true;
}

bool VrLauncher::startProcess(const QString& hostAddress, const QString& appName, bool takeover)
{
    // 只帶最少的參數（見 vrlauncher.h）。位置參數放在 "--" 之後：app 名稱以 '-' 開頭時不會被當成選項。
    QStringList args{QStringLiteral("stream"), QStringLiteral("--display-target"), QStringLiteral("pcvr")};
    if (takeover) {
        args << QStringLiteral("--takeover");
    }
    if (s_ChildHook) {
        args << QStringLiteral("--xr-handback");  // §VR-SHELL
    }
    args << QStringLiteral("--") << hostAddress << appName;
    m_ChildIsPlayer = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] launching VR stream: app=\"%s\"%s",
                qPrintable(logSafe(appName)), takeover ? " (takeover)" : "");
    return startChild(args, QStringLiteral("VR stream"));
}

namespace {
// 0＝沒有子行程；INT64_MAX＝子行程在跑；其他＝結束之後的保護期限（steady clock 毫秒）
std::atomic<int64_t> s_ChildGuardUntilMs{0};
int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

bool VrLauncher::childGuardActive()
{
    return steadyMs() < s_ChildGuardUntilMs.load(std::memory_order_acquire);
}

bool VrLauncher::startChild(const QStringList& args, const QString& what)
{

    if (m_Proc == nullptr) {
        m_Proc = new QProcess(this);
        // stdout 直接轉給父行程（wrapper 會收）；stderr 自己讀：照樣轉寫到父行程的 stderr，同時抓錯誤行給 GUI 顯示
        m_Proc->setProcessChannelMode(QProcess::ForwardedOutputChannel);
#if defined(Q_OS_LINUX)
        // GUI 異常死亡（崩潰、SIGKILL）時子行程也收到 SIGTERM、正常收尾，不會變成沒有 GUI 的孤兒串流
        m_Proc->setChildProcessModifier([]() {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
        });
#endif
        connect(m_Proc, &QProcess::readyReadStandardError, this, &VrLauncher::onStderr);
        connect(m_Proc, &QProcess::finished, this, &VrLauncher::processEnded);
        connect(m_Proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
            if (err == QProcess::FailedToStart) {
                // §REVIEW-1119：這個訊號在 start() 返回前就進來。呼叫端若看到 true 會再把 launching 設回去。
                m_StartFailed = true;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] failed to start the stream process");
                if (!singleInstanceReacquire()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] could not take the single-instance lock back");
                }
                s_ChildGuardUntilMs.store(0, std::memory_order_release);
                if (s_ChildHook) {
                    s_ChildHook(ChildEvent::Ended);
                }
                setLaunching(false, QString(), "failed to start");
                emit runningChanged();
                emit finished(-1, tr("Could not start the VR stream."));
            }
        });
    }

    // 子行程用 GUI 改寫之前的環境：main() 為了平面 GUI 把 SDL 的 video driver 改成 x11／wayland，子行程是無頭的
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"QT_QPA_PLATFORM", "SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER"}) {
        QByteArray original;
        if (SfEnv::originalValue(name, &original)) {
            env.insert(QString::fromLatin1(name), QString::fromLocal8Bit(original));
        }
        else {
            env.remove(QString::fromLatin1(name));
        }
    }
    if (s_DevVideo.stripAppEnv) {
        // 實驗（dev，--dev-video appenv=strip）：子行程不帶 Steam 給這個捷徑的 app 識別，SteamVR 就不會把它當成跟平面
        // GUI 同一個 app（同一個 app 的 VR 畫面一出來，SteamVR 會打開選單把平面視窗收進去）
        int removed = 0;
        for (const QString& k : env.keys()) {
            if (k == QLatin1String("SteamAppId") || k == QLatin1String("SteamGameId") || k == QLatin1String("SteamOverlayGameId") ||
                k == QLatin1String("STEAM_COMPAT_APP_ID")) {
                env.remove(k);
                removed++;
            }
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] dev: removed %d Steam app id variable(s) from the child", removed);
    }
    m_Proc->setProcessEnvironment(env);

    m_StderrPartial.clear();
    m_LastError.clear();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] starting the %s process", qPrintable(what));
    // §VR-SHELL：告訴 VR 介面（它保留自己的 XR session，等子行程的接手）
    if (s_ChildHook) {
        s_ChildHook(ChildEvent::Starting);
    }
    // 子行程是同一支程式，會自己拿單一實例鎖：開之前先放掉，結束後再拿回
    singleInstanceReleaseForChild();
    s_ChildGuardUntilMs.store(INT64_MAX, std::memory_order_release);
    m_StartFailed = false;
    m_Proc->start(QCoreApplication::applicationFilePath(), args);
    // §REVIEW-1119：FailedToStart 不會再來一次 finished。回 false，launchDesktop／launchNetBenchStep 才不會把遮罩設回去。
    if (m_StartFailed) {
        return false;
    }
    emit runningChanged();
    return true;
}

QString VrLauncher::playerError(int exitCode, bool crashed) const
{
    // §VR-PLAYER 的結束碼（playersession.h）：0 正常、2 XR 起不來、3 影片開不了或讀取失敗、4 XR session 中途結束
    if (crashed) {
        return tr("The video player stopped unexpectedly.");
    }
    if (exitCode == 2) {
        return tr("Could not start VR for the video player.") + (m_LastError.isEmpty() ? QString() : QStringLiteral("\n") + m_LastError);
    }
    if (exitCode == 3) {
        return tr("Could not play this video.") + (m_LastError.isEmpty() ? QString() : QStringLiteral("\n") + m_LastError);
    }
    if (exitCode != 0 && exitCode != 4) {
        return !m_LastError.isEmpty() ? m_LastError : tr("The video player ended with an error (code %1).").arg(exitCode);
    }
    return QString();
}

void VrLauncher::onStderr()
{
    const QByteArray data = m_Proc->readAllStandardError();
    fwrite(data.constData(), 1, data.size(), stderr);
    fflush(stderr);

    m_StderrPartial += data;
    int nl;
    while ((nl = m_StderrPartial.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_StderrPartial.left(nl)).trimmed();
        m_StderrPartial.remove(0, nl + 1);
        if (line.startsWith(QLatin1String("VIPLE-XR-STATE "))) {
            const QString st = line.mid(15);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] child XR session %s", qPrintable(logSafe(st)));
            if (st == QLatin1String("HANDBACK") && s_ChildHook) {
                s_ChildHook(ChildEvent::HandBack);
            }
            // 子行程的 XR session 到了 VISIBLE／FOCUSED（或已經要交回）：頭盔畫面已經換成它的，「啟動中」到此為止
            setLaunching(false, QString(), "child XR session up");
            emit childXrState(st);
            continue;
        }
        const QString err = userFacingError(line);
        if (!err.isEmpty()) {
            m_LastError = err;
        }
    }
    // 沒有換行的超長輸出不要一直累積
    if (m_StderrPartial.size() > 64 * 1024) {
        m_StderrPartial.clear();
    }
}

void VrLauncher::processEnded(int exitCode, QProcess::ExitStatus status)
{
    if (!m_StderrPartial.isEmpty()) {
        const QString err = userFacingError(QString::fromUtf8(m_StderrPartial).trimmed());
        if (!err.isEmpty()) {
            m_LastError = err;
        }
        m_StderrPartial.clear();
    }
    if (!singleInstanceReacquire()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] could not take the single-instance lock back");
    }
    s_ChildGuardUntilMs.store(steadyMs() + 3000, std::memory_order_release);
    if (s_ChildHook) {
        s_ChildHook(ChildEvent::Ended);
    }

    QString error;
    if (m_ChildIsPlayer) {
        error = playerError(exitCode, status != QProcess::NormalExit);
    }
    else if (status != QProcess::NormalExit) {
        error = tr("The VR stream stopped unexpectedly.");
    }
    else if (exitCode == 2) {
        // main.cpp：主機上在跑別的 app（appQuitRequired）
        error = tr("Another app is running on the host. Quit it first, then start the VR app again.");
    }
    else if (exitCode != 0) {
        error = !m_LastError.isEmpty() ? m_LastError
                                       : tr("The VR stream ended with an error (code %1).").arg(exitCode);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] stream process finished exit=%d status=%d%s%s",
                exitCode, static_cast<int>(status), error.isEmpty() ? "" : " error=", qPrintable(logSafe(error)));
    // §FRAME-HOTSPOT-PAUSE：子行程正常結束時自己會還原熱點；被強制結束或當掉的話在這裡補做
    FrameHotspot::recoverStale();
    setLaunching(false, QString(), "process ended");
    emit runningChanged();
    emit finished(exitCode, error);
}
