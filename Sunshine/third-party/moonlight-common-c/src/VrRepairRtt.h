// VipleStream 2.0 §VR-REPAIR-RTT：補包（§VR-LINK-REPAIR）的門檻、等待上限、重報時機跟著量到的往返時間走。
//
// 原本這三道上限都用幀週期訂（VideoStream.c 的門檻「往返＋2 ms ≤ 一個半幀週期」、RtpVideoQueue.c 的等待上限
// 「下一幀到了之後一個幀週期」、server 的年齡上限「三個幀週期」）。往返 10 ms 上下、120 Hz 時三道都剛好卡在邊上：
// 補包多半在放棄之後才到。這裡把「等多久、什麼時候報、值不值得報」改成由往返時間決定，只在一條連線送影像時適用；
// 兩條以上在送時整套現行規則不動。說明與常數的依據見 docs/vr_protocol.md §4.10 的「§VR-REPAIR-RTT」。
//
// 這個檔只有純邏輯：沒有 I/O、不讀時鐘、不配置記憶體，C 與 C++ 都能 include。接收迴圈（VideoStream.c）、佇列
// （RtpVideoQueue.c）、模擬器（moonlight-common-c/tests/vr_repair_sim）與單元測試呼叫的是同一份。
// Q／A／S3 三份必須 byte-identical（check_commonc_sync.ps1 檢查）。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "VipleVr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VRRR_RTT_WIN              8        // 最近幾筆往返時間樣本（PING 每 250 ms 一筆 → 約 2 s）
#define VRRR_ETA_MARGIN_US        2000u    // server 收到回報到補包送完（實測 ≤ 1 ms）＋接收逾時進位到整數毫秒
#define VRRR_GATE_HYST_US         2000u    // 門檻關掉之後，要低這麼多才再打開
#define VRRR_CAP_SLACK_US         2000u    // 等待上限＝一輪（eta）＋再一輪（最短往返＋餘裕）＋這麼多……
#define VRRR_CAP_MAX_US           33000u   // ……但不超過這麼久（120 Hz 的 4 個顯示格、60 Hz 的 2 個幀週期）
#define VRRR_GATE_US              25000u   // 門檻：中位數的往返＋餘裕超過這麼久（120 Hz 的 3 個顯示格）就整個不回報、不等
#define VRRR_CAP_OVERRIDE_MIN_MS  8u
#define VRRR_CAP_OVERRIDE_MAX_MS  100u
#define VRRR_IDLE_MIN_US          3000u    // 「這個 block 這麼久沒有新封包就回報」的下限（＝原本固定的 3 ms）
#define VRRR_STALL_WIN            16       // 最近幾筆「block 內的停頓」
#define VRRR_STALL_MIN_US         3000u    // 短於這個的間隔不記
#define VRRR_STALL_MAX_AGE_US     3000000u // 超過這麼久的樣本不採用
#define VRRR_AHEAD_MAX            3u       // 一次最多替後面幾個「整幀沒到」的幀預先回報
#define VRRR_AHEAD_SPAN_US        34000u   // 後面整幀沒到的幀合起來超過這麼久（120 Hz 4 幀、60 Hz 2 幀）就不補
#define VRRR_IDLE_SMALL_SHARDS    32u      // VRRR_IDLE_SIZE：shard 總數不超過這個數的 block（一兩毫秒內到齊）才用 3 ms

// 「多久沒有新封包就回報」的做法（STREAM_CONFIGURATION.vrRepairIdle 用同一組值）
#define VRRR_IDLE_AUTO            0        // 幀週期 ≤ 10 ms（100 Hz 以上）用 SIZE，其餘用 FIXED3
#define VRRR_IDLE_FIXED3          1        // 固定 3 ms（＝現行）
#define VRRR_IDLE_FRAME           2        // 一個幀週期（等於交給「下一幀到了」那一條；不會對晚到的封包誤報）
#define VRRR_IDLE_LEARNED         3        // 依量到的 block 內停頓調整（3 ms～一個幀週期）
#define VRRR_IDLE_SIZE            4        // 小的 block（≤ VRRR_IDLE_SMALL_SHARDS 個 shard）3 ms，其餘一個幀週期

