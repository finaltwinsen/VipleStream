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
  constexpr uint32_t kPeriod60 = 16667;  // µs：半速串流（§VR-HALF-RATE，頭盔照樣 120 Hz 顯示）時 host 的週期
  constexpr uint16_t kFull = 120;  // 一個 1 s 視窗的樣本數（頭部一直在動）
  constexpr uint16_t kWorn = 43;  // 2026-10-10 配戴場次：60 Hz 串流每秒 41～45 筆（只算頭部有移動、第一次顯示的幀）

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
    void warm(uint16_t n = kFull) {
      for (int i = 0; i < predict::k_warmup_windows; ++i) {
        EXPECT_FALSE(feed(15, n));
      }
    }

    /// 探測 +8 ms 並確認採用：回傳探測後的值（含暖機）
    uint32_t probe_honored(uint32_t start, uint16_t n = kFull) {
      warm(n);
      EXPECT_FALSE(feed(7, n));
      EXPECT_FALSE(feed(7, n));
      auto r = feed(7, n);
      EXPECT_TRUE(r);
      EXPECT_EQ(r->to_us, start + 8000);
      EXPECT_FALSE(feed(-1, n));  // settle：改值後 1 s
      EXPECT_FALSE(feed(-1, n));  // 2 s：開始算 verify 的第 1 個
      EXPECT_FALSE(feed(-1, n));
      r = feed(-1, n);
      EXPECT_TRUE(r);
      EXPECT_EQ(r->action, predict::action_e::honored);
      return start + 8000;
    }

    /// 同 feed()，落後直接給 0.1 ms 的整數（期望值要逐筆相等的地方用，不經過浮點）
    std::optional<predict::result_t> feed_100us(int16_t lag_100us, uint16_t n = kFull) {
      return predict::on_pose_lag(lag_100us, n, t0 + std::chrono::seconds(sec++));
    }

    /// 模擬「落後＝最佳值 − 目前值」跑幾個視窗（每個視窗 n 筆樣本）：回傳最後的值
    uint32_t follow(int64_t optimum, uint32_t cur, int windows, uint16_t n = kFull) {
      for (int i = 0; i < windows; ++i) {
        if (auto r = feed((optimum - static_cast<int64_t>(cur)) / 1000.0, n)) {
          cur = r->to_us;
        }
      }
      return cur;
    }

    /// 2026-10-10 配戴場次開半速之前的狀態：120 Hz 全速學到 33.7 ms，而且這個行程已確認 SteamVR 採用中途改值
    /// （之後的 session 直接追蹤）。分兩場湊出剛好 33700：第一場的探測沒有效果，下一場從 38333 − 12633＝25700
    /// 開始；第二場 +8 ms 的探測被採用。
    void learn_33700_at_120() {
      predict::begin_session(kPeriod120, 0);
      warm();
      feed_100us(-126);
      feed_100us(-126);
      EXPECT_TRUE(feed_100us(-127));  // 3 個視窗平均 −12.633 ms → 探測 −8 ms
      feed_100us(-126);  // settle
      feed_100us(-126);
      feed_100us(-126);
      const auto r = feed_100us(-127);  // 探測後還是 −12.633 ms → 不採用
      ASSERT_TRUE(r);
      ASSERT_EQ(r->action, predict::action_e::not_honored);
      ASSERT_EQ(r->next_session_us, 25700u);
      ASSERT_EQ(probe_honored(predict::begin_session(kPeriod120, 0).us), 33700u);
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

// ── 2026-10-10：半速串流（§VR-HALF-RATE）──────────────────────────────────────────────────────────────
// 頭盔照樣 120 Hz 顯示，host 的 SteamVR、遊戲、編碼跑 60 Hz：host 的週期加倍、頭盔的顯示週期沒變。

TEST_F(PredictTest, MinSamplesFollowStreamRate) {
  // 串流半秒的幀數，夾在 20～60
  EXPECT_EQ(predict::min_samples_for(kPeriod120), 60);
  EXPECT_EQ(predict::min_samples_for(kPeriod90), 45);
  EXPECT_EQ(predict::min_samples_for(13889), 36);  // 72 Hz
  EXPECT_EQ(predict::min_samples_for(kPeriod60), 30);
  EXPECT_EQ(predict::min_samples_for(16666), 30);  // client 把 8333333 ns 加倍送來的值
  EXPECT_EQ(predict::min_samples_for(6944), 60);  // 144 Hz：上限
  EXPECT_EQ(predict::min_samples_for(40000), 20);  // 下限
  EXPECT_EQ(predict::min_samples_for(0), 60);
}

TEST_F(PredictTest, HalfRateStreamUsesWindowsWithWearingSampleCounts) {
  // 60 Hz 串流每秒最多 60 筆、配戴實測 41～45 筆：門檻固定 60 的時候整場沒有一個視窗被採用、完全不調整
  const uint32_t start = predict::begin_session(kPeriod60, 0, kPeriod120).us;
  for (int i = 0; i < 20; ++i) {
    EXPECT_FALSE(feed(7, 29));  // 不到半秒的幀數（30）：不用，也不算進暖機
  }
  for (int i = 0; i < predict::k_warmup_windows; ++i) {
    EXPECT_FALSE(feed(7, static_cast<uint16_t>(41 + i % 5)));  // 41～45：暖機
  }
  EXPECT_FALSE(feed(7, 41));
  EXPECT_FALSE(feed(7, 45));
  const auto r = feed(7, 43);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->action, predict::action_e::update);
  EXPECT_EQ(r->from_us, start);
  EXPECT_EQ(r->to_us, start + 8000);
}

