/**
 * @file tests/unit/test_vr_abr_fast.cpp
 * @brief VipleStream 2.0 §ABR-ADAPT：VR 自適應快速碼率控制器單元測試（第 1、2、3 期）。
 */
#include "../tests_common.h"

#include <cmath>
#include <random>
#include <string>
#include <vector>

#include <src/vr/vr_abr_fast.h>

namespace fast = vr::abr;
using namespace std::chrono_literals;

namespace {
  constexpr int kConfigured100Mbps = 100000;
  constexpr int kConfigured50Mbps = 50000;
  constexpr int kConfigured20Mbps = 20000;

  struct VrAbrFastTest : testing::Test {
    fast::steady::time_point t0 = fast::steady::now();

    /// 輔助函式：讓控制器內部 delay_trend_t 進入過載狀態（模擬網路壅塞）
    void make_trend_overuse(fast::fast_controller_t &c, fast::steady::time_point at) {
      // 5 秒 20 ms 基準，接著急劇上升觸發 delay-trend overuse
      for (int i = 0; i < 50; ++i) {
        c.on_rtt(20, at + i * 100ms);
      }
      double rtt = 20.0;
      for (int i = 1; i <= 20; ++i) {
        rtt += 10.0;
        c.on_rtt(static_cast<int>(rtt), at + 5000ms + i * 100ms);
      }
    }
  };

  /// 穩定在 75 Mbps、事件開始之後有過飽和降碼、applied 被放到 10 Mbps。給分步拉回的測試用。
  struct congestion_episode_t {
    fast::fast_controller_t c{kConfigured100Mbps};
    fast::steady::time_point episode;
    fast::steady::time_point t_sat;

    explicit congestion_episode_t(fast::steady::time_point t0)
        : episode(t0 + 13s), t_sat(t0 + 14s) {
      c.set_cap_kbps(75000);
      c.set_applied_kbps(75000);
      c.on_rates(75000, 75000, t0);  // 開場起點：14 秒後的飽和降碼才算數（開場 10 秒內不算）
      for (auto t = t0; t <= t0 + 12s; t += 100ms) {
        c.set_applied_kbps(75000);
        c.tick(t);
      }
      c.on_rates(40000, 75000, t_sat);  // 收 40、送 75：飽和
      c.on_loss(12, t_sat);
      c.tick(t_sat);
      c.set_applied_kbps(10000);
    }
  };
}  // namespace

// ==============================================================================
// 既有測試更新（因應 §ABR-ADAPT 規則改動更新並在註解說明）
// ==============================================================================

// 1. 單張救不回：§ABR-ADAPT 新規則下，零星丟包需伴隨延遲過載（或剛脫離過載 1.5s 內）才視為壅塞降碼打 9 折（×0.90），原因加註 +delay
TEST_F(VrAbrFastTest, SingleFecFailedCutsByTenPercent) {
  fast::fast_controller_t c(kConfigured100Mbps);
  EXPECT_EQ(c.state(), fast::fast_state_e::healthy);

  // 延遲平穩時視為隨機丟包，不降碼
  c.on_fec_failed(1, t0);
  EXPECT_EQ(c.state(), fast::fast_state_e::healthy);
  EXPECT_EQ(c.random_losses(), 1);

  // 注入延遲過載後，救不回事件即判定為壅塞觸發中度降碼（×0.90）
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);  // 先消化 delay-trend 降碼進入 hold/ramp
  c.set_applied_kbps(100000);  // 重設為基準測試

  const auto t_loss = t0 + 10000ms;
  c.on_fec_failed(1, t_loss);
  EXPECT_EQ(c.state(), fast::fast_state_e::cut);
  EXPECT_STREQ(c.last_reason(), "fec-failed-1+delay");

  auto next = c.tick(t_loss);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, 90000);  // 100000 * 0.90 = 90000
  EXPECT_EQ(c.applied_kbps(), 90000);
}

// 2. 1 秒內救不回 ≥ 2 張：伴隨延遲過載時觸發嚴重降碼打 75 折（×0.75），原因加註 +delay
TEST_F(VrAbrFastTest, TwoFecFailedWithinOneSecondCutsByTwentyFivePercent) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t_loss = t0 + 10000ms;
  c.on_fec_failed(2, t_loss);
  EXPECT_EQ(c.state(), fast::fast_state_e::cut);
  EXPECT_STREQ(c.last_reason(), "fec-failed-ge-2+delay");

  auto next = c.tick(t_loss);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, 75000);  // 100000 * 0.75 = 75000
}

// 2b. 1 秒內分批達到 2 張救不回亦觸發嚴重降碼（伴隨延遲過載）
TEST_F(VrAbrFastTest, TwoFecFailedBatchedWithinOneSecond) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t1 = t0 + 10000ms;
  c.on_fec_failed(1, t1);
  auto res1 = c.tick(t1);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(*res1, 90000);  // 第 1 張先打 9 折

  // 200 ms 後來第 2 張（1 秒內累積 2 張）
  const auto t2 = t1 + 200ms;
  c.on_fec_failed(1, t2);
  auto res2 = c.tick(t2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(*res2, 67500);  // 90000 * 0.75 = 67500
}

// 3. 同一秒內第二次嚴重降碼打 60 折（×0.60），原因加註 +delay
TEST_F(VrAbrFastTest, SecondCriticalCutWithinSameSecondCutsToSixtyPercent) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t1 = t0 + 10000ms;
  c.on_loss(2, t1);
  auto res1 = c.tick(t1);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(*res1, 75000);  // 100000 * 0.75 = 75000
  EXPECT_STREQ(c.last_reason(), "loss-ge-2+delay");

  // 500 ms 後（同一秒視窗內）第二次嚴重降碼（連線 dropBlock）
  const auto t2 = t1 + 500ms;
  c.on_drop_block(t2);
  auto res2 = c.tick(t2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(*res2, 60000);  // 基準 100000 * 0.60 = 60000
  EXPECT_STREQ(c.last_reason(), "drop-block+delay");
}

// §VR-ABR-CUT-NO-RAISE：第一刀之後舊 AIMD 把 applied 砍半，同一秒第二刀不可用舊基準把碼率拉高。
// 要繼續降（以砍半後的 applied × 同一個 0.60），不是停在原地，更不是拉回 60000。
TEST_F(VrAbrFastTest, SecondCriticalCutAfterExternalHalveStaysBelowApplied) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t1 = t0 + 10000ms;
  c.on_loss(2, t1);
  auto res1 = c.tick(t1);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(*res1, 75000);  // 第一刀：100000 × 0.75，基準留在 100000

  c.set_applied_kbps(37500);  // 舊 AIMD（丟包 ≥ 30%）再砍半

  const auto t2 = t1 + 500ms;
  c.on_drop_block(t2);
  auto res2 = c.tick(t2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_LT(*res2, 37500);
  EXPECT_EQ(*res2, static_cast<int>(37500 * 0.60f));  // 不是舊基準 100000 × 0.60 = 60000
  EXPECT_STREQ(c.last_reason(), "drop-block+delay");
}

// 同一條規則的飽和分支（V-4：17187 被拉回 48125）。收／送 = 0.8 時飽和係數約 0.76，
// 第二刀係數是 min(飽和係數, 0.70)＝0.70。外部砍半之後要從砍半後的 applied 再乘 0.70。
TEST_F(VrAbrFastTest, SecondSaturatedCutAfterExternalHalveStaysBelowApplied) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(100000);
  c.set_applied_kbps(100000);
  c.on_rates(100000, 100000, t0);  // 開場起點，後面的飽和降碼才算數
  const auto t1 = t0 + 20s;
  c.on_rates(80000, 100000, t1);  // 收 80、送 100
  c.on_loss(12, t1);
  auto res1 = c.tick(t1);
  ASSERT_TRUE(res1.has_value());

  const int half = *res1 / 2;
  c.set_applied_kbps(half);

  const auto t2 = t1 + 500ms;
  c.on_loss(12, t2);
  auto res2 = c.tick(t2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_LT(*res2, half);
  EXPECT_EQ(*res2, static_cast<int>(std::round(half * 0.70)));
}

// §VR-ABR-CUT-NO-RAISE：pending 是上一拍排好的降碼。外部把 applied 砍到 pending 以下之後，
// tick 不可以把那個 pending 套回去把碼率拉高。
TEST_F(VrAbrFastTest, PendingCutDoesNotRaiseAfterExternalDrop) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t_loss = t0 + 10000ms;
  c.on_loss(2, t_loss);           // 排入 100000 × 0.75＝75000，applied 還是 100000
  c.set_applied_kbps(50000);      // 外部砍到 pending 以下
  const int before = c.applied_kbps();
  auto r = c.tick(t_loss);
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(c.applied_kbps(), before);
  EXPECT_EQ(c.applied_kbps(), 50000);
  EXPECT_EQ(c.state(), fast::fast_state_e::hold);
}

// 同秒第二刀：外部降碼剛好落在「基準 × 係數」上時，仍要以現在的 applied 再乘同一個係數，不能停在原地。
TEST_F(VrAbrFastTest, SecondCriticalCutAtFromBaseKeepsCutting) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t1 = t0 + 10000ms;
  c.on_loss(2, t1);
  auto res1 = c.tick(t1);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(*res1, 75000);

  const int from_base = static_cast<int>(100000 * 0.60f);  // 第二刀用的舊基準 × 0.60
  c.set_applied_kbps(from_base);

  const auto t2 = t1 + 500ms;
  c.on_drop_block(t2);
  auto res2 = c.tick(t2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(*res2, static_cast<int>(from_base * 0.60f));
  EXPECT_LT(*res2, from_base);
}

// 飽和分支的同一種邊界：from_base 剛好等於 applied 時，第二刀要繼續降。
TEST_F(VrAbrFastTest, SecondSaturatedCutAtFromBaseKeepsCutting) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(100000);
  c.set_applied_kbps(100000);
  c.on_rates(100000, 100000, t0);
  const auto t1 = t0 + 20s;
  c.on_rates(80000, 100000, t1);  // 飽和係數 0.76，第二刀係數是 min(0.76, 0.70)＝0.70
  c.on_loss(12, t1);
  auto res1 = c.tick(t1);
  ASSERT_TRUE(res1.has_value());

  const int from_base = static_cast<int>(std::round(100000 * 0.70));
  c.set_applied_kbps(from_base);

  const auto t2 = t1 + 500ms;
  c.on_loss(12, t2);
  auto res2 = c.tick(t2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(*res2, static_cast<int>(std::round(from_base * 0.70)));
  EXPECT_LT(*res2, from_base);
}

// 4. HOLD 期間禁止任何碼率回升
TEST_F(VrAbrFastTest, HoldPeriodProhibitsBitrateRamping) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t_cut = t0 + 10000ms;
  c.on_loss(2, t_cut);
  auto res = c.tick(t_cut);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(*res, 75000);
  EXPECT_EQ(c.state(), fast::fast_state_e::hold);

  // 在 HOLD 冷卻期間內（未滿 600 ms），tick 不得回升任何碼率
  EXPECT_FALSE(c.tick(t_cut + 200ms).has_value());
  EXPECT_FALSE(c.tick(t_cut + 400ms).has_value());
  EXPECT_FALSE(c.tick(t_cut + 590ms).has_value());
  EXPECT_EQ(c.state(), fast::fast_state_e::hold);
  EXPECT_EQ(c.current_target_kbps(), 75000);
}

// 5. 自適應 RAMP 平滑回升：隨 base RTT 動態調整間隔與步長
//    §ABR-ADAPT 新規則：HOLD 與 RAMP 間隔隨 base RTT 縮放，回升初期採乘法增加
TEST_F(VrAbrFastTest, AdaptiveRampTimingAndStep) {
  fast::fast_controller_t c(kConfigured100Mbps);
  // 給定穩定 base 200 ms
  for (int i = 0; i < 50; ++i) {
    c.on_rtt(200, t0 + i * 100ms);
  }
  const auto t_cut = t0 + 5000ms;
  // 透過 12 筆 persistent 丟包觸發降碼
  c.on_loss(12, t_cut);
  c.tick(t_cut);
  EXPECT_EQ(c.applied_kbps(), 75000);
  EXPECT_EQ(c.state(), fast::fast_state_e::hold);

  // base 200 ms 時 HOLD 長度 = 8 * 200 = 1600 ms
  EXPECT_FALSE(c.tick(t_cut + 1590ms).has_value());
  c.tick(t_cut + 1600ms);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // base 200 ms 時 RAMP 間隔 = clamp(4 * 200, 250, 1000) = 800 ms
  // 滿 800 ms（t_cut + 2400ms）第一次回升：乘法步長 = 75000 * 0.08 * 0.8s = 4800 kbps
  EXPECT_FALSE(c.tick(t_cut + 2390ms).has_value());
  auto r1 = c.tick(t_cut + 2400ms);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, 79800);  // 75000 + 4800 = 79800
}

// 6. 下限：§ABR-ADAPT 改為技術最低值 k_floor_tech_kbps = 2000 kbps，不再採用 10 Mbps / 30%
TEST_F(VrAbrFastTest, LowerBoundFloorProtection) {
  // 案例 A：設定 20 Mbps，舊規則下限為 10 Mbps；新規則技術下限為 2 Mbps (2000 Kbps)
  {
    fast::fast_controller_t c(kConfigured20Mbps);
    EXPECT_EQ(c.floor_kbps(), 2000);

    make_trend_overuse(c, t0);
    c.tick(t0 + 7000ms);

    auto t = t0 + 10000ms;
    for (int i = 0; i < 15; ++i) {
      t += 2000ms;
      c.on_loss(2, t);
      c.tick(t);
    }
    // 應降至 2000 kbps 且不得低於 2000
    EXPECT_EQ(c.applied_kbps(), 2000);
  }

  // 案例 B：設定 100 Mbps，舊規則下限為 30 Mbps；新規則技術下限仍為 2 Mbps (2000 Kbps)
  {
    fast::fast_controller_t c(kConfigured100Mbps);
    EXPECT_EQ(c.floor_kbps(), 2000);
  }

  // 案例 C：設定碼率低於 2000 kbps（如 1500 kbps）時，下限不可高於設定值（保留 B039-2）
  {
    fast::fast_controller_t c(1500);
    EXPECT_EQ(c.floor_kbps(), 1500);
  }
}

// 7. 防振盪：RAMP 中 5 秒內再次降碼 -> 上限暫壓至 applied × 0.85 維持 30 秒（整段保留）
TEST_F(VrAbrFastTest, AntiOscillationPenaltyThirtySeconds) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t_cut1 = t0 + 10000ms;
  c.on_loss(2, t_cut1);
  c.tick(t_cut1);  // target = 75000, 進入 hold

  // 解除 HOLD 進入 RAMP
  c.tick(t_cut1 + 2000ms);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // 回升到 79000
  c.set_applied_kbps(79000);

  // 於進入 RAMP 後 2 秒遭遇阻力再次降碼，applied 為 79000，防振盪上限 penalty_cap = 79000 * 0.85 = 67150
  const auto t_cut2 = t_cut1 + 3000ms;
  c.on_loss(2, t_cut2);
  auto cut2 = c.tick(t_cut2);
  ASSERT_TRUE(cut2.has_value());
  EXPECT_EQ(*cut2, 59250);  // 79000 * 0.75 = 59250

  // 解除 HOLD 再次進入 RAMP
  c.tick(t_cut2 + 2000ms);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // 期間持續回升，但必須受限於 penalty_cap (67150)，維持 30 秒
  auto t = t_cut2 + 2000ms;
  while (t < t_cut2 + 29000ms) {
    t += 1000ms;
    c.tick(t);
    EXPECT_LE(c.current_target_kbps(), 67150);
  }

  // 超過 30 秒後防振盪懲罰解除，上限恢復為 100000
  EXPECT_EQ(c.effective_cap_kbps(t_cut2 + 35000ms), 100000);
}

