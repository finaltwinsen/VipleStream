/**
 * @file tests/unit/test_vr_latch.cpp
 * @brief VipleStream 2.0 §VR（M4a R3）：LATCH 頻率鎖控制器與 HAPTIC／CLIENT_TIMING TLV 打包。
 */
#include "../tests_common.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <utility>
#include <src/vr/vr_latch.h>
#include <src/vr/vr_session.h>

using vr::latch::controller_t;
using vr::latch::mode_e;

namespace {
  constexpr uint32_t kPeriod90 = 11'111'111;  // 90 Hz
  constexpr uint32_t kPeriod120 = 8'333'333;  // 120 Hz
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

  // §VR-LATCH-V2：每則都是新幀（frameId 遞增）
  vr::latch::result_t feed_v2(controller_t &c, int32_t slack_us, int n, int64_t &now, uint32_t &frame_id, uint32_t period_ns = kPeriod90) {
    vr::latch::result_t r;
    for (int i = 0; i < n; ++i) {
      now += kStepNs;
      r = c.on_latch(slack_us, period_ns, now, ++frame_id);
    }
    return r;
  }

  struct loop_result_t {
    uint64_t slips_after_settle = 0;  ///< 控制器量到的相位滑移（穩態）
    uint64_t repeats_after_settle = 0;  ///< latch 時沒有新幀（重複幀）
    uint64_t skips_after_settle = 0;  ///< 一個 latch 之間到了兩張以上，舊的被跳過
    int max_abs_ppm_after_settle = 0;
    double mean_slack_after_settle_us = 0;
    double final_err_us = 0;
  };

