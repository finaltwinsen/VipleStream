// VipleStream 2.0 §VR-LATCH-FLIGHT — runtime 防護的狀態機（純邏輯）。
//
// 「等在途的幀」讓 frame thread 偶爾有一格晚零點幾毫秒才交幀。runtime 不接受的話會把提前量加大、整幀往後排
// （延後 8 ms 那一場：predictAhead 多一個週期、開始漏幀）。這裡每秒看一次：predictAhead 連續幾秒高過基準半個週期，
// 或最近 10 秒 XR 漏了幾幀，就把功能暫停一段時間；冷卻每次加倍、有上限，到時自動恢復。
// 規則和既有的 XrContext::pcvrWaitGuard（§VR-LATCH-WAIT 的防護）相同，那一個沒有改寫；另外多一個「同一場最多觸發
// 幾次，到了就整場關掉」的選項（flight 不用，留給之後的功能）。
//
// header-only，只用標準函式庫（不依賴 Qt／OpenXR／libplacebo），做法同 xrlatchstats.h、xrphase.h：沒有 OpenXR 的建置也編
// 得到，`xr-probe --selftest-latch` 會接著跑這裡的 selfTest()。全部用整數算（時刻與時間長度是奈秒）。
//
// 執行緒：沒有鎖，呼叫端自己保證只在一條執行緒用（XrContext 是 frame thread）。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <string>

namespace xrguard {

struct Config {
    uint32_t baseAfterSec = 3;   // 起點（bring-up）幾秒之後才開始記 predictAhead 的基準
    uint32_t highSecs = 2;       // predictAhead 連幾秒高過基準半個週期就觸發（0＝不看這一項）
    uint32_t missedPer10s = 3;   // 最近 10 秒 XR 漏幾幀就觸發（0＝不看這一項）
    uint32_t coolSec0 = 60;      // 第一次暫停多久；之後每次加倍，
    uint32_t coolSecMax = 480;   //   加到這個上限為止
    uint32_t maxTrips = 0;       // 同一場最多觸發幾次，到了就整場關掉；0＝不設上限
};

// 「沒有觸發」不叫 None：X11 的標頭把 None 定義成巨集（#define None 0L），Linux 上有些編譯單元在 include 這個檔之前
// 就帶進了它，列舉值會被換掉、編不過（同一個坑見 vkfruc.h）
enum class Trip { NoTrip, PredictAhead, Missed };

class RuntimeGuard
{
public:
    // 預設的建構子不標 explicit：這個類別會當別的結構的成員，那個結構用 {} 初始化時才不會被擋
    RuntimeGuard() : RuntimeGuard(Config()) {}
    explicit RuntimeGuard(const Config& c) : m_Cfg(c), m_CoolSec(c.coolSec0) {}

    // 起點（bring-up 的時刻）：基準在它之後 baseAfterSec 秒才開始記。沒呼叫的話以第一次 tick() 的時刻為起點
    void start(uint64_t startNs)
    {
        m_StartNs = startNs;
        m_Started = true;
    }

