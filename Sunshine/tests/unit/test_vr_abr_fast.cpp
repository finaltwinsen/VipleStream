/**
 * @file tests/unit/test_vr_abr_fast.cpp
 * @brief VipleStream 2.0 §VR-ABR-FAST：VR 反應式快速降碼率單元測試。
 */
#include "../tests_common.h"

#include <src/vr/vr_abr_fast.h>

namespace fast = vr::abr;
using namespace std::chrono_literals;

namespace {
  constexpr int kConfigured100Mbps = 100000;
  constexpr int kConfigured20Mbps = 20000;

  struct VrAbrFastTest : testing::Test {
    fast::steady::time_point t0 = fast::steady::now();
  };
}  // namespace

// 1. 單張救不回只打 9 折（中度降碼 ×0.90）
TEST_F(VrAbrFastTest, SingleFecFailedCutsByTenPercent) {
  fast::fast_controller_t c(kConfigured100Mbps);
  EXPECT_EQ(c.state(), fast::fast_state_e::healthy);

  c.on_fec_failed(1, t0);
  EXPECT_EQ(c.state(), fast::fast_state_e::cut);
  EXPECT_STREQ(c.last_reason(), "fec-failed-1");

  auto next = c.tick(t0);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, 90000);  // 100000 * 0.90 = 90000
  EXPECT_EQ(c.applied_kbps(), 90000);
}

// 2. 1 秒內救不回 ≥ 2 張打 75 折（嚴重降碼 ×0.75）
TEST_F(VrAbrFastTest, TwoFecFailedWithinOneSecondCutsByTwentyFivePercent) {
  fast::fast_controller_t c(kConfigured100Mbps);

  // 一次來 2 張救不回
  c.on_fec_failed(2, t0);
  EXPECT_EQ(c.state(), fast::fast_state_e::cut);
  EXPECT_STREQ(c.last_reason(), "fec-failed-ge-2");

  auto next = c.tick(t0);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, 75000);  // 100000 * 0.75 = 75000
}

// 2b. 1 秒內分批達到 2 張救不回亦觸發嚴重降碼
TEST_F(VrAbrFastTest, TwoFecFailedBatchedWithinOneSecond) {
  fast::fast_controller_t c(kConfigured100Mbps);

  c.on_fec_failed(1, t0);
  auto t1 = c.tick(t0);
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(*t1, 90000);  // 第 1 張先打 9 折

  // 200 ms 後來第 2 張（1 秒內累積 2 張）
  c.on_fec_failed(1, t0 + 200ms);
  auto t2 = c.tick(t0 + 200ms);
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(*t2, 67500);  // 90000 * 0.75 = 67500
}

// 3. 同一秒內第二次嚴重降碼打 60 折（×0.60）
TEST_F(VrAbrFastTest, SecondCriticalCutWithinSameSecondCutsToSixtyPercent) {
  fast::fast_controller_t c(kConfigured100Mbps);

  // 第一次嚴重降碼（例如一次 LOSS 回報 lostFrames >= 2）
  c.on_loss(2, t0);
  auto t1 = c.tick(t0);
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(*t1, 75000);  // 100000 * 0.75 = 75000

  // 500 ms 後（在 1 秒視窗內）第二次嚴重降碼（例如連線 dropBlock）
  c.on_drop_block(t0 + 500ms);
  auto t2 = c.tick(t0 + 500ms);
  ASSERT_TRUE(t2.has_value());
  EXPECT_EQ(*t2, 60000);  // 同秒內第二次嚴重：基準 100000 * 0.60 = 60000！
}

