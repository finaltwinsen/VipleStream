// §TS-PATH：串流期間若 host 在 Tailscale CGNAT，背景看路徑是直連還是 DERP 中繼。
//
// 只在看到中繼時升級，而且每一段連續中繼各做一次（不是每 5 秒重做）：
//   第一次看到中繼只開始計時。
//   ≥ 5 s   disco ping
//   ≥ 15 s  restun，接著 ping
//   ≥ 30 s  rebind，接著 ping（算第 1 次 rebind）
//   之後距離上一次 rebind ≥ 90 s 再 rebind，整個串流最多 5 次。
// 回到直連就清掉這一段的升級階段（5／15／30）。rebind 次數與「沒有 operator」
// 這個結論留到這次串流結束，不因中途恢復直連而重算。
//
// stop() 只設旗標、關掉正在用的 socket、叫醒等待，不 join。
// 行程結束時才 join：每個請求自己有逾時，socket 被 shutdown 會立刻從 recv 回來。

#include "tailscalepath.h"

#include "SDL_compat.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(Q_OS_LINUX)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

constexpr int kPollSec = 5;
constexpr int kPingSec = 5;
constexpr int kRestunSec = 15;
constexpr int kRebindSec = 30;
constexpr int kRebindGapSec = 90;
constexpr int kMaxRebinds = 5;
constexpr int kHttpTimeoutMs = 5000;
constexpr int kProcTimeoutMs = 10000;
constexpr int kProcSliceMs = 100;
constexpr int kMaxHttpBody = 4 * 1024 * 1024;

using clock = std::chrono::steady_clock;

// 行程要結束時關掉，避免靜態解構階段還去碰 SDL。
std::atomic<bool> g_Log{true};

void tsLog(const QString& line)
{
    if (!g_Log.load()) {
        return;
    }
    const QByteArray utf8 = line.toUtf8();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-TS-PATH] %s", utf8.constData());
}

// 落在 100.64.0.0/10 就回標準 IPv4 字串，否則空字串。IPv4-mapped IPv6 也認。
QString tailscaleHost(const QString& hostAddress)
{
    QString s = hostAddress.trimmed();
    if (s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']'))) {
        s = s.mid(1, s.size() - 2);
    }
    const int zone = s.indexOf(QLatin1Char('%'));
    if (zone >= 0) {
        s = s.left(zone);
    }
    const QHostAddress addr(s);
    if (addr.isNull()) {
        return {};
    }
    bool ok = false;
    const quint32 ip = addr.toIPv4Address(&ok);
    if (!ok) {
        return {};
    }
    // 高 10 bit 固定：100.64.0.0 = 0x64400000，/10 的遮罩是 0xFFC00000。
    if ((ip & 0xFFC00000u) != 0x64400000u) {
        return {};
    }
    return QHostAddress(ip).toString();
}

bool sameIp(const QString& a, const QString& b)
{
    const QHostAddress ha(a.trimmed());
    const QHostAddress hb(b.trimmed());
    if (ha.isNull() || hb.isNull()) {
        return false;
    }
    bool oka = false;
    bool okb = false;
    const quint32 va = ha.toIPv4Address(&oka);
    const quint32 vb = hb.toIPv4Address(&okb);
    if (oka && okb) {
        return va == vb;
    }
    return ha.isEqual(hb, QHostAddress::TolerantConversion);
}

QString oneLine(const QByteArray& raw, int maxChars)
{
    QString s = QString::fromUtf8(raw).trimmed();
    s.replace(QLatin1Char('\r'), QLatin1Char(' '));
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    while (s.contains(QLatin1String("  "))) {
        s.replace(QLatin1String("  "), QLatin1String(" "));
    }
    if (s.size() > maxChars) {
        s = s.left(maxChars) + QStringLiteral("...");
    }
    return s;
}

#if defined(Q_OS_LINUX)
const char* errnoName(int err)
{
    switch (err) {
    case ENOENT:
        return "ENOENT";
    case EACCES:
        return "EACCES";
    case EPERM:
        return "EPERM";
    case ECONNREFUSED:
        return "ECONNREFUSED";
    case ETIMEDOUT:
        return "ETIMEDOUT";
    case ENOTSOCK:
        return "ENOTSOCK";
    default:
        return nullptr;
    }
}

QString errnoText(int err)
{
    const char* name = errnoName(err);
    if (name != nullptr) {
        return QStringLiteral("errno=%1 (%2)").arg(err).arg(QLatin1String(name));
    }
    return QStringLiteral("errno=%1").arg(err);
}
#endif

struct Snapshot {
    enum class Kind { Missing, Direct, Relay } kind = Kind::Missing;
    QString curAddr;
    QString relay;
    QString hostName;
};

enum class ReadKind { Ok, Transient, Unavailable };

struct ReadResult {
    ReadKind kind = ReadKind::Transient;
    Snapshot snap;
    QString error;
};

struct ActionResult {
    bool denied = false;
    // false：命令根本沒送出去（連不上、exe 起不來、或送出前就被 stop）
    bool issued = false;
    QString summary;
};

QString withDenialHint(const ActionResult& r)
{
    if (!r.denied) {
        return r.summary;
    }
    return r.summary + QStringLiteral("; run once on the headset: sudo tailscale set --operator=$USER; not retrying restun/rebind this stream");
}

struct Endpoint {
    QString exe;
    QString sock;
};

