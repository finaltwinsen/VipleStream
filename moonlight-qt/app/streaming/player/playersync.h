// VipleStream 2.0 §VR-PLAYER — 影片幀要在哪一個顯示格出現、什麼時候交出去（純邏輯）。
//
// XR frame thread 每一格在 xrWaitFrame 返回後挑「最新交出的影像」（XrVideo 是 latest-wins 的信箱），這一格的畫面在
// 預測顯示時刻才被看見，兩者差一個固定的提前量（ahead，Frame 上約 26～28 ms）。所以一張影片幀：
//   1. 照影音時鐘換成「該被看見的時刻」td（steady 時鐘）；
//   2. 取離 td 最近的顯示格（四捨五入到格點，誤差在 ±半格內，24 fps 在 120 Hz 時每張都剛好 5 格）；
//   3. 那一格的挑影像時刻 latch＝格點的顯示時刻 − ahead；
//   4. 在「上一格的挑影像之後」交出去（latch − 一個週期＋guard）：太早會被上一格挑走（提前一格出現），
//      太晚趕不上影像 render thread 畫完（晚一格）。中間這段有將近一個週期的餘裕。
// 交出時已經過了 latch − renderBudget 的算「晚了」：下一張也已經晚了就丟掉這一張（反正輪不到它），否則照交。
//
// 鎖相（Lock）：td 來自聲音時鐘，回呼的抖動與聲音裝置和 steady 時鐘的微小速率差會讓 td 相對格點前後飄幾毫秒；
// 片源的 td 剛好落在兩格中間時，四捨五入會在相鄰兩格之間跳，24 fps 變成 4／6 格交錯。所以記住上一張放在哪一格：
// 這一張「照片源間隔推算的那一格」離 td 不到 kLockWindow 個週期就用它，真的飄遠了才換格（一次只差一格）。
//
// 格點由 XR frame thread 發布（最近一次醒來的時刻、那一格的預測顯示時刻、週期）。沒有 XR（--no-xr 的測試）時呼叫端
// 自己用固定週期的假格點。
//
// header-only，只用標準函式庫（不依賴 Qt／FFmpeg／OpenXR），`xr-probe --selftest-latch` 會接著跑這裡的 selfTest()。

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace playersync {

// XR frame thread 發布的顯示格點（steady 時鐘，ns）。periodNs＝0＝還沒有
struct Grid {
    uint64_t wakeNs = 0;     // 最近一次 xrWaitFrame 返回（＝那一格挑影像的大約時刻）
    uint64_t displayNs = 0;  // 那一格的預測顯示時刻
    uint64_t periodNs = 0;
    bool valid() const { return periodNs > 0 && displayNs > wakeNs; }
    uint64_t aheadNs() const { return displayNs - wakeNs; }
};

// 上一張交出的那一張放在哪一格（呼叫端在交出之後更新；seek、暫停、序號改變時 reset）
struct Lock {
    bool have = false;
    uint64_t slotDisplayNs = 0;
    double pts = 0.0;
    void reset() { have = false; }
    void update(uint64_t slotDisplay, double p)
    {
        have = true;
        slotDisplayNs = slotDisplay;
        pts = p;
    }
};

// 推算的那一格離 td 多少個週期之內就沿用。要大於 0.5＋時鐘抖動（以週期計）才不會換格之後又換回來：
// 換格時誤差的平均約是 W − 抖動，換過去之後變成 W − 抖動 − 1，再遇到反方向的抖動不能超出 −W，所以 W > 0.5＋抖動。
// 1.2 容得下 ±0.7 個週期（120 Hz 約 ±5.8 ms）的抖動；代價是一張最多偏離理想的格 1.2 個週期（120 Hz 10 ms，影音對齊
// 還在可以接受的範圍）
constexpr double kLockWindow = 1.2;

struct Plan {
    int64_t slot = 0;           // 相對於 grid.displayNs 的格數（可為負：已經過去的格）
    uint64_t slotDisplayNs = 0; // 該格的顯示時刻
    uint64_t latchNs = 0;       // 該格的挑影像時刻
    uint64_t submitAtNs = 0;    // 最早可以交出的時刻（上一格挑完之後）
    bool late = false;          // nowNs 已經超過 latch − renderBudget
};

// 交出時刻在上一格挑影像之後多久（挑影像本身在醒來之後還要一點時間：xrBeginFrame、取信箱）
constexpr uint64_t kGuardNs = 1000000;
// 影像 render thread 把一張畫進 swapchain 要多久的保守值（libplacebo 在頭盔上 4K 約 2 ms）
constexpr uint64_t kRenderBudgetNs = 2500000;

inline Plan plan(const Grid& g, uint64_t tdNs, uint64_t nowNs, uint64_t renderBudgetNs = kRenderBudgetNs,
                 const Lock* lock = nullptr, double pts = 0.0)
{
    Plan p;
    const double per = static_cast<double>(g.periodNs);
    const double d = static_cast<double>(static_cast<int64_t>(tdNs - g.displayNs));
    p.slot = static_cast<int64_t>(std::floor(d / per + 0.5));
    if (lock != nullptr && lock->have) {
        const double step = std::floor((pts - lock->pts) * 1e9 / per + 0.5);
        const double expect = static_cast<double>(static_cast<int64_t>(lock->slotDisplayNs - g.displayNs)) + step * per;
        if (step >= 1.0 && std::fabs(d - expect) < kLockWindow * per) {
            p.slot = static_cast<int64_t>(std::floor(expect / per + 0.5));
        }
    }
    p.slotDisplayNs = static_cast<uint64_t>(static_cast<int64_t>(g.displayNs) + p.slot * static_cast<int64_t>(g.periodNs));
    p.latchNs = p.slotDisplayNs - g.aheadNs();
    p.submitAtNs = p.latchNs - g.periodNs + kGuardNs;
    p.late = nowNs + renderBudgetNs > p.latchNs;
    return p;
}

enum class Action { Wait, Submit, Drop };

// 一張幀（tdNs）要怎麼處理。haveNext／nextTdNs＝下一張（已經解出來的話）。
// Wait：還不到交出時刻，*waitUntilNs＝什麼時候再來；Submit：現在交；Drop：丟掉，接著處理下一張
inline Action decide(const Grid& g, uint64_t tdNs, bool haveNext, uint64_t nextTdNs, uint64_t nowNs, uint64_t* waitUntilNs,
                     uint64_t renderBudgetNs = kRenderBudgetNs, const Lock* lock = nullptr, double pts = 0.0,
                     double nextPts = 0.0)
{
    const Plan p = plan(g, tdNs, nowNs, renderBudgetNs, lock, pts);
    if (haveNext) {
        // 下一張的格：假設這一張照 p 交出（鎖相從這一張接著推）
        Lock after;
        after.update(p.slotDisplayNs, pts);
        const Plan n = plan(g, nextTdNs, nowNs, renderBudgetNs, lock != nullptr ? &after : nullptr, nextPts);
        // 下一張落在同一格或更早（幀率比顯示高、或時鐘跳了）→ 這一張不會被看見
        if (n.slot <= p.slot) {
            return Action::Drop;
        }
        // 這一張晚了、下一張也晚了 → 跳過這一張
        if (p.late && n.late) {
            return Action::Drop;
        }
    }
    if (p.late) {
        return Action::Submit;
    }
    if (nowNs >= p.submitAtNs) {
        return Action::Submit;
    }
    *waitUntilNs = p.submitAtNs;
    return Action::Wait;
}

// 模擬：固定幀率的片源在固定週期的顯示上，每張被看見幾格（給自測，也給 log 的「節拍是否均勻」說明用）。
// 回傳每張的格數；jitterNs＞0 時每張的交出時刻照固定順序晃動（決定性，不用亂數）
inline std::vector<int> simulate(double fps, double hz, int frames, uint64_t aheadNs, uint64_t jitterNs,
                                 uint64_t clockJitterNs = 0, double clockDrift = 0.0, bool useLock = true)
{
    Grid g;
    g.periodNs = static_cast<uint64_t>(1e9 / hz + 0.5);
    g.wakeNs = 1000000000ull;
    g.displayNs = g.wakeNs + aheadNs;
    const uint64_t t0 = g.displayNs + 37 * g.periodNs + g.periodNs / 3;  // 第一張的時刻不貼在格點上
    std::vector<uint64_t> td(static_cast<size_t>(frames));
    for (int i = 0; i < frames; i++) {
        // 時鐘抖動（決定性的鋸齒）與速率差
        const double tNs = static_cast<double>(i) * 1e9 / fps * (1.0 + clockDrift);
        const int64_t cj = clockJitterNs == 0 ? 0
                                              : static_cast<int64_t>((i * 4099) % 13 - 6) * static_cast<int64_t>(clockJitterNs) / 6;
        td[static_cast<size_t>(i)] = static_cast<uint64_t>(static_cast<int64_t>(t0 + static_cast<uint64_t>(tNs + 0.5)) + cj);
    }
    std::vector<int> shown(static_cast<size_t>(frames), 0);
    std::vector<uint64_t> submitAt(static_cast<size_t>(frames), 0);
    std::vector<bool> dropped(static_cast<size_t>(frames), false);
    // 先算每張何時交出（呈現端照 decide() 的規則，假設來得及）
    Lock lock;
    for (int i = 0; i < frames; i++) {
        uint64_t wait = 0;
        const uint64_t j = jitterNs == 0 ? 0 : static_cast<uint64_t>((i * 7919) % 11) * jitterNs / 10;
        const double pts = static_cast<double>(i) / fps;
        const Lock* lk = useLock ? &lock : nullptr;
        const Plan p = plan(g, td[static_cast<size_t>(i)], 0, kRenderBudgetNs, lk, pts);
        const bool haveNext = i + 1 < frames;
        const Action a = decide(g, td[static_cast<size_t>(i)], haveNext, haveNext ? td[static_cast<size_t>(i) + 1] : 0,
                                p.submitAtNs + j, &wait, kRenderBudgetNs, lk, pts, haveNext ? (i + 1) / fps : 0.0);
        if (a == Action::Drop) {
            dropped[static_cast<size_t>(i)] = true;
            continue;
        }
        submitAt[static_cast<size_t>(i)] = p.submitAtNs + j;
        lock.update(p.slotDisplayNs, pts);
    }
    // 每一格挑影像時看「最後交出的那一張」
    const uint64_t endNs = td.back() + 2 * g.periodNs;
    int64_t ptr = -1, best = -1;
    for (int64_t k = 0;; k++) {
        const uint64_t latch = g.wakeNs + static_cast<uint64_t>(k) * g.periodNs;
        if (latch > endNs) {
            break;
        }
        while (ptr + 1 < frames &&
               (dropped[static_cast<size_t>(ptr + 1)] || submitAt[static_cast<size_t>(ptr + 1)] <= latch)) {
            ptr++;
            if (!dropped[static_cast<size_t>(ptr)]) {
                best = ptr;
            }
        }
        if (best >= 0) {
            shown[static_cast<size_t>(best)]++;
        }
    }
    return shown;
}

// 自測；回傳失敗數（簽名同 xrlatch::selfTest）
inline int selfTest(const std::function<void(const char* name, bool ok, const std::string& detail)>& report)
{
    int failures = 0;
    auto check = [&](const char* nm, bool ok, const std::string& detail) {
        if (!ok) {
            failures++;
        }
        report(nm, ok, detail);
    };
    auto counts = [](const std::vector<int>& v, int from, int to) {
        std::string s;
        for (int i = from; i < to && i < static_cast<int>(v.size()); i++) {
            s += std::to_string(v[static_cast<size_t>(i)]);
        }
        return s;
    };
    {
        // 24 fps @ 120 Hz：除了最後一張（後面沒有格可以數）每張都是 5 格；交出時刻晃 ±3 ms 也一樣
        const std::vector<int> a = simulate(24.0, 120.0, 240, 27000000, 0);
        const std::vector<int> b = simulate(24.0, 120.0, 240, 27000000, 3000000);
        bool ok = true;
        for (size_t i = 0; i + 1 < a.size(); i++) {
            ok = ok && a[i] == 5 && b[i] == 5;
        }
        check("playersync-24at120", ok, counts(a, 0, 20) + " / " + counts(b, 0, 20));
    }
    {
        // 60 fps @ 120 Hz：每張 2 格；30 fps：每張 4 格
        const std::vector<int> a = simulate(60.0, 120.0, 300, 27000000, 2000000);
        const std::vector<int> b = simulate(30.0, 120.0, 150, 27000000, 2000000);
        bool ok = true;
        for (size_t i = 0; i + 1 < a.size(); i++) {
            ok = ok && a[i] == 2;
        }
        for (size_t i = 0; i + 1 < b.size(); i++) {
            ok = ok && b[i] == 4;
        }
        check("playersync-60-30at120", ok, counts(a, 0, 20) + " / " + counts(b, 0, 20));
    }
    {
        // 25 fps @ 120 Hz：4.8 格——只能 4、5 交錯，總和對、每張只差一格
        const std::vector<int> a = simulate(25.0, 120.0, 250, 27000000, 1000000);
        int sum = 0, lo = 99, hi = 0;
        for (size_t i = 0; i + 1 < a.size(); i++) {
            sum += a[i];
            lo = a[i] < lo ? a[i] : lo;
            hi = a[i] > hi ? a[i] : hi;
        }
        const int want = static_cast<int>(std::lround(249 * 4.8));
        const bool ok = lo == 4 && hi == 5 && std::abs(sum - want) <= 1;
        char buf[80];
        std::snprintf(buf, sizeof(buf), "sum=%d want=%d range=%d..%d ", sum, want, lo, hi);
        check("playersync-25at120", ok, buf + counts(a, 0, 20));
    }
    {
        // 120 fps 片源 @ 90 Hz：一定要丟；剩下的每張 1 格、沒有 0 格又沒丟的
        const std::vector<int> a = simulate(120.0, 90.0, 360, 27000000, 0);
        int shown = 0;
        for (size_t i = 0; i + 1 < a.size(); i++) {
            shown += a[i] > 0 ? 1 : 0;
        }
        const bool ok = shown >= 265 && shown <= 272;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "shown=%d of 359", shown);
        check("playersync-120at90", ok, buf);
    }
    {
        // 時鐘抖 ±3 ms、速率差 0.1%：不鎖相會出現 4／6；鎖相時只有速率差累積滿一格的那一次換格（每次差一格）
        const std::vector<int> a = simulate(24.0, 120.0, 480, 27000000, 1000000, 3000000, 0.001, true);
        const std::vector<int> b = simulate(24.0, 120.0, 480, 27000000, 1000000, 3000000, 0.001, false);
        int offA = 0, offB = 0, bad = 0;
        for (size_t i = 0; i + 1 < a.size(); i++) {
            offA += a[i] != 5 ? 1 : 0;
            offB += b[i] != 5 ? 1 : 0;
            bad += (a[i] < 4 || a[i] > 6) ? 1 : 0;
        }
        const bool ok = offA <= 2 && bad == 0 && offB > offA;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "lock off5=%d nolock off5=%d ", offA, offB);
        check("playersync-lock", ok, buf + counts(a, 0, 30));
    }
    {
        // 晚了：下一張也晚 → 丟；下一張還來得及 → 照交；還早 → 等到上一格挑完之後
        Grid g;
        g.periodNs = 8333333;
        g.wakeNs = 10000000000ull;
        g.displayNs = g.wakeNs + 27000000;
        uint64_t wait = 0;
        const uint64_t now = g.wakeNs + 100000;
        const uint64_t nowLate = g.wakeNs + g.periodNs - 1000000;  // 下一格的挑影像也只剩 1 ms
        const Action late2 = decide(g, g.displayNs, true, g.displayNs + g.periodNs, nowLate, &wait);  // 這一格與下一格都晚
        const Action late1 = decide(g, g.displayNs, true, g.displayNs + 5 * g.periodNs, now, &wait);   // 下一張還早
        const Action early = decide(g, g.displayNs + 10 * g.periodNs, false, 0, now, &wait);
        const uint64_t want = g.wakeNs + 9 * g.periodNs + kGuardNs;
        const Action same = decide(g, g.displayNs + 10 * g.periodNs, true, g.displayNs + 10 * g.periodNs + 1000000, now, &wait);
        const bool ok = late2 == Action::Drop && late1 == Action::Submit && early == Action::Wait && same == Action::Drop;
        uint64_t w2 = 0;
        decide(g, g.displayNs + 10 * g.periodNs, false, 0, now, &w2);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "late2=%d late1=%d early=%d same=%d waitErr=%lld ns", static_cast<int>(late2),
                      static_cast<int>(late1), static_cast<int>(early), static_cast<int>(same),
                      static_cast<long long>(static_cast<int64_t>(w2 - want)));
        check("playersync-decide", ok && w2 == want, buf);
    }
    return failures;
}

}  // namespace playersync