// 4. HOLD 期間（2.0 秒冷卻）禁止任何碼率回升
TEST_F(VrAbrFastTest, HoldPeriodProhibitsBitrateRamping) {
  fast::fast_controller_t c(kConfigured100Mbps);

  c.on_loss(2, t0);
  auto t1 = c.tick(t0);
  ASSERT_TRUE(t1.has_value());
  EXPECT_EQ(*t1, 75000);
  EXPECT_EQ(c.state(), fast::fast_state_e::hold);

  // 在 2.0 秒內，tick 不得回升任何碼率
  EXPECT_FALSE(c.tick(t0 + 500ms).has_value());
  EXPECT_FALSE(c.tick(t0 + 1000ms).has_value());
  EXPECT_FALSE(c.tick(t0 + 1500ms).has_value());
  EXPECT_FALSE(c.tick(t0 + 1999ms).has_value());
  EXPECT_EQ(c.state(), fast::fast_state_e::hold);
  EXPECT_EQ(c.current_target_kbps(), 75000);
}

// 5. RAMP 平滑回升：滿 2.0 秒後，每 1.5 秒乾淨就 +4%×上限
TEST_F(VrAbrFastTest, RampEveryOnePointFiveSecondsFourPercent) {
  fast::fast_controller_t c(kConfigured100Mbps);

  c.on_loss(2, t0);
  c.tick(t0);  // target = 75000，進入 hold

  // 滿 2.0 秒（t0 + 2000ms）解除 HOLD，轉入 RAMP
  c.tick(t0 + 2000ms);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // 再過 1.5 秒（t0 + 3500ms）：第一次回升 +4%×100000 = +4000
  auto r1 = c.tick(t0 + 3500ms);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, 79000);  // 75000 + 4000 = 79000
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // 再過 1.5 秒（t0 + 5000ms）：第二次回升 +4000
  auto r2 = c.tick(t0 + 5000ms);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(*r2, 83000);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // 尚未滿 1.5 秒不回升
  EXPECT_FALSE(c.tick(t0 + 5500ms).has_value());
}

// 6. 下限：max(10 Mbps, 設定碼率 × 30%)
TEST_F(VrAbrFastTest, LowerBoundFloorProtection) {
  // 案例 A：設定 20 Mbps，30% 是 6 Mbps < 10 Mbps，下限鎖定 10 Mbps (10000 Kbps)
  {
    fast::fast_controller_t c(kConfigured20Mbps);
    EXPECT_EQ(c.floor_kbps(), 10000);

    // 連續多次嚴重降碼
    c.on_loss(2, t0);
    c.tick(t0);  // 20000 * 0.75 = 15000
    c.on_loss(2, t0 + 2s);
    c.tick(t0 + 2s);  // 15000 * 0.75 = 11250
    c.on_loss(2, t0 + 4s);
    auto next = c.tick(t0 + 4s);  // 11250 * 0.75 = 8437 < 10000 -> 限制在 10000
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 10000);
  }

  // 案例 B：設定 100 Mbps，30% 是 30 Mbps > 10 Mbps，下限為 30 Mbps (30000 Kbps)
  {
    fast::fast_controller_t c(kConfigured100Mbps);
    EXPECT_EQ(c.floor_kbps(), 30000);

    c.on_loss(2, t0);
    c.tick(t0);  // 75000
    c.on_loss(2, t0 + 2s);
    c.tick(t0 + 2s);  // 56250
    c.on_loss(2, t0 + 4s);
    c.tick(t0 + 4s);  // 42187
    c.on_loss(2, t0 + 6s);
    c.tick(t0 + 6s);  // 31640
    c.on_loss(2, t0 + 8s);
    auto next = c.tick(t0 + 8s);  // 31640 * 0.75 = 23730 < 30000 -> 限制在 30000
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 30000);
  }
}