struct Monitor {
    Monitor() = default;
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void start(const QString& hostAddress);
    void stop();
    void shutdownJoin();

    bool current(quint64 gen) const
    {
        return !quit.load() && generation.load() == gen;
    }

    bool waitInterval(quint64 gen);

#if defined(Q_OS_LINUX)
    // 登記正在用的 socket，讓 stop()／解構能 shutdown 把它從 recv 叫醒。
    bool adoptFd(int fd, quint64 gen);
    void clearFd(int fd);
#endif

    void threadMain();
    bool watch(const QString& hostAddress, quint64 gen);

    std::mutex mu;
    std::condition_variable cv;
    std::thread thread;
    std::atomic<bool> quit{false};
    std::atomic<quint64> generation{0};
    bool active = false;
    bool shuttingDown = false;
    QString host;
#if defined(Q_OS_LINUX)
    int sockFd = -1;
#endif
};

Monitor& monitor()
{
    static Monitor m;
    return m;
}

void shutdownAtExit()
{
    monitor().shutdownJoin();
}

void registerExitHook()
{
    static std::once_flag once;
    std::call_once(once, [] { qAddPostRoutine(&shutdownAtExit); });
}

Monitor::~Monitor()
{
    shutdownJoin();
}

void Monitor::shutdownJoin()
{
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (shuttingDown) {
            return;
        }
        shuttingDown = true;
        g_Log.store(false);
        quit.store(true);
        active = false;
        generation.fetch_add(1);
#if defined(Q_OS_LINUX)
        if (sockFd >= 0) {
            ::shutdown(sockFd, SHUT_RDWR);
        }
#endif
        if (thread.joinable()) {
            toJoin = std::move(thread);
        }
    }
    cv.notify_all();
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

void Monitor::start(const QString& hostAddress)
{
    registerExitHook();
    {
        std::lock_guard<std::mutex> lock(mu);
        if (shuttingDown) {
            return;
        }
        // 同一個 host 已經在看就不要重來，避免把升級階段清掉。
        if (active && host == hostAddress && thread.joinable()) {
            return;
        }
        host = hostAddress;
        active = true;
        generation.fetch_add(1);
#if defined(Q_OS_LINUX)
        if (sockFd >= 0) {
            ::shutdown(sockFd, SHUT_RDWR);
        }
#endif
        if (!thread.joinable()) {
            thread = std::thread([this] { threadMain(); });
        }
    }
    cv.notify_all();
}

void Monitor::stop()
{
    {
        std::lock_guard<std::mutex> lock(mu);
        if (shuttingDown) {
            return;
        }
        active = false;
        generation.fetch_add(1);
#if defined(Q_OS_LINUX)
        if (sockFd >= 0) {
            ::shutdown(sockFd, SHUT_RDWR);
        }
#endif
    }
    cv.notify_all();
}

bool Monitor::waitInterval(quint64 gen)
{
    std::unique_lock<std::mutex> lock(mu);
    cv.wait_for(lock, std::chrono::seconds(kPollSec), [&] {
        return quit.load() || generation.load() != gen;
    });
    return !quit.load() && generation.load() == gen;
}

#if defined(Q_OS_LINUX)
bool Monitor::adoptFd(int fd, quint64 gen)
{
    std::lock_guard<std::mutex> lock(mu);
    sockFd = fd;
    if (quit.load() || generation.load() != gen) {
        ::shutdown(fd, SHUT_RDWR);
        return false;
    }
    return true;
}

void Monitor::clearFd(int fd)
{
    std::lock_guard<std::mutex> lock(mu);
    if (sockFd == fd) {
        sockFd = -1;
    }
}
#endif

void Monitor::threadMain()
{
    std::unique_lock<std::mutex> lock(mu);
    while (!quit.load()) {
        cv.wait(lock, [&] { return quit.load() || active; });
        if (quit.load()) {
            break;
        }
        const QString h = host;
        const quint64 gen = generation.load();
        lock.unlock();
        const bool gaveUp = watch(h, gen);
        lock.lock();
        if (quit.load()) {
            break;
        }
        if (gaveUp && generation.load() == gen) {
            active = false;
        }
    }
}

#if defined(Q_OS_LINUX)

struct OwnedFd {
    Monitor* owner = nullptr;
    int fd = -1;
    ~OwnedFd()
    {
        if (fd < 0) {
            return;
        }
        if (owner != nullptr) {
            owner->clearFd(fd);
        }
        ::close(fd);
        fd = -1;
    }
    OwnedFd() = default;
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
};

void setTimeval(int fd, int opt, int timeoutMs)
{
    if (timeoutMs < 1) {
        timeoutMs = 1;
    }
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof(tv));
}

// 只開 socket、設 5 秒收送逾時、改成非阻塞。失敗回 -1（已關掉）。
int openUnixSocket(int* errOut)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *errOut = errno;
        return -1;
    }
    setTimeval(fd, SO_RCVTIMEO, kHttpTimeoutMs);
    setTimeval(fd, SO_SNDTIMEO, kHttpTimeoutMs);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        *errOut = errno;
        ::close(fd);
        return -1;
    }
    return fd;
}