// 8. 訊號來源判定：LATCH 延遲中度降碼、送達率低中度降碼、delay-trend 快速降碼
TEST_F(VrAbrFastTest, ModerateAndCriticalSignalSources) {
  // LATCH 連續 2 筆 slack < -1000 us 觸發中度降碼（不改行為）
  {
    fast::fast_controller_t c(kConfigured100Mbps);
    c.on_latch_slack(-1200, t0);
    EXPECT_EQ(c.state(), fast::fast_state_e::healthy);
    c.on_latch_slack(-1500, t0 + 100ms);
    EXPECT_EQ(c.state(), fast::fast_state_e::cut);
    EXPECT_STREQ(c.last_reason(), "latch-slack-late");
    auto next = c.tick(t0 + 100ms);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 90000);
  }

  // 主連線送達率 < 90% 伴隨延遲過載時觸發中度降碼
  {
    fast::fast_controller_t c(kConfigured100Mbps);
    make_trend_overuse(c, t0);
    c.tick(t0 + 7000ms);
    c.set_applied_kbps(100000);

    const auto t = t0 + 10000ms;
    c.on_delivery(88.5, t);
    EXPECT_EQ(c.state(), fast::fast_state_e::cut);
    EXPECT_STREQ(c.last_reason(), "delivery-low+delay");
    auto next = c.tick(t);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 90000);
  }

  // RTT 膨脹由 delay_trend_t 判定：進入 overuse 時觸發 delay-trend 降碼（單次 ×0.85）
  {
    fast::fast_controller_t c(kConfigured100Mbps);
    // 5 秒 base 20 ms
    for (int i = 0; i < 50; ++i) {
      c.on_rtt(20, t0 + i * 100ms);
      EXPECT_EQ(c.state(), fast::fast_state_e::healthy);
    }
    // 急劇上升觸發 delay-trend
    double rtt = 20.0;
    bool cut_triggered = false;
    for (int i = 1; i <= 20; ++i) {
      rtt += 10.0;
      const auto t = t0 + 5000ms + i * 100ms;
      c.on_rtt(static_cast<int>(rtt), t);
      auto res = c.tick(t);
      if (res.has_value() && !cut_triggered) {
        cut_triggered = true;
        EXPECT_STREQ(c.last_reason(), "delay-trend");
        EXPECT_EQ(*res, 85000);  // 100000 * 0.85 = 85000
      }
    }
    EXPECT_TRUE(cut_triggered);
  }
}

// ==============================================================================
// 任務 §ABR-ADAPT 第 1 期第 1 段新增測試（情境 6 ~ 10）
// ==============================================================================

// 6. 控制器：行動網路抖動＋零星丟包不降碼
//    fast_controller_t(20000)，餵情境 1 的 RTT，並每 3 s on_fec_failed(1) 一次，跑 60 s，每 100 ms tick
//    -> 碼率不得低於 20000 × 0.9，random_losses() > 0。
TEST_F(VrAbrFastTest, ControllerMobileJitterAndSporadicLossDoesNotCut) {
  fast::fast_controller_t c(kConfigured20Mbps);
  std::mt19937 rng(1);
  std::uniform_real_distribution<double> dist(0.0, 25.0);

  for (int i = 0; i < 600; ++i) {
    const auto t = t0 + i * 100ms;
    const double rtt = 38.0 + dist(rng);
    c.on_rtt(static_cast<int>(rtt), t);

    if (i > 0 && i % 80 == 0) {  // 每 8 秒一次 FEC 救不回 (n=1)：20 秒視窗內最多 3 次，低於 §ABR-LOSSRATE 門檻
      c.on_fec_failed(1, t);
    }

    c.tick(t);
  }

  EXPECT_GE(c.applied_kbps(), static_cast<int>(kConfigured20Mbps * 0.9));
  EXPECT_GT(c.random_losses(), 0);
  EXPECT_EQ(c.cuts(), 0);
}

// 7. 控制器：持續大量丟包仍會降
//    同上但每 200 ms on_fec_failed(2) -> 3 s 內要降碼，原因含 +persistent。
TEST_F(VrAbrFastTest, ControllerPersistentHeavyLossTriggersCut) {
  fast::fast_controller_t c(kConfigured20Mbps);
  std::mt19937 rng(1);
  std::uniform_real_distribution<double> dist(0.0, 25.0);

  bool cut_triggered = false;
  std::string cut_reason = "";
  double cut_latency_sec = -1.0;

  for (int i = 0; i < 30; ++i) {  // 最長跑 3 秒
    const auto t = t0 + i * 100ms;
    const double rtt = 38.0 + dist(rng);
    c.on_rtt(static_cast<int>(rtt), t);

    if (i % 2 == 0) {  // 每 200 ms 丟包 2 張 (每秒累計 10 權重)
      c.on_fec_failed(2, t);
    }

    auto res = c.tick(t);
    if (res.has_value() && !cut_triggered) {
      cut_triggered = true;
      cut_reason = c.last_reason();
      cut_latency_sec = i * 0.1;
    }
  }

  EXPECT_TRUE(cut_triggered);
  EXPECT_LE(cut_latency_sec, 3.0);
  EXPECT_NE(cut_reason.find("+persistent"), std::string::npos);
}

// 8. 控制器：bufferbloat 降碼並回升
//    configured 50000，情境 3 的延遲上升 -> 降碼（原因 delay-trend）；
//    之後 RTT 回到 base -> 最終爬回 ≥ 45000，並驗證回升前段是乘法（單步增量隨 applied 變大）。
TEST_F(VrAbrFastTest, ControllerBufferbloatCutAndRampRecovery) {
  fast::fast_controller_t c(kConfigured50Mbps);

  // 10 秒 base 20 ms 平穩
  for (int i = 0; i < 100; ++i) {
    const auto t = t0 + i * 100ms;
    c.on_rtt(20, t);
    c.tick(t);
  }

  // 開始 bufferbloat 每 100 ms +5 ms
  const auto t_bloat = t0 + 10000ms;
  double rtt = 20.0;
  bool cut_triggered = false;
  for (int i = 1; i <= 20; ++i) {
    rtt += 5.0;
    const auto t = t_bloat + i * 100ms;
    c.on_rtt(static_cast<int>(rtt), t);
    auto res = c.tick(t);
    if (res.has_value() && !cut_triggered) {
      cut_triggered = true;
      EXPECT_STREQ(c.last_reason(), "delay-trend");
    }
  }
  EXPECT_TRUE(cut_triggered);

  // RTT 回復為 base (20 ms)，持續推進觀測回升步長
  const auto t_recover = t_bloat + 2000ms;
  std::vector<int> ramp_steps;
  int prev_applied = c.applied_kbps();

  for (int i = 1; i <= 600; ++i) {
    const auto t = t_recover + i * 100ms;
    c.on_rtt(20, t);
    auto res = c.tick(t);
    if (res.has_value() && std::string(c.last_reason()) == "ramp-clean") {
      ramp_steps.push_back(*res - prev_applied);
      prev_applied = *res;
    }
  }

  // 最終應成功爬回 >= 45000
  EXPECT_GE(c.applied_kbps(), 45000);

  // 驗證回升乘法階段：在 applied < 0.8 * last_congested 時，步長隨 applied 變大
  // 尋找連續兩步乘法階段步長
  bool mult_step_growth_found = false;
  for (size_t i = 1; i < ramp_steps.size(); ++i) {
    if (ramp_steps[i] > ramp_steps[i - 1]) {
      mult_step_growth_found = true;
      break;
    }
  }
  EXPECT_TRUE(mult_step_growth_found);
}

// 9. 下限：連續嚴重丟包可降到 2000 但不低於 2000
TEST_F(VrAbrFastTest, LowerBoundTechFloorProtectionAt2000) {
  fast::fast_controller_t c(kConfigured20Mbps);
  EXPECT_EQ(c.floor_kbps(), 2000);

  // 營造持續延遲過載環境使丟包皆觸發嚴重降碼
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);

  auto t = t0 + 10000ms;
  for (int i = 0; i < 20; ++i) {
    t += 2000ms;
    c.on_loss(5, t);
    c.tick(t);
  }

  EXPECT_EQ(c.applied_kbps(), 2000);
  EXPECT_EQ(c.floor_kbps(), 2000);
}

// 10. HOLD 長度隨 RTT：base 5 ms 時 HOLD ≈ 600 ms；base 200 ms 時 HOLD = 1600 ms
TEST_F(VrAbrFastTest, HoldDurationAdaptsToBaseRtt) {
  // 案例 A：base 5 ms -> 8 * 5 = 40 ms，clamp 至下限 600 ms
  {
    fast::fast_controller_t c(kConfigured20Mbps);
    for (int i = 0; i < 50; ++i) {
      c.on_rtt(5, t0 + i * 100ms);
    }
    const auto t_cut = t0 + 5000ms;
    c.on_loss(12, t_cut);  // 12 筆 persistent 觸發降碼
    c.tick(t_cut);
    EXPECT_EQ(c.state(), fast::fast_state_e::hold);

    // 590 ms 仍處於 HOLD
    EXPECT_FALSE(c.tick(t_cut + 590ms).has_value());
    EXPECT_EQ(c.state(), fast::fast_state_e::hold);

    // 610 ms 已解除 HOLD 轉入 RAMP
    c.tick(t_cut + 610ms);
    EXPECT_EQ(c.state(), fast::fast_state_e::ramp);
  }

  // 案例 B：base 200 ms -> 8 * 200 = 1600 ms
  {
    fast::fast_controller_t c(kConfigured20Mbps);
    for (int i = 0; i < 50; ++i) {
      c.on_rtt(200, t0 + i * 100ms);
    }
    const auto t_cut = t0 + 5000ms;
    c.on_loss(12, t_cut);
    c.tick(t_cut);
    EXPECT_EQ(c.state(), fast::fast_state_e::hold);

    // 1590 ms 仍處於 HOLD
    EXPECT_FALSE(c.tick(t_cut + 1590ms).has_value());
    EXPECT_EQ(c.state(), fast::fast_state_e::hold);

    // 1610 ms 已解除 HOLD 轉入 RAMP
    c.tick(t_cut + 1610ms);
    EXPECT_EQ(c.state(), fast::fast_state_e::ramp);
  }
}

// ==============================================================================
// 任務 §ABR-ADAPT 第 2、3 期新增測試（情境 1 ~ 7）
// ==============================================================================

// 1. OWD 可用時，RTT 劇烈膨脹但 OWD 平穩 -> 不降碼；OWD 膨脹（每 100 ms +5 ms）-> 1 s 內降碼，原因 owd-trend
TEST_F(VrAbrFastTest, OwdActiveSuppressesRttAndTriggersOwdTrendCut) {
  fast::fast_controller_t c(kConfigured50Mbps);

  // 先建立 5 秒平穩基準（RTT = 20 ms，OWD = -300.0 ms 含固定時鐘偏移）
  for (int i = 0; i < 50; ++i) {
    const auto t = t0 + i * 100ms;
    c.on_rtt(20, t);
    c.on_owd(-300.0, t);
    c.tick(t);
  }
  EXPECT_TRUE(c.owd_active(t0 + 4900ms));

  // RTT 劇烈膨脹（每 100 ms +10 ms），但 OWD 維持平穩 -> RTT 雖進 overuse，但因 OWD 可用而不觸發降碼
  const auto t_rtt_spike = t0 + 5000ms;
  int rtt = 20;
  for (int i = 1; i <= 20; ++i) {
    rtt += 10;
    const auto t = t_rtt_spike + i * 100ms;
    c.on_owd(-300.0, t);
    c.on_rtt(rtt, t);
    EXPECT_FALSE(c.tick(t).has_value());
  }
  EXPECT_EQ(c.trend().state(), fast::trend_state_e::overuse);
  EXPECT_EQ(c.owd_trend().state(), fast::trend_state_e::normal);
  EXPECT_FALSE(c.congested(t_rtt_spike + 2000ms));
  EXPECT_EQ(c.cuts(), 0);
  EXPECT_EQ(c.applied_kbps(), kConfigured50Mbps);

  // OWD 開始膨脹（每 100 ms +5 ms）-> 1 秒內觸發降碼，原因 owd-trend
  const auto t_owd_rise = t_rtt_spike + 2000ms;
  double owd = -300.0;
  bool cut_triggered = false;
  double cut_latency_sec = -1.0;
  for (int i = 1; i <= 20; ++i) {
    owd += 5.0;
    const auto t = t_owd_rise + i * 100ms;
    c.on_owd(owd, t);
    auto res = c.tick(t);
    if (res.has_value() && !cut_triggered) {
      cut_triggered = true;
      cut_latency_sec = i * 0.1;
      EXPECT_STREQ(c.last_reason(), "owd-trend");
      EXPECT_EQ(*res, 42500);  // 50000 * 0.85 = 42500
      EXPECT_TRUE(c.congested(t));
    }
  }
  EXPECT_TRUE(cut_triggered);
  EXPECT_LE(cut_latency_sec, 1.0);
}

// 2. OWD 停 3 s 後 -> 回到看 RTT（RTT 膨脹會觸發 delay-trend）
TEST_F(VrAbrFastTest, OwdInactiveAfterThreeSecondsFallsBackToRtt) {
  fast::fast_controller_t c(kConfigured50Mbps);

  // 前 5 秒同時餵 OWD 與 RTT
  for (int i = 0; i < 50; ++i) {
    const auto t = t0 + i * 100ms;
    c.on_rtt(20, t);
    c.on_owd(15.0, t);
  }
  const auto t_last_owd = t0 + 4900ms;
  EXPECT_TRUE(c.owd_active(t_last_owd));

  // OWD 停餵 3 秒，期間 RTT 維持平穩 20 ms
  for (int i = 1; i <= 30; ++i) {
    const auto t = t_last_owd + i * 100ms;
    c.on_rtt(20, t);
    c.tick(t);
  }
  const auto t_after_3s = t_last_owd + 3000ms;
  EXPECT_FALSE(c.owd_active(t_after_3s));

  // 此時 RTT 膨脹（每 100 ms +5 ms）應能正常觸發 delay-trend 降碼
  double rtt = 20.0;
  bool cut_triggered = false;
  for (int i = 1; i <= 20; ++i) {
    rtt += 5.0;
    const auto t = t_after_3s + i * 100ms;
    c.on_rtt(static_cast<int>(rtt), t);
    auto res = c.tick(t);
    if (res.has_value() && !cut_triggered) {
      cut_triggered = true;
      EXPECT_STREQ(c.last_reason(), "delay-trend");
      EXPECT_EQ(*res, 42500);
    }
  }
  EXPECT_TRUE(cut_triggered);
}

// 3. 鏈路飽和：applied 50000、rates recv=20000 send=40000 -> delay 類降碼一步落到 ≈ 50000×0.5=25000（factor 夾在 0.5）；
//    recv=36000 send=40000 -> factor=0.855 -> 夾 0.85
TEST_F(VrAbrFastTest, SaturatedRatesCutDirectlyToLinkCapacity) {
  // 案例 A：recv=20000, send=40000 -> 0.95 * 20000 / 40000 = 0.475 -> clamp 至 0.5 -> 50000 * 0.5 = 25000
  {
    fast::fast_controller_t c(kConfigured50Mbps);
    make_trend_overuse(c, t0);
    c.tick(t0 + 7000ms);
    c.set_applied_kbps(50000);

    const auto t_cut = t0 + 8000ms;
    c.on_rates(20000, 40000, t_cut);
    EXPECT_EQ(c.recv_kbps(), 20000);
    EXPECT_EQ(c.send_kbps(), 40000);

    c.on_loss(2, t_cut);
    auto res = c.tick(t_cut);
    ASSERT_TRUE(res.has_value());
    EXPECT_STREQ(c.last_reason(), "loss-ge-2+delay");
    EXPECT_EQ(*res, 25000);
  }

  // 案例 B：recv=36000, send=40000 (send >= recv * 1.05 = 37800) -> 0.95 * 36000 / 40000 = 0.855 -> clamp 至 0.85
  // 若無 saturated rates，loss-ge-2+delay 原本為 0.75 (37500)；有飽和 rates 則夾在 0.85 (50000 * 0.85 = 42500)
  {
    fast::fast_controller_t c(kConfigured50Mbps);
    make_trend_overuse(c, t0);
    c.tick(t0 + 7000ms);
    c.set_applied_kbps(50000);

    const auto t_cut = t0 + 8000ms;
    c.on_rates(36000, 40000, t_cut);
    c.on_loss(2, t_cut);
    auto res = c.tick(t_cut);
    ASSERT_TRUE(res.has_value());
    EXPECT_STREQ(c.last_reason(), "loss-ge-2+delay");
    EXPECT_EQ(*res, 42500);
  }
}