  /**
   * @brief §VR-LATCH-V2 閉迴路模型（temp/launchertest/analysis/latch_sim.py 的決定性簡化版）：
   *        client 每 T latch 一次，取最新已就緒、還沒顯示過的幀；server 幀間隔 T×(1+(drift+ppm)·1e-6)，
   *        就緒時間＝送出＋30 ms 管線延遲＋±jitter（LCG，決定性），解碼器依序輸出（就緒時間單調）；
   *        client 每 100 ms 送最後一張新幀的 slack 與 frameId，ppm 經 20 ms 才在 server 生效。
   */
  loop_result_t run_loop(controller_t &c, uint32_t period_ns, double drift_ppm, double init_phase_us, double jitter_us, double seconds, double settle_s) {
    const double T = (double) period_ns;
    const double D = 30e6;
    const double J = jitter_us * 1000.0;
    uint32_t lcg = 0x9e3779b9u ^ (uint32_t) (init_phase_us * 7.0 + drift_ppm * 131.0 + 1000.0);
    auto uniform = [&lcg]() {
      lcg = lcg * 1664525u + 1013904223u;
      return (double) (lcg >> 8) / 16777216.0;  // [0, 1)
    };

    std::deque<std::pair<double, int32_t>> pending;  // (生效時間, ppm)
    std::deque<std::pair<double, uint32_t>> ready;  // (就緒時間, frameId)，還沒 latch 的
    int32_t applied = 0;
    double next_send = init_phase_us * 1000.0;
    double last_ready = -1e18;
    uint32_t next_id = 1;
    uint32_t latched_id = 0;
    bool have_slack = false;
    int32_t slack_us = 0;
    double next_latch_send = 0;
    vr::latch::result_t last;
    uint64_t slips_at_settle = 0;
    bool settled = false;
    double slack_sum = 0;
    uint64_t slack_n = 0;
    loop_result_t out;

    const auto latches = (int64_t) (seconds * 1e9 / T);
    for (int64_t k = 1; k < latches; ++k) {
      const double L = (double) k * T;
      const bool in_steady = L >= settle_s * 1e9;
      if (in_steady && !settled) {
        settled = true;
        slips_at_settle = last.slips;
      }
      // 產生可能在 L 之前就緒的幀
      while (next_send + D - J <= L) {
        while (!pending.empty() && pending.front().first <= next_send) {
          applied = pending.front().second;
          pending.pop_front();
        }
        double r = next_send + D + (uniform() * 2.0 - 1.0) * J;
        r = std::max(r, last_ready + 0.5e6);
        last_ready = r;
        ready.emplace_back(r, next_id++);
        next_send += T * (1.0 + (drift_ppm + applied) * 1e-6);
      }
      // latch：最新一張已就緒的幀；更舊的跳過
      int took = 0;
      double took_ready = 0;
      while (!ready.empty() && ready.front().first <= L) {
        took_ready = ready.front().first;
        latched_id = ready.front().second;
        ready.pop_front();
        ++took;
      }
      if (took > 0) {
        slack_us = (int32_t) std::lround((L - took_ready) / 1000.0);
        have_slack = true;
        if (in_steady) {
          out.skips_after_settle += (uint64_t) (took - 1);
          slack_sum += slack_us;
          ++slack_n;
        }
      } else if (in_steady) {
        ++out.repeats_after_settle;
      }
      // 10 Hz LATCH
      if (have_slack && L >= next_latch_send) {
        next_latch_send = L + 100e6;
        last = c.on_latch(slack_us, period_ns, (int64_t) L, latched_id);
        if (last.update) {
          pending.emplace_back(L + 20e6, last.ppm);
          if (in_steady) {
            out.max_abs_ppm_after_settle = std::max(out.max_abs_ppm_after_settle, std::abs(last.ppm));
          }
        }
      }
    }
    out.slips_after_settle = last.slips - slips_at_settle;
    out.mean_slack_after_settle_us = slack_n ? slack_sum / (double) slack_n : 0;
    out.final_err_us = last.err_us;
    return out;
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

// ── §VR-LATCH-V2 ──────────────────────────────────────────────────────────

TEST(VrLatchTest, DefaultIsLegacy) {
  controller_t c;
  EXPECT_EQ(c.mode(), mode_e::legacy);
  int64_t now = 0;
  const auto r = feed(c, 3000, 3, now);
  EXPECT_EQ(r.mode, mode_e::legacy);
  EXPECT_EQ(r.dropped, 0u);
}

TEST(VrLatchTest, V2ConfigureClampsAndSurvivesReset) {
  controller_t c;
  c.configure(mode_e::v2, 10, kPeriod90);  // 低於 25% → 25%
  EXPECT_EQ(c.mode(), mode_e::v2);
  EXPECT_NEAR(c.target_v2_us(), 11111.1 * 0.25, 0.5);
  c.configure(mode_e::v2, 80, kPeriod90);  // 高於 50% → 50%
  EXPECT_NEAR(c.target_v2_us(), 11111.1 * 0.50, 0.5);
  c.configure(mode_e::v2, 40, kPeriod90);
  EXPECT_NEAR(c.target_v2_us(), 4444.4, 0.5);
  c.reset();  // 設定不因 reset 消失
  EXPECT_EQ(c.mode(), mode_e::v2);
  EXPECT_NEAR(c.target_v2_us(), 4444.4, 0.5);
}

TEST(VrLatchTest, V2ModulusIsSmallerOfHostAndDisplayPeriod) {
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod120);  // host 120 Hz、client 顯示 90 Hz
  int64_t now = 0;
  uint32_t id = 0;
  feed_v2(c, 3000, 1, now, id, kPeriod90);
  EXPECT_NEAR(c.modulus_us(), 8333.3, 0.5);
  EXPECT_NEAR(c.target_v2_us(), 3333.3, 0.5);

  controller_t d;
  d.configure(mode_e::v2, 40, 0);  // host 週期未知 → 用顯示週期
  feed_v2(d, 3000, 1, now, id, kPeriod90);
  EXPECT_NEAR(d.modulus_us(), 11111.1, 0.5);
}

TEST(VrLatchTest, V2RepeatedFrameIdIsNotANewMeasurement) {
  // client 沒有新幀時每 100 ms 重送同一個 frameId：只算一次
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod90);
  int64_t now = 0;
  vr::latch::result_t r;
  for (int i = 0; i < 10; ++i) {
    now += kStepNs;
    r = c.on_latch(8000, kPeriod90, now, 5);
  }
  EXPECT_EQ(c.latch_count(), 1u);
  EXPECT_EQ(r.dropped, 9u);
  // legacy 照舊每則都算
  controller_t l;
  now = 0;
  for (int i = 0; i < 10; ++i) {
    now += kStepNs;
    l.on_latch(8000, kPeriod90, now, 5);
  }
  EXPECT_EQ(l.latch_count(), 10u);
}