// 0 連上（改回阻塞），-1 失敗（*errOut），-2 這次串流已經 stop。呼叫端持有 fd。
int connectUnix(int fd, const QString& path, Monitor& m, quint64 gen, clock::time_point deadline, int* errOut)
{
    const QByteArray utf8 = path.toUtf8();
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (utf8.isEmpty() || static_cast<std::size_t>(utf8.size()) >= sizeof(addr.sun_path)) {
        *errOut = ENAMETOOLONG;
        return -1;
    }
    std::memcpy(addr.sun_path, utf8.constData(), static_cast<std::size_t>(utf8.size()));

    int rc;
    do {
        rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    } while (rc < 0 && errno == EINTR);
    if (rc < 0 && errno != EINPROGRESS) {
        *errOut = errno;
        return -1;
    }
    if (rc < 0) {
        for (;;) {
            if (!m.current(gen)) {
                return -2;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            if (left <= 0) {
                *errOut = ETIMEDOUT;
                return -1;
            }
            // 切片是為了 stop() 沒來得及 shutdown 時也能在 200 ms 內看到旗標。
            const int slice = static_cast<int>(left > 200 ? 200 : left);
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int pr = ::poll(&pfd, 1, slice);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                *errOut = errno;
                return -1;
            }
            if (pr == 0) {
                continue;
            }
            if (!m.current(gen)) {
                return -2;
            }
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) {
                *errOut = errno;
                return -1;
            }
            if (soerr != 0) {
                *errOut = soerr;
                return -1;
            }
            break;
        }
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    setTimeval(fd, SO_RCVTIMEO, kHttpTimeoutMs);
    setTimeval(fd, SO_SNDTIMEO, kHttpTimeoutMs);
    return 0;
}

struct HttpResult {
    enum class Kind { Ok, Denied, Timeout, Stopped, ConnectFailed, Bad } kind = Kind::Bad;
    int status = -1;
    int errNo = 0;
    QByteArray body;
};

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x4000
#endif

bool sendAll(int fd, const QByteArray& bytes)
{
    int off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.constData() + off, static_cast<size_t>(bytes.size() - off), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<int>(n);
    }
    return true;
}

int parseHttpStatus(const QByteArray& raw)
{
    if (!raw.startsWith("HTTP/")) {
        return -1;
    }
    const int sp = raw.indexOf(' ');
    if (sp < 0 || sp + 1 >= raw.size()) {
        return -1;
    }
    int end = raw.indexOf(' ', sp + 1);
    const int cr = raw.indexOf('\r', sp + 1);
    if (end < 0 || (cr >= 0 && cr < end)) {
        end = cr;
    }
    if (end < 0) {
        return -1;
    }
    bool ok = false;
    const int code = raw.mid(sp + 1, end - (sp + 1)).toInt(&ok);
    return ok ? code : -1;
}

QByteArray parseHttpBody(const QByteArray& raw)
{
    const int p = raw.indexOf("\r\n\r\n");
    if (p < 0) {
        return {};
    }
    return raw.mid(p + 4);
}

bool bodyDenies(const QByteArray& body)
{
    if (body.contains("--operator")) {
        return true;
    }
    const QByteArray lower = body.toLower();
    // 「Access is denied.」是 Windows 系統訊息，中間有 is，對不到 "access denied"。
    return lower.contains("access denied") || lower.contains("access is denied");
}