// 7. 防振盪：RAMP 中 5 秒內又降碼 → 上限暫時壓到 applied × 0.85 維持 30 秒
TEST_F(VrAbrFastTest, AntiOscillationPenaltyThirtySeconds) {
  fast::fast_controller_t c(kConfigured100Mbps);

  c.on_loss(2, t0);
  c.tick(t0);  // target = 75000，進入 hold

  // t0 + 2000ms 進入 RAMP
  c.tick(t0 + 2000ms);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // t0 + 3500ms（滿 1.5s）回升到 79000
  auto r1 = c.tick(t0 + 3500ms);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, 79000);

  // 於 t0 + 4000ms（進入 RAMP 後 2 秒，小於 5 秒）遭遇網路阻力再次觸發降碼
  // 此時 applied 為 79000，防振盪上限 penalty_cap = 79000 * 0.85 = 67150
  c.on_loss(2, t0 + 4000ms);
  auto cut1 = c.tick(t0 + 4000ms);
  ASSERT_TRUE(cut1.has_value());
  // 79000 * 0.75 = 59250 (小於 penalty_cap 67150)
  EXPECT_EQ(*cut1, 59250);

  // t0 + 6000ms 解除 HOLD 再次進入 RAMP
  c.tick(t0 + 6000ms);
  EXPECT_EQ(c.state(), fast::fast_state_e::ramp);

  // 期間持續回升，但必須受限於 67150，持續 30 秒（至 t0 + 34000ms）
  fast::steady::time_point t = t0 + 6000ms;
  while (t < t0 + 33000ms) {
    t += 1500ms;
    c.tick(t);
    EXPECT_LE(c.current_target_kbps(), 67150);
  }

  // 超過 30 秒（t0 + 35000ms）後，防振盪懲罰解除，上限恢復為 100000
  EXPECT_EQ(c.effective_cap_kbps(t0 + 35000ms), 100000);
  t = t0 + 35000ms;
  c.tick(t);
  t += 1500ms;
  c.tick(t);
  EXPECT_GT(c.current_target_kbps(), 67150);
}

// 8. 補充測試：中度訊號（LATCH 連續 2 筆負 slack、主連線送達率 < 90%）與嚴重訊號（RTT 膨脹、dropBlock）
TEST_F(VrAbrFastTest, ModerateAndCriticalSignalSources) {
  // LATCH 連續 2 筆 slack < -1000 us 觸發中度降碼
  {
    fast::fast_controller_t c(kConfigured100Mbps);
    c.on_latch_slack(-1200, t0);
    EXPECT_EQ(c.state(), fast::fast_state_e::healthy);  // 第 1 筆不觸發
    c.on_latch_slack(-1500, t0 + 100ms);
    EXPECT_EQ(c.state(), fast::fast_state_e::cut);      // 連續 2 筆觸發中度
    EXPECT_STREQ(c.last_reason(), "latch-slack-late");
    auto next = c.tick(t0 + 100ms);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 90000);
  }

  // 主連線送達率 < 90% 觸發中度降碼
  {
    fast::fast_controller_t c(kConfigured100Mbps);
    c.on_delivery(88.5, t0);
    EXPECT_EQ(c.state(), fast::fast_state_e::cut);
    EXPECT_STREQ(c.last_reason(), "delivery-low");
    auto next = c.tick(t0);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 90000);
  }

  // RTT 超過基準 15 ms 持續 500 ms 觸發嚴重降碼
  {
    fast::fast_controller_t c(kConfigured100Mbps);
    c.on_rtt(10, t0);  // baseline = 10 ms
    c.on_rtt(26, t0 + 100ms);  // 26 >= 10 + 15，開始超標
    EXPECT_EQ(c.state(), fast::fast_state_e::healthy);
    c.on_rtt(28, t0 + 599ms);  // 持續未滿 500 ms（499 ms）
    EXPECT_EQ(c.state(), fast::fast_state_e::healthy);
    c.on_rtt(30, t0 + 601ms);  // 持續滿 501 ms -> 觸發嚴重降碼
    EXPECT_EQ(c.state(), fast::fast_state_e::cut);
    EXPECT_STREQ(c.last_reason(), "rtt-inflation");
    auto next = c.tick(t0 + 601ms);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 75000);
  }
}
