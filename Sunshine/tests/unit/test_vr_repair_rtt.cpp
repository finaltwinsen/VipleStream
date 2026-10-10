/**
 * @file tests/unit/test_vr_repair_rtt.cpp
 * @brief VipleStream 2.0 §VR-REPAIR-RTT：補包的時間預算跟著往返時間走的純邏輯（common-c 的 VrRepairRtt.h）。
 *
 * 這個標頭是 client（C）的程式，server 不編它；放在這裡測，是因為專案裡跑得起來的單元測試只有這一套，而且
 * Sunshine/third-party 的那一份要和 client 的逐位元組相同（check_commonc_sync.ps1）。同一批期望值另有不靠測試框架
 * 的版本：moonlight-qt/moonlight-common-c/tests/vr_repair_sim/test_vrrr.c（以 C 編一次、以 C++ 再編一次）。
 */
#include "../tests_common.h"

#include <moonlight-common-c/src/VrRepairRtt.h>

#include <cstring>
#include <initializer_list>

namespace {
  VRRR_RTT_STATS stats_of(std::initializer_list<uint16_t> ms10) {
    VRRR_RTT_WINDOW w;
    vrrrRttReset(&w);
    for (const uint16_t v : ms10) {
      vrrrRttAdd(&w, v);
    }
    VRRR_RTT_STATS st {};
    EXPECT_TRUE(vrrrRttStats(&w, &st));
    return st;
  }

  /// 這條路徑的典型狀況：只有一條連線在送、往返視窗 8.1／9.6／10.9 ms、120 Hz
  VRRR_IN base_in() {
    VRRR_IN in;
    std::memset(&in, 0, sizeof(in));
    in.negotiatedRepair = true;
    in.negotiatedRtt = true;
    in.armRtt = true;
    in.carrying = 1;
    in.legacyRttMs = 10;
    in.haveStats = true;
    in.st.loUs = 8100;
    in.st.midUs = 9600;
    in.st.hiUs = 10900;
    in.st.samples = 8;
    in.holdUs = 8333;
    in.framePeriodUs = 8333;
    in.nowUs = 1000000;
    return in;
  }

  VRRR_OUT decide(const VRRR_IN &in, VRRR_STATE &s) {
    VRRR_OUT o;
    vrrrDecide(&in, &s, &o);
    return o;
  }
}  // namespace

// ── 往返時間視窗：最小／中位數／第二大 ─────────────────────────────

TEST(VrRepairRtt, WindowStats) {
  VRRR_RTT_WINDOW w;
  VRRR_RTT_STATS st {};
  vrrrRttReset(&w);
  EXPECT_FALSE(vrrrRttStats(&w, &st));  // 沒有樣本

  st = stats_of({96});
  EXPECT_EQ(st.loUs, 9600u);
  EXPECT_EQ(st.midUs, 9600u);
  EXPECT_EQ(st.hiUs, 9600u);
  EXPECT_EQ(st.samples, 1);

  st = stats_of({80, 120});  // 不到 3 筆：hi 取最大；偶數筆的中位數取較大的
  EXPECT_EQ(st.loUs, 8000u);
  EXPECT_EQ(st.midUs, 12000u);
  EXPECT_EQ(st.hiUs, 12000u);

  st = stats_of({80, 96, 383});  // 單一離群值不進 hi
  EXPECT_EQ(st.midUs, 9600u);
  EXPECT_EQ(st.hiUs, 9600u);

  st = stats_of({81, 86, 96, 107, 132, 96, 383, 90});
  EXPECT_EQ(st.loUs, 8100u);
  EXPECT_EQ(st.midUs, 9600u);
  EXPECT_EQ(st.hiUs, 13200u);
  EXPECT_EQ(st.samples, 8);

  st = stats_of({81, 86, 96, 107, 132, 96, 383, 90, 250});  // 第 9 筆把最舊的擠掉
  EXPECT_EQ(st.loUs, 8600u);
  EXPECT_EQ(st.midUs, 10700u);
  EXPECT_EQ(st.hiUs, 25000u);
  EXPECT_EQ(st.samples, 8);

  st = stats_of({81, 86, 96, 107, 132, 96, 383, 90, 300, 300, 300, 300, 300, 300, 300, 300});
  EXPECT_EQ(st.loUs, 30000u);
  EXPECT_EQ(st.hiUs, 30000u);
}