HttpResult localApi(Monitor& m, quint64 gen, const char* method, const QByteArray& target, Endpoint* ep)
{
    HttpResult out;
    // 整個請求（含改試下一個 socket）共用 5 秒，不是每一段各 5 秒。
    const auto deadline = clock::now() + std::chrono::milliseconds(kHttpTimeoutMs);
    QString tryPaths[3];
    int tries = 0;
    auto addPath = [&](const QString& p) {
        if (p.isEmpty()) {
            return;
        }
        for (int i = 0; i < tries; ++i) {
            if (tryPaths[i] == p) {
                return;
            }
        }
        if (tries < 3) {
            tryPaths[tries++] = p;
        }
    };
    // 上次連成功的路徑先試，失敗再退回兩個常見位置。
    addPath(ep->sock);
    addPath(QStringLiteral("/run/tailscale/tailscaled.sock"));
    addPath(QStringLiteral("/var/run/tailscale/tailscaled.sock"));

    int lastErr = 0;
    bool sawTimeout = false;
    bool connected = false;
    OwnedFd owned;
    for (int i = 0; i < tries; ++i) {
        if (!m.current(gen)) {
            out.kind = HttpResult::Kind::Stopped;
            return out;
        }
        if (clock::now() >= deadline) {
            sawTimeout = true;
            break;
        }
        int err = 0;
        const int fd = openUnixSocket(&err);
        if (fd < 0) {
            lastErr = err;
            continue;
        }
        // 先登記再 connect，stop() 才能 shutdown 把 poll 叫醒。
        OwnedFd attempt;
        attempt.owner = &m;
        attempt.fd = fd;
        if (!m.adoptFd(fd, gen)) {
            out.kind = HttpResult::Kind::Stopped;
            return out;
        }
        const int cst = connectUnix(fd, tryPaths[i], m, gen, deadline, &err);
        if (cst == -2) {
            out.kind = HttpResult::Kind::Stopped;
            return out;
        }
        if (cst != 0) {
            lastErr = err;
            if (err == ETIMEDOUT) {
                sawTimeout = true;
            }
            continue;
        }
        owned.fd = attempt.fd;
        owned.owner = attempt.owner;
        attempt.fd = -1;
        attempt.owner = nullptr;
        ep->sock = tryPaths[i];
        connected = true;
        break;
    }
    if (!connected) {
        // 路徑不存在或權限不夠：這次串流結束。逾時（tailscaled 忙、socket 卡住）下一個 5 秒再試。
        if (sawTimeout) {
            out.kind = HttpResult::Kind::Timeout;
        } else {
            out.kind = HttpResult::Kind::ConnectFailed;
            out.errNo = lastErr;
        }
        return out;
    }

    QByteArray req;
    req += method;
    req += ' ';
    req += target;
    req += " HTTP/1.0\r\nHost: local-tailscaled.sock\r\nConnection: close\r\n";
    if (std::strcmp(method, "POST") == 0) {
        req += "Content-Length: 0\r\n";
    }
    req += "\r\n";
    if (!sendAll(owned.fd, req)) {
        if (!m.current(gen)) {
            out.kind = HttpResult::Kind::Stopped;
        } else {
            out.kind = HttpResult::Kind::Timeout;
        }
        return out;
    }

    QByteArray raw;
    raw.reserve(4096);
    bool timedOut = false;
    char buf[4096];
    while (raw.size() < kMaxHttpBody) {
        if (!m.current(gen)) {
            out.kind = HttpResult::Kind::Stopped;
            out.body = parseHttpBody(raw);
            out.status = parseHttpStatus(raw);
            return out;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        setTimeval(owned.fd, SO_RCVTIMEO, static_cast<int>(left > kHttpTimeoutMs ? kHttpTimeoutMs : left));
        const ssize_t n = ::recv(owned.fd, buf, sizeof(buf), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!m.current(gen)) {
                out.kind = HttpResult::Kind::Stopped;
                out.body = parseHttpBody(raw);
                out.status = parseHttpStatus(raw);
                return out;
            }
            timedOut = (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT);
            break;
        }
        raw.append(buf, static_cast<int>(n));
    }

    out.status = parseHttpStatus(raw);
    out.body = parseHttpBody(raw);
    if (!m.current(gen)) {
        out.kind = HttpResult::Kind::Stopped;
        return out;
    }
    if (out.status == 403 || (out.status <= 0 && bodyDenies(raw))) {
        out.kind = HttpResult::Kind::Denied;
        return out;
    }
    if (out.status <= 0 && timedOut) {
        out.kind = HttpResult::Kind::Timeout;
        return out;
    }
    if (out.status <= 0) {
        out.kind = HttpResult::Kind::Bad;
        return out;
    }
    out.kind = HttpResult::Kind::Ok;
    return out;
}

QString httpSummary(const HttpResult& http)
{
    switch (http.kind) {
    case HttpResult::Kind::Ok:
        if (http.body.isEmpty()) {
            return QStringLiteral("http %1").arg(http.status);
        }
        return QStringLiteral("http %1: %2").arg(http.status).arg(oneLine(http.body, 160));
    case HttpResult::Kind::Denied:
        return QStringLiteral("http 403");
    case HttpResult::Kind::Timeout:
        return QStringLiteral("timed out");
    case HttpResult::Kind::Stopped:
        return QStringLiteral("stopped");
    case HttpResult::Kind::ConnectFailed:
        return QStringLiteral("connect failed (%1)").arg(errnoText(http.errNo));
    case HttpResult::Kind::Bad:
        return QStringLiteral("bad response");
    }
    return QStringLiteral("bad response");
}

bool fillPeer(const QByteArray& body, const QString& host, Snapshot* snap, QString* error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("status JSON parse failed");
        return false;
    }
    const QJsonObject peers = doc.object().value(QStringLiteral("Peer")).toObject();
    for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject peer = it.value().toObject();
        const QJsonArray ips = peer.value(QStringLiteral("TailscaleIPs")).toArray();
        bool match = false;
        for (const QJsonValue& v : ips) {
            if (sameIp(v.toString(), host)) {
                match = true;
                break;
            }
        }
        if (!match) {
            continue;
        }
        snap->curAddr = peer.value(QStringLiteral("CurAddr")).toString().trimmed();
        snap->relay = peer.value(QStringLiteral("Relay")).toString().trimmed();
        snap->hostName = peer.value(QStringLiteral("HostName")).toString().trimmed();
        snap->kind = snap->curAddr.isEmpty() ? Snapshot::Kind::Relay : Snapshot::Kind::Direct;
        return true;
    }
    snap->kind = Snapshot::Kind::Missing;
    return true;
}

ReadResult readStatus(Monitor& m, quint64 gen, const QString& host, Endpoint* ep)
{
    ReadResult r;
    const HttpResult http = localApi(m, gen, "GET", QByteArrayLiteral("/localapi/v0/status"), ep);
    if (http.kind == HttpResult::Kind::Stopped) {
        // 呼叫端用 current() 分辨是 stop。這裡不記 log。
        r.kind = ReadKind::Transient;
        return r;
    }
    if (http.kind == HttpResult::Kind::ConnectFailed) {
        r.kind = ReadKind::Unavailable;
        r.error = QStringLiteral("cannot reach tailscaled (/run/tailscale/tailscaled.sock, /var/run/tailscale/tailscaled.sock; %1); path recovery off this stream")
                      .arg(errnoText(http.errNo));
        ep->sock.clear();
        return r;
    }
    if (http.kind == HttpResult::Kind::Denied) {
        r.kind = ReadKind::Unavailable;
        r.error = QStringLiteral("status http 403; path recovery off this stream");
        return r;
    }
    if (http.kind != HttpResult::Kind::Ok || http.status != 200) {
        r.kind = ReadKind::Transient;
        r.error = QStringLiteral("status read failed (%1)").arg(httpSummary(http));
        return r;
    }
    QString error;
    if (!fillPeer(http.body, host, &r.snap, &error)) {
        r.kind = ReadKind::Transient;
        r.error = error;
        return r;
    }
    r.kind = ReadKind::Ok;
    return r;
}