TEST_F(PredictTest, SampleThresholdIsHalfASecondOfStreamFrames) {
  struct case_t {
    uint32_t period_us;
    uint32_t display_us;
    uint16_t need;
  };
  static const case_t cases[] = {
    {kPeriod120, 0, 60},  // 和 2026-10-10 之前一樣：59 不採用、60 採用
    {6944, 0, 60},  // 144 Hz：上限
    {kPeriod90, 0, 45},  // 90、72 Hz 的全速場次門檻跟著變寬
    {13889, 0, 36},
    {kPeriod60, kPeriod120, 30},  // 半速
    {kPeriod60, 0, 30},  // 不帶 vrDispHz 的舊 client 開半速
  };
  for (const auto &c : cases) {
    SCOPED_TRACE(testing::Message() << "period " << c.period_us << " display " << c.display_us);
    predict::reset_for_test();
    const uint32_t start = predict::begin_session(c.period_us, 0, c.display_us).us;
    for (int i = 0; i < 20; ++i) {
      EXPECT_FALSE(feed(7, static_cast<uint16_t>(c.need - 1)));  // 差一筆：不用，也不算進暖機
    }
    for (int i = 0; i < predict::k_warmup_windows + 2; ++i) {
      EXPECT_FALSE(feed(7, c.need));
    }
    const auto r = feed(7, c.need);  // 暖機 10 個＋3 個視窗 → 探測
    ASSERT_TRUE(r);
    EXPECT_EQ(r->to_us, start + 8000);
  }
}

TEST_F(PredictTest, HalfRateDefaultStartUsesDisplayPeriod) {
  // 沒學過任何值：預設看頭盔的顯示週期（38.3 ms），不是加倍之後的 host 週期（46.7 ms，超前十幾毫秒）
  const auto s = predict::begin_session(kPeriod60, 0, kPeriod120);
  EXPECT_EQ(s.us, kPeriod120 + predict::k_default_extra_us);
  EXPECT_EQ(s.source, predict::start_e::initial);
  // 沒給顯示週期（舊 client），或給的和 host 的週期一樣：照舊
  predict::reset_for_test();
  EXPECT_EQ(predict::begin_session(kPeriod60, 0).us, kPeriod60 + predict::k_default_extra_us);
  predict::reset_for_test();
  EXPECT_EQ(predict::begin_session(kPeriod120, 0, kPeriod120).us, kPeriod120 + predict::k_default_extra_us);
}

TEST_F(PredictTest, HalfRateReusesFullRateValueAndLearnsItsOwn) {
  const uint32_t learned120 = probe_honored(predict::begin_session(kPeriod120, 0).us);
  // 顯示週期明寫成和 host 的一樣＝沒給：還是全速自己的值
  const auto same = predict::begin_session(kPeriod120, 0, kPeriod120);
  EXPECT_EQ(same.source, predict::start_e::learned);
  EXPECT_EQ(same.us, learned120);
  // 半速：顯示更新率相同 → 直接沿用，不換算（換算會變成 14 ms＋(v − 14 ms)×2）
  const auto half = predict::begin_session(kPeriod60, 0, kPeriod120);
  EXPECT_EQ(half.source, predict::start_e::same_display);
  EXPECT_EQ(half.us, learned120);
  EXPECT_EQ(half.from_period_us, kPeriod120);
  EXPECT_EQ(half.from_display_us, kPeriod120);
  // 半速自己收斂到另一個值（少 5 ms）
  warm(kWorn);
  const int64_t optimum_half = static_cast<int64_t>(learned120) - 5000;
  const uint32_t learned_half = follow(optimum_half, half.us, 30, kWorn);
  EXPECT_NEAR(static_cast<double>(learned_half), static_cast<double>(optimum_half), 1500.0);
  // 兩個各自記住：回到全速仍是全速自己的值，再開半速是半速自己的值
  const auto back_full = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(back_full.source, predict::start_e::learned);
  EXPECT_EQ(back_full.us, learned120);
  const auto back_half = predict::begin_session(kPeriod60, 0, kPeriod120);
  EXPECT_EQ(back_half.source, predict::start_e::learned);
  EXPECT_EQ(back_half.us, learned_half);
}

