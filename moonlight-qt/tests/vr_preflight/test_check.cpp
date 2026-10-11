// §VR-PREFLIGHT：對真的 VrPreflightCheck／VrRttProbe（app/backend/vrpreflightcheck.cpp）與 VrLauncher 的測試。
//
// 需要 Qt（Core＋Network），不需要 QtTest 也不開視窗；建置與執行方式見同目錄的 run_tests.sh。
// 用法：test_check <moonlight-qt 目錄>
//
// 這裡驗得到的：
//   - 對本機的 TCP listener 真的量一次：連得上（快）、沒有人在聽（量不到→照常開始）、位址是主機名稱、IPv6、
//     不去問 proxy 設定、GUI 執行緒卡住時量到的時間不會被拖長、量到一半取消。
//   - 太慢的那幾條路（等待、自動開始、仍要開始、取消、作廢的結果不算）：往返時間用 --vr-test-preflight 的腳本
//     給，計時器、訊號、log 都是產品的程式碼在跑。
//   - VrPreflightDialog.qml 每一句話在 qml_zh_TW.qm 裡都有翻譯；QML 用到的 VrLauncher 成員在真的類別與
//     QML 測試的替身裡都存在。
// 驗不到的：
//   - 真的網路上「往返超過門檻」的量測。本機 loopback 的握手一定是幾毫秒以內；延後 accept() 不會讓握手變慢
//     （核心自己回 SYN-ACK），所以這裡沒有用它假裝。
//   - VrLauncher.launch() 之後真的走到開子行程：非 Linux／沒有 OpenXR 的建置上 VR 模式開不起來（blockedReason），
//     這一段只編譯、連結、檢查名稱。

#include "vrlauncher.h"
#include "vrpreflightcheck.h"

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTranslator>

#include <cstdio>
#include <functional>

// ---- vrlauncher.cpp 需要、但定義在 main.cpp／sfenv.cpp 的東西（這個測試不連那兩個檔）----

void singleInstanceReleaseForChild() {}
bool singleInstanceReacquire() { return true; }

namespace SfEnv {
bool originalValue(const char*, QByteArray*) { return false; }
}

// ---- log 攔截（stubs/SDL_compat.h）----

static QStringList g_Log;

void testLogLine(const char* level, const char* line)
{
    g_Log << QString::fromUtf8(line);
    printf("    [%s] %s\n", level, line);
    fflush(stdout);
}

namespace {

int g_Checks = 0;
int g_Failures = 0;
const char* g_Case = "";

void check(bool ok, const char* expr, int line)
{
    g_Checks++;
    if (!ok) {
        g_Failures++;
        printf("FAIL %s (line %d): %s\n", g_Case, line, expr);
        fflush(stdout);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void beginCase(const char* name)
{
    g_Case = name;
    g_Log.clear();
    printf("case: %s\n", name);
    fflush(stdout);
}

const QString kPrefix = QStringLiteral("[VIPLE-VR-LAUNCHER] preflight: ");

int logCount(const QString& needle)
{
    int n = 0;
    for (const QString& l : std::as_const(g_Log)) {
        if (l.contains(needle)) {
            n++;
        }
    }
    return n;
}

bool logHas(const QString& needle)
{
    return logCount(needle) > 0;
}

// log 裡第一個符合的數字（例：sample 1/3: (\d+) ms）；沒有回 -1
int logNumber(const QString& pattern)
{
    const QRegularExpression re(pattern);
    for (const QString& l : std::as_const(g_Log)) {
        const QRegularExpressionMatch m = re.match(l);
        if (m.hasMatch()) {
            return m.captured(1).toInt();
        }
    }
    return -1;
}

// 跑事件迴圈直到 cond 成立或逾時。用「等到有事件才醒」的方式，事件一到就處理（跟 GUI 的事件迴圈一樣），
// 量到的延遲才不會混進這個測試自己輪詢的間隔；另外每 5 ms 有一個計時器把迴圈叫醒去看逾時
bool waitFor(const std::function<bool()>& cond, int timeoutMs)
{
    QTimer wake;
    wake.start(5);
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
    }
    return true;
}

void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    waitFor([&]() { return t.elapsed() >= ms; }, ms + 1000);
}

// 開頭那幾次量測的 log 有幾行（"launch: … (sample 1 of 3, …)" 那一行不算）
int sampleLines()
{
    return logCount(QStringLiteral("preflight: sample "));
}

// 接上一個 VrPreflightCheck 的三個訊號
struct Watch {
    int proceed = 0;
    int cancelled = 0;
    int changed = 0;
    int maxStreak = 0;
    bool sawWaiting = false;
    bool sawChecking = false;
    qint64 waitingAtMs = -1;
    qint64 endAtMs = -1;
    QElapsedTimer clock;