TEST(VrRepairRtt, OnlyTheLinksThatCarryVideoCount) {
  // 補包只從正在送影像的連線回來：估計不可以取自沒在送的快連線
  VRRR_RTT_STATS pl[2] {};
  pl[0].loUs = 8000;
  pl[0].midUs = 9600;
  pl[0].hiUs = 10900;
  pl[0].samples = 8;
  pl[1].loUs = 3000;
  pl[1].midUs = 3300;
  pl[1].hiUs = 3500;
  pl[1].samples = 5;
  VRRR_RTT_STATS st {};
  ASSERT_TRUE(vrrrRttCombine(pl, 2, 0x1, 0x3, &st));
  EXPECT_EQ(st.loUs, 8000u);
  EXPECT_EQ(st.midUs, 9600u);
  EXPECT_EQ(st.hiUs, 10900u);
  ASSERT_TRUE(vrrrRttCombine(pl, 2, 0x2, 0x3, &st));
  EXPECT_EQ(st.hiUs, 3500u);
  ASSERT_TRUE(vrrrRttCombine(pl, 2, 0x3, 0x3, &st));  // 兩條都在送：等最慢的那條
  EXPECT_EQ(st.loUs, 3000u);
  EXPECT_EQ(st.midUs, 9600u);
  EXPECT_EQ(st.hiUs, 10900u);
  EXPECT_EQ(st.samples, 5);
  ASSERT_TRUE(vrrrRttCombine(pl, 2, 0x0, 0x3, &st));  // 還不知道哪條在送：看全部
  EXPECT_EQ(st.loUs, 3000u);
  EXPECT_EQ(st.hiUs, 10900u);
  EXPECT_FALSE(vrrrRttCombine(pl, 2, 0x1, 0x2, &st));  // 在送的那條沒有樣本
  EXPECT_FALSE(vrrrRttCombine(pl, 2, 0x3, 0x0, &st));
}

// ── 三個估計、等待上限、門檻 ───────────────────────────────────────

TEST(VrRepairRtt, ThreeEstimates) {
  EXPECT_EQ(vrrrEtaUs(0), 2000u);
  EXPECT_EQ(vrrrEtaUs(5000), 8250u);
  EXPECT_EQ(vrrrEtaUs(9600), 14000u);
  EXPECT_EQ(vrrrEtaUs(10000), 14500u);
  EXPECT_EQ(vrrrEtaUs(10900), 15625u);
  EXPECT_EQ(vrrrEtaUs(13200), 18500u);
  EXPECT_EQ(vrrrEtaUs(15000), 20750u);
  EXPECT_EQ(vrrrEtaUs(20000), 27000u);
  EXPECT_EQ(vrrrEtaUs(30000), 39500u);
  EXPECT_EQ(vrrrEtaTryUs(9600), 11600u);
  EXPECT_EQ(vrrrEtaTryUs(23000), 25000u);
  EXPECT_EQ(vrrrEtaFastUs(8100), 10100u);
}

TEST(VrRepairRtt, HoldCapFollowsTheRoundTrip) {
  EXPECT_EQ(vrrrCapUs(8333, 15625, 10100, 0), 27725u);  // 這條路徑的典型值
  EXPECT_EQ(vrrrCapUs(8333, 14500, 12000, 0), 28500u);
  EXPECT_EQ(vrrrCapUs(8333, 20750, 11000, 0), 33000u);  // 上限 33 ms
  EXPECT_EQ(vrrrCapUs(8333, 5750, 5000, 0), 16666u);  // 區網：不少於原本有進展時的兩個幀週期
  EXPECT_EQ(vrrrCapUs(11111, 5750, 5000, 0), 22222u);
  EXPECT_EQ(vrrrCapUs(14000, 3250, 3000, 0), 28000u);
  EXPECT_EQ(vrrrCapUs(14000, 14500, 12000, 0), 28500u);
  EXPECT_EQ(vrrrCapUs(8333, 15625, 10100, 20), 20000u);  // 指定值夾在 8～100 ms
  EXPECT_EQ(vrrrCapUs(8333, 15625, 10100, 4), 8000u);
  EXPECT_EQ(vrrrCapUs(8333, 15625, 10100, 500), 100000u);
}