// 4. 畫質飽和：QP 一直 20 -> RAMP 不回升；QP 改 30 -> 恢復回升
TEST_F(VrAbrFastTest, QualitySaturationBlocksRampUntilQpRises) {
  fast::fast_controller_t c(kConfigured50Mbps);
  for (int i = 0; i < 50; ++i) {
    c.on_rtt(20, t0 + i * 100ms);
  }

  // 先降碼並解除 HOLD 進入 RAMP（applied = 37500）
  const auto t_cut = t0 + 5000ms;
  c.on_loss(12, t_cut);
  c.tick(t_cut);
  EXPECT_EQ(c.applied_kbps(), 37500);

  const auto t_ramp = t_cut + 1000ms;
  c.on_quality(2000, 1, t_ramp);  // HEVC, QP = 20.00 <= 22.00
  c.tick(t_ramp);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);
  EXPECT_TRUE(c.quality_saturated(t_ramp));
  EXPECT_STREQ(c.ramp_block_reason(t_ramp), "quality");

  // 持續 5 秒每 500 ms 餵 QP = 20 -> RAMP 停住不回升
  for (int i = 1; i <= 50; ++i) {
    const auto t = t_ramp + i * 100ms;
    if (i % 5 == 0) {
      c.on_quality(2000, 1, t);
    }
    EXPECT_FALSE(c.tick(t).has_value());
  }
  EXPECT_EQ(c.applied_kbps(), 37500);
  EXPECT_EQ(c.ramps(), 0);

  // QP 改為 30（3000）-> 解除畫質飽和並恢復回升
  const auto t_unblock = t_ramp + 5000ms;
  c.on_quality(3000, 1, t_unblock);
  EXPECT_FALSE(c.quality_saturated(t_unblock));
  EXPECT_STREQ(c.ramp_block_reason(t_unblock), "none");

  for (int i = 1; i <= 20; ++i) {
    const auto t = t_unblock + i * 100ms;
    if (i % 5 == 0) {
      c.on_quality(3000, 1, t);
    }
    c.tick(t);
  }
  EXPECT_GT(c.applied_kbps(), 37500);
  EXPECT_GT(c.ramps(), 0);
}

// 5. quality-trim（沒有塞過車＝有餘裕，回收門檻是 QP 6）：QP 一直 6、HEALTHY -> 10 s 後開始每 3 s −5%，cuts() 不變、trims() 增加、狀態不進 HOLD；
//    QP 回到 25 -> 停止 trim 並開始回升
TEST_F(VrAbrFastTest, QualityTrimReducesBitrateWithoutHoldAndRecoversWhenQpRises) {
  fast::fast_controller_t c(kConfigured50Mbps);
  EXPECT_EQ(c.state(), fast::fast_state_e::healthy);

  // t = 0 起餵 QP = 6 (600)，前 9.9 秒尚未滿 10 秒門檻，不觸發 trim
  c.on_quality(600, 1, t0);
  for (int i = 1; i < 100; ++i) {
    const auto t = t0 + i * 100ms;
    if (i % 5 == 0) {
      c.on_quality(600, 1, t);
    }
    EXPECT_FALSE(c.tick(t).has_value());
  }
  EXPECT_EQ(c.trims(), 0);
  EXPECT_EQ(c.cuts(), 0);
  EXPECT_EQ(c.applied_kbps(), 50000);

  // 滿 10 秒（t0 + 10000ms）：第一次 quality-trim（-5% -> 47500），狀態維持 HEALTHY（不進 HOLD）
  const auto t_10s = t0 + 10000ms;
  c.on_quality(600, 1, t_10s);
  auto trim1 = c.tick(t_10s);
  ASSERT_TRUE(trim1.has_value());
  EXPECT_EQ(*trim1, 47500);
  EXPECT_STREQ(c.last_reason(), "quality-trim");
  EXPECT_EQ(c.trims(), 1);
  EXPECT_EQ(c.cuts(), 0);
  EXPECT_EQ(c.state(), fast::fast_state_e::healthy);

  // 10.1s ~ 12.9s 之間未滿 3 秒間隔，不再次 trim，且因畫質飽和不進入 RAMP
  for (int i = 101; i < 130; ++i) {
    const auto t = t0 + i * 100ms;
    if (i % 5 == 0) {
      c.on_quality(600, 1, t);
    }
    EXPECT_FALSE(c.tick(t).has_value());
    EXPECT_NE(c.state(), fast::fast_state_e::hold);
  }
  EXPECT_EQ(c.trims(), 1);

  // 滿 13 秒（距上次 trim 滿 3 秒）：第二次 quality-trim（round(47500 * 0.95) = 45125）
  const auto t_13s = t0 + 13000ms;
  c.on_quality(600, 1, t_13s);
  auto trim2 = c.tick(t_13s);
  ASSERT_TRUE(trim2.has_value());
  EXPECT_EQ(*trim2, 45125);
  EXPECT_EQ(c.trims(), 2);
  EXPECT_EQ(c.cuts(), 0);
  EXPECT_EQ(c.state(), fast::fast_state_e::healthy);

  // QP 回到 25 (2500) -> 停止 trim 並轉入 RAMP 開始回升
  const auto t_recover = t0 + 14000ms;
  c.on_quality(2500, 1, t_recover);
  for (int i = 0; i <= 30; ++i) {
    const auto t = t_recover + i * 100ms;
    if (i > 0 && i % 5 == 0) {
      c.on_quality(2500, 1, t);
    }
    c.tick(t);
  }
  EXPECT_EQ(c.trims(), 2);
  EXPECT_EQ(c.cuts(), 0);
  EXPECT_GT(c.applied_kbps(), 45125);
  EXPECT_GT(c.ramps(), 0);
}

// 5b. §VR-ABR-TRIM-PROP：QP 比門檻低越多，單步收得越多。這個測試沒有塞過車＝§VR-QP-ROOM 的「有餘裕」，門檻是 6：
//     QP 6 -> ×0.95，QP 4 -> ×0.95×2^(-2/12)，QP 1 以下 -> 封頂 −25%
TEST_F(VrAbrFastTest, QualityTrimStepGrowsWithQpGap) {
  auto first_trim = [&](int qp_x100) {
    fast::fast_controller_t c(kConfigured50Mbps);
    for (int i = 0; i < 100; ++i) {
      const auto t = t0 + i * 100ms;
      if (i % 5 == 0) {
        c.on_quality(qp_x100, 1, t);
      }
      EXPECT_FALSE(c.tick(t).has_value());
    }
    c.on_quality(qp_x100, 1, t0 + 10000ms);
    auto r = c.tick(t0 + 10000ms);
    EXPECT_TRUE(r.has_value());
    return r.value_or(0);
  };
  EXPECT_EQ(first_trim(600), 47500);
  EXPECT_EQ(first_trim(400), 42318);
  EXPECT_EQ(first_trim(100), 37500);
  EXPECT_EQ(first_trim(0), 37500);
}

// 5c. §VR-ABR-TRIM-PROP 回程：quality-trim 收下來的碼率，QP 變差時幾步內補回收之前的水準（不超過）
TEST_F(VrAbrFastTest, QualityRestoreReturnsToPreTrimRateQuickly) {
  fast::fast_controller_t c(kConfigured50Mbps);
  auto t = t0;
  // 載入畫面：QP 1，30 秒（有餘裕時的回收門檻是 6，QP 1 每步收到封頂的 −25%）
  for (int i = 0; i <= 300; ++i) {
    t = t0 + i * 100ms;
    if (i % 5 == 0) {
      c.on_quality(100, 1, t);
    }
    c.tick(t);
  }
  const int low = c.applied_kbps();
  EXPECT_LT(low, 15000);
  EXPECT_EQ(c.cuts(), 0);
  // 進場景：QP 27，10 秒內要回到 3 萬以上、且不超過收之前的 5 萬
  const auto t1 = t;
  for (int i = 1; i <= 100; ++i) {
    t = t1 + i * 100ms;
    if (i % 5 == 0) {
      c.on_quality(c.applied_kbps() < 30000 ? 2700 : 1800, 1, t);
    }
    c.tick(t);
  }
  EXPECT_GE(c.applied_kbps(), 30000);
  EXPECT_LE(c.applied_kbps(), 50000);
  EXPECT_EQ(c.cuts(), 0);
}

// 6. 解碼：p95 = 0.9×period -> 不回升；連 2 筆 1.1×period -> decode-overload 降 10%；之後 4 s 沒樣本 -> 解除
TEST_F(VrAbrFastTest, DecodeTightBlocksRampAndOverloadCutsByTenPercent) {
  fast::fast_controller_t c(kConfigured50Mbps);
  for (int i = 0; i < 50; ++i) {
    c.on_rtt(20, t0 + i * 100ms);
  }

  // 設定 applied = 40000 並推進 tick 使其進入 RAMP
  const auto t_start = t0 + 5000ms;
  c.set_applied_kbps(40000);
  c.tick(t_start);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // p95 = 0.9 * period (9000 us / 10000 us) -> 解碼吃緊，RAMP 不回升
  c.on_decode(9000, 10000, t_start);
  EXPECT_STREQ(c.ramp_block_reason(t_start), "decode");
  for (int i = 1; i <= 15; ++i) {
    const auto t = t_start + i * 100ms;
    if (i == 10) {
      c.on_decode(9000, 10000, t);
    }
    EXPECT_FALSE(c.tick(t).has_value());
  }
  EXPECT_EQ(c.applied_kbps(), 40000);
  EXPECT_EQ(c.ramps(), 0);

  // 連續 2 筆 p95 = 1.1 * period (11000 us / 10000 us) -> 觸發 decode-overload 降碼 10%
  const auto t_ov1 = t_start + 2000ms;
  c.on_decode(11000, 10000, t_ov1);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);  // 第 1 筆尚未降碼

  const auto t_ov2 = t_start + 3000ms;
  c.on_decode(11000, 10000, t_ov2);
  EXPECT_EQ(c.state(), fast::fast_state_e::cut);
  EXPECT_STREQ(c.last_reason(), "decode-overload");

  auto cut_res = c.tick(t_ov2);
  ASSERT_TRUE(cut_res.has_value());
  EXPECT_EQ(*cut_res, 36000);  // 40000 * 0.90 = 36000

  // 之後 4 秒沒有新 decode 樣本 -> 3 秒有效期限屆滿後解除阻擋並恢復回升
  for (int i = 1; i <= 40; ++i) {
    const auto t = t_ov2 + i * 100ms;
    c.on_rtt(20, t);
    c.tick(t);
  }
  const auto t_after_4s = t_ov2 + 4000ms;
  EXPECT_STREQ(c.ramp_block_reason(t_after_4s), "none");
  EXPECT_GT(c.applied_kbps(), 36000);
  EXPECT_GT(c.ramps(), 0);
}

// 7. ramp_block_reason 依優先序回傳正確（"delay" > "decode" > "quality" > "none"）
TEST_F(VrAbrFastTest, RampBlockReasonFollowsPriorityOrder) {
  fast::fast_controller_t c(kConfigured50Mbps);

  // 初始無任何阻擋 -> "none"
  EXPECT_STREQ(c.ramp_block_reason(t0), "none");

  // 1. 僅畫質飽和（沒有塞過車＝有餘裕，門檻是 QP 10）-> "quality"
  c.on_quality(1000, 1, t0);
  EXPECT_STREQ(c.ramp_block_reason(t0), "quality");

  // 2. 同時解碼吃緊 (p95 = 9000 > 8500) 與畫質飽和 -> 優先回傳 "decode"
  c.on_decode(9000, 10000, t0);
  EXPECT_STREQ(c.ramp_block_reason(t0), "decode");

  // 3. 同時延遲過載、解碼吃緊、畫質飽和 -> 優先回傳 "delay"
  make_trend_overuse(c, t0);
  const auto t_over = t0 + 7000ms;
  c.on_quality(2000, 1, t_over);
  c.on_decode(9000, 10000, t_over);
  EXPECT_STREQ(c.ramp_block_reason(t_over), "delay");

  // 4. 當 OWD 可用且平穩時，delay 改看 OWD（normal），應退回次高優先序 "decode"
  for (int i = 0; i < 10; ++i) {
    c.on_owd(10.0, t_over + i * 50ms);
  }
  const auto t_owd_clean = t_over + 450ms;
  c.on_quality(2000, 1, t_owd_clean);
  c.on_decode(9000, 10000, t_owd_clean);
  EXPECT_STREQ(c.ramp_block_reason(t_owd_clean), "decode");

  // 5. 解碼恢復正常 (p95 = 5000 <= 8500)，應退回 "quality"
  c.on_decode(5000, 10000, t_owd_clean);
  EXPECT_STREQ(c.ramp_block_reason(t_owd_clean), "quality");

  // 6. 畫質不再飽和 (QP = 28 > 22)，應退回 "none"
  c.on_quality(2800, 1, t_owd_clean);
  EXPECT_STREQ(c.ramp_block_reason(t_owd_clean), "none");
}


// §ABR-LOSSRATE（11-04 桌球實測）：延遲平穩但每 3 秒一張救不回 -> 視為跟碼率有關，降 10% 並暫壓上限；不低於 10 Mbps
TEST_F(VrAbrFastTest, SustainedRandomLossCutsByLossRateButNotBelowTenMbps) {
  fast::fast_controller_t c(kConfigured20Mbps);
  std::vector<int> cuts;
  for (int i = 0; i < 3000; ++i) {  // 300 秒
    const auto t = t0 + i * 100ms;
    c.on_rtt(30, t);
    if (i > 0 && i % 30 == 0) {
      c.on_fec_failed(1, t);
    }
    auto r = c.tick(t);
    if (r.has_value() && std::string(c.last_reason()) == "loss-rate") {
      cuts.push_back(*r);
    }
  }
  ASSERT_FALSE(cuts.empty());
  EXPECT_EQ(cuts.front(), 18000);  // 20000 × 0.90
  EXPECT_GE(c.applied_kbps(), 10000);
  EXPECT_GE(c.loss_rate_cuts(), 1);
}

// §ABR-LOSSRATE-VERIFY（11-07 遠端桌球）：丟包頻率跟碼率無關（降碼前後都是每 3 秒一張）-> 30 秒後撤銷、回到原碼率、
// 上限解除，之後 10 分鐘不再因此降
TEST_F(VrAbrFastTest, LossRateCutRevertedWhenLossDoesNotDrop) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(75000);  // 單一連線上限（同 stream.cpp）
  c.set_applied_kbps(75000);
  int firstCutAt = -1, revertAt = -1;
  for (int i = 0; i < 6000; ++i) {  // 600 秒
    const auto t = t0 + i * 100ms;
    c.on_rtt(30, t);
    if (i > 0 && i % 30 == 0) {
      c.on_fec_failed(1, t);
    }
    auto r = c.tick(t);
    if (r.has_value()) {
      c.set_applied_kbps(*r);
      if (firstCutAt < 0 && std::string(c.last_reason()) == "loss-rate") {
        firstCutAt = i;
        EXPECT_EQ(*r, 67500);
      }
      if (revertAt < 0 && std::string(c.last_reason()) == "loss-rate-revert") {
        revertAt = i;
        EXPECT_EQ(*r, 75000);
      }
    }
  }
  ASSERT_GE(firstCutAt, 0);
  ASSERT_GE(revertAt, 0);
  EXPECT_EQ(revertAt - firstCutAt, 300);  // 30 秒試驗
  EXPECT_EQ(c.loss_rate_cuts(), 1);       // 停用 10 分鐘：整場只降這一次
  EXPECT_EQ(c.loss_rate_reverts(), 1);
  EXPECT_EQ(c.applied_kbps(), 75000);
  EXPECT_EQ(c.effective_cap_kbps(t0 + 600s), 75000);  // 丟包率上限已解除，只剩連線上限
}

// §ABR-LOSSRATE-VERIFY：降碼後丟包真的變少（每 3 秒一張 -> 每 15 秒一張）-> 保留這一刀
TEST_F(VrAbrFastTest, LossRateCutKeptWhenLossDrops) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(75000);  // 單一連線上限（同 stream.cpp）
  c.set_applied_kbps(75000);
  int cutAt = -1, maxDuringCap = 0;
  for (int i = 0; i < 1200; ++i) {  // 120 秒
    const auto t = t0 + i * 100ms;
    c.on_rtt(30, t);
    const int every = (cutAt < 0) ? 30 : 150;
    if (i > 0 && i % every == 0) {
      c.on_fec_failed(1, t);
    }
    auto r = c.tick(t);
    if (r.has_value()) {
      c.set_applied_kbps(*r);
      if (cutAt < 0 && std::string(c.last_reason()) == "loss-rate") {
        cutAt = i;
      }
    }
    if (cutAt >= 0 && i > cutAt && i < cutAt + 590) {
      maxDuringCap = std::max(maxDuringCap, c.applied_kbps());
    }
  }
  ASSERT_GE(cutAt, 0);
  EXPECT_EQ(c.loss_rate_reverts(), 0);
  EXPECT_EQ(maxDuringCap, 67500);  // 試驗判定有效：上限照留 60 秒，不會被拉回 75 Mbps
}

