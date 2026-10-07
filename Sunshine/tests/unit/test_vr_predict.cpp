/**
 * @file tests/unit/test_vr_predict.cpp
 * @brief VipleStream 2.0 §VR-PREDICT：依姿態落後調整 SecondsFromVsyncToPhotons 的控制器。
 */
#include "../tests_common.h"

#include <src/vr/vr_predict.h>

namespace predict = vr::predict;
using namespace std::chrono_literals;

namespace {
  constexpr uint32_t kPeriod120 = 8333;  // µs
  constexpr uint32_t kPeriod90 = 11111;  // µs
  constexpr uint16_t kFull = 120;  // 一個 1 s 視窗的樣本數（頭部一直在動）

  int16_t lag_ms(double ms) {
    return static_cast<int16_t>(ms * 10.0);
  }

  struct PredictTest: testing::Test {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    int sec = 0;

    void SetUp() override {
      predict::reset_for_test();
    }

    void TearDown() override {
      predict::reset_for_test();
    }

    /// 每呼叫一次＝下一個 1 s 視窗
    std::optional<predict::result_t> feed(double ms, uint16_t n = kFull) {
      return predict::on_pose_lag(lag_ms(ms), n, t0 + std::chrono::seconds(sec++));
    }

    /// 2026-10-05：每個 session 開頭的暖機視窗（不用）
    void warm() {
      for (int i = 0; i < predict::k_warmup_windows; ++i) {
        EXPECT_FALSE(feed(15));
      }
    }

    /// 探測 +8 ms 並確認採用：回傳探測後的值（含暖機）
    uint32_t probe_honored(uint32_t start) {
      warm();
      EXPECT_FALSE(feed(7));
      EXPECT_FALSE(feed(7));
      auto r = feed(7);
      EXPECT_TRUE(r);
      EXPECT_EQ(r->to_us, start + 8000);
      EXPECT_FALSE(feed(-1));  // settle：改值後 1 s
      EXPECT_FALSE(feed(-1));  // 2 s：開始算 verify 的第 1 個
      EXPECT_FALSE(feed(-1));
      r = feed(-1);
      EXPECT_TRUE(r);
      EXPECT_EQ(r->action, predict::action_e::honored);
      return start + 8000;
    }
  };
}  // namespace

TEST_F(PredictTest, FixedByConfigNeverAdjusts) {
  const auto s = predict::begin_session(kPeriod120, 44000);
  EXPECT_EQ(s.us, 44000u);
  EXPECT_EQ(s.source, predict::start_e::fixed);
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(feed(30));
  }
}

TEST_F(PredictTest, DefaultStartIsPeriodPlus30ms) {
  const auto s = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(s.us, kPeriod120 + predict::k_default_extra_us);
  EXPECT_EQ(s.source, predict::start_e::initial);
}

TEST_F(PredictTest, IgnoresSparseOrMissingSamples) {
  predict::begin_session(kPeriod120, 0);
  for (int i = 0; i < 6; ++i) {
    EXPECT_FALSE(feed(30, 40));  // 樣本 < 60：不算
  }
  for (int i = 0; i < 6; ++i) {
    EXPECT_FALSE(predict::on_pose_lag(predict::k_lag_none, kFull, t0 + std::chrono::seconds(sec++)));
  }
}

TEST_F(PredictTest, ProbeAveragesThreeWindows) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  warm();
  EXPECT_FALSE(feed(6));
  EXPECT_FALSE(feed(8));
  auto r = feed(7);  // 平均 7 ms → 探測：60% 只有 4.2 ms，下限 8 ms
  ASSERT_TRUE(r);
  EXPECT_EQ(r->action, predict::action_e::update);
  EXPECT_FALSE(r->quiet);
  EXPECT_EQ(r->from_us, start);
  EXPECT_EQ(r->to_us, start + 8000);
  EXPECT_EQ(r->lag_us, 7000);
}

TEST_F(PredictTest, NearOptimalSkipsProbe) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  warm();
  EXPECT_FALSE(feed(1));
  EXPECT_FALSE(feed(-1));
  EXPECT_FALSE(feed(0.5));  // 平均 < 1.5 ms：不探測、繼續量
  EXPECT_FALSE(feed(4));
  EXPECT_FALSE(feed(5));
  auto r = feed(4);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->to_us, start + 8000);
}

TEST_F(PredictTest, HonoredProbeThenTracksSmoothly) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  uint32_t cur = probe_honored(start);
  // 追蹤：雜訊 ±5 ms、平均 0 → 每次修正 ≤ 3 ms，值停在探測後附近
  uint32_t lo = cur, hi = cur;
  for (int i = 0; i < 40; ++i) {
    if (auto r = feed(i % 2 ? 5 : -5)) {
      EXPECT_EQ(r->action, predict::action_e::update);
      EXPECT_TRUE(r->quiet);
      EXPECT_LE(std::abs(static_cast<int>(r->to_us) - static_cast<int>(r->from_us)), 3000);
      cur = r->to_us;
      lo = std::min(lo, cur);
      hi = std::max(hi, cur);
    }
  }
  EXPECT_LE(hi - lo, 3000u);
}

