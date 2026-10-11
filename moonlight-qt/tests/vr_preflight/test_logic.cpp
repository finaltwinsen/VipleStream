// §VR-PREFLIGHT 的判斷邏輯測試（moonlight-qt/app/backend/vrpreflight.h）。
//
// 只用標準函式庫，g++／MSVC 都能單獨編（建置方式見同目錄的 run_tests.ps1）。時間全部是測試自己給的假時鐘：
// 「往返 150 ms」是直接把 150 交給 Gate，不是真的等 150 ms、也不是靠延後 accept 假裝
// （延後 accept 不會讓 TCP 握手變慢，量不出往返時間）。真的對 socket 量的部分在 test_check.cpp。

#include "../../app/backend/vrpreflight.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vrpreflight;

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
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

bool contains(const std::vector<std::string>& lines, const char* needle)
{
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasLine(const std::vector<std::string>& lines, const char* exact)
{
    for (const std::string& l : lines) {
        if (l == exact) {
            return true;
        }
    }
    return false;
}

void beginCase(const char* name)
{
    g_Case = name;
}

// ---- Gate：開頭那幾次 ----

void testFirstSampleFast()
{
    beginCase("first sample fast");
    Gate g;
    CHECK(g.phase() == Phase::Idle);
    Step s = g.begin(1000);
    CHECK(s.next == Next::ProbeNow);
    CHECK(s.log.empty());
    CHECK(g.phase() == Phase::Checking);
    CHECK(g.busy());

    g.probeStarted(1000);
    s = g.onSample(1014, Sample::measured(12));
    CHECK(s.next == Next::Launch);                 // 不等剩下兩次
    CHECK(g.phase() == Phase::Idle);
    CHECK(s.log.size() == 2);
    CHECK(hasLine(s.log, "sample 1/3: 12 ms"));
    CHECK(hasLine(s.log, "launch: 12 ms <= 60 ms (sample 1 of 3, 14 ms after the request)"));
    CHECK(g.lastRttMs() == 12);
    CHECK(g.measuredRttMs() == -1);                // 沒進過 Waiting
}

void testThresholdBoundary()
{
    beginCase("threshold boundary");
    CHECK(kThresholdMs == 60);
    CHECK(isFastEnough(Sample::measured(0)));
    CHECK(isFastEnough(Sample::measured(59)));
    CHECK(isFastEnough(Sample::measured(60)));     // 門檻本身算夠快
    CHECK(!isFastEnough(Sample::measured(61)));
    CHECK(!isFastEnough(Sample::failed("timeout")));
    CHECK(Sample::measured(-5).rttMs == 0);

    Gate g;
    g.begin(0);
    Step s = g.onSample(60, Sample::measured(60));
    CHECK(s.next == Next::Launch);

    Gate h;
    h.begin(0);
    s = h.onSample(61, Sample::measured(61));
    CHECK(s.next == Next::ProbeNow);
    CHECK(h.phase() == Phase::Checking);
}

void testSecondSampleFast()
{
    beginCase("second sample fast");
    Gate g;
    g.begin(0);
    Step s = g.onSample(120, Sample::measured(120));
    CHECK(s.next == Next::ProbeNow);
    CHECK(hasLine(s.log, "sample 1/3: 120 ms"));
    s = g.onSample(160, Sample::measured(40));
    CHECK(s.next == Next::Launch);
    CHECK(hasLine(s.log, "launch: 40 ms <= 60 ms (sample 2 of 3, 160 ms after the request)"));
}

void testTimeoutsThenFast()
{
    beginCase("two timeouts then fast");
    Gate g;
    g.begin(0);
    Step s = g.onSample(1000, Sample::failed("timeout"));
    CHECK(s.next == Next::ProbeNow);
    CHECK(hasLine(s.log, "sample 1/3: no answer (timeout)"));
    CHECK(g.lastRttMs() == -1);
    s = g.onSample(2000, Sample::failed("timeout"));
    CHECK(s.next == Next::ProbeNow);
    s = g.onSample(2040, Sample::measured(40));
    CHECK(s.next == Next::Launch);
    CHECK(contains(s.log, "launch: 40 ms <= 60 ms (sample 3 of 3"));
}

void testAllSlowEntersWaiting()
{
    beginCase("all slow -> waiting");
    Gate g;
    g.begin(0);
    g.probeStarted(0);
    Step s = g.onSample(118, Sample::measured(118));
    CHECK(s.next == Next::ProbeNow);
    g.probeStarted(118);
    s = g.onSample(222, Sample::measured(104));
    CHECK(s.next == Next::ProbeNow);
    g.probeStarted(222);
    s = g.onSample(353, Sample::measured(131));
    CHECK(s.next == Next::ProbeLater);
    CHECK(s.delayMs == 2000 - 131);                // 從上一次開始量的時刻起算
    CHECK(g.phase() == Phase::Waiting);
    CHECK(g.measuredRttMs() == 104);               // 取最小值
    CHECK(g.lastRttMs() == 131);
    CHECK(g.goodStreak() == 0);
    CHECK(hasLine(s.log, "sample 3/3: 131 ms"));
    CHECK(hasLine(s.log, "wait: best of 3 samples is 104 ms > 60 ms (118, 104, 131) - asking the user, probing every 2 s"));
}

void testSlowWithFailures()
{
    beginCase("one slow, two failures -> waiting");
    Gate g;
    g.begin(0);
    g.onSample(120, Sample::measured(120));
    g.onSample(1120, Sample::failed("timeout"));
    const Step s = g.onSample(1125, Sample::failed("refused"));
    CHECK(s.next == Next::ProbeLater);
    CHECK(g.phase() == Phase::Waiting);
    CHECK(g.measuredRttMs() == 120);
    CHECK(contains(s.log, "wait: best of 3 samples is 120 ms > 60 ms (120, timeout, refused)"));
}

void testUnmeasured()
{
    beginCase("unmeasured -> launch");
    Gate g;
    g.begin(0);
    Step s = g.onSample(5, Sample::failed("refused"));
    CHECK(s.next == Next::ProbeNow);
    s = g.onSample(1005, Sample::failed("timeout"));
    CHECK(s.next == Next::ProbeNow);
    s = g.onSample(2005, Sample::failed(""));
    CHECK(s.next == Next::Launch);                 // 量不到不擋
    CHECK(g.phase() == Phase::Idle);
    CHECK(hasLine(s.log, "sample 3/3: no answer (unknown)"));
    CHECK(hasLine(s.log, "unmeasured: no answer on 3 samples (refused, timeout, unknown) - launching as before"));
    CHECK(g.measuredRttMs() == -1);
}

// ---- Gate：等待 ----

Gate waitingGate(int64_t* now)
{
    Gate g;
    g.begin(0);
    g.probeStarted(0);
    g.onSample(100, Sample::measured(100));
    g.probeStarted(100);
    g.onSample(200, Sample::measured(100));
    g.probeStarted(200);
    g.onSample(300, Sample::measured(100));
    *now = 300;
    return g;
}

void testAutoStart()
{
    beginCase("auto-start after three good probes");
    int64_t now = 0;
    Gate g = waitingGate(&now);
    CHECK(g.phase() == Phase::Waiting);

    g.probeStarted(2200);
    Step s = g.onSample(2212, Sample::measured(12));
    CHECK(s.next == Next::ProbeLater);
    CHECK(s.delayMs == 2000 - 12);
    CHECK(g.goodStreak() == 1);
    CHECK(hasLine(s.log, "probe 1: 12 ms (fast enough 1/3)"));

    g.probeStarted(4200);
    s = g.onSample(4260, Sample::measured(60));
    CHECK(s.next == Next::ProbeLater);
    CHECK(g.goodStreak() == 2);

    g.probeStarted(6200);
    s = g.onSample(6209, Sample::measured(9));
    CHECK(s.next == Next::Launch);
    CHECK(g.phase() == Phase::Idle);
    CHECK(hasLine(s.log, "probe 3: 9 ms (fast enough 3/3)"));
    CHECK(hasLine(s.log, "auto-start after 6 s: 60 ms or less on 3 probes in a row (last 9 ms)"));
    // 結束之後保留最後的值（對話框淡出時還在顯示）
    CHECK(g.goodStreak() == 3);
    CHECK(g.lastRttMs() == 9);
    CHECK(g.measuredRttMs() == 100);
}

void testStreakResets()
{
    beginCase("streak resets on a slow or failed probe");
    int64_t now = 0;
    Gate g = waitingGate(&now);
    Step s = g.onSample(2000, Sample::measured(30));
    CHECK(g.goodStreak() == 1);
    s = g.onSample(4000, Sample::measured(30));
    CHECK(g.goodStreak() == 2);
    s = g.onSample(6000, Sample::measured(150));
    CHECK(s.next == Next::ProbeLater);
    CHECK(g.goodStreak() == 0);
    s = g.onSample(8000, Sample::measured(30));
    CHECK(g.goodStreak() == 1);
    s = g.onSample(10000, Sample::measured(30));
    CHECK(g.goodStreak() == 2);
    s = g.onSample(12000, Sample::failed("timeout"));
    CHECK(s.next == Next::ProbeLater);
    CHECK(g.goodStreak() == 0);
    CHECK(g.lastRttMs() == -1);
    s = g.onSample(14000, Sample::measured(30));
    s = g.onSample(16000, Sample::measured(30));
    CHECK(s.next == Next::ProbeLater);
    s = g.onSample(18000, Sample::measured(30));
    CHECK(s.next == Next::Launch);
    CHECK(contains(s.log, "auto-start after 18 s"));
}

void testOverride()
{
    beginCase("start anyway");
    int64_t now = 0;
    Gate g = waitingGate(&now);
    g.onSample(2300, Sample::measured(120));
    Step s = g.startAnyway(8200);
    CHECK(s.next == Next::Launch);
    CHECK(g.phase() == Phase::Idle);
    CHECK(hasLine(s.log, "override: the user chose to start anyway after 8 s (last 120 ms)"));

    // 之後才回來的量測結果不算
    s = g.onSample(8300, Sample::measured(10));
    CHECK(s.next == Next::None);
    CHECK(s.log.empty());
    CHECK(g.lastRttMs() == 120);

    // 第二次按不做事
    s = g.startAnyway(8400);
    CHECK(s.next == Next::None);
    CHECK(s.log.empty());

    // 開頭那幾次還沒有結果時也可以按
    Gate h;
    h.begin(0);
    s = h.startAnyway(700);
    CHECK(s.next == Next::Launch);
    CHECK(hasLine(s.log, "override: the user chose to start anyway after 1 s (nothing measured yet)"));
}

void testCancel()
{
    beginCase("cancel");
    int64_t now = 0;
    Gate g = waitingGate(&now);
    g.onSample(2300, Sample::failed("timeout"));
    Step s = g.cancel(5400, "the user");
    CHECK(s.next == Next::Cancelled);
    CHECK(g.phase() == Phase::Idle);
    CHECK(hasLine(s.log, "cancel: the user after 5 s (last no answer (timeout))"));

    s = g.cancel(5500, "the user");
    CHECK(s.next == Next::None);
    CHECK(s.log.empty());

    Gate h;
    Step t = h.cancel(0, "the user");              // 沒在檢查
    CHECK(t.next == Next::None);
    t = h.startAnyway(0);
    CHECK(t.next == Next::None);
    t = h.onSample(0, Sample::measured(10));
    CHECK(t.next == Next::None);

    h.begin(100);
    t = h.cancel(160, nullptr);
    CHECK(t.next == Next::Cancelled);
    CHECK(hasLine(t.log, "cancel: unknown after 0 s (nothing measured yet)"));
}

void testBeginWhileBusyAndRestart()
{
    beginCase("begin while busy / restart");
    int64_t now = 0;
    Gate g = waitingGate(&now);
    Step s = g.begin(500);
    CHECK(s.next == Next::None);                   // 不重來
    CHECK(g.phase() == Phase::Waiting);
    CHECK(g.measuredRttMs() == 100);

    g.cancel(600, "the user");
    s = g.begin(700);
    CHECK(s.next == Next::ProbeNow);
    CHECK(g.phase() == Phase::Checking);
    CHECK(g.measuredRttMs() == -1);                // 上一次的值清掉
    CHECK(g.lastRttMs() == -1);
    CHECK(g.goodStreak() == 0);
    s = g.onSample(710, Sample::measured(10));
    CHECK(hasLine(s.log, "sample 1/3: 10 ms"));    // 從第一次重新數
    CHECK(hasLine(s.log, "launch: 10 ms <= 60 ms (sample 1 of 3, 10 ms after the request)"));
}

void testProbeDelay()
{
    beginCase("delay to the next probe");
    int64_t now = 0;
    Gate g = waitingGate(&now);
    g.probeStarted(10000);
    Step s = g.onSample(10130, Sample::measured(130));
    CHECK(s.delayMs == 1870);
    g.probeStarted(12000);
    s = g.onSample(13000, Sample::failed("timeout"));     // 逾時用掉 1 秒
    CHECK(s.delayMs == 1000);
    g.probeStarted(14000);
    s = g.onSample(16500, Sample::measured(130));         // 結果晚到（GUI 忙）
    CHECK(s.delayMs == 0);
    g.probeStarted(20000);
    s = g.onSample(19000, Sample::measured(130));         // 時鐘倒退也不會算出負的或超過間隔
    CHECK(s.delayMs == 2000);
}

void testWaitLogRateLimit()
{
    beginCase("log rate limit while waiting");
    int64_t now = 0;
    Gate g = waitingGate(&now);
    int logged = 0;
    std::vector<std::string> all;
    for (int i = 1; i <= 100; i++) {
        now += 2000;
        const Step s = g.onSample(now, Sample::measured(118));
        CHECK(s.next == Next::ProbeLater);
        logged += static_cast<int>(s.log.size());
        all.insert(all.end(), s.log.begin(), s.log.end());
    }
    // 前 30 次每次都記，之後第 45、60、75、90 次
    CHECK(logged == 34);
    CHECK(hasLine(all, "probe 30: 118 ms"));
    CHECK(!contains(all, "probe 31:"));
    CHECK(hasLine(all, "probe 45: 118 ms (14 similar probes not logged)"));
    CHECK(hasLine(all, "probe 60: 118 ms (14 similar probes not logged)"));
    CHECK(hasLine(all, "probe 90: 118 ms (14 similar probes not logged)"));

    // 類別變了照記，並帶出中間省略了幾次
    now += 2000;
    Step s = g.onSample(now, Sample::failed("timeout"));
    CHECK(s.log.size() == 1);
    CHECK(hasLine(s.log, "probe 101: no answer (timeout) (10 similar probes not logged)"));
    now += 2000;
    s = g.onSample(now, Sample::failed("timeout"));
    CHECK(s.log.empty());                          // 跟上一次同類、又不是整 15 次
    now += 2000;
    s = g.onSample(now, Sample::measured(40));
    CHECK(hasLine(s.log, "probe 103: 40 ms (fast enough 1/3) (1 similar probes not logged)"));
    now += 2000;
    s = g.onSample(now, Sample::measured(41));
    CHECK(hasLine(s.log, "probe 104: 41 ms (fast enough 2/3)"));   // 夠快的每次都記
    now += 2000;
    s = g.onSample(now, Sample::measured(42));
    CHECK(s.next == Next::Launch);
    CHECK(contains(s.log, "auto-start after 210 s"));
}

// ---- TestScript ----

std::string seq(TestScript& script, int n)
{
    std::string out;
    for (int i = 0; i < n; i++) {
        const Sample s = script.next();
        if (!out.empty()) {
            out += ' ';
        }
        out += s.ok ? std::to_string(s.rttMs) : std::string("x");
    }
    return out;
}

void testScriptParse()
{
    beginCase("test script: parse");
    TestScript t;
    std::string err = "untouched";
    CHECK(t.configure("", &err));
    CHECK(!t.enabled());                           // 空字串＝關
    CHECK(!t.next().ok);

    CHECK(t.configure("150", &err));
    CHECK(t.enabled());
    CHECK(t.spec() == "150");
    CHECK(seq(t, 4) == "150 150 150 150");         // 最後一項一直重複

    CHECK(t.configure("150*8,30", &err));
    CHECK(seq(t, 11) == "150 150 150 150 150 150 150 150 30 30 30");

    CHECK(t.configure("x", &err));
    CHECK(seq(t, 3) == "x x x");
    CHECK(t.next().why == "scripted");

    CHECK(t.configure("X*2,40", &err));
    CHECK(seq(t, 4) == "x x 40 40");

    CHECK(t.configure("0,999,7*2,x,5", &err));
    CHECK(seq(t, 8) == "0 999 7 7 x 5 5 5");

    CHECK(t.configure("150*4,30*2,150,30", &err));
    CHECK(seq(t, 9) == "150 150 150 150 30 30 150 30 30");
    t.rewind();
    CHECK(seq(t, 5) == "150 150 150 150 30");      // 每一場從頭

    const char* bad[] = {
        "abc", "1000", "-5", "+5", "150*0", "150*", "*3", "150,,30", "150 ,30", " 150", "1e3",
        "150*100001", ",", "150,", ",150", "150*2*3", "x2", "xx", "15.5", "150;30", "9999999999",
    };
    for (const char* spec : bad) {
        TestScript b;
        std::string e;
        const bool ok = b.configure(spec, &e);
        if (ok || e.empty() || b.enabled()) {
            printf("  rejected spec not handled: '%s'\n", spec);
        }
        CHECK(!ok);
        CHECK(!e.empty());
        CHECK(!b.enabled());                       // 格式不對＝維持關
    }

    // 上限
    std::string many;
    for (int i = 0; i < TestScript::kMaxItems; i++) {
        many += (i ? "," : "");
        many += "10";
    }
    CHECK(t.configure(many, &err));
    many += ",10";
    CHECK(!t.configure(many, &err));
    CHECK(err.find("too many") != std::string::npos);
    CHECK(t.configure("150*100000", &err));
    CHECK(t.configure("x", nullptr));              // err 可以不給
    CHECK(!t.configure("bad", nullptr));
}

void testSimulatedDuration()
{
    beginCase("simulated duration");
    CHECK(simulatedDurationMs(Sample::measured(150)) == 150);
    CHECK(simulatedDurationMs(Sample::measured(0)) == 0);
    CHECK(simulatedDurationMs(Sample::failed("scripted")) == kSampleTimeoutMs);
    CHECK(simulatedDurationMs(Sample::measured(5000)) == kSampleTimeoutMs);
}

// ---- 整段流程：Gate＋TestScript，用假時鐘照 VrPreflightCheck 的方式跑 ----

struct Sim {
    std::vector<std::string> log;
    int64_t waitingAt = -1;       // 進 Waiting 的時刻（對話框出現）；沒進過＝-1
    int64_t endAt = -1;
    Next outcome = Next::None;
    int probes = 0;
};

// userAt >= 0：那個時刻使用者按了按鈕（anyway＝仍要開始，否則取消）
Sim simulate(const char* spec, int64_t userAt = -1, bool anyway = false)
{
    Sim sim;
    TestScript script;
    std::string err;
    if (!script.configure(spec, &err)) {
        printf("  bad spec in test: %s\n", spec);
        g_Failures++;
        return sim;
    }
    Gate gate;
    int64_t now = 0;
    Step step = gate.begin(now);
    for (int guard = 0; guard < 100000; guard++) {
        sim.log.insert(sim.log.end(), step.log.begin(), step.log.end());
        if (sim.waitingAt < 0 && gate.phase() == Phase::Waiting) {
            sim.waitingAt = now;
        }
        if (step.next == Next::Launch || step.next == Next::Cancelled) {
            sim.outcome = step.next;
            sim.endAt = now;
            return sim;
        }

        // 下一件事發生的時刻：量測的結果，或使用者按按鈕
        int64_t probeStart = now;
        if (step.next == Next::ProbeLater) {
            probeStart = now + step.delayMs;
        }
        if (userAt >= 0 && userAt <= probeStart) {
            now = userAt;
            step = anyway ? gate.startAnyway(now) : gate.cancel(now, "the user");
            userAt = -1;
            continue;
        }
        now = probeStart;
        gate.probeStarted(now);
        const Sample s = script.next();
        sim.probes++;
        const int64_t resultAt = now + simulatedDurationMs(s);
        if (userAt >= 0 && userAt <= resultAt) {
            now = userAt;
            step = anyway ? gate.startAnyway(now) : gate.cancel(now, "the user");
            userAt = -1;
            continue;
        }
        now = resultAt;
        step = gate.onSample(now, s);
    }
    printf("  simulation did not end: %s\n", spec);
    g_Failures++;
    return sim;
}

void testScenarios()
{
    beginCase("scenario: fast path");
    Sim sim = simulate("12");
    CHECK(sim.outcome == Next::Launch);
    CHECK(sim.endAt == 12);                        // 多出來的延遲就是一次往返
    CHECK(sim.probes == 1);
    CHECK(sim.waitingAt == -1);

    beginCase("scenario: relay, stays slow, user cancels");
    sim = simulate("118", 5000, false);
    CHECK(sim.outcome == Next::Cancelled);
    CHECK(sim.waitingAt == 3 * 118);               // 三次都量完才判定太慢
    CHECK(sim.endAt == 5000);
    CHECK(hasLine(sim.log, "cancel: the user after 5 s (last 118 ms)"));

    beginCase("scenario: relay, user starts anyway");
    sim = simulate("118", 9100, true);
    CHECK(sim.outcome == Next::Launch);
    CHECK(sim.endAt == 9100);
    CHECK(hasLine(sim.log, "override: the user chose to start anyway after 9 s (last 118 ms)"));

    beginCase("scenario: relay goes direct (150*8,30)");
    sim = simulate("150*8,30");
    CHECK(sim.outcome == Next::Launch);
    CHECK(sim.waitingAt == 450);
    CHECK(sim.probes == 11);                       // 開頭 3 次＋等待中 8 次
    CHECK(sim.endAt == 16330);                     // 等待中每 2 秒量一次：…12300、14300、16300 開始，各 30 ms
    CHECK(hasLine(sim.log, "probe 5: 150 ms"));
    CHECK(hasLine(sim.log, "probe 8: 30 ms (fast enough 3/3)"));
    CHECK(hasLine(sim.log, "auto-start after 16 s: 60 ms or less on 3 probes in a row (last 30 ms)"));

    beginCase("scenario: flapping (150*4,30*2,150,30)");
    sim = simulate("150*4,30*2,150,30");
    CHECK(sim.outcome == Next::Launch);
    CHECK(sim.probes == 3 + 7);
    CHECK(sim.endAt == 14330);
    CHECK(hasLine(sim.log, "probe 3: 30 ms (fast enough 2/3)"));
    CHECK(hasLine(sim.log, "probe 4: 150 ms"));
    CHECK(hasLine(sim.log, "probe 5: 30 ms (fast enough 1/3)"));

    beginCase("scenario: unmeasurable (x)");
    sim = simulate("x");
    CHECK(sim.outcome == Next::Launch);
    CHECK(sim.endAt == 3000);                      // 三次逾時之後照常開始
    CHECK(sim.waitingAt == -1);
    CHECK(hasLine(sim.log, "unmeasured: no answer on 3 samples (scripted, scripted, scripted) - launching as before"));

    beginCase("scenario: two timeouts then fast (x*2,40)");
    sim = simulate("x*2,40");
    CHECK(sim.outcome == Next::Launch);
    CHECK(sim.endAt == 2040);
    CHECK(sim.waitingAt == -1);

    beginCase("scenario: user cancels while the first samples are still running");
    sim = simulate("x", 1500, false);
    CHECK(sim.outcome == Next::Cancelled);
    CHECK(sim.endAt == 1500);
    CHECK(hasLine(sim.log, "cancel: the user after 2 s (last no answer (scripted))"));

    beginCase("scenario: host stops answering while waiting, then comes back fast");
    sim = simulate("150*3,x*4,20");
    CHECK(sim.outcome == Next::Launch);
    CHECK(sim.waitingAt == 450);
    CHECK(hasLine(sim.log, "probe 1: no answer (scripted)"));
    CHECK(hasLine(sim.log, "probe 7: 20 ms (fast enough 3/3)"));
    // 等待中的量測固定每 2 秒開始一次（300 起算）：2300、4300、6300、8300（各逾時 1 秒），10300、12300、14300（各 20 ms）
    CHECK(sim.endAt == 14320);
}

}  // namespace

int main()
{
    testFirstSampleFast();
    testThresholdBoundary();
    testSecondSampleFast();
    testTimeoutsThenFast();
    testAllSlowEntersWaiting();
    testSlowWithFailures();
    testUnmeasured();
    testAutoStart();
    testStreakResets();
    testOverride();
    testCancel();
    testBeginWhileBusyAndRestart();
    testProbeDelay();
    testWaitLogRateLimit();
    testScriptParse();
    testSimulatedDuration();
    testScenarios();

    printf("vr_preflight logic: %d checks, %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