// §VR-ABR-OUTAGE：斷訊連刀觸發防振盪上限；拉回斷訊前碼率時要清掉，下一輪不可被壓回去（11-07：29.6 -> 5.9 Mbps）
TEST_F(VrAbrFastTest, OutageRestoreClearsAntiOscillationCap) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.set_applied_kbps(40000);
  for (auto t = t0 + 7000ms; t <= t0 + 20000ms; t += 100ms) {  // 40 Mbps 穩定撐住 13 秒
    c.set_applied_kbps(40000);
    c.tick(t);
  }
  ASSERT_EQ(c.stable_kbps(), 40000);
  const auto t1 = t0 + 20000ms;
  c.on_loss(2, t1);
  c.tick(t1);                       // 40000 -> 30000，HOLD
  c.tick(t1 + 2100ms);              // 進 RAMP
  c.set_applied_kbps(32000);
  c.on_loss(2, t1 + 3000ms);        // RAMP 5 秒內又降 -> 防振盪上限 32000 × 0.85
  c.tick(t1 + 3000ms);
  ASSERT_LT(c.effective_cap_kbps(t1 + 4000ms), 30000);

  EXPECT_EQ(c.on_outage_restored(30000, t1 + 8000ms), 30000);
  EXPECT_EQ(c.effective_cap_kbps(t1 + 8000ms), 100000);
  auto r = c.tick(t1 + 8100ms);
  EXPECT_FALSE(r.has_value());      // 不會被壓回
  EXPECT_EQ(c.applied_kbps(), 30000);
}

// §ABR-RESTORE-STABLE（11-08）：斷訊前的值從沒撐住過（開場 75 Mbps 6 秒就塞）-> 不拉回；
// 撐住 10 秒的值才拉回、而且不超過它；拉回後 5 秒內又塞 -> 穩定值作廢
TEST_F(VrAbrFastTest, OutageRestoreOnlyUpToStableBitrate) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(75000);
  make_trend_overuse(c, t0);        // 開場就塞（約第 6 秒降碼）
  c.tick(t0 + 6500ms);
  c.set_applied_kbps(20000);
  EXPECT_EQ(c.on_outage_restored(75000, t0 + 8000ms), 0);
  EXPECT_EQ(c.stable_kbps(), 0);

  // 20 Mbps 撐住 12 秒 -> 穩定值 20000；拉回 75000 只給 20000
  for (auto t = t0 + 8000ms; t <= t0 + 20000ms; t += 100ms) {
    c.set_applied_kbps(20000);
    c.tick(t);
  }
  EXPECT_EQ(c.stable_kbps(), 20000);
  c.set_applied_kbps(8000);
  EXPECT_EQ(c.on_outage_restored(75000, t0 + 20500ms), 20000);

  // 拉回後 2 秒又塞 -> 穩定值作廢，下一次斷訊不再拉回
  c.on_loss(2, t0 + 22500ms);
  c.tick(t0 + 22500ms);
  EXPECT_EQ(c.stable_kbps(), 0);
  c.set_applied_kbps(10000);
  EXPECT_EQ(c.on_outage_restored(75000, t0 + 25000ms), 0);
}

// §VR-ABR-STARTUP-RESTORE（11-09）：開場一陣丟包（延遲沒漲）連降到底、還沒有穩定值 -> 拉回斷訊前的一半；
// 拉回後 5 秒內又降碼 -> 這一場不再拉回一半
TEST_F(VrAbrFastTest, StartupOutageRestoresHalfWithoutDelayCongestion) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(75000);
  c.set_applied_kbps(9375);
  ASSERT_EQ(c.stable_kbps(), 0);
  EXPECT_EQ(c.on_outage_restored(75000, t0 + 5000ms), 37500);
  EXPECT_EQ(c.applied_kbps(), 37500);

  c.on_loss(12, t0 + 7000ms);  // 拉回後 2 秒又降（一次 12 張＝persistent 門檻）
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(10000);
  EXPECT_EQ(c.on_outage_restored(75000, t0 + 12000ms), 0);
}

// §VR-ABR-SATURATED-NO-RESTORE（11-09 午）：鏈路飽和（送出 ≥ 收到 × 1.05）時的降碼＝塞車，6 秒內不做斷訊拉回
TEST_F(VrAbrFastTest, NoOutageRestoreRightAfterSaturatedCut) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(100000);
  c.set_applied_kbps(90000);
  c.on_rates(90000, 90000, t0);  // 開場起點
  const auto t1 = t0 + 20000ms;  // 開場 10 秒以後
  c.on_rates(60000, 90000, t1);  // 送 90、收 60：飽和
  c.on_loss(12, t1);             // persistent（飽和才允許）
  c.tick(t1);
  ASSERT_LT(c.applied_kbps(), 90000);
  c.set_applied_kbps(40000);
  EXPECT_EQ(c.on_outage_restored(90000, t1 + 3000ms), 0);
  EXPECT_EQ(c.on_outage_restored(90000, t1 + 8000ms), 0);  // 沒有穩定值、又塞過：也不拉回一半
}

// §VR-ABR-RESTORE-STEP：塞車事件過了 6 秒也不一步拉回，每次最多翻倍，直到達到目標
TEST_F(VrAbrFastTest, CongestionEpisodeRestoreDoublesUntilTarget) {
  congestion_episode_t f(t0);
  ASSERT_EQ(f.c.stable_kbps(), 75000);
  ASSERT_EQ(f.c.applied_kbps(), 10000);

  EXPECT_EQ(f.c.on_outage_restored(75000, f.t_sat + 3000ms, f.episode), 0);  // 6 秒內仍不拉

  const auto t_step = f.t_sat + 7000ms;
  EXPECT_EQ(f.c.on_outage_restored(75000, t_step, f.episode), 20000);
  EXPECT_STREQ(f.c.last_reason(), "outage-restore-step");
  EXPECT_TRUE(f.c.restore_stepped_this_call());  // stream 靠這個回報部分拉回，不靠回傳值比 want
  EXPECT_EQ(f.c.applied_kbps(), 20000);

  EXPECT_EQ(f.c.on_outage_restored(75000, t_step + 100ms, f.episode), 40000);
  EXPECT_STREQ(f.c.last_reason(), "outage-restore-step");
  EXPECT_TRUE(f.c.restore_stepped_this_call());

  EXPECT_EQ(f.c.on_outage_restored(75000, t_step + 200ms, f.episode), 75000);
  EXPECT_STREQ(f.c.last_reason(), "outage-restore");  // 這一階已經到目標，不再是分步
  EXPECT_FALSE(f.c.restore_stepped_this_call());
}

// 分步後 5 秒內外部把碼率降下來：同一個事件之後回 0；換一個 episode_start 才可以再拉
TEST_F(VrAbrFastTest, RestoreStepCutBlocksSameEpisodeUntilANewOne) {
  congestion_episode_t f(t0);
  ASSERT_EQ(f.c.stable_kbps(), 75000);
  const auto t_step = f.t_sat + 7000ms;
  ASSERT_EQ(f.c.on_outage_restored(75000, t_step, f.episode), 20000);

  f.c.set_applied_kbps(12000, t_step + 1s);  // 外部降碼，穩定值不會被清掉
  EXPECT_EQ(f.c.on_outage_restored(75000, t_step + 1s, f.episode), 0);
  EXPECT_TRUE(f.c.restore_step_blocked_this_call());
  EXPECT_TRUE(f.c.take_restore_step_block_log());
  EXPECT_EQ(f.c.on_outage_restored(75000, t_step + 1500ms, f.episode), 0);
  EXPECT_TRUE(f.c.restore_step_blocked_this_call());
  EXPECT_FALSE(f.c.take_restore_step_block_log());  // 同一個事件只記一次

  // 新事件的起點晚於那次飽和降碼：不是塞車事件，旗標已清，一步拉回穩定值
  const auto episode2 = f.t_sat + 1s;
  EXPECT_EQ(f.c.on_outage_restored(75000, t_step + 2s, episode2), 75000);
  EXPECT_STREQ(f.c.last_reason(), "outage-restore");
}

// 分步後 5 秒內控制器自己的降碼也封鎖同一個事件。穩定值會被既有的「拉回失敗」清掉，
// 重新撐住之後同一個事件仍回 0；換事件才拉得回去。
TEST_F(VrAbrFastTest, FastCutAfterRestoreStepBlocksSameEpisode) {
  congestion_episode_t f(t0);
  const auto t_step = f.t_sat + 7000ms;
  ASSERT_EQ(f.c.on_outage_restored(75000, t_step, f.episode), 20000);

  const auto t_cut = t_step + 500ms;
  f.c.on_loss(12, t_cut);
  auto cut = f.c.tick(t_cut);
  ASSERT_TRUE(cut.has_value());
  EXPECT_EQ(*cut, 15000);  // 20000 × 0.75，收送樣本已經超過 1 秒，不是飽和刀
  EXPECT_EQ(f.c.on_outage_restored(75000, t_cut + 100ms, f.episode), 0);

  const int held = f.c.applied_kbps();
  for (auto t = t_cut; t <= t_cut + 12s; t += 100ms) {
    f.c.set_applied_kbps(held);
    f.c.tick(t);
  }
  ASSERT_EQ(f.c.stable_kbps(), held);
  f.c.set_applied_kbps(held / 2);
  // 穩定值回來了、也過了飽和降碼的 6 秒，仍因同一個事件的旗標拒絕
  EXPECT_EQ(f.c.on_outage_restored(75000, t_cut + 13s, f.episode), 0);
  const auto episode2 = t_cut + 13s;
  EXPECT_EQ(f.c.on_outage_restored(75000, t_cut + 13s, episode2), held);
}

// 事件裡沒有飽和降碼：有 episode_start 也是一步拉回
TEST_F(VrAbrFastTest, OutageRestoreWithoutSaturatedCutIsOneShot) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(75000);
  c.set_applied_kbps(75000);
  for (auto t = t0; t <= t0 + 12s; t += 100ms) {
    c.set_applied_kbps(75000);
    c.tick(t);
  }
  ASSERT_EQ(c.stable_kbps(), 75000);
  c.set_applied_kbps(10000);
  EXPECT_EQ(c.on_outage_restored(75000, t0 + 13s, t0 + 13s), 75000);
  EXPECT_STREQ(c.last_reason(), "outage-restore");
}

// 飽和降碼在事件開始之前：這次事件裡沒有那一刀，一步拉回
TEST_F(VrAbrFastTest, SaturatedCutBeforeEpisodeStartRestoresInOneStep) {
  congestion_episode_t f(t0);
  ASSERT_EQ(f.c.stable_kbps(), 75000);
  const auto later = f.t_sat + 1s;
  EXPECT_EQ(f.c.on_outage_restored(75000, f.t_sat + 7000ms, later), 75000);
  EXPECT_STREQ(f.c.last_reason(), "outage-restore");
}

// 不傳 episode_start：過了 6 秒、有穩定值時仍一步拉回（既有呼叫的結果不變）
TEST_F(VrAbrFastTest, OutageRestoreWithoutEpisodeStartMatchesPreviousBehavior) {
  congestion_episode_t f(t0);
  ASSERT_EQ(f.c.stable_kbps(), 75000);
  ASSERT_EQ(f.c.applied_kbps(), 10000);
  EXPECT_EQ(f.c.on_outage_restored(75000, f.t_sat + 7000ms), 75000);
  EXPECT_STREQ(f.c.last_reason(), "outage-restore");
}

// 分步後先停 hold：每 500 ms 的 tick（乾淨視窗）在 1.5 秒內不得回升，之後第 2 階才是 applied×2。
// 最後一階到達目標，回到 healthy。
TEST_F(VrAbrFastTest, SteppedRestoreHoldBlocksRampUntilSecondStep) {
  congestion_episode_t f(t0);
  const auto t_step = f.t_sat + 7000ms;
  ASSERT_EQ(f.c.on_outage_restored(75000, t_step, f.episode), 20000);
  EXPECT_EQ(f.c.state(), fast::fast_state_e::hold);
  EXPECT_EQ(f.c.applied_kbps(), 20000);

  for (int i = 1; i <= 3; ++i) {  // +500、+1000、+1500 ms，涵蓋整個維持窗
    auto r = f.c.tick(t_step + i * 500ms);
    EXPECT_FALSE(r.has_value()) << "tick +" << (i * 500) << " ms";
    EXPECT_EQ(f.c.applied_kbps(), 20000);
  }

  EXPECT_EQ(f.c.on_outage_restored(75000, t_step + 1600ms, f.episode), 40000);
  EXPECT_TRUE(f.c.restore_stepped_this_call());
  EXPECT_EQ(f.c.state(), fast::fast_state_e::hold);

  EXPECT_EQ(f.c.on_outage_restored(75000, t_step + 1700ms, f.episode), 75000);
  EXPECT_FALSE(f.c.restore_stepped_this_call());
  EXPECT_STREQ(f.c.last_reason(), "outage-restore");
  EXPECT_EQ(f.c.state(), fast::fast_state_e::healthy);
}

// 分步後 5 秒內的 quality-trim 是省頻寬，不是降碼，不可以封鎖下一階。
TEST_F(VrAbrFastTest, QualityTrimAfterRestoreStepDoesNotBlockNextStep) {
  congestion_episode_t f(t0);
  const auto t_step = f.t_sat + 7000ms;
  ASSERT_EQ(f.c.on_outage_restored(75000, t_step, f.episode), 20000);

  const auto t_trim = t_step + 2s;  // hold 1.5 秒已過，tick 才會做到 quality-trim
  for (auto t = t_trim - 10s; t <= t_trim; t += 1s) {
    f.c.on_quality(600, 1, t);  // QP 6，間隔 1 秒小於 1.5 秒的新鮮窗，連續滿 10 秒
  }
  auto trim = f.c.tick(t_trim);
  ASSERT_TRUE(trim.has_value());
  EXPECT_EQ(*trim, 19000);  // round(20000 × 0.95)
  EXPECT_STREQ(f.c.last_reason(), "quality-trim");

  EXPECT_EQ(f.c.on_outage_restored(75000, t_trim, f.episode), 38000);
  EXPECT_TRUE(f.c.restore_stepped_this_call());
  EXPECT_FALSE(f.c.restore_step_blocked_this_call());
}

// 上限往下夾仍算降碼：分步後 5 秒內要把同一個事件的後續拉回停掉。
TEST_F(VrAbrFastTest, CapCutAfterRestoreStepStillBlocks) {
  congestion_episode_t f(t0);
  const auto t_step = f.t_sat + 7000ms;
  ASSERT_EQ(f.c.on_outage_restored(75000, t_step, f.episode), 20000);

  f.c.set_cap_kbps(15000);
  auto cut = f.c.tick(t_step + 100ms);
  ASSERT_TRUE(cut.has_value());
  EXPECT_EQ(*cut, 15000);

  EXPECT_EQ(f.c.on_outage_restored(75000, t_step + 200ms, f.episode), 0);
  EXPECT_TRUE(f.c.restore_step_blocked_this_call());
}

// 分步後又飽和降碼，6 秒冷卻還沒過也要標成封鎖，呼叫端才印得出那一行。
TEST_F(VrAbrFastTest, BlockedRestoreInsideSaturatedCooldownFlagsTheBlock) {
  congestion_episode_t f(t0);
  const auto t_step = f.t_sat + 7000ms;
  ASSERT_EQ(f.c.on_outage_restored(75000, t_step, f.episode), 20000);

  const auto t_cut = t_step + 500ms;
  f.c.on_rates(10000, 20000, t_cut);  // 新鮮的飽和收送，這一刀才會改寫 last_saturated_cut_
  f.c.on_loss(12, t_cut);
  auto cut = f.c.tick(t_cut);
  ASSERT_TRUE(cut.has_value());
  EXPECT_LT(*cut, 20000);

  const auto t_ask = t_cut + 2000ms;  // 仍在這次飽和降碼的 6 秒內，也在分步後的 5 秒內
  EXPECT_EQ(f.c.on_outage_restored(75000, t_ask, f.episode), 0);
  EXPECT_TRUE(f.c.restore_step_blocked_this_call());
}