// ── 往返時間：最近 VRRR_RTT_WIN 筆的最小／中位數／第二大 ──────────────────────────────

typedef struct _VRRR_RTT_WINDOW {
    uint16_t ms10[VRRR_RTT_WIN];  // 0.1 ms
    uint8_t count;
    uint8_t pos;
} VRRR_RTT_WINDOW;

typedef struct _VRRR_RTT_STATS {
    uint32_t loUs;    // 最小值：路徑本身的往返時間（補包不可能比它更早到）
    uint32_t midUs;   // 中位數（偶數筆取較大的那一個）：值不值得試
    uint32_t hiUs;    // 第二大（單一離群值不採用；不到 3 筆時取最大）：等多久、多久重報
    uint8_t samples;
} VRRR_RTT_STATS;

VIPLE_VR_INLINE void vrrrRttReset(VRRR_RTT_WINDOW* w) {
    w->count = 0;
    w->pos = 0;
}

VIPLE_VR_INLINE void vrrrRttAdd(VRRR_RTT_WINDOW* w, uint16_t ms10) {
    w->ms10[w->pos] = ms10;
    w->pos = (uint8_t)((w->pos + 1) % VRRR_RTT_WIN);
    if (w->count < VRRR_RTT_WIN) {
        w->count++;
    }
}

// 沒有樣本回 false
VIPLE_VR_INLINE bool vrrrRttStats(const VRRR_RTT_WINDOW* w, VRRR_RTT_STATS* out) {
    uint16_t s[VRRR_RTT_WIN];
    int n = w->count;
    int i, k;

    if (n <= 0) {
        return false;
    }
    if (n > VRRR_RTT_WIN) {
        n = VRRR_RTT_WIN;
    }
    for (i = 0; i < n; i++) {
        uint16_t v = w->ms10[i];
        for (k = i; k > 0 && s[k - 1] > v; k--) {
            s[k] = s[k - 1];
        }
        s[k] = v;
    }
    out->loUs = (uint32_t)s[0] * 100u;
    out->midUs = (uint32_t)s[n / 2] * 100u;
    out->hiUs = (uint32_t)s[n >= 3 ? n - 2 : n - 1] * 100u;
    out->samples = (uint8_t)n;
    return true;
}

// 補包只從「正在送影像的連線」回來：只看那幾條（carryMask 的 bit i＝第 i 條；haveMask＝有樣本而且已確認的）。
// 兩條以上時 hi、mid 取最大（等最慢的那條）、lo 取最小。carryMask＝0（還不知道哪條在送）：看全部有樣本的。
// 沒有任何一條可用回 false。
VIPLE_VR_INLINE bool vrrrRttCombine(const VRRR_RTT_STATS* perLink, int linkCount, unsigned carryMask, unsigned haveMask, VRRR_RTT_STATS* out) {
    unsigned use = haveMask & (carryMask != 0 ? carryMask : ~0u);
    bool any = false;
    int i;

    for (i = 0; i < linkCount && i < 32; i++) {
        if (!(use & (1u << i))) {
            continue;
        }
        if (!any) {
            *out = perLink[i];
            any = true;
        }
        else {
            if (perLink[i].loUs < out->loUs) {
                out->loUs = perLink[i].loUs;
            }
            if (perLink[i].midUs > out->midUs) {
                out->midUs = perLink[i].midUs;
            }
            if (perLink[i].hiUs > out->hiUs) {
                out->hiUs = perLink[i].hiUs;
            }
            if (perLink[i].samples < out->samples) {
                out->samples = perLink[i].samples;
            }
        }
    }
    return any;
}