TEST(VrLatchTest, V2OutlierDropped) {
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod90);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v2(c, 20'000, 1, now, id);  // > 1.5 個週期（16.7 ms）
  EXPECT_EQ(c.latch_count(), 0u);
  EXPECT_EQ(r.dropped, 1u);
  EXPECT_EQ(r.ppm, 0);
  r = feed_v2(c, -20'000, 1, now, id);
  EXPECT_EQ(c.latch_count(), 0u);
  EXPECT_EQ(r.dropped, 2u);
}

TEST(VrLatchTest, V2LateFrameMeasuredModuloPeriodSpeedsUp) {
  // 晚到 500 µs 的幀被下一個 latch 接走，client 量成 T−500。legacy 當成「很早到」而放慢（方向錯），
  // v2 折返成「晚到」而加快。
  const int32_t wrapped = 11111 - 500;
  controller_t l;
  int64_t now = 0;
  auto rl = feed(l, wrapped, 30, now);
  EXPECT_GT(rl.ppm, 0);

  controller_t v;
  v.configure(mode_e::v2, 40, kPeriod90);
  now = 0;
  uint32_t id = 0;
  auto rv = feed_v2(v, wrapped, 30, now, id);
  EXPECT_LT(rv.ppm, 0);
  EXPECT_LT(rv.err_us, -4000.0);  // −500 − 4444
}

TEST(VrLatchTest, V2EarlyAndLateDirections) {
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod90);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v2(c, 8000, 30, now, id);  // e = +3556（< T/2，不折返）→ 太早到 → 放慢
  EXPECT_GT(r.ppm, 0);
  controller_t d;
  d.configure(mode_e::v2, 40, kPeriod90);
  now = 0;
  r = feed_v2(d, 1000, 30, now, id);  // e = −3444 → 太晚 → 加快
  EXPECT_LT(r.ppm, 0);
  controller_t e;
  e.configure(mode_e::v2, 40, kPeriod90);
  now = 0;
  r = feed_v2(e, 4444, 50, now, id);  // 在目標上
  EXPECT_LE(std::abs(r.ppm), 1);
}

TEST(VrLatchTest, V2AntiWindupWhileSaturated) {
  // 誤差 +5000 µs：比例項 600 ppm 早就飽和 → 積分不累加；誤差消失後 ppm 立刻回到 0 附近。
  // legacy 在同樣情況積分會頂到 +150，誤差消失後還要很久才退。
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod90);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v2(c, 4444 + 5000, 600, now, id);  // 60 s
  EXPECT_EQ(r.ppm, vr::latch::VR_LATCH_PPM_MAX);
  EXPECT_NEAR(r.integral_ppm, 0.0, 1e-9);
  r = feed_v2(c, 4444, 30, now, id);  // 3 s 回到目標
  // 剩下的是 EMA 退出飽和後那段誤差面積的正常積分（約 0.008×0.1×Σema ≈ 6.5 ppm）
  EXPECT_LE(std::abs(r.ppm), 10);

  controller_t l;
  now = 0;
  auto rl = feed(l, 2778 + 5000, 600, now);
  EXPECT_NEAR(rl.integral_ppm, controller_t::kIntegralMax, 1e-9);
  rl = feed(l, 2778, 30, now);
  EXPECT_GT(rl.ppm, 100);
}