// applied > INT_MAX/2 時，applied×2 用 32 位元會溢位成負數、拉回被拒絕。64 位元計算後應一步給到目標。
TEST_F(VrAbrFastTest, RestoreStepMultiplyStaysPositiveAboveIntMaxHalf) {
  constexpr int kBig = 2000000000;
  constexpr int kApplied = 1500000000;  // > INT_MAX/2，×2 超出 int
  static_assert(kApplied > 2147483647 / 2, "must overflow a 32-bit multiply");
  fast::fast_controller_t c(kBig);
  c.on_rates(kBig, kBig, t0);
  for (auto t = t0; t <= t0 + 12s; t += 100ms) {
    c.set_applied_kbps(kBig);
    c.tick(t);
  }
  ASSERT_EQ(c.stable_kbps(), kBig);

  const auto t_cut = t0 + 20s;
  const auto episode = t_cut - 1s;
  c.on_rates(kBig / 2, kBig, t_cut);
  c.on_loss(12, t_cut);
  auto cut = c.tick(t_cut);
  ASSERT_TRUE(cut.has_value());
  c.set_applied_kbps(kApplied);

  EXPECT_EQ(c.on_outage_restored(kBig, t_cut + 7s, episode), kBig);
  EXPECT_FALSE(c.restore_stepped_this_call());
  EXPECT_STREQ(c.last_reason(), "outage-restore");
}

// §ABR-LOSSRATE：零星（每 15 秒一張）不觸發
TEST_F(VrAbrFastTest, SparseRandomLossDoesNotTriggerLossRate) {
  fast::fast_controller_t c(kConfigured100Mbps);
  for (int i = 0; i < 1200; ++i) {
    const auto t = t0 + i * 100ms;
    c.on_rtt(30, t);
    if (i > 0 && i % 150 == 0) {
      c.on_fec_failed(1, t);
    }
    c.tick(t);
  }
  EXPECT_EQ(c.loss_rate_cuts(), 0);
  EXPECT_EQ(c.cuts(), 0);
}

// 11-04：同一次塞車連降兩刀，塞車點要記第一刀前的值；之後回升接近塞車點要慢（每秒 ≤ 0.5%）
TEST_F(VrAbrFastTest, CongestionEpisodeKeepsFirstCutPointAndRampsSlowlyNearIt) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(75000);
  c.tick(t0);
  ASSERT_EQ(c.applied_kbps(), 75000);
  make_trend_overuse(c, t0);  // 第一刀 delay-trend
  c.tick(t0 + 7000ms);
  const int after_first = c.applied_kbps();
  ASSERT_LT(after_first, 75000);
  EXPECT_EQ(c.last_congested_kbps(), 75000);
  // 1.6 秒後同一次塞車的第二刀（丟包＋延遲）
  c.on_fec_failed(1, t0 + 7600ms);
  c.tick(t0 + 7600ms);
  EXPECT_EQ(c.last_congested_kbps(), 75000);

  // 延遲回到平穩後回升：從 0.8×75000＝60000 以上起算，每秒最多 +0.5%
  int prev = c.applied_kbps();
  auto prev_t = t0 + 7600ms;
  for (int i = 1; i <= 1200; ++i) {
    const auto t = t0 + 7600ms + i * 100ms;
    c.on_rtt(25, t);
    auto r = c.tick(t);
    if (r.has_value() && std::string(c.last_reason()) == "ramp-clean" && prev >= 60000) {
      const double dt = std::chrono::duration<double>(t - prev_t).count();
      EXPECT_LE(*r - prev, static_cast<int>(prev * 0.005) + 1);
      EXPECT_GE(dt, 0.99);
    }
    if (r.has_value()) {
      prev = *r;
      prev_t = t;
    }
  }
}

// §ABR-RATCHET：30 秒沒再塞車後，塞車點每秒往上放寬 0.1%，最多 +50%
TEST_F(VrAbrFastTest, CongestionPointRelaxesWhenQuiet) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_cap_kbps(75000);
  c.tick(t0);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  ASSERT_EQ(c.last_congested_kbps(), 75000);
  const int lc0 = c.effective_congested_kbps();
  for (int i = 1; i <= 600; ++i) {  // 60 秒平穩
    const auto t = t0 + 7000ms + i * 100ms;
    c.on_rtt(25, t);
    c.tick(t);
  }
  const int lc60 = c.effective_congested_kbps();
  EXPECT_GE(lc60, static_cast<int>(lc0 * 1.02));
  EXPECT_LE(lc60, static_cast<int>(lc0 * 1.04));
  for (int i = 601; i <= 6500; ++i) {
    const auto t = t0 + 7000ms + i * 100ms;
    c.on_rtt(25, t);
    c.tick(t);
  }
  EXPECT_EQ(c.effective_congested_kbps(), static_cast<int>(75000 * 1.5));
  EXPECT_EQ(c.applied_kbps(), 75000);  // 平穩夠久要回到上限
}

// §ABR-SHORTFALL：沒有延遲趨勢、沒有丟包，但收到的比送出少三成，連續兩次回報就直接降到約收得到的量
TEST_F(VrAbrFastTest, RateShortfallCutsWithoutWaitingForTrend) {
  fast::fast_controller_t c(75000);
  for (int i = 0; i < 50; ++i) {
    c.on_rtt(5, t0 + i * 100ms);
  }
  c.set_applied_kbps(75000);
  const auto t1 = t0 + 6000ms;
  c.on_rates(58600, 84800, t1);
  EXPECT_FALSE(c.tick(t1).has_value());  // 只有一次：還不降
  const auto t2 = t1 + 500ms;
  c.on_rates(58600, 84800, t2);
  auto res = c.tick(t2);
  ASSERT_TRUE(res.has_value());
  EXPECT_STREQ(c.last_reason(), "rate-shortfall+delay");
  // 0.95 × 58600 / 84800 = 0.656 → 75000 × 0.656 ≈ 49200
  EXPECT_NEAR(*res, 49200, 600);
}

// §ABR-SHORTFALL：無線零星掉包造成的小落差（送 80、收 74，約 8%）不降
TEST_F(VrAbrFastTest, SmallRateGapDoesNotCut) {
  fast::fast_controller_t c(75000);
  for (int i = 0; i < 50; ++i) {
    c.on_rtt(5, t0 + i * 100ms);
  }
  c.set_applied_kbps(75000);
  for (int i = 0; i < 6; ++i) {
    const auto t = t0 + 6000ms + i * 500ms;
    c.on_rates(74000, 80000, t);
    EXPECT_FALSE(c.tick(t).has_value());
  }
}

// §ABR-OWD-STALL（11-08 在家）：Wi-Fi 停頓（一段時間收不到、之後一次全到）不是排隊——判過載時最新幾張已回到基準 -> 不降碼
namespace {
  // 144 Hz 逐張餵單向延遲：base_ms 平穩、stall_ms>0 時在 at 那一刻停頓 stall_ms，停頓期間送出的幀在停頓結束時一起到
  struct owd_feeder_t {
    fast::fast_controller_t &c;
    std::chrono::steady_clock::time_point t;
    double jitter_phase = 0.0;
    void steady_for(std::chrono::milliseconds dur, double owd_ms) {
      const auto end = t + dur;
      while (t < end) {
        jitter_phase += 1.0;
        c.on_owd(owd_ms + (static_cast<int>(jitter_phase) % 3) * 0.5, t);  // 0～1 ms 抖動
        c.tick(t);
        t += 6944us;
      }
    }
    void stall(std::chrono::milliseconds stall, double base_owd_ms) {
      const int n = static_cast<int>(stall / 6944us);
      const auto arrive = t + stall;
      for (int i = 0; i < n; ++i) {
        const double waited = std::chrono::duration<double, std::milli>(stall - i * 6944us).count();
        c.on_owd(base_owd_ms + waited, arrive);
      }
      c.tick(arrive);
      t = arrive + 6944us;
    }
  };
}  // namespace

TEST_F(VrAbrFastTest, OwdWifiStallIsNotCongestion) {
  fast::fast_controller_t c(kConfigured50Mbps);
  owd_feeder_t f{c, t0};
  f.steady_for(5000ms, 2.0);
  ASSERT_EQ(c.cuts(), 0);
  // 每隔 3 秒停頓一次 100 ms（11-08 在家 1728 那場：每 30～50 秒一次 20～100 ms）
  for (int k = 0; k < 4; ++k) {
    f.stall(100ms, 2.0);
    f.steady_for(3000ms, 2.0);
  }
  EXPECT_EQ(c.cuts(), 0) << "last reason=" << c.last_reason();
  EXPECT_GT(c.owd_stall_skips(), 0);  // 確實判過載過，是新規則擋下的
  EXPECT_EQ(c.applied_kbps(), kConfigured50Mbps);
  EXPECT_EQ(c.last_congested_kbps(), 0);
}

TEST_F(VrAbrFastTest, OwdQueueBuildupAfterStallStillCuts) {
  fast::fast_controller_t c(kConfigured50Mbps);
  owd_feeder_t f{c, t0};
  f.steady_for(5000ms, 2.0);
  f.stall(100ms, 2.0);
  f.steady_for(3000ms, 2.0);
  const int skips = c.owd_stall_skips();
  // 真的排隊：每張多 0.15 ms（每秒 +21 ms），佇列一直不消
  double owd = 2.0;
  bool cut = false;
  for (int i = 0; i < 144 * 2 && !cut; ++i) {
    owd += 0.15;
    c.on_owd(owd, f.t);
    if (c.tick(f.t).has_value() && std::strcmp(c.last_reason(), "owd-trend") == 0) cut = true;
    f.t += 6944us;
  }
  EXPECT_TRUE(cut);
  EXPECT_EQ(c.owd_stall_skips(), skips);
}

// §VR-NETSEL-HOLD：頭盔通知要切網路後，丟包與延遲都不降碼、隨機丟包不計數；過了那段時間照常降
TEST_F(VrAbrFastTest, NetSwitchHoldSkipsNetworkCuts) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);

  const auto t1 = t0 + 10000ms;
  c.begin_net_switch(8000, t1);
  c.begin_net_switch(2000, t1 + 100ms);  // 重複通知（ENet 與連線各一份）不會縮短
  for (int i = 0; i < 20; ++i) {
    const auto t = t1 + std::chrono::milliseconds(300 * i);
    c.on_loss(5, t);
    c.on_drop_block(t);
    EXPECT_FALSE(c.tick(t).has_value()) << "i=" << i << " reason=" << c.last_reason();
  }
  EXPECT_EQ(c.applied_kbps(), 100000);
  EXPECT_GT(c.net_switch_skips(), 0);
  EXPECT_EQ(c.random_losses(), 0);
  // 解碼過載也擋（畫面一次湧進來時解碼 p95 會被拉高）
  for (int i = 0; i < 10; ++i) {
    const auto t = t1 + 6000ms + std::chrono::milliseconds(200 * i);
    c.on_decode(20000, 6944, t);
    EXPECT_FALSE(c.tick(t).has_value()) << "decode i=" << i << " reason=" << c.last_reason();
  }

  const auto t2 = t1 + 8500ms;
  EXPECT_FALSE(c.net_switch_active(t2));
  c.on_loss(2, t2);
  auto res = c.tick(t2);
  ASSERT_TRUE(res.has_value());
  EXPECT_LT(*res, 100000);
}

// 超過上限的要求只算 20 秒
TEST_F(VrAbrFastTest, NetSwitchHoldIsCapped) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.begin_net_switch(60000, t0);
  EXPECT_TRUE(c.net_switch_active(t0 + 19000ms));
  EXPECT_FALSE(c.net_switch_active(t0 + 20500ms));
}

// ==============================================================================
// §ABR-PERSIST-VERIFY（11-13）：丟包跟碼率無關時，+persistent 不可以一路降到下限
// ==============================================================================
namespace {
  /// 每 period 一次短暫斷訊（一筆回報掉 13 張），送出與收到相近（沒有持續的落差，只有斷訊那一下收得少）。回傳最後的碼率
  int run_bursty_line(fast::fast_controller_t &c, fast::steady::time_point t0, std::chrono::milliseconds from, std::chrono::milliseconds to,
                      std::chrono::milliseconds period, int *lowest = nullptr) {
    int applied = c.applied_kbps();
    auto next_burst = from + 3000ms;  // 先有幾秒乾淨的收／送回報
    for (auto t = from; t < to; t += 100ms) {
      const auto now = t0 + t;
      c.on_rtt(35, now);
      if (t >= next_burst) {
        c.on_rates(applied * 80 / 100, applied, now);  // 斷訊那一下收到的比送出的少
        c.on_loss(13, now);
        next_burst += period;
      } else {
        c.on_rates(applied, applied, now);
      }
      if (auto r = c.tick(now)) {
        applied = *r;
        c.set_applied_kbps(applied);
      }
      if (lowest && applied < *lowest) {
        *lowest = applied;
      }
    }
    return applied;
  }
}  // namespace

TEST_F(VrAbrFastTest, PersistentCutsRevertedWhenLossIgnoresBitrate) {
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  int lowest = 75000;
  // 每 1.5 秒一次斷訊，持續 60 秒（實測那天每分鐘 15～35 次）
  const int end = run_bursty_line(c, t0, 1000ms, 61000ms, 1500ms, &lowest);
  EXPECT_EQ(c.persistent_reverts(), 1);
  EXPECT_TRUE(c.persistent_disabled(t0 + 61000ms));
  EXPECT_GT(c.persistent_skips(), 10);
  // 舊行為：一路降到 2000。新行為：降兩三刀就停，之後回升
  EXPECT_GE(lowest, 20000);
  EXPECT_GE(end, 37000);
  EXPECT_EQ(c.persistent_episode_start_kbps(), 75000);
}

TEST_F(VrAbrFastTest, PersistentRevertNeverLeavesBitrateBelowHalfOfEpisodeStart) {
  // 判定之後碼率不低於這一串開始時的一半：兩刀後還高於一半就停在那裡，已經低於一半就拉回一半
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  int applied = 75000;
  int at_judged = 0;
  for (auto t = 1000ms; t < 12000ms; t += 100ms) {
    const auto now = t0 + t;
    c.on_rtt(35, now);
    const bool burst = t >= 4000ms && (t.count() % 1500) == 0;
    c.on_rates(burst ? applied * 80 / 100 : applied, applied, now);
    if (burst) {
      c.on_loss(13, now);
    }
    if (auto r = c.tick(now)) {
      applied = *r;
      c.set_applied_kbps(applied);
    }
    if (c.persistent_reverts() == 1 && at_judged == 0) {
      at_judged = applied;
    }
  }
  ASSERT_EQ(c.persistent_reverts(), 1);
  EXPECT_GE(at_judged, 37500);
  EXPECT_GE(applied, at_judged);  // 判定之後沒有再往下
}

TEST_F(VrAbrFastTest, PersistentCutsKeptWhenLossStopsAfterCut) {
  // 真的送不動：降了之後丟包就停 → 不判定無效、規則照常
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  int applied = 75000;
  for (auto t = 1000ms; t < 30000ms; t += 100ms) {
    const auto now = t0 + t;
    c.on_rtt(35, now);
    const bool lossy = applied > 50000 && (t.count() % 500) == 0;
    c.on_rates(lossy ? applied * 80 / 100 : applied, applied, now);
    if (lossy) {
      c.on_loss(13, now);
    }
    if (auto r = c.tick(now)) {
      applied = *r;
      c.set_applied_kbps(applied);
    }
  }
  EXPECT_EQ(c.persistent_reverts(), 0);
  EXPECT_FALSE(c.persistent_disabled(t0 + 30000ms));
  EXPECT_LE(applied, 60000);
}

TEST_F(VrAbrFastTest, PersistentOffTimeDoublesOnRepeat) {
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  run_bursty_line(c, t0, 1000ms, 30000ms, 1500ms);
  ASSERT_EQ(c.persistent_reverts(), 1);
  // 5 分鐘後規則恢復，線路還是一樣 → 再判定一次，這次停用 10 分鐘
  run_bursty_line(c, t0, 30000ms, 30000ms + 6 * 60000ms, 1500ms);
  EXPECT_EQ(c.persistent_reverts(), 2);
  EXPECT_TRUE(c.persistent_disabled(t0 + 30000ms + 6 * 60000ms + 8 * 60000ms));
}

