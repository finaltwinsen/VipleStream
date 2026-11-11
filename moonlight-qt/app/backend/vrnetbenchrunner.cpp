#include "vrnetbenchrunner.h"

#include "backend/computermanager.h"
#include "backend/nvcomputer.h"
#include "backend/vrlauncher.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <Limelight.h>
#include <SDL.h>

namespace {
constexpr int kStates = 3;                // B → C → B
constexpr qint64 kPauseTimeoutMs = 20000; // 服務暫停家用 Wi-Fi 的等待上限
constexpr qint64 kResumeTimeoutMs = 60000;
constexpr qint64 kSettleAdapterMs = 6000;   // 切到只用轉接器之後
constexpr qint64 kSettleHomeMs = 20000;     // 接回家用 Wi-Fi 之後（單一無線電重新連線，熱點跟著掉約 20 秒）
constexpr qint64 kBetweenStepsMs = 3000;    // 上一階的子行程結束、host 停掉片段
// 一階的子行程超過這麼久還沒結束就強制結束（正常＝步長＋連線約 15 秒；子行程自己在寫完結果 20 秒後也會結束）
constexpr qint64 kStepTimeoutExtraMs = 150000;

bool stateIsAdapterOnly(int idx) { return idx == 1; }
QString stateName(int idx) { return stateIsAdapterOnly(idx) ? QStringLiteral("C") : QStringLiteral("B"); }
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
}  // namespace

VrNetBenchRunner::VrNetBenchRunner(QObject* parent) : QObject(parent)
{
    QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (dataHome.isEmpty()) {
        dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    if (!dataHome.isEmpty()) {
        m_Dir = QDir(dataHome).filePath(QStringLiteral("viplestream"));
    }
    m_Timer.setInterval(500);
    connect(&m_Timer, &QTimer::timeout, this, &VrNetBenchRunner::tick);
}

VrNetBenchRunner::~VrNetBenchRunner()
{
    if (m_AdapterOnly) {
        writeRequest(false);  // 程式結束：交還家用 Wi-Fi（服務 5 秒沒看到心跳也會自己交還）
    }
}

int VrNetBenchRunner::progress() const
{
    if (m_Phase == Phase::Idle) {
        return m_Verdict.isEmpty() ? 0 : 100;
    }
    const int perState = NetBench::ladderEyes().size();
    return qBound(0, (m_StateIdx * perState + m_EyeIdx) * 100 / (kStates * perState), 99);
}

QVariantList VrNetBenchRunner::rows() const
{
    QVariantList out;
    for (size_t li = 0; li < m_Ladders.size(); ++li) {
        const NetBench::Ladder& l = m_Ladders[li];
        for (const NetBench::StepResult& s : l.steps) {
            QVariantMap m;
            m[QStringLiteral("state")] = l.state == QLatin1String("C") ? tr("Adapter only") : tr("Home Wi-Fi + adapter");
            m[QStringLiteral("eye")] = s.eye;
            m[QStringLiteral("held")] = s.held;
            m[QStringLiteral("deficit")] = s.ran ? QString::number(s.deficitPct, 'f', 2) + QLatin1Char('%') : QStringLiteral("-");
            m[QStringLiteral("mtp95")] = s.ran && s.mtp95 > 0 ? QString::number(s.mtp95, 'f', 1) : QStringLiteral("-");
            m[QStringLiteral("mbps")] = s.ran ? QString::number(s.recvMbps, 'f', 0) : QStringLiteral("-");
            QString note;
            if (!s.ran) {
                note = tr("no result");
            }
            else if (s.half) {
                note = s.headsetLimited ? tr("headset limit") : tr("dropped to half rate");
            }
            else if (!s.held) {
                note = s.headsetLimited ? tr("headset limit") : tr("not smooth");
            }
            m[QStringLiteral("note")] = note;
            out << m;
        }
    }
    return out;
}

QString VrNetBenchRunner::lastSaved() const
{
    QSettings settings;
    return settings.value(QStringLiteral("vrnetbench/last")).toString();
}

void VrNetBenchRunner::log(const QString& msg)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-NETBENCH] %s", qUtf8Printable(msg));
}