TEST(VrRepairRtt, GateWithHysteresis) {
  EXPECT_TRUE(vrrrGateOpen(25000, 33000, true));
  EXPECT_FALSE(vrrrGateOpen(25001, 33000, true));
  EXPECT_TRUE(vrrrGateOpen(23000, 33000, false));  // 關掉之後要低 2 ms 才再開
  EXPECT_FALSE(vrrrGateOpen(23001, 33000, false));
  EXPECT_TRUE(vrrrGateOpen(14000, 14000, true));  // 等待上限被指定成 14 ms：門檻跟著降
  EXPECT_FALSE(vrrrGateOpen(14001, 14000, true));
  EXPECT_TRUE(vrrrGateOpen(12000, 14000, false));
  EXPECT_FALSE(vrrrGateOpen(12001, 14000, false));
}

// ── 期限、值不值得報、重報 ─────────────────────────────────────────

TEST(VrRepairRtt, LimitAndFeasibility) {
  EXPECT_EQ(vrrrLimitUs(1000, 0, 400, 8333, 27725), 36458u);  // 還沒有後面的封包：估下一幀在第一個封包＋一個幀週期到
  EXPECT_EQ(vrrrLimitUs(1000, 9000, 400, 8333, 27725), 36725u);  // 有後面的封包：它到達之後 cap
  EXPECT_EQ(vrrrLimitUs(1000, 0, 0, 8333, 27725), 28725u);
  EXPECT_EQ(vrrrLimitUs(20000, 0, 400, 8333, 27725), 47725u);
  EXPECT_TRUE(vrrrFeasible(1000, 11600, 12600));
  EXPECT_FALSE(vrrrFeasible(1001, 11600, 12600));
}

TEST(VrRepairRtt, RetryTiming) {
  EXPECT_EQ(vrrrRetryAtUs(20000, 9000, 15625, 10100, 99999, 3, 3), 0u);  // 次數用完
  EXPECT_EQ(vrrrRetryAtUs(10000, 9000, 15625, 10100, 36725, 1, 3), 24625u);  // 上一份＋eta
  EXPECT_EQ(vrrrRetryAtUs(10000, 9000, 15625, 10100, 34725, 1, 3), 24625u);  // 剛好趕得上
  EXPECT_EQ(vrrrRetryAtUs(10000, 9000, 15625, 10100, 34724, 1, 3), 0u);  // 差 1 µs：不報
  EXPECT_EQ(vrrrRetryAtUs(10000, 0, 15625, 10100, 36725, 1, 3), 10000u);
  EXPECT_EQ(vrrrRetryAtUs(30000, 9000, 15625, 10100, 45000, 1, 3), 30000u);  // 該報的時刻已經過了：現在
  EXPECT_EQ(vrrrRetryAtUs(30000, 9000, 15625, 10100, 40099, 1, 3), 0u);
}

TEST(VrRepairRtt, HeadDeadline) {
  // 後面的封包 9000 到、cap 27725、etaTry 11600
  EXPECT_EQ(vrrrHeadDeadlineUs(10000, 9000, 27725, 11600, true, false, true), 9000u);  // server 說補不了
  EXPECT_EQ(vrrrHeadDeadlineUs(10000, 9000, 27725, 11600, false, true, true), 9000u);  // 後面缺太多幀
  EXPECT_EQ(vrrrHeadDeadlineUs(10000, 9000, 27725, 11600, false, false, false), 36725u);  // 沒報過、值得試
  EXPECT_EQ(vrrrHeadDeadlineUs(25125, 9000, 27725, 11600, false, false, false), 36725u);
  EXPECT_EQ(vrrrHeadDeadlineUs(25126, 9000, 27725, 11600, false, false, false), 9000u);  // 趕不上：不等
  EXPECT_EQ(vrrrHeadDeadlineUs(10000, 9000, 27725, 11600, false, false, true), 36725u);  // 報過：等到上限
}