    explicit Watch(VrPreflightCheck* c)
    {
        clock.start();
        QObject::connect(c, &VrPreflightCheck::proceed, c, [this]() {
            proceed++;
            endAtMs = clock.elapsed();
        });
        QObject::connect(c, &VrPreflightCheck::cancelled, c, [this]() {
            cancelled++;
            endAtMs = clock.elapsed();
        });
        QObject::connect(c, &VrPreflightCheck::changed, c, [this, c]() {
            changed++;
            sawChecking = sawChecking || c->checking();
            if (c->waiting() && !sawWaiting) {
                sawWaiting = true;
                waitingAtMs = clock.elapsed();
            }
            maxStreak = qMax(maxStreak, c->goodStreak());
        });
    }
};

// 本機的 listener：數有幾條連線進來、收到幾個位元組（應該是 0：檢查不送資料）
struct Listener {
    QTcpServer server;
    int accepted = 0;
    qint64 bytes = 0;

    bool listen(const QHostAddress& address)
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                accepted++;
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s]() { bytes += s->readAll().size(); });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        return server.listen(address, 0);
    }

    int port() const { return server.serverPort(); }
};

// 一個確定沒有人在聽的埠：先聽、記下來、關掉
int closedPort()
{
    QTcpServer s;
    if (!s.listen(QHostAddress::LocalHost, 0)) {
        return 1;
    }
    const int port = s.serverPort();
    s.close();
    return port;
}

// ---- 真的量 ----

void testRealFast()
{
    beginCase("real socket: listener answers -> launch at once");
    Listener listener;
    CHECK(listener.listen(QHostAddress::LocalHost));

    VrPreflightCheck c;
    Watch w(&c);
    CHECK(!c.busy());
    CHECK(c.begin(QStringLiteral("127.0.0.1"), listener.port()));
    CHECK(c.busy());
    CHECK(c.checking());
    CHECK(!c.waiting());
    CHECK(waitFor([&]() { return w.proceed == 1; }, 3000));
    printf("    time from begin to proceed: %lld ms (includes starting the probe thread)\n", static_cast<long long>(w.endAtMs));
    CHECK(w.endAtMs >= 0 && w.endAtMs < 500);
    CHECK(w.cancelled == 0);
    CHECK(!w.sawWaiting);
    CHECK(!c.busy());
    CHECK(logHas(kPrefix + QStringLiteral("checking the round trip to the host (tcp port %1)").arg(listener.port())));
    const int rtt = logNumber(QStringLiteral("sample 1/3: (\\d+) ms"));
    CHECK(rtt >= 0 && rtt <= 20);
    CHECK(c.lastRttMs() == rtt);
    CHECK(logHas(QStringLiteral("launch: %1 ms <= 60 ms (sample 1 of 3").arg(rtt)));
    CHECK(sampleLines() == 1);                              // 第一次就夠快：不再量

    spin(300);
    CHECK(listener.accepted == 1);                          // 只連了一次
    CHECK(listener.bytes == 0);                             // 沒有送任何資料
    CHECK(w.proceed == 1);
}

void testRealSecondRun()
{
    beginCase("real socket: the same object can be used again");
    Listener listener;
    CHECK(listener.listen(QHostAddress::LocalHost));
    VrPreflightCheck c;
    Watch w(&c);
    for (int i = 1; i <= 5; i++) {
        QElapsedTimer t;
        t.start();
        CHECK(c.begin(QStringLiteral("127.0.0.1"), listener.port()));
        CHECK(waitFor([&]() { return w.proceed == i; }, 3000));
        // 夠快的路徑上使用者多等的時間：一次往返，加上交給量測執行緒再回來的時間
        printf("    run %d: begin to proceed %.2f ms\n", i, static_cast<double>(t.nsecsElapsed()) / 1e6);
        CHECK(t.elapsed() < 200);
        CHECK(!c.busy());
    }
    CHECK(logCount(QStringLiteral("launch: ")) == 5);
    spin(200);
    CHECK(listener.accepted == 5);
}