    // 每秒呼叫一次（功能沒生效的那幾秒也要呼叫：基準與上一次的漏幀總數照樣更新）。
    //   active      ＝功能這一秒開著（例如同場切換的這一邊）；冷卻中由這裡自己判斷，不必算進去
    //   aheadNs     ＝目前的 predictAhead（0＝還沒有）
    //   periodNs    ＝顯示週期
    //   missedTotal ＝XR 漏幀的累計值
    // 回傳這一次有沒有觸發。兩個條件同時成立時回 PredictAhead
    Trip tick(uint64_t nowNs, bool active, uint64_t aheadNs, uint64_t periodNs, uint64_t missedTotal)
    {
        if (!m_Started) {
            start(nowNs);
        }
        const uint64_t sinceStartNs = nowNs > m_StartNs ? nowNs - m_StartNs : 0;
        if (sinceStartNs >= static_cast<uint64_t>(m_Cfg.baseAfterSec) * kSecNs && aheadNs > 0 &&
            (m_BaseNs == 0 || aheadNs < m_BaseNs)) {
            m_BaseNs = aheadNs;  // 看過的最小值；功能關著的那一邊、冷卻期間也照樣更新
        }
        const uint64_t missedDelta = (m_MissedInit && missedTotal > m_MissedLast) ? missedTotal - m_MissedLast : 0;
        m_MissedLast = missedTotal;
        m_MissedInit = true;
        if (!active || paused(nowNs)) {
            clearWindow();  // 功能沒在作用的時候發生的事不算在它頭上
            return Trip::NoTrip;
        }
        m_Missed[m_MissedIdx++ % kMissedSecs] = static_cast<uint32_t>((std::min)(missedDelta, static_cast<uint64_t>(0xffffffffu)));
        uint64_t missed10 = 0;
        for (const uint32_t m : m_Missed) {
            missed10 += m;
        }
        const bool high = m_BaseNs > 0 && aheadNs > m_BaseNs + periodNs / 2;
        m_HighRun = high ? m_HighRun + 1u : 0u;
        Trip trip = Trip::NoTrip;
        if (m_Cfg.highSecs > 0 && m_HighRun >= m_Cfg.highSecs) {
            trip = Trip::PredictAhead;
        }
        else if (m_Cfg.missedPer10s > 0 && missed10 >= m_Cfg.missedPer10s) {
            trip = Trip::Missed;
        }
        if (trip == Trip::NoTrip) {
            return trip;
        }
        m_LastPauseSec = m_CoolSec;
        m_LastMissed = static_cast<uint32_t>((std::min)(missed10, static_cast<uint64_t>(0xffffffffu)));
        m_LastAheadNs = aheadNs;
        m_OffUntilNs = nowNs + static_cast<uint64_t>(m_CoolSec) * kSecNs;
        m_CoolSec = (std::min)(m_CoolSec * 2u, m_Cfg.coolSecMax);
        m_Trips++;
        if (m_Cfg.maxTrips > 0 && m_Trips >= m_Cfg.maxTrips) {
            m_Disabled = true;
        }
        clearWindow();
        return trip;
    }

    // 冷卻中，或已經整場關掉
    bool paused(uint64_t nowNs) const { return m_Disabled || nowNs < m_OffUntilNs; }
    bool disabled() const { return m_Disabled; }
    uint32_t trips() const { return m_Trips; }
    uint32_t coolSec() const { return m_CoolSec; }  // 下一次觸發會暫停多久
    uint64_t baseAheadNs() const { return m_BaseNs; }
    // 最近一次觸發：暫停了多久、當時最近 10 秒漏了幾幀、當時的 predictAhead（印 log 用）
    uint32_t lastPauseSec() const { return m_LastPauseSec; }
    uint32_t lastMissed() const { return m_LastMissed; }
    uint64_t lastAheadNs() const { return m_LastAheadNs; }

private:
    static constexpr uint64_t kSecNs = 1000000000ull;
    static constexpr uint32_t kMissedSecs = 10;

    void clearWindow()
    {
        std::fill(std::begin(m_Missed), std::end(m_Missed), 0u);
        m_HighRun = 0;
    }

    Config m_Cfg;
    uint32_t m_CoolSec;
    bool m_Started = false;
    uint64_t m_StartNs = 0;
    uint64_t m_BaseNs = 0;       // predictAhead 的基準（0＝還沒有）
    uint64_t m_OffUntilNs = 0;   // 暫停到這個時刻
    uint32_t m_HighRun = 0;      // 連續幾秒高過基準半個週期
    uint32_t m_Missed[kMissedSecs] = {};  // 最近 10 秒每秒的 XR 漏幀
    uint32_t m_MissedIdx = 0;
    uint64_t m_MissedLast = 0;
    bool m_MissedInit = false;
    uint32_t m_Trips = 0;
    bool m_Disabled = false;
    uint32_t m_LastPauseSec = 0;
    uint32_t m_LastMissed = 0;
    uint64_t m_LastAheadNs = 0;
};

// 最近一段時間（預設 10 秒）裡發生了幾次。固定大小、不配置記憶體；一個視窗裡超過 kCap 次就停在 kCap
// （拿它判斷「10 秒內有沒有 3 次」這種門檻綽綽有餘）
class WindowCount
{
public:
    static constexpr size_t kCap = 64;

    WindowCount() : WindowCount(10000000000ull) {}
    explicit WindowCount(uint64_t windowNs) : m_WindowNs(windowNs) {}

    void add(uint64_t nowNs)
    {
        m_T[m_Head++ % kCap] = nowNs;
        if (m_N < kCap) {
            m_N++;
        }
    }