TEST(VrRepairRtt, LossHoldoffRepairGuardAndNackFlags) {
  EXPECT_EQ(vrrrLossHoldoffUs(8100, 8333), 18433u);
  EXPECT_EQ(vrrrLossHoldoffUs(10000, 16666), 28666u);

  EXPECT_TRUE(vrrrCannotBeRepair(5000, 1000, 8000));  // 回報之後不到一個往返就到的：不是補包
  EXPECT_FALSE(vrrrCannotBeRepair(8000, 1000, 8000));
  EXPECT_TRUE(vrrrCannotBeRepair(7999, 1000, 8000));
  EXPECT_FALSE(vrrrCannotBeRepair(5000, 0, 8000));  // 沒報過
  EXPECT_FALSE(vrrrCannotBeRepair(1100, 1000, 800));  // 往返短到分不出來
  EXPECT_TRUE(vrrrCannotBeRepair(16000, 1000, 20000));
  EXPECT_FALSE(vrrrCannotBeRepair(18500, 1000, 20000));

  EXPECT_EQ(vrrrNackFlags(15625), 0x90);
  EXPECT_EQ(vrrrNackFlags(14500), 0x8F);
  EXPECT_EQ(vrrrNackFlags(17000), 0x91);
  EXPECT_EQ(vrrrNackFlags(14001), 0x8F);
  EXPECT_EQ(vrrrNackFlags(0), 0x81);
  EXPECT_EQ(vrrrNackFlags(200000), 0xFF);
}

// ── 後面整幀沒到的幀 ───────────────────────────────────────────────

TEST(VrRepairRtt, WholeFramesMissingBehind) {
  EXPECT_EQ(vrrrBurstUs(210, 70), 14700u);
  EXPECT_EQ(vrrrBurstUs(84, 49), 4116u);
  EXPECT_EQ(vrrrBurstUs(10, 0), 0u);
  EXPECT_EQ(vrrrAheadMaxFrames(8333), 3u);
  EXPECT_EQ(vrrrAheadMaxFrames(6944), 3u);
  EXPECT_EQ(vrrrAheadMaxFrames(11111), 3u);
  EXPECT_EQ(vrrrAheadMaxFrames(13888), 2u);
  EXPECT_EQ(vrrrAheadMaxFrames(16666), 2u);
  EXPECT_EQ(vrrrAheadMaxFrames(40000), 0u);

  EXPECT_FALSE(vrrrGapTooMany(0, 25000, 12000, 52725, 8333, 9, 9, 0));
  EXPECT_TRUE(vrrrGapTooMany(4, 25000, 12000, 52725, 8333, 9, 9, 0));  // 120 Hz 缺 4 幀：不補
  EXPECT_FALSE(vrrrGapTooMany(3, 25000, 12000, 52725, 8333, 9, 9, 0));
  EXPECT_TRUE(vrrrGapTooMany(3, 25000, 12000, 52725, 16666, 9, 9, 0));  // 60 Hz 缺 3 幀：不補
  EXPECT_FALSE(vrrrGapTooMany(2, 25000, 12000, 52725, 16666, 9, 9, 0));
  EXPECT_FALSE(vrrrGapTooMany(1, 25000, 12000, 52725, 8333, 105, 105, 70));  // 兩整幀 210 個封包約 14.7 ms：來得及
  EXPECT_TRUE(vrrrGapTooMany(2, 25000, 12000, 52725, 8333, 105, 105, 70));  // 三整幀：來不及
  EXPECT_TRUE(vrrrGapTooMany(1, 25000, 12000, 52725, 8333, 105, 105, 80));  // 到達比較慢
  EXPECT_FALSE(vrrrGapTooMany(1, 25000, 12000, 52725, 8333, 30, 105, 49));
  EXPECT_TRUE(vrrrGapTooMany(1, 41000, 12000, 52725, 8333, 9, 9, 0));  // 已經太晚
}