void testRealNobodyListening()
{
    beginCase("real socket: nobody listening -> unmeasured, launch as before");
    const int port = closedPort();
    VrPreflightCheck c;
    Watch w(&c);
    CHECK(c.begin(QStringLiteral("127.0.0.1"), port));
    // Linux 立刻拒絕；Windows 對本機關著的埠要重試約 2 秒才回報，所以每一次都先碰到 1 秒的逾時
    CHECK(waitFor([&]() { return w.proceed == 1; }, 3 * vrpreflight::kSampleTimeoutMs + 1500));
    printf("    time from begin to proceed: %lld ms\n", static_cast<long long>(w.endAtMs));
    CHECK(w.endAtMs <= 3 * vrpreflight::kSampleTimeoutMs + 600);
    CHECK(w.cancelled == 0);
    CHECK(!w.sawWaiting);                                   // 量不到不會跳對話框
    CHECK(logCount(QStringLiteral("no answer (")) == 3);
    CHECK(logHas(kPrefix + QStringLiteral("unmeasured: no answer on 3 samples (")));
    CHECK(logHas(QStringLiteral(") - launching as before")));
    CHECK(c.lastRttMs() == -1);
    CHECK(c.measuredRttMs() == -1);
}

void testRealGuiThreadStall()
{
    beginCase("real socket: a stalled GUI thread does not inflate the measurement");
    Listener listener;
    CHECK(listener.listen(QHostAddress::LocalHost));
    VrPreflightCheck c;
    Watch w(&c);
    // 先跑一次，讓量測執行緒已經起來（第一次含開執行緒的時間）
    CHECK(c.begin(QStringLiteral("127.0.0.1"), listener.port()));
    CHECK(waitFor([&]() { return w.proceed == 1; }, 3000));
    g_Log.clear();

    CHECK(c.begin(QStringLiteral("127.0.0.1"), listener.port()));
    // listener 也在這條執行緒上，但握手由核心完成，不需要它 accept
    QThread::msleep(400);                                   // GUI 執行緒卡住 400 ms（例如同步的 HTTP）
    CHECK(w.proceed == 1);                                  // 結果還沒被處理
    CHECK(waitFor([&]() { return w.proceed == 2; }, 3000));
    const int rtt = logNumber(QStringLiteral("sample 1/3: (\\d+) ms"));
    printf("    measured %d ms while the GUI thread was blocked for 400 ms\n", rtt);
    CHECK(rtt >= 0 && rtt <= 20);                           // 量的是握手，不是 GUI 執行緒什麼時候有空
    CHECK(logHas(QStringLiteral("launch: ")));
}

void testRealHostNameAndIpv6()
{
    beginCase("real socket: host name instead of an address");
    {
        // "localhost" 可能解析成 ::1 或 127.0.0.1，兩邊都聽同一個埠
        Listener v4;
        CHECK(v4.listen(QHostAddress::LocalHost));
        QTcpServer v6;
        const bool haveV6 = v6.listen(QHostAddress::LocalHostIPv6, static_cast<quint16>(v4.port()));
        VrPreflightCheck c;
        Watch w(&c);
        CHECK(c.begin(QStringLiteral("localhost"), v4.port()));
        CHECK(waitFor([&]() { return w.proceed == 1; }, 4000));
        const int rtt = logNumber(QStringLiteral("sample 1/3: (\\d+) ms"));
        printf("    localhost: %d ms (IPv6 listener on the same port: %s)\n", rtt, haveV6 ? "yes" : "no");
        CHECK(rtt >= 0 && rtt <= 20);                       // 查名稱的時間不算在往返裡
    }

    beginCase("real socket: IPv6 literal");
    {
        Listener v6;
        if (!v6.listen(QHostAddress::LocalHostIPv6)) {
            printf("    skipped: no IPv6 loopback on this machine\n");
            return;
        }
        VrPreflightCheck c;
        Watch w(&c);
        CHECK(c.begin(QStringLiteral("::1"), v6.port()));
        CHECK(waitFor([&]() { return w.proceed == 1; }, 3000));
        CHECK(logHas(QStringLiteral("launch: ")));
        spin(200);
        CHECK(v6.accepted == 1);
    }
}