void VrNetBenchRunner::setStatus(const QString& s)
{
    m_Status = s;
    emit changed();
}

bool VrNetBenchRunner::start(QObject* launcher, QObject* computerManager)
{
    if (running()) {
        return false;
    }
    m_Launcher = qobject_cast<VrLauncher*>(launcher);
    m_Cm = qobject_cast<ComputerManager*>(computerManager);
    m_Ladders.clear();
    m_Verdict.clear();
    m_Cancel = false;
    m_Retried = false;
    m_StateIdx = 0;
    m_EyeIdx = 0;
    if (m_Launcher == nullptr || m_Cm == nullptr || m_Dir.isEmpty()) {
        setStatus(tr("The network test is not available here."));
        return false;
    }
    if (m_Launcher->running()) {
        setStatus(tr("Stop the current stream first."));
        return false;
    }
    // 主機：線上、配對過、有 PCVR 的第一台
    m_Computer = nullptr;
    for (NvComputer* c : m_Cm->getComputers()) {
        QReadLocker lock(&c->lock);
        if (c->state == NvComputer::CS_ONLINE && c->pairState == NvComputer::PS_PAIRED &&
                (c->vipleStreamVr & VIPLE_VR_SERVER_CAP_PCVR) && !c->activeAddress.isNull()) {
            m_Computer = c;
            break;
        }
    }
    if (m_Computer == nullptr) {
        setStatus(tr("No VR-capable host is online. Turn on the host PC and try again."));
        return false;
    }
    const NetStatus st = readStatus();
    if (!st.fresh) {
        setStatus(tr("The headset network helper is not running. Reinstall VipleStream with the Steam Frame installer script."));
        return false;
    }
    if (!st.apStation) {
        setStatus(tr("The streaming adapter is not connected, so there is nothing to compare."));
        return false;
    }
    if (hostFor(true).isEmpty()) {
        setStatus(tr("The host has no address on the streaming adapter's network."));
        return false;
    }
    m_Loc = st.loc;
    connect(m_Launcher, &VrLauncher::finished, this, &VrNetBenchRunner::onChildFinished, Qt::UniqueConnection);
    log(QStringLiteral("start: host=%1 loc=%2").arg(hostFor(false), m_Loc.isEmpty() ? QStringLiteral("(none)") : m_Loc));
    m_Timer.start();
    beginState();
    return true;
}

void VrNetBenchRunner::cancel()
{
    if (!running()) {
        return;
    }
    m_Cancel = true;
    log(QStringLiteral("cancel requested"));
    if (m_Phase != Phase::Step) {
        finish(tr("Cancelled."));
    }
    else {
        setStatus(tr("Cancelling after this step..."));
    }
}

void VrNetBenchRunner::beginState()
{
    m_EyeIdx = 0;
    NetBench::Ladder l;
    l.state = stateName(m_StateIdx);
    m_Ladders.push_back(l);
    const bool want = stateIsAdapterOnly(m_StateIdx);
    m_PhaseStartMs = nowMs();
    if (want != m_AdapterOnly) {
        m_AdapterOnly = want;
        writeRequest(want);
        log(QStringLiteral("state %1: %2").arg(l.state, want ? QStringLiteral("pausing home Wi-Fi") : QStringLiteral("giving home Wi-Fi back")));
        setStatus(want ? tr("Pausing the headset's home Wi-Fi...") : tr("Reconnecting the headset's home Wi-Fi..."));
        m_Phase = Phase::SwitchNet;
    }
    else {
        log(QStringLiteral("state %1: no switch needed").arg(l.state));
        m_Phase = Phase::Settle;
        m_PhaseStartMs = nowMs() - (stateIsAdapterOnly(m_StateIdx) ? kSettleAdapterMs : kSettleHomeMs);  // 不用等
    }
    emit changed();
}