// ── 「多久沒有新封包才回報」 ───────────────────────────────────────

TEST(VrRepairRtt, IdlePolicy) {
  VRRR_STALLS s;
  std::memset(&s, 0, sizeof(s));
  EXPECT_EQ(vrrrIdlePolicy(VRRR_IDLE_AUTO, 8333), VRRR_IDLE_SIZE);  // 100 Hz 以上依 block 大小
  EXPECT_EQ(vrrrIdlePolicy(VRRR_IDLE_AUTO, 10000), VRRR_IDLE_SIZE);
  EXPECT_EQ(vrrrIdlePolicy(VRRR_IDLE_AUTO, 10001), VRRR_IDLE_FIXED3);
  EXPECT_EQ(vrrrIdlePolicy(VRRR_IDLE_AUTO, 16666), VRRR_IDLE_FIXED3);

  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_FIXED3, &s, 1000000, 8333, 101), 3000u);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_FRAME, &s, 1000000, 8333, 9), 8333u);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_FRAME, &s, 1000000, 2000, 9), 3000u);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_SIZE, &s, 1000000, 8333, 9), 3000u);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_SIZE, &s, 1000000, 8333, 32), 3000u);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_SIZE, &s, 1000000, 8333, 33), 8333u);  // 超過 32 個 shard：等下一幀
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_SIZE, &s, 1000000, 8333, 101), 8333u);

  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1000000, 16666, 80), 3000u);  // 沒有樣本
  vrrrStallAdd(&s, 4000, 1000000);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1000000, 16666, 80), 3000u);  // 只有一筆
  vrrrStallAdd(&s, 5000, 1100000);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1100000, 16666, 80), 6000u);  // 第二大 4000 ×1.25＋1000
  vrrrStallAdd(&s, 4500, 1200000);
  vrrrStallAdd(&s, 4000, 1300000);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1300000, 16666, 80), 6625u);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1300000, 6000, 80), 6000u);  // 夾在幀週期
  vrrrStallAdd(&s, 2999, 1400000);  // 短於 3 ms 的不記
  EXPECT_EQ(vrrrStallHiUs(&s, 1400000), 4500u);
  EXPECT_EQ(vrrrStallHiUs(&s, 4150000), 4000u);  // 最舊兩筆過期
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 9000000, 16666, 80), 3000u);

  // 短於 3 ms 的不記：清空之後只加兩筆 2999，記了的話第二大會是 2999
  std::memset(&s, 0, sizeof(s));
  vrrrStallAdd(&s, 2999, 1000000);
  vrrrStallAdd(&s, 2999, 1000001);
  EXPECT_EQ(vrrrStallHiUs(&s, 1000002), 0u);
  EXPECT_EQ(vrrrIdleUs(VRRR_IDLE_LEARNED, &s, 1000002, 16666, 80), 3000u);
  vrrrStallAdd(&s, 3000, 1000002);  // 剛好 3 ms 的要記
  vrrrStallAdd(&s, 3000, 1000003);
  EXPECT_EQ(vrrrStallHiUs(&s, 1000004), 3000u);
}

// ── 保險 ───────────────────────────────────────────────────────────