// 記下有沒有人來問「這條連線要走哪個 proxy」。回答一律是不走，所以裝著它不會改變任何連線
class RecordingProxyFactory : public QNetworkProxyFactory
{
public:
    explicit RecordingProxyFactory(QAtomicInt* asked) : m_Asked(asked) {}

    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery&) override
    {
        m_Asked->ref();
        return {QNetworkProxy(QNetworkProxy::NoProxy)};
    }

private:
    QAtomicInt* m_Asked;
};

void testRealIgnoresProxy()
{
    beginCase("real socket: the application proxy settings are not consulted");
    // 目的地要用不是 loopback 的位址：對 loopback，Qt 本來就不問 proxy，測不出差別。用這台機器自己的位址、
    // 一個沒有人在聽的埠——封包不會離開這台機器，也不必開一個對外的 listener
    QString ownAddress;
    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress& a : addresses) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal()) {
            ownAddress = a.toString();
            break;
        }
    }
    if (ownAddress.isEmpty()) {
        printf("    skipped: this machine has no non-loopback IPv4 address\n");
        return;
    }

    QAtomicInt asked;
    QNetworkProxyFactory::setApplicationProxyFactory(new RecordingProxyFactory(&asked));   // Qt 接手這個物件

    // 先確認這個量法抓得到：沒有指定 proxy 的 socket 連同一個位址時，Qt 會來問
    {
        QTcpSocket plain;
        plain.connectToHost(ownAddress, static_cast<quint16>(closedPort()));
        spin(50);
        plain.abort();
    }
    CHECK(asked.loadRelaxed() > 0);
    asked.storeRelaxed(0);

    VrPreflightCheck c;
    Watch w(&c);
    CHECK(c.begin(ownAddress, closedPort()));
    CHECK(waitFor([&]() { return sampleLines() >= 1 || w.proceed == 1; }, 3000));   // 至少量完一次
    c.cancel("the user");
    spin(100);
    QNetworkProxyFactory::setApplicationProxyFactory(nullptr);
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    CHECK(asked.loadRelaxed() == 0);                        // 量測的連線沒有去問 proxy 設定：直接連
    CHECK(sampleLines() >= 1);
}

void testRealCancelMidProbe()
{
    beginCase("real socket: cancel while a probe is in flight");
    const int port = closedPort();
    VrPreflightCheck c;
    Watch w(&c);
    CHECK(c.begin(QStringLiteral("127.0.0.1"), port));
    spin(100);
    if (w.proceed == 0 && c.busy()) {
        c.cancel("the user");
        CHECK(w.cancelled == 1);
        CHECK(!c.busy());
        CHECK(logHas(kPrefix + QStringLiteral("cancel: the user after 0 s")));
        const int logged = g_Log.size();
        const int changed = w.changed;
        spin(vrpreflight::kSampleTimeoutMs + 500);          // 被丟掉的那一次不會再回報、也不會再量
        CHECK(w.proceed == 0);
        CHECK(w.cancelled == 1);
        CHECK(g_Log.size() == logged);
        CHECK(w.changed == changed);
    }
    else {
        // 這台機器對關著的埠立刻回「拒絕」（三次在 100 ms 內就量完了），沒有進行中的量測可以取消
        printf("    skipped: refused connections are reported at once on this machine\n");
    }

    // 取消之後同一個物件照樣能用
    Listener listener;
    CHECK(listener.listen(QHostAddress::LocalHost));
    const int before = w.proceed;
    CHECK(c.begin(QStringLiteral("127.0.0.1"), listener.port()));
    CHECK(waitFor([&]() { return w.proceed == before + 1; }, 3000));
}

void testNoTarget()
{
    beginCase("no target -> launch as before, inside the call");
    VrPreflightCheck c;
    Watch w(&c);
    CHECK(c.begin(QString(), 47989));
    CHECK(w.proceed == 1);                                  // begin() 回來之前就發了
    CHECK(!c.busy());
    CHECK(logHas(kPrefix + QStringLiteral("unmeasured: no address or port to probe - launching as before")));
    CHECK(c.begin(QStringLiteral("127.0.0.1"), 0));
    CHECK(w.proceed == 2);
    CHECK(c.begin(QStringLiteral("127.0.0.1"), 70000));
    CHECK(w.proceed == 3);
    CHECK(c.begin(QStringLiteral("127.0.0.1"), -1));
    CHECK(w.proceed == 4);
    CHECK(w.changed == 0);                                  // 介面上什麼都沒變
    c.startAnyway();                                        // 不在檢查中：不做事
    c.cancel("the user");
    CHECK(w.proceed == 4);
    CHECK(w.cancelled == 0);
}