// ── 三個「回報→補包到齊」的估計 ───────────────────────────────────────────────

// 等多久、多久重報：往返時間的高估值再多四分之一＋固定餘裕（補包「該到了」的時刻）
VIPLE_VR_INLINE uint32_t vrrrEtaUs(uint32_t rttHiUs) {
    return rttHiUs + rttHiUs / 4u + VRRR_ETA_MARGIN_US;
}

// 第一份回報值不值得送：用中位數（一半以上的機會趕得上才試）
VIPLE_VR_INLINE uint32_t vrrrEtaTryUs(uint32_t rttMidUs) {
    return rttMidUs + VRRR_ETA_MARGIN_US;
}

// 再報一輪值不值得：用最短的往返（已經在等了，多報一份的代價很小）
VIPLE_VR_INLINE uint32_t vrrrEtaFastUs(uint32_t rttLoUs) {
    return rttLoUs + VRRR_ETA_MARGIN_US;
}

// 後面的封包（下一幀）最多被壓住多久。overrideMs＝0：跟著往返時間走——夠補一輪（etaUs）再重報一輪（etaFastUs），
// 不少於現行規則在等待中有進展時的上限（2×holdUs；holdUs＝clamp(幀週期, 4, 14 ms)），不超過 VRRR_CAP_MAX_US；
// 否則用指定值（夾在 8～100 ms）
VIPLE_VR_INLINE uint32_t vrrrCapUs(uint32_t holdUs, uint32_t etaUs, uint32_t etaFastUs, uint32_t overrideMs) {
    uint64_t cap;

    if (overrideMs != 0) {
        if (overrideMs < VRRR_CAP_OVERRIDE_MIN_MS) {
            overrideMs = VRRR_CAP_OVERRIDE_MIN_MS;
        }
        if (overrideMs > VRRR_CAP_OVERRIDE_MAX_MS) {
            overrideMs = VRRR_CAP_OVERRIDE_MAX_MS;
        }
        return overrideMs * 1000u;
    }
    cap = (uint64_t)etaUs + etaFastUs + VRRR_CAP_SLACK_US;
    if (cap < 2ull * holdUs) {
        cap = 2ull * holdUs;
    }
    if (cap > VRRR_CAP_MAX_US) {
        cap = VRRR_CAP_MAX_US;
    }
    return (uint32_t)cap;
}

// 門檻（依往返時間退場）：照中位數的往返，連「下一幀到了才發現缺」的事件都要超過 VRRR_GATE_US（或等待上限）才補得回來，
// 就整個不回報、不等。有遲滯：關掉之後要低 VRRR_GATE_HYST_US 才再打開
VIPLE_VR_INLINE bool vrrrGateOpen(uint32_t etaTryUs, uint32_t capUs, bool wasOpen) {
    const uint32_t limit = capUs < VRRR_GATE_US ? capUs : VRRR_GATE_US;
    return wasOpen ? etaTryUs <= limit : (uint64_t)etaTryUs + VRRR_GATE_HYST_US <= limit;
}

// 等待的最後期限算到哪裡：有後面的封包在排隊＝它到達之後 capUs；還沒有＝估下一幀最快在「這個 block 第一個封包
// 到達＋一個幀週期」到（已經過了就當成現在），再加 capUs。firstRxUs＝0：這個 block 還沒收到任何封包
VIPLE_VR_INLINE uint64_t vrrrLimitUs(uint64_t nowUs, uint64_t headArrivalUs, uint64_t firstRxUs, uint32_t framePeriodUs, uint32_t capUs) {
    uint64_t base;

    if (headArrivalUs != 0) {
        base = headArrivalUs;
    }
    else {
        base = firstRxUs != 0 ? firstRxUs + framePeriodUs : nowUs;
        if (base < nowUs) {
            base = nowUs;
        }
    }
    return base + capUs;
}