TEST(VrRepairRtt, BreakerTripsOnThreeConsecutiveFailuresAndBacksOff) {
  VRRR_BREAKER b;
  std::memset(&b, 0, sizeof(b));
  EXPECT_FALSE(vrrrBreakerNote(&b, 1000000, 0, 1));
  EXPECT_FALSE(vrrrBreakerNote(&b, 1100000, 0, 1));
  EXPECT_TRUE(vrrrBreakerNote(&b, 1200000, 0, 1));  // 連續 3 次：暫停 2 s
  EXPECT_TRUE(vrrrBreakerPaused(&b, 3199999));
  EXPECT_FALSE(vrrrBreakerPaused(&b, 3200000));
  EXPECT_FALSE(vrrrBreakerNote(&b, 2000000, 0, 5));  // 暫停期間的結果不算
  EXPECT_TRUE(vrrrBreakerNote(&b, 3300000, 0, 1));  // 恢復後第一個結果就失敗：立刻再暫停，時間加倍
  EXPECT_EQ(b.level, 1);
  EXPECT_TRUE(vrrrBreakerPaused(&b, 7299999));
  EXPECT_FALSE(vrrrBreakerPaused(&b, 7300000));
  EXPECT_TRUE(vrrrBreakerNote(&b, 7400000, 0, 1));  // 8 s
  EXPECT_TRUE(vrrrBreakerNote(&b, 15500000, 0, 1));  // 16 s
  EXPECT_EQ(b.level, 3);
  EXPECT_TRUE(vrrrBreakerNote(&b, 31600000, 0, 1));  // 最多 16 s
  EXPECT_EQ(b.level, 3);
  EXPECT_TRUE(vrrrBreakerPaused(&b, 47599999));
  EXPECT_FALSE(vrrrBreakerPaused(&b, 47600000));
  EXPECT_FALSE(vrrrBreakerNote(&b, 47700000, 1, 0));  // 恢復後第一個結果成功：退避歸零
  EXPECT_EQ(b.level, 0);
  EXPECT_FALSE(vrrrBreakerNote(&b, 47800000, 0, 2));
  EXPECT_TRUE(vrrrBreakerNote(&b, 47900000, 0, 1));
  EXPECT_FALSE(vrrrBreakerPaused(&b, 49900000));  // 又是 2 s
}

TEST(VrRepairRtt, BreakerTripsOnFiveFailuresOutOfEight) {
  VRRR_BREAKER b;
  std::memset(&b, 0, sizeof(b));
  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(vrrrBreakerNote(&b, 1000000 + (uint64_t) i * 1000, 0, 1));
    EXPECT_FALSE(vrrrBreakerNote(&b, 1000500 + (uint64_t) i * 1000, 1, 0));
  }
  EXPECT_FALSE(vrrrBreakerNote(&b, 1010000, 0, 1));  // 最近 8 個裡 4 個失敗
  EXPECT_TRUE(vrrrBreakerNote(&b, 1011000, 0, 1));  // 5 個：暫停
}

// ── 同場 A/B ───────────────────────────────────────────────────────

TEST(VrRepairRtt, AbArmStartsOnTheLegacySide) {
  EXPECT_TRUE(vrrrAbArmRtt(0, 0));  // 沒開：一直在 RTT 那一邊
  EXPECT_FALSE(vrrrAbArmRtt(0, 40));
  EXPECT_FALSE(vrrrAbArmRtt(39999999, 40));
  EXPECT_TRUE(vrrrAbArmRtt(40000000, 40));
  EXPECT_FALSE(vrrrAbArmRtt(80000000, 40));
}

// ── 每個封包的決定 ─────────────────────────────────────────────────