TEST_F(PredictTest, TrackingConvergesAndLearns) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  uint32_t cur = probe_honored(start);
  // 真正的最佳值比探測後再多 5 ms：模擬「落後＝最佳值 − 目前值」
  const int64_t optimum = static_cast<int64_t>(cur) + 5000;
  for (int i = 0; i < 30; ++i) {
    if (auto r = feed((optimum - static_cast<int64_t>(cur)) / 1000.0)) {
      cur = r->to_us;
    }
  }
  EXPECT_NEAR(static_cast<double>(cur), static_cast<double>(optimum), 1500.0);
  const auto s2 = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(s2.source, predict::start_e::learned);
  EXPECT_EQ(s2.us, cur);
}

TEST_F(PredictTest, OutlierWindowIsClamped) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  const uint32_t cur = probe_honored(start);
  // 卡頓那一秒冒出 +147 ms（第六輪 a 實例）：先夾到 20 ms，移動平均 0.25×(20−(−1)) → 修正上限 3 ms
  auto r = feed(147);
  ASSERT_TRUE(r);
  EXPECT_LE(static_cast<int>(r->to_us) - static_cast<int>(cur), 3000);
}

TEST_F(PredictTest, SessionDriftIsBounded) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  uint32_t cur = probe_honored(start);
  for (int i = 0; i < 100; ++i) {
    if (auto r = feed(20)) {
      cur = r->to_us;
    }
  }
  EXPECT_EQ(cur, start + 40000);
}

TEST_F(PredictTest, IgnoredProbeStopsAndCarriesToNextSession) {
  // 2026-10-05：判一次「不採用」只停這個 session；下一個 session 再探測一次，連續第二次才只量不探測
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  warm();
  EXPECT_FALSE(feed(7));
  EXPECT_FALSE(feed(7));
  ASSERT_TRUE(feed(7));  // 探測 +8 ms
  EXPECT_FALSE(feed(7));  // settle
  EXPECT_FALSE(feed(6.5));
  EXPECT_FALSE(feed(7));
  auto r = feed(7.5);  // 平均 7：沒動 → 不採用
  ASSERT_TRUE(r);
  EXPECT_EQ(r->action, predict::action_e::not_honored);
  EXPECT_FALSE(r->runtime_ignored);
  EXPECT_EQ(r->to_us, start);  // 退回起始值
  EXPECT_EQ(r->next_session_us, start + 7000);
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(feed(7));  // 本 session 不再調
  }

  // 第二個 session：從推算值開始，仍然探測
  const auto s2 = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(s2.source, predict::start_e::learned);
  EXPECT_EQ(s2.us, start + 7000);
  warm();
  EXPECT_FALSE(feed(3));
  EXPECT_FALSE(feed(3));
  r = feed(3);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->action, predict::action_e::update);  // 再探測 +8 ms
  EXPECT_EQ(r->to_us, s2.us + 8000);
  EXPECT_FALSE(feed(3));  // settle
  EXPECT_FALSE(feed(3));
  EXPECT_FALSE(feed(3));
  r = feed(3);  // 又沒動 → 第二次不採用
  ASSERT_TRUE(r);
  EXPECT_EQ(r->action, predict::action_e::not_honored);
  EXPECT_TRUE(r->runtime_ignored);
  EXPECT_EQ(r->next_session_us, s2.us + 3000);

  // 第三個 session：只量 3 個視窗推算再下一次的起始值（action learned，不必送 driver）
  const auto s3 = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(s3.us, s2.us + 3000);
  warm();
  EXPECT_FALSE(feed(2));
  EXPECT_FALSE(feed(2));
  r = feed(2);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->action, predict::action_e::learned);
  EXPECT_EQ(r->to_us, s3.us);
  EXPECT_EQ(r->next_session_us, s3.us + 2000);
}

TEST_F(PredictTest, WarmupDiscardsFirstWindows) {
  // 第六輪：探測落在遊戲載入、host 掉拍的那幾秒。開頭的視窗一律不用
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  for (int i = 0; i < predict::k_warmup_windows; ++i) {
    EXPECT_FALSE(feed(7));
  }
  EXPECT_FALSE(feed(7));
  EXPECT_FALSE(feed(7));
  auto r = feed(7);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->to_us, start + 8000);
}

TEST_F(PredictTest, UnstableWindowsDoNotProbe) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  warm();
  EXPECT_FALSE(feed(0));
  EXPECT_FALSE(feed(7));
  EXPECT_FALSE(feed(14));  // 最大差 14 ms > 6 ms：不探測、重量
  EXPECT_FALSE(feed(7));
  EXPECT_FALSE(feed(8));
  auto r = feed(6);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->to_us, start + 8000);
}