// 在 atUs 回報的話，補包趕得上期限嗎（etaUs 用哪一個估計由呼叫端決定）
VIPLE_VR_INLINE bool vrrrFeasible(uint64_t atUs, uint32_t etaUs, uint64_t limitUs) {
    return atUs + etaUs <= limitUs;
}

// 下一次重報的時刻；0＝不會再報（次數用完、或再報也趕不上）。lastNackUs＝0（還沒有上一份的時刻）：現在
VIPLE_VR_INLINE uint64_t vrrrRetryAtUs(uint64_t nowUs, uint64_t lastNackUs, uint32_t etaUs, uint32_t etaFastUs, uint64_t limitUs,
                                       unsigned nackCount, unsigned nackMax) {
    uint64_t at;

    if (nackCount >= nackMax) {
        return 0;
    }
    at = lastNackUs != 0 ? lastNackUs + etaUs : nowUs;
    if (at < nowUs) {
        at = nowUs;
    }
    return vrrrFeasible(at, etaFastUs, limitUs) ? at : 0;
}

// 排在最前面那個後面的封包最晚等到什麼時候。
//   gone：server 說補不了（或回報送不出去）→ 不等；
//   tooMany：後面整幀沒到的幀多到補不完 → 不等（一定要走 intra refresh，越早越好）；
//   everNacked＝false（這個 block 沒報過）：現在報還值得試就等到上限（馬上會報），不值得就不等；
//   報過了：等到上限（補包晚到也收得下；該不該重報由回報那一側決定）。
// 所以一張完整到達的幀最多被補包機制壓住 capUs。
VIPLE_VR_INLINE uint64_t vrrrHeadDeadlineUs(uint64_t nowUs, uint64_t headArrivalUs, uint32_t capUs, uint32_t etaTryUs, bool gone, bool tooMany,
                                            bool everNacked) {
    const uint64_t cap = headArrivalUs + capUs;

    if (gone || tooMany) {
        return headArrivalUs;
    }
    if (!everNacked) {
        return vrrrFeasible(nowUs, etaTryUs, cap) ? cap : headArrivalUs;
    }
    return cap;
}

// 放棄一個 block 之後多久之內不再替「還沒報過的 block」回報與等待：LOSS 送到 server（半個往返）、它從下一張開始 intra
// refresh（最多一個幀週期）、那一張送到（半個往返），再多 VRRR_ETA_MARGIN_US。
// 這一段比一個幀週期長，所以只能從這一輪第一次放棄起算、中間再放棄的不延長（呼叫端 RtpVideoQueue.c 的 mlDrain）：
// 每放棄一次就重來的話，受損的幀一幀接一幀來時永遠出不了這一段
VIPLE_VR_INLINE uint32_t vrrrLossHoldoffUs(uint32_t rttLoUs, uint32_t framePeriodUs) {
    return rttLoUs + framePeriodUs + VRRR_ETA_MARGIN_US;
}

// 這個封包不可能是補包：回報送出之後還不到一個（最短的）往返時間它就到了——是原本就在路上、只是晚到的。
// 不知道往返時間（rttLoUs＝0）、或往返短到分不出來時回 false（當成可能是補包）
VIPLE_VR_INLINE bool vrrrCannotBeRepair(uint64_t rxUs, uint64_t firstNackUs, uint32_t rttLoUs) {
    uint32_t guard = rttLoUs / 8u;

    if (guard < 1000u) {
        guard = 1000u;
    }
    return firstNackUs != 0 && rttLoUs > guard && rxUs + guard < firstNackUs + rttLoUs;
}

// VIPLE_VR_TLV_NACK.flags：照 §VR-REPAIR-RTT 的規則送出的回報帶 client 的 eta（毫秒、無條件進位、1～127）
VIPLE_VR_INLINE uint8_t vrrrNackFlags(uint32_t etaUs) {
    uint32_t ms = (etaUs + 999u) / 1000u;

    if (ms < 1u) {
        ms = 1u;
    }
    if (ms > VIPLE_VR_NACK_ETA_MASK) {
        ms = VIPLE_VR_NACK_ETA_MASK;
    }
    return (uint8_t)(VIPLE_VR_NACK_F_RTT | ms);
}