// 複核（11-13）補的案例
TEST_F(VrAbrFastTest, PolicedLinkIsNotJudgedIneffective) {
  // 限速器直接丟、延遲不漲：容量 34 Mbps、開場 75。降了之後丟的比例跟著降 → 不可以判成「跟碼率無關」，要一路降到容量以下
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  const int capacity = 34000;
  int applied = 75000;
  for (auto t = 1000ms; t < 40000ms; t += 100ms) {
    const auto now = t0 + t;
    c.on_rtt(35, now);
    if ((t.count() % 500) == 0) {
      c.on_rates(std::min(applied, capacity), applied, now);
    }
    if (applied > capacity && (t.count() % 300) == 0) {
      c.on_loss(13, now);
    }
    if (auto r = c.tick(now)) {
      applied = *r;
      c.set_applied_kbps(applied);
    }
  }
  EXPECT_EQ(c.persistent_reverts(), 0);
  EXPECT_LE(applied, capacity * 115 / 100);
}

TEST_F(VrAbrFastTest, UniformRandomLossIsJudgedOnceAndDoesNotOscillate) {
  // 均勻 6% 隨機丟包（每一筆回報都收得比送的少 6%）：判定一次就停，之後 4 分鐘內不可以反覆恢復規則再降
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  int applied = 75000;
  int lowest_after = 0;
  for (auto t = 1000ms; t < 240000ms; t += 100ms) {
    const auto now = t0 + t;
    c.on_rtt(35, now);
    if ((t.count() % 500) == 0) {
      c.on_rates(applied * 94 / 100, applied, now);
    }
    if ((t.count() % 700) == 0) {
      c.on_loss(13, now);
    }
    if (auto r = c.tick(now)) {
      applied = *r;
      c.set_applied_kbps(applied);
    }
    if (c.persistent_reverts() >= 1 && t > 30000ms) {
      lowest_after = lowest_after == 0 ? applied : std::min(lowest_after, applied);
    }
  }
  EXPECT_EQ(c.persistent_reverts(), 1);
  EXPECT_GE(lowest_after, 9000);  // 丟包率規則最多再試一次 −10%（之後會撤銷），不會回到一路往下
}

TEST_F(VrAbrFastTest, RealCongestionCutAfterJudgementIsNotOverwritten) {
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  int applied = 75000;
  fast::steady::time_point judged {};
  // 先讓它判定無效，但判定之後、下一個 tick 之前塞一刀真的塞車
  for (auto t = 1000ms; t < 20000ms && judged == fast::steady::time_point {}; t += 100ms) {
    const auto now = t0 + t;
    c.on_rtt(35, now);
    const bool burst = (t.count() % 1500) == 0;
    if ((t.count() % 500) == 0) {
      c.on_rates(burst ? applied * 80 / 100 : applied, applied, now);
    }
    if (burst) {
      c.on_loss(13, now);
      if (c.persistent_reverts() == 1) {
        judged = now;
        break;  // 不 tick
      }
    }
    if (auto r = c.tick(now)) {
      applied = *r;
      c.set_applied_kbps(applied);
    }
  }
  ASSERT_NE(judged, fast::steady::time_point {});
  const int before = applied;
  make_trend_overuse(c, judged + 10ms);  // delay-trend 真塞車
  int after = applied;
  for (int i = 0; i < 8; ++i) {
    if (auto r = c.tick(judged + 7200ms + i * 250ms)) {
      after = *r;
      c.set_applied_kbps(after);
      if (i == 0) {
        EXPECT_LT(after, before);
      }
    }
  }
  EXPECT_LE(after, before);  // 沒有被「拉回一半」蓋過去
}

namespace {
  /// 穩在 25 Mbps，15～19 秒那 4 秒收到的只剩 pct%。回傳 {最低碼率, 最後碼率}
  std::pair<int, int> run_outage(fast::fast_controller_t &c, fast::steady::time_point t0, int pct) {
    int applied = 25000;
    int lowest = applied;
    for (auto t = 1000ms; t < 30000ms; t += 100ms) {
      const auto now = t0 + t;
      c.on_rtt(35, now);
      const bool outage = t >= 15000ms && t < 19000ms;
      if ((t.count() % 500) == 0) {
        c.on_rates(outage ? applied * pct / 100 : applied, applied, now);
      }
      if (auto r = c.tick(now)) {
        applied = *r;
        c.set_applied_kbps(applied);
      }
      lowest = std::min(lowest, applied);
    }
    return {lowest, applied};
  }
}  // namespace

TEST_F(VrAbrFastTest, TotalOutageDoesNotCutByShortfall) {
  // 11-13 第二輪實測：一次一秒以上幾乎完全收不到的斷訊，rate-shortfall 2.5 秒內連降三刀 25 → 8.7 Mbps
  fast::fast_controller_t c(25000);
  c.set_experimental_rules(true);
  const auto [lowest, end] = run_outage(c, t0, 10);
  EXPECT_GE(lowest, 24000);
  EXPECT_GE(end, 24000);
  EXPECT_GT(c.shortfall_outage_skips(), 0);
}

TEST_F(VrAbrFastTest, PartialOutageShortfallIsJudgedAndRestoredToStable) {
  // 收到剩一半、持續 4 秒：降了兩刀落差沒變小 → 判定無效，拉回先前穩定撐住的碼率
  fast::fast_controller_t c(25000);
  c.set_experimental_rules(true);
  const auto [lowest, end] = run_outage(c, t0, 50);
  EXPECT_EQ(c.persistent_reverts(), 1);
  EXPECT_GE(lowest, 5000);
  EXPECT_GE(end, 24000);
}

TEST_F(VrAbrFastTest, ShortfallOnPolicedLinkStillConverges) {
  // 只有收／送落差、沒有成串丟包回報的限速線路（容量 30、開場 60）：rate-shortfall 要照常降到容量附近，不可以被判成無效
  fast::fast_controller_t c(60000);
  c.set_experimental_rules(true);
  const int capacity = 30000;
  int applied = 60000;
  for (auto t = 1000ms; t < 30000ms; t += 100ms) {
    const auto now = t0 + t;
    c.on_rtt(35, now);
    if ((t.count() % 500) == 0) {
      c.on_rates(std::min(applied, capacity), applied, now);
    }
    if (auto r = c.tick(now)) {
      applied = *r;
      c.set_applied_kbps(applied);
    }
  }
  EXPECT_EQ(c.persistent_reverts(), 0);
  EXPECT_LE(applied, capacity * 115 / 100);
}

TEST_F(VrAbrFastTest, PathUpgradeClearsCongestionMemoryAndRampsFast) {
  // 11-13 第三輪實測：開場在中繼上被延遲規則壓到 2 Mbps，30 秒後轉直連；舊行為每秒 +0.5%，兩分鐘後還在 3 Mbps
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  int applied = 75000;
  auto drive = [&](std::chrono::milliseconds from, std::chrono::milliseconds to, int rtt) {
    for (auto t = from; t < to; t += 100ms) {
      const auto now = t0 + t;
      c.on_rtt(rtt, now);
      if ((t.count() % 500) == 0) {
        c.on_rates(applied, applied, now);
      }
      if (auto r = c.tick(now)) {
        applied = *r;
        c.set_applied_kbps(applied);
      }
    }
  };
  // 中繼上：已經被壓到低點，而且塞車點就記在低點（在 2.5 Mbps 時又因延遲上升降了一刀）
  applied = 2500;
  c.set_applied_kbps(applied);
  make_trend_overuse(c, t0);
  if (auto r = c.tick(t0 + 7100ms)) {
    applied = *r;
    c.set_applied_kbps(applied);
  }
  ASSERT_GT(c.last_congested_kbps(), 0);
  ASSERT_LE(c.last_congested_kbps(), 3000);
  const auto up = 30000ms;
  drive(8000ms, up, 220);  // 延遲停在高點不再漲，慢爬
  const int low = applied;
  ASSERT_LE(low, 4000);    // 舊行為：22 秒只爬一點點
  c.on_path_upgraded(t0 + up);
  EXPECT_EQ(c.last_congested_kbps(), 0);
  EXPECT_TRUE(c.path_boost_active(t0 + up + 1000ms));
  drive(up, up + 15000ms, 20);
  EXPECT_GE(applied, low * 8);  // 15 秒內至少 8 倍（每秒 +25%）；舊行為約 +8%
}

TEST_F(VrAbrFastTest, LossRateRuleStaysOffWhilePersistentIsJudgedIneffective) {
  fast::fast_controller_t c(75000);
  c.set_experimental_rules(true);
  run_bursty_line(c, t0, 1000ms, 20000ms, 1500ms);
  ASSERT_EQ(c.persistent_reverts(), 1);
  const int cuts_before = c.loss_rate_cuts();
  run_bursty_line(c, t0, 20000ms, 120000ms, 1500ms);
  EXPECT_EQ(c.loss_rate_cuts(), cuts_before);
}

// §ABR-OUTAGE-HOLD（11-13 第四輪）
TEST_F(VrAbrFastTest, OutageWithoutPriorCongestionDoesNotCut) {
  // 延遲一直正常，每 3 秒一次斷訊（掉 20 張＝約 140 ms），斷訊結束那一下延遲量測被灌爆、丟包跟著回報
  fast::fast_controller_t c(40000);
  c.set_experimental_rules(true);
  int applied = 40000;
  int lowest = applied;
  for (auto t = 1000ms; t < 60000ms; t += 100ms) {
    const auto now = t0 + t;
    const bool burst = t >= 5000ms && (t.count() % 3000) == 0;
    c.on_owd(burst ? 180.0 : 25.0, now);
    c.on_rtt(burst ? 300 : 35, now);
    if ((t.count() % 500) == 0) {
      c.on_rates(burst ? applied * 70 / 100 : applied, applied, now);
    }
    if (burst) {
      EXPECT_TRUE(c.on_outage(140, now));
      c.on_loss(20, now);
      c.on_fec_failed(2, now + 20ms);
    }
    if (auto r = c.tick(now)) {
      applied = *r;
      c.set_applied_kbps(applied);
    }
    lowest = std::min(lowest, applied);
  }
  EXPECT_GE(lowest, 40000);
  EXPECT_GT(c.outage_holds(), 10);
}

TEST_F(VrAbrFastTest, LossDuringOngoingCongestionIsNotTreatedAsOutage) {
  // 延遲已經連續上升好幾秒（真的塞車）才開始成串丟包：不可以當成斷訊，要照常降
  fast::fast_controller_t c(40000);
  c.set_experimental_rules(true);
  make_trend_overuse(c, t0);              // 5 秒基準＋2 秒一路上升，結束在 t0+7000ms
  const auto now = t0 + 7050ms;
  EXPECT_FALSE(c.on_outage(140, now));
  c.on_loss(20, now);
  auto r = c.tick(now + 10ms);
  ASSERT_TRUE(r.has_value());
  EXPECT_LT(*r, 40000);
}

TEST_F(VrAbrFastTest, SustainedShortfallIsNotTreatedAsOutage) {
  // 限速器直接丟（延遲不漲、收到的一直比送出的少）：成串丟包不可以被當成斷訊而永遠不降
  fast::fast_controller_t c(60000);
  c.set_experimental_rules(true);
  const int capacity = 30000;
  int applied = 60000;
  for (auto t = 1000ms; t < 40000ms; t += 100ms) {
    const auto now = t0 + t;
    c.on_rtt(35, now);
    if ((t.count() % 500) == 0) {
      c.on_rates(std::min(applied, capacity), applied, now);
    }
    if (applied > capacity && (t.count() % 400) == 0) {
      c.on_outage(140, now);
      c.on_loss(20, now);
    }
    if (auto r = c.tick(now)) {
      applied = *r;
      c.set_applied_kbps(applied);
    }
  }
  EXPECT_LE(applied, capacity * 115 / 100);
}


TEST_F(VrAbrFastTest, ExperimentalRulesOffBehavesLikeBefore) {
  // 開關關著（預設）：成串丟包照舊每次都降，不判定、不暫停、不因斷訊而擋
  fast::fast_controller_t c(75000);
  EXPECT_FALSE(c.experimental_rules());
  const int end = run_bursty_line(c, t0, 1000ms, 61000ms, 1500ms);
  EXPECT_EQ(c.persistent_reverts(), 0);
  EXPECT_FALSE(c.persistent_disabled(t0 + 61000ms));
  EXPECT_FALSE(c.on_outage(140, t0 + 61000ms));
  EXPECT_EQ(c.outage_holds(), 0);
  EXPECT_LE(end, 10000);  // 一路降下去（原本的行為）
  c.on_path_upgraded(t0 + 62000ms);
  EXPECT_EQ(c.path_upgrades(), 0);
}

// §VR-QP-ROOM：沒有塞過車時用較嚴的門檻（QP 10 才算飽和）；QP 15 在這時不擋回升
TEST_F(VrAbrFastTest, RoomyLinkUsesStricterQualityGates) {
  fast::fast_controller_t c(kConfigured50Mbps);
  EXPECT_TRUE(c.link_roomy(t0));
  EXPECT_EQ(c.qp_saturated_x100(t0), fast::k_fast_qp_saturated_roomy_x100);
  EXPECT_EQ(c.qp_trim_x100(t0), fast::k_fast_qp_trim_roomy_x100);
  c.on_quality(1500, 1, t0);
  EXPECT_STREQ(c.ramp_block_reason(t0), "none");
  c.on_quality(900, 1, t0 + 100ms);
  EXPECT_STREQ(c.ramp_block_reason(t0 + 100ms), "quality");
}

// §VR-QP-ROOM：回收門檻固定 QP 6，不隨線路狀況切換；剛塞過車的 30 秒內停止回升的門檻回到 QP 20
TEST_F(VrAbrFastTest, RecentCongestionUsesLooseSaturationGateButSameTrimGate) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);
  const auto t_loss = t0 + 10000ms;
  c.on_fec_failed(2, t_loss);
  ASSERT_TRUE(c.tick(t_loss).has_value());
  ASSERT_GT(c.last_congested_kbps(), 0);
  const auto t1 = t_loss + 5000ms;
  EXPECT_TRUE(c.recently_congested(t1));
  EXPECT_FALSE(c.link_roomy(t1));
  EXPECT_EQ(c.qp_saturated_x100(t1), fast::k_fast_qp_saturated_x100);
  EXPECT_EQ(c.qp_trim_x100(t1), fast::k_fast_qp_trim_roomy_x100);
}

// §VR-QP-ROOM：接近塞車點（原始塞車點的九成以內）時停止回升的門檻是 QP 20；離開要低過遲滯；150 秒沒再塞車後界線失效
TEST_F(VrAbrFastTest, NearCeilingGateHasHysteresisAndExpires) {
  fast::fast_controller_t c(kConfigured100Mbps);
  make_trend_overuse(c, t0);
  c.tick(t0 + 7000ms);
  c.set_applied_kbps(100000);
  const auto t_loss = t0 + 10000ms;
  c.on_fec_failed(2, t_loss);
  ASSERT_TRUE(c.tick(t_loss).has_value());
  const int ceiling = c.last_congested_kbps();
  ASSERT_GT(ceiling, 0);
  const auto t1 = t_loss + 40000ms;  // 安靜期過了、界線還有效
  ASSERT_FALSE(c.recently_congested(t1));
  const int limit = static_cast<int>(fast::k_fast_roomy_ceiling_ratio * ceiling);  // 40 秒只放寬 1%，仍是八成那一條
  c.set_applied_kbps(limit / 2);
  EXPECT_FALSE(c.near_ceiling(t1));
  EXPECT_EQ(c.qp_saturated_x100(t1), fast::k_fast_qp_saturated_roomy_x100);
  c.set_applied_kbps(static_cast<int>(ceiling * 0.89));
  EXPECT_TRUE(c.near_ceiling(t1));
  EXPECT_EQ(c.qp_saturated_x100(t1), fast::k_fast_qp_saturated_x100);
  // 遲滯：剛好低於界線一點點仍算接近；低過 6% 才離開
  c.set_applied_kbps(static_cast<int>(limit * 1.01 * 0.97));
  EXPECT_TRUE(c.near_ceiling(t1));
  c.set_applied_kbps(static_cast<int>(limit * 0.90));
  EXPECT_FALSE(c.near_ceiling(t1));
  // 150 秒沒再塞車：界線失效，碼率高於塞車點也不算接近
  c.set_applied_kbps(ceiling);
  EXPECT_FALSE(c.near_ceiling(t_loss + 151000ms));
  EXPECT_TRUE(c.link_roomy(t_loss + 151000ms));
}

// ==============================================================================
// §RATE-UNIFIED 第 0 期（2026-11-20）：RTT 嚴重膨脹交給控制器、補包量大停止回升
// ==============================================================================