TEST(VrRepairRtt, DecideWithoutTheFeatureIsTheOriginalRule) {
  // 沒協商 VIPLE_VR_LINK_F_REPAIR_RTT：和 VideoStream.c 原本寫在接收迴圈裡的算式逐位元相同
  for (const uint32_t hold : {8333u, 11111u, 14000u}) {
    for (int carrying = 0; carrying <= 2; ++carrying) {
      for (int rtt = -1; rtt <= 40; ++rtt) {
        VRRR_IN in = base_in();
        in.negotiatedRtt = false;
        in.haveStats = false;
        in.holdUs = hold;
        in.carrying = carrying;
        in.legacyRttMs = rtt;
        VRRR_STATE s {};
        const VRRR_OUT o = decide(in, s);

        const bool want_dual = carrying >= 2;
        const bool want_repair = rtt >= 0 && (uint32_t) rtt * 1000u + 2000u <= hold + hold / 2;
        ASSERT_EQ(o.mlHoldDual, want_dual) << hold << ' ' << carrying << ' ' << rtt;
        ASSERT_EQ(o.mlRepair, want_repair) << hold << ' ' << carrying << ' ' << rtt;
        ASSERT_FALSE(o.mlRttMode);
        ASSERT_EQ(o.mlHoldMaxUs, (want_dual || want_repair) ? hold : 0u);
        if (want_repair) {
          const uint32_t rto = 2000u * (uint32_t) rtt + 3000u;
          ASSERT_EQ(o.mlRepairRtoUs, rto < 4000u ? 4000u : (rto > 10000u ? 10000u : rto));
        }
      }
    }
  }
}

TEST(VrRepairRtt, DecideRttRules) {
  VRRR_IN in = base_in();
  VRRR_STATE s {};
  VRRR_OUT o = decide(in, s);
  EXPECT_TRUE(o.eligible);
  EXPECT_TRUE(o.mlRttMode);
  EXPECT_TRUE(o.mlRepair);
  EXPECT_FALSE(o.mlHoldDual);
  EXPECT_EQ(o.etaUs, 15625u);
  EXPECT_EQ(o.etaTryUs, 11600u);
  EXPECT_EQ(o.etaFastUs, 10100u);
  EXPECT_EQ(o.capUs, 27725u);
  EXPECT_EQ(o.loUs, 8100u);
  EXPECT_EQ(o.mlHoldMaxUs, 8333u);
  EXPECT_EQ(o.idlePolicy, VRRR_IDLE_SIZE);

  in.armRtt = false;  // A/B 的現行那一邊
  o = decide(in, s);
  EXPECT_FALSE(o.mlRttMode);
  EXPECT_TRUE(o.mlRepair);
  EXPECT_EQ(o.mlRepairRtoUs, 10000u);

  in.armRtt = true;
  in.haveStats = false;  // 在送的連線沒有樣本
  o = decide(in, s);
  EXPECT_FALSE(o.mlRttMode);

  in.haveStats = true;
  in.capOverrideMs = 14;
  o = decide(in, s);
  EXPECT_TRUE(o.mlRttMode);
  EXPECT_EQ(o.capUs, 14000u);

  in.capOverrideMs = 0;
  in.framePeriodUs = 16666;  // 60 Hz
  in.holdUs = 14000;
  o = decide(in, s);
  EXPECT_EQ(o.capUs, 28000u);
  EXPECT_EQ(o.idlePolicy, VRRR_IDLE_FIXED3);
}

TEST(VrRepairRtt, DecideOnlyWhenOneLinkCarriesVideo) {
  VRRR_IN in = base_in();
  in.carrying = 2;
  in.legacyRttMs = 30;
  VRRR_STATE s {};
  const VRRR_OUT o = decide(in, s);
  EXPECT_FALSE(o.eligible);
  EXPECT_FALSE(o.mlRttMode);
  EXPECT_TRUE(o.mlHoldDual);  // 兩條在送：原本的規則（等另一條的那一份）
  EXPECT_FALSE(o.mlRepair);
  EXPECT_EQ(o.mlHoldMaxUs, 8333u);
}

TEST(VrRepairRtt, DecideGateClosesAboveTwentyThreeMs) {
  VRRR_IN in = base_in();
  VRRR_STATE s {};
  EXPECT_TRUE(decide(in, s).mlRttMode);
  in.st.midUs = 24000;
  in.st.hiUs = 26000;
  VRRR_OUT o = decide(in, s);  // 門檻關：不回報、不等
  EXPECT_TRUE(o.eligible);
  EXPECT_FALSE(o.mlRttMode);
  EXPECT_FALSE(o.mlRepair);
  EXPECT_EQ(o.mlHoldMaxUs, 0u);
  in.st.midUs = 22500;
  EXPECT_FALSE(decide(in, s).mlRttMode);  // 遲滯
  in.st.midUs = 21000;
  EXPECT_TRUE(decide(in, s).mlRttMode);
}

