#include "vrlauncher.h"

#include "sfenv.h"

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
    static const char* const k_Prefixes[] = {"Stream failed: ", "Stream error: ", "Error: "};
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
    : QObject(parent)
{
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

QString VrLauncher::blockedReason() const
{
#if defined(Q_OS_LINUX) && defined(HAVE_OPENXR)
    if (inPidNamespacedSandbox()) {
        return tr("VR mode is not available when VipleStream is started this way (Flatpak PID sandbox). "
                  "Start it from the VipleStream VR shortcut instead.");
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
    m_VrMode = on;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] mode=%s", on ? "vr" : "desktop");
    emit vrModeChanged();
    return true;
}

bool VrLauncher::launch(const QString& hostAddress, const QString& appName, bool takeover)
{
    if (!m_VrMode || running() || hostAddress.isEmpty() || appName.isEmpty() || !blockedReason().isEmpty()) {
        return false;
    }

    // 只帶最少的參數（見 vrlauncher.h）。位置參數放在 "--" 之後：app 名稱以 '-' 開頭時不會被當成選項。
    QStringList args{QStringLiteral("stream"), QStringLiteral("--display-target"), QStringLiteral("pcvr")};
    if (takeover) {
        args << QStringLiteral("--takeover");
    }
    args << QStringLiteral("--") << hostAddress << appName;

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
    m_Proc->setProcessEnvironment(env);

    m_StderrPartial.clear();
    m_LastError.clear();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] launching VR stream: app=\"%s\"%s",
                qPrintable(appName), takeover ? " (takeover)" : "");
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
    if (status != QProcess::NormalExit) {
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