TEST_F(PredictTest, FullRateReusesHalfRateValueWithoutConversion) {
  // 只學過半速的值，再開全速 120 Hz：同樣沿用、不換算
  const uint32_t learned_half = probe_honored(predict::begin_session(kPeriod60, 0, kPeriod120).us, kWorn);
  const auto full = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(full.source, predict::start_e::same_display);
  EXPECT_EQ(full.us, learned_half);
  EXPECT_EQ(full.from_period_us, kPeriod60);
  EXPECT_EQ(full.from_display_us, kPeriod120);
}

TEST_F(PredictTest, HalfRateSourceIsChosenByDisplayRate) {
  // 120 Hz 與 90 Hz 的全速都學過，再開 120 Hz 的半速（host 16667）：host 的週期離 90 Hz 的 11111 比較近，
  // 但要看的是顯示週期 → 沿用 120 Hz 的值
  const uint32_t learned120 = probe_honored(predict::begin_session(kPeriod120, 0).us);
  const uint32_t s90 = predict::begin_session(kPeriod90, 0).us;
  warm();
  ASSERT_NE(follow(static_cast<int64_t>(s90) + 4000, s90, 30), s90);  // 90 Hz 也學到自己的值
  const auto half = predict::begin_session(kPeriod60, 0, kPeriod120);
  EXPECT_EQ(half.source, predict::start_e::same_display);
  EXPECT_EQ(half.us, learned120);
  EXPECT_EQ(half.from_period_us, kPeriod120);
}

TEST_F(PredictTest, ConversionGoesByDisplayRate) {
  // 只學過半速（頭盔 120 Hz、串流 60 Hz），換成 90 Hz 的頭盔全速：比的是顯示週期 8333 → 11111，不是 host 的 16667 → 11111
  const uint32_t learned_half = probe_honored(predict::begin_session(kPeriod60, 0, kPeriod120).us, kWorn);
  const auto s90 = predict::begin_session(kPeriod90, 0);
  EXPECT_EQ(s90.source, predict::start_e::converted);
  EXPECT_EQ(s90.us, predict::convert_for_period(learned_half, kPeriod120, kPeriod90));
  EXPECT_EQ(s90.from_period_us, kPeriod60);
  EXPECT_EQ(s90.from_display_us, kPeriod120);
  // 反過來：只學過 90 Hz 全速，開 120 Hz 的半速 → 11111 → 8333（值變小），不是 → 16667（值變大）
  predict::reset_for_test();
  const uint32_t learned90 = probe_honored(predict::begin_session(kPeriod90, 0).us);
  const auto half = predict::begin_session(kPeriod60, 0, kPeriod120);
  EXPECT_EQ(half.source, predict::start_e::converted);
  EXPECT_EQ(half.us, predict::convert_for_period(learned90, kPeriod90, kPeriod120));
  EXPECT_LT(half.us, learned90);
  EXPECT_EQ(half.from_period_us, kPeriod90);
}