void VrNetBenchRunner::tick()
{
    if (m_AdapterOnly) {
        writeRequest(true);  // 心跳：服務 5 秒沒看到就交還家用 Wi-Fi
    }
    const qint64 now = nowMs();
    switch (m_Phase) {
    case Phase::SwitchNet: {
        const NetStatus st = readStatus();
        if (m_AdapterOnly) {
            if (st.applied == QLatin1String("adapter-only")) {
                log(QStringLiteral("home Wi-Fi paused; settling"));
                m_Phase = Phase::Settle;
                m_PhaseStartMs = now;
                setStatus(tr("Home Wi-Fi paused; waiting for the adapter link to settle..."));
            }
            else if (now - m_PhaseStartMs > kPauseTimeoutMs) {
                finish(tr("Could not pause the headset's home Wi-Fi (%1).").arg(st.err.isEmpty() ? tr("no answer from the helper") : st.err));
            }
        }
        else {
            // §REVIEW-1111：狀態字來自 helper 的 nmcli；helper 已固定用英文輸出（install-steamframe.sh 的 LC_ALL=C）。
            // 舊版 helper 在非英文系統會寫成譯文，這裡刻意不放寬——譯文分不出「已連線」還是「已斷線」。
            if (st.applied == QLatin1String("normal") && st.wlan0.startsWith(QLatin1String("connected"))) {
                log(QStringLiteral("home Wi-Fi reconnected; settling"));
                m_Phase = Phase::Settle;
                m_PhaseStartMs = now;
                setStatus(tr("Home Wi-Fi reconnected; waiting for the network to settle..."));
            }
            else if (now - m_PhaseStartMs > kResumeTimeoutMs) {
                finish(tr("The headset's home Wi-Fi did not reconnect (state: %1).").arg(st.wlan0));
            }
        }
        return;
    }
    case Phase::Settle: {
        const qint64 need = stateIsAdapterOnly(m_StateIdx) ? kSettleAdapterMs : kSettleHomeMs;
        if (now - m_PhaseStartMs >= need) {
            launchStep();
        }
        return;
    }
    case Phase::Step:
        if (now - m_PhaseStartMs > NetBench::kDefaultStepSec * 1000ll + kStepTimeoutExtraMs && m_Launcher != nullptr) {
            log(QStringLiteral("step did not end in time - stopping the stream process"));
            m_PhaseStartMs = now;  // killChild 之後 finished() 會進 onChildFinished
            m_Launcher->killChild();
        }
        return;
    default:
        return;
    }
}

QString VrNetBenchRunner::resultPath() const
{
    return QDir(m_Dir).filePath(QStringLiteral("netbench-step.json"));
}

void VrNetBenchRunner::launchStep()
{
    if (m_Cancel) {
        finish(tr("Cancelled."));
        return;
    }
    if (m_Launcher == nullptr) {
        finish(tr("The network test is not available here."));
        return;
    }
    const int eye = NetBench::ladderEyes().value(m_EyeIdx);
    const QString path = resultPath();
    QDir().mkpath(m_Dir);
    QFile::remove(path);
    const QString host = hostFor(stateIsAdapterOnly(m_StateIdx));
    if (host.isEmpty() || !m_Launcher->launchNetBenchStep(host, eye, NetBench::kDefaultStepSec, path)) {
        finish(tr("Could not start the test stream."));
        return;
    }
    m_Phase = Phase::Step;
    m_PhaseStartMs = nowMs();
    const QString stateText = stateIsAdapterOnly(m_StateIdx) ? tr("adapter only") : tr("home Wi-Fi + adapter");
    setStatus(tr("Testing %1: %2 x %2 per eye at 144 Hz (step %3 of up to %4)...")
                  .arg(stateText).arg(eye).arg(m_StateIdx * NetBench::ladderEyes().size() + m_EyeIdx + 1)
                  .arg(kStates * NetBench::ladderEyes().size()));
}