// ── 後面整幀沒到的幀：補不補得完 ────────────────────────────────────────────

// 一次補這麼多個封包要多久才到齊（pktGapUs＝平常同一幀相鄰封包的到達間隔，0＝還沒量到）。呼叫端給的封包數是
// 「data＋parity」，server 實際只補 data 再多八分之一，所以已經多估約四分之一
VIPLE_VR_INLINE uint32_t vrrrBurstUs(uint32_t pkts, uint32_t pktGapUs) {
    const uint64_t us = (uint64_t)pkts * pktGapUs;
    return us > 1000000u ? 1000000u : (uint32_t)us;
}

// 最多替後面幾個整幀沒到的幀預先回報：合起來不超過 VRRR_AHEAD_SPAN_US，最多 VRRR_AHEAD_MAX 幀
VIPLE_VR_INLINE unsigned vrrrAheadMaxFrames(uint32_t framePeriodUs) {
    unsigned n;

    if (framePeriodUs == 0) {
        return 0;
    }
    n = VRRR_AHEAD_SPAN_US / framePeriodUs;
    return n > VRRR_AHEAD_MAX ? VRRR_AHEAD_MAX : n;
}

// 目前這一幀後面有 gapFrames 個整幀沒到的幀：true＝補不完（幀數超過上限，或照量到的到達速率，這麼多封包在期限前到不齊）
// → 全部不補、不等。curNeed＝目前這個 block 還差幾個；framePkts＝最近一幀的封包數
VIPLE_VR_INLINE bool vrrrGapTooMany(unsigned gapFrames, uint64_t nowUs, uint32_t etaTryUs, uint64_t limitUs, uint32_t framePeriodUs,
                                    uint32_t curNeed, uint32_t framePkts, uint32_t pktGapUs) {
    if (gapFrames == 0) {
        return false;
    }
    if (gapFrames > vrrrAheadMaxFrames(framePeriodUs)) {
        return true;
    }
    return !vrrrFeasible(nowUs + vrrrBurstUs(curNeed + gapFrames * framePkts, pktGapUs), etaTryUs, limitUs);
}

// ── block 內的停頓：決定「多久沒有新封包才算尾端掉了」（VRRR_IDLE_LEARNED）──────────────────

typedef struct _VRRR_STALLS {
    uint32_t us[VRRR_STALL_WIN];
    uint64_t atUs[VRRR_STALL_WIN];  // 0＝空格
    uint8_t pos;
} VRRR_STALLS;

// 同一個 block 相鄰兩個被採用的封包隔了 stallUs 才到，而且後面那個確定不是補包
VIPLE_VR_INLINE void vrrrStallAdd(VRRR_STALLS* s, uint32_t stallUs, uint64_t nowUs) {
    if (stallUs < VRRR_STALL_MIN_US || nowUs == 0) {
        return;
    }
    s->us[s->pos] = stallUs;
    s->atUs[s->pos] = nowUs;
    s->pos = (uint8_t)((s->pos + 1) % VRRR_STALL_WIN);
}

// 最近 VRRR_STALL_MAX_AGE_US 內的樣本裡第二大的（單一離群值不採用）；不到兩筆回 0
VIPLE_VR_INLINE uint32_t vrrrStallHiUs(const VRRR_STALLS* s, uint64_t nowUs) {
    uint32_t top = 0, second = 0;
    int n = 0;
    int i;

    for (i = 0; i < VRRR_STALL_WIN; i++) {
        if (s->atUs[i] == 0 || nowUs < s->atUs[i] || nowUs - s->atUs[i] > VRRR_STALL_MAX_AGE_US) {
            continue;
        }
        n++;
        if (s->us[i] > top) {
            second = top;
            top = s->us[i];
        }
        else if (s->us[i] > second) {
            second = s->us[i];
        }
    }
    return n >= 2 ? second : 0;
}

