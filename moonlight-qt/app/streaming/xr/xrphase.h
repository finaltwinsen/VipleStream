// VipleStream 2.0 §VR-XRLOOP — 相位換算與延遲鏈的小工具（`[VIPLE-VR-XRLOOP]`、`[VIPLE-VR-LDIAG]` 的 events、
// `[VIPLE-XR] 10s video` 的相位欄位）。
//
// header-only，只用標準函式庫（不依賴 Qt／OpenXR／libplacebo），做法同 xrlatchstats.h：沒有 OpenXR 的建置也編得到，
// `xr-probe --selftest-latch` 會接著跑這裡的 selfTest()，獨立的 g++ 測試也只 include 這一個檔。
//
// 兩種相位座標，單位都是微秒、範圍都是 [0, 顯示週期)：
//   LDIAG 相位 ：原點是預測顯示時刻（xrWaitFrame 給的 predictedDisplayTime）的格點。[VIPLE-VR-LDIAG] 的 latchPhase、
//                gpu-by-phase 與 [VIPLE-VR-XRLOOP]、10s video 新增的相位欄位都是這個座標。
//   vblank 相位：原點是那一格開頭的 vblank。頭盔上 gpu-trace 的時間軸用這個座標。
// 換算：vblank 相位＝(LDIAG 相位＋kPdtAfterVblankUs) mod 週期。
//
// 用之前要知道的限制：kPdtAfterVblankUs 與下面那幾個相位窗都只在 Steam Frame 的 120 Hz（SteamVR 2.18.2）量過。
// 別的更新率、別的頭盔不適用——用到 vblank 相位的判斷一律先看 is120Hz()，不成立就不要算。
//
// 另外有 RankScan：10 秒行要印的那些分位數用它算（一次掃過去，不排序）。相位是環，它的分位數不可以照數值的大小取——
// 先用 phaseCut() 把環從這個視窗的中心的正對面切開（phaseMedianUs／phaseNthFromCut）。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace xrphase {

// 預測顯示時刻落在某一次 vblank 之後多久（六份 gpu-trace 都是 3.712～3.717 ms）
constexpr uint32_t kPdtAfterVblankUs = 3716;
// 一張影像的 GPU 工作「交出」（最後一筆送出去）的時刻落在這兩段 vblank 相位，就會撞上頭盔合成器自己的那一筆：
// [Lo, Mid) 是做到一半被插隊，[Mid, Hi) 是排在它後面。全速、延後挑影像 6 ms 量的；半速＋補幀時被擋的那一段
// 早 0.25～0.5 ms，判讀時要記得
constexpr uint32_t kCollideLoUs = 2000, kCollideMidUs = 3600, kCollideHiUs = 5200;

// 顯示週期是不是 120 Hz（±2%）
inline bool is120Hz(uint64_t periodUs)
{
    return periodUs >= 8170 && periodUs <= 8500;
}

// 時刻 t 的 LDIAG 相位。grid＝任何一格的預測顯示時刻（和 t 同一個時鐘；只拿它對週期取餘，哪一格都一樣），
// t 比 grid 早也回 [0, period)。period＝0 回 0
inline uint32_t phaseUs(uint64_t tUs, uint64_t gridUs, uint32_t periodUs)
{
    if (periodUs == 0) {
        return 0;
    }
    if (tUs >= gridUs) {
        return static_cast<uint32_t>((tUs - gridUs) % periodUs);
    }
    const uint32_t back = static_cast<uint32_t>((gridUs - tUs) % periodUs);
    return back == 0 ? 0u : periodUs - back;
}

inline uint32_t ldiagToVblankUs(uint32_t ldiagUs, uint32_t periodUs)
{
    if (periodUs == 0) {
        return 0;
    }
    return static_cast<uint32_t>((static_cast<uint64_t>(ldiagUs) + kPdtAfterVblankUs) % periodUs);
}

inline uint32_t vblankToLdiagUs(uint32_t vblankUs, uint32_t periodUs)
{
    if (periodUs == 0) {
        return 0;
    }
    const uint64_t back = kPdtAfterVblankUs % periodUs;
    return static_cast<uint32_t>((static_cast<uint64_t>(vblankUs % periodUs) + periodUs - back) % periodUs);
}

// 交出相位（vblank 相位）會不會撞上合成器：Clear＝不會、Preempt＝做到一半被插隊、Queued＝排在它後面
enum class Handoff { Clear, Preempt, Queued };

inline Handoff handoffClass(uint32_t vblankPhaseUs)
{
    if (vblankPhaseUs >= kCollideLoUs && vblankPhaseUs < kCollideMidUs) {
        return Handoff::Preempt;
    }
    if (vblankPhaseUs >= kCollideMidUs && vblankPhaseUs < kCollideHiUs) {
        return Handoff::Queued;
    }
    return Handoff::Clear;
}

// 延遲鏈：解碼（DU 進佇列到解出）＋render（影像 render thread 開始處理到 GPU 做完）＋slack（GPU 做完到 latch，
// 可以是負的）＋lead（latch 到預測顯示時刻）。各項都是微秒，可以直接相加
inline int64_t chainSumUs(int64_t decUs, int64_t renderUs, int64_t slackUs, int64_t leadUs)
{
    return decUs + renderUs + slackUs + leadUs;
}