ActionResult doPing(Monitor& m, quint64 gen, const QString& host, Endpoint* ep)
{
    const QByteArray target = QByteArrayLiteral("/localapi/v0/ping?ip=")
        + QUrl::toPercentEncoding(host)
        + QByteArrayLiteral("&type=disco");
    const HttpResult http = localApi(m, gen, "POST", target, ep);
    ActionResult r;
    r.issued = http.kind != HttpResult::Kind::ConnectFailed && http.kind != HttpResult::Kind::Stopped;
    r.denied = false;
    r.summary = httpSummary(http);
    if (http.kind == HttpResult::Kind::Stopped) {
        r.issued = false;
    }
    return r;
}

ActionResult doDebug(Monitor& m, quint64 gen, const char* action, Endpoint* ep)
{
    const QByteArray target = QByteArrayLiteral("/localapi/v0/debug?action=") + action;
    const HttpResult http = localApi(m, gen, "POST", target, ep);
    ActionResult r;
    r.denied = http.kind == HttpResult::Kind::Denied;
    r.issued = http.kind != HttpResult::Kind::ConnectFailed && http.kind != HttpResult::Kind::Stopped;
    r.summary = httpSummary(http);
    if (http.kind == HttpResult::Kind::Stopped) {
        r.issued = false;
    }
    return r;
}

#elif defined(Q_OS_WIN)

QString tailscaleExePath()
{
    QString pf = qEnvironmentVariable("ProgramFiles");
    if (pf.isEmpty()) {
        pf = QStringLiteral("C:\\Program Files");
    }
    return QDir(pf).filePath(QStringLiteral("Tailscale/tailscale.exe"));
}

bool outputDenied(const QByteArray& all)
{
    if (all.contains("--operator")) {
        return true;
    }
    const QByteArray lower = all.toLower();
    return lower.contains("access denied") || lower.contains("access is denied");
}

struct ProcResult {
    enum class Kind { Finished, Failed, Stopped } kind = Kind::Failed;
    bool launched = false;
    int exitCode = -1;
    QByteArray stdoutBytes;
    QByteArray stderrBytes;
    QByteArray combined() const { return stdoutBytes + stderrBytes; }
};

// CommandLineToArgvW 的引號規則。tailscale.exe 在 "Program Files" 底下，路徑一定要加引號。
QString quoteWinArg(const QString& arg)
{
    const bool plain = !arg.isEmpty()
        && !arg.contains(QLatin1Char(' '))
        && !arg.contains(QLatin1Char('\t'))
        && !arg.contains(QLatin1Char('"'));
    if (plain) {
        return arg;
    }
    QString out;
    out.reserve(arg.size() + 8);
    out.append(QLatin1Char('"'));
    int bs = 0;
    for (const QChar c : arg) {
        if (c == QLatin1Char('\\')) {
            ++bs;
            continue;
        }
        if (c == QLatin1Char('"')) {
            out.append(QString(bs * 2 + 1, QLatin1Char('\\')));
            out.append(c);
            bs = 0;
            continue;
        }
        if (bs > 0) {
            out.append(QString(bs, QLatin1Char('\\')));
            bs = 0;
        }
        out.append(c);
    }
    if (bs > 0) {
        out.append(QString(bs * 2, QLatin1Char('\\')));
    }
    out.append(QLatin1Char('"'));
    return out;
}

struct WinHandle {
    HANDLE h = nullptr;
    explicit WinHandle(HANDLE handle = nullptr) : h(handle) {}
    ~WinHandle() { reset(); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept : h(other.h) { other.h = nullptr; }
    WinHandle& operator=(WinHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            h = other.h;
            other.h = nullptr;
        }
        return *this;
    }
    void reset(HANDLE next = nullptr)
    {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
        h = next;
    }
    HANDLE get() const { return h; }
};

struct AttrList {
    LPPROC_THREAD_ATTRIBUTE_LIST p = nullptr;
    ~AttrList()
    {
        if (p != nullptr) {
            DeleteProcThreadAttributeList(p);
        }
    }
};

// 等待期間把 pipe 讀乾。只在行程結束後才讀，buffer 滿了子行程會卡死在 WriteFile。
void drainPipe(HANDLE h, QByteArray* dest)
{
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return;
    }
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
            return;
        }
        char buf[4096];
        const DWORD want = avail > sizeof(buf) ? static_cast<DWORD>(sizeof(buf)) : avail;
        DWORD n = 0;
        if (!ReadFile(h, buf, want, &n, nullptr) || n == 0) {
            return;
        }
        dest->append(buf, static_cast<int>(n));
    }
}