// VRRR_IDLE_AUTO（與不認得的值）換成實際的做法
VIPLE_VR_INLINE uint8_t vrrrIdlePolicy(uint8_t policy, uint32_t framePeriodUs) {
    if (policy == VRRR_IDLE_FIXED3 || policy == VRRR_IDLE_FRAME || policy == VRRR_IDLE_LEARNED || policy == VRRR_IDLE_SIZE) {
        return policy;
    }
    return framePeriodUs <= 10000u ? VRRR_IDLE_SIZE : VRRR_IDLE_FIXED3;
}

// 「這個 block 多久沒有新封包才回報」。policy 是 vrrrIdlePolicy() 的結果；blockShards＝這個 block 的 shard 總數
VIPLE_VR_INLINE uint32_t vrrrIdleUs(uint8_t policy, const VRRR_STALLS* s, uint64_t nowUs, uint32_t framePeriodUs, uint32_t blockShards) {
    uint32_t idle = VRRR_IDLE_MIN_US;

    if (policy == VRRR_IDLE_FRAME || (policy == VRRR_IDLE_SIZE && blockShards > VRRR_IDLE_SMALL_SHARDS)) {
        idle = framePeriodUs;
    }
    else if (policy == VRRR_IDLE_LEARNED) {
        const uint32_t hi = vrrrStallHiUs(s, nowUs);
        idle = hi + hi / 4u + 1000u;
        if (framePeriodUs >= VRRR_IDLE_MIN_US && idle > framePeriodUs) {
            idle = framePeriodUs;
        }
    }
    return idle < VRRR_IDLE_MIN_US ? VRRR_IDLE_MIN_US : idle;
}

// ── 保險：等了卻救不回就暫停（回到「不回報、不等」），有退避 ─────────────────────────────
//
// 結果只算兩種：ok＝回報過、補包真的用上、收齊了；bad＝回報過、等到期限還是放棄。（沒報就放棄的、因為後面缺太多幀
// 而放棄的都不算。）
// 連續 VRRR_BRK_STREAK 次 bad，或最近 VRRR_BRK_HIST 個結果裡 bad 達到 VRRR_BRK_HIST_BAD 個 → 暫停。
// 暫停 VRRR_BRK_PAUSE_US × 2^level；暫停結束後第一個結果是 bad 就立刻再暫停、level 加一（最多 VRRR_BRK_LEVEL_MAX），
// 是 ok 就 level 歸零。

#define VRRR_BRK_STREAK      3u
#define VRRR_BRK_HIST        8u
#define VRRR_BRK_HIST_BAD    5u
#define VRRR_BRK_PAUSE_US    2000000u
#define VRRR_BRK_LEVEL_MAX   3u

typedef struct _VRRR_BREAKER {
    uint8_t streak;       // 連續幾次 bad
    uint8_t hist;         // 最近的結果（bit 0＝最新；1＝bad）
    uint8_t histCount;
    uint8_t level;        // 退避的級數
    bool probing;         // 暫停剛結束，下一個結果決定要不要再暫停
    uint64_t pausedUntilUs;
    uint32_t trips;       // 觸發過幾次（統計）
} VRRR_BREAKER;

VIPLE_VR_INLINE bool vrrrBreakerPaused(const VRRR_BREAKER* b, uint64_t nowUs) {
    return nowUs < b->pausedUntilUs;
}