void VrNetBenchRunner::onChildFinished(int exitCode, const QString& error)
{
    if (m_Phase != Phase::Step) {
        return;
    }
    const int eye = NetBench::ladderEyes().value(m_EyeIdx);
    NetBench::StepResult r = NetBench::readResult(resultPath(), eye);
    if (!r.ran && r.error.isEmpty()) {
        r.error = QStringLiteral("no measurements");
    }
    if (!error.isEmpty()) {
        r.error = error;
    }
    // 一次性的狀況重跑一次（三個網路狀態用同一條規則）：
    //   整階沒影像（11-09：頭盔硬體解碼器有一階 0 張解出，跟網路無關）、
    //   差一點沒過（沒掉到半速、缺額 ≤ 1%，只是某一兩次連續重複超過 3 格——11-09 C 在 1152 只有一次 5 格就判不撐住）
    const bool noVideo = !r.ran || (r.recvMbps < 1.0 && r.deficitPct > 99.0);
    const bool marginal = r.ran && !r.held && !r.half && r.deficitPct <= 1.0;
    if ((noVideo || marginal) && !m_Retried && !m_Cancel) {
        m_Retried = true;
        log(QStringLiteral("state %1 eye=%2: %3 - running this step once more")
                .arg(m_Ladders.back().state).arg(eye)
                .arg(noVideo ? QStringLiteral("no video") : QStringLiteral("only just missed (deficit %1%, run %2)").arg(r.deficitPct, 0, 'f', 3).arg(r.maxRun)));
        m_Phase = Phase::Settle;
        m_PhaseStartMs = nowMs() - (stateIsAdapterOnly(m_StateIdx) ? kSettleAdapterMs : kSettleHomeMs) + kBetweenStepsMs;
        emit changed();
        return;
    }
    m_Retried = false;
    m_Ladders.back().steps.push_back(r);
    log(QStringLiteral("state %1 eye=%2 exit=%3 held=%4 deficit=%5% rep10=%6 run=%7 half=%8 mtp95=%9 recv=%10 Mbps lateNet=%11 lateHeadset=%12%13")
            .arg(m_Ladders.back().state).arg(eye).arg(exitCode).arg(r.held ? 1 : 0).arg(r.deficitPct, 0, 'f', 3).arg(r.rep10, 0, 'f', 1)
            .arg(r.maxRun).arg(r.half ? 1 : 0).arg(r.mtp95, 0, 'f', 1).arg(r.recvMbps, 0, 'f', 1).arg(r.lateNet).arg(r.lateHeadset)
            .arg(r.error.isEmpty() ? QString() : QStringLiteral(" error=") + r.error));
    // 第一階就沒有結果：多半是主機不支援（沒有「VR Network Test」）或連不上，不往下跑
    if (!r.ran && m_StateIdx == 0 && m_EyeIdx == 0) {
        finish(tr("The test stream did not run: %1").arg(r.error));
        return;
    }
    if (m_Cancel) {
        finish(tr("Cancelled."));
        return;
    }
    const bool more = r.held && m_EyeIdx + 1 < NetBench::ladderEyes().size();
    if (more) {
        ++m_EyeIdx;
        m_Phase = Phase::Settle;
        m_PhaseStartMs = nowMs() - (stateIsAdapterOnly(m_StateIdx) ? kSettleAdapterMs : kSettleHomeMs) + kBetweenStepsMs;
        emit changed();
        return;
    }
    if (m_StateIdx + 1 < kStates) {
        ++m_StateIdx;
        beginState();
        return;
    }
    finish(QString());
}