// xrlatch::Counters 換算成「多出來的重複顯示格數」：late、gap 是串流幀數（一張佔 stride 格），lateShift、early 是
// 顯示格數。這個總和不受 Classifier 的基準把同一種事件貼成 early 還是 lateShift 影響。stride＝0 當成 1
inline uint32_t eventsTotal(uint32_t late, uint32_t lateShift, uint32_t gap, uint32_t early, uint32_t stride)
{
    const uint64_t s = stride == 0 ? 1u : stride;
    const uint64_t total = static_cast<uint64_t>(lateShift) + early + s * (static_cast<uint64_t>(late) + gap);
    return static_cast<uint32_t>((std::min)(total, static_cast<uint64_t>(0xffffffffu)));
}

// 相位落在 [lo, hi) 這一段裡的話，離這一段結束還有多久；不在裡面回 0
inline uint32_t deferWaitUs(uint32_t vblankPhaseUs, uint32_t loUs, uint32_t hiUs)
{
    return (vblankPhaseUs >= loUs && vblankPhaseUs < hiUs) ? hiUs - vblankPhaseUs : 0u;
}

// 同場切換（§VR-LATCH A/B）：一個統計視窗裡「開」那一邊跑了 onFrames 格、「關」那一邊跑了 offFrames 格，這個視窗算
// 哪一邊。少的那一邊不超過 1%（120 Hz 的 10 秒視窗是 12 格）就算多的那一邊——'1'＝開、'0'＝關；否則兩邊混在一起，
// 回 'm'（不拿來比）。一格都沒有也回 'm'
inline char abWindowSide(uint32_t onFrames, uint32_t offFrames)
{
    const uint64_t total = static_cast<uint64_t>(onFrames) + offFrames;
    if (total == 0) {
        return 'm';
    }
    if (static_cast<uint64_t>(offFrames) * 100u <= total) {
        return '1';
    }
    if (static_cast<uint64_t>(onFrames) * 100u <= total) {
        return '0';
    }
    return 'm';
}

// 一個視窗（一千多筆整數，通常是微秒）的分位數：掃一遍建立計數，之後每個分位數只要掃計數，不排序、不複製。
// 為什麼不逐一 nth_element：印 10 秒行的那一格要十幾個分位數，1200 筆每挑一個約 10 µs，加起來在開發機上是
// 0.2 ms（頭盔更久），而那一格離下一次 xrWaitFrame 放行只有一毫秒多。一個視窗裡的數值通常只差幾百到幾千，
// 用計數的做法掃一次、取三個分位數是 3～6 µs（開發機，1200 筆）。
// 數值散得越開越久——每取一個分位數要從最小值掃到它所在的位置：散滿一個顯示週期（相位就是這樣）約 10 µs，
// 散滿整個計數範圍約 17 µs，和複製一份再 nth_element（約 26 µs）同一個量級，不是「幾微秒」。
//  - 結果和「排序之後取第 k 個」完全相同。at(p) 的位置同 XrContext 的 pctOf：第 min(n−1, ⌊p×(n−1)+0.5⌋) 個（從 0 算）。
//  - 比最小值大 kSpan 以上的離群值另外放一個清單排序；視窗裡大半都是離群值時（很少見）就等於整個排序。
//  - 內部的緩衝區留著重用：物件當成員留著，不要每次建；prepare() 先把緩衝區配好。
class RankScan
{
public:
    static constexpr int64_t kSpan = 16384;

    void prepare()
    {
        if (m_Count.empty()) {
            m_Count.assign(static_cast<size_t>(kSpan), 0u);
        }
        m_Over.reserve(64);
    }

    template <typename T>
    void scan(const std::vector<T>& v)
    {
        std::fill(m_Count.begin(), m_Count.begin() + static_cast<std::ptrdiff_t>(m_Used), 0u);  // 只清上一次用到的範圍
        m_Used = 0;
        m_InRange = 0;
        m_Over.clear();
        m_N = v.size();
        m_Min = m_Max = 0;
        if (v.empty()) {
            return;
        }
        int64_t lo = static_cast<int64_t>(v[0]), hi = lo;
        for (const T x : v) {
            lo = (std::min)(lo, static_cast<int64_t>(x));
            hi = (std::max)(hi, static_cast<int64_t>(x));
        }
        m_Min = lo;
        m_Max = hi;
        prepare();
        m_Used = static_cast<size_t>((std::min)(hi - lo + 1, kSpan));
        for (const T x : v) {
            const int64_t off = static_cast<int64_t>(x) - lo;
            if (off < kSpan) {
                m_Count[static_cast<size_t>(off)]++;
                m_InRange++;
            }
            else {
                m_Over.push_back(static_cast<int64_t>(x));
            }
        }
        std::sort(m_Over.begin(), m_Over.end());
    }

    size_t count() const { return m_N; }
    int64_t minValue() const { return m_Min; }  // 沒有資料時 0
    int64_t maxValue() const { return m_Max; }

