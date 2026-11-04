/**
 * @file tests/unit/test_vr_abr_delay_trend.cpp
 * @brief VipleStream 2.0 §ABR-ADAPT：排隊延遲趨勢估計器單元測試（第 1 期第 1 段）。
 */
#include "../tests_common.h"

#include <cmath>
#include <random>

#include <src/vr/abr_delay_trend.h>

namespace dt = vr::abr;
using namespace std::chrono_literals;

namespace {
  struct VrAbrDelayTrendTest : testing::Test {
    dt::steady::time_point t0 = dt::steady::now();
  };
}  // namespace

// 1. 行動網路抖動不誤判：base 38 ms，每 100 ms 一筆，rtt = 38 + 均勻亂數 [0, 25]（固定種子 std::mt19937(1)），跑 60 s
//    -> 全程不得進入 overuse；γ 最後應 > 初值 (12.5 ms)。
TEST_F(VrAbrDelayTrendTest, MobileNetworkJitterDoesNotTriggerOveruse) {
  dt::delay_trend_t d;
  const double init_gamma = d.gamma_ms();
  EXPECT_DOUBLE_EQ(init_gamma, 12.5);

  std::mt19937 rng(1);
  std::uniform_real_distribution<double> dist(0.0, 25.0);

  bool entered_overuse = false;
  for (int i = 0; i < 600; ++i) {
    const auto t = t0 + i * 100ms;
    const double rtt = 38.0 + dist(rng);
    d.on_rtt(rtt, t);
    if (d.state() == dt::trend_state_e::overuse) {
      entered_overuse = true;
    }
  }

  EXPECT_FALSE(entered_overuse);
  EXPECT_GT(d.gamma_ms(), init_gamma);
  EXPECT_GE(d.base_ms(), 35.0);
  EXPECT_LE(d.base_ms(), 45.0);
  EXPECT_GT(d.jitter_ms(), 15.0);
}

// 2. 偶發尖峰不誤判：同上，再每 7 s 插一筆 +120 ms 的尖峰 -> 不得 overuse。
TEST_F(VrAbrDelayTrendTest, OccasionalSpikesDoNotTriggerOveruse) {
  dt::delay_trend_t d;
  std::mt19937 rng(1);
  std::uniform_real_distribution<double> dist(0.0, 25.0);

  bool entered_overuse = false;
  for (int i = 0; i < 600; ++i) {
    const auto t = t0 + i * 100ms;
    double rtt = 38.0 + dist(rng);
    if (i > 0 && i % 70 == 0) {
      rtt += 120.0;  // 每 7 秒插入一筆 +120 ms 孤立尖峰
    }
    d.on_rtt(rtt, t);
    if (d.state() == dt::trend_state_e::overuse) {
      entered_overuse = true;
    }
  }

  EXPECT_FALSE(entered_overuse);
  EXPECT_GT(d.gamma_ms(), dt::k_trend_gamma_initial_ms);
}

// 3. bufferbloat 偵測：base 20 ms 平穩 10 s，之後每 100 ms +5 ms（每秒 +50 ms）-> 在開始上升後 1.0 s 內進入 overuse。
TEST_F(VrAbrDelayTrendTest, BufferbloatDetectedWithinOneSecond) {
  dt::delay_trend_t d;

  // 10 s 平穩基準（20 ms）
  for (int i = 0; i < 100; ++i) {
    d.on_rtt(20.0, t0 + i * 100ms);
    EXPECT_EQ(d.state(), dt::trend_state_e::normal);
  }

  const auto t_start_rise = t0 + 10000ms;
  double rtt = 20.0;
  bool entered_overuse = false;
  double detect_latency_sec = -1.0;

  for (int i = 1; i <= 30; ++i) {
    rtt += 5.0;  // 每 100 ms +5 ms
    const auto t = t_start_rise + i * 100ms;
    d.on_rtt(rtt, t);
    if (d.state() == dt::trend_state_e::overuse && !entered_overuse) {
      entered_overuse = true;
      detect_latency_sec = i * 0.1;
    }
  }

  EXPECT_TRUE(entered_overuse);
  EXPECT_LE(detect_latency_sec, 1.0);
  EXPECT_GE(detect_latency_sec, 0.3);  // 需持續至少 300 ms
}

