#include "vrlauncher.h"

#include "sfenv.h"
#include "vrpreflightcheck.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcessEnvironment>

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

}  // namespace

VrLauncher::VrLauncher(QObject* parent)
    : QObject(parent),
      m_Preflight(new VrPreflightCheck(this))
{
    // §VR-PREFLIGHT：檢查的結果。proceed＝可以開了（夠快、量不到、連續夠快、使用者堅持）
    connect(m_Preflight, &VrPreflightCheck::changed, this, &VrLauncher::preflightChanged);
    connect(m_Preflight, &VrPreflightCheck::proceed, this, &VrLauncher::preflightPassed);
    connect(m_Preflight, &VrPreflightCheck::cancelled, this, [this]() {
        m_PendingAddress.clear();
        m_PendingApp.clear();
        m_PendingTakeover = false;
    });
}

VrLauncher::~VrLauncher()
{
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
    return m_Proc != nullptr && m_Proc->state() != QProcess::NotRunning;
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
    // 夠快或量不到時 proceed() 幾乎立刻發出（沒有可以量的目標時就在這個呼叫裡），接著 preflightPassed() 開子行程
    return m_Preflight->begin(probeHost, probePort);
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
        return;
    }
    startProcess(hostAddress, appName, takeover);
}

bool VrLauncher::launchPlayer(const QString& source, const QString& format, const QString& title, const QString& audioSource)
{
    if (running() || source.trimmed().isEmpty() || !available() || !blockedReason().isEmpty()) {
        return false;
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
    // 來源放在 "--" 之後：以 '-' 開頭的檔名不會被當成選項
    args << QStringLiteral("--") << source.trimmed();
    m_ChildIsPlayer = true;
    return startChild(args, QStringLiteral("video player"));
}

bool VrLauncher::startProcess(const QString& hostAddress, const QString& appName, bool takeover)
{
    // 只帶最少的參數（見 vrlauncher.h）。位置參數放在 "--" 之後：app 名稱以 '-' 開頭時不會被當成選項。
    QStringList args{QStringLiteral("stream"), QStringLiteral("--display-target"), QStringLiteral("pcvr")};
    if (takeover) {
        args << QStringLiteral("--takeover");
    }
    args << QStringLiteral("--") << hostAddress << appName;
    m_ChildIsPlayer = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] launching VR stream: app=\"%s\"%s",
                qPrintable(appName), takeover ? " (takeover)" : "");
    return startChild(args, QStringLiteral("VR stream"));
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
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] failed to start the stream process");
                if (!singleInstanceReacquire()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] could not take the single-instance lock back");
                }
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
    // 子行程是同一支程式，會自己拿單一實例鎖：開之前先放掉，結束後再拿回
    singleInstanceReleaseForChild();
    m_Proc->start(QCoreApplication::applicationFilePath(), args);
    emit runningChanged();
    return true;
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
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] child XR session %s", qPrintable(st));
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

    QString error;
    if (m_ChildIsPlayer) {
        // §VR-PLAYER 的結束碼（playersession.h）：0 正常、2 XR 起不來、3 影片開不了或讀取失敗、4 XR session 中途結束
        if (status != QProcess::NormalExit) {
            error = tr("The video player stopped unexpectedly.");
        }
        else if (exitCode == 2) {
            error = tr("Could not start VR for the video player.") + (m_LastError.isEmpty() ? QString() : QStringLiteral("\n") + m_LastError);
        }
        else if (exitCode == 3) {
            error = tr("Could not play this video.") + (m_LastError.isEmpty() ? QString() : QStringLiteral("\n") + m_LastError);
        }
        else if (exitCode != 0 && exitCode != 4) {
            error = !m_LastError.isEmpty() ? m_LastError : tr("The video player ended with an error (code %1).").arg(exitCode);
        }
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
                exitCode, static_cast<int>(status), error.isEmpty() ? "" : " error=", qPrintable(error));
    emit runningChanged();
    emit finished(exitCode, error);
}