    // 由小到大第 idx 個（從 0 算；超過就給最後一個）。沒有資料回 0
    int64_t nth(size_t idx) const
    {
        if (m_N == 0) {
            return 0;
        }
        idx = (std::min)(idx, m_N - 1);
        if (idx >= m_InRange) {
            return m_Over[idx - m_InRange];
        }
        size_t seen = 0;
        for (size_t i = 0; i < m_Used; i++) {
            seen += m_Count[i];
            if (seen > idx) {
                return m_Min + static_cast<int64_t>(i);
            }
        }
        return m_Max;  // 走不到這裡
    }

    // 第 p 分位數（p 在 0～1 之外就夾住）。沒有資料回 0
    int64_t at(double p) const
    {
        if (m_N == 0) {
            return 0;
        }
        p = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
        return nth(static_cast<size_t>(p * static_cast<double>(m_N - 1) + 0.5));
    }

private:
    std::vector<uint32_t> m_Count;  // m_Count[i]＝值等於 m_Min＋i 的筆數
    std::vector<int64_t> m_Over;    // 比 m_Min 大 kSpan 以上的，排好序
    size_t m_Used = 0;              // m_Count 這一次用到前幾格
    size_t m_N = 0, m_InRange = 0;
    int64_t m_Min = 0, m_Max = 0;
};

// 相位是環，不能照數值的大小取分位數：一群樣本橫跨 0／週期的接縫時，照數值排會被切成頭尾兩群，中位數落在兩群的
// 交界、不是那一群的中心（例：中心在接縫之後 0.4 ms、前後各散 2 ms 的一群，照數值取的中位數是 2.0 ms）。沒有哪一群
// 正好跨在接縫上也一樣：樣本散滿大半圈時（影像做完的相位就是這樣），照數值取的中位數看接縫落在哪裡，可以差到一毫秒。
// 要取的是環狀的中位數——到所有樣本的環上距離總和最小的位置，也就是前半圈、後半圈各有一半樣本的那一點。它不看
// 接縫在哪裡：整個視窗的資料轉一個角度，它就跟著轉同樣的角度。
// 做法：先估出這個視窗的中心在哪一格（detail::phaseCentreBin），把環從中心的正對面切開，從切點往後排——值不小於
// 切點的在前，比切點小的（繞過接縫的那一部分）在後——再照這個順序取第幾個（取到的是真的樣本值）。
struct PhaseCut {
    uint32_t cutUs = 0;  // 切點，落在 [0, period)。0＝就切在接縫上（等於照數值排）
    size_t below = 0;    // 值比切點小的筆數：從切點往後排時，排在最後面的那一段
};

// 估中心用的格數（120 Hz 時一格約 130 µs）。要是偶數
constexpr size_t kPhaseCutBins = 64;

namespace detail {

// 一個視窗的相位（µs）落在 kPhaseCutBins 格裡各有幾筆。值不在 [0, period) 的樣本（更新率剛換的那幾張）算在最後一格。
// 一筆只做一次乘法、不做除法；剛好在格線上的值可能歸到前一格，這裡只拿來估中心，無妨。period 不可以是 0
inline void phaseBins(const std::vector<uint32_t>& phases, uint32_t periodUs, uint32_t bins[kPhaseCutBins])
{
    std::fill(bins, bins + kPhaseCutBins, 0u);
    const uint64_t scale = (static_cast<uint64_t>(kPhaseCutBins) << 32) / periodUs;
    for (const uint32_t x : phases) {
        bins[static_cast<size_t>(((std::min)(x, periodUs - 1) * scale) >> 32)]++;
    }
}

// 這個視窗的中心在哪一條格線上（第 k 條格線＝第 k 格的起點）：到所有樣本的環上距離總和最小的那一條，樣本算在它那一格
// 的正中間；一樣小取最前面的。也就是以格為單位取的環狀中位數。
// 格線往前走一格，總和只差「後半圈的筆數 − 前半圈的筆數」：前面那半圈的格（k＋1 … k＋half−1）各近一格，後面那半圈的
// （k＋half＋1 … k＋n−1）各遠一格，緊鄰的第 k 格與正對面的第 k＋half 格距離不變——所以掃一圈就好，不必每條格線重算
inline size_t phaseCentreBin(const uint32_t bins[kPhaseCutBins])
{
    const size_t n = kPhaseCutBins, half = kPhaseCutBins / 2;
    int64_t ahead = 0, behind = 0;
    for (size_t o = 1; o < half; o++) {
        ahead += bins[o];
        behind += bins[half + o];
    }
    int64_t cost = 0, bestCost = 0;  // 相對於第 0 條格線的總和
    size_t best = 0;
    for (size_t k = 0; k + 1 < n; k++) {
        cost += behind - ahead;
        ahead += static_cast<int64_t>(bins[(k + half) % n]) - static_cast<int64_t>(bins[(k + 1) % n]);
        behind += static_cast<int64_t>(bins[k]) - static_cast<int64_t>(bins[(k + half + 1) % n]);
        if (cost < bestCost) {
            bestCost = cost;
            best = k + 1;
        }
    }
    return best;
}

}  // namespace detail