TEST_F(PredictTest, ConversionSourceWithSameDisplayRatePrefersCloserStreamRate) {
  // 120 Hz 的頭盔全速、半速各學到一個值，換到別的顯示更新率時取 host 週期較接近的那一筆——和哪一場先跑無關
  for (const bool half_first : {false, true}) {
    SCOPED_TRACE(half_first ? "half-rate session first" : "full-rate session first");
    predict::reset_for_test();
    uint32_t full = 0;
    uint32_t half = 0;
    if (half_first) {
      half = probe_honored(predict::begin_session(kPeriod60, 0, kPeriod120).us, kWorn);
      const uint32_t s = predict::begin_session(kPeriod120, 0).us;
      warm();
      full = follow(static_cast<int64_t>(s) + 4000, s, 30);
    } else {
      full = probe_honored(predict::begin_session(kPeriod120, 0).us);
      const uint32_t s = predict::begin_session(kPeriod60, 0, kPeriod120).us;
      warm(kWorn);
      half = follow(static_cast<int64_t>(s) - 4000, s, 30, kWorn);
    }
    ASSERT_NE(full, half);
    // 90 Hz 的頭盔全速（host 11111）：離 8333 比離 16667 近 → 全速那一筆
    const auto s90 = predict::begin_session(kPeriod90, 0);
    EXPECT_EQ(s90.source, predict::start_e::converted);
    EXPECT_EQ(s90.from_period_us, kPeriod120);
    EXPECT_EQ(s90.us, predict::convert_for_period(full, kPeriod120, kPeriod90));
    // 90 Hz 的頭盔、host 的週期加倍（22222）：半速那一筆
    const auto s45 = predict::begin_session(2 * kPeriod90, 0, kPeriod90);
    EXPECT_EQ(s45.source, predict::start_e::converted);
    EXPECT_EQ(s45.from_period_us, kPeriod60);
    EXPECT_EQ(s45.us, predict::convert_for_period(half, kPeriod120, kPeriod90));
  }
}

TEST_F(PredictTest, RevertInHalfRateSessionRestoresItsOwnLearnedValue) {
  const uint32_t learned120 = probe_honored(predict::begin_session(kPeriod120, 0).us);
  ASSERT_EQ(predict::begin_session(kPeriod60, 0, kPeriod120).us, learned120);
  warm(kWorn);
  const auto r1 = feed(4, kWorn);  // 移動平均 1 ms → +0.5 ms：半速有了自己的值
  ASSERT_TRUE(r1);
  EXPECT_EQ(r1->to_us, learned120 + 500);
  const auto r2 = feed(8, kWorn);
  ASSERT_TRUE(r2);
  EXPECT_GT(r2->to_us, r1->to_us);
  predict::revert(*r2);  // 沒送到 driver：半速那一筆退回上一步，全速的值不受影響
  EXPECT_EQ(predict::begin_session(kPeriod60, 0, kPeriod120).us, r1->to_us);
  EXPECT_EQ(predict::begin_session(kPeriod120, 0).us, learned120);
}

TEST_F(PredictTest, WearingSessionReplayOldClient) {
  // 重播 2026-10-10 的配戴場次（同一個 server 行程，先全速、再半速）。舊 client 不帶 vrDispHz：host 分不出半速，
  // 把它當成 60 Hz 的頭盔，120 Hz 學到的 33.7 ms 換算成 53.4 ms（那一場的 log 是 33688 → 53376，週期 16666）
  ASSERT_NO_FATAL_FAILURE(learn_33700_at_120());
  const auto s = predict::begin_session(kPeriod60, 0);
  EXPECT_EQ(s.source, predict::start_e::converted);
  EXPECT_EQ(s.from_period_us, kPeriod120);
  EXPECT_EQ(s.us, 53402u);
  // 每秒一個視窗、43 筆樣本；姿態一開始超前 22.8 ms，之後 v2p 每降 1 ms 落後就加 1 ms（最佳值 30.6 ms）。
  // 修正前 43 < 60，整場沒有一個視窗被採用、v2p 停在 53.4 ms
  const int64_t optimum = 30600;
  EXPECT_EQ(follow(optimum, s.us, predict::k_warmup_windows, kWorn), s.us);  // 暖機
  const auto r = feed((optimum - static_cast<int64_t>(s.us)) / 1000.0, kWorn);  // −22.8 ms（先夾到 −20 ms）
  ASSERT_TRUE(r);  // 暖機一過就開始往回收：追蹤階段每秒最多 3 ms
  EXPECT_EQ(r->action, predict::action_e::update);
  EXPECT_TRUE(r->quiet);
  EXPECT_EQ(r->to_us, s.us - 2500);
  uint32_t cur = follow(optimum, r->to_us, 14, kWorn);  // 暖機後 15 個視窗內到位
  EXPECT_NEAR(static_cast<double>(cur), static_cast<double>(optimum), 1500.0);
  cur = follow(optimum, cur, 30, kWorn);  // 之後停在那裡
  EXPECT_NEAR(static_cast<double>(cur), static_cast<double>(optimum), 1500.0);
  // 學到的值存在（16667, 16667）；120 Hz 全速的值沒被動到
  EXPECT_EQ(predict::begin_session(kPeriod60, 0).us, cur);
  EXPECT_EQ(predict::begin_session(kPeriod120, 0).us, 33700u);
  // 之後換成帶 vrDispHz 的 client：（16667, 16667）那一筆算是「60 Hz 的頭盔」的值，不拿來用；沿用 120 Hz 全速的
  const auto upgraded = predict::begin_session(kPeriod60, 0, kPeriod120);
  EXPECT_EQ(upgraded.source, predict::start_e::same_display);
  EXPECT_EQ(upgraded.us, 33700u);
}