TEST(VrRepairRtt, DecideKeepsTheRulesForABlockAlreadyWaiting) {
  // 門檻剛關、第二條連線開始送、A/B 換邊、保險剛暫停：已經照 RTT 規則回報、還在等的 block 沿用到結束
  {
    VRRR_IN in = base_in();
    VRRR_STATE s {};
    EXPECT_TRUE(decide(in, s).mlRttMode);
    in.st.midUs = 24000;
    in.st.hiUs = 26000;
    in.latched = true;
    VRRR_OUT o = decide(in, s);
    EXPECT_TRUE(o.mlRttMode);
    EXPECT_TRUE(o.mlRepair);
    in.latched = false;
    o = decide(in, s);
    EXPECT_FALSE(o.mlRttMode);
    in.latched = true;  // 關著的時候才開始等的不算
    EXPECT_FALSE(decide(in, s).mlRttMode);
  }
  {
    VRRR_IN in = base_in();
    VRRR_STATE s {};
    EXPECT_TRUE(decide(in, s).mlRttMode);
    in.carrying = 2;
    in.latched = true;
    VRRR_OUT o = decide(in, s);
    EXPECT_FALSE(o.eligible);
    EXPECT_TRUE(o.mlRttMode);
    EXPECT_FALSE(o.mlHoldDual);
    EXPECT_EQ(o.capUs, 27725u);
    in.latched = false;
    o = decide(in, s);
    EXPECT_FALSE(o.mlRttMode);
    EXPECT_TRUE(o.mlHoldDual);
  }
  {
    VRRR_IN in = base_in();
    VRRR_STATE s {};
    EXPECT_TRUE(decide(in, s).mlRttMode);
    in.armRtt = false;
    in.latched = true;
    EXPECT_TRUE(decide(in, s).mlRttMode);
    in.latched = false;
    EXPECT_FALSE(decide(in, s).mlRttMode);
  }
  {
    VRRR_IN in = base_in();
    VRRR_STATE s {};
    EXPECT_TRUE(decide(in, s).mlRttMode);
    in.badDelta = 3;
    in.latched = true;
    in.nowUs += 1000;
    VRRR_OUT o = decide(in, s);
    EXPECT_TRUE(o.tripped);
    EXPECT_TRUE(o.paused);
    EXPECT_TRUE(o.mlRttMode);
    in.badDelta = 0;
    in.latched = false;
    in.nowUs += 1000;
    EXPECT_FALSE(decide(in, s).mlRttMode);
  }
}

TEST(VrRepairRtt, DecideBreakerPausesForTwoSeconds) {
  VRRR_IN in = base_in();
  VRRR_STATE s {};
  in.badDelta = 1;
  EXPECT_FALSE(decide(in, s).tripped);
  in.nowUs += 500000;
  EXPECT_FALSE(decide(in, s).tripped);
  in.nowUs += 500000;
  VRRR_OUT o = decide(in, s);
  EXPECT_TRUE(o.tripped);
  EXPECT_TRUE(o.paused);
  EXPECT_FALSE(o.mlRttMode);
  EXPECT_FALSE(o.mlRepair);
  EXPECT_EQ(o.mlHoldMaxUs, 0u);
  in.badDelta = 0;
  in.nowUs += 1999999;
  EXPECT_TRUE(decide(in, s).paused);
  in.nowUs += 1;
  o = decide(in, s);
  EXPECT_FALSE(o.paused);
  EXPECT_TRUE(o.mlRttMode);
}

TEST(VrRepairRtt, DecideIgnoresResultsFromTheLegacySide) {
  VRRR_IN in = base_in();
  VRRR_STATE s {};
  in.armRtt = false;
  in.badDelta = 3;
  const VRRR_OUT o = decide(in, s);
  EXPECT_FALSE(o.tripped);
  EXPECT_EQ(s.brk.streak, 0);
}