// 一個視窗的相位（µs，[0, period)）要從哪裡切開：它的中心的正對面。樣本擠成一群時切點落在那一群的對面，從切點往後排
// 就是照數值排、只是不會被接縫切開。period＝0 或沒有資料：切在 0
inline PhaseCut phaseCut(const std::vector<uint32_t>& phases, uint32_t periodUs)
{
    PhaseCut out;
    if (periodUs == 0 || phases.empty()) {
        return out;
    }
    uint32_t bins[kPhaseCutBins];
    detail::phaseBins(phases, periodUs, bins);
    const size_t cutBin = (detail::phaseCentreBin(bins) + kPhaseCutBins / 2) % kPhaseCutBins;
    out.cutUs = static_cast<uint32_t>(static_cast<uint64_t>(cutBin) * periodUs / kPhaseCutBins);
    for (const uint32_t x : phases) {
        out.below += x < out.cutUs ? 1u : 0u;
    }
    return out;
}

// 從切點往後數第 idx 個（從 0 算；超過就給最後一個）。scanned 要先對同一個視窗 scan() 過。回傳的值沒有折回：落在
// [cut, cut＋period)，比切點小的那一段加了一個週期——取 p95、max 這種「比中位數晚多少」的量時才不會倒過來。
// 沒有資料回 0
inline int64_t phaseNthFromCut(const RankScan& scanned, const PhaseCut& cut, uint32_t periodUs, size_t idx)
{
    const size_t n = scanned.count();
    if (n == 0) {
        return 0;
    }
    idx = (std::min)(idx, n - 1);
    const size_t below = (std::min)(cut.below, n);
    const size_t atOrAbove = n - below;  // 值不小於切點的筆數：照數值排的話它們排在後面，從切點往後排的話在前面
    if (idx < atOrAbove) {
        return scanned.nth(below + idx);
    }
    return scanned.nth(idx - atOrAbove) + static_cast<int64_t>(periodUs);
}

// 從切點往後排的第 p 分位數（位置的取法同 RankScan::at），一樣沒有折回。沒有資料回 0
inline int64_t phaseAtFromCut(const RankScan& scanned, const PhaseCut& cut, uint32_t periodUs, double p)
{
    const size_t n = scanned.count();
    if (n == 0) {
        return 0;
    }
    p = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
    return phaseNthFromCut(scanned, cut, periodUs, static_cast<size_t>(p * static_cast<double>(n - 1) + 0.5));
}