// ---- 太慢的那幾條路：往返時間用腳本給 ----

void testScriptedAutoStart()
{
    beginCase("scripted 150*3,30: wait, then auto-start after three good probes");
    QString err;
    CHECK(VrPreflightCheck::setTestScript(QStringLiteral("150*3,30"), &err));
    CHECK(logHas(kPrefix + QStringLiteral("dev --vr-test-preflight \"150*3,30\": scripted samples replace the measurement (dev-only, not saved)")));

    VrPreflightCheck c;
    Watch w(&c);
    CHECK(c.begin(QString(), 0));                           // 有腳本時不需要位址
    CHECK(logHas(kPrefix + QStringLiteral("checking the round trip to the host (scripted samples, dev)")));
    CHECK(c.checking());

    CHECK(waitFor([&]() { return w.sawWaiting; }, 2000));
    printf("    waiting entered after %lld ms (expected about 450)\n", static_cast<long long>(w.waitingAtMs));
    CHECK(w.waitingAtMs >= 400 && w.waitingAtMs <= 900);
    CHECK(c.waiting());
    CHECK(!c.checking());
    CHECK(c.measuredRttMs() == 150);
    CHECK(c.lastRttMs() == 150);
    CHECK(c.goodStreak() == 0);
    CHECK(w.proceed == 0);
    CHECK(logHas(kPrefix + QStringLiteral("wait: best of 3 samples is 150 ms > 60 ms (150, 150, 150) - asking the user, probing every 2 s")));

    // 檢查中不接第二次
    CHECK(!c.begin(QStringLiteral("127.0.0.1"), 1));
    CHECK(c.waiting());

    // 等待中每 2 秒量一次：2330、4330、6330 ms 左右各有一次 30 ms，第三次自己開始
    CHECK(waitFor([&]() { return c.goodStreak() == 1; }, 3000));
    CHECK(c.lastRttMs() == 30);
    CHECK(c.measuredRttMs() == 150);                        // 判定太慢時的值不跟著變
    CHECK(c.waiting());
    CHECK(waitFor([&]() { return w.proceed == 1; }, 6000));
    printf("    auto-start after %lld ms (expected about 6330)\n", static_cast<long long>(w.endAtMs));
    CHECK(w.endAtMs >= 5700 && w.endAtMs <= 7200);
    CHECK(w.maxStreak == 3);
    CHECK(!c.busy());
    CHECK(c.goodStreak() == 3);                             // 結束之後保留，對話框淡出時還在顯示
    CHECK(w.cancelled == 0);
    CHECK(logHas(kPrefix + QStringLiteral("probe 1: 30 ms (fast enough 1/3)")));
    CHECK(logHas(kPrefix + QStringLiteral("probe 3: 30 ms (fast enough 3/3)")));
    CHECK(logHas(kPrefix + QStringLiteral("auto-start after 6 s: 60 ms or less on 3 probes in a row (last 30 ms)")));

    const int logged = g_Log.size();
    spin(2500);                                             // 開始之後不再量
    CHECK(g_Log.size() == logged);
    CHECK(w.proceed == 1);
    CHECK(VrPreflightCheck::setTestScript(QString()));
}

