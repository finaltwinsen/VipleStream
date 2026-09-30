/**
 * @file tests/unit/test_vr_latch.cpp
 * @brief VipleStream 2.0 §VR（M4a R3）：LATCH 頻率鎖控制器與 HAPTIC／CLIENT_TIMING TLV 打包。
 */
#include "../tests_common.h"

#include <cstring>
#include <src/vr/vr_latch.h>
#include <src/vr/vr_session.h>

using vr::latch::controller_t;

namespace {
  constexpr uint32_t kPeriod90 = 11'111'111;  // 90 Hz
  constexpr int64_t kStepNs = 100'000'000;  // LATCH 10 Hz

  // 以固定 slack 餵 n 則，回傳最後一次結果
  vr::latch::result_t feed(controller_t &c, int32_t slack_us, int n, int64_t &now) {
    vr::latch::result_t r;
    for (int i = 0; i < n; ++i) {
      now += kStepNs;
      r = c.on_latch(slack_us, kPeriod90, now);
    }
    return r;
  }
}  // namespace

TEST(VrLatchTest, TargetIsQuarterPeriodClamped) {
  EXPECT_NEAR(controller_t::target_for_period(kPeriod90), 2777.8, 0.5);
  EXPECT_DOUBLE_EQ(controller_t::target_for_period(2'000'000), 1000.0);  // 下限 1 ms
  EXPECT_DOUBLE_EQ(controller_t::target_for_period(40'000'000), 4000.0);  // 上限 4 ms
}

TEST(VrLatchTest, EarlyFramesSlowDownServer) {
  // 幀比目標早到很多 → 白等 → server 放慢（ppm > 0）
  controller_t c;
  int64_t now = 0;
  auto r = feed(c, 8000, 30, now);
  EXPECT_GT(r.ppm, 0);
  EXPECT_LE(r.ppm, vr::latch::VR_LATCH_PPM_MAX);
}

TEST(VrLatchTest, LateFramesSpeedUpServer) {
  // 幀晚到（負 slack）→ server 加快（ppm < 0）
  controller_t c;
  int64_t now = 0;
  auto r = feed(c, -3000, 30, now);
  EXPECT_LT(r.ppm, 0);
  EXPECT_GE(r.ppm, -vr::latch::VR_LATCH_PPM_MAX);
}

TEST(VrLatchTest, OnTargetStaysNearZero) {
  controller_t c;
  int64_t now = 0;
  const int32_t target = (int32_t) controller_t::target_for_period(kPeriod90);
  auto r = feed(c, target, 50, now);
  EXPECT_LE(std::abs(r.ppm), 1);
}

TEST(VrLatchTest, ClampedToPlusMinus200) {
  controller_t c;
  int64_t now = 0;
  // 離群值（遠超過一個週期）夾到 ±1 週期；長時間飽和也不能超過 ±200 ppm
  auto r = feed(c, 1'000'000, 600, now);
  EXPECT_EQ(r.ppm, vr::latch::VR_LATCH_PPM_MAX);
  controller_t c2;
  now = 0;
  r = feed(c2, -1'000'000, 600, now);
  EXPECT_EQ(r.ppm, -vr::latch::VR_LATCH_PPM_MAX);
}

TEST(VrLatchTest, UpdateThrottledButRefreshedEverySecond) {
  controller_t c;
  int64_t now = 0;
  const int32_t target = (int32_t) controller_t::target_for_period(kPeriod90);
  auto r = feed(c, target, 1, now);
  EXPECT_TRUE(r.update);  // 第一次一定送
  int updates = 0;
  for (int i = 0; i < 25; ++i) {  // 2.5 s、穩定在目標
    now += kStepNs;
    if (c.on_latch(target, kPeriod90, now).update) {
      ++updates;
    }
  }
  EXPECT_GE(updates, 2);  // 每 1 s 例行更新
  EXPECT_LE(updates, 3);  // 但不是每則都送
}

TEST(VrLatchTest, StaleDecaysToZero) {
  controller_t c;
  int64_t now = 0;
  auto r = feed(c, 9000, 40, now);
  ASSERT_GT(r.ppm, 0);
  // 3 s 沒有 LATCH：每次 idle 減半，最後回到 0
  now += 3'000'000'000LL;
  int32_t ppm = r.ppm;
  for (int i = 0; i < 12; ++i) {
    now += 1'000'000'000LL;
    auto ri = c.on_idle(now);
    if (ri.update) {
      EXPECT_LE(std::abs(ri.ppm), std::abs(ppm));
      ppm = ri.ppm;
    }
  }
  EXPECT_EQ(ppm, 0);
  EXPECT_EQ(c.last_sent_ppm(), 0);
}

TEST(VrLatchTest, BadPeriodIgnored) {
  controller_t c;
  int64_t now = kStepNs;
  auto r = c.on_latch(0, 0, now);  // period 0 → 沿用預設 90 Hz
  EXPECT_NEAR(r.target_us, 2777.8, 0.5);
  now += kStepNs;
  r = c.on_latch(0, 900'000'000, now);  // 0.9 s 週期不合理 → 沿用
  EXPECT_NEAR(r.target_us, 2777.8, 0.5);
}

TEST(VrLatchTest, TlvLayouts) {
  EXPECT_EQ(sizeof(VIPLE_VR_TLV_LATCH), 12u);
  EXPECT_EQ(sizeof(VIPLE_VR_TLV_HAPTIC), 20u);
  EXPECT_EQ(sizeof(VIPLE_VR_TLV_CLIENT_TIMING), 32u);
}

TEST(VrLatchTest, HapticTlvRoundTrip) {
  VIPLE_VR_TLV_HAPTIC h {};
  h.device = VIPLE_VR_POSE_RIGHT;
  h.durationUs = 25000;
  h.frequencyHz = 160.0f;
  h.amplitude = 0.75f;
  h.eventId = 7;
  const auto tlv = vr::session_state_t::make_tlv(VIPLE_VR_S2C_HAPTIC, &h, sizeof(h));
  ASSERT_EQ(tlv.size(), 2u + sizeof(h));
  EXPECT_EQ(tlv[0], VIPLE_VR_S2C_HAPTIC);
  EXPECT_EQ(tlv[1], sizeof(h));
  VIPLE_VR_TLV_HAPTIC back {};
  std::memcpy(&back, tlv.data() + 2, sizeof(back));
  EXPECT_EQ(back.device, VIPLE_VR_POSE_RIGHT);
  EXPECT_EQ(back.durationUs, 25000u);
  EXPECT_FLOAT_EQ(back.frequencyHz, 160.0f);
  EXPECT_FLOAT_EQ(back.amplitude, 0.75f);
  EXPECT_EQ(back.eventId, 7u);
}
