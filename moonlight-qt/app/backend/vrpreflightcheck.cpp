#include "vrpreflightcheck.h"

#include <QNetworkProxy>
#include <QTcpSocket>

#include "SDL_compat.h"

namespace {

// dev（--vr-test-preflight）：行程內全域，絕不進 StreamingPreferences（那邊的值會被存回設定）
vrpreflight::TestScript s_TestScript;

QString describeSocketError(QAbstractSocket::SocketError error)
{
    switch (error) {
    case QAbstractSocket::ConnectionRefusedError:
        return QStringLiteral("refused");
    case QAbstractSocket::RemoteHostClosedError:
        return QStringLiteral("closed");
    case QAbstractSocket::HostNotFoundError:
        return QStringLiteral("host not found");
    case QAbstractSocket::SocketTimeoutError:
        return QStringLiteral("timeout");
    case QAbstractSocket::NetworkError:
        return QStringLiteral("network error");
    default:
        return QStringLiteral("socket error %1").arg(static_cast<int>(error));
    }
}

}  // namespace

VrRttProbe::VrRttProbe(QObject* parent)
    : QObject(parent)
{
}

void VrRttProbe::start(quint64 id, const QString& host, quint16 port, int timeoutMs)
{
    abort();

    if (m_Timeout == nullptr) {
        // 在這條執行緒裡才建立（建構子是在 GUI 執行緒跑的）
        m_Timeout = new QTimer(this);
        m_Timeout->setSingleShot(true);
        connect(m_Timeout, &QTimer::timeout, this, [this]() {
            finish(m_Socket, false, QStringLiteral("timeout"));
        });
    }

    m_Id = id;
    QTcpSocket* socket = new QTcpSocket(this);
    m_Socket = socket;

    // 跟 NvHTTP、NvComputer::getActiveAddressReachability() 一樣不走 proxy：要量的是到主機的那一條路
    socket->setProxy(QNetworkProxy::NoProxy);

    // 位址是主機名稱時先查 DNS：查到之後才開始算，只量握手那一段。位址是 IP 時 hostFound 在 connectToHost() 裡面
    // 就發了，等於從送出 SYN 之前開始算
    connect(socket, &QTcpSocket::hostFound, this, [this, socket]() {
        if (socket == m_Socket) {
            m_Elapsed.restart();
        }
    });
    connect(socket, &QTcpSocket::connected, this, [this, socket]() {
        finish(socket, true, QString());
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error) {
        finish(socket, false, describeSocketError(error));
    });

    m_Timeout->start(timeoutMs);
    m_Elapsed.start();
    socket->connectToHost(host, port);
}

void VrRttProbe::abort()
{
    if (m_Timeout != nullptr) {
        m_Timeout->stop();
    }
    if (m_Socket != nullptr) {
        QTcpSocket* socket = m_Socket;
        m_Socket = nullptr;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
}

void VrRttProbe::finish(QTcpSocket* socket, bool ok, const QString& why)
{
    // 先取時間，後面的整理不算進去
    const qint64 elapsedNs = m_Elapsed.nsecsElapsed();

    if (socket == nullptr || socket != m_Socket) {
        return;   // 這一次已經被丟掉或回報過了
    }
    m_Socket = nullptr;
    m_Timeout->stop();

    // 可能正在這個 socket 自己的 signal 裡面（errorOccurred 還可能是從 connectToHost() 裡直接發的）：
    // 不在這裡 abort()，交給事件迴圈刪掉，解構時會關閉連線
    socket->disconnect(this);
    socket->deleteLater();

    emit finished(m_Id, ok, ok ? static_cast<int>((elapsedNs + 500000) / 1000000) : -1, why);
}

VrPreflightCheck::VrPreflightCheck(QObject* parent)
    : QObject(parent)
{
    m_Clock.start();
    m_NextProbe.setSingleShot(true);
    connect(&m_NextProbe, &QTimer::timeout, this, &VrPreflightCheck::startProbe);
}

VrPreflightCheck::~VrPreflightCheck()
{
    if (m_Thread.isRunning()) {
        // 量測是非同步的，事件迴圈馬上就能結束；m_Probe 由 QThread::finished 接的 deleteLater 刪掉
        m_Thread.quit();
        m_Thread.wait();
    }
}

bool VrPreflightCheck::setTestScript(const QString& spec, QString* error)
{
    vrpreflight::TestScript script;
    std::string err;
    if (!script.configure(spec.toStdString(), &err)) {
        if (error != nullptr) {
            *error = QString::fromStdString(err);
        }
        return false;
    }
    s_TestScript = script;
    if (script.enabled()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-LAUNCHER] preflight: dev --vr-test-preflight \"%s\": scripted samples replace the "
                    "measurement (dev-only, not saved)",
                    script.spec().c_str());
    }
    return true;
}

