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

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>

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
    return failures;
}

}  // namespace xrphase