void testScriptedOverrideAndCancel()
{
    beginCase("scripted 150: start anyway");
    CHECK(VrPreflightCheck::setTestScript(QStringLiteral("150")));
    {
        VrPreflightCheck c;
        Watch w(&c);
        CHECK(c.begin(QString(), 0));
        CHECK(waitFor([&]() { return c.waiting(); }, 2000));
        spin(300);
        c.startAnyway();
        CHECK(w.proceed == 1);                              // 按下去當下就開
        CHECK(!c.busy());
        CHECK(logHas(kPrefix + QStringLiteral("override: the user chose to start anyway after 1 s (last 150 ms)")));
        const int logged = g_Log.size();
        const int changed = w.changed;
        c.startAnyway();                                    // 連按兩次
        c.cancel("the user");
        spin(2600);                                         // 排好的下一次量測不會再跑
        CHECK(w.proceed == 1);
        CHECK(w.cancelled == 0);
        CHECK(g_Log.size() == logged);
        CHECK(w.changed == changed);
    }

    beginCase("scripted 150: cancel");
    {
        VrPreflightCheck c;
        Watch w(&c);
        CHECK(c.begin(QString(), 0));
        CHECK(waitFor([&]() { return c.waiting(); }, 2000));
        // 等到第一次等待中的量測有結果（對話框上的「最新」會更新）
        CHECK(waitFor([&]() { return logHas(QStringLiteral("probe 1: 150 ms")); }, 3500));
        c.cancel("the user");
        CHECK(w.cancelled == 1);
        CHECK(w.proceed == 0);
        CHECK(!c.busy());
        CHECK(logHas(kPrefix + QStringLiteral("cancel: the user after ")));
        const int logged = g_Log.size();
        spin(2600);
        CHECK(g_Log.size() == logged);
        CHECK(w.proceed == 0);
        CHECK(w.cancelled == 1);

        // 取消之後再開一次：腳本從頭來
        CHECK(c.begin(QString(), 0));
        CHECK(waitFor([&]() { return c.waiting(); }, 2000));
        CHECK(logCount(QStringLiteral("sample 1/3: 150 ms")) == 2);
        c.cancel("mode changed");
        CHECK(logHas(kPrefix + QStringLiteral("cancel: mode changed after ")));
    }
    CHECK(VrPreflightCheck::setTestScript(QString()));
}

void testStaleResultIgnored()
{
    beginCase("a result that arrives after the check ended is ignored");
    CHECK(VrPreflightCheck::setTestScript(QStringLiteral("400")));
    VrPreflightCheck c;
    Watch w(&c);
    CHECK(c.begin(QString(), 0));
    spin(100);
    c.cancel("the user");                                   // 第一次量測（400 ms）還在路上
    CHECK(w.cancelled == 1);

    CHECK(VrPreflightCheck::setTestScript(QStringLiteral("20")));
    g_Log.clear();
    CHECK(c.begin(QString(), 0));
    CHECK(waitFor([&]() { return w.proceed == 1; }, 1000));
    CHECK(logHas(QStringLiteral("sample 1/3: 20 ms")));
    spin(600);                                              // 上一場的 400 ms 在這段時間回來
    CHECK(!logHas(QStringLiteral("400 ms")));
    CHECK(sampleLines() == 1);
    CHECK(w.proceed == 1);
    CHECK(w.cancelled == 1);
    CHECK(!c.busy());
    CHECK(VrPreflightCheck::setTestScript(QString()));
}

void testScriptedUnmeasuredAndBadSpec()
{
    beginCase("scripted x*2,40: two timeouts, then fast");
    CHECK(VrPreflightCheck::setTestScript(QStringLiteral("x*2,40")));
    {
        VrPreflightCheck c;
        Watch w(&c);
        CHECK(c.begin(QString(), 0));
        spin(1300);
        CHECK(c.checking());                                // 介面這時候會顯示「正在檢查連線」
        CHECK(w.proceed == 0);
        CHECK(waitFor([&]() { return w.proceed == 1; }, 2000));
        printf("    proceed after %lld ms (expected about 2040)\n", static_cast<long long>(w.endAtMs));
        CHECK(w.endAtMs >= 1900 && w.endAtMs <= 2700);
        CHECK(!w.sawWaiting);
        CHECK(logHas(QStringLiteral("launch: 40 ms <= 60 ms (sample 3 of 3")));
    }

    beginCase("bad script is rejected and changes nothing");
    QString err;
    CHECK(!VrPreflightCheck::setTestScript(QStringLiteral("150*"), &err));
    CHECK(err.contains(QStringLiteral("expected a comma-separated list")));
    CHECK(!VrPreflightCheck::setTestScript(QStringLiteral("1000")));
    {
        // 上一個腳本（x*2,40）還在
        VrPreflightCheck c;
        Watch w(&c);
        CHECK(c.begin(QString(), 0));
        CHECK(c.busy());
        c.cancel("the user");
    }
    CHECK(VrPreflightCheck::setTestScript(QString()));
    {
        VrPreflightCheck c;
        Watch w(&c);
        CHECK(c.begin(QString(), 0));                       // 腳本關了、又沒有位址：照常開始
        CHECK(w.proceed == 1);
    }
}