// 一個視窗的相位的中位數（µs，折回 [0, period)）：從切點往後數第 n/2 個——位置同 [VIPLE-XR] 10s video 既有的 p50。
// rank 是呼叫端留著重用的 RankScan（這裡會重掃一次）。沒有資料回 0；period＝0 就是照數值取
inline uint32_t phaseMedianUs(RankScan& rank, const std::vector<uint32_t>& phases, uint32_t periodUs)
{
    rank.scan(phases);
    const int64_t v = phaseNthFromCut(rank, phaseCut(phases, periodUs), periodUs, phases.size() / 2);
    return static_cast<uint32_t>(periodUs == 0 ? v : v % static_cast<int64_t>(periodUs));
}

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
    char buf[200];
    const uint32_t T = 8333;  // 120 Hz

    {
        // latch：LDIAG 2750 → 距 vblank 6.466 ms
        const uint32_t v = ldiagToVblankUs(2750, T);
        std::snprintf(buf, sizeof(buf), "ldiag 2750 -> vblank %u us", v);
        caseDone("phase-ldiag-to-vblank-latch", v == 6466, buf);
    }
    {
        // xrWaitFrame 放行：LDIAG 4840 → 距 vblank 0.223 ms（加了 3716 之後繞過一個週期）
        const uint32_t v = ldiagToVblankUs(4840, T);
        std::snprintf(buf, sizeof(buf), "ldiag 4840 -> vblank %u us", v);
        caseDone("phase-ldiag-to-vblank-release", v == 223, buf);
    }
    {
        const uint32_t a = vblankToLdiagUs(6466, T), b = vblankToLdiagUs(223, T);
        std::snprintf(buf, sizeof(buf), "vblank 6466 -> ldiag %u us, vblank 223 -> ldiag %u us", a, b);
        caseDone("phase-vblank-to-ldiag", a == 2750 && b == 4840, buf);
    }
    {
        const bool yes = is120Hz(8333) && is120Hz(8170) && is120Hz(8500);
        const bool no = !is120Hz(8169) && !is120Hz(8501) && !is120Hz(11111) && !is120Hz(13889);
        caseDone("phase-is-120hz", yes && no, yes && no ? "8170-8500 us only" : "wrong range");
    }
    {
        const uint64_t grid = 5000000;
        const uint32_t a = phaseUs(grid + 3ull * T + 100, grid, T), b = phaseUs(grid - 100, grid, T), c = phaseUs(grid, grid, T);
        // t 比格點早整數個週期也是 0；週期 0 不可以除
        const uint32_t d = phaseUs(grid - 2ull * T, grid, T), e = phaseUs(grid + 123, grid, 0);
        std::snprintf(buf, sizeof(buf), "grid+3T+100 -> %u, grid-100 -> %u, grid -> %u, grid-2T -> %u, period 0 -> %u", a, b, c, d, e);
        caseDone("phase-of-time", a == 100 && b == 8233 && c == 0 && d == 0 && e == 0, buf);
    }
    {
        const bool ok = handoffClass(1999) == Handoff::Clear && handoffClass(2000) == Handoff::Preempt &&
                        handoffClass(3599) == Handoff::Preempt && handoffClass(3600) == Handoff::Queued &&
                        handoffClass(5199) == Handoff::Queued && handoffClass(5200) == Handoff::Clear &&
                        handoffClass(8000) == Handoff::Clear && handoffClass(0) == Handoff::Clear;
        caseDone("phase-handoff-class", ok, ok ? "clear <2000, preempt <3600, queued <5200, clear" : "wrong boundary");
    }
    {
        const int64_t a = chainSumUs(900, 1700, 4200, 22260), b = chainSumUs(900, 1700, -300, 22260);
        std::snprintf(buf, sizeof(buf), "900+1700+4200+22260 = %lld, with slack -300 = %lld", static_cast<long long>(a),
                      static_cast<long long>(b));
        caseDone("phase-chain-sum", a == 29060 && b == 24560, buf);
    }
    {
        const uint32_t a = eventsTotal(2, 10, 1, 5, 1), b = eventsTotal(2, 10, 1, 5, 2);
        // stride 0（還沒估出來）當成 1；總和超過 32 bit 就停在上限
        const uint32_t c = eventsTotal(2, 10, 1, 5, 0), d = eventsTotal(0xffffffffu, 1, 0xffffffffu, 1, 4);
        std::snprintf(buf, sizeof(buf), "stride 1 -> %u, stride 2 -> %u, stride 0 -> %u, overflow -> %u", a, b, c, d);
        caseDone("phase-events-total", a == 18 && b == 21 && c == 18 && d == 0xffffffffu, buf);
    }
    {
        const uint32_t a = deferWaitUs(1500, 700, 5300), b = deferWaitUs(600, 700, 5300), c = deferWaitUs(5300, 700, 5300),
                       d = deferWaitUs(5299, 700, 5300), e = deferWaitUs(700, 700, 5300);
        std::snprintf(buf, sizeof(buf), "1500 -> %u, 600 -> %u, 5300 -> %u, 5299 -> %u, 700 -> %u", a, b, c, d, e);
        caseDone("phase-defer-wait", a == 3800 && b == 0 && c == 0 && d == 1 && e == 4600, buf);
    }
    {
        // 兩個方向互為反函數（120 Hz 的整個週期，以及別的更新率——那裡的數字沒有物理意義，但換算不可以出界）
        bool ok = true;
        uint32_t bad = 0, badPeriod = 0;
        for (uint32_t period : {8333u, 8170u, 8500u, 11111u, 13889u, 6944u, 3000u}) {
            for (uint32_t x = 0; x < period; x++) {
                const uint32_t v = ldiagToVblankUs(x, period);
                if (v >= period || vblankToLdiagUs(v, period) != x) {
                    ok = false;
                    bad = x;
                    badPeriod = period;
                }
            }
        }
        std::snprintf(buf, sizeof(buf), "mismatch at phase %u of period %u us", bad, badPeriod);
        caseDone("phase-roundtrip", ok, ok ? "every phase of 7 periods maps back" : buf);
    }
    {
        // RankScan：每個位置都要和「排序之後取第 k 個」相同。同一個物件接連掃形狀不同的視窗（緩衝區重用）：
        // 擠在一起的、帶幾個很大的離群值的、有負值的、最小值離其他值很遠的（大半落在計數範圍外）、全部相同的、
        // 1 筆、2 筆、空的
        uint32_t rng = 0x2545F491u;
        auto rnd = [&rng]() {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };
        RankScan scan;
        int checks = 0, mismatches = 0;
        auto check = [&](const std::vector<int32_t>& v) {
            std::vector<int32_t> sorted = v;
            std::sort(sorted.begin(), sorted.end());
            scan.scan(v);
            bool ok = scan.count() == v.size() && scan.minValue() == (v.empty() ? 0 : sorted.front()) &&
                      scan.maxValue() == (v.empty() ? 0 : sorted.back());
            for (size_t i = 0; i < v.size() + 2; i++) {  // 超過最後一個位置給最後一個
                const int64_t want = v.empty() ? 0 : sorted[(std::min)(i, v.size() - 1)];
                ok = ok && scan.nth(i) == want;
            }
            for (double p : {-0.5, 0.0, 0.05, 0.25, 0.5, 0.95, 0.99, 1.0, 1.5}) {
                const double q = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
                const int64_t want =
                    v.empty() ? 0 : sorted[(std::min)(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
                ok = ok && scan.at(p) == want;
            }
            checks++;
            mismatches += ok ? 0 : 1;
        };
        std::vector<int32_t> v;
        for (int round = 0; round < 40; round++) {
            v.clear();
            const int n = round % 4 == 3 ? 37 : 1200;
            for (int i = 0; i < n; i++) {
                int32_t x = 4700 + static_cast<int32_t>(rnd() % 300);  // 擠在一起
                if (round % 5 == 1 && rnd() % 100 == 0) {
                    x = 100000 + static_cast<int32_t>(rnd() % 900000);  // 幾個很大的離群值
                }
                if (round % 5 == 2) {
                    x = static_cast<int32_t>(rnd() % 6000) - 1500;  // 有負值、散得開
                }
                if (round % 5 == 3) {
                    x = i == 0 ? -20 : 90000 + static_cast<int32_t>(rnd() % 500);  // 最小值離其他值很遠
                }
                if (round % 5 == 4) {
                    x = static_cast<int32_t>(rnd() % 40000);  // 範圍比計數的上限大
                }
                v.push_back(x);
            }
            check(v);
        }
        check(std::vector<int32_t>(500, 8333));
        check(std::vector<int32_t>{42});
        check(std::vector<int32_t>{7, -7});
        check(std::vector<int32_t>());
        check(std::vector<int32_t>{5, 5 + static_cast<int32_t>(RankScan::kSpan) - 1, 5 + static_cast<int32_t>(RankScan::kSpan)});  // 計數範圍的邊界
        // uint32 的視窗（相位、時間長度）走同一份
        const std::vector<uint32_t> u = {8332, 0, 4840, 4841, 4839, 3000000000u};
        scan.scan(u);
        const bool unsignedOk = scan.nth(0) == 0 && scan.nth(2) == 4840 && scan.at(0.5) == 4841 && scan.nth(4) == 8332 &&
                                scan.maxValue() == 3000000000ll;
        std::snprintf(buf, sizeof(buf), "%d windows checked against a sorted copy, %d mismatch(es)%s", checks, mismatches,
                      unsignedOk ? "" : ", uint32 window wrong");
        caseDone("rank-scan-matches-sort", mismatches == 0 && unsignedOk, buf);
    }

    // ── 相位的分位數（環狀）──
    auto ringDist = [](uint32_t a, uint32_t b, uint32_t period) {  // 兩個相位在環上差多少
        const uint32_t d = a > b ? a - b : b - a;
        return (std::min)(d, period - d);
    };
    {
        // 估中心的那一步（detail::phaseCentreBin，掃一圈累加）要和照定義算的選到同一條格線：每條格線到每一格正中間的
        // 環上距離 × 那一格的筆數，取總和最小的、一樣小取最前面的。各種形狀：一群、兩群、散滿、只有一格有、全空
        uint32_t rng = 0x1234567u;
        auto rnd = [&rng]() {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };
        const size_t nb = kPhaseCutBins;
        int checked = 0, wrong = 0;
        for (int round = 0; round < 500; round++) {
            uint32_t bins[kPhaseCutBins] = {};
            const int kind = round % 5;
            if (kind == 0 || kind == 1) {
                const size_t centre = rnd() % nb, width = 1 + rnd() % 24;
                for (int i = 0; i < 600; i++) {
                    bins[(centre + rnd() % width) % nb]++;
                }
                if (kind == 1) {  // 另一群，小一點
                    const size_t other = (centre + 8 + rnd() % 40) % nb;
                    for (int i = 0; i < 250; i++) {
                        bins[(other + rnd() % 4) % nb]++;
                    }
                }
            }
            else if (kind == 2) {
                for (int i = 0; i < 600; i++) {
                    bins[rnd() % nb]++;
                }
            }
            else if (kind == 3) {
                bins[rnd() % nb] = 1 + rnd() % 1000;
            }
            size_t want = 0;
            uint64_t wantCost = ~static_cast<uint64_t>(0);
            for (size_t k = 0; k < nb; k++) {
                uint64_t cost = 0;
                for (size_t b = 0; b < nb; b++) {
                    const size_t twice = (2 * b + 1 + 2 * nb - 2 * k) % (2 * nb);  // 2 ×（格的正中間 − 格線），往前量
                    cost += static_cast<uint64_t>(bins[b]) * (std::min)(twice, 2 * nb - twice);
                }
                if (cost < wantCost) {
                    wantCost = cost;
                    want = k;
                }
            }
            wrong += detail::phaseCentreBin(bins) == want ? 0 : 1;
            checked++;
        }
        std::snprintf(buf, sizeof(buf), "%d histograms checked against the definition, %d mismatch(es)", checked, wrong);
        caseDone("phase-centre-matches-definition", wrong == 0, buf);
    }
    {
        // 橫跨 0／週期接縫的一群。照數值排是 10、100、200、8200、8300，中間那個是 200；從它的中心的正對面切開再排是
        // 8200、8300、10、100、200，中間是 10
        RankScan rank;
        const std::vector<uint32_t> v = {8200, 8300, 10, 100, 200};
        const PhaseCut cut = phaseCut(v, T);
        const uint32_t m = phaseMedianUs(rank, v, T);
        std::snprintf(buf, sizeof(buf), "{8200, 8300, 10, 100, 200} -> %u us (cut at %u us, %u below it)", m, cut.cutUs,
                      static_cast<unsigned>(cut.below));
        caseDone("phase-median-across-seam", m == 10 && cut.cutUs == 4166 && cut.below == 3, buf);
    }
    {
        // 一群 600 筆（中心 5600、前後各散 1500，沒有跨接縫）：和照數值取的中位數相同。整群轉任何角度——包括轉到
        // 跨接縫的位置——中位數都跟著轉同樣的角度；照數值取的在跨接縫的那些角度會差到一毫秒以上
        uint32_t rng = 0x9E3779B9u;
        auto rnd = [&rng]() {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };
        RankScan rank;
        std::vector<uint32_t> base, turned, sorted;
        for (int i = 0; i < 600; i++) {
            base.push_back(4100 + rnd() % 3001);
        }
        sorted = base;
        std::sort(sorted.begin(), sorted.end());
        const uint32_t m0 = phaseMedianUs(rank, base, T);
        bool ok = m0 == sorted[sorted.size() / 2];
        uint32_t turns = 0, wrong = 0, plainWorst = 0;
        for (uint32_t turn = 0; turn < T; turn += 97) {
            turned.clear();
            for (const uint32_t x : base) {
                turned.push_back((x + turn) % T);
            }
            const uint32_t want = (m0 + turn) % T;
            wrong += phaseMedianUs(rank, turned, T) == want ? 0u : 1u;
            sorted = turned;
            std::sort(sorted.begin(), sorted.end());
            plainWorst = (std::max)(plainWorst, ringDist(sorted[sorted.size() / 2], want, T));
            turns++;
        }
        ok = ok && wrong == 0 && plainWorst > 1000;
        std::snprintf(buf, sizeof(buf), "%u turns of one cluster, %u wrong; a plain median would be off by up to %u us", turns, wrong,
                      plainWorst);
        caseDone("phase-median-turns-with-data", ok, buf);
    }
    {
        // 影像做完的相位實際上散滿整圈：六成在一段 3 ms 裡，其餘分成兩小群在它的前後（頭盔合成器那一筆的前後）。
        // 這種形狀的資料，照數值取的中位數看接縫落在哪裡可以差一毫秒以上；環狀的中位數跟著資料轉，只差在估中心的
        // 那一格以內（不到 0.2 ms）
        uint32_t rng = 0x6C8E9CF5u;
        auto rnd = [&rng]() {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };
        RankScan rank;
        std::vector<uint32_t> base, turned, sorted;
        for (int i = 0; i < 600; i++) {
            const uint32_t pick = rnd() % 100;
            base.push_back(pick < 62 ? 5335 + rnd() % 3000 : (pick < 84 ? 752 + rnd() % 2000 : 2585 + rnd() % 2750));
        }
        for (uint32_t& x : base) {
            x %= T;
        }
        const uint32_t m0 = phaseMedianUs(rank, base, T);
        uint32_t ringWorst = 0, plainWorst = 0;
        for (uint32_t turn = 0; turn < T; turn += 97) {
            turned.clear();
            for (const uint32_t x : base) {
                turned.push_back((x + turn) % T);
            }
            const uint32_t want = (m0 + turn) % T;
            ringWorst = (std::max)(ringWorst, ringDist(phaseMedianUs(rank, turned, T), want, T));
            sorted = turned;
            std::sort(sorted.begin(), sorted.end());
            plainWorst = (std::max)(plainWorst, ringDist(sorted[sorted.size() / 2], want, T));
        }
        std::snprintf(buf, sizeof(buf), "ring-shaped window: median %u us, off by at most %u us as the data turns (a plain median: up to %u us)",
                      m0, ringWorst, plainWorst);
        caseDone("phase-median-ring-shaped", m0 > 6335 && m0 < 7335 && ringWorst <= 200 && plainWorst > 1000, buf);
    }
    {
        // 補幀那一批做完的相位大概長這樣：一群在接縫之後 0.4 ms、前後各散 2 ms，另外有幾筆離群值散在對面（晚很多才
        // 做完的）。離群值分到前面或後面都只差幾個名次
        uint32_t rng = 0x2545F491u;
        auto rnd = [&rng]() {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };
        RankScan rank;
        std::vector<uint32_t> v, sorted;
        for (int i = 0; i < 580; i++) {
            v.push_back((T + 400 - 2000 + rnd() % 4001) % T);
        }
        for (int i = 0; i < 20; i++) {
            v.push_back(2600 + rnd() % 3900);
        }
        const uint32_t m = phaseMedianUs(rank, v, T);
        sorted = v;
        std::sort(sorted.begin(), sorted.end());
        const uint32_t plain = sorted[sorted.size() / 2];
        std::snprintf(buf, sizeof(buf), "cluster centred 400 us past the seam: %u us (a plain median reads %u us)", m, plain);
        caseDone("phase-median-outliers", ringDist(m, 400, T) <= 300 && ringDist(plain, 400, T) > 1000, buf);
    }
    {
        // 從切點往後數、不折回：xrWaitFrame 多半在相位 4800～5099 返回（這裡每個值各一筆），有兩格晚了 3.4 ms 以上、
        // 繞過格點（相位 207、300）。照數值取的 max 是那一群裡最大的（看不到晚的那兩格）；從切點往後數它們排在最後面，
        // 值是相位＋一個週期。302 筆：p50 是第 ⌊0.5×301＋0.5⌋＝151 個（4951）、p95 是第 ⌊0.95×301＋0.5⌋＝286 個（5086）
        RankScan rank;
        std::vector<uint32_t> v;
        for (uint32_t i = 0; i < 300; i++) {
            v.push_back(4800 + i);
        }
        v.push_back(207);
        v.push_back(300);
        rank.scan(v);
        const PhaseCut cut = phaseCut(v, T);
        const int64_t p50 = phaseAtFromCut(rank, cut, T, 0.50), p95 = phaseAtFromCut(rank, cut, T, 0.95);
        const int64_t last = phaseNthFromCut(rank, cut, T, v.size() - 1), beyond = phaseNthFromCut(rank, cut, T, v.size() + 5);
        const int64_t first = phaseNthFromCut(rank, cut, T, 0);
        std::snprintf(buf, sizeof(buf), "p50 %lld, p95 %lld, max %lld us (plain max %lld us), cut at %u us",
                      static_cast<long long>(p50), static_cast<long long>(p95), static_cast<long long>(last),
                      static_cast<long long>(rank.maxValue()), cut.cutUs);
        caseDone("phase-from-cut-unfolded",
                 first == 4800 && p50 == 4951 && p95 == 5086 && last == 300 + static_cast<int64_t>(T) && beyond == last &&
                     phaseAtFromCut(rank, cut, T, 1.0) == last && phaseAtFromCut(rank, cut, T, -1.0) == first &&
                     rank.maxValue() == 5099 && cut.cutUs == 781 && cut.below == 2,
                 buf);
    }
    {
        // 邊界：沒有資料；period＝0（照數值取）；每一格都一樣多（每條格線的總和相同，取第 0 條當中心 → 切在半個週期）；
        // 值超過週期的樣本（更新率剛換的那幾張）算在最後一格，不可以出界
        RankScan rank;
        const std::vector<uint32_t> none;
        const PhaseCut cutNone = phaseCut(none, T);
        rank.scan(none);
        bool ok = phaseMedianUs(rank, none, T) == 0 && cutNone.cutUs == 0 && cutNone.below == 0 &&
                  phaseNthFromCut(rank, cutNone, T, 3) == 0 && phaseAtFromCut(rank, cutNone, T, 0.5) == 0;
        const std::vector<uint32_t> seam = {8200, 8300, 10, 100, 200};
        const uint32_t noPeriod = phaseMedianUs(rank, seam, 0);
        ok = ok && noPeriod == 200 && phaseCut(seam, 0).cutUs == 0;
        std::vector<uint32_t> even;
        for (uint32_t i = 0; i < static_cast<uint32_t>(kPhaseCutBins); i++) {
            even.push_back(i * T / static_cast<uint32_t>(kPhaseCutBins) + 5);
        }
        const PhaseCut cutEven = phaseCut(even, T);
        const uint32_t evenMedian = phaseMedianUs(rank, even, T);
        ok = ok && cutEven.cutUs == T / 2 && cutEven.below == even.size() / 2 && evenMedian == even[0];
        const std::vector<uint32_t> mixed = {100, 200, 300, 9000, 12000, 4000000000u};
        const PhaseCut cutMixed = phaseCut(mixed, T);
        const uint32_t mixedMedian = phaseMedianUs(rank, mixed, T);
        ok = ok && cutMixed.cutUs < T && cutMixed.below <= mixed.size() && mixedMedian < T;
        // 剛好落在切點上的樣本排在最前面（「不小於切點」那一段的開頭）：4166、8200、8300、10、100 的中間是 8300
        const std::vector<uint32_t> onCut = {8200, 8300, 10, 100, 4166};
        const PhaseCut cutOnCut = phaseCut(onCut, T);
        ok = ok && cutOnCut.cutUs == 4166 && cutOnCut.below == 2 && phaseMedianUs(rank, onCut, T) == 8300;
        std::snprintf(buf, sizeof(buf), "empty -> 0, period 0 -> %u us, one sample per bin -> cut at %u us, out-of-range samples -> %u us",
                      noPeriod, cutEven.cutUs, mixedMedian);
        caseDone("phase-cut-edge-cases", ok, buf);
    }
    {
        // 同場切換：一個視窗算哪一邊。少的那一邊不超過 1% 才算多的那一邊（1200 格的視窗：12 格可以、13 格就是混合）
        const struct {
            uint32_t on, off;
            char want;
        } k[] = {{1200, 0, '1'},   {0, 1200, '0'},   {1199, 1, '1'},  {1, 1199, '0'}, {1188, 12, '1'}, {1187, 13, 'm'},
                 {12, 1188, '0'},  {13, 1187, 'm'},  {600, 600, 'm'}, {1, 0, '1'},    {0, 1, '0'},     {99, 1, '1'},
                 {98, 1, 'm'},     {0, 0, 'm'},      {4000000000u, 40000000u, '1'},   {4000000000u, 40404041u, 'm'},
                 {40000000u, 4000000000u, '0'},      {40404041u, 4000000000u, 'm'},   {4294967295u, 1, '1'}};
        // （最後五組：用 32 bit 算會溢位的大小）
        int wrong = 0;
        for (const auto& e : k) {
            wrong += abWindowSide(e.on, e.off) == e.want ? 0 : 1;
        }
        std::snprintf(buf, sizeof(buf), "%d of %d wrong; 1188+12 -> %c, 1187+13 -> %c, 13+1187 -> %c", wrong,
                      static_cast<int>(sizeof(k) / sizeof(k[0])), abWindowSide(1188, 12), abWindowSide(1187, 13), abWindowSide(13, 1187));
        caseDone("ab-window-side", wrong == 0, buf);
    }
    return failures;
}

}  // namespace xrphase