TEST_F(PredictTest, UnstableVerifyIsInconclusive) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  warm();
  EXPECT_FALSE(feed(7));
  EXPECT_FALSE(feed(7));
  ASSERT_TRUE(feed(7));  // 探測 +8 ms
  EXPECT_FALSE(feed(7));  // settle
  EXPECT_FALSE(feed(0));
  EXPECT_FALSE(feed(12));
  auto r = feed(-4);  // 最大差 16 ms：判不出來
  ASSERT_TRUE(r);
  EXPECT_EQ(r->action, predict::action_e::inconclusive);
  EXPECT_EQ(r->from_us, start + 8000);
  EXPECT_EQ(r->to_us, start);
  EXPECT_EQ(r->spread_us, 16000);
  // 重新量、再探測（不算一次不採用）
  EXPECT_FALSE(feed(7));
  EXPECT_FALSE(feed(7));
  r = feed(7);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->action, predict::action_e::update);
  EXPECT_EQ(r->from_us, start);
  EXPECT_EQ(r->to_us, start + 8000);
}

TEST_F(PredictTest, ConvertMatchesFrameMeasurements) {
  // 10-04：120 Hz 約需 44.6 ms、90 Hz 約需 54.7 ms（F≈14 ms 的由來）
  EXPECT_NEAR(predict::convert_for_period(44600, kPeriod120, kPeriod90), 54800.0, 300.0);
  EXPECT_NEAR(predict::convert_for_period(54700, kPeriod90, kPeriod120), 44500.0, 300.0);
  EXPECT_EQ(predict::convert_for_period(40000, kPeriod90, kPeriod90), 40000u);
}

TEST_F(PredictTest, LearnedValuesArePerRefreshRate) {
  // 120 Hz 學到的值不再原封不動拿去給 90 Hz（第七輪落後 16 ms 的原因）
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  const uint32_t learned120 = probe_honored(start);
  const auto s90 = predict::begin_session(kPeriod90, 0);
  EXPECT_EQ(s90.source, predict::start_e::converted);
  EXPECT_EQ(s90.from_period_us, kPeriod120);
  EXPECT_EQ(s90.us, predict::convert_for_period(learned120, kPeriod120, kPeriod90));
  // 90 Hz 追蹤收斂到另一個值
  warm();
  uint32_t cur90 = s90.us;
  const int64_t optimum90 = static_cast<int64_t>(cur90) + 6000;
  for (int i = 0; i < 40; ++i) {
    if (auto r = feed((optimum90 - static_cast<int64_t>(cur90)) / 1000.0)) {
      cur90 = r->to_us;
    }
  }
  // 兩個更新率各自記住
  const auto back120 = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(back120.source, predict::start_e::learned);
  EXPECT_EQ(back120.us, learned120);
  const auto back90 = predict::begin_session(kPeriod90, 0);
  EXPECT_EQ(back90.source, predict::start_e::learned);
  EXPECT_EQ(back90.us, cur90);
}

TEST_F(PredictTest, HonoredRuntimeSkipsProbeNextSession) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  const uint32_t learned = probe_honored(start);
  const auto s2 = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(s2.us, learned);
  warm();
  // 直接追蹤：暖機後第一個視窗 4 ms → 移動平均 1 ms → 修正 0.5 ms（安靜）
  auto r = feed(4);
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->quiet);
  EXPECT_EQ(r->to_us, learned + 500);
}

TEST_F(PredictTest, RevertWhenNotSentReprobes) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  warm();
  EXPECT_FALSE(feed(7));
  EXPECT_FALSE(feed(7));
  auto r = feed(7);
  ASSERT_TRUE(r);
  predict::revert(*r);  // 探測沒送到 driver：重新量 3 個視窗，再從起始值探測
  EXPECT_FALSE(feed(7));
  EXPECT_FALSE(feed(7));
  r = feed(7);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->from_us, start);
  EXPECT_EQ(r->to_us, start + 8000);
}

TEST_F(PredictTest, SummaryEveryTenSecondsWhileTracking) {
  const uint32_t start = predict::begin_session(kPeriod120, 0).us;
  probe_honored(start);
  EXPECT_FALSE(predict::take_summary(t0 + std::chrono::seconds(sec)));  // 第一次只起算
  const int begin = sec;
  for (int i = 0; i < 10; ++i) {
    feed(3);
  }
  const auto s = predict::take_summary(t0 + std::chrono::seconds(begin + 10));
  ASSERT_TRUE(s);
  EXPECT_EQ(s->windows, 10u);
  EXPECT_GT(s->updates, 0u);
  EXPECT_LE(s->min_us, s->now_us);
  EXPECT_GE(s->max_us, s->now_us);
  EXPECT_FALSE(predict::take_summary(t0 + std::chrono::seconds(begin + 11)));
}