// 不用 QProcess。CREATE_NO_WINDOW 避免 console 子系統的 tailscale.exe 閃出主控台。
// stdout／stderr 走 pipe，等待時把資料讀走。每 100 ms 看一次停止旗標，合計 10 秒。
ProcResult runTailscale(Monitor& m, quint64 gen, const QString& exe, const QStringList& args)
{
    ProcResult out;
    if (!m.current(gen)) {
        out.kind = ProcResult::Kind::Stopped;
        return out;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rawOutR = nullptr;
    HANDLE rawOutW = nullptr;
    HANDLE rawErrR = nullptr;
    HANDLE rawErrW = nullptr;
    if (!CreatePipe(&rawOutR, &rawOutW, &sa, 65536) || !CreatePipe(&rawErrR, &rawErrW, &sa, 65536)) {
        if (rawOutR != nullptr) {
            CloseHandle(rawOutR);
        }
        if (rawOutW != nullptr) {
            CloseHandle(rawOutW);
        }
        if (rawErrR != nullptr) {
            CloseHandle(rawErrR);
        }
        if (rawErrW != nullptr) {
            CloseHandle(rawErrW);
        }
        out.kind = ProcResult::Kind::Failed;
        out.stderrBytes = "CreatePipe failed";
        return out;
    }
    WinHandle outRead(rawOutR);
    WinHandle outWrite(rawOutW);
    WinHandle errRead(rawErrR);
    WinHandle errWrite(rawErrW);
    // 父行程的讀端不要繼承出去。
    SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0);

    WinHandle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (nul.get() == nullptr || nul.get() == INVALID_HANDLE_VALUE) {
        out.kind = ProcResult::Kind::Failed;
        out.stderrBytes = "open NUL failed";
        return out;
    }

    QString cmd = quoteWinArg(exe);
    for (const QString& a : args) {
        cmd.append(QLatin1Char(' '));
        cmd.append(quoteWinArg(a));
    }
    std::wstring mutableCmd = cmd.toStdWString();
    const std::wstring app = exe.toStdWString();

    QByteArray attrBuf;
    AttrList attr;
    bool haveList = false;
    SIZE_T attrBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
    if (attrBytes > 0 && attrBytes < 65536) {
        attrBuf.resize(static_cast<int>(attrBytes));
        if (InitializeProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data()), 1, 0, &attrBytes)) {
            attr.p = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
            HANDLE inherit[3] = {nul.get(), outWrite.get(), errWrite.get()};
            if (UpdateProcThreadAttribute(attr.p, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inherit, sizeof(inherit), nullptr, nullptr)) {
                haveList = true;
            } else {
                DeleteProcThreadAttributeList(attr.p);
                attr.p = nullptr;
            }
        }
    }

    STARTUPINFOEXW six{};
    six.StartupInfo.cb = haveList ? sizeof(six) : sizeof(STARTUPINFOW);
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdInput = nul.get();
    six.StartupInfo.hStdOutput = outWrite.get();
    six.StartupInfo.hStdError = errWrite.get();
    six.lpAttributeList = haveList ? attr.p : nullptr;

    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | (haveList ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL created = CreateProcessW(
        app.c_str(),
        &mutableCmd[0],
        nullptr,
        nullptr,
        TRUE,
        flags,
        nullptr,
        nullptr,
        &six.StartupInfo,
        &pi);
    // 先留下錯誤碼。後面關 handle 會把它蓋掉。
    const DWORD createErr = created ? 0 : GetLastError();
    // 屬性清單用完就可以拆。子行程已經複製到自己的 handle。
    if (attr.p != nullptr) {
        DeleteProcThreadAttributeList(attr.p);
        attr.p = nullptr;
    }
    // 父行程關掉寫端，子行程結束後讀端才看得到 EOF。讀的時候也不會把 pipe 留滿。
    outWrite.reset();
    errWrite.reset();
    nul.reset();

    if (!created) {
        out.kind = ProcResult::Kind::Failed;
        out.launched = false;
        out.stderrBytes = QByteArray("CreateProcess failed ")
            + QByteArray::number(static_cast<quint32>(createErr));
        return out;
    }
    out.launched = true;
    WinHandle process(pi.hProcess);
    WinHandle threadHandle(pi.hThread);

    auto takeExit = [&]() {
        DWORD code = 0;
        if (!GetExitCodeProcess(process.get(), &code)) {
            out.exitCode = -1;
            return;
        }
        out.exitCode = static_cast<int>(code);
    };
    auto killAndFinish = [&](ProcResult::Kind kind) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 1000);
        drainPipe(outRead.get(), &out.stdoutBytes);
        drainPipe(errRead.get(), &out.stderrBytes);
        takeExit();
        out.kind = kind;
    };

    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        drainPipe(outRead.get(), &out.stdoutBytes);
        drainPipe(errRead.get(), &out.stderrBytes);
        const ULONGLONG elapsed = GetTickCount64() - t0;
        if (elapsed >= static_cast<ULONGLONG>(kProcTimeoutMs)) {
            killAndFinish(m.current(gen) ? ProcResult::Kind::Failed : ProcResult::Kind::Stopped);
            return out;
        }
        if (!m.current(gen)) {
            killAndFinish(ProcResult::Kind::Stopped);
            return out;
        }
        int remain = kProcTimeoutMs - static_cast<int>(elapsed);
        if (remain < 1) {
            remain = 1;
        }
        const int slice = remain < kProcSliceMs ? remain : kProcSliceMs;
        const DWORD wr = WaitForSingleObject(process.get(), static_cast<DWORD>(slice));
        if (wr == WAIT_OBJECT_0) {
            break;
        }
        if (wr == WAIT_FAILED) {
            killAndFinish(m.current(gen) ? ProcResult::Kind::Failed : ProcResult::Kind::Stopped);
            return out;
        }
    }

    drainPipe(outRead.get(), &out.stdoutBytes);
    drainPipe(errRead.get(), &out.stderrBytes);
    takeExit();
    out.kind = ProcResult::Kind::Finished;
    return out;
}