    // 距現在不到一個視窗的有幾筆（滿一個視窗的那一筆不算）
    uint32_t count(uint64_t nowNs)
    {
        while (m_N > 0) {
            const uint64_t oldest = m_T[(m_Head - m_N) % kCap];
            if (nowNs > oldest && nowNs - oldest >= m_WindowNs) {
                m_N--;
            }
            else {
                break;
            }
        }
        return static_cast<uint32_t>(m_N);
    }

    void clear()
    {
        m_N = 0;
        m_Head = 0;
    }

private:
    uint64_t m_T[kCap] = {};
    size_t m_Head = 0;  // 累計加了幾筆（下一筆寫在 m_Head % kCap）
    size_t m_N = 0;     // 還在視窗裡的筆數（最舊的在 m_Head − m_N）
    uint64_t m_WindowNs;
};

// 自測；回傳失敗數。每個案例呼叫 report(名稱, 是否通過, 細節)（簽名同 xrlatch::selfTest）
inline int selfTest(const std::function<void(const char* name, bool ok, const std::string& detail)>& report)
{
    int failures = 0;
    auto caseDone = [&](const char* name, bool ok, const std::string& detail) {
        if (!ok) {
            failures++;
        }
        if (report) {
            report(name, ok, detail);
        }
    };
    char buf[320];
    const uint64_t S = 1000000000ull;      // 一秒
    const uint64_t P = 8333333ull;         // 120 Hz 的顯示週期
    const uint64_t t0 = 100ull * S;        // bring-up 的時刻
    const uint64_t low = 28500000ull;      // 平常的 predictAhead：28.5 ms
    const uint64_t high = low + P / 2 + 1; // 剛好高過基準半個週期
    auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    auto tripName = [](Trip t) { return t == Trip::NoTrip ? "none" : (t == Trip::PredictAhead ? "predictAhead" : "missed"); };

    {
        // 基準＝起點 3 秒之後看過的最小值。前 3 秒的 predictAhead 很高（runtime 起步期）：不記，也不會因為它觸發
        RuntimeGuard g;
        g.start(t0);
        const Trip a = g.tick(t0 + 1 * S, true, 40000000ull, P, 0);
        const Trip b = g.tick(t0 + 2 * S, true, 40000000ull, P, 0);
        const uint64_t base2 = g.baseAheadNs();
        const Trip c = g.tick(t0 + 3 * S, true, low, P, 0);  // 剛好滿 3 秒：開始記
        const uint64_t base3 = g.baseAheadNs();
        g.tick(t0 + 4 * S, true, 29000000ull, P, 0);  // 比基準高一點：不動
        const uint64_t base4 = g.baseAheadNs();
        g.tick(t0 + 5 * S, true, 28400000ull, P, 0);  // 更低：更新
        const uint64_t base5 = g.baseAheadNs();
        const bool ok = a == Trip::NoTrip && b == Trip::NoTrip && c == Trip::NoTrip && base2 == 0 && base3 == low && base4 == low &&
                        base5 == 28400000ull && g.trips() == 0 && !g.paused(t0 + 5 * S);
        std::snprintf(buf, sizeof(buf), "base after 2 s = %.1f ms, 3 s = %.1f, 4 s = %.1f, 5 s = %.1f", ms(base2), ms(base3), ms(base4),
                      ms(base5));
        caseDone("guard-base-after-3s", ok, buf);
    }
    {
        // 生效中，predictAhead 連 2 秒高過基準半個週期 → 第 2 秒觸發，暫停 60 秒，到時自動恢復；下一次會是 120 秒
        RuntimeGuard g;
        g.start(t0);
        g.tick(t0 + 3 * S, true, low, P, 0);
        const Trip a = g.tick(t0 + 4 * S, true, high, P, 0);
        const Trip b = g.tick(t0 + 5 * S, true, high, P, 0);
        const uint64_t tripNs = t0 + 5 * S;
        const bool ok = a == Trip::NoTrip && b == Trip::PredictAhead && g.paused(tripNs) && g.paused(tripNs + 60 * S - 1) &&
                        !g.paused(tripNs + 60 * S) && g.lastPauseSec() == 60 && g.coolSec() == 120 && g.trips() == 1 &&
                        !g.disabled() && g.lastAheadNs() == high;
        std::snprintf(buf, sizeof(buf), "second high second -> %s, paused %u s at %.1f ms, next %u s", tripName(b), g.lastPauseSec(),
                      ms(g.lastAheadNs()), g.coolSec());
        caseDone("guard-trip-predict-ahead", ok, buf);
    }
    {
        // 只有 1 秒偏高、下一秒回到基準：不觸發（連續才算，中間斷了就從頭數）
        RuntimeGuard g;
        g.start(t0);
        g.tick(t0 + 3 * S, true, low, P, 0);
        bool ok = true;
        for (uint64_t s = 4; s < 24; s++) {
            ok = ok && g.tick(t0 + s * S, true, s % 2 == 0 ? high : low, P, 0) == Trip::NoTrip;
        }
        ok = ok && g.trips() == 0 && !g.paused(t0 + 24 * S);
        caseDone("guard-one-high-second", ok, ok ? "alternating high/low seconds never trip" : "tripped");
    }
    {
        // 生效中，10 秒內 XR 漏了 3 幀 → 觸發。第一次 tick 只記累計值（開場之前漏的不算）
        RuntimeGuard g;
        g.start(t0);
        const Trip a = g.tick(t0 + 3 * S, true, low, P, 5);
        const Trip b = g.tick(t0 + 4 * S, true, low, P, 6);
        const Trip c = g.tick(t0 + 5 * S, true, low, P, 6);
        const Trip d = g.tick(t0 + 6 * S, true, low, P, 7);
        const Trip e = g.tick(t0 + 7 * S, true, low, P, 8);
        const bool ok = a == Trip::NoTrip && b == Trip::NoTrip && c == Trip::NoTrip && d == Trip::NoTrip && e == Trip::Missed &&
                        g.lastMissed() == 3 && g.paused(t0 + 7 * S) && g.lastPauseSec() == 60;
        std::snprintf(buf, sizeof(buf), "third missed frame within 10 s -> %s (missed %u)", tripName(e), g.lastMissed());
        caseDone("guard-trip-missed", ok, buf);
    }
    {
        // 另一邊（功能關著）的那幾秒 predictAhead 偏高、漏了 9 幀：不觸發。換回來時視窗是空的——再一秒偏高不觸發，
        // 那 9 幀也不算；換回來之後自己漏的第 3 幀才觸發
        RuntimeGuard g;
        g.start(t0);
        g.tick(t0 + 3 * S, true, low, P, 0);
        bool ok = g.tick(t0 + 4 * S, false, high, P, 2) == Trip::NoTrip && g.tick(t0 + 5 * S, false, high, P, 5) == Trip::NoTrip &&
                  g.tick(t0 + 6 * S, false, high, P, 9) == Trip::NoTrip;
        ok = ok && g.tick(t0 + 7 * S, true, high, P, 9) == Trip::NoTrip;
        ok = ok && g.tick(t0 + 8 * S, true, low, P, 10) == Trip::NoTrip;
        ok = ok && g.tick(t0 + 9 * S, true, low, P, 11) == Trip::NoTrip && g.trips() == 0;
        const Trip last = g.tick(t0 + 10 * S, true, low, P, 12);
        ok = ok && last == Trip::Missed && g.lastMissed() == 3;
        std::snprintf(buf, sizeof(buf), "nothing counted while inactive; own third miss -> %s", tripName(last));
        caseDone("guard-inactive-side", ok, buf);
    }
    {
        // 連續觸發五次：冷卻 60 → 120 → 240 → 480 → 480 秒；冷卻中再偏高也不觸發；每次到時自動恢復
        RuntimeGuard g;
        g.start(t0);
        uint64_t t = t0 + 3 * S;
        g.tick(t, true, low, P, 0);
        uint32_t pauses[5] = {};
        bool ok = true;
        for (int k = 0; k < 5; k++) {
            t += S;
            const Trip a = g.tick(t, true, high, P, 0);
            t += S;
            const Trip b = g.tick(t, true, high, P, 0);
            ok = ok && a == Trip::NoTrip && b == Trip::PredictAhead;
            pauses[k] = g.lastPauseSec();
            ok = ok && g.tick(t + S, true, high, P, 0) == Trip::NoTrip && g.tick(t + 2 * S, true, high, P, 0) == Trip::NoTrip &&
                 g.paused(t + 2 * S);
            t += static_cast<uint64_t>(pauses[k]) * S;  // 冷卻結束的那一刻
            ok = ok && !g.paused(t);
        }
        ok = ok && pauses[0] == 60 && pauses[1] == 120 && pauses[2] == 240 && pauses[3] == 480 && pauses[4] == 480 &&
             g.coolSec() == 480 && g.trips() == 5 && !g.disabled();
        std::snprintf(buf, sizeof(buf), "pauses %u, %u, %u, %u, %u s", pauses[0], pauses[1], pauses[2], pauses[3], pauses[4]);
        caseDone("guard-cooldown-doubles", ok, buf);
    }
    {
        // 次數上限 3：第三次觸發之後整場關掉，paused() 永遠為真、不再觸發。上限 0（不設）：觸發十次也不會關
        auto tripOnce = [&](RuntimeGuard& g, uint64_t& t) {
            t += S;
            g.tick(t, true, high, P, 0);
            t += S;
            const Trip r = g.tick(t, true, high, P, 0);
            t += static_cast<uint64_t>(g.lastPauseSec()) * S;
            return r;
        };
        Config c3;
        c3.maxTrips = 3;
        RuntimeGuard g(c3);
        g.start(t0);
        uint64_t t = t0 + 3 * S;
        g.tick(t, true, low, P, 0);
        bool ok = tripOnce(g, t) == Trip::PredictAhead && !g.disabled();
        ok = ok && tripOnce(g, t) == Trip::PredictAhead && !g.disabled();
        ok = ok && tripOnce(g, t) == Trip::PredictAhead && g.disabled() && g.trips() == 3;
        ok = ok && g.paused(t) && g.paused(t + 100000 * S) && tripOnce(g, t) == Trip::NoTrip && g.trips() == 3;
        RuntimeGuard noLimit;
        noLimit.start(t0);
        uint64_t u = t0 + 3 * S;
        noLimit.tick(u, true, low, P, 0);
        for (int k = 0; k < 10; k++) {
            ok = ok && tripOnce(noLimit, u) == Trip::PredictAhead;
        }
        ok = ok && !noLimit.disabled() && noLimit.trips() == 10 && !noLimit.paused(u);
        std::snprintf(buf, sizeof(buf), "limit 3: disabled after %u trips; no limit: %u trips, disabled=%d", g.trips(), noLimit.trips(),
                      noLimit.disabled() ? 1 : 0);
        caseDone("guard-max-trips", ok, buf);
    }
    {
        // WindowCount：10 秒內加 3 次 → 3；第一筆滿 10 秒之後 → 2；clear() → 0。一個視窗裡超過 64 次就停在 64；
        // 繞過一圈之後 clear() 也要真的清空（裡面還留著沒過期的時刻），清完再加照常計數
        WindowCount w;
        w.add(t0);
        w.add(t0 + 2 * S);
        w.add(t0 + 4 * S);
        const uint32_t a = w.count(t0 + 5 * S);
        const uint32_t b = w.count(t0 + 10 * S - 1);
        const uint32_t c = w.count(t0 + 10 * S);
        const uint32_t d = w.count(t0 + 13 * S);
        w.clear();
        const uint32_t e = w.count(t0 + 13 * S);
        WindowCount many;
        for (uint64_t i = 0; i < 100; i++) {
            many.add(t0 + i * 1000000ull);
        }
        const uint32_t f = many.count(t0 + S);
        many.clear();
        const uint32_t h = many.count(t0 + S);
        many.add(t0 + S);
        many.add(t0 + 2 * S);
        const uint32_t k = many.count(t0 + 3 * S);
        const uint32_t g = many.count(t0 + 20 * S);
        std::snprintf(buf, sizeof(buf),
                      "3 adds -> %u, just before 10 s -> %u, at 10 s -> %u, at 13 s -> %u, cleared -> %u; 100 adds -> %u, cleared -> %u, 2 more -> %u, later -> %u",
                      a, b, c, d, e, f, h, k, g);
        caseDone("guard-window-count", a == 3 && b == 3 && c == 2 && d == 1 && e == 0 && f == 64 && h == 0 && k == 2 && g == 0, buf);
    }
    {
        // 漏幀看的是最近 10 秒：第 4、8、15 秒各漏一幀（第三幀時第一幀已經滑出視窗）不觸發；第 4、8、13 秒就觸發
        RuntimeGuard sparse, dense;
        sparse.start(t0);
        dense.start(t0);
        Trip sparseTrip = Trip::NoTrip, denseTrip = Trip::NoTrip;
        uint64_t denseAt = 0;
        for (uint64_t s = 3; s <= 20; s++) {
            const uint64_t sparseTotal = (s >= 4 ? 1u : 0u) + (s >= 8 ? 1u : 0u) + (s >= 15 ? 1u : 0u);
            const uint64_t denseTotal = (s >= 4 ? 1u : 0u) + (s >= 8 ? 1u : 0u) + (s >= 13 ? 1u : 0u);
            if (sparse.tick(t0 + s * S, true, low, P, sparseTotal) != Trip::NoTrip) {
                sparseTrip = Trip::Missed;
            }
            const Trip r = dense.tick(t0 + s * S, true, low, P, denseTotal);
            if (r != Trip::NoTrip && denseAt == 0) {
                denseTrip = r;
                denseAt = s;
            }
        }
        std::snprintf(buf, sizeof(buf), "misses at 4/8/15 s -> %s, at 4/8/13 s -> %s at %llu s", tripName(sparseTrip), tripName(denseTrip),
                      static_cast<unsigned long long>(denseAt));
        caseDone("guard-missed-window-slides", sparseTrip == Trip::NoTrip && denseTrip == Trip::Missed && denseAt == 13, buf);
    }
    {
        // 兩個條件在同一秒成立：回 PredictAhead（漏幀數照樣帶出來給 log）
        RuntimeGuard g;
        g.start(t0);
        g.tick(t0 + 3 * S, true, low, P, 0);
        const Trip a = g.tick(t0 + 4 * S, true, high, P, 2);
        const Trip b = g.tick(t0 + 5 * S, true, high, P, 3);
        std::snprintf(buf, sizeof(buf), "first -> %s, second -> %s (missed %u)", tripName(a), tripName(b), g.lastMissed());
        caseDone("guard-both-conditions", a == Trip::NoTrip && b == Trip::PredictAhead && g.lastMissed() == 3, buf);
    }
    {
        // 「高過基準半個週期」是嚴格大於：剛好等於基準＋半個週期不算，多 1 ns 才算
        RuntimeGuard g;
        g.start(t0);
        g.tick(t0 + 3 * S, true, low, P, 0);
        bool ok = true;
        for (uint64_t s = 4; s < 10; s++) {
            ok = ok && g.tick(t0 + s * S, true, low + P / 2, P, 0) == Trip::NoTrip;
        }
        const Trip a = g.tick(t0 + 10 * S, true, low + P / 2 + 1, P, 0);
        const Trip b = g.tick(t0 + 11 * S, true, low + P / 2 + 1, P, 0);
        ok = ok && a == Trip::NoTrip && b == Trip::PredictAhead;
        caseDone("guard-half-period-boundary", ok, ok ? "exactly half a period above the base does not count" : "wrong boundary");
    }
    {
        // 沒有給起點：以第一次 tick 為起點，3 秒之後才記基準。冷卻期間看到更低的 predictAhead 照樣更新基準
        RuntimeGuard g;
        g.tick(t0, true, low, P, 0);
        g.tick(t0 + 1 * S, true, low, P, 0);
        g.tick(t0 + 2 * S, true, low, P, 0);
        const uint64_t before = g.baseAheadNs();
        g.tick(t0 + 3 * S, true, low, P, 0);
        const uint64_t after = g.baseAheadNs();
        g.tick(t0 + 4 * S, true, high, P, 0);
        const Trip trip = g.tick(t0 + 5 * S, true, high, P, 0);
        g.tick(t0 + 6 * S, true, low - 1000000ull, P, 0);  // 冷卻中
        const uint64_t paused = g.baseAheadNs();
        const bool ok = before == 0 && after == low && trip == Trip::PredictAhead && g.paused(t0 + 6 * S) && paused == low - 1000000ull;
        std::snprintf(buf, sizeof(buf), "base %.1f -> %.1f ms from the first tick; %.1f ms while paused", ms(before), ms(after), ms(paused));
        caseDone("guard-default-start-and-base-while-paused", ok, buf);
    }
    return failures;
}

}  // namespace xrguard
