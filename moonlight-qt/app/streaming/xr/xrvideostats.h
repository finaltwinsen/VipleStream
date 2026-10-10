// VipleStream 2.0 §VR-XRLOOP／§VR-PASS — 影像 render thread 這一側的量測：`[VIPLE-XR] 10s video` 行尾那幾欄的 10 秒視窗
// （RenderDiag）與每個 pass 的 GPU 時間（PassStats，`[VIPLE-VR-PASS]`）。
//
// header-only，只用標準函式庫（不依賴 Qt／OpenXR／libplacebo），做法同 xrlatchstats.h、xrphase.h：沒有 OpenXR 的建置也編
// 得到，`xr-probe --selftest-latch` 會接著跑這裡的 selfTest()，獨立的 g++ 測試也只 include 這幾個檔。
//
// 分工：XrVideo（xrvideo.h／.cpp）只負責在對的時刻、對的執行緒、拿著對的鎖把數字推進來；分位數怎麼取、哪些情況印 -、
// 字串怎麼組、pass 的名稱怎麼認，都在這裡——這些才是寫錯了從 log 上看不出來的部分，所以放在自測跑得到的地方。
//
// 執行緒：兩個結構都沒有鎖。RenderDiag 由 XrVideo 的 m_StatsMutex 保護（影像 render thread 推、XR frame thread 整包換走
// 之後在鎖外組字串）；PassStats 只在影像 render thread 用。

#pragma once

#include "xrphase.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace xrvideostats {

// 影像 render thread 這一側的 10 s 視窗（[VIPLE-XR] 10s video 行尾那幾欄）。相位是 LDIAG 的座標（顯示格點之後多久，µs），
// 沒有格點時不記
struct RenderDiag {
    std::vector<uint32_t> mboxUs;            // 解碼幀在 mailbox 裡等了多久（放進去 → 影像 render thread 取出）
    std::vector<uint32_t> swWaitUs;          // xrWaitSwapchainImage 這個呼叫花多久
    std::vector<uint32_t> subPhaseUs;        // 送出（pl_vulkan_hold_ex 返回）的相位
    std::vector<uint32_t> donePhaseUs;       // GPU 做完的相位
    std::vector<uint32_t> synthSubPhaseUs;   // §VR-SYNTH：補幀那一批送出的相位
    std::vector<uint32_t> synthDonePhaseUs;  //            與做完的相位
    uint32_t phasePeriodUs = 0;   // 上面四組相位是對哪一個顯示週期取的（最近推進去的那一筆；0＝這個視窗還沒有）
    uint32_t swNotOk = 0;         // xrWaitSwapchainImage 回的不是 XR_SUCCESS 的次數
    uint32_t relMaxUs = 0;        // xrReleaseSwapchainImage 這個呼叫的最大值
    uint32_t handoffN = 0;        // 顯示是 120 Hz 時：分過類的張數，
    uint32_t handoffPreempt = 0;  //   其中送出的相位落在「做到一半會被合成器插隊」那一段的，
    uint32_t handoffQueued = 0;   //   與「排在合成器後面」那一段的（見 xrphase::handoffClass）
    xrphase::RankScan rank;       // statsTail 算分位數用的（緩衝區留著重用；整包交換時跟著換，無妨）
    void clear()
    {
        mboxUs.clear();
        swWaitUs.clear();
        subPhaseUs.clear();
        donePhaseUs.clear();
        synthSubPhaseUs.clear();
        synthDonePhaseUs.clear();
        phasePeriodUs = swNotOk = relMaxUs = handoffN = handoffPreempt = handoffQueued = 0;
    }
    // 120 Hz、10 s 約 1200 筆：先配好，之後往裡面推不必配置記憶體（clear() 與整包交換都留著空間）
    void reserveWindow()
    {
        for (std::vector<uint32_t>* v : {&mboxUs, &swWaitUs, &subPhaseUs, &donePhaseUs, &synthSubPhaseUs, &synthDonePhaseUs}) {
            v->reserve(2048);
        }
        rank.prepare();
    }
    // 組出 [VIPLE-XR] 10s video 行尾的那一段。每個視窗掃一遍取分位數（xrphase::RankScan，不排序）；位置和
    // XrVideo::takeStatsLine 既有的取法相同（p50＝第 n/2 個、p95＝第 n×95/100 個）。
    //   mbox＝解碼幀在 mailbox 裡等多久；swWait＝xrWaitSwapchainImage、swNotOk＝它回的不是 XR_SUCCESS 的次數；
    //   rel＝xrReleaseSwapchainImage；sub／done＝送出（這張影像最後一筆 GPU 工作送出去）與 GPU 做完的相位；
    //   coll／queued＝送出的相位落在「做到一半會被合成器插隊」「排在合成器後面」那兩段的比例。
    //   沒有樣本的相位（沒有顯示格點，例如 XR 桌面）與沒有分過類的比例（顯示不是 120 Hz）印 -
    // 相位的 p50 不是照數值的大小取的：相位是環，一群樣本橫跨 0／週期的接縫時（補幀那一批做完的時刻就落在那裡）
    // 照數值取會差好幾毫秒。取的是環狀的中位數（前半圈、後半圈各有一半樣本的那一點），見 xrphase::phaseCut
    std::string statsTail(bool synth)
    {
        auto p95index = [](size_t n) { return n == 0 ? static_cast<size_t>(0) : (std::min)(n - 1, (n * 95) / 100); };
        auto phase50 = [this](const std::vector<uint32_t>& v) {
            return v.empty() ? std::string("-") : std::to_string(xrphase::phaseMedianUs(rank, v, phasePeriodUs));
        };
        auto share = [&](uint32_t n) {
            if (handoffN == 0) {
                return std::string("-");
            }
            char pct[16];
            std::snprintf(pct, sizeof(pct), "%.1f", 100.0 * static_cast<double>(n) / static_cast<double>(handoffN));
            return std::string(pct);
        };
        rank.scan(mboxUs);
        const double mbox50 = static_cast<double>(rank.nth(mboxUs.size() / 2)) / 1000.0;
        const double mbox95 = static_cast<double>(rank.nth(p95index(mboxUs.size()))) / 1000.0;
        rank.scan(swWaitUs);
        const double swWait95 = static_cast<double>(rank.nth(p95index(swWaitUs.size()))) / 1000.0;
        const double swWaitMax = static_cast<double>(rank.maxValue()) / 1000.0;
        char head[160];
        std::snprintf(head, sizeof(head), " | mbox p50/p95=%.2f/%.2f ms swWait p95/max=%.2f/%.2f ms swNotOk=%u rel max=%u us", mbox50,
                      mbox95, swWait95, swWaitMax, swNotOk, relMaxUs);
        std::string s = head;
        s += " | sub/done p50=" + phase50(subPhaseUs) + "/" + phase50(donePhaseUs) + " us coll=" + share(handoffPreempt) +
             "% queued=" + share(handoffQueued) + "%";
        if (synth) {
            // §VR-SYNTH：補幀那一批送出與做完的相位
            s += " synthSub/synthDone p50=" + phase50(synthSubPhaseUs) + "/" + phase50(synthDonePhaseUs) + " us";
        }
        return s;
    }
};