QString procSummary(const ProcResult& pr)
{
    const QByteArray all = pr.combined();
    const QString brief = oneLine(all, 160);
    if (pr.kind == ProcResult::Kind::Stopped) {
        return QStringLiteral("stopped");
    }
    if (outputDenied(all)) {
        return QStringLiteral("exit %1 access denied").arg(pr.exitCode);
    }
    if (pr.kind == ProcResult::Kind::Failed) {
        if (brief.isEmpty()) {
            return QStringLiteral("timed out or failed to start");
        }
        return QStringLiteral("failed: %1").arg(brief);
    }
    if (brief.isEmpty()) {
        return QStringLiteral("exit %1").arg(pr.exitCode);
    }
    return QStringLiteral("exit %1: %2").arg(pr.exitCode).arg(brief);
}

ReadResult readStatus(Monitor& m, quint64 gen, const QString& host, Endpoint* ep)
{
    ReadResult r;
    const ProcResult pr = runTailscale(m, gen, ep->exe, {QStringLiteral("status"), QStringLiteral("--json")});
    if (pr.kind == ProcResult::Kind::Stopped) {
        r.kind = ReadKind::Transient;
        return r;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(pr.stdoutBytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        r.kind = ReadKind::Transient;
        r.error = pr.stdoutBytes.isEmpty()
            ? QStringLiteral("status read failed (%1)").arg(procSummary(pr))
            : QStringLiteral("status JSON parse failed");
        return r;
    }
    const QJsonObject peers = doc.object().value(QStringLiteral("Peer")).toObject();
    for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject peer = it.value().toObject();
        const QJsonArray ips = peer.value(QStringLiteral("TailscaleIPs")).toArray();
        bool match = false;
        for (const QJsonValue& v : ips) {
            if (sameIp(v.toString(), host)) {
                match = true;
                break;
            }
        }
        if (!match) {
            continue;
        }
        r.snap.curAddr = peer.value(QStringLiteral("CurAddr")).toString().trimmed();
        r.snap.relay = peer.value(QStringLiteral("Relay")).toString().trimmed();
        r.snap.hostName = peer.value(QStringLiteral("HostName")).toString().trimmed();
        r.snap.kind = r.snap.curAddr.isEmpty() ? Snapshot::Kind::Relay : Snapshot::Kind::Direct;
        r.kind = ReadKind::Ok;
        return r;
    }
    r.snap.kind = Snapshot::Kind::Missing;
    r.kind = ReadKind::Ok;
    return r;
}

ActionResult doPing(Monitor& m, quint64 gen, const QString& host, Endpoint* ep)
{
    const ProcResult pr = runTailscale(m, gen, ep->exe,
                                       {QStringLiteral("ping"), QStringLiteral("--c"), QStringLiteral("3"),
                                        QStringLiteral("--timeout"), QStringLiteral("3s"), host});
    ActionResult r;
    r.denied = false;
    r.issued = pr.launched && pr.kind != ProcResult::Kind::Stopped;
    r.summary = procSummary(pr);
    return r;
}

ActionResult doDebug(Monitor& m, quint64 gen, const char* action, Endpoint* ep)
{
    const ProcResult pr = runTailscale(m, gen, ep->exe,
                                       {QStringLiteral("debug"), QString::fromLatin1(action)});
    ActionResult r;
    r.denied = outputDenied(pr.combined());
    r.issued = pr.launched && pr.kind != ProcResult::Kind::Stopped;
    r.summary = procSummary(pr);
    return r;
}

#else

ReadResult readStatus(Monitor& m, quint64 gen, const QString& host, Endpoint* ep)
{
    Q_UNUSED(m);
    Q_UNUSED(gen);
    Q_UNUSED(host);
    Q_UNUSED(ep);
    ReadResult r;
    r.kind = ReadKind::Unavailable;
    r.error = QStringLiteral("path recovery is not implemented on this platform; off this stream");
    return r;
}

ActionResult doPing(Monitor& m, quint64 gen, const QString& host, Endpoint* ep)
{
    Q_UNUSED(m);
    Q_UNUSED(gen);
    Q_UNUSED(host);
    Q_UNUSED(ep);
    ActionResult r;
    r.summary = QStringLiteral("not implemented");
    return r;
}

ActionResult doDebug(Monitor& m, quint64 gen, const char* action, Endpoint* ep)
{
    Q_UNUSED(m);
    Q_UNUSED(gen);
    Q_UNUSED(action);
    Q_UNUSED(ep);
    ActionResult r;
    r.summary = QStringLiteral("not implemented");
    return r;
}

#endif