TEST_F(PredictTest, WearingSessionReplayWithDisplayRate) {
  // 同一個情境、client 帶了 vrDispHz：直接從 120 Hz 學到的 33.7 ms 開始，離最佳值只有 3.1 ms（不帶時是 22.8 ms）
  ASSERT_NO_FATAL_FAILURE(learn_33700_at_120());
  const auto s = predict::begin_session(kPeriod60, 0, kPeriod120);
  EXPECT_EQ(s.source, predict::start_e::same_display);
  EXPECT_EQ(s.from_period_us, kPeriod120);
  EXPECT_EQ(s.us, 33700u);
  const int64_t optimum = 30600;
  EXPECT_EQ(follow(optimum, s.us, predict::k_warmup_windows, kWorn), s.us);  // 暖機
  uint32_t cur = follow(optimum, s.us, 6, kWorn);  // 暖機後 6 個視窗內到位（不帶 vrDispHz 時要 15 個）
  EXPECT_LT(cur, s.us);
  EXPECT_NEAR(static_cast<double>(cur), static_cast<double>(optimum), 1500.0);
  cur = follow(optimum, cur, 30, kWorn);
  EXPECT_NEAR(static_cast<double>(cur), static_cast<double>(optimum), 1500.0);
  // 半速學到的值另外存；120 Hz 全速的值沒被動到
  const auto again = predict::begin_session(kPeriod60, 0, kPeriod120);
  EXPECT_EQ(again.source, predict::start_e::learned);
  EXPECT_EQ(again.us, cur);
  EXPECT_EQ(predict::begin_session(kPeriod120, 0).us, 33700u);
}

TEST_F(PredictTest, FullRate120TraceIsUnchanged) {
  // 全速 120 Hz、沒有顯示週期的場次：起始值與每一步都要和 2026-10-10 之前相同。to_us 是改之前的實作對同一串
  // 輸入的輸出（0＝那個視窗沒有動作）；以後刻意調整控制器時要重新產生
  const auto s = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(s.source, predict::start_e::initial);
  EXPECT_EQ(s.us, 38333u);
  warm();
  struct step_t {
    int16_t lag_100us;  // 落後（0.1 ms）
    uint32_t to_us;
  };
  static const step_t steps[] = {
    {-41, 0},
    {-50, 0},
    {-44, 30333},  // 3 個視窗平均 −4.5 ms → 探測 −8 ms
    {39, 0},  // settle
    {31, 0},
    {42, 0},
    {36, 30333},  // 採用，之後追蹤
    {33, 32108},
    {21, 33036},
    {4, 0},
    {-8, 0},
    {19, 0},
    {26, 33651},
    {2, 0},
    {-15, 0},
    {-29, 0},
    {-6, 0},
    {11, 0},
    {1470, 36060},  // 卡頓那一秒（+147 ms）
    {9, 37076},
    {-33, 0},
    {-48, 36452},
    {-22, 35943},
    {0, 0},
    {14, 0},
    {55, 36655},
    {61, 37684},
    {-74, 37145},
  };
  for (const auto &st : steps) {
    const auto r = feed_100us(st.lag_100us);
    EXPECT_EQ(r ? r->to_us : 0u, st.to_us) << "lag " << st.lag_100us;
  }
  // 下一場、換更新率時的起始值也一樣
  const auto next = predict::begin_session(kPeriod120, 0);
  EXPECT_EQ(next.source, predict::start_e::learned);
  EXPECT_EQ(next.us, 37145u);
  const auto s90 = predict::begin_session(kPeriod90, 0);
  EXPECT_EQ(s90.source, predict::start_e::converted);
  EXPECT_EQ(s90.from_period_us, kPeriod120);
  EXPECT_EQ(s90.us, 44861u);
  EXPECT_EQ(predict::begin_session(6944, 0).us, 33287u);  // 144 Hz
}