void testDestroyWhileBusy()
{
    beginCase("destroying the object while a real probe is in flight");
    const int port = closedPort();
    QElapsedTimer t;
    t.start();
    {
        VrPreflightCheck c;
        Watch w(&c);
        CHECK(c.begin(QStringLiteral("127.0.0.1"), port));
        spin(50);
    }
    printf("    destroyed in %lld ms\n", static_cast<long long>(t.elapsed()));
    CHECK(t.elapsed() < 1000);                              // 不會等量測逾時才結束
    spin(1200);                                             // 之後也沒有東西回頭找已經不在的物件
}

// ---- 翻譯與名稱 ----

QString readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        printf("    cannot read %s\n", qPrintable(path));
        g_Failures++;
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

void testTranslations(const QString& root)
{
    beginCase("every string of VrPreflightDialog.qml is translated in qml_zh_TW.qm");
    QTranslator translator;
    CHECK(translator.load(root + QStringLiteral("/app/languages/qml_zh_TW.qm")));
    const QString qml = readFile(root + QStringLiteral("/app/gui/VrPreflightDialog.qml"));

    const QRegularExpression re(QStringLiteral("qsTr\\(\"((?:[^\"\\\\]|\\\\.)*)\"\\)"));
    QRegularExpressionMatchIterator it = re.globalMatch(qml);
    const QRegularExpression placeholder(QStringLiteral("%\\d"));
    QSet<QString> seen;
    while (it.hasNext()) {
        const QString source = it.next().captured(1);
        if (seen.contains(source)) {
            continue;
        }
        seen.insert(source);
        const QString translated = translator.translate("VrPreflightDialog", source.toUtf8().constData());
        const bool ok = !translated.isEmpty() && translated != source;
        if (!ok) {
            printf("    not translated: %s\n", qPrintable(source));
        }
        CHECK(ok);
        // 佔位符要一樣多（%1、%2）
        QStringList inSource, inTranslated;
        QRegularExpressionMatchIterator p = placeholder.globalMatch(source);
        while (p.hasNext()) {
            inSource << p.next().captured(0);
        }
        p = placeholder.globalMatch(translated);
        while (p.hasNext()) {
            inTranslated << p.next().captured(0);
        }
        inSource.sort();
        inTranslated.sort();
        CHECK(inSource == inTranslated);
    }
    printf("    %d strings checked\n", static_cast<int>(seen.size()));
    CHECK(seen.size() >= 10);
}

bool metaHas(const QMetaObject* mo, const QString& name)
{
    if (mo->indexOfProperty(name.toLatin1().constData()) >= 0) {
        return true;
    }
    for (int i = 0; i < mo->methodCount(); i++) {
        if (QString::fromLatin1(mo->method(i).name()) == name) {
            return true;
        }
    }
    return false;
}