TEST(VrLatchTest, V2RefreshRateChangeRestarts) {
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod90);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v2(c, 4444 + 1000, 100, now, id);  // 10 s：積分累積
  ASSERT_GT(r.integral_ppm, 5.0);
  r = feed_v2(c, 3333, 1, now, id, kPeriod120);  // 換 120 Hz（模數變 8.33 ms），剛好在新目標上
  EXPECT_NEAR(r.integral_ppm, 0.0, 0.01);  // 重來後只有這一筆的積分
  EXPECT_NEAR(r.err_us, 0.0, 1.0);
  EXPECT_NEAR(r.target_us, 3333.3, 0.5);
}

TEST(VrLatchTest, V2SlipCounting) {
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod90);
  int64_t now = 0;
  uint32_t id = 0;
  feed_v2(c, 500, 5, now, id);
  auto r = feed_v2(c, 10'800, 1, now, id);  // 相位越過 latch 邊緣：跳近一個週期
  EXPECT_EQ(r.slips, 1u);
  r = feed_v2(c, 9000, 1, now, id);  // 小跳動不算
  EXPECT_EQ(r.slips, 1u);
}

TEST(VrLatchTest, V2ClosedLoopLocksFromAnyPhase90Hz) {
  // 25 組初始相位 × 三種時鐘漂移，±600 µs 到達抖動：120 s 之後不得滑移、不得重複幀、ppm 不得頂到上限，
  // 平均 slack 在目標 ±500 µs 內。
  for (const double drift : {-60.0, 0.0, 60.0}) {
    for (int i = 0; i < 25; ++i) {
      controller_t c;
      c.configure(mode_e::v2, 40, kPeriod90);
      const double phase = 11111.1 * i / 25.0;
      const auto res = run_loop(c, kPeriod90, drift, phase, 600.0, 300.0, 120.0);
      SCOPED_TRACE(testing::Message() << "drift=" << drift << " phase=" << phase);
      EXPECT_EQ(res.slips_after_settle, 0u);
      EXPECT_EQ(res.repeats_after_settle, 0u);
      EXPECT_EQ(res.skips_after_settle, 0u);
      EXPECT_LT(res.max_abs_ppm_after_settle, vr::latch::VR_LATCH_PPM_MAX);
      EXPECT_NEAR(res.mean_slack_after_settle_us, 4444.4, 500.0);
    }
  }
}

TEST(VrLatchTest, V2ClosedLoopLocksFromAnyPhase120Hz) {
  for (const double drift : {-60.0, 60.0}) {
    for (int i = 0; i < 25; ++i) {
      controller_t c;
      c.configure(mode_e::v2, 40, kPeriod120);
      const double phase = 8333.3 * i / 25.0;
      const auto res = run_loop(c, kPeriod120, drift, phase, 600.0, 300.0, 120.0);
      SCOPED_TRACE(testing::Message() << "drift=" << drift << " phase=" << phase);
      EXPECT_EQ(res.slips_after_settle, 0u);
      EXPECT_EQ(res.repeats_after_settle, 0u);
      EXPECT_LT(res.max_abs_ppm_after_settle, vr::latch::VR_LATCH_PPM_MAX);
      EXPECT_NEAR(res.mean_slack_after_settle_us, 3333.3, 500.0);
    }
  }
}

TEST(VrLatchTest, TlvLayouts) {
  EXPECT_EQ(sizeof(VIPLE_VR_TLV_LATCH), 12u);
  EXPECT_EQ(sizeof(VIPLE_VR_TLV_HAPTIC), 20u);
  EXPECT_EQ(sizeof(VIPLE_VR_TLV_CLIENT_TIMING), 36u);
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