bool VrPreflightCheck::begin(const QString& host, int port)
{
    if (m_Gate.busy()) {
        return false;
    }

    m_Script = s_TestScript;
    m_Script.rewind();
    m_Host = host;
    m_Port = (port > 0 && port <= 65535) ? static_cast<quint16>(port) : 0;

    if (!m_Script.enabled() && (m_Host.isEmpty() || m_Port == 0)) {
        // 沒有可以量的目標：不擋，照原本的方式開始
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-LAUNCHER] preflight: unmeasured: no address or port to probe - launching as before");
        emit proceed();
        return true;
    }

    if (m_Script.enabled()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-LAUNCHER] preflight: checking the round trip to the host (scripted samples, dev)");
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-LAUNCHER] preflight: checking the round trip to the host (tcp port %d)",
                    static_cast<int>(m_Port));
    }
    apply(m_Gate.begin(nowMs()));
    return true;
}

void VrPreflightCheck::startAnyway()
{
    apply(m_Gate.startAnyway(nowMs()));
}

void VrPreflightCheck::cancel(const char* who)
{
    apply(m_Gate.cancel(nowMs(), who));
}

void VrPreflightCheck::apply(const vrpreflight::Step& step)
{
    for (const std::string& line : step.log) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-LAUNCHER] preflight: %s", line.c_str());
    }

    switch (step.next) {
    case vrpreflight::Next::None:
        // 這個事件來的時候已經不在檢查中：什麼都沒變
        return;
    case vrpreflight::Next::ProbeNow:
        startProbe();
        break;
    case vrpreflight::Next::ProbeLater:
        m_NextProbe.start(step.delayMs);
        break;
    case vrpreflight::Next::Launch:
        stopProbing();
        emit changed();   // 介面先收起對話框
        emit proceed();
        return;
    case vrpreflight::Next::Cancelled:
        stopProbing();
        emit changed();
        emit cancelled();
        return;
    }
    emit changed();
}

void VrPreflightCheck::startProbe()
{
    if (!m_Gate.busy()) {
        return;
    }

    const quint64 id = ++m_ProbeId;
    m_ProbePending = true;
    m_Gate.probeStarted(nowMs());

    if (m_Script.enabled()) {
        const vrpreflight::Sample sample = m_Script.next();
        QTimer::singleShot(vrpreflight::simulatedDurationMs(sample), this, [this, id, sample]() {
            onProbeFinished(id, sample.ok, sample.rttMs, QString::fromStdString(sample.why));
        });
        return;
    }

    ensureWorker();
    emit probeRequested(id, m_Host, m_Port, vrpreflight::kSampleTimeoutMs);
}

void VrPreflightCheck::stopProbing()
{
    m_NextProbe.stop();
    if (m_ProbePending) {
        m_ProbePending = false;
        // 已經在路上的結果（含腳本排好的那一個）回來時 m_ProbePending 是 false，會被丟掉
        if (m_Probe != nullptr) {
            emit probeAbortRequested();
        }
    }
}

void VrPreflightCheck::onProbeFinished(quint64 id, bool ok, int rttMs, const QString& why)
{
    if (!m_ProbePending || id != m_ProbeId) {
        return;
    }
    m_ProbePending = false;
    apply(m_Gate.onSample(nowMs(), ok ? vrpreflight::Sample::measured(rttMs)
                                      : vrpreflight::Sample::failed(why.toStdString())));
}

void VrPreflightCheck::ensureWorker()
{
    if (m_Probe != nullptr) {
        return;
    }
    m_Probe = new VrRttProbe();
    m_Probe->moveToThread(&m_Thread);
    connect(&m_Thread, &QThread::finished, m_Probe, &QObject::deleteLater);
    connect(this, &VrPreflightCheck::probeRequested, m_Probe, &VrRttProbe::start);
    connect(this, &VrPreflightCheck::probeAbortRequested, m_Probe, &VrRttProbe::abort);
    connect(m_Probe, &VrRttProbe::finished, this, &VrPreflightCheck::onProbeFinished);
    m_Thread.setObjectName(QStringLiteral("VrPreflight"));
    m_Thread.start();
}