VIPLE_VR_INLINE unsigned vrrrBrkPopcount(uint8_t v) {
    unsigned n = 0;
    while (v) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

VIPLE_VR_INLINE void vrrrBrkTrip(VRRR_BREAKER* b, uint64_t nowUs) {
    b->pausedUntilUs = nowUs + ((uint64_t)VRRR_BRK_PAUSE_US << b->level);
    b->streak = 0;
    b->hist = 0;
    b->histCount = 0;
    b->probing = true;
    b->trips++;
}

// okDelta／badDelta：上次呼叫以來的 ok、bad 個數。回傳 true＝這一次觸發了暫停
VIPLE_VR_INLINE bool vrrrBreakerNote(VRRR_BREAKER* b, uint64_t nowUs, uint32_t okDelta, uint32_t badDelta) {
    bool tripped = false;

    if (vrrrBreakerPaused(b, nowUs)) {
        return false;  // 暫停期間（進入暫停前就在等的 block）的結果不算
    }
    while (badDelta-- > 0 && !tripped) {
        if (b->probing) {
            if (b->level < VRRR_BRK_LEVEL_MAX) {
                b->level++;
            }
            vrrrBrkTrip(b, nowUs);
            tripped = true;
            break;
        }
        b->streak++;
        b->hist = (uint8_t)((b->hist << 1) | 1u);
        if (b->histCount < VRRR_BRK_HIST) {
            b->histCount++;
        }
        if (b->streak >= VRRR_BRK_STREAK || vrrrBrkPopcount(b->hist) >= VRRR_BRK_HIST_BAD) {
            vrrrBrkTrip(b, nowUs);
            tripped = true;
        }
    }
    if (!tripped) {
        while (okDelta-- > 0) {
            b->streak = 0;
            b->hist = (uint8_t)(b->hist << 1);
            if (b->histCount < VRRR_BRK_HIST) {
                b->histCount++;
            }
            if (b->probing) {
                b->probing = false;
                b->level = 0;
            }
        }
    }
    return tripped;
}

// ── 同場 A/B（開發用）：每 abSec 秒在現行規則與 RTT 規則之間切換，從現行規則開始 ─────────────────
// 回傳 true＝這一刻在 RTT 那一邊。abSec＝0：不交替，一直在 RTT 那一邊
VIPLE_VR_INLINE bool vrrrAbArmRtt(uint64_t elapsedUs, uint32_t abSec) {
    if (abSec == 0) {
        return true;
    }
    return ((elapsedUs / 1000000u) / abSec) % 2u == 1u;
}

// ── 每個封包的決定（VideoStream 的接收迴圈、模擬器、單元測試共用同一份）────────────────────

typedef struct _VRRR_IN {
    bool negotiatedRepair;      // 協商了 VIPLE_VR_LINK_F_REPAIR
    bool negotiatedRtt;         // 協商了 VIPLE_VR_LINK_F_REPAIR_RTT
    bool armRtt;                // 這個時刻允許用 RTT 規則（沒有 A/B 時恆為 true；A/B 的現行那一邊是 false）
    int carrying;               // 幾條連線實際在送影像（vrmlVideoCarrying()）
    int legacyRttMs;            // vrmlCtrlRttMs()：−1＝沒有已確認的連線
    bool haveStats;             // st 有效（正在送影像的連線有往返時間樣本）
    VRRR_RTT_STATS st;
    uint32_t holdUs;            // 現行的等待上限 mlHoldUs＝clamp(幀週期, 4, 14 ms)
    uint32_t framePeriodUs;     // 1e6／fps，不夾
    uint32_t capOverrideMs;     // 0＝自動
    uint8_t idlePolicy;         // VRRR_IDLE_*
    bool latched;               // 佇列正在等的 block（或它後面預先報過的幀）是照 RTT 規則回報的
    uint64_t nowUs;
    uint32_t okDelta, badDelta; // 上一個封包以來 RTT 規則下的 ok／bad（見 VRRR_BREAKER）
} VRRR_IN;

typedef struct _VRRR_STATE {
    bool init;
    bool gateOpen;
    bool lastRttMode;
    VRRR_BREAKER brk;
    uint32_t etaUs, etaTryUs, etaFastUs, capUs, loUs;  // 上一次算出來的值（被 latched 沿用規則時、樣本剛好沒有了就用它）
} VRRR_STATE;

typedef struct _VRRR_OUT {
    bool mlRepair;
    bool mlHoldDual;
    bool mlRttMode;
    bool tripped;               // 這個封包觸發了保險的暫停（呼叫端印 log）
    bool paused;
    bool eligible;              // RTT 規則適用（不管門檻與保險）
    uint32_t mlHoldMaxUs;
    uint32_t mlRepairRtoUs;
    uint32_t etaUs, etaTryUs, etaFastUs, capUs, loUs;
    uint8_t idlePolicy;         // vrrrIdlePolicy() 之後
} VRRR_OUT;

VIPLE_VR_INLINE void vrrrDecide(const VRRR_IN* in, VRRR_STATE* s, VRRR_OUT* out) {
    const bool dual = in->carrying >= 2;
    bool eligible;
    bool rtt = false;

    if (!s->init) {
        s->init = true;
        s->gateOpen = true;
    }
    memset(out, 0, sizeof(*out));
    out->idlePolicy = vrrrIdlePolicy(in->idlePolicy, in->framePeriodUs);

    eligible = in->negotiatedRepair && in->negotiatedRtt && in->armRtt && in->legacyRttMs >= 0 && !dual && in->haveStats;
    out->eligible = eligible;
    if (eligible) {
        s->etaUs = vrrrEtaUs(in->st.hiUs);
        s->etaTryUs = vrrrEtaTryUs(in->st.midUs);
        s->etaFastUs = vrrrEtaFastUs(in->st.loUs);
        s->capUs = vrrrCapUs(in->holdUs, s->etaUs, s->etaFastUs, in->capOverrideMs);
        s->loUs = in->st.loUs;
        out->tripped = vrrrBreakerNote(&s->brk, in->nowUs, in->okDelta, in->badDelta);
        s->gateOpen = vrrrGateOpen(s->etaTryUs, s->capUs, s->gateOpen);
        out->paused = vrrrBreakerPaused(&s->brk, in->nowUs);
        rtt = s->gateOpen && !out->paused;
    }
    // 已經照 RTT 規則回報、正在等的 block：沿用到它結束（門檻剛關、保險剛暫停、A/B 換邊、第二條連線開始送都一樣）
    if (!rtt && in->latched && s->lastRttMode && in->negotiatedRepair && in->negotiatedRtt) {
        rtt = true;
    }
    s->lastRttMode = rtt;

    if (rtt) {
        out->mlRttMode = true;
        out->mlRepair = true;
        out->mlHoldDual = false;
        out->mlRepairRtoUs = s->etaUs;
        out->mlHoldMaxUs = in->holdUs;
        out->etaUs = s->etaUs;
        out->etaTryUs = s->etaTryUs;
        out->etaFastUs = s->etaFastUs;
        out->capUs = s->capUs;
        out->loUs = s->loUs;
        return;
    }
    out->mlHoldDual = dual;
    if (eligible) {
        // 門檻關著或保險暫停中：不回報、不等（和現行規則門檻關著時相同）
        out->mlRepair = false;
        out->mlHoldMaxUs = 0;
        return;
    }
    // 現行規則（VideoStream.c 原本那幾行，逐位元相同）
    out->mlRepair = in->negotiatedRepair && in->legacyRttMs >= 0 &&
                    (uint32_t)in->legacyRttMs * 1000u + 2000u <= in->holdUs + in->holdUs / 2u;
    if (out->mlRepair) {
        const uint32_t rtoUs = 2000u * (uint32_t)in->legacyRttMs + 3000u;
        out->mlRepairRtoUs = rtoUs < 4000u ? 4000u : (rtoUs > 10000u ? 10000u : rtoUs);
    }
    out->mlHoldMaxUs = (dual || out->mlRepair) ? in->holdUs : 0;
}

#ifdef __cplusplus
}
#endif