// 沒有單向延遲的 client：照樣降 ×0.75，但走控制器——有冷卻、記塞車點；冷卻中再來一次不疊刀
TEST_F(VrAbrFastTest, RttSevereWithoutOwdCutsThroughController) {
  using sev = fast::fast_controller_t::rtt_severe_e;
  fast::fast_controller_t c(kConfigured50Mbps);
  c.tick(t0);
  const auto t1 = t0 + 1000ms;
  EXPECT_EQ(c.on_rtt_severe(t1), sev::cut);
  EXPECT_STREQ(c.last_reason(), "rtt-severe+delay");
  EXPECT_LT(c.rtt_severe_owd_excess_ms(), 0.0);  // 沒有單向延遲可看
  auto r = c.tick(t1);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 37500);  // 50000 × 0.75
  EXPECT_EQ(c.state(), fast::fast_state_e::hold);
  EXPECT_EQ(c.last_congested_kbps(), 50000);
  EXPECT_TRUE(c.recently_congested(t1 + 500ms));
  EXPECT_FALSE(c.link_roomy(t1 + 500ms));
  // 冷卻中（沒有 RTT 樣本時冷卻 2 秒）：不處理
  EXPECT_EQ(c.on_rtt_severe(t1 + 500ms), sev::held);
  EXPECT_FALSE(c.tick(t1 + 500ms).has_value());
  EXPECT_EQ(c.cuts(), 1);
  EXPECT_EQ(c.rtt_severe_cuts(), 1);
  // 冷卻過了還是嚴重：再降一刀
  c.tick(t1 + 2100ms);
  EXPECT_EQ(c.on_rtt_severe(t1 + 2200ms), sev::cut);
  r = c.tick(t1 + 2200ms);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 28125);  // 37500 × 0.75
  EXPECT_EQ(c.rtt_severe_cuts(), 2);
  EXPECT_EQ(c.rtt_severe_skips(), 0);
}

// 單向延遲可用而且平穩：塞的是上行，不降；單向延遲樣本斷了（回饋到不了）就退回照 RTT 降
TEST_F(VrAbrFastTest, RttSevereWithFlatOwdIsUplinkOnly) {
  using sev = fast::fast_controller_t::rtt_severe_e;
  fast::fast_controller_t c(kConfigured50Mbps);
  for (int i = 0; i < 50; ++i) {
    c.on_owd(20.0, t0 + i * 10ms);
  }
  const auto t1 = t0 + 500ms;
  c.tick(t1);
  EXPECT_EQ(c.on_rtt_severe(t1), sev::uplink_only);
  EXPECT_NEAR(c.rtt_severe_owd_excess_ms(), 0.0, 0.5);
  EXPECT_FALSE(c.tick(t1).has_value());
  EXPECT_EQ(c.cuts(), 0);
  EXPECT_EQ(c.rtt_severe_skips(), 1);
  EXPECT_EQ(c.last_congested_kbps(), 0);
  EXPECT_TRUE(c.link_roomy(t1));
  // 3 秒沒有單向延遲樣本
  const auto t2 = t1 + 3000ms;
  ASSERT_FALSE(c.owd_active(t2));
  EXPECT_EQ(c.on_rtt_severe(t2), sev::cut);
  auto r = c.tick(t2);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 37500);
}

// 單向延遲慢慢墊高（每秒 1 ms，趨勢抓不到）到比這一場的最低值高出 50 ms 以上：影像方向也在排隊，RTT 嚴重膨脹時要降
TEST_F(VrAbrFastTest, RttSevereWithStandingOwdCuts) {
  using sev = fast::fast_controller_t::rtt_severe_e;
  fast::fast_controller_t c(kConfigured50Mbps);
  auto t = t0;
  for (int i = 0; i < 5000; ++i) {  // 100 秒
    t = t0 + i * 20ms;
    c.on_owd(20.0 + i * 0.02, t);
    if (i % 25 == 0) {
      c.tick(t);
    }
  }
  ASSERT_EQ(c.cuts(), 0);  // 趨勢與安全網都沒有判過載
  ASSERT_EQ(c.applied_kbps(), 50000);
  EXPECT_EQ(c.on_rtt_severe(t), sev::cut);
  EXPECT_GE(c.rtt_severe_owd_excess_ms(), fast::k_fast_rtt_severe_owd_excess_ms);
  auto r = c.tick(t);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 37500);
  EXPECT_STREQ(c.last_reason(), "rtt-severe+delay");
}

// 補包量 ≥ 3%：停止回升；降到 1.5% 以下、再過 2 秒才解除（遲滯）；估不出來（−1）的那一輪不算
TEST_F(VrAbrFastTest, RepairLoadBlocksRampWithHysteresis) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.set_applied_kbps(20000);
  c.tick(t0);
  ASSERT_EQ(c.state(), fast::fast_state_e::ramp);
  EXPECT_EQ(c.take_repair_event(t0), 0);
  c.on_repair_load(4.0, t0);
  EXPECT_TRUE(c.repair_tight(t0));
  EXPECT_STREQ(c.ramp_block_reason(t0), "repair");
  EXPECT_EQ(c.take_repair_event(t0), 1);
  EXPECT_EQ(c.take_repair_event(t0), 0);
  // 3 秒、每 500 ms 一筆 2%（比平常多 1.5 個百分點以上）：還擋著
  for (int i = 1; i <= 6; ++i) {
    const auto t = t0 + i * 500ms;
    c.on_repair_load(2.0, t);
    EXPECT_FALSE(c.tick(t).has_value());
  }
  EXPECT_EQ(c.applied_kbps(), 20000);
  EXPECT_EQ(c.ramps(), 0);
  // 降到 0.5%：最後一次超過門檻（t0+3000）之後 2 秒內仍擋
  for (int i = 7; i <= 9; ++i) {
    const auto t = t0 + i * 500ms;
    c.on_repair_load(i == 8 ? -1.0 : 0.5, t);
    EXPECT_STREQ(c.ramp_block_reason(t), "repair");
    EXPECT_FALSE(c.tick(t).has_value());
  }
  EXPECT_EQ(c.take_repair_event(t0 + 4500ms), 0);
  // 滿 2 秒：解除、回升
  const auto t_free = t0 + 5000ms;
  c.on_repair_load(0.5, t_free);
  EXPECT_FALSE(c.repair_tight(t_free));
  EXPECT_STREQ(c.ramp_block_reason(t_free), "none");
  EXPECT_EQ(c.take_repair_event(t_free), -1);
  auto r = c.tick(t_free + 250ms);
  ASSERT_TRUE(r.has_value());
  EXPECT_GT(*r, 20000);
  EXPECT_EQ(c.cuts(), 0);
}

// 線路乾淨（補包量 0～1%）：完全不擋，回升照常
TEST_F(VrAbrFastTest, SmallRepairLoadNeverBlocksRamp) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.set_applied_kbps(20000);
  c.tick(t0);
  for (int i = 1; i <= 40; ++i) {
    const auto t = t0 + i * 500ms;
    c.on_repair_load(i % 2 ? 0.0 : 1.0, t);
    EXPECT_FALSE(c.repair_tight(t));
    c.tick(t);
  }
  EXPECT_EQ(c.take_repair_event(t0 + 20000ms), 0);
  EXPECT_GT(c.ramps(), 0);
  EXPECT_GT(c.applied_kbps(), 20000);
  EXPECT_LT(c.repair_floor_pct(), 1.0);
}

// 補包量一直固定在 4%（跟碼率無關的無線成串掉包）：擋 25 秒、碼率沒動、補包量沒變少＝跟碼率無關，放行並當成這條線路
// 平常的量；之後 3 秒平均再多出 3 個百分點才又擋。
// （第二輪審查 C01／C13 改了語意：擋著時平常量不往上學、比的是 3 秒平均，所以平常量是放行時直接設成 4%，
//  單一個 8% 的視窗也不再觸發，要持續約 3 秒）
TEST_F(VrAbrFastTest, RepairUsualLevelIsLearnedSoConstantLoadDoesNotBlockForever) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.set_applied_kbps(20000);
  c.tick(t0);
  for (int i = 0; i <= 90; ++i) {  // 45 秒
    const auto t = t0 + i * 500ms;
    c.on_repair_load(4.0, t);
    if (i == 40) {
      EXPECT_TRUE(c.repair_tight(t));  // 20 秒時還擋著
      EXPECT_STREQ(c.ramp_block_reason(t), "repair");
      EXPECT_DOUBLE_EQ(c.repair_floor_pct(), 0.0);  // 擋著時不往上學
    }
  }
  const auto t1 = t0 + 45000ms;
  EXPECT_FALSE(c.repair_tight(t1));
  EXPECT_STREQ(c.ramp_block_reason(t1), "none");
  EXPECT_EQ(c.repair_unrelated_releases(), 1);
  EXPECT_GT(c.repair_floor_pct(), 2.5);
  EXPECT_LE(c.repair_floor_pct(), 4.0);
  c.on_repair_load(5.0, t1 + 500ms);  // 只多 1 個百分點
  EXPECT_FALSE(c.repair_tight(t1 + 500ms));
  c.on_repair_load(8.0, t1 + 1000ms);  // 單一視窗多了 4 個百分點：3 秒平均還沒到
  EXPECT_FALSE(c.repair_tight(t1 + 1000ms));
  for (int i = 3; i <= 8; ++i) {  // 持續 3 秒
    c.on_repair_load(8.0, t1 + i * 500ms);
  }
  EXPECT_TRUE(c.repair_tight(t1 + 4000ms));
  EXPECT_STREQ(c.ramp_block_reason(t1 + 4000ms), "repair");
}

// 審查 11-21：平常補包量約 14%（高過 floor 上限 10）。放行時 floor 被夾成 10，若再觸發仍用 floor+3＝13，
// 下一個 14% 樣本立刻又擋，放行等於沒有。接受量記的是放行當下的量（不夾 10），連續 14% 不該再擋；升到 20% 以上才再觸發。
TEST_F(VrAbrFastTest, HighUsualRepairLoadStaysReleasedUntilItRisesFurther) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.set_applied_kbps(20000);
  c.tick(t0);
  for (int i = 0; i <= 90; ++i) {  // 45 秒，涵蓋 25 秒判定
    const auto t = t0 + i * 500ms;
    c.on_repair_load(14.0, t);
  }
  const auto t1 = t0 + 45000ms;
  EXPECT_FALSE(c.repair_tight(t1));
  EXPECT_EQ(c.repair_unrelated_releases(), 1);
  EXPECT_DOUBLE_EQ(c.repair_floor_pct(), fast::k_fast_repair_floor_max_pct);  // 14% 被夾成 10
  for (int i = 1; i <= 40; ++i) {  // 放行後再 20 秒的 14%
    const auto t = t1 + i * 500ms;
    c.on_repair_load(14.0, t);
    EXPECT_FALSE(c.repair_tight(t));
  }
  EXPECT_EQ(c.repair_unrelated_releases(), 1);
  const auto t2 = t1 + 20000ms;
  for (int i = 1; i <= 8; ++i) {  // 升到 20%，3 秒平均超過接受量 + 3 後再擋
    c.on_repair_load(20.0, t2 + i * 500ms);
  }
  EXPECT_TRUE(c.repair_tight(t2 + 4000ms));
  EXPECT_STREQ(c.ramp_block_reason(t2 + 4000ms), "repair");
}

// C01：每 1.5 秒有一個視窗補 5%、其餘 0（跟碼率無關的週期性成串補包）。3 秒平均約 1.4～2.1%，從頭到尾都不擋，回升照常
TEST_F(VrAbrFastTest, PeriodicRepairBurstsDoNotBlockRamp) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.set_applied_kbps(20000);
  c.tick(t0);
  int tight_samples = 0;
  for (int i = 1; i <= 120; ++i) {  // 60 秒
    const auto t = t0 + i * 500ms;
    c.on_repair_load(i % 3 == 0 ? 5.0 : 0.0, t);
    tight_samples += c.repair_tight(t) ? 1 : 0;
    c.tick(t);
  }
  EXPECT_EQ(tight_samples, 0);
  EXPECT_LT(c.repair_mean_pct(), 3.0);
  EXPECT_EQ(c.applied_kbps(), 50000);  // 一路爬到上限
}

// C01＋出口：週期性成串補包剛好在開場就碰上一個 5% 的視窗（3 秒平均只有這一筆）而開始擋：碼率沒動、補包量沒變少，
// 25 秒後判定跟碼率無關放行，平常量設成當時的 3 秒平均；之後不再擋，碼率爬到上限
TEST_F(VrAbrFastTest, PeriodicRepairBurstsStartingTightAreReleasedAsUnrelated) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.set_applied_kbps(20000);
  c.tick(t0);
  auto last_tight = t0;
  for (int i = 0; i <= 160; ++i) {  // 80 秒
    const auto t = t0 + i * 500ms;
    c.on_repair_load(i % 3 == 0 ? 5.0 : 0.0, t);
    if (c.repair_tight(t)) {
      last_tight = t;
      EXPECT_EQ(c.applied_kbps(), 20000);  // 擋著時不回升
    }
    c.tick(t);
  }
  EXPECT_TRUE(last_tight > t0);
  EXPECT_LE(last_tight - t0, 27000ms);  // 25 秒判定（加最後 2 秒維持以內）
  EXPECT_EQ(c.repair_unrelated_releases(), 1);
  EXPECT_FALSE(c.repair_related());  // 開始擋之前碼率沒有往上加
  EXPECT_GT(c.repair_floor_pct(), 1.0);
  EXPECT_LT(c.repair_floor_pct(), 2.5);
  EXPECT_EQ(c.applied_kbps(), 50000);
}

namespace {
  /// C13：補包比例隨碼率上升（越加碼補越多）：20 Mbps 時 0%，每多 1 Mbps 多 1 個百分點
  double rising_repair_pct(int applied_kbps) {
    return std::max(0.0, (applied_kbps - 20000) / 1000.0);
  }
}  // namespace

// C13：越加碼補越多。開始擋之前碼率剛往上加過＝可能是加碼造成的：第一階擋 90 秒、之後每一階加倍；擋著期間平常量不往上學。
// 以前（平常量照樣往上學、每階只擋約 25 秒）三分鐘內就爬到平常量的 10% 上限（約 33 Mbps，補包 13%）
TEST_F(VrAbrFastTest, RepairRisingWithBitrateKeepsRampBlocked) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.set_applied_kbps(20000);
  c.tick(t0);
  fast::steady::time_point first_tight {};
  int applied_at_first_tight = 0;
  int applied_at_3min = 0;
  for (int i = 1; i <= 1200; ++i) {  // 10 分鐘
    const auto t = t0 + i * 500ms;
    c.on_repair_load(rising_repair_pct(c.applied_kbps()), t);
    if (c.repair_tight(t) && first_tight == fast::steady::time_point {}) {
      first_tight = t;
      applied_at_first_tight = c.applied_kbps();
      EXPECT_TRUE(c.repair_related());
      EXPECT_EQ(c.repair_judge_s(), 90);
    }
    if (first_tight != fast::steady::time_point {} && t - first_tight < 89000ms) {
      EXPECT_TRUE(c.repair_tight(t));  // 第一階整整擋 90 秒
      EXPECT_EQ(c.applied_kbps(), applied_at_first_tight);
      EXPECT_LT(c.repair_floor_pct(), 1.0);  // 擋著時平常量不往上學
    }
    c.tick(t);
    if (i == 360) {
      applied_at_3min = c.applied_kbps();
    }
  }
  ASSERT_NE(first_tight, fast::steady::time_point {});
  EXPECT_LT(applied_at_first_tight, 26000);  // 補包多出 3 個百分點左右就擋住
  EXPECT_LT(applied_at_3min, 30000);
  EXPECT_LE(c.repair_unrelated_releases(), 3);  // 90＋180＋360 秒：10 分鐘最多放行 3 次
  // 3 秒平均比單一視窗晚約 1.5 秒反應，乘法回升（每秒 +8%）時每一階會多爬一點：實測三階停在約 24.9／29.7／34.8 Mbps
  EXPECT_LT(c.applied_kbps(), 36000);
  EXPECT_EQ(c.cuts(), 0);  // 只擋回升，不降碼
}

// C02：估不出來的那一輪（−1，fast-loss 那種 elapsedMs = 0 的呼叫）完全不算：3 秒平均、平常量、擋不擋都不變
TEST_F(VrAbrFastTest, UnmeasurableRepairWindowIsIgnored) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.on_repair_load(2.0, t0);
  c.on_repair_load(2.0, t0 + 500ms);
  const double mean = c.repair_mean_pct();
  const double floor = c.repair_floor_pct();
  c.on_repair_load(-1.0, t0 + 600ms);
  EXPECT_DOUBLE_EQ(c.repair_mean_pct(), mean);
  EXPECT_DOUBLE_EQ(c.repair_floor_pct(), floor);
  EXPECT_DOUBLE_EQ(c.repair_pct(), 2.0);
  EXPECT_FALSE(c.repair_tight(t0 + 600ms));
}