void VrNetBenchRunner::finish(const QString& failure)
{
    m_Timer.stop();
    if (m_AdapterOnly) {
        m_AdapterOnly = false;
        writeRequest(false);
    }
    m_Phase = Phase::Idle;
    if (!failure.isEmpty()) {
        log(QStringLiteral("ended without a result: %1").arg(failure));
        m_Verdict.clear();
        setStatus(failure);
        return;
    }
    const NetBench::Ladder* b2 = m_Ladders.size() >= 3 ? &m_Ladders[2] : nullptr;
    const NetBench::Decision d = NetBench::compareLadders(m_Ladders[0], m_Ladders[1], b2);
    const bool pickC = d.verdict == NetBench::Verdict::AdapterOnly;
    log(QStringLiteral("result: %1 (basis: %2, level %3) top B=%4 C=%5 B2=%6")
            .arg(pickC ? QStringLiteral("adapter only") : QStringLiteral("keep home Wi-Fi"), d.basis).arg(d.level)
            .arg(m_Ladders[0].topHeld()).arg(m_Ladders[1].topHeld()).arg(b2 != nullptr ? b2->topHeld() : -1));
    const QString date = QDate::currentDate().toString(Qt::ISODate);
    QSettings settings;
    if (!m_Loc.isEmpty()) {
        // vrnetmode.h 的「自動」讀這個（14 天後重測）
        settings.setValue(QStringLiteral("vrnetsel/") + m_Loc, QStringLiteral("%1|%2").arg(pickC ? QStringLiteral("C") : QStringLiteral("B"), date));
    }
    const int topB = (std::min)(m_Ladders[0].topHeld(), b2 != nullptr ? b2->topHeld() : m_Ladders[0].topHeld());
    const int topC = m_Ladders[1].topHeld();
    m_Verdict = pickC ? tr("Use adapter only: the headset pauses home Wi-Fi while you play VR here (smoothest: %1 per eye with adapter only, %2 with home Wi-Fi).")
                            .arg(topC).arg(topB)
                      : tr("Keep home Wi-Fi: adapter only was not clearly better here (smoothest: %1 per eye with home Wi-Fi, %2 with adapter only).")
                            .arg(topB).arg(topC);
    if (m_Ladders[1].headsetCapped() || m_Ladders[0].headsetCapped()) {
        m_Verdict += QLatin1Char(' ') + tr("The highest step was limited by the headset itself, not the network.");
    }
    settings.setValue(QStringLiteral("vrnetbench/last"),
                      QStringLiteral("%1: %2").arg(date, pickC ? tr("adapter only") : tr("keep home Wi-Fi")));
    setStatus(tr("Network test finished."));
}

void VrNetBenchRunner::writeRequest(bool adapterOnly)
{
    if (m_Dir.isEmpty() || !QDir().mkpath(m_Dir)) {
        return;
    }
    QJsonObject o;
    o.insert(QStringLiteral("mode"), adapterOnly ? QStringLiteral("adapter-only") : QStringLiteral("normal"));
    o.insert(QStringLiteral("t"), static_cast<qint64>(QDateTime::currentSecsSinceEpoch()));
    QSaveFile f(QDir(m_Dir).filePath(QStringLiteral("netmode-request.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

VrNetBenchRunner::NetStatus VrNetBenchRunner::readStatus() const
{
    NetStatus s;
    QFile f(QDir(m_Dir).filePath(QStringLiteral("netmode-status.json")));
    if (!f.open(QIODevice::ReadOnly)) {
        return s;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const qint64 t = o.value(QStringLiteral("t")).toVariant().toLongLong();
    s.fresh = t > 0 && QDateTime::currentSecsSinceEpoch() - t <= 5;
    s.apStation = o.value(QStringLiteral("ap_station")).toBool();
    s.applied = o.value(QStringLiteral("applied")).toString();
    s.loc = o.value(QStringLiteral("loc")).toString();
    s.wlan0 = o.value(QStringLiteral("wlan0")).toString();
    s.err = o.value(QStringLiteral("err")).toString();
    return s;
}

// 家用 Wi-Fi 的狀態用平常串流的位址；只用轉接器時家用網段連不到，改用主機在轉接器網段上的位址
// （主機在 /serverinfo 廣告的介面裡，和頭盔某張網卡（wlan0 以外）同一個子網路的那一個）
QString VrNetBenchRunner::hostFor(bool adapterOnly) const
{
    if (m_Computer == nullptr) {
        return QString();
    }
    QReadLocker lock(&m_Computer->lock);
    if (!adapterOnly) {
        return m_Computer->activeAddress.isNull() ? QString() : m_Computer->activeAddress.toString();
    }
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const auto& adv : m_Computer->serverAdvertisedInterfaces) {
        const QHostAddress host(adv.address);
        if (host.protocol() != QAbstractSocket::IPv4Protocol) {
            continue;
        }
        for (const QNetworkInterface& ni : ifaces) {
            if (ni.name() == QLatin1String("wlan0") || (ni.flags() & QNetworkInterface::IsLoopBack) ||
                    !(ni.flags() & QNetworkInterface::IsUp)) {
                continue;
            }
            for (const QNetworkAddressEntry& e : ni.addressEntries()) {
                if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && e.prefixLength() > 0 &&
                        host.isInSubnet(e.ip(), e.prefixLength())) {
                    return adv.address;
                }
            }
        }
    }
    return QString();
}
