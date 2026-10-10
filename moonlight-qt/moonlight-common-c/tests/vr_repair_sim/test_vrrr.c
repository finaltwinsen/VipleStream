// VipleStream 2.0 §VR-REPAIR-RTT：VrRepairRtt.h 的單元測試。
//
// 不依賴任何測試框架，C 與 C++ 各編一次（這個標頭兩邊都要能 include）：
//   gcc -std=gnu11 -Wall -Wextra -I../../moonlight-common-c/src -o test_vrrr.exe test_vrrr.c && ./test_vrrr.exe
//   g++ -std=c++20 -x c++ -Wall -Wextra -I../../moonlight-common-c/src -o test_vrrr_cpp.exe test_vrrr.c && ./test_vrrr_cpp.exe
// （run_tests.sh 會做這兩步。）同一批期望值另外以 googletest 的形式放在 Sunshine/tests/unit/test_vr_repair_rtt.cpp，
// 跟著 server 的測試一起在 Linux 的建置上跑。
#include "VrRepairRtt.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define EQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
    if (_a != _b) { printf("FAIL %s:%d: %s = %llu, want %llu\n", __FILE__, __LINE__, #a, _a, _b); g_fail++; } else { g_pass++; } } while (0)
#define YES(a) EQ(!!(a), 1)
#define NO(a) EQ(!!(a), 0)

static VRRR_IN baseIn(void) {
    VRRR_IN in;
    memset(&in, 0, sizeof(in));
    in.negotiatedRepair = true;
    in.negotiatedRtt = true;
    in.armRtt = true;
    in.carrying = 1;
    in.legacyRttMs = 10;
    in.haveStats = true;
    in.st.loUs = 8100; in.st.midUs = 9600; in.st.hiUs = 10900; in.st.samples = 8;
    in.holdUs = 8333;
    in.framePeriodUs = 8333;
    in.nowUs = 1000000;
    return in;
}

int main(void) {
    VRRR_RTT_WINDOW w;
    VRRR_RTT_STATS st, pl[2];
    VRRR_STALLS s;
    VRRR_BREAKER b;
    int i;

    // ── R：往返時間視窗 ──
    vrrrRttReset(&w);
    NO(vrrrRttStats(&w, &st));                                                                                 // R1
    vrrrRttAdd(&w, 96);
    YES(vrrrRttStats(&w, &st)); EQ(st.loUs, 9600); EQ(st.midUs, 9600); EQ(st.hiUs, 9600); EQ(st.samples, 1);   // R2
    vrrrRttReset(&w); vrrrRttAdd(&w, 80); vrrrRttAdd(&w, 120);
    vrrrRttStats(&w, &st); EQ(st.loUs, 8000); EQ(st.midUs, 12000); EQ(st.hiUs, 12000);                         // R3
    vrrrRttReset(&w); vrrrRttAdd(&w, 80); vrrrRttAdd(&w, 96); vrrrRttAdd(&w, 383);
    vrrrRttStats(&w, &st); EQ(st.loUs, 8000); EQ(st.midUs, 9600); EQ(st.hiUs, 9600);                           // R4
    vrrrRttReset(&w);
    { const uint16_t v[8] = {81, 86, 96, 107, 132, 96, 383, 90}; for (i = 0; i < 8; i++) vrrrRttAdd(&w, v[i]); }
    vrrrRttStats(&w, &st); EQ(st.loUs, 8100); EQ(st.midUs, 9600); EQ(st.hiUs, 13200); EQ(st.samples, 8);      // R5
    vrrrRttAdd(&w, 250);
    vrrrRttStats(&w, &st); EQ(st.loUs, 8600); EQ(st.midUs, 10700); EQ(st.hiUs, 25000); EQ(st.samples, 8);     // R6
    for (i = 0; i < 8; i++) vrrrRttAdd(&w, 300);
    vrrrRttStats(&w, &st); EQ(st.loUs, 30000); EQ(st.midUs, 30000); EQ(st.hiUs, 30000);                        // R7
    vrrrRttReset(&w);
    NO(vrrrRttStats(&w, &st));                                                                                 // R8：重設之後又是空的

    // ── M：只看正在送影像的連線 ──
    pl[0].loUs = 8000; pl[0].midUs = 9600; pl[0].hiUs = 10900; pl[0].samples = 8;
    pl[1].loUs = 3000; pl[1].midUs = 3300; pl[1].hiUs = 3500; pl[1].samples = 5;
    YES(vrrrRttCombine(pl, 2, 0x1, 0x3, &st)); EQ(st.loUs, 8000); EQ(st.midUs, 9600); EQ(st.hiUs, 10900); EQ(st.samples, 8);   // M1：只有第 0 條在送
    YES(vrrrRttCombine(pl, 2, 0x2, 0x3, &st)); EQ(st.loUs, 3000); EQ(st.midUs, 3300); EQ(st.hiUs, 3500);                       // M2
    YES(vrrrRttCombine(pl, 2, 0x3, 0x3, &st)); EQ(st.loUs, 3000); EQ(st.midUs, 9600); EQ(st.hiUs, 10900); EQ(st.samples, 5);   // M3：兩條都在送
    YES(vrrrRttCombine(pl, 2, 0x0, 0x3, &st)); EQ(st.loUs, 3000); EQ(st.midUs, 9600); EQ(st.hiUs, 10900);                      // M4：還不知道哪條在送
    NO(vrrrRttCombine(pl, 2, 0x1, 0x2, &st));                                                                                  // M5：在送的那條沒有樣本
    NO(vrrrRttCombine(pl, 2, 0x3, 0x0, &st));                                                                                  // M6

    // ── E：三個估計 ──
    EQ(vrrrEtaUs(0), 2000); EQ(vrrrEtaUs(5000), 8250); EQ(vrrrEtaUs(9600), 14000); EQ(vrrrEtaUs(10000), 14500); EQ(vrrrEtaUs(10900), 15625);
    EQ(vrrrEtaUs(13200), 18500); EQ(vrrrEtaUs(15000), 20750); EQ(vrrrEtaUs(20000), 27000); EQ(vrrrEtaUs(30000), 39500);
    EQ(vrrrEtaTryUs(9600), 11600); EQ(vrrrEtaTryUs(23000), 25000); EQ(vrrrEtaFastUs(8100), 10100); EQ(vrrrEtaFastUs(0), 2000);

    // ── C：等待上限（跟著往返時間走）──
    EQ(vrrrCapUs(8333, 15625, 10100, 0), 27725);    // 這條路徑的典型值（lo 8.1／hi 10.9）
    EQ(vrrrCapUs(8333, 14500, 12000, 0), 28500);    // 往返 10 ms、估計準確
    EQ(vrrrCapUs(8333, 20750, 11000, 0), 33000);    // hi 15：33750 → 夾在上限
    EQ(vrrrCapUs(8333, 5750, 5000, 0), 16666);      // 區網往返 3 ms：不少於現行規則有進展時的上限（兩個幀週期）
    EQ(vrrrCapUs(8333, 3250, 3000, 0), 16666);      // 區網往返 1 ms
    EQ(vrrrCapUs(11111, 5750, 5000, 0), 22222);     // 90 Hz、區網
    EQ(vrrrCapUs(14000, 3250, 3000, 0), 28000);     // 60 Hz、區網：現行的 2×14 ms
    EQ(vrrrCapUs(14000, 14500, 12000, 0), 28500);   // 60 Hz、往返 10 ms
    EQ(vrrrCapUs(14000, 20750, 11000, 0), 33000);   // 60 Hz、hi 15
    EQ(vrrrCapUs(8333, 15625, 10100, 20), 20000); EQ(vrrrCapUs(8333, 15625, 10100, 4), 8000); EQ(vrrrCapUs(8333, 15625, 10100, 500), 100000);
    EQ(vrrrCapUs(8333, 15625, 10100, 8), 8000); EQ(vrrrCapUs(8333, 15625, 10100, 100), 100000);

    // ── G：門檻（依往返時間退場，含遲滯）──
    YES(vrrrGateOpen(25000, 33000, true)); NO(vrrrGateOpen(25001, 33000, true));
    YES(vrrrGateOpen(23000, 33000, false)); NO(vrrrGateOpen(23001, 33000, false));
    YES(vrrrGateOpen(14000, 14000, true)); NO(vrrrGateOpen(14001, 14000, true));     // 等待上限被指定成 14 ms：門檻跟著降
    YES(vrrrGateOpen(12000, 14000, false)); NO(vrrrGateOpen(12001, 14000, false));
    YES(vrrrGateOpen(11600, 27725, false));                                           // 這條路徑的典型值

    // ── L／F：期限算到哪裡、值不值得試 ──
    EQ(vrrrLimitUs(1000, 0, 400, 8333, 27725), 8733 + 27725);
    EQ(vrrrLimitUs(1000, 9000, 400, 8333, 27725), 9000 + 27725);
    EQ(vrrrLimitUs(1000, 0, 0, 8333, 27725), 1000 + 27725);
    EQ(vrrrLimitUs(20000, 0, 400, 8333, 27725), 20000 + 27725);
    YES(vrrrFeasible(1000, 11600, 12600)); NO(vrrrFeasible(1001, 11600, 12600));

    // ── T：重報的時刻 ──
    EQ(vrrrRetryAtUs(20000, 9000, 15625, 10100, 99999, 3, 3), 0);            // T1：次數用完
    EQ(vrrrRetryAtUs(10000, 9000, 15625, 10100, 36725, 1, 3), 24625);        // T2：上一份＋eta，趕得上（24625＋10100 ≤ 36725）
    EQ(vrrrRetryAtUs(10000, 9000, 15625, 10100, 34725, 1, 3), 24625);        // T3：剛好
    EQ(vrrrRetryAtUs(10000, 9000, 15625, 10100, 34724, 1, 3), 0);            // T4：差 1 µs，不報
    EQ(vrrrRetryAtUs(10000, 0, 15625, 10100, 36725, 1, 3), 10000);           // T5：沒有上一份的時刻（lastNack＝0）→ 現在
    EQ(vrrrRetryAtUs(30000, 9000, 15625, 10100, 45000, 1, 3), 30000);        // T6：該報的時刻已經過了 → 現在
    EQ(vrrrRetryAtUs(30000, 9000, 15625, 10100, 40099, 1, 3), 0);            // T7：現在報也趕不上
    EQ(vrrrRetryAtUs(10000, 9000, 15625, 10100, 36725, 2, 3), 24625);        // T8：第三份也照同樣的間隔

    // ── D：環頭的期限 ──
    EQ(vrrrHeadDeadlineUs(10000, 9000, 27725, 11600, true, false, true), 9000);            // D1：server 說補不了
    EQ(vrrrHeadDeadlineUs(10000, 9000, 27725, 11600, false, true, true), 9000);            // D2：後面缺太多幀
    EQ(vrrrHeadDeadlineUs(10000, 9000, 27725, 11600, false, false, false), 36725);         // D3：還沒報、值得試
    EQ(vrrrHeadDeadlineUs(25125, 9000, 27725, 11600, false, false, false), 36725);         // D4：剛好趕得上
    EQ(vrrrHeadDeadlineUs(25126, 9000, 27725, 11600, false, false, false), 9000);          // D5：趕不上 → 不等
    EQ(vrrrHeadDeadlineUs(10000, 9000, 27725, 11600, false, false, true), 36725);          // D6：報過 → 等到上限
    EQ(vrrrHeadDeadlineUs(50000, 9000, 27725, 11600, false, false, true), 36725);          // D7：同上（已經過了上限，呼叫端放行）

    // ── H／N／K ──
    EQ(vrrrLossHoldoffUs(8100, 8333), 18433); EQ(vrrrLossHoldoffUs(10000, 16666), 28666); EQ(vrrrLossHoldoffUs(0, 8333), 10333);
    YES(vrrrCannotBeRepair(5000, 1000, 8000)); NO(vrrrCannotBeRepair(8000, 1000, 8000)); YES(vrrrCannotBeRepair(7999, 1000, 8000));
    NO(vrrrCannotBeRepair(5000, 0, 8000)); NO(vrrrCannotBeRepair(1100, 1000, 800));
    YES(vrrrCannotBeRepair(16000, 1000, 20000)); NO(vrrrCannotBeRepair(18500, 1000, 20000));
    NO(vrrrCannotBeRepair(5000, 1000, 0));                                                   // 不知道往返時間：當成可能是補包
    EQ(vrrrNackFlags(15625), 0x90); EQ(vrrrNackFlags(14500), 0x8F); EQ(vrrrNackFlags(17000), 0x91); EQ(vrrrNackFlags(14001), 0x8F);
    EQ(vrrrNackFlags(0), 0x81); EQ(vrrrNackFlags(200000), 0xFF);
    EQ(vrrrNackFlags(15625) & VIPLE_VR_NACK_F_RTT, VIPLE_VR_NACK_F_RTT); EQ(vrrrNackFlags(15625) & VIPLE_VR_NACK_ETA_MASK, 16);

    // ── A：後面整幀沒到的幀 ──
    EQ(vrrrBurstUs(0, 70), 0); EQ(vrrrBurstUs(210, 70), 14700); EQ(vrrrBurstUs(84, 49), 4116); EQ(vrrrBurstUs(10, 0), 0);
    EQ(vrrrBurstUs(100000, 70), 1000000);
    EQ(vrrrAheadMaxFrames(8333), 3); EQ(vrrrAheadMaxFrames(6944), 3); EQ(vrrrAheadMaxFrames(11111), 3); EQ(vrrrAheadMaxFrames(13888), 2);
    EQ(vrrrAheadMaxFrames(16666), 2); EQ(vrrrAheadMaxFrames(40000), 0); EQ(vrrrAheadMaxFrames(0), 0);
    NO(vrrrGapTooMany(0, 25000, 12000, 52725, 8333, 9, 9, 0));                 // A1：沒有缺幀
    YES(vrrrGapTooMany(4, 25000, 12000, 52725, 8333, 9, 9, 0));                // A2：120 Hz 後面缺 4 幀（＋目前這一幀＝42 ms）
    NO(vrrrGapTooMany(3, 25000, 12000, 52725, 8333, 9, 9, 0));                 // A3：缺 3 幀、小幀：補
    YES(vrrrGapTooMany(3, 25000, 12000, 52725, 16666, 9, 9, 0));               // A4：60 Hz 缺 3 幀：不補
    NO(vrrrGapTooMany(2, 25000, 12000, 52725, 16666, 9, 9, 0));                // A5：60 Hz 缺 2 幀：補
    NO(vrrrGapTooMany(1, 25000, 12000, 52725, 8333, 105, 105, 70));            // A6：每幀 105 個封包、兩整幀（210 個約 14.7 ms 到齊）：來得及
    YES(vrrrGapTooMany(2, 25000, 12000, 52725, 8333, 105, 105, 70));           // A6b：三整幀（315 個要 22 ms）：來不及
    YES(vrrrGapTooMany(1, 25000, 12000, 52725, 8333, 105, 105, 80));           // A6c：到達比較慢（80 µs 一個）：兩整幀也來不及
    NO(vrrrGapTooMany(1, 25000, 12000, 52725, 8333, 30, 105, 49));             // A7：目前這一幀差 30 個＋一整幀：補
    YES(vrrrGapTooMany(1, 41000, 12000, 52725, 8333, 9, 9, 0));                // A8：已經太晚（41000＋12000 > 52725）

    // ── S：block 內的停頓與「多久沒有新封包才回報」──
    memset(&s, 0, sizeof(s));
    EQ(vrrrIdlePolicy(VRRR_IDLE_AUTO, 8333), VRRR_IDLE_SIZE); EQ(vrrrIdlePolicy(VRRR_IDLE_AUTO, 10000), VRRR_IDLE_SIZE);
    EQ(vrrrIdlePolicy(VRRR_IDLE_AUTO, 10001), VRRR_IDLE_FIXED3); EQ(vrrrIdlePolicy(VRRR_IDLE_AUTO, 16666), VRRR_IDLE_FIXED3);
    EQ(vrrrIdlePolicy(VRRR_IDLE_LEARNED, 8333), VRRR_IDLE_LEARNED); EQ(vrrrIdlePolicy(VRRR_IDLE_FRAME, 16666), VRRR_IDLE_FRAME);
    EQ(vrrrIdlePolicy(VRRR_IDLE_FIXED3, 8333), VRRR_IDLE_FIXED3); EQ(vrrrIdlePolicy(VRRR_IDLE_SIZE, 16666), VRRR_IDLE_SIZE);
    EQ(vrrrIdlePolicy(9, 16666), VRRR_IDLE_FIXED3);
    EQ(vrrrIdleUs(VRRR_IDLE_FIXED3, &s, 1000000, 8333, 101), 3000);
    EQ(vrrrIdleUs(VRRR_IDLE_FRAME, &s, 1000000, 8333, 9), 8333); EQ(vrrrIdleUs(VRRR_IDLE_FRAME, &s, 1000000, 2000, 9), 3000);
    EQ(vrrrIdleUs(VRRR_IDLE_SIZE, &s, 1000000, 8333, 9), 3000); EQ(vrrrIdleUs(VRRR_IDLE_SIZE, &s, 1000000, 8333, 32), 3000);
    EQ(vrrrIdleUs(VRRR_IDLE_SIZE, &s, 1000000, 8333, 33), 8333); EQ(vrrrIdleUs(VRRR_IDLE_SIZE, &s, 1000000, 8333, 101), 8333);
    EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1000000, 16666, 80), 3000);                                    // S1：沒有樣本
    vrrrStallAdd(&s, 4000, 1000000);
    EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1000000, 16666, 80), 3000);                                    // S2：只有一筆
    vrrrStallAdd(&s, 5000, 1100000);
    EQ(vrrrStallHiUs(&s, 1100000), 4000); EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1100000, 16666, 80), 6000);   // S3
    vrrrStallAdd(&s, 4500, 1200000); vrrrStallAdd(&s, 4000, 1300000);
    EQ(vrrrStallHiUs(&s, 1300000), 4500); EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1300000, 16666, 80), 6625);   // S4
    EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1300000, 6000, 80), 6000);                                     // S5：夾在幀週期
    vrrrStallAdd(&s, 500, 1400000); vrrrStallAdd(&s, 2999, 1400000);
    EQ(vrrrStallHiUs(&s, 1400000), 4500);                                                               // S6：短於 3 ms 的不記
    EQ(vrrrStallHiUs(&s, 4150000), 4000);                                                               // S7：最舊兩筆過期
    EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 9000000, 16666, 80), 3000);                                    // S8：全部過期
    memset(&s, 0, sizeof(s));
    vrrrStallAdd(&s, 7000, 1000000); vrrrStallAdd(&s, 9000, 1000100); vrrrStallAdd(&s, 8000, 1000200);
    EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1000300, 8333, 80), 8333); EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1000300, 16666, 80), 11000);   // S9、S10
    memset(&s, 0, sizeof(s));
    for (i = 0; i < 30; i++) vrrrStallAdd(&s, 2000 + (uint32_t)i * 100, 1000000 + (uint64_t)i);
    EQ(vrrrStallHiUs(&s, 1000100), 4800);                                                               // S11
    memset(&s, 0, sizeof(s));
    vrrrStallAdd(&s, 2999, 1000000); vrrrStallAdd(&s, 2999, 1000001);
    EQ(vrrrStallHiUs(&s, 1000002), 0);                                                                  // S12：短於 3 ms 的不記（記了這裡會是 2999）
    EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1000002, 16666, 80), 3000);
    vrrrStallAdd(&s, 3000, 1000002); vrrrStallAdd(&s, 3000, 1000003);
    EQ(vrrrStallHiUs(&s, 1000004), 3000);                                                               //      剛好 3 ms 的要記

    // ── B：保險（連續失敗或失敗比例，有退避）──
    memset(&b, 0, sizeof(b));
    NO(vrrrBreakerNote(&b, 1000000, 0, 1)); NO(vrrrBreakerNote(&b, 1100000, 0, 1));
    YES(vrrrBreakerNote(&b, 1200000, 0, 1));                                                 // B1：連續 3 次 → 暫停 2 s
    YES(vrrrBreakerPaused(&b, 1200001)); YES(vrrrBreakerPaused(&b, 3199999)); NO(vrrrBreakerPaused(&b, 3200000));
    NO(vrrrBreakerNote(&b, 2000000, 0, 5));                                                  // B2：暫停期間的結果不算
    YES(vrrrBreakerNote(&b, 3300000, 0, 1));                                                 // B3：恢復後第一個結果就失敗 → 立刻再暫停 4 s
    EQ(b.level, 1); NO(vrrrBreakerPaused(&b, 7300000)); YES(vrrrBreakerPaused(&b, 7299999));
    YES(vrrrBreakerNote(&b, 7400000, 0, 1)); EQ(b.level, 2); YES(vrrrBreakerPaused(&b, 15399999)); NO(vrrrBreakerPaused(&b, 15400000));   // 8 s
    YES(vrrrBreakerNote(&b, 15500000, 0, 1)); EQ(b.level, 3); YES(vrrrBreakerPaused(&b, 31499999));                                       // 16 s
    YES(vrrrBreakerNote(&b, 31600000, 0, 1)); EQ(b.level, 3); YES(vrrrBreakerPaused(&b, 47599999)); NO(vrrrBreakerPaused(&b, 47600000));  // 仍是 16 s
    NO(vrrrBreakerNote(&b, 47700000, 1, 0)); EQ(b.level, 0); NO(b.probing);                  // B4：恢復後第一個結果成功 → 退避歸零
    NO(vrrrBreakerNote(&b, 47800000, 0, 2)); YES(vrrrBreakerNote(&b, 47900000, 0, 1));       //     之後又要連續 3 次才暫停（2 s）
    YES(vrrrBreakerPaused(&b, 49899999)); NO(vrrrBreakerPaused(&b, 49900000)); EQ(b.trips, 6);
    memset(&b, 0, sizeof(b));
    for (i = 0; i < 4; i++) { NO(vrrrBreakerNote(&b, 1000000 + (uint64_t)i * 1000, 0, 1)); NO(vrrrBreakerNote(&b, 1000500 + (uint64_t)i * 1000, 1, 0)); }
    NO(vrrrBreakerNote(&b, 1010000, 0, 1));                                                  // B5：壞好交替 → 最近 8 個裡 4 個失敗，不暫停
    YES(vrrrBreakerNote(&b, 1011000, 0, 1));                                                 //     再一個 → 8 個裡 5 個失敗 → 暫停
    memset(&b, 0, sizeof(b));
    NO(vrrrBreakerNote(&b, 1000000, 1, 2)); EQ(b.streak, 0); EQ(b.histCount, 3);            // B6：同一次呼叫裡先算失敗、再算成功
    NO(vrrrBreakerNote(&b, 1000001, 7, 0)); EQ(b.streak, 0);                                 // B7：只有成功
    memset(&b, 0, sizeof(b));
    NO(vrrrBreakerNote(&b, 1000000, 0, 2)); NO(vrrrBreakerNote(&b, 1000001, 1, 0));          // B8：兩次失敗之後一次成功 → 連續數歸零
    NO(vrrrBreakerNote(&b, 1000002, 0, 2)); YES(vrrrBreakerNote(&b, 1000003, 0, 1));

    // ── X：同場 A/B ──
    YES(vrrrAbArmRtt(0, 0)); YES(vrrrAbArmRtt(123456789, 0));                                // 沒開：一直在 RTT 那一邊
    NO(vrrrAbArmRtt(0, 40)); NO(vrrrAbArmRtt(39999999, 40));                                 // 從現行規則開始
    YES(vrrrAbArmRtt(40000000, 40)); YES(vrrrAbArmRtt(79999999, 40));
    NO(vrrrAbArmRtt(80000000, 40)); YES(vrrrAbArmRtt(120000000, 40));
    NO(vrrrAbArmRtt(19999999, 20)); YES(vrrrAbArmRtt(20000000, 20));

    // ── P：每個封包的決定 ──
    {
        VRRR_IN in = baseIn();
        VRRR_STATE s0;
        VRRR_OUT o;
        // P1～P5：現行規則那一支（沒協商 RTT）——和 VideoStream.c 原本那幾行相同
        memset(&s0, 0, sizeof(s0)); in.negotiatedRtt = false; in.legacyRttMs = 9;
        vrrrDecide(&in, &s0, &o); YES(o.mlRepair); NO(o.mlRttMode); NO(o.mlHoldDual); EQ(o.mlRepairRtoUs, 10000); EQ(o.mlHoldMaxUs, 8333);
        in.legacyRttMs = 10; vrrrDecide(&in, &s0, &o); YES(o.mlRepair);
        in.legacyRttMs = 11; vrrrDecide(&in, &s0, &o); NO(o.mlRepair); EQ(o.mlHoldMaxUs, 0);                          // P2：120 Hz 門檻 10 ms
        in.carrying = 2; in.legacyRttMs = 30; vrrrDecide(&in, &s0, &o); NO(o.mlRepair); YES(o.mlHoldDual); EQ(o.mlHoldMaxUs, 8333);   // P3
        in.carrying = 1; in.legacyRttMs = -1; vrrrDecide(&in, &s0, &o); NO(o.mlRepair); EQ(o.mlHoldMaxUs, 0);         // P4
        in.holdUs = 14000; in.legacyRttMs = 0; vrrrDecide(&in, &s0, &o); EQ(o.mlRepairRtoUs, 4000);                   // P5：重報間隔的夾法
        in.legacyRttMs = 3; vrrrDecide(&in, &s0, &o); EQ(o.mlRepairRtoUs, 9000);
        in.legacyRttMs = 19; vrrrDecide(&in, &s0, &o); YES(o.mlRepair); EQ(o.mlRepairRtoUs, 10000);
        in.legacyRttMs = 20; vrrrDecide(&in, &s0, &o); NO(o.mlRepair);
        in.negotiatedRepair = false; in.legacyRttMs = 5; vrrrDecide(&in, &s0, &o); NO(o.mlRepair); EQ(o.mlHoldMaxUs, 0);   // 沒協商補包
    }
    {
        // P5b：現行那一支對每一個往返毫秒數（−1～40）、三種更新率、一條／兩條在送，都和原本寫在接收迴圈裡的算式逐位元相同
        static const uint32_t holds[3] = {8333, 11111, 14000};
        int h, rtt, carrying;
        for (h = 0; h < 3; h++) {
            for (carrying = 0; carrying <= 2; carrying++) {
                for (rtt = -1; rtt <= 40; rtt++) {
                    VRRR_IN in = baseIn();
                    VRRR_STATE s0;
                    VRRR_OUT o;
                    const uint32_t mlHoldUs = holds[h];
                    const bool wantDual = carrying >= 2;
                    const bool wantRepair = rtt >= 0 && (uint32_t)rtt * 1000u + 2000u <= mlHoldUs + mlHoldUs / 2;
                    const uint32_t rtoUs = 2000u * (uint32_t)rtt + 3000u;
                    memset(&s0, 0, sizeof(s0));
                    in.negotiatedRtt = false; in.haveStats = false; in.holdUs = mlHoldUs; in.carrying = carrying; in.legacyRttMs = rtt;
                    vrrrDecide(&in, &s0, &o);
                    EQ(o.mlHoldDual, wantDual); EQ(o.mlRepair, wantRepair); NO(o.mlRttMode);
                    EQ(o.mlHoldMaxUs, (wantDual || wantRepair) ? mlHoldUs : 0);
                    if (wantRepair) {
                        EQ(o.mlRepairRtoUs, rtoUs < 4000u ? 4000u : (rtoUs > 10000u ? 10000u : rtoUs));
                    }
                }
            }
        }
    }
    {
        VRRR_IN in = baseIn();
        VRRR_STATE s0;
        VRRR_OUT o;
        memset(&s0, 0, sizeof(s0));
        vrrrDecide(&in, &s0, &o);                                                                                      // P6：RTT 規則
        YES(o.eligible); YES(o.mlRttMode); YES(o.mlRepair); NO(o.mlHoldDual); EQ(o.etaUs, 15625); EQ(o.etaTryUs, 11600);
        EQ(o.etaFastUs, 10100); EQ(o.capUs, 27725); EQ(o.loUs, 8100); EQ(o.mlRepairRtoUs, 15625); EQ(o.mlHoldMaxUs, 8333);
        EQ(o.idlePolicy, VRRR_IDLE_SIZE);
        in.armRtt = false; vrrrDecide(&in, &s0, &o);                                                                   // P7：A/B 的現行那一邊
        NO(o.eligible); NO(o.mlRttMode); YES(o.mlRepair); EQ(o.mlRepairRtoUs, 10000);
        in.armRtt = true; in.haveStats = false; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode); YES(o.mlRepair);            // P8：沒有樣本 → 現行規則
        in.haveStats = true; in.capOverrideMs = 14; vrrrDecide(&in, &s0, &o); YES(o.mlRttMode); EQ(o.capUs, 14000);    // P9：指定等待上限
        in.capOverrideMs = 0; in.framePeriodUs = 16666; in.holdUs = 14000; vrrrDecide(&in, &s0, &o);                   // P10：60 Hz
        YES(o.mlRttMode); EQ(o.capUs, 28000); EQ(o.idlePolicy, VRRR_IDLE_FIXED3); EQ(o.mlHoldMaxUs, 14000);
        in.negotiatedRepair = false; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode); NO(o.mlRepair);                        // P10b：沒協商補包就沒有 RTT 規則
        in.negotiatedRepair = true; in.legacyRttMs = -1; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode); NO(o.mlRepair);    // P10c：沒有已確認的連線
    }
    {
        VRRR_IN in = baseIn();
        VRRR_STATE s0;
        VRRR_OUT o;
        memset(&s0, 0, sizeof(s0));
        vrrrDecide(&in, &s0, &o); YES(o.mlRttMode);
        in.st.midUs = 24000; in.st.hiUs = 26000; vrrrDecide(&in, &s0, &o);                                             // P11：門檻關（26000 > 25000）
        YES(o.eligible); NO(o.mlRttMode); NO(o.mlRepair); EQ(o.mlHoldMaxUs, 0);
        in.st.midUs = 22500; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode);                                                //      遲滯：24500＋2000 > 25000
        in.st.midUs = 21000; vrrrDecide(&in, &s0, &o); YES(o.mlRttMode);                                               //      23000＋2000 ≤ 25000 → 再開
        in.st.midUs = 24000; in.latched = true; vrrrDecide(&in, &s0, &o);                                              // P12：正在等的 block 沿用 RTT 規則
        YES(o.mlRttMode); YES(o.mlRepair); EQ(o.etaTryUs, 26000);
        in.latched = false; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode); NO(o.mlRepair);                                 //      等完了 → 關
        in.latched = true; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode);                                                  //      關著的時候才開始等的不算（上一個封包不是 RTT 規則）
    }
    {
        VRRR_IN in = baseIn();
        VRRR_STATE s0;
        VRRR_OUT o;
        memset(&s0, 0, sizeof(s0));
        vrrrDecide(&in, &s0, &o); YES(o.mlRttMode);
        in.carrying = 2; in.latched = true; vrrrDecide(&in, &s0, &o);                                                  // P13：第二條連線開始送、但這個 block 還在等
        NO(o.eligible); YES(o.mlRttMode); NO(o.mlHoldDual); EQ(o.etaUs, 15625); EQ(o.capUs, 27725);
        in.latched = false; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode); YES(o.mlHoldDual); YES(o.mlRepair);             //      等完了 → 兩條連線的現行規則
        in.carrying = 1; in.armRtt = false; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode);
    }
    {
        VRRR_IN in = baseIn();
        VRRR_STATE s0;
        VRRR_OUT o;
        memset(&s0, 0, sizeof(s0));
        vrrrDecide(&in, &s0, &o); YES(o.mlRttMode);
        in.armRtt = false; in.latched = true; vrrrDecide(&in, &s0, &o);                                                // P13b：A/B 換到現行那一邊、但這個 block 還在等
        NO(o.eligible); YES(o.mlRttMode); EQ(o.capUs, 27725);
        in.latched = false; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode); YES(o.mlRepair); EQ(o.mlRepairRtoUs, 10000);    //       等完了 → 現行規則
    }
    {
        VRRR_IN in = baseIn();
        VRRR_STATE s0;
        VRRR_OUT o;
        memset(&s0, 0, sizeof(s0));
        in.badDelta = 1; vrrrDecide(&in, &s0, &o); NO(o.tripped); YES(o.mlRttMode);                                    // P14：保險
        in.nowUs += 500000; vrrrDecide(&in, &s0, &o); NO(o.tripped);
        in.nowUs += 500000; vrrrDecide(&in, &s0, &o); YES(o.tripped); YES(o.paused); NO(o.mlRttMode); NO(o.mlRepair); EQ(o.mlHoldMaxUs, 0);
        in.badDelta = 0; in.nowUs += 1999999; vrrrDecide(&in, &s0, &o); YES(o.paused); NO(o.mlRttMode);
        in.nowUs += 1; vrrrDecide(&in, &s0, &o); NO(o.paused); YES(o.mlRttMode);                                       //      2 s 後恢復
        in.okDelta = 1; in.nowUs += 1000; vrrrDecide(&in, &s0, &o); NO(o.tripped); EQ(s0.brk.level, 0);
    }
    {
        VRRR_IN in = baseIn();
        VRRR_STATE s0;
        VRRR_OUT o;
        memset(&s0, 0, sizeof(s0));
        vrrrDecide(&in, &s0, &o); YES(o.mlRttMode);
        in.badDelta = 3; in.latched = true; in.nowUs += 1000; vrrrDecide(&in, &s0, &o);                                // P15：保險剛暫停、但這個 block 還在等
        YES(o.tripped); YES(o.paused); YES(o.mlRttMode); YES(o.mlRepair);
        in.badDelta = 0; in.latched = false; in.nowUs += 1000; vrrrDecide(&in, &s0, &o); NO(o.mlRttMode); NO(o.mlRepair);   //   等完了 → 暫停生效
    }
    {
        VRRR_IN in = baseIn();
        VRRR_STATE s0;
        VRRR_OUT o;
        memset(&s0, 0, sizeof(s0));
        in.armRtt = false; in.badDelta = 3; vrrrDecide(&in, &s0, &o);                                                  // P16：現行那一邊的結果不進保險
        NO(o.tripped); EQ(s0.brk.streak, 0);
        in.armRtt = true; in.badDelta = 0; vrrrDecide(&in, &s0, &o); YES(o.mlRttMode); NO(o.paused);
    }

    printf("VrRepairRtt.h: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