// ==============================================================================
// 補包比例的視窗（第三輪審查 C07／C08／C09）
// ==============================================================================

namespace {
  /// 和 stream.cpp abr_expected_packets 同一條式子：kbps × 125 ÷ 封包大小 × 秒數
  auto expected_at(int kbps, int pkt = 1392) {
    return [kbps, pkt](long long ms) {
      return (int) ((double) kbps * 125.0 / (double) pkt * (double) ms / 1000.0);
    };
  }
}  // namespace

// C07／C09：3 Mbps 時 500 ms 只有約 134 個封包（不到 200）：這一輪不取走，和下一輪合起來算，分母是合起來的 1000 ms。
// 以前每一輪都取走、比例回 −1 被丟掉，低碼率時控制器整段看不到補包量
TEST_F(VrAbrFastTest, RepairWindowCarriesOverUntilEnoughPackets) {
  fast::repair_window_t w;
  const auto exp = expected_at(3000);
  int accum = 0;
  accum += 5;
  EXPECT_EQ(w.settle(t0, 500, 200, exp), 0);  // 134 個：不取
  accum += 5;
  const long long win = w.settle(t0 + 500ms, 500, 200, exp);
  ASSERT_EQ(win, 1000);
  const double pct = 100.0 * accum / exp(win);
  EXPECT_NEAR(pct, 10.0 * 100.0 / 269.0, 0.01);  // 約 3.7%，不是用 500 ms 的分母放大成兩倍
  accum = 0;
  // 結算之後從這一刻重新起算
  EXPECT_EQ(w.settle(t0 + 1000ms, 500, 200, exp), 0);
  EXPECT_EQ(w.settle(t0 + 1500ms, 500, 200, exp), 1000);
}

// C08：FEC status 停下後的第一個 ping-tick 涵蓋 1.2 秒，呼叫端照樣傳 500：分母要用實際的 1200 ms
TEST_F(VrAbrFastTest, RepairWindowUsesRealElapsedTimeNotCallerWindow) {
  fast::repair_window_t w;
  const auto exp = expected_at(50000);
  EXPECT_EQ(w.settle(t0, 500, 200, exp), 500);  // 第一次：照呼叫端的視窗
  EXPECT_EQ(w.settle(t0 + 1200ms, 500, 200, exp), 1200);
  EXPECT_EQ(w.settle(t0 + 1700ms, 500, 200, exp), 500);
}

// 沒有視窗的呼叫（fast-loss，elapsedMs = 0）不結算、也不移動起點
TEST_F(VrAbrFastTest, RepairWindowIgnoresCallsWithoutAWindow) {
  fast::repair_window_t w;
  const auto exp = expected_at(50000);
  EXPECT_EQ(w.settle(t0, 500, 200, exp), 500);
  EXPECT_EQ(w.settle(t0 + 300ms, 0, 200, exp), 0);
  EXPECT_EQ(w.settle(t0 + 700ms, 500, 200, exp), 700);
}

// 碼率估不出來（0 個封包）時不會無限期累積：10 秒後回 −1（呼叫端丟掉），從那一刻重新起算
TEST_F(VrAbrFastTest, RepairWindowDropsStaleDataWhenNeverMeasurable) {
  fast::repair_window_t w;
  const auto none = [](long long) { return 0; };
  long long r = 0;
  int calls = 0;
  for (int i = 0; i <= 20 && r == 0; ++i) {
    r = w.settle(t0 + i * 500ms, 500, 200, none);
    ++calls;
  }
  EXPECT_EQ(r, -1);
  EXPECT_EQ(calls, 20);  // 起點是 t0 − 500 ms，t0 + 9.5 s 剛好滿 10 秒
  EXPECT_EQ(w.settle(t0 + 10000ms, 500, 200, none), 0);
}

// C11：換到更好的路（on_path_upgraded）時，舊路徑上的補包狀態一起作廢：不再擋、3 秒平均歸零、平常的量壓到門檻以下，
// 新路徑上又「越加碼補越多」時，等待時間從 90 秒起算，不沿用舊路徑判定放行後加倍的 180 秒
TEST_F(VrAbrFastTest, PathUpgradeResetsRepairState) {
  fast::fast_controller_t c(kConfigured50Mbps);
  c.set_experimental_rules(true);
  c.set_applied_kbps(20000);
  c.tick(t0);
  auto t = t0;
  int i = 1;
  for (; i <= 2400 && c.repair_unrelated_releases() == 0; ++i) {
    t = t0 + i * 500ms;
    c.on_repair_load(rising_repair_pct(c.applied_kbps()), t);
    c.tick(t);
  }
  ASSERT_EQ(c.repair_unrelated_releases(), 1);
  // 判定放行後讓它再擋一次，確認舊路徑上的等待已經加倍
  fast::steady::time_point tight_again {};
  for (int k = 0; k < 600 && tight_again == fast::steady::time_point {}; ++k) {
    t += 500ms;
    c.on_repair_load(rising_repair_pct(c.applied_kbps()) + 5.0, t);
    if (c.repair_tight(t)) {
      tight_again = t;
      EXPECT_EQ(c.take_repair_event(t), 1);
    }
    c.tick(t);
  }
  ASSERT_NE(tight_again, fast::steady::time_point {});
  ASSERT_TRUE(c.repair_related());
  ASSERT_EQ(c.repair_judge_s(), 180);

  t += 500ms;
  c.on_path_upgraded(t);
  EXPECT_FALSE(c.repair_tight(t));
  EXPECT_DOUBLE_EQ(c.repair_mean_pct(), 0.0);
  EXPECT_LE(c.repair_floor_pct(), fast::k_fast_repair_tight_pct);
  EXPECT_EQ(c.take_repair_event(t), -1);  // 解除事件照常印出

  // 新路徑上補包又隨碼率上升（以換路當下的碼率為零點）
  const int base = c.applied_kbps();
  fast::steady::time_point tight_new {};
  for (int k = 0; k < 600 && tight_new == fast::steady::time_point {}; ++k) {
    t += 500ms;
    c.on_repair_load(std::max(0.0, (c.applied_kbps() - base) / 1000.0), t);
    if (c.repair_tight(t)) {
      tight_new = t;
    }
    c.tick(t);
  }
  ASSERT_NE(tight_new, fast::steady::time_point {});
  EXPECT_TRUE(c.repair_related());
  EXPECT_EQ(c.repair_judge_s(), 90);
}

// ==============================================================================
// §VR-QP-ROOM 退避（第二輪審查 C12）
// ==============================================================================

namespace {
  /// 在 at 這一刻、applied = kbps 時塞一次車（沒有單向延遲的 client：RTT 嚴重膨脹 → ×0.75，記塞車點）
  void congest_at(fast::fast_controller_t &c, int kbps, fast::steady::time_point at) {
    c.set_applied_kbps(kbps);
    ASSERT_EQ(c.on_rtt_severe(at), fast::fast_controller_t::rtt_severe_e::cut);
    ASSERT_TRUE(c.tick(at).has_value());
  }
}  // namespace

// 界線失效後在同一個塞車點附近又塞＝線路的容量：有效期 150 → 300 → 600 秒封頂；有效期內又塞維持
TEST_F(VrAbrFastTest, NearCeilingExpiryBacksOffWhenSamePointCongestsAgain) {
  fast::fast_controller_t c(kConfigured100Mbps);
  auto t = t0 + 1000ms;
  congest_at(c, 60000, t);
  EXPECT_EQ(c.ceiling_expire_s(), 150);
  t += 171s;  // 失效後約 21 秒爬回塞車點
  congest_at(c, 61000, t);
  EXPECT_EQ(c.ceiling_expire_s(), 300);
  EXPECT_EQ(c.ceiling_backoffs(), 1);
  // 200 秒後界線仍有效（以前 150 秒就失效）
  c.set_applied_kbps(static_cast<int>(c.last_congested_kbps() * 0.9));
  EXPECT_TRUE(c.near_ceiling(t + 200s));
  EXPECT_FALSE(c.near_ceiling(t + 301s));
  t += 321s;
  congest_at(c, 60000, t);
  EXPECT_EQ(c.ceiling_expire_s(), 600);
  t += 621s;
  congest_at(c, 60500, t);
  EXPECT_EQ(c.ceiling_expire_s(), 600);  // 封頂
  t += 100s;  // 有效期內又塞（窄線路）：維持
  congest_at(c, 59000, t);
  EXPECT_EQ(c.ceiling_expire_s(), 600);
}

// 縮回 150 秒：在離舊點很遠的地方塞、很久沒塞、碼率爬過舊點 1.15 倍都沒塞、換路
TEST_F(VrAbrFastTest, NearCeilingBackoffShrinksWhenLinkChanges) {
  fast::fast_controller_t c(kConfigured100Mbps);
  c.set_experimental_rules(true);  // on_path_upgraded 要開關打開才作用
  auto t = t0 + 1000ms;
  congest_at(c, 60000, t);
  t += 171s;
  congest_at(c, 60000, t);
  ASSERT_EQ(c.ceiling_expire_s(), 300);
  // 在很不一樣的點塞（線路變了）
  t += 310s;
  congest_at(c, 40000, t);
  EXPECT_EQ(c.ceiling_expire_s(), 150);
  // 重新退避一次，再「很久沒塞」
  t += 160s;
  congest_at(c, 40000, t);
  ASSERT_EQ(c.ceiling_expire_s(), 300);
  t += 300s + 121s;
  congest_at(c, 40000, t);
  EXPECT_EQ(c.ceiling_expire_s(), 150);
  // 退避後碼率爬過舊點的 1.15 倍都沒塞：線路變好了
  t += 160s;
  congest_at(c, 40000, t);
  ASSERT_EQ(c.ceiling_expire_s(), 300);
  c.set_applied_kbps(47000);
  c.tick(t + 200s);
  EXPECT_EQ(c.ceiling_expire_s(), 150);
  // 換路
  t += 400s;
  congest_at(c, 47000, t);
  t += 160s;
  congest_at(c, 47000, t);
  ASSERT_EQ(c.ceiling_expire_s(), 300);
  c.on_path_upgraded(t + 1s);
  EXPECT_EQ(c.ceiling_expire_s(), 150);
}

// 在家寬線路：單次偶發塞車之後沒有在同一點再塞，150 秒就失效、回到 QP 10 門檻（不會被鎖）
TEST_F(VrAbrFastTest, SingleTransientCongestionStillExpiresAfter150s) {
  fast::fast_controller_t c(kConfigured100Mbps);
  const auto t = t0 + 1000ms;
  congest_at(c, 100000, t);
  c.set_applied_kbps(95000);
  EXPECT_TRUE(c.near_ceiling(t + 60s));
  EXPECT_FALSE(c.near_ceiling(t + 151s));
  EXPECT_TRUE(c.link_roomy(t + 151s));
  EXPECT_EQ(c.qp_saturated_x100(t + 151s), fast::k_fast_qp_saturated_roomy_x100);
  EXPECT_EQ(c.ceiling_backoffs(), 0);
}

// 中等線路 30 分鐘：約 60 Mbps 就塞、塞車點附近 QP 15。以前每約 170 秒重撞一次（約 10 次），退避後約 5 次
TEST_F(VrAbrFastTest, MediumLineDoesNotRehitCongestionPointPeriodically) {
  fast::fast_controller_t c(150000);
  c.set_applied_kbps(60000);
  auto t = t0;
  c.tick(t);
  for (int i = 1; i <= 3600; ++i) {  // 30 分鐘、每 500 ms
    t = t0 + i * 500ms;
    c.on_quality(1500, 1, t);
    if (c.applied_kbps() >= 60000 && c.state() != fast::fast_state_e::hold) {
      c.on_rtt_severe(t);
    }
    c.tick(t);
  }
  EXPECT_GE(c.cuts(), 3);
  EXPECT_LE(c.cuts(), 6);
  EXPECT_GE(c.ceiling_backoffs(), 2);
  EXPECT_EQ(c.ceiling_expire_s(), 600);
}

// ==============================================================================
// §VR-RATE-ENC（第二輪審查 C11）：半速時控制器的上限夾在編碼器實際送得出的量
// ==============================================================================

// 半速：上限＝設定碼率 ÷ 2，applied 立刻壓到實際送出量（不記塞車點）；之後的降碼每一刀都真的降、塞車點是真的值
TEST_F(VrAbrFastTest, HalfRateCapsAppliedToWhatTheEncoderCanSend) {
  fast::fast_controller_t c(150000);
  c.set_applied_kbps(140000);
  c.tick(t0);
  EXPECT_TRUE(c.set_rate_div(2));
  EXPECT_FALSE(c.set_rate_div(2));
  EXPECT_EQ(c.cap_kbps(), 75000);
  c.set_cap_kbps(150000);  // 呼叫端每一輪都會重設上限：分頻的限制要留著
  EXPECT_EQ(c.cap_kbps(), 75000);
  auto r = c.tick(t0 + 500ms);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 75000);
  EXPECT_STREQ(c.last_reason(), "cap");
  EXPECT_EQ(c.last_congested_kbps(), 0);  // 換分頻不是塞車
  EXPECT_EQ(c.cuts(), 0);
  // 半速期間塞車：從真正送出的 75 Mbps 降
  const auto t1 = t0 + 5000ms;
  ASSERT_EQ(c.on_rtt_severe(t1), fast::fast_controller_t::rtt_severe_e::cut);
  r = c.tick(t1);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 56250);
  EXPECT_EQ(c.last_congested_kbps(), 75000);
  // 單連線上限（75 Mbps）比半速上限低時取較低的
  c.set_cap_kbps(60000);
  EXPECT_EQ(c.cap_kbps(), 60000);
}

// 升回全速：applied 從半速時的實際送出量照 RAMP 往上爬，不會一下翻倍
TEST_F(VrAbrFastTest, FullRateAgainRampsFromHalfRateOutputWithoutJump) {
  fast::fast_controller_t c(150000);
  c.set_applied_kbps(140000);
  c.tick(t0);
  c.set_rate_div(2);
  ASSERT_EQ(c.tick(t0 + 500ms).value_or(0), 75000);
  for (int i = 2; i <= 20; ++i) {  // 半速期間 QP 一直很高也不會爬過 75
    c.on_quality(2500, 1, t0 + i * 500ms);
    c.tick(t0 + i * 500ms);
  }
  ASSERT_EQ(c.applied_kbps(), 75000);
  const auto t1 = t0 + 10000ms;
  EXPECT_TRUE(c.set_rate_div(1));
  EXPECT_EQ(c.cap_kbps(), 150000);
  int prev = c.applied_kbps();
  for (int i = 1; i <= 40; ++i) {  // 每 500 ms 一輪，20 秒
    const auto t = t1 + i * 500ms;
    c.on_quality(2500, 1, t);
    c.tick(t);
    EXPECT_LE(c.applied_kbps(), static_cast<int>(prev * 1.10) + 1);  // 每一步最多約 +8%
    prev = c.applied_kbps();
  }
  EXPECT_GT(c.applied_kbps(), 75000);  // 有在往上爬
}

// quality-trim 收下來的碼率往回補（quality-restore）不受補包量擋：那是回到剛才送得動的碼率
TEST_F(VrAbrFastTest, RepairLoadDoesNotBlockQualityRestore) {
  fast::fast_controller_t c(kConfigured50Mbps);
  auto t = t0;
  for (int i = 0; i <= 300; ++i) {  // 載入畫面：QP 1，30 秒
    t = t0 + i * 100ms;
    if (i % 5 == 0) {
      c.on_quality(100, 1, t);
    }
    c.tick(t);
  }
  ASSERT_LT(c.applied_kbps(), 15000);
  const auto t1 = t;
  for (int i = 1; i <= 100; ++i) {  // 進場景：QP 27，同時補包量 5%
    t = t1 + i * 100ms;
    if (i % 5 == 0) {
      c.on_quality(c.applied_kbps() < 30000 ? 2700 : 1800, 1, t);
      c.on_repair_load(5.0, t);
    }
    c.tick(t);
  }
  EXPECT_TRUE(c.repair_tight(t));
  EXPECT_GE(c.applied_kbps(), 30000);
  EXPECT_LE(c.applied_kbps(), 50000);
  EXPECT_EQ(c.cuts(), 0);
}