// 每個 pass 的 GPU 時間（[VIPLE-VR-PASS]）。libplacebo 每個 pass 本來就帶 GPU 計時器、每次執行都查結果，XrVideo 只是掛
// callback 把數字接出來，不會多任何 GPU 指令
struct PassStats {
    // convert＝pl_render_image 的 pass（解碼出來的影像轉成 RGB；依它在那一次算繪裡的序號，3 以上併在 Convert3）；
    // 其餘是我們自己的 pass。Flow 的粗、細兩層是同一個 shader，記在一起
    enum Slot { Convert0, Convert1, Convert2, Convert3, Unwarp, Down8, Down16, Flow, Combine, Other, SlotCount };
    struct Stat {
        uint32_t n = 0;
        uint64_t sumNs = 0, minNs = 0, maxNs = 0;
    };
    Stat stat[SlotCount];
    uint64_t windowStartUs = 0;      // 這個 60 秒視窗的起點（0＝還沒開始）
    bool everSampled = false;        // 印過有數字的一行了
    bool unavailableLogged = false;  // 印過「這顆驅動沒有計時」了

    static int slotForConvert(int index) { return index <= 0 ? Convert0 : (index >= 3 ? Convert3 : Convert0 + index); }
    // 我們自己的 pass 用 pl_custom_shader 的 description 認（子字串）；"downscale 16" 要排在 "downscale" 前面。
    // 每個 pass 畫完都會進來一次，而且排在這張影像後面幾個 pass 與送出之前，所以直接在 C 字串上找、不配置記憶體
    static int slotForDescription(const char* desc)
    {
        if (desc == nullptr) {
            return Other;
        }
        auto has = [desc](const char* what) { return std::strstr(desc, what) != nullptr; };
        return has("fovea unwarp") ? Unwarp
               : has("synth downscale 16") ? Down16
               : has("synth downscale") ? Down8
               : has("synth flow") ? Flow
               : has("synth combine") ? Combine
               : Other;
    }
    void note(int slot, uint64_t ns)
    {
        if (slot < 0 || slot >= SlotCount) {
            return;
        }
        Stat& s = stat[slot];
        s.minNs = s.n == 0 ? ns : (std::min)(s.minNs, ns);
        s.maxNs = (std::max)(s.maxNs, ns);
        s.sumNs += ns;
        s.n++;
    }
    // 每張影像畫完呼叫一次。滿 60 秒就回傳要印的那一行並清掉視窗；還沒到、或沒有東西要印回空字串。
    // 只列這 60 秒有樣本的名稱（次數／平均／最小／最大，µs）。開場的 60 秒一筆樣本都沒有＝這顆驅動的佇列
    // 不支援時間戳查詢（libplacebo 靜默略過）：印一次說明，之後不再印
    std::string takeLine(uint64_t nowUs)
    {
        if (windowStartUs == 0) {
            windowStartUs = nowUs;
            return std::string();
        }
        if (nowUs - windowStartUs < 60000000ull) {
            return std::string();
        }
        windowStartUs = nowUs;
        static const char* const kName[SlotCount] = {"convert0", "convert1", "convert2", "convert3", "unwarp",
                                                     "down8",    "down16",   "flow",     "combine",  "other"};
        std::string fields;
        for (int i = 0; i < SlotCount; i++) {
            const Stat& s = stat[i];
            if (s.n == 0) {
                continue;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), " %s:%u/%llu/%llu/%llu", kName[i], s.n,
                          static_cast<unsigned long long>(s.sumNs / s.n / 1000u), static_cast<unsigned long long>(s.minNs / 1000u),
                          static_cast<unsigned long long>(s.maxNs / 1000u));
            fields += buf;
            stat[i] = Stat();
        }
        if (!fields.empty()) {
            everSampled = true;
            return "[VIPLE-VR-PASS] 60s" + fields;
        }
        if (!everSampled && !unavailableLogged) {
            unavailableLogged = true;
            return std::string("[VIPLE-VR-PASS] pass timers unavailable on this driver");
        }
        return std::string();
    }
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
    const uint32_t T = 8333;  // 120 Hz
    static const char* const kEmptyTail =
        " | mbox p50/p95=0.00/0.00 ms swWait p95/max=0.00/0.00 ms swNotOk=0 rel max=0 us | sub/done p50=-/- us coll=-% queued=-%";
    // 預期的字串和實際的不一樣時，細節裡兩個都印出來
    auto same = [](const std::string& got, const std::string& want) { return got == want ? got : "got [" + got + "] want [" + want + "]"; };
    // 細節裡不帶 log 的 tag（自測的輸出不可以被當成真的 [VIPLE-VR-PASS] 行抓走）
    auto untag = [](const std::string& line) {
        const std::string tag = "[VIPLE-VR-PASS] ";
        return line.compare(0, tag.size(), tag) == 0 ? line.substr(tag.size()) : line;
    };

    // ── RenderDiag::statsTail（[VIPLE-XR] 10s video 行尾）──
    {
        // 空的視窗：數字印 0、相位與比例印 -；補幀開著多一段，也是 -
        RenderDiag d;
        const std::string plain = d.statsTail(false), synth = d.statsTail(true);
        const bool ok = plain == kEmptyTail && synth == std::string(kEmptyTail) + " synthSub/synthDone p50=-/- us";
        caseDone("video-tail-empty", ok, ok ? plain : same(plain, kEmptyTail));
    }
    {
        // 全速 120 Hz 的 100 張。mbox：95 張等 20 µs、5 張等 3 ms → p50 0.02、p95（第 95 個）3.00；swWait：一張 250 µs；
        // 送出的相位 80 張在 5380（距 vblank 0.763 ms，不會撞上合成器）、12 張在 7117（距 vblank 2.500，做到一半被插隊）、
        // 8 張在 284（距 vblank 4.000，排在合成器後面），各自 1500 µs 後做完（7117 的那 12 張繞過格點，做完在 284）
        RenderDiag d;
        d.reserveWindow();
        for (uint32_t k = 0; k < 100; k++) {
            const uint32_t sub = k < 80 ? 5380u : (k < 92 ? 7117u : 284u);
            d.mboxUs.push_back(k % 20 == 0 ? 3000u : 20u);
            d.swWaitUs.push_back(k == 33 ? 250u : 30u);
            d.subPhaseUs.push_back(sub);
            d.donePhaseUs.push_back((sub + 1500) % T);
            const xrphase::Handoff h = xrphase::handoffClass(xrphase::ldiagToVblankUs(sub, T));
            d.handoffN++;
            d.handoffPreempt += h == xrphase::Handoff::Preempt ? 1u : 0u;
            d.handoffQueued += h == xrphase::Handoff::Queued ? 1u : 0u;
        }
        d.phasePeriodUs = T;
        d.relMaxUs = 40;
        const std::string got = d.statsTail(false);
        const char* const want = " | mbox p50/p95=0.02/3.00 ms swWait p95/max=0.03/0.25 ms swNotOk=0 rel max=40 us"
                                 " | sub/done p50=5380/6880 us coll=12.0% queued=8.0%";
        caseDone("video-tail-full-rate", got == want, same(got, want));
    }
    {
        // 半速＋補幀：補幀那一批做完的相位橫跨 0／週期的接縫（8200、8300、10、100、200 各 100 張）。照數值的大小取
        // 第 250 個會是 200；環狀的中位數是 10。一張影像 acquire 兩次，swWait 有 1000 筆
        RenderDiag d;
        const uint32_t kDone[5] = {8200, 8300, 10, 100, 200};
        for (uint32_t k = 0; k < 500; k++) {
            d.mboxUs.push_back(26);
            d.swWaitUs.push_back(30);
            d.swWaitUs.push_back(30);
            d.subPhaseUs.push_back(5380);
            d.donePhaseUs.push_back(6880);
            d.synthSubPhaseUs.push_back(6000);
            d.synthDonePhaseUs.push_back(kDone[k % 5]);
        }
        d.phasePeriodUs = T;
        d.handoffN = 500;
        d.swNotOk = 1;
        d.relMaxUs = 90;
        std::vector<uint32_t> sorted = d.synthDonePhaseUs;
        std::sort(sorted.begin(), sorted.end());
        const std::string got = d.statsTail(true);
        const char* const want = " | mbox p50/p95=0.03/0.03 ms swWait p95/max=0.03/0.03 ms swNotOk=1 rel max=90 us"
                                 " | sub/done p50=5380/6880 us coll=0.0% queued=0.0% synthSub/synthDone p50=6000/10 us";
        caseDone("video-tail-synth-across-seam", got == want && sorted[sorted.size() / 2] == 200, same(got, want));
    }
    {
        // 沒有顯示格點（XR 桌面，或那幾格的預測顯示時刻換算不出來）：mbox、swWait 照印，相位與比例印 -
        RenderDiag d;
        for (int k = 0; k < 10; k++) {
            d.mboxUs.push_back(10);
            d.swWaitUs.push_back(20);
        }
        d.relMaxUs = 9;
        const std::string got = d.statsTail(true);
        const char* const want = " | mbox p50/p95=0.01/0.01 ms swWait p95/max=0.02/0.02 ms swNotOk=0 rel max=9 us"
                                 " | sub/done p50=-/- us coll=-% queued=-% synthSub/synthDone p50=-/- us";
        caseDone("video-tail-no-grid", got == want, same(got, want));
    }
    {
        // 90 Hz（週期 11111 µs）：相位照印（對 11111 取的），但「會不會撞上合成器」的相位窗只量過 120 Hz，沒有分類 → -
        RenderDiag d;
        for (int k = 0; k < 10; k++) {
            d.subPhaseUs.push_back(9000);
            d.donePhaseUs.push_back(10500);
        }
        d.phasePeriodUs = 11111;
        const std::string got = d.statsTail(false);
        const char* const want = " | mbox p50/p95=0.00/0.00 ms swWait p95/max=0.00/0.00 ms swNotOk=0 rel max=0 us"
                                 " | sub/done p50=9000/10500 us coll=-% queued=-%";
        caseDone("video-tail-other-rate", got == want, same(got, want));
    }
    {
        // 分位數的位置（也驗視窗比預留的 2048 筆大的時候）：5000 筆 0…4999 → p50 是第 2500 個、p95 是第 4750 個、
        // max 是 4999（印成 5.00 ms）
        RenderDiag d;
        d.reserveWindow();
        for (uint32_t i = 0; i < 5000; i++) {
            d.mboxUs.push_back(i);
            d.swWaitUs.push_back(4999 - i);
        }
        const std::string got = d.statsTail(false);
        const char* const want = " | mbox p50/p95=2.50/4.75 ms swWait p95/max=4.75/5.00 ms swNotOk=0 rel max=0 us"
                                 " | sub/done p50=-/- us coll=-% queued=-%";
        caseDone("video-tail-positions", got == want, same(got, want));
    }
    {
        // 視窗的輪替（XrVideo::takeStatsLine 的做法）：整包換出去 → 換出去的那一包有資料、留下來的是空的；
        // clear() 之後每一項都歸零，印出來和空的視窗相同
        RenderDiag live, taken;
        live.reserveWindow();
        taken.reserveWindow();
        for (uint32_t k = 0; k < 5; k++) {
            live.mboxUs.push_back(20);
            live.swWaitUs.push_back(30);
            live.subPhaseUs.push_back(5380);
            live.donePhaseUs.push_back(6880);
            live.synthSubPhaseUs.push_back(1000);
            live.synthDonePhaseUs.push_back(2000);
        }
        live.phasePeriodUs = T;
        live.handoffN = 5;
        live.handoffPreempt = 1;
        live.handoffQueued = 2;
        live.swNotOk = 3;
        live.relMaxUs = 44;
        std::swap(live, taken);
        bool ok = taken.subPhaseUs.size() == 5 && taken.handoffN == 5 && taken.phasePeriodUs == T && live.mboxUs.empty() &&
                  live.subPhaseUs.empty() && live.handoffN == 0 && live.phasePeriodUs == 0 && live.statsTail(false) == kEmptyTail;
        const std::string got = taken.statsTail(true);
        const char* const want = " | mbox p50/p95=0.02/0.02 ms swWait p95/max=0.03/0.03 ms swNotOk=3 rel max=44 us"
                                 " | sub/done p50=5380/6880 us coll=20.0% queued=40.0% synthSub/synthDone p50=1000/2000 us";
        ok = ok && got == want;
        taken.clear();
        ok = ok && taken.mboxUs.empty() && taken.swWaitUs.empty() && taken.subPhaseUs.empty() && taken.donePhaseUs.empty() &&
             taken.synthSubPhaseUs.empty() && taken.synthDonePhaseUs.empty() && taken.phasePeriodUs == 0 && taken.swNotOk == 0 &&
             taken.relMaxUs == 0 && taken.handoffN == 0 && taken.handoffPreempt == 0 && taken.handoffQueued == 0 &&
             taken.statsTail(false) == kEmptyTail;
        caseDone("video-window-reuse", ok, same(got, want));
    }

    // ── PassStats（[VIPLE-VR-PASS]）──
    {
        // pass 的名稱怎麼認。xrvideo.cpp 裡五個 pl_custom_shader 的 description 都要認得出來（"synth downscale 16" 要先於
        // "synth downscale"）；libplacebo 給的字串前後可能多東西；認不得的、沒有名稱的歸 other。pl_render_image 的 pass
        // 照序號：0、1、2 各一格，3 以上併在一起，負的當 0
        const bool names = PassStats::slotForDescription("viple vr fovea unwarp") == PassStats::Unwarp &&
                           PassStats::slotForDescription("viple vr synth downscale") == PassStats::Down8 &&
                           PassStats::slotForDescription("viple vr synth downscale 16") == PassStats::Down16 &&
                           PassStats::slotForDescription("viple vr synth flow") == PassStats::Flow &&
                           PassStats::slotForDescription("viple vr synth combine") == PassStats::Combine &&
                           PassStats::slotForDescription("x, viple vr synth flow x2, y") == PassStats::Flow &&
                           PassStats::slotForDescription("color conversion") == PassStats::Other &&
                           PassStats::slotForDescription("") == PassStats::Other &&
                           PassStats::slotForDescription(nullptr) == PassStats::Other;
        const bool convert = PassStats::slotForConvert(0) == PassStats::Convert0 && PassStats::slotForConvert(1) == PassStats::Convert1 &&
                             PassStats::slotForConvert(2) == PassStats::Convert2 && PassStats::slotForConvert(3) == PassStats::Convert3 &&
                             PassStats::slotForConvert(9) == PassStats::Convert3 && PassStats::slotForConvert(-1) == PassStats::Convert0;
        caseDone("pass-names", names && convert, names && convert ? "5 custom passes, unknown -> other, convert 0-3+" : "wrong slot");
    }
    {
        // 60 秒、半速＋補幀：每張影像 convert0 兩次（第一批與補幀那一批）、unwarp、down8、down16 各一次、flow 兩層、
        // combine 一次。3600 張（每張 16667 µs）剛好滿 60 秒，在最後一張印。
        //   convert0：7200 次，平均 320、最小 319、最大 321 µs；unwarp：有一張被合成器擋住（2650 µs）→ 平均還是 793；
        //   flow：兩層 150 與 450 記在一起；combine：有一筆 999 ns → 最小 0 µs
        PassStats p;
        uint64_t now = 9000000;
        std::string line = p.takeLine(now);  // 第一次呼叫只定視窗的起點
        bool ok = line.empty() && p.windowStartUs == now;
        int printed = 0, printedAt = -1;
        for (int i = 0; i < 3600 && printed == 0; i++) {
            now += 16667;
            p.note(PassStats::Convert0, 319000);
            p.note(PassStats::Unwarp, i == 100 ? 2650000u : 793000u);
            p.note(PassStats::Convert0, 321000);
            p.note(PassStats::slotForDescription("viple vr synth downscale"), 60000);
            p.note(PassStats::slotForDescription("viple vr synth downscale 16"), 20000);
            p.note(PassStats::Flow, 150000);
            p.note(PassStats::Flow, 450000);
            p.note(PassStats::Combine, i == 7 ? 999u : 900000u);
            line = p.takeLine(now);
            if (!line.empty()) {
                printed++;
                printedAt = i;
            }
        }
        const char* const want = "[VIPLE-VR-PASS] 60s convert0:7200/320/319/321 unwarp:3600/793/793/2650 down8:3600/60/60/60 "
                                 "down16:3600/20/20/20 flow:7200/300/150/450 combine:3600/899/0/900";
        ok = ok && printed == 1 && printedAt == 3599 && line == want && p.everSampled && p.stat[PassStats::Convert0].n == 0 &&
             p.stat[PassStats::Flow].n == 0;
        // 下一個視窗只有 convert0 和一個認不得的 pass：只列有樣本的
        std::string next;
        for (int i = 0; i < 3601 && next.empty(); i++) {
            now += 16667;
            p.note(PassStats::Convert0, 400000);
            if (i == 0) {
                p.note(PassStats::slotForDescription("something new"), 5000);
            }
            next = p.takeLine(now);
        }
        ok = ok && next == "[VIPLE-VR-PASS] 60s convert0:3600/400/400/400 other:1/5/5/5";
        caseDone("pass-60s-line", ok, same(untag(line), untag(want)));
    }
    {
        // 這顆驅動沒有計時：開場 60 秒一筆樣本都沒有 → 印一次說明，之後不再印；後來有樣本了照常印。
        // 開場有樣本、之後某個視窗沒有：那個視窗什麼都不印（不會誤報沒有計時）。超出範圍的格不記
        static const char* const kUnavailable = "[VIPLE-VR-PASS] pass timers unavailable on this driver";
        PassStats p;
        uint64_t now = 1000;
        std::string first, later;
        int lines = 0;
        for (int i = 0; i < 12000; i++) {
            now += 16667;
            const std::string line = p.takeLine(now);
            if (!line.empty()) {
                (lines == 0 ? first : later) = line;
                lines++;
            }
        }
        bool ok = first == kUnavailable && lines == 1 && later.empty();
        p.note(PassStats::Convert0, 100000);
        for (int i = 0; i < 3601 && later.empty(); i++) {
            now += 16667;
            later = p.takeLine(now);
        }
        ok = ok && later == "[VIPLE-VR-PASS] 60s convert0:1/100/100/100";
        PassStats q;
        now = 5;
        q.takeLine(now);
        q.note(PassStats::Convert0, 1000);
        now += 60000000;
        const std::string withSamples = q.takeLine(now);
        now += 60000000;
        const std::string quiet = q.takeLine(now);
        q.note(-1, 5);
        q.note(PassStats::SlotCount, 5);
        now += 60000000;
        ok = ok && withSamples == "[VIPLE-VR-PASS] 60s convert0:1/1/1/1" && quiet.empty() && q.takeLine(now).empty();
        caseDone("pass-unavailable-once", ok, same(untag(first), untag(kUnavailable)));
    }
    return failures;
}

}  // namespace xrvideostats