// 4. 慢速膨脹靠安全網：base 30，之後每秒 +8 ms 持續 15 s -> 最終要 overuse（記錄花幾秒）。
TEST_F(VrAbrDelayTrendTest, SlowInflationTriggersSafetyNetOveruse) {
  dt::delay_trend_t d;

  // 先給予 5 秒 base 30 ms 建立基準
  for (int i = 0; i < 50; ++i) {
    d.on_rtt(30.0, t0 + i * 100ms);
  }

  const auto t_start_rise = t0 + 5000ms;
  double rtt = 30.0;
  bool entered_overuse = false;
  double elapsed_sec = -1.0;

  for (int i = 1; i <= 150; ++i) {
    rtt += 0.8;  // 每 100 ms +0.8 ms，相當於每秒 +8 ms
    const auto t = t_start_rise + i * 100ms;
    d.on_rtt(rtt, t);
    if (d.state() == dt::trend_state_e::overuse && !entered_overuse) {
      entered_overuse = true;
      elapsed_sec = i * 0.1;
    }
  }

  EXPECT_TRUE(entered_overuse);
  EXPECT_GT(elapsed_sec, 0.0);
  EXPECT_LE(elapsed_sec, 15.0);
  // 記錄並驗證慢速膨脹花費時間
  std::cout << "[INFO] Slow bufferbloat detected after " << elapsed_sec << " seconds" << std::endl;
}

// 5. LAN 小台階：base 1 ms ± 0.2，之後一次跳 +25 ms 並持續上升每秒 +20 ms -> 1 s 內 overuse。
TEST_F(VrAbrDelayTrendTest, LanSmallStepDetectedWithinOneSecond) {
  dt::delay_trend_t d;
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> dist(-0.2, 0.2);

  // 5 秒 LAN 平靜基準
  for (int i = 0; i < 50; ++i) {
    d.on_rtt(1.0 + dist(rng), t0 + i * 100ms);
    EXPECT_EQ(d.state(), dt::trend_state_e::normal);
  }

  const auto t_jump = t0 + 5000ms;
  double rtt = 26.0;  // 一次跳躍 +25 ms（1.0 -> 26.0）
  bool entered_overuse = false;
  double detect_latency_sec = -1.0;

  for (int i = 1; i <= 20; ++i) {
    const auto t = t_jump + i * 100ms;
    d.on_rtt(rtt, t);
    rtt += 2.0;  // 持續每秒 +20 ms (每 100 ms +2 ms)
    if (d.state() == dt::trend_state_e::overuse && !entered_overuse) {
      entered_overuse = true;
      detect_latency_sec = i * 0.1;
    }
  }

  EXPECT_TRUE(entered_overuse);
  EXPECT_LE(detect_latency_sec, 1.0);
}

// 補充：快照、無效 RTT 忽略、overuse_recently 視窗判定測試
TEST_F(VrAbrDelayTrendTest, SnapshotAndRecentOveruseTracking) {
  dt::delay_trend_t d;

  // 非法 RTT <= 0 忽略
  d.on_rtt(0.0, t0);
  d.on_rtt(-10.0, t0 + 100ms);
  EXPECT_DOUBLE_EQ(d.base_ms(), 0.0);

  // 送入正常樣本
  for (int i = 0; i < 100; ++i) {
    d.on_rtt(20.0, t0 + i * 100ms);
  }

  // 快照讀取
  auto snap = d.snapshot();
  EXPECT_DOUBLE_EQ(snap.base_ms, 20.0);
  EXPECT_DOUBLE_EQ(snap.jitter_ms, 0.0);
  EXPECT_EQ(snap.state, dt::trend_state_e::normal);

  // 注入急劇上升觸發 overuse
  auto t_rise = t0 + 10000ms;
  double rtt = 20.0;
  for (int i = 1; i <= 20; ++i) {
    rtt += 10.0;
    d.on_rtt(rtt, t_rise + i * 100ms);
  }
  EXPECT_EQ(d.state(), dt::trend_state_e::overuse);
  const auto t_over = t_rise + 2000ms;
  EXPECT_TRUE(d.overuse_recently(t_over, 1500ms));
  EXPECT_TRUE(d.overuse_recently(t_over + 1000ms, 1500ms));
  EXPECT_FALSE(d.overuse_recently(t_over + 2000ms, 1500ms));
}