bool Monitor::watch(const QString& hostAddress, quint64 gen)
{
    Endpoint ep;
#if defined(Q_OS_WIN)
    ep.exe = tailscaleExePath();
    if (!QFile::exists(ep.exe)) {
        tsLog(QStringLiteral("tailscale.exe not found at %1; path recovery off this stream").arg(ep.exe));
        return true;
    }
#endif
    if (!current(gen)) {
        return false;
    }
    tsLog(QStringLiteral("watching %1").arg(hostAddress));

    enum class Seen { Unknown, Direct, Relay, Missing } seen = Seen::Unknown;
    struct Streak {
        clock::time_point since{};
        bool didPing = false;
        bool didRestun = false;
        bool didRebind = false;
    } streak;
    int rebindCount = 0;
    bool privDenied = false;
    bool capLogged = false;
    bool haveLastRebind = false;
    clock::time_point lastRebind{};
    bool transientLogged = false;

    auto resetStreak = [&]() {
        streak = {};
    };

    while (current(gen)) {
        const ReadResult read = readStatus(*this, gen, hostAddress, &ep);
        if (!current(gen)) {
            break;
        }
        if (read.kind == ReadKind::Unavailable) {
            if (!read.error.isEmpty()) {
                tsLog(read.error);
            }
            return true;
        }
        if (read.kind == ReadKind::Transient) {
            if (!transientLogged && !read.error.isEmpty()) {
                transientLogged = true;
                tsLog(read.error);
            }
        } else {
            transientLogged = false;
            const Snapshot& snap = read.snap;
            if (snap.kind == Snapshot::Kind::Direct) {
                if (seen == Seen::Relay) {
                    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - streak.since).count();
                    tsLog(QStringLiteral("direct: peer %1 CurAddr=%2 (relay lasted %3 s)")
                              .arg(hostAddress, snap.curAddr)
                              .arg(sec));
                } else if (seen != Seen::Direct) {
                    tsLog(QStringLiteral("direct: peer %1 CurAddr=%2").arg(hostAddress, snap.curAddr));
                }
                seen = Seen::Direct;
                resetStreak();
            } else if (snap.kind == Snapshot::Kind::Missing) {
                if (seen != Seen::Missing) {
                    tsLog(QStringLiteral("no peer with TailscaleIP %1").arg(hostAddress));
                }
                seen = Seen::Missing;
                resetStreak();
            } else {
                const auto now = clock::now();
                if (seen != Seen::Relay) {
                    streak = {};
                    streak.since = now;
                    seen = Seen::Relay;
                    const QString relayShown = snap.relay.isEmpty() ? QStringLiteral("-") : snap.relay;
                    if (snap.hostName.isEmpty()) {
                        tsLog(QStringLiteral("relay: peer %1 CurAddr empty relay=%2").arg(hostAddress, relayShown));
                    } else {
                        tsLog(QStringLiteral("relay: peer %1 host=%2 CurAddr empty relay=%3")
                                  .arg(hostAddress, snap.hostName, relayShown));
                    }
                } else {
                    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - streak.since).count();

                    auto ping = [&]() -> bool {
                        const ActionResult r = doPing(*this, gen, hostAddress, &ep);
                        if (!current(gen)) {
                            return false;
                        }
                        tsLog(QStringLiteral("disco ping %1 (relay %2 s) -> %3").arg(hostAddress).arg(sec).arg(r.summary));
                        return true;
                    };
                    auto rebind = [&]() -> bool {
                        if (privDenied) {
                            return true;
                        }
                        if (rebindCount >= kMaxRebinds) {
                            if (!capLogged) {
                                capLogged = true;
                                tsLog(QStringLiteral("rebind cap reached (%1 this stream); not rebinding again").arg(kMaxRebinds));
                            }
                            return true;
                        }
                        const int n = rebindCount + 1;
                        const ActionResult r = doDebug(*this, gen, "rebind", &ep);
                        if (!current(gen)) {
                            return false;
                        }
                        tsLog(QStringLiteral("rebind (relay %1 s, %2/%3) -> %4")
                                  .arg(sec)
                                  .arg(n)
                                  .arg(kMaxRebinds)
                                  .arg(withDenialHint(r)));
                        if (r.denied) {
                            privDenied = true;
                        }
                        if (r.issued) {
                            rebindCount = n;
                        }
                        lastRebind = clock::now();
                        haveLastRebind = true;
                        return true;
                    };

                    if (!streak.didPing && sec >= kPingSec) {
                        streak.didPing = true;
                        if (!ping()) {
                            break;
                        }
                    }
                    if (!current(gen)) {
                        break;
                    }
                    if (!streak.didRestun && sec >= kRestunSec) {
                        streak.didRestun = true;
                        if (!privDenied) {
                            const ActionResult r = doDebug(*this, gen, "restun", &ep);
                            if (!current(gen)) {
                                break;
                            }
                            tsLog(QStringLiteral("restun (relay %1 s) -> %2").arg(sec).arg(withDenialHint(r)));
                            if (r.denied) {
                                privDenied = true;
                            }
                        }
                        if (!ping()) {
                            break;
                        }
                    }
                    if (!current(gen)) {
                        break;
                    }
                    if (!streak.didRebind && sec >= kRebindSec) {
                        streak.didRebind = true;
                        if (!rebind()) {
                            break;
                        }
                        if (!ping()) {
                            break;
                        }
                    }
                    if (!current(gen)) {
                        break;
                    }
                    if (streak.didRebind && !privDenied && haveLastRebind
                        && clock::now() - lastRebind >= std::chrono::seconds(kRebindGapSec)) {
                        if (!rebind()) {
                            break;
                        }
                    }
                }
            }
        }
        if (!waitInterval(gen)) {
            break;
        }
    }
    return false;
}

} // namespace

void TailscalePath::start(const QString& hostAddress)
{
    const QString ip = tailscaleHost(hostAddress);
    if (ip.isEmpty()) {
        return;
    }
    monitor().start(ip);
}

void TailscalePath::stop()
{
    monitor().stop();
}