void testNamesMatch(const QString& root)
{
    beginCase("VrLauncher members used from QML exist in the class and in the QML test double");
    VrLauncher launcher;
    const QMetaObject* mo = launcher.metaObject();

    // QML 裡寫的 VrLauncher.<名稱>；AppView.qml 另外把單例存成 launcher 再呼叫 launch
    QSet<QString> used;
    const QRegularExpression re(QStringLiteral("\\b(?:VrLauncher|launcher)\\.([a-z][A-Za-z0-9_]*)"));
    const char* files[] = {"VrPreflightDialog.qml", "AppView.qml", "main.qml", "SettingsView.qml"};
    for (const char* file : files) {
        QRegularExpressionMatchIterator it = re.globalMatch(readFile(root + QStringLiteral("/app/gui/") + QLatin1String(file)));
        while (it.hasNext()) {
            used.insert(it.next().captured(1));
        }
    }
    CHECK(used.contains(QStringLiteral("launch")));
    CHECK(used.contains(QStringLiteral("preflightWaiting")));
    CHECK(used.contains(QStringLiteral("preflightStartAnyway")));

    const QString mock = readFile(root + QStringLiteral("/tests/vr_preflight/qml/imports/VrLauncher/VrLauncher.qml"));
    QStringList sorted = used.values();
    sorted.sort();
    for (const QString& name : std::as_const(sorted)) {
        const bool inClass = metaHas(mo, name);
        const QRegularExpression decl(QStringLiteral("\\b(?:property\\s+\\w+|function|signal)\\s+%1\\b").arg(name));
        const bool inMock = decl.match(mock).hasMatch();
        if (!inClass || !inMock) {
            printf("    %s: class=%d test double=%d\n", qPrintable(name), inClass ? 1 : 0, inMock ? 1 : 0);
        }
        CHECK(inClass);
        CHECK(inMock);
    }
    printf("    %d names checked: %s\n", static_cast<int>(sorted.size()), qPrintable(sorted.join(QStringLiteral(", "))));

    // QML 呼叫的簽章與接的訊號
    CHECK(mo->indexOfMethod("launch(QString,QString,int,QString,bool)") >= 0);
    CHECK(mo->indexOfMethod("preflightStartAnyway()") >= 0);
    CHECK(mo->indexOfMethod("preflightCancel()") >= 0);
    CHECK(mo->indexOfSignal("preflightChanged()") >= 0);
    CHECK(mo->indexOfSignal("runningChanged()") >= 0);
    CHECK(mo->indexOfSignal("finished(int,QString)") >= 0);
    // preflight 的屬性都跟著 preflightChanged 更新（對話框的 binding 靠它）
    const char* live[] = {"preflightChecking", "preflightWaiting", "preflightMeasuredMs", "preflightRttMs", "preflightGoodStreak"};
    for (const char* name : live) {
        const QMetaProperty p = mo->property(mo->indexOfProperty(name));
        CHECK(p.isValid());
        CHECK(p.hasNotifySignal() && QByteArray(p.notifySignal().name()) == "preflightChanged");
    }

    // 沒有在檢查時的值
    CHECK(!launcher.preflightChecking());
    CHECK(!launcher.preflightWaiting());
    CHECK(launcher.preflightMeasuredMs() == -1);
    CHECK(launcher.preflightRttMs() == -1);
    CHECK(launcher.preflightGoodStreak() == 0);
    CHECK(launcher.preflightThresholdMs() == 60);
    CHECK(launcher.preflightGoodNeeded() == 3);
    CHECK(launcher.preflightIntervalSec() == 2);

    // 不在 VR 模式（這個平台也開不了）：launch 不接受、也不會開始檢查；兩個按鈕不做事
    int changed = 0;
    QObject::connect(&launcher, &VrLauncher::preflightChanged, &launcher, [&changed]() { changed++; });
    g_Log.clear();
    CHECK(!launcher.launch(QStringLiteral("127.0.0.1:47989"), QStringLiteral("127.0.0.1"), 47989, QStringLiteral("App"), false));
    launcher.preflightStartAnyway();
    launcher.preflightCancel();
    spin(100);
    CHECK(changed == 0);
    CHECK(!launcher.running());
    CHECK(g_Log.isEmpty());
#if !(defined(Q_OS_LINUX) && defined(HAVE_OPENXR))
    CHECK(!launcher.blockedReason().isEmpty());
    CHECK(!launcher.setVrMode(true));
    CHECK(!launcher.vrMode());
#endif

    // dev 腳本的入口（main.cpp 用的那一個）
    QString err;
    CHECK(!VrLauncher::setTestPreflightScript(QStringLiteral("nope"), &err));
    CHECK(!err.isEmpty());
    CHECK(VrLauncher::setTestPreflightScript(QStringLiteral("150")));
    CHECK(logHas(QStringLiteral("dev --vr-test-preflight \"150\"")));
    CHECK(VrLauncher::setTestPreflightScript(QString()));
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        fprintf(stderr, "usage: test_check <path to the moonlight-qt directory>\n");
        return 2;
    }
    const QString root = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();

    testRealFast();
    testRealSecondRun();
    testRealNobodyListening();
    testRealGuiThreadStall();
    testRealHostNameAndIpv6();
    testRealIgnoresProxy();
    testRealCancelMidProbe();
    testNoTarget();
    testScriptedAutoStart();
    testScriptedOverrideAndCancel();
    testStaleResultIgnored();
    testScriptedUnmeasuredAndBadSpec();
    testDestroyWhileBusy();
    testTranslations(root);
    testNamesMatch(root);

    printf("vr_preflight check: %d checks, %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
