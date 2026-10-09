/**
 * @file tests/unit/test_vr_latch.cpp
 * @brief VipleStream 2.0 §VR（M4a R3）：LATCH 頻率鎖控制器與 HAPTIC／CLIENT_TIMING TLV 打包。
 */
#include "../tests_common.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <random>
#include <utility>
#include <vector>
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
    // §VR-LATCH-V3
    double gate_open_at_s = -1;  ///< 控制器第一次回報閘門開著的時刻（沒開過＝−1）
    vr::latch::result_t last;  ///< 最後一則 LATCH 的結果
    std::vector<std::pair<double, double>> phase_err;  ///< （latch 時刻 s，真正的相位誤差 µs）：不含抖動的到達時刻對目標，取最短路徑
    std::vector<std::pair<double, vr::latch::result_t>> steps;  ///< （時刻 s，結果）：每一次 burst
  };

  /// §VR-LATCH-V3：閉迴路模型的額外事件
  struct loop_opts_t {
    double shift_at_s = -1;  ///< 這個時刻起送出的幀，到達時刻一律多 shift_us（host 管線變長）；< 0＝沒有
    double shift_us = 0;  ///< 只支援 ≥ 0
    double freeze_at_s = -1;  ///< 這個時刻起 freeze_s 秒內送出的幀到不了 client（影像停住；LATCH 照送同一個 frameId）；< 0＝沒有
    double freeze_s = 0;
    bool trace = false;  ///< 記下每一個 latch 的真正相位誤差（loop_result_t::phase_err）
    double trace_target_us = 0;  ///< trace 用的目標 slack
  };

  /// 折到 [−period/2, period/2)
  double wrap_half(double x, double period) {
    x = std::fmod(x + period / 2, period);
    if (x < 0) {
      x += period;
    }
    return x - period / 2;
  }

  /**
   * @brief §VR-LATCH-V2 閉迴路模型（temp/launchertest/analysis/latch_sim.py 的決定性簡化版）：
   *        client 每 T latch 一次，取最新已就緒、還沒顯示過的幀；server 幀間隔 T×(1+(drift+ppm)·1e-6)，
   *        就緒時間＝送出＋30 ms 管線延遲＋±jitter（LCG，決定性），解碼器依序輸出（就緒時間單調）；
   *        client 每 100 ms 送最後一張新幀的 slack 與 frameId，ppm 經 20 ms 才在 server 生效。
   *        §VR-LATCH-V3：結果帶 burst 時，burst_ppm 同樣經 20 ms 生效、burst_us 之後改回基準（bridge 的做法）；
   *        opts 可以加一次到達時刻的平移，或一段影像停住的時間（幀到不了 client，LATCH 照送同一個 frameId）。
   *        沒有 burst、沒有平移、沒有停住時的數值與原本完全相同。
   */
  loop_result_t run_loop(controller_t &c, uint32_t period_ns, double drift_ppm, double init_phase_us, double jitter_us, double seconds, double settle_s, const loop_opts_t &opts = {}) {
    const double T = (double) period_ns;
    const double D = 30e6;
    const double J = jitter_us * 1000.0;
    // 種子的算式可能是負值（drift −60、相位小於約 980 µs 時是 −6860 起跳）：負的 double 直接轉 uint32_t 是未定義行為，
    // 先轉 int64_t 再取低 32 位元才有定義。x86-64 上兩種寫法得到的值相同（回繞），既有測試的抖動序列不變。
    uint32_t lcg = 0x9e3779b9u ^ (uint32_t) (int64_t) (init_phase_us * 7.0 + drift_ppm * 131.0 + 1000.0);
    auto uniform = [&lcg]() {
      lcg = lcg * 1664525u + 1013904223u;
      return (double) (lcg >> 8) / 16777216.0;  // [0, 1)
    };

    std::deque<std::pair<double, int32_t>> pending;  // (生效時間, ppm)
    std::deque<std::pair<double, uint32_t>> ready;  // (就緒時間, frameId)，還沒 latch 的
    std::deque<double> ready_nominal;  // 與 ready 一一對應：不含抖動的就緒時間
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
        // 影像停住的時段：host 的節拍照走，但這段時間送出的幀到不了 client（不發布、frameId 不前進）
        const bool frozen = opts.freeze_at_s >= 0 && next_send >= opts.freeze_at_s * 1e9 && next_send < (opts.freeze_at_s + opts.freeze_s) * 1e9;
        if (!frozen) {
          // 到達時刻的平移（沒有時 shift 是 0.0，nominal 與原本的 next_send + D 是同一個值）
          const double shift = opts.shift_at_s >= 0 && next_send >= opts.shift_at_s * 1e9 ? opts.shift_us * 1000.0 : 0.0;
          const double nominal = next_send + D + shift;
          double r = nominal + (uniform() * 2.0 - 1.0) * J;
          r = std::max(r, last_ready + 0.5e6);
          last_ready = r;
          ready.emplace_back(r, next_id++);
          ready_nominal.push_back(nominal);
        }
        next_send += T * (1.0 + (drift_ppm + applied) * 1e-6);
      }
      // latch：最新一張已就緒的幀；更舊的跳過
      int took = 0;
      double took_ready = 0;
      double took_nominal = 0;
      while (!ready.empty() && ready.front().first <= L) {
        took_ready = ready.front().first;
        latched_id = ready.front().second;
        took_nominal = ready_nominal.front();
        ready.pop_front();
        ready_nominal.pop_front();
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
        if (opts.trace) {
          out.phase_err.emplace_back(L / 1e9, wrap_half((L - took_nominal) / 1000.0 - opts.trace_target_us, T / 1000.0));
        }
      } else if (in_steady) {
        ++out.repeats_after_settle;
      }
      // 10 Hz LATCH
      if (have_slack && L >= next_latch_send) {
        next_latch_send = L + 100e6;
        last = c.on_latch(slack_us, period_ns, (int64_t) L, latched_id);
        if (out.gate_open_at_s < 0 && last.gate_open) {
          out.gate_open_at_s = L / 1e9;
        }
        if (last.update) {
          if (last.burst_us > 0) {
            // §VR-LATCH-V3：burst 起、burst 止兩筆（控制器在盲區內不會再更新，佇列維持時間順序）
            pending.emplace_back(L + 20e6, last.burst_ppm);
            pending.emplace_back(L + 20e6 + (double) last.burst_us * 1000.0, last.ppm);
            out.steps.emplace_back(L / 1e9, last);
          } else {
            pending.emplace_back(L + 20e6, last.ppm);
          }
          if (in_steady) {
            out.max_abs_ppm_after_settle = std::max(out.max_abs_ppm_after_settle, std::abs(last.ppm));
          }
        }
      }
    }
    out.slips_after_settle = last.slips - slips_at_settle;
    out.mean_slack_after_settle_us = slack_n ? slack_sum / (double) slack_n : 0;
    out.final_err_us = last.err_us;
    out.last = last;
    return out;
  }

  // ── §VR-LATCH-V3 的輔助 ──

  /// 每 100 ms 一則、每則都是新幀，而且 frameId 以這個更新率該有的速度前進（閘門會開）
  vr::latch::result_t feed_v3(controller_t &c, int32_t slack_us, int n, int64_t &now, uint32_t &frame_id, uint32_t period_ns = kPeriod120) {
    const auto per_latch = (uint32_t) (kStepNs / period_ns);  // 120 Hz：12 張；90 Hz：9 張
    vr::latch::result_t r;
    for (int i = 0; i < n; ++i) {
      now += kStepNs;
      frame_id += per_latch;
      r = c.on_latch(slack_us, period_ns, now, frame_id);
    }
    return r;
  }

  constexpr double kT120 = 8333.333;  // 120 Hz 的週期（µs）
  constexpr double kT90 = 11111.111;

  /// 取模後落在 0～T 的 slack（整數 µs），讓誤差（最短路徑）等於 err_us
  int32_t slack_for_err(double target_us, double err_us, double period_us) {
    double s = std::fmod(target_us + err_us, period_us);
    if (s < 0) {
      s += period_us;
    }
    return (int32_t) std::lround(s);
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

// ── §VR-LATCH-V3 ──────────────────────────────────────────────────────────

TEST(VrLatchTest, V3DefaultUnchanged) {
  // 沒設定＝legacy；v3 的欄位在 legacy／v2 一律是初值，帶不帶 options 對 v2 沒有差別
  controller_t l;
  int64_t now = 0;
  auto r = feed(l, 3000, 3, now);
  EXPECT_EQ(r.mode, mode_e::legacy);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  EXPECT_FALSE(r.gate_open);
  EXPECT_EQ(r.steps, 0u);
  EXPECT_EQ(r.burst_us, 0u);
  EXPECT_FALSE(r.eval_valid);
  EXPECT_FALSE(l.unsettled(now));
  EXPECT_FALSE(l.learned(now).has_value());

  controller_t a;
  controller_t b;
  a.configure(mode_e::v2, 40, kPeriod90);
  vr::latch::options_t opt;
  opt.acquire = true;  // v2 不看
  opt.have_seed = true;
  opt.seed_integral_ppm = -46.0;
  b.configure(mode_e::v2, 40, kPeriod90, opt);
  int64_t na = 0;
  int64_t nb = 0;
  uint32_t ia = 0;
  uint32_t ib = 0;
  for (const int32_t slack : {8000, 1000, 10611, 4444, 300, 6000}) {
    const auto ra = feed_v2(a, slack, 20, na, ia);
    const auto rb = feed_v2(b, slack, 20, nb, ib);
    EXPECT_EQ(ra.ppm, rb.ppm);
    EXPECT_DOUBLE_EQ(ra.integral_ppm, rb.integral_ppm);
    EXPECT_DOUBLE_EQ(ra.err_us, rb.err_us);
    EXPECT_EQ(rb.burst_us, 0u);
    EXPECT_EQ(rb.phase, vr::latch::kPhaseWait);
  }
  EXPECT_FALSE(b.unsettled(nb));

  // v3 的目標上限是 65%，v2 仍是 50%
  controller_t c;
  c.configure(mode_e::v3, 80, kPeriod120);
  EXPECT_NEAR(c.target_v2_us(), kT120 * 0.65, 0.5);
  c.configure(mode_e::v3, 10, kPeriod120);
  EXPECT_NEAR(c.target_v2_us(), kT120 * 0.25, 0.5);
  c.configure(mode_e::v2, 80, kPeriod120);
  EXPECT_NEAR(c.target_v2_us(), kT120 * 0.50, 0.5);
}

TEST(VrLatchTest, V3CircMedianIgnoresWrapAndNearMiss) {
  // 5 筆在目標上，加一筆晚到被下一個 latch 接走的（T−500）與一筆剛好趕上的（300 µs）：中位數不動
  const double target = kT120 * 0.4;
  const double xs[] = {target, target, target, kT120 - 500.0, target, 300.0, target};
  auto cs = vr::latch::circ_median(xs, 7, kT120);
  EXPECT_NEAR(cs.center_us, target, 1e-6);
  EXPECT_NEAR(cs.mad_us, 0.0, 1e-6);

  // 樣本落在 0／T 邊緣兩側：線性的中位數會是 100，環狀的在邊緣上
  const double edge[] = {kT120 - 100.0, 50.0, kT120 - 50.0, 100.0, 0.0};
  cs = vr::latch::circ_median(edge, 5, kT120);
  EXPECT_LT(std::min(cs.center_us, kT120 - cs.center_us), 1.0);
  EXPECT_NEAR(cs.mad_us, 50.0, 1e-6);

  // 偶數筆：中間兩筆的平均
  const double even[] = {1000.0, 1200.0, 1400.0, 1600.0};
  cs = vr::latch::circ_median(even, 4, kT120);
  EXPECT_NEAR(cs.center_us, 1300.0, 1e-6);
  EXPECT_EQ(vr::latch::circ_median(nullptr, 0, kT120).center_us, 0.0);

  // 經過控制器：追蹤中混進這兩筆，誤差不動
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  const auto on_target = (int32_t) std::lround(target);
  auto r = feed_v3(c, on_target, 15, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  r = feed_v3(c, (int32_t) (kT120 - 500.0), 1, now, id);
  EXPECT_LT(std::abs(r.err_us), 50.0);
  r = feed_v3(c, 300, 1, now, id);
  EXPECT_LT(std::abs(r.err_us), 50.0);
  EXPECT_LE(std::abs(r.ppm), 6);
  r = feed_v3(c, on_target, 5, now, id);
  EXPECT_LT(std::abs(r.err_us), 50.0);
}

TEST(VrLatchTest, V3ErrorIsShortestPath) {
  // 目標 65%（5417 µs）、slack 300 µs：線性看是「晚了 5117」，最短路徑是「早 3217」（往晚的方向繞過 latch 邊緣比較近）
  controller_t c;
  c.configure(mode_e::v3, 65, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, 300, 12, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_NEAR(r.err_us, 300.0 - kT120 * 0.65 + kT120, 1.0);
  EXPECT_GT(r.ppm, 0);
  EXPECT_NEAR(r.target_us, kT120 * 0.65, 0.5);
  EXPECT_NEAR(r.slack_ema_us, r.target_us + r.err_us, 1e-9);

  // 目標 40%、晚到 500 µs 的幀量成 T−500：最短路徑是「晚 3833」
  controller_t d;
  d.configure(mode_e::v3, 40, kPeriod120);
  now = 0;
  r = feed_v3(d, (int32_t) (kT120 - 500.0), 12, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_NEAR(r.err_us, -500.0 - kT120 * 0.4, 1.5);
  EXPECT_LT(r.ppm, 0);

  // 負的 slack（協定上的「晚到」）也折到同一個相位
  controller_t e;
  e.configure(mode_e::v3, 40, kPeriod120);
  now = 0;
  r = feed_v3(e, -500, 12, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_NEAR(r.err_us, -500.0 - kT120 * 0.4, 1.5);
}

TEST(VrLatchTest, V3GateBlocksBlackFrames) {
  // host 沒有畫面時每 100 ms 一張黑幀：frameId 每則 LATCH 只前進 1。閘門不開、不取樣本，輸出只有積分（這裡是起點的 −46）
  controller_t c;
  vr::latch::options_t opt;
  opt.have_seed = true;
  opt.seed_integral_ppm = -46.0;
  c.configure(mode_e::v3, 40, kPeriod120, opt);
  int64_t now = 0;
  uint32_t id = 0;
  for (int i = 0; i < 40; ++i) {
    now += kStepNs;
    const auto r = c.on_latch(7000, kPeriod120, now, ++id);
    EXPECT_FALSE(r.gate_open);
    EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
    EXPECT_EQ(r.ppm, -46);
    EXPECT_DOUBLE_EQ(r.integral_ppm, -46.0);
    if (i == 0) {
      EXPECT_TRUE(r.update);  // 第一則就把積分送出去
    }
  }
  EXPECT_TRUE(c.unsettled(now));

  // 正常出幀之後：最近 5 個間隔的張數過七成（第 4 個正常間隔：1＋4×12＝49 ≥ 42）閘門才開，再收滿 5 筆樣本才開始追蹤
  auto r = feed_v3(c, 7000, 3, now, id);
  EXPECT_FALSE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  r = feed_v3(c, 7000, 1, now, id);
  EXPECT_TRUE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  EXPECT_EQ(r.ppm, -46);
  r = feed_v3(c, 7000, 3, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  EXPECT_EQ(r.ppm, -46);
  r = feed_v3(c, 7000, 1, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_GT(r.ppm, 100);  // 誤差 +3667 µs：比例項進來了

  // 半速串流（host 的幀週期是顯示週期的兩倍）：每 100 ms 前進 6 張就是全速
  controller_t h;
  h.configure(mode_e::v3, 40, 2 * kPeriod120);
  now = 0;
  id = 0;
  for (int i = 0; i < 12; ++i) {
    now += kStepNs;
    id += 6;
    r = h.on_latch(3333, kPeriod120, now, id);
  }
  EXPECT_TRUE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_NEAR(h.modulus_us(), kT120, 0.5);  // 模數仍是顯示週期
}

TEST(VrLatchTest, V3GateSurvivesOneHitch) {
  for (const uint32_t period : {kPeriod120, kPeriod90}) {
    SCOPED_TRACE(testing::Message() << "period=" << period);
    const auto per_latch = (uint32_t) (kStepNs / period);
    const auto on_target = (int32_t) std::lround(period / 1000.0 * 0.4);
    controller_t c;
    c.configure(mode_e::v3, 40, period);
    int64_t now = 0;
    uint32_t id = 0;
    auto r = feed_v3(c, on_target, 20, now, id, period);
    ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);

    // 一個間隔少 10 張（120 Hz：0.5 s 內 60 張少 10 張；90 Hz 一個間隔只有 9 張，少 8 張）：閘門仍開、繼續追蹤
    now += kStepNs;
    id += per_latch > 10 ? per_latch - 10 : 1;
    r = c.on_latch(on_target, period, now, id);
    EXPECT_TRUE(r.gate_open);
    EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
    r = feed_v3(c, on_target, 6, now, id, period);
    EXPECT_TRUE(r.gate_open);
    EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);

    // 連續三個間隔都只前進 1 張（host 掉拍）：閘門關、回到等待，輸出只剩積分
    for (int i = 0; i < 3; ++i) {
      now += kStepNs;
      r = c.on_latch(on_target, period, now, ++id);
    }
    EXPECT_FALSE(r.gate_open);
    EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
    EXPECT_EQ(r.ppm, (int32_t) std::lround(r.integral_ppm));

    // 一個間隔超過 300 ms（LATCH 中斷）：就算張數夠也關
    r = feed_v3(c, on_target, 12, now, id, period);
    ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
    now += 4 * kStepNs;
    id += 4 * per_latch;
    r = c.on_latch(on_target, period, now, id);
    EXPECT_FALSE(r.gate_open);
    EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  }
}

TEST(VrLatchTest, V3IdleKeepsIntegral) {
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  const auto slack = (int32_t) std::lround(kT120 * 0.4 + 800.0);
  auto r = feed_v3(c, slack, 60, now, id);  // 誤差 +800 µs 約 5 s：積分累積
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  const double integ = r.integral_ppm;
  ASSERT_GT(integ, 20.0);
  ASSERT_GT(r.ppm, (int32_t) std::lround(integ) + 50);  // 比例項約 96 ppm

  // 不到 2 s：不動
  now += 1'500'000'000LL;
  auto ri = c.on_idle(now);
  EXPECT_FALSE(ri.update);
  EXPECT_EQ(ri.ppm, r.ppm);

  // LATCH 停超過 2 s：比例項歸零、積分原封不動，回到等待
  now += 1'000'000'000LL;
  ri = c.on_idle(now);
  EXPECT_TRUE(ri.update);
  EXPECT_EQ(ri.ppm, (int32_t) std::lround(integ));
  EXPECT_DOUBLE_EQ(ri.integral_ppm, integ);
  EXPECT_EQ(ri.phase, vr::latch::kPhaseWait);
  EXPECT_FALSE(ri.gate_open);
  EXPECT_TRUE(c.unsettled(now));

  // 停到 59 s：積分還在
  for (int i = 0; i < 56; ++i) {
    now += 1'000'000'000LL;
    ri = c.on_idle(now);
    EXPECT_DOUBLE_EQ(ri.integral_ppm, integ);
    EXPECT_EQ(ri.ppm, (int32_t) std::lround(integ));
  }
  // 停滿 60 s 之後每次減半，最後回到 0
  now += 2'000'000'000LL;
  ri = c.on_idle(now);
  EXPECT_DOUBLE_EQ(ri.integral_ppm, integ / 2);
  now += 1'000'000'000LL;
  ri = c.on_idle(now);
  EXPECT_DOUBLE_EQ(ri.integral_ppm, integ / 4);
  for (int i = 0; i < 20; ++i) {
    now += 1'000'000'000LL;
    ri = c.on_idle(now);
  }
  EXPECT_EQ(ri.integral_ppm, 0.0);
  EXPECT_EQ(c.last_sent_ppm(), 0);

  // LATCH 回來：閘門要重新收滿 5 個間隔，而且第一個間隔就是那段空檔（> 300 ms），要等它排出去
  r = feed_v3(c, slack, 5, now, id);
  EXPECT_FALSE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  r = feed_v3(c, slack, 1, now, id);
  EXPECT_TRUE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);

  // v2 的 idle 照舊：2 s 之後每次減半（積分也是）
  controller_t v;
  v.configure(mode_e::v2, 40, kPeriod120);
  now = 0;
  id = 0;
  auto rv = feed_v2(v, slack, 60, now, id, kPeriod120);
  ASSERT_GT(rv.integral_ppm, 20.0);
  const int32_t sent = v.last_sent_ppm();
  now += 3'000'000'000LL;
  const auto rvi = v.on_idle(now);
  EXPECT_DOUBLE_EQ(rvi.integral_ppm, rv.integral_ppm / 2);
  EXPECT_EQ(rvi.ppm, sent / 2);
}

TEST(VrLatchTest, V3SeedIntegral) {
  vr::latch::options_t opt;
  opt.have_seed = true;
  opt.seed_integral_ppm = -46.2;
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120, opt);
  int64_t now = kStepNs;
  // 第一則 LATCH（顯示週期由預設的 90 Hz 換成 120 Hz）不可以把起點清掉
  auto r = c.on_latch(3333, kPeriod120, now, 1);
  EXPECT_TRUE(r.update);
  EXPECT_EQ(r.ppm, -46);
  EXPECT_DOUBLE_EQ(r.integral_ppm, -46.2);
  c.reset();  // 回到剛設定完的狀態
  EXPECT_EQ(c.mode(), mode_e::v3);
  r = c.on_latch(3333, kPeriod120, now, 1);
  EXPECT_DOUBLE_EQ(r.integral_ppm, -46.2);

  // 夾在積分上限內；不是有限值就當成沒有
  opt.seed_integral_ppm = -500.0;
  controller_t d;
  d.configure(mode_e::v3, 40, kPeriod120, opt);
  EXPECT_DOUBLE_EQ(d.on_latch(3333, kPeriod120, now, 1).integral_ppm, -controller_t::kIntegralMax);
  opt.seed_integral_ppm = std::nan("");
  controller_t n;
  n.configure(mode_e::v3, 40, kPeriod120, opt);
  EXPECT_EQ(n.on_latch(3333, kPeriod120, now, 1).integral_ppm, 0.0);

  // legacy／v2 不用起點
  opt.seed_integral_ppm = -46.2;
  controller_t v;
  v.configure(mode_e::v2, 40, kPeriod120, opt);
  uint32_t id = 0;
  EXPECT_NEAR(feed_v2(v, 3333, 1, now, id, kPeriod120).integral_ppm, 0.0, 0.01);

  // 追蹤中換更新率（120 → 90 Hz）：和 v2 一樣重來，積分歸零、回到等待
  controller_t s;
  opt.seed_integral_ppm = 30.0;
  s.configure(mode_e::v3, 40, kPeriod120, opt);
  now = 0;
  id = 0;
  r = feed_v3(s, 3333, 15, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  ASSERT_GT(r.integral_ppm, 25.0);
  r = feed_v3(s, 4444, 1, now, id, kPeriod90);
  EXPECT_EQ(r.integral_ppm, 0.0);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  EXPECT_FALSE(r.gate_open);
  EXPECT_EQ(r.ppm, 0);
}

TEST(VrLatchTest, V3LearnedRoundTrip) {
  vr::latch::reset_learned_for_test();
  EXPECT_FALSE(vr::latch::recall(kPeriod120).has_value());
  vr::latch::remember(kPeriod120, -46.2);
  ASSERT_TRUE(vr::latch::recall(kPeriod120).has_value());
  EXPECT_DOUBLE_EQ(*vr::latch::recall(kPeriod120), -46.2);
  EXPECT_DOUBLE_EQ(*vr::latch::recall(kPeriod120 + 100'000), -46.2);  // 差 1.2%：同一個
  EXPECT_FALSE(vr::latch::recall(kPeriod90).has_value());  // 差 33%：另一個
  EXPECT_FALSE(vr::latch::recall(0).has_value());
  vr::latch::remember(kPeriod90, 12.0);
  vr::latch::remember(kPeriod120 - 50'000, -40.0);  // 蓋掉同一個週期的值
  EXPECT_DOUBLE_EQ(*vr::latch::recall(kPeriod120), -40.0);
  EXPECT_DOUBLE_EQ(*vr::latch::recall(kPeriod90), 12.0);
  vr::latch::remember(kPeriod120, 1000.0);  // 夾在積分上限
  EXPECT_DOUBLE_EQ(*vr::latch::recall(kPeriod120), controller_t::kIntegralMax);
  vr::latch::remember(0, 5.0);  // 週期不知道、數值不是有限值：忽略
  vr::latch::remember(kPeriod90, std::nan(""));
  EXPECT_DOUBLE_EQ(*vr::latch::recall(kPeriod90), 12.0);
  vr::latch::reset_learned_for_test();
  EXPECT_FALSE(vr::latch::recall(kPeriod120).has_value());

  // 控制器：連續追蹤滿 60 s 才交出積分的移動平均
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  const auto slack = (int32_t) std::lround(kT120 * 0.4 + 100.0);
  auto r = feed_v3(c, slack, 10, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  const int64_t track_since = now;
  EXPECT_FALSE(c.learned(now).has_value());
  r = feed_v3(c, slack, 599, now, id);  // 追蹤 59.9 s
  EXPECT_FALSE(c.learned(now).has_value());
  r = feed_v3(c, slack, 1, now, id);
  ASSERT_EQ(now - track_since, 60'000'000'000LL);
  const auto learned = c.learned(now);
  ASSERT_TRUE(learned.has_value());
  EXPECT_GT(*learned, 30.0);  // 誤差 +100 µs：積分每秒約 0.8 ppm 往上，移動平均（時間常數約 10 s）跟在後面
  EXPECT_LT(*learned, r.integral_ppm);
  EXPECT_NEAR(*learned, r.integral_ppm - 8.0, 2.0);

  // 積分的平均超過 120 ppm：不像時鐘差，不交
  r = feed_v3(c, (int32_t) std::lround(kT120 * 0.4 + 600.0), 600, now, id);
  ASSERT_GT(r.integral_ppm, 125.0);
  EXPECT_FALSE(c.learned(now).has_value());

  // 離開追蹤就沒有
  controller_t d;
  d.configure(mode_e::v3, 40, kPeriod120);
  now = 0;
  id = 0;
  r = feed_v3(d, slack, 700, now, id);
  ASSERT_TRUE(d.learned(now).has_value());
  now += 3'000'000'000LL;
  d.on_idle(now);
  EXPECT_FALSE(d.learned(now).has_value());

  // v2 沒有
  controller_t v;
  v.configure(mode_e::v2, 40, kPeriod120);
  now = 0;
  id = 0;
  feed_v2(v, slack, 700, now, id, kPeriod120);
  EXPECT_FALSE(v.learned(now).has_value());
}

TEST(VrLatchTest, V3DirectionLatchNoFlip) {
  // 誤差在反相點（目標 ± T/2）附近：+0.45T 與 −0.45T 只差 0.1T，最短路徑的方向卻相反。
  // 鎖住方向之後維持同一個方向走，不會一下 +200 一下 −200。
  const double target = kT120 * 0.4;
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, slack_for_err(target, 0.45 * kT120, kT120), 12, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_EQ(r.ppm, vr::latch::VR_LATCH_PPM_MAX);
  EXPECT_NEAR(r.err_us, 0.45 * kT120, 1.0);

  // 相位越過反相點：最短路徑變成 −0.45T，鎖存把它接成 +0.55T
  for (int i = 0; i < 12; ++i) {
    r = feed_v3(c, slack_for_err(target, -0.45 * kT120, kT120), 1, now, id);
    EXPECT_EQ(r.ppm, vr::latch::VR_LATCH_PPM_MAX);
    EXPECT_GT(r.err_us, 0.0);
  }
  EXPECT_NEAR(r.err_us, 0.55 * kT120, 1.0);
  EXPECT_NEAR(r.integral_ppm, 0.0, 1e-9);  // 輸出飽和、誤差同向：不積分

  // 一路走到 −0.2T（接成 +0.8T）方向都不變
  for (int i = 0; i < 8; ++i) {
    r = feed_v3(c, slack_for_err(target, -0.2 * kT120, kT120), 1, now, id);
    EXPECT_EQ(r.ppm, vr::latch::VR_LATCH_PPM_MAX);
  }
  EXPECT_NEAR(r.err_us, 0.8 * kT120, 1.0);

  // 誤差回到 0.15T 內才放開；之後往另一邊偏，就照新的方向鎖
  r = feed_v3(c, slack_for_err(target, 0.1 * kT120, kT120), 8, now, id);
  EXPECT_NEAR(r.err_us, 0.1 * kT120, 1.0);
  EXPECT_LT(r.ppm, vr::latch::VR_LATCH_PPM_MAX);
  EXPECT_GT(r.ppm, 0);
  r = feed_v3(c, slack_for_err(target, -0.45 * kT120, kT120), 8, now, id);
  EXPECT_NEAR(r.err_us, -0.45 * kT120, 1.0);
  EXPECT_EQ(r.ppm, -vr::latch::VR_LATCH_PPM_MAX);
  for (int i = 0; i < 8; ++i) {
    r = feed_v3(c, slack_for_err(target, 0.45 * kT120, kT120), 1, now, id);
    EXPECT_EQ(r.ppm, -vr::latch::VR_LATCH_PPM_MAX);
  }
  EXPECT_NEAR(r.err_us, -0.55 * kT120, 1.0);
}

TEST(VrLatchTest, V3ClosedLoopSteady) {
  // 目標 40／50／65%、±600 µs 到達抖動、時鐘漂移 ±60 ppm：120 s 之後不滑移、不重複、不跳幀，平均 slack 在目標 ±300 µs 內，
  // 積分學到漂移（∓60 ppm）。輸出離上限 200 還有距離：積分約 60（±15）＋比例項 0.12 ppm/µs × 中位數的偏差
  // （均勻 ±600 µs 的抖動下 7 筆的中位數最多偏到約 550 µs＝66 ppm），實際最大 121～133 ppm。
  for (const uint32_t period : {kPeriod120, kPeriod90}) {
    const double T = period / 1000.0;
    for (const int pct : {40, 50, 65}) {
      for (const double drift : {-60.0, 60.0}) {
        for (int i = 0; i < 5; ++i) {
          controller_t c;
          c.configure(mode_e::v3, pct, period);
          const double phase = T * i / 5.0;
          const auto res = run_loop(c, period, drift, phase, 600.0, 300.0, 120.0);
          SCOPED_TRACE(testing::Message() << "period=" << period << " pct=" << pct << " drift=" << drift << " phase=" << phase);
          EXPECT_EQ(res.slips_after_settle, 0u);
          EXPECT_EQ(res.repeats_after_settle, 0u);
          EXPECT_EQ(res.skips_after_settle, 0u);
          EXPECT_NEAR(res.mean_slack_after_settle_us, T * pct / 100.0, 300.0);
          EXPECT_LE(res.max_abs_ppm_after_settle, 150);  // 60＋15＋72，留一點餘裕
          EXPECT_NEAR(res.last.integral_ppm, -drift, 15.0);
          EXPECT_EQ(res.last.phase, vr::latch::kPhaseTrack);
          EXPECT_TRUE(res.last.gate_open);
          EXPECT_EQ(res.last.steps, 0u);  // 沒開 acquire：不步進
        }
      }
    }
  }
}

TEST(VrLatchTest, V3ClosedLoopConverges) {
  // 25 個起始相位 × 三種目標，不步進、±600 µs 抖動：45 s 內真正的相位誤差（不含抖動）進到 500 µs 內並停在裡面。
  // 起始在反相點附近的最久（±200 ppm 每秒只移 0.2 ms：120 Hz 約 22 s、90 Hz 約 28 s），沒有卡住或繞圈的。
  // 斷言看的是模型裡的真值：控制器自己的 err 是 7 筆的中位數，抖動 ±600 µs 下單筆就可能超過 500。
  // 這裡不加時鐘漂移：沒有起點的積分要另外花幾十秒學漂移（穩態見 V3ClosedLoopSteady）。
  for (const uint32_t period : {kPeriod120, kPeriod90}) {
    const double T = period / 1000.0;
    for (const int pct : {40, 50, 65}) {
      for (int i = 0; i < 25; ++i) {
        controller_t c;
        c.configure(mode_e::v3, pct, period);
        loop_opts_t o;
        o.trace = true;
        o.trace_target_us = T * pct / 100.0;
        const double phase = T * i / 25.0;
        const auto res = run_loop(c, period, 0.0, phase, 600.0, 50.0, 49.0, o);
        SCOPED_TRACE(testing::Message() << "period=" << period << " pct=" << pct << " phase=" << phase);
        // 最後一次超出 500 µs 的時刻
        double last_out = 0;
        for (const auto &[t, e] : res.phase_err) {
          if (std::abs(e) >= 500.0) {
            last_out = t;
          }
        }
        EXPECT_LT(last_out, 45.0);
        EXPECT_EQ(res.last.phase, vr::latch::kPhaseTrack);
        EXPECT_EQ(res.last.steps, 0u);
      }
    }
  }
}

TEST(VrLatchTest, V3UnsettledFlag) {
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  EXPECT_TRUE(c.unsettled(now));  // 還沒有任何 LATCH
  auto r = feed_v3(c, 3333, 9, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  EXPECT_TRUE(c.unsettled(now));
  r = feed_v3(c, 3333, 1, now, id);  // 第 10 則：收滿 5 筆樣本，開始追蹤
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_TRUE(c.unsettled(now));  // 進入追蹤未滿 3 s
  r = feed_v3(c, 3333, 29, now, id);
  EXPECT_TRUE(c.unsettled(now));
  r = feed_v3(c, 3333, 1, now, id);
  EXPECT_FALSE(c.unsettled(now));  // 滿 3 s

  // 閘門一關（黑幀）就不穩定；回到追蹤再等 3 s
  for (int i = 0; i < 3; ++i) {
    now += kStepNs;
    r = c.on_latch(3333, kPeriod120, now, ++id);
  }
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  EXPECT_TRUE(c.unsettled(now));
  r = feed_v3(c, 3333, 12, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_TRUE(c.unsettled(now));
  r = feed_v3(c, 3333, 30, now, id);
  EXPECT_FALSE(c.unsettled(now));

  // legacy／v2 恆為 false（§VR-PREDICT 的行為不變）
  controller_t v;
  v.configure(mode_e::v2, 40, kPeriod120);
  EXPECT_FALSE(v.unsettled(0));
  now = 0;
  feed_v2(v, 3333, 3, now, id, kPeriod120);
  EXPECT_FALSE(v.unsettled(now));
  controller_t l;
  EXPECT_FALSE(l.unsettled(0));
}

TEST(VrLatchTest, SwitchModeKeepsIntegral) {
  // 同場 A/B：v2 追蹤一陣子（積分累積）→ 換成 v3 → 再換回 v2。積分與最後送出的 ppm 都留著。
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  const auto slack = (int32_t) std::lround(kT120 * 0.4 + 600.0);
  vr::latch::result_t r;
  for (int i = 0; i < 100; ++i) {
    now += kStepNs;
    id += 12;
    r = c.on_latch(slack, kPeriod120, now, id);
  }
  ASSERT_EQ(r.mode, mode_e::v2);
  const double integ = r.integral_ppm;
  ASSERT_GT(integ, 20.0);
  const int32_t sent = c.last_sent_ppm();

  c.switch_mode(mode_e::v3, 60);
  EXPECT_EQ(c.mode(), mode_e::v3);
  EXPECT_NEAR(c.target_v2_us(), kT120 * 0.60, 0.5);  // v3 的上限是 65%
  EXPECT_EQ(c.last_sent_ppm(), sent);
  EXPECT_TRUE(c.unsettled(now));
  now += kStepNs;
  id += 12;
  r = c.on_latch(slack, kPeriod120, now, id);
  EXPECT_EQ(r.mode, mode_e::v3);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  EXPECT_DOUBLE_EQ(r.integral_ppm, integ);
  EXPECT_EQ(r.ppm, (int32_t) std::lround(integ));  // 樣本重來：只輸出積分
  r = feed_v3(c, slack, 12, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_NEAR(r.err_us, kT120 * 0.4 + 600.0 - kT120 * 0.6, 1.0);
  const double integ_v3 = r.integral_ppm;

  c.switch_mode(mode_e::v2, 60);  // v2 的上限是 50%
  EXPECT_EQ(c.mode(), mode_e::v2);
  EXPECT_NEAR(c.target_v2_us(), kT120 * 0.50, 0.5);
  EXPECT_FALSE(c.unsettled(now));
  now += kStepNs;
  id += 12;
  r = c.on_latch(slack, kPeriod120, now, id);
  EXPECT_EQ(r.mode, mode_e::v2);
  EXPECT_NEAR(r.integral_ppm, integ_v3, 1.0);  // 只多了這一筆的積分
  EXPECT_NEAR(r.err_us, kT120 * 0.4 + 600.0 - kT120 * 0.5, 1.0);  // EMA 從這一筆重新開始
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);

  c.switch_mode(mode_e::legacy, 40);
  now += kStepNs;
  r = c.on_latch(slack, kPeriod120, now, id);
  EXPECT_EQ(r.mode, mode_e::legacy);
}

TEST(VrLatchTest, V3AbSpecParse) {
  auto ab = vr::latch::parse_ab("60:v2:50");
  ASSERT_TRUE(ab.has_value());
  EXPECT_EQ(ab->seconds, 60);
  EXPECT_EQ(ab->mode, mode_e::v2);
  EXPECT_EQ(ab->target_pct, 50);
  ab = vr::latch::parse_ab("90:v3:55");
  ASSERT_TRUE(ab.has_value());
  EXPECT_EQ(ab->mode, mode_e::v3);
  EXPECT_EQ(ab->target_pct, 55);
  ab = vr::latch::parse_ab("20:legacy:40");
  ASSERT_TRUE(ab.has_value());
  EXPECT_EQ(ab->mode, mode_e::legacy);
  EXPECT_TRUE(vr::latch::parse_ab("600:v3:65").has_value());
  for (const char *bad : {"", "60", "60:v2", "60:v2:50:1", "19:v2:50", "601:v2:50", "60:v4:50", "60:V2:50", "60:v2:51", "60:v3:66", "60:v2:24", "sixty:v2:50", "60:v2:5o", "60 :v2:50", "+60:v2:50", "-60:v2:50", "60::50", ":v2:50", "60:v2:"}) {
    EXPECT_FALSE(vr::latch::parse_ab(bad).has_value()) << "spec='" << bad << "'";
  }
}

// ── §VR-LATCH-V3 定時 burst（options_t::acquire）──────────────────────────

namespace {
  /// 開著 acquire 的 v3 控制器（seed_ppm 不是 0 時當積分的起點，讓基準不是 0）
  controller_t make_acquire(int pct, uint32_t period_ns, double seed_ppm = 0.0, int step_ppm = 4000) {
    vr::latch::options_t opt;
    opt.acquire = true;
    opt.step_ppm = step_ppm;
    opt.have_seed = seed_ppm != 0.0;
    opt.seed_integral_ppm = seed_ppm;
    controller_t c;
    c.configure(mode_e::v3, pct, period_ns, opt);
    return c;
  }
}  // namespace

TEST(VrLatchTest, V3StepSignAndDuration) {
  // 32 個相位：|誤差| 超過 0.15T 才步進；burst 相對基準的方向＝誤差的方向（太早到 → 週期變長），
  // 長度＝|誤差| ÷ 4000 ppm（µs × 250），這一筆的 ppm 是基準（積分）
  for (const uint32_t period : {kPeriod120, kPeriod90}) {
    const double T = period / 1000.0;
    const double target = T * 0.4;
    for (int k = 0; k < 32; ++k) {
      auto c = make_acquire(40, period, -46.0);
      const int32_t slack = slack_for_err(target, ((k + 0.5) / 32.0 - 0.5) * T, T);
      const double e = wrap_half(slack - target, T);
      SCOPED_TRACE(testing::Message() << "period=" << period << " k=" << k << " err=" << e);
      int64_t now = 0;
      uint32_t id = 0;
      auto r = feed_v3(c, slack, 9, now, id, period);
      EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
      EXPECT_EQ(r.burst_us, 0u);
      EXPECT_EQ(r.ppm, -46);
      r = feed_v3(c, slack, 1, now, id, period);  // 第 10 則：收滿 5 筆
      if (std::abs(e) > 0.15 * T) {
        EXPECT_TRUE(r.update);
        EXPECT_EQ(r.ppm, -46);
        EXPECT_EQ(r.burst_ppm - r.ppm, e > 0 ? 4000 : -4000);
        EXPECT_NEAR((double) r.burst_us, std::abs(e) * 250.0, 1.0);
        EXPECT_EQ(r.step_why, vr::latch::kStepAcq);
        EXPECT_NEAR(r.step_err_us, e, 1e-6);
        EXPECT_NEAR(r.center_us, (double) slack, 1e-6);
        EXPECT_EQ(r.phase, vr::latch::kPhaseBurst);
        EXPECT_EQ(r.steps, 1u);
        EXPECT_FALSE(r.eval_valid);
        EXPECT_EQ(c.last_sent_ppm(), -46);
      } else {
        EXPECT_EQ(r.burst_us, 0u);
        EXPECT_EQ(r.step_why, 0);
        EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
        EXPECT_EQ(r.steps, 0u);
      }
      EXPECT_TRUE(c.unsettled(now));
    }
  }

  // 沒開 acquire：同樣的誤差不步進（＝S3 的行為）
  {
    controller_t c;
    c.configure(mode_e::v3, 40, kPeriod120);
    int64_t now = 0;
    uint32_t id = 0;
    const auto r = feed_v3(c, slack_for_err(kT120 * 0.4, 3000.0, kT120), 60, now, id);
    EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
    EXPECT_EQ(r.steps, 0u);
    EXPECT_EQ(r.ppm, vr::latch::VR_LATCH_PPM_MAX);
  }

  // vr_latch_acquire_ppm：2000 ppm 走兩倍久；超出 1000～5000 的值夾住；單次最長 1.5 s
  {
    int64_t now = 0;
    uint32_t id = 0;
    auto c = make_acquire(40, kPeriod120, 0.0, 2000);
    auto r = feed_v3(c, slack_for_err(kT120 * 0.4, -3000.0, kT120), 10, now, id);
    EXPECT_EQ(r.burst_ppm, -2000);
    EXPECT_NEAR((double) r.burst_us, 3000.0 * 500.0, 300.0);
    EXPECT_EQ(r.ppm, 0);

    auto lo = make_acquire(40, kPeriod120, 0.0, 10);
    now = 0;
    r = feed_v3(lo, slack_for_err(kT120 * 0.4, 1400.0, kT120), 10, now, id);
    EXPECT_EQ(r.burst_ppm, 1000);
    EXPECT_NEAR((double) r.burst_us, 1400.0 * 1000.0, 600.0);

    auto hi = make_acquire(40, kPeriod120, 0.0, 99999);
    now = 0;
    r = feed_v3(hi, slack_for_err(kT120 * 0.4, 3000.0, kT120), 10, now, id);
    EXPECT_EQ(r.burst_ppm, 5000);
    EXPECT_NEAR((double) r.burst_us, 3000.0 * 200.0, 150.0);

    auto cap = make_acquire(40, kPeriod90, 0.0, 2000);
    now = 0;
    r = feed_v3(cap, slack_for_err(kT90 * 0.4, 5000.0, kT90), 10, now, id, kPeriod90);
    EXPECT_EQ(r.burst_us, 1'500'000u);  // 5000 µs ÷ 2000 ppm＝2.5 s → 夾在 1.5 s，剩下的交給下一步
    EXPECT_EQ(r.burst_ppm, 2000);
  }
}

TEST(VrLatchTest, V3BlindNoUpdates) {
  // 誤差 +3000 µs → burst 750 ms，盲區到 burst 結束後 250 ms（共 1.0 s）。盲區內 LATCH 與 idle 都不更新 pacing
  // （每 1 s 的例行重送會把走到一半的 burst 蓋掉）；盲區後第一則把基準再寫一次。
  auto c = make_acquire(40, kPeriod120, -46.0);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, slack_for_err(kT120 * 0.4, 3000.0, kT120), 10, now, id);
  ASSERT_NEAR((double) r.burst_us, 750'000.0, 130.0);  // slack 是整數 µs：誤差差不到 0.5 µs，burst 差不到 125 µs
  ASSERT_TRUE(r.update);
  const int64_t step_at = now;
  for (int i = 1; i <= 9; ++i) {
    // 盲區裡的樣本什麼值都不看（相位正在動）
    r = feed_v3(c, i * 700, 1, now, id);
    EXPECT_FALSE(r.update) << "latch " << i;
    EXPECT_EQ(r.ppm, -46);
    EXPECT_EQ(r.burst_us, 0u);
    EXPECT_EQ(r.phase, vr::latch::kPhaseBurst);
    EXPECT_TRUE(r.gate_open);  // 閘門的紀錄照常更新
    const auto ri = c.on_idle(now + 50'000'000);
    EXPECT_FALSE(ri.update) << "idle " << i;
    EXPECT_EQ(ri.ppm, -46);
  }
  ASSERT_EQ(now - step_at, 900'000'000LL);
  // 盲區結束（step_at＋1.0 s）：第一則 LATCH 補寫基準、開始重新量
  r = feed_v3(c, slack_for_err(kT120 * 0.4, 100.0, kT120), 1, now, id);
  EXPECT_TRUE(r.update);
  EXPECT_EQ(r.ppm, -46);
  EXPECT_EQ(r.burst_us, 0u);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  r = feed_v3(c, slack_for_err(kT120 * 0.4, 100.0, kT120), 1, now, id);
  EXPECT_FALSE(r.update);  // 之後照常節流

  // LATCH 在步進之後就停了：盲區內 idle 不動；盲區過後、停滿 2 s 才由 idle 把基準寫回去
  auto d = make_acquire(40, kPeriod120, -46.0);
  now = 0;
  id = 0;
  r = feed_v3(d, slack_for_err(kT120 * 0.4, 3000.0, kT120), 10, now, id);
  ASSERT_NEAR((double) r.burst_us, 750'000.0, 130.0);
  EXPECT_FALSE(d.on_idle(now + 500'000'000).update);
  EXPECT_FALSE(d.on_idle(now + 999'000'000).update);
  EXPECT_FALSE(d.on_idle(now + 1'500'000'000).update);  // 盲區過了，但 LATCH 才停 1.5 s
  const auto ri = d.on_idle(now + 2'100'000'000);
  EXPECT_TRUE(ri.update);
  EXPECT_EQ(ri.ppm, -46);
  EXPECT_EQ(ri.phase, vr::latch::kPhaseWait);
  EXPECT_DOUBLE_EQ(ri.integral_ppm, -46.0);
}

TEST(VrLatchTest, V3TrimThenTrack) {
  // 第一步之後還差 1200 µs（> 0.12T＝1000）→ 再補一步；之後剩 100 µs → 追蹤。成敗只看最後的誤差。
  const double target = kT120 * 0.4;
  auto c = make_acquire(40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, slack_for_err(target, 3000.0, kT120), 10, now, id);
  ASSERT_EQ(r.step_why, vr::latch::kStepAcq);
  ASSERT_NEAR((double) r.burst_us, 750'000.0, 130.0);
  EXPECT_FALSE(r.eval_valid);

  // 盲區 1.0 s（9 則在裡面），之後 5 筆量到 +1200
  r = feed_v3(c, slack_for_err(target, 2000.0, kT120), 9, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseBurst);
  r = feed_v3(c, slack_for_err(target, 1200.0, kT120), 4, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  EXPECT_FALSE(r.eval_valid);
  r = feed_v3(c, slack_for_err(target, 1200.0, kT120), 1, now, id);
  EXPECT_EQ(r.step_why, vr::latch::kStepTrim);
  EXPECT_NEAR((double) r.burst_us, 300'000.0, 130.0);
  EXPECT_EQ(r.burst_ppm, 4000);
  ASSERT_TRUE(r.eval_valid);
  EXPECT_NEAR(r.eval_moved_us, 1800.0, 1.0);  // 3000 → 1200：往要求的方向動了 1800
  EXPECT_NEAR(r.eval_cmd_us, 3000.0, 1.0);
  EXPECT_NEAR(r.eval_err_us, 1200.0, 1.0);
  EXPECT_EQ(r.eval_outcome, vr::latch::kEvalTrim);
  EXPECT_EQ(r.steps, 2u);

  // 盲區 0.55 s（5 則在裡面），之後 5 筆量到 +100 → 追蹤
  r = feed_v3(c, slack_for_err(target, 600.0, kT120), 5, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseBurst);
  EXPECT_FALSE(r.eval_valid);
  r = feed_v3(c, slack_for_err(target, 100.0, kT120), 4, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  r = feed_v3(c, slack_for_err(target, 100.0, kT120), 1, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_EQ(r.burst_us, 0u);
  ASSERT_TRUE(r.eval_valid);
  EXPECT_NEAR(r.eval_moved_us, 1100.0, 1.0);
  EXPECT_NEAR(r.eval_cmd_us, 1200.0, 1.0);
  EXPECT_NEAR(r.eval_err_us, 100.0, 1.0);
  EXPECT_EQ(r.eval_outcome, vr::latch::kEvalTrack);
  EXPECT_EQ(r.steps, 2u);
  EXPECT_EQ(r.bad_steps, 0u);
  r = feed_v3(c, slack_for_err(target, 100.0, kT120), 1, now, id);
  EXPECT_FALSE(r.eval_valid);  // 結果只帶出一次

  // 往另一個方向：太晚到 → 週期變短；修整完還在 0.12T～0.15T 之間也算成功（步數用完、誤差在進入門檻內）
  auto d = make_acquire(50, kPeriod120);
  now = 0;
  id = 0;
  const double target50 = kT120 * 0.5;
  r = feed_v3(d, slack_for_err(target50, -2000.0, kT120), 10, now, id);
  EXPECT_EQ(r.burst_ppm, -4000);
  EXPECT_NEAR((double) r.burst_us, 500'000.0, 130.0);
  r = feed_v3(d, slack_for_err(target50, -1100.0, kT120), 7 + 5, now, id);  // 盲區 0.75 s（7 則）＋ 5 筆
  ASSERT_EQ(r.step_why, vr::latch::kStepTrim);
  EXPECT_EQ(r.burst_ppm, -4000);
  EXPECT_NEAR(r.eval_moved_us, -900.0, 1.0);
  EXPECT_NEAR(r.eval_cmd_us, -2000.0, 1.0);
  r = feed_v3(d, slack_for_err(target50, -1100.0, kT120), 5 + 5, now, id);  // 盲區 0.525 s（5 則）＋ 5 筆：沒有動
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  ASSERT_TRUE(r.eval_valid);
  EXPECT_EQ(r.eval_outcome, vr::latch::kEvalTrack);
  EXPECT_EQ(r.steps, 2u);
  EXPECT_EQ(r.bad_steps, 0u);
}

TEST(VrLatchTest, V3ReacqNeedsFiveInARow) {
  // 追蹤中的誤差是最近 7 筆的中位數：要連續 5 筆都超過 0.15T（1250 µs）才重新步進
  const double target = kT120 * 0.4;
  const int32_t on = slack_for_err(target, 0.0, kT120);
  const int32_t off = slack_for_err(target, 2000.0, kT120);
  auto c = make_acquire(40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, on, 20, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  ASSERT_EQ(r.steps, 0u);

  // 4 筆偏掉的樣本：中位數從第 4 筆起偏 4 次（偏掉的佔多數的那 4 筆），接著回到目標 → 不步進
  r = feed_v3(c, off, 4, now, id);
  EXPECT_NEAR(r.err_us, 2000.0, 1.0);
  for (int i = 0; i < 12; ++i) {
    r = feed_v3(c, on, 1, now, id);
    EXPECT_EQ(r.burst_us, 0u);
  }
  EXPECT_EQ(r.steps, 0u);
  EXPECT_NEAR(r.err_us, 0.0, 1.0);

  // 一直偏：第 4 筆起中位數偏掉，連 5 筆（第 8 筆）才步進
  for (int i = 1; i <= 7; ++i) {
    r = feed_v3(c, off, 1, now, id);
    EXPECT_EQ(r.burst_us, 0u) << "sample " << i;
  }
  r = feed_v3(c, off, 1, now, id);
  EXPECT_EQ(r.step_why, vr::latch::kStepReacq);
  EXPECT_NEAR((double) r.burst_us, 500'000.0, 130.0);
  EXPECT_EQ(r.burst_ppm - r.ppm, 4000);
  EXPECT_EQ(r.steps, 1u);
  EXPECT_FALSE(r.eval_valid);

  // 這一步成功（之後量到在目標上）→ 追蹤，退避不啟動
  r = feed_v3(c, on, 7 + 5, now, id);  // 盲區 0.75 s（7 則）＋ 5 筆
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  ASSERT_TRUE(r.eval_valid);
  EXPECT_NEAR(r.eval_moved_us, 2000.0, 1.0);
  EXPECT_EQ(r.eval_outcome, vr::latch::kEvalTrack);
  EXPECT_EQ(r.bad_steps, 0u);
}

TEST(VrLatchTest, V3BackoffNotDisable) {
  // 量測完全不動（步進對它沒有作用）：一輪兩步（第一步＋修整）→ 記一次失敗 → 5 s 內不再步進 → 之後每輪的退避
  // 10／20／40／60／60… s。任何 60 s 內不超過 8 步，而且一直會再試（沒有整場停用）。
  const double target = kT120 * 0.4;
  const int32_t stuck = slack_for_err(target, 3000.0, kT120);
  auto c = make_acquire(40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  std::vector<double> step_at;  // 每一步的時刻（s）
  std::vector<double> bad_at;  // 每一次判失敗的時刻
  std::vector<uint8_t> step_why;
  uint32_t bad = 0;
  for (int i = 0; i < 6000; ++i) {  // 600 s
    const auto r = feed_v3(c, stuck, 1, now, id);
    if (r.burst_us > 0) {
      step_at.push_back(now / 1e9);
      step_why.push_back(r.step_why);
    }
    if (r.bad_steps != bad) {
      ASSERT_EQ(r.bad_steps, bad + 1);
      bad = r.bad_steps;
      bad_at.push_back(now / 1e9);
      ASSERT_TRUE(r.eval_valid);
      EXPECT_EQ(r.eval_outcome, vr::latch::kEvalBackoff);
      EXPECT_NEAR(r.eval_moved_us, 0.0, 1.0);
      EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);  // 失敗之後照常用 PI 追
    }
  }
  ASSERT_GE(bad_at.size(), 6u);
  ASSERT_EQ(step_at.size() / 2, bad_at.size());  // 每輪剛好兩步（最後一輪也走完了）
  ASSERT_EQ(step_at.size() % 2, 0u);
  for (size_t round = 0; round < bad_at.size(); ++round) {
    SCOPED_TRACE(testing::Message() << "round " << round);
    EXPECT_EQ(step_why[2 * round], round == 0 ? vr::latch::kStepAcq : vr::latch::kStepReacq);
    EXPECT_EQ(step_why[2 * round + 1], vr::latch::kStepTrim);
    EXPECT_LT(step_at[2 * round + 1], bad_at[round]);
    if (round > 0) {
      // 上一輪判失敗之後，隔 5 s × 2^(round−1)（上限 60 s）才開始下一輪
      const double backoff = std::min(5.0 * (double) (1u << std::min<size_t>(round - 1, 10)), 60.0);
      EXPECT_GE(step_at[2 * round] - bad_at[round - 1], backoff - 1e-6);
      EXPECT_LT(step_at[2 * round] - bad_at[round - 1], backoff + 0.3);
    }
  }
  // 任何 60 s 內不超過 8 步
  for (size_t i = 0; i < step_at.size(); ++i) {
    size_t n = 0;
    for (size_t j = i; j < step_at.size() && step_at[j] < step_at[i] + 60.0; ++j) {
      ++n;
    }
    EXPECT_LE(n, 8u) << "window starting at " << step_at[i];
  }
  EXPECT_GT(step_at.back(), 600.0 - 70.0);  // 到最後都還在試

  // 每一步都成功、但馬上又被推開（每 2.3 s 就想再步進一次）：60 s 的步數上限擋住，過了窗又可以走
  auto d = make_acquire(40, kPeriod120);
  now = 0;
  id = 0;
  const int32_t on = slack_for_err(target, 0.0, kT120);
  step_at.clear();
  vr::latch::result_t r;
  for (int i = 0; i < 2000; ++i) {  // 200 s
    // 步進之後到重新開始追蹤之前量到的都在目標上；一回到追蹤又偏掉
    const bool recovering = r.phase == vr::latch::kPhaseBurst || r.phase == vr::latch::kPhaseMeasure;
    r = feed_v3(d, i < 20 || recovering ? on : stuck, 1, now, id);
    if (r.burst_us > 0) {
      step_at.push_back(now / 1e9);
    }
    EXPECT_EQ(r.bad_steps, 0u);
  }
  ASSERT_GE(step_at.size(), 24u);  // 200 s：三個多 60 s 的窗，每窗 8 步
  EXPECT_LT(step_at[7] - step_at[0], 25.0);  // 前 8 步很快用完
  EXPECT_GE(step_at[8] - step_at[0], 60.0);  // 第 9 步要等第 1 步滿 60 s
  for (size_t i = 0; i < step_at.size(); ++i) {
    size_t n = 0;
    for (size_t j = i; j < step_at.size() && step_at[j] < step_at[i] + 60.0; ++j) {
      ++n;
    }
    EXPECT_LE(n, 8u) << "window starting at " << step_at[i];
  }
}

TEST(VrLatchTest, V3ClosedLoopLocksFast) {
  // 120／90 Hz × 64 個起始相位 × 漂移 0／±60 ppm、±600 µs 抖動：最多兩步；閘門開後 4 s 起，真正的相位誤差
  // （不含抖動）都在門檻內——有步進的場次在修整門檻 0.12T 內；起始誤差本來就在進入門檻 0.15T 內的場次不步進，
  // 交給 PI 慢慢收，這 4 s 裡不保證進到 0.12T（逆向漂移 60 ppm 時差一點），但不會超出 0.15T。
  for (const uint32_t period : {kPeriod120, kPeriod90}) {
    const double T = period / 1000.0;
    for (const double drift : {-60.0, 0.0, 60.0}) {
      for (const int pct : {40, 50}) {
        for (int i = 0; i < 64; ++i) {
          auto c = make_acquire(pct, period);
          loop_opts_t o;
          o.trace = true;
          o.trace_target_us = T * pct / 100.0;
          const double phase = T * i / 64.0;
          const auto res = run_loop(c, period, drift, phase, 600.0, 14.0, 13.0, o);
          SCOPED_TRACE(testing::Message() << "period=" << period << " drift=" << drift << " pct=" << pct << " phase=" << phase);
          ASSERT_GT(res.gate_open_at_s, 0.0);
          EXPECT_LT(res.gate_open_at_s, 1.0);
          EXPECT_LE(res.last.steps, 2u);
          EXPECT_EQ(res.last.bad_steps, 0u);
          EXPECT_EQ(res.last.phase, vr::latch::kPhaseTrack);
          const double limit = (res.last.steps > 0 ? 0.12 : 0.15) * T;
          double worst = 0;
          for (const auto &[t, e] : res.phase_err) {
            if (t >= res.gate_open_at_s + 4.0) {
              worst = std::max(worst, std::abs(e));
            }
          }
          EXPECT_LT(worst, limit);
          // 步進的時刻不設上限：起始誤差略低於進入門檻的場次一開始不步進，追蹤幾秒後才被抖動推過門檻（reacq）
          for (const auto &[t, r] : res.steps) {
            EXPECT_EQ(std::abs(r.burst_ppm - r.ppm), 4000);
          }
        }
      }
    }
  }
}

TEST(VrLatchTest, V3ClosedLoopLoadShift) {
  // 追蹤 20 s 之後，到達時刻一次晚 3 ms（host 管線變長）：2.5 s 內回到 0.12T 內，之後不再步進
  for (const uint32_t period : {kPeriod120, kPeriod90}) {
    const double T = period / 1000.0;
    for (const double drift : {-60.0, 0.0, 60.0}) {
      for (const int pct : {40, 50}) {
        for (int i = 0; i < 16; ++i) {
          auto c = make_acquire(pct, period);
          loop_opts_t o;
          o.trace = true;
          o.trace_target_us = T * pct / 100.0;
          o.shift_at_s = 20.0 + 0.013 * i;
          o.shift_us = 3000.0;
          const double phase = T * i / 16.0;
          const auto res = run_loop(c, period, drift, phase, 600.0, 40.0, 39.0, o);
          SCOPED_TRACE(testing::Message() << "period=" << period << " drift=" << drift << " pct=" << pct << " phase=" << phase);
          double worst = 0;
          for (const auto &[t, e] : res.phase_err) {
            if (t >= o.shift_at_s + 2.5) {
              worst = std::max(worst, std::abs(e));
            }
          }
          EXPECT_LT(worst, 0.12 * T);
          int after_shift = 0;
          for (const auto &[t, r] : res.steps) {
            if (t >= o.shift_at_s) {
              ++after_shift;
              EXPECT_EQ(r.step_why, vr::latch::kStepReacq);
              EXPECT_LT(t, o.shift_at_s + 1.5);
              EXPECT_LT(r.burst_ppm, r.ppm);  // 幀變晚到 → 週期變短
            }
          }
          EXPECT_EQ(after_shift, 1);
          EXPECT_EQ(res.last.bad_steps, 0u);
          EXPECT_EQ(res.last.phase, vr::latch::kPhaseTrack);
        }
      }
    }
  }
}

TEST(VrLatchTest, V3InvariantsUnderRandomInput) {
  // 亂七八糟的輸入（極端的 slack、重複／倒退／亂跳的 frameId、時間停住或跳很遠、不合理的週期、中途換模式、idle 穿插）：
  // 數值都是有限值、輸出在範圍內、盲區內不更新 pacing、任何 60 s 內不超過 8 步、沒開 acquire 就不步進。
  std::mt19937_64 gen(20261008);
  const auto rnd = [&gen](uint64_t n) {
    return (uint64_t) (gen() % n);
  };
  uint64_t steps_seen = 0;
  for (int run = 0; run < 120; ++run) {
    controller_t c;
    vr::latch::options_t opt;
    opt.acquire = rnd(4) != 0;
    opt.step_ppm = (int) rnd(8000) - 500;
    opt.have_seed = rnd(2) == 0;
    opt.seed_integral_ppm = (double) rnd(600) - 300.0;
    const uint32_t hosts[] = {0, kPeriod120, kPeriod90, 2 * kPeriod120, 4'000'000, 60'000'000};
    c.configure(mode_e::v3, (int) rnd(100), hosts[rnd(6)], opt);
    const int step_ppm = std::clamp(opt.step_ppm, controller_t::kV3StepPpmMin, controller_t::kV3StepPpmMax);
    int64_t now = (int64_t) rnd(1'000'000'000);
    uint32_t id = (uint32_t) gen();
    uint32_t period = rnd(2) ? kPeriod120 : kPeriod90;
    int center = (int) rnd(12000);
    std::vector<int64_t> step_times;
    int64_t blind_until = 0;
    for (int i = 0; i < 3000; ++i) {
      vr::latch::result_t r;
      const uint64_t what = rnd(100);
      if (what < 3) {
        now += (int64_t) rnd(3'000'000'000);
        r = c.on_idle(now);
      } else if (what < 4) {
        const mode_e modes[] = {mode_e::legacy, mode_e::v2, mode_e::v3, mode_e::v3, mode_e::v3};
        c.switch_mode(modes[rnd(5)], (int) rnd(100));  // 盲區不因為換模式而取消（下面的不變式照查）
        continue;
      } else {
        const uint64_t tk = rnd(100);  // 時間：多半 100 ms，偶爾不動、倒退一點、或隔很久
        if (tk < 80) {
          now += kStepNs;
        } else if (tk >= 85 && tk < 88) {
          now -= (int64_t) rnd(2'000'000);
        } else if (tk >= 88 && tk < 97) {
          now += (int64_t) rnd(400'000'000);
        } else if (tk >= 97) {
          now += (int64_t) rnd(100'000'000'000);
        }
        const uint64_t fk = rnd(100);  // frameId：多半正常前進，偶爾重複、倒退、亂跳
        if (fk < 80) {
          id += 12;
        } else if (fk < 88) {
          id += (uint32_t) rnd(3);
        } else if (fk < 92) {
          id -= (uint32_t) rnd(50);
        } else if (fk < 95) {
          id = (uint32_t) gen();
        }
        const uint64_t sk = rnd(100);  // slack：圍著一個偶爾搬家的中心，偶爾是極端值
        if (rnd(200) == 0) {
          center = (int) rnd(12000);
        }
        int32_t slack = center + (int) rnd(1500) - 750;
        if (sk < 2) {
          slack = (int32_t) gen();
        } else if (sk == 2) {
          slack = INT32_MIN;
        } else if (sk == 3) {
          slack = INT32_MAX;
        }
        const uint64_t pk = rnd(1000);  // 顯示週期：偶爾換更新率、偶爾不合理
        uint32_t p = period;
        if (pk < 3) {
          period = period == kPeriod120 ? kPeriod90 : kPeriod120;
          p = period;
        } else if (pk < 8) {
          p = (uint32_t) gen();
        } else if (pk < 12) {
          p = 0;
        }
        r = c.on_latch(slack, p, now, id);
      }
      ASSERT_TRUE(std::isfinite(r.integral_ppm) && std::isfinite(r.err_us) && std::isfinite(r.center_us) && std::isfinite(r.target_us));
      ASSERT_LE(std::abs(r.integral_ppm), controller_t::kIntegralMax);
      ASSERT_LE(std::abs(r.ppm), vr::latch::VR_LATCH_PPM_MAX);
      ASSERT_LE(r.phase, vr::latch::kPhaseTrack);
      if (r.burst_us > 0) {
        ++steps_seen;
        ASSERT_EQ(r.mode, mode_e::v3);
        ASSERT_TRUE(opt.acquire);
        ASSERT_TRUE(r.update);
        ASSERT_LE(r.burst_us, 1'500'000u);
        ASSERT_EQ(std::abs(r.burst_ppm - r.ppm), step_ppm);
        ASSERT_LE(std::abs(r.ppm), 150);  // 基準＝積分
        ASSERT_GT((double) (r.burst_ppm - r.ppm) * r.step_err_us, 0.0);  // 方向＝誤差的方向
        ASSERT_GE(now, blind_until);
        step_times.push_back(now);
        blind_until = now + (int64_t) r.burst_us * 1000 + controller_t::kV3SettleNs;
        const auto recent = std::count_if(step_times.begin(), step_times.end(), [now](int64_t t) {
          return now - t < 60'000'000'000;
        });
        ASSERT_LE(recent, 8);
      } else if (r.mode == mode_e::v3 && now < blind_until) {
        ASSERT_FALSE(r.update) << "run " << run << " call " << i;
      }
      if (r.eval_valid) {
        ASSERT_TRUE(std::isfinite(r.eval_moved_us) && std::isfinite(r.eval_cmd_us) && std::isfinite(r.eval_err_us));
        ASSERT_LE(r.eval_outcome, vr::latch::kEvalVoid);
      }
    }
  }
  EXPECT_GT(steps_seen, 500u);  // 這組輸入確實走到步進的路徑
}

// ── §VR-LATCH-V3：審查後補的情境（影像停住、被打斷的步進、重新進追蹤時的積分、burst 中換模式、偏低的 step_ppm）──

TEST(VrLatchTest, V3FrozenVideoDropsProportional) {
  // 影像停住、LATCH 還在送（client 每 100 ms 重送同一個 frameId）：閘門一關就把比例項拿掉、回到等待，積分原封不動；
  // 閘門還開著時，單獨一則重複的 LATCH 只是不當新樣本。
  const double target = kT120 * 0.4;
  const int32_t slack = slack_for_err(target, 1500.0, kT120);
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, slack, 20, now, id);  // 誤差 +1500 µs：比例項 180 ppm
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  ASSERT_GT(r.ppm, 150);
  const double integ = r.integral_ppm;
  const auto base = (int32_t) std::lround(integ);
  const int32_t held = c.last_sent_ppm();
  const uint64_t count = c.latch_count();
  const uint64_t dropped = r.dropped;

  // 第一則重複：最近 5 個間隔還有 48 張（門檻 42），閘門開著 → 不當新樣本，其他都不動
  now += kStepNs;
  r = c.on_latch(slack, kPeriod120, now, id);
  EXPECT_TRUE(r.gate_open);
  EXPECT_FALSE(r.update);
  EXPECT_EQ(r.ppm, held);
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_EQ(c.latch_count(), count);
  EXPECT_EQ(r.dropped, dropped + 1);

  // 第二則重複：剩 36 張，閘門關 → 比例項拿掉、回到等待（§VR-PREDICT 這時不採樣）
  now += kStepNs;
  r = c.on_latch(slack, kPeriod120, now, id);
  EXPECT_FALSE(r.gate_open);
  EXPECT_TRUE(r.update);
  EXPECT_EQ(r.ppm, base);
  EXPECT_DOUBLE_EQ(r.integral_ppm, integ);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  EXPECT_FALSE(r.eval_valid);
  EXPECT_TRUE(c.unsettled(now));
  EXPECT_EQ(c.last_sent_ppm(), base);

  // 停 60 s：每 100 ms 一則重複，1 s 的 idle tick 照跑。輸出停在積分；LATCH 沒有停，所以積分不衰減
  for (int i = 0; i < 600; ++i) {
    now += kStepNs;
    r = c.on_latch(slack, kPeriod120, now, id);
    EXPECT_FALSE(r.update) << "dup " << i;
    EXPECT_EQ(r.ppm, base);
    EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
    if (i % 10 == 9) {
      EXPECT_FALSE(c.on_idle(now + 1).update) << "idle " << i;
    }
  }
  EXPECT_DOUBLE_EQ(r.integral_ppm, integ);
  EXPECT_EQ(c.last_sent_ppm(), base);
  EXPECT_EQ(c.latch_count(), count);

  // 影像恢復：閘門要重新收到七成的張數（第 4 個正常間隔：48 ≥ 42），再收滿 5 筆才回到追蹤
  r = feed_v3(c, slack, 3, now, id);
  EXPECT_FALSE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  EXPECT_EQ(r.ppm, base);
  r = feed_v3(c, slack, 1, now, id);
  EXPECT_TRUE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  r = feed_v3(c, slack, 4, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_GT(r.ppm, 150);

  // v2 對照（行為不變）：重複的 LATCH 不算「收到 LATCH」，2 s 後由 idle 每秒減半到 0（積分也是）
  controller_t v;
  v.configure(mode_e::v2, 40, kPeriod120);
  now = 0;
  id = 0;
  for (int i = 0; i < 20; ++i) {
    now += kStepNs;
    id += 12;
    v.on_latch(slack, kPeriod120, now, id);
  }
  ASSERT_GT(v.last_sent_ppm(), 150);
  for (int i = 0; i < 100; ++i) {
    now += kStepNs;
    v.on_latch(slack, kPeriod120, now, id);
    if (i % 10 == 9) {
      v.on_idle(now + 1);
    }
  }
  EXPECT_LE(std::abs(v.last_sent_ppm()), 2);
}

TEST(VrLatchTest, V3ClosedLoopFrozenVideoKeepsLock) {
  // 閉迴路：追蹤 90 s（積分學到 ±46 ppm 的時鐘差）之後影像停 60 s——幀到不了 client、LATCH 照送同一個 frameId。
  // 停住期間輸出只剩積分，所以恢復時相位還在原地：恢復之後真正的相位誤差（不含抖動）一直在 0.12T 內，不必重新鎖定。
  // 最後一次的比例項留著的話（最多 ±200 ppm），60 s 可以漂掉一整個週期以上，恢復後要重新收斂。
  for (const uint32_t period : {kPeriod120, kPeriod90}) {
    const double T = period / 1000.0;
    for (const double drift : {-46.0, 46.0}) {
      for (int i = 0; i < 8; ++i) {
        controller_t c;
        c.configure(mode_e::v3, 40, period);
        loop_opts_t o;
        o.trace = true;
        o.trace_target_us = T * 0.4;
        o.freeze_at_s = 90.0;
        o.freeze_s = 60.0;
        const double phase = T * i / 8.0;
        const auto res = run_loop(c, period, drift, phase, 600.0, 170.0, 169.0, o);
        SCOPED_TRACE(testing::Message() << "period=" << period << " drift=" << drift << " phase=" << phase);
        double worst = 0;
        size_t after = 0;
        for (const auto &[t, e] : res.phase_err) {
          EXPECT_FALSE(t > 90.5 && t < 150.0) << "a frame was latched during the freeze at " << t;
          if (t >= 150.0) {
            worst = std::max(worst, std::abs(e));
            ++after;
          }
        }
        EXPECT_GT(after, 1500u);  // 恢復後確實有幀（20 s × 更新率）
        EXPECT_LT(worst, 0.12 * T);  // 實際 50～600 µs（積分與時鐘差的殘差 × 60 s）
        EXPECT_EQ(res.last.phase, vr::latch::kPhaseTrack);
        EXPECT_TRUE(res.last.gate_open);
      }
    }
  }
}

TEST(VrLatchTest, V3InterruptedRoundIsVoid) {
  // 一步之後、量完之前節拍斷了（黑幀、影像停住、LATCH 停）：那一步不評估（void，只帶出一次）、不算成敗；
  // 之後是新的一輪——用進入門檻 0.15T，不是把很久以後量到的相位當成同一輪的修整（門檻 0.12T）。
  const double target = kT120 * 0.4;
  const int32_t slack_far = slack_for_err(target, 3000.0, kT120);
  const int32_t slack_near = slack_for_err(target, 1100.0, kT120);  // 0.12T（1000）＜ 1100 ＜ 0.15T（1250）

  // (a) burst 期間 host 開始只送黑幀（每 100 ms 一張），持續 60 s
  {
    auto c = make_acquire(40, kPeriod120, -46.0);
    int64_t now = 0;
    uint32_t id = 0;
    auto r = feed_v3(c, slack_far, 10, now, id);
    ASSERT_EQ(r.step_why, vr::latch::kStepAcq);
    ASSERT_NEAR((double) r.burst_us, 750'000.0, 130.0);  // 盲區 1.0 s：之後 9 則在裡面
    int voids = 0;
    for (int i = 1; i <= 600; ++i) {
      now += kStepNs;
      r = c.on_latch(1234, kPeriod120, now, ++id);
      if (i <= 9) {
        EXPECT_FALSE(r.update) << "blind " << i;
        EXPECT_FALSE(r.eval_valid);
        EXPECT_EQ(r.phase, vr::latch::kPhaseBurst);
      } else if (r.eval_valid) {
        ++voids;
        EXPECT_EQ(i, 10);  // 盲區一結束、閘門關著的第一則
        EXPECT_EQ(r.eval_outcome, vr::latch::kEvalVoid);
        EXPECT_NEAR(r.eval_cmd_us, 3000.0, 1.0);
        EXPECT_EQ(r.eval_moved_us, 0.0);
        EXPECT_EQ(r.eval_err_us, 0.0);
        EXPECT_TRUE(r.update);  // 同一則把基準補寫一次
        EXPECT_EQ(r.ppm, -46);
        EXPECT_EQ(r.burst_us, 0u);
        EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
      }
    }
    EXPECT_EQ(voids, 1);
    EXPECT_EQ(r.steps, 1u);
    EXPECT_EQ(r.bad_steps, 0u);

    // 恢復出幀，相位在 +1100 µs：沒到進入門檻就不步進，也不會再帶出那一步的結果
    for (int i = 0; i < 30; ++i) {
      r = feed_v3(c, slack_near, 1, now, id);
      EXPECT_EQ(r.burst_us, 0u) << "latch " << i;
      EXPECT_FALSE(r.eval_valid) << "latch " << i;
    }
    EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
    EXPECT_NEAR(r.err_us, 1100.0, 1.0);
    EXPECT_EQ(r.steps, 1u);
    EXPECT_EQ(r.bad_steps, 0u);
  }

  // (b) 步進之後 LATCH 停了 30 s（XR 失焦）
  {
    auto c = make_acquire(40, kPeriod120, -46.0);
    int64_t now = 0;
    uint32_t id = 0;
    auto r = feed_v3(c, slack_far, 10, now, id);
    ASSERT_EQ(r.step_why, vr::latch::kStepAcq);
    auto ri = c.on_idle(now + 500'000'000);  // 盲區內
    EXPECT_FALSE(ri.update);
    EXPECT_FALSE(ri.eval_valid);
    ri = c.on_idle(now + 1'500'000'000);  // 盲區過了，但 LATCH 才停 1.5 s
    EXPECT_FALSE(ri.update);
    EXPECT_FALSE(ri.eval_valid);
    ri = c.on_idle(now + 2'500'000'000);  // 停滿 2 s：回到等待，那一步作廢
    EXPECT_TRUE(ri.update);
    EXPECT_EQ(ri.ppm, -46);
    EXPECT_EQ(ri.phase, vr::latch::kPhaseWait);
    ASSERT_TRUE(ri.eval_valid);
    EXPECT_EQ(ri.eval_outcome, vr::latch::kEvalVoid);
    EXPECT_NEAR(ri.eval_cmd_us, 3000.0, 1.0);
    for (int s = 4; s <= 30; ++s) {
      ri = c.on_idle(now + s * 1'000'000'000LL);
      EXPECT_FALSE(ri.eval_valid) << "idle " << s;  // 只帶出一次
    }
    now += 30'000'000'000LL;
    for (int i = 0; i < 30; ++i) {
      r = feed_v3(c, slack_near, 1, now, id);
      EXPECT_EQ(r.burst_us, 0u) << "latch " << i;
      EXPECT_FALSE(r.eval_valid) << "latch " << i;
    }
    EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
    EXPECT_EQ(r.steps, 1u);
    EXPECT_EQ(r.bad_steps, 0u);
  }

  // (c) 步進之後影像停住（LATCH 只剩重複的 frameId）；恢復時誤差還很大 → 新的一輪從 acq 開始，兩步的額度是完整的
  {
    auto c = make_acquire(40, kPeriod120, -46.0);
    int64_t now = 0;
    uint32_t id = 0;
    auto r = feed_v3(c, slack_far, 10, now, id);
    ASSERT_EQ(r.step_why, vr::latch::kStepAcq);
    int voids = 0;
    for (int i = 1; i <= 40; ++i) {
      now += kStepNs;
      r = c.on_latch(slack_far, kPeriod120, now, id);  // 同一個 frameId
      if (r.eval_valid) {
        ++voids;
        EXPECT_EQ(i, 10);
        EXPECT_EQ(r.eval_outcome, vr::latch::kEvalVoid);
        EXPECT_TRUE(r.update);
        EXPECT_EQ(r.ppm, -46);
        EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
      } else {
        EXPECT_FALSE(r.update) << "dup " << i;  // 盲區內不寫；回到等待之後也沒有東西要寫
      }
    }
    EXPECT_EQ(voids, 1);

    r = feed_v3(c, slack_far, 7, now, id);  // 第 4 則閘門開，到這裡 4 筆樣本
    EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
    EXPECT_EQ(r.burst_us, 0u);
    r = feed_v3(c, slack_far, 1, now, id);
    EXPECT_EQ(r.step_why, vr::latch::kStepAcq);  // 不是 trim
    EXPECT_FALSE(r.eval_valid);  // 沒有拿 4 秒前那一步來比
    EXPECT_NEAR((double) r.burst_us, 750'000.0, 130.0);
    EXPECT_EQ(r.steps, 2u);
    r = feed_v3(c, slack_for_err(target, 1200.0, kT120), 9 + 5, now, id);  // 盲區 9 則＋5 筆：還差 1200 → 修整
    EXPECT_EQ(r.step_why, vr::latch::kStepTrim);
    ASSERT_TRUE(r.eval_valid);
    EXPECT_EQ(r.eval_outcome, vr::latch::kEvalTrim);
    EXPECT_EQ(r.steps, 3u);
    r = feed_v3(c, slack_for_err(target, 100.0, kT120), 5 + 5, now, id);  // 盲區 5 則＋5 筆
    EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
    ASSERT_TRUE(r.eval_valid);
    EXPECT_EQ(r.eval_outcome, vr::latch::kEvalTrack);
    EXPECT_EQ(r.bad_steps, 0u);
  }

  // (d) 盲區過後、5 筆還沒量完，閘門關了（連兩個間隔只前進 1 張）
  {
    auto c = make_acquire(40, kPeriod120);
    int64_t now = 0;
    uint32_t id = 0;
    auto r = feed_v3(c, slack_far, 10, now, id);
    ASSERT_EQ(r.step_why, vr::latch::kStepAcq);
    r = feed_v3(c, slack_near, 9, now, id);
    EXPECT_EQ(r.phase, vr::latch::kPhaseBurst);
    r = feed_v3(c, slack_near, 2, now, id);
    EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
    EXPECT_FALSE(r.eval_valid);
    now += kStepNs;
    r = c.on_latch(slack_near, kPeriod120, now, ++id);  // 49 張：閘門還開著，這是第 3 筆樣本
    EXPECT_TRUE(r.gate_open);
    EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
    EXPECT_FALSE(r.eval_valid);
    now += kStepNs;
    r = c.on_latch(slack_near, kPeriod120, now, ++id);  // 38 張：閘門關
    EXPECT_FALSE(r.gate_open);
    EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
    ASSERT_TRUE(r.eval_valid);
    EXPECT_EQ(r.eval_outcome, vr::latch::kEvalVoid);
    EXPECT_EQ(r.bad_steps, 0u);
  }
}

TEST(VrLatchTest, V3TrackReentryIntegralBounded) {
  // 離開追蹤一陣子（這裡是 60 s 黑幀，閘門關著）再回來：第一筆的積分用「距上一次 PI 更新多久」當 dt，夾在 1 s
  // （沿用 v2 的寫法）。所以那一筆最多積 Ki × 誤差 × 1 s（誤差 1500 µs 時 12 ppm，是平常一筆的十倍，但不會更多）；
  // 不管離開多久都一樣，之後每 100 ms 一筆回到平常的量。離開追蹤的期間（等待、量測）積分不動。
  const double target = kT120 * 0.4;
  const int32_t on = slack_for_err(target, 0.0, kT120);
  const int32_t off = slack_for_err(target, 1500.0, kT120);
  const double err = wrap_half(off - target, kT120);
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, on, 30, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  for (int i = 0; i < 600; ++i) {  // 60 s 黑幀：閘門關、回到等待
    now += kStepNs;
    r = c.on_latch(on, kPeriod120, now, ++id);
  }
  ASSERT_EQ(r.phase, vr::latch::kPhaseWait);
  const double before = r.integral_ppm;
  r = feed_v3(c, off, 7, now, id);  // 恢復出幀、誤差 +1500 µs：第 4 則閘門開，到這裡 4 筆樣本
  ASSERT_EQ(r.phase, vr::latch::kPhaseMeasure);
  EXPECT_DOUBLE_EQ(r.integral_ppm, before);
  r = feed_v3(c, off, 1, now, id);
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_GT(r.integral_ppm - before, 0.0);
  EXPECT_LE(r.integral_ppm - before, controller_t::kKiV2 * err * 1.0 + 1e-9);
  const double first = r.integral_ppm;
  r = feed_v3(c, off, 1, now, id);
  EXPECT_NEAR(r.integral_ppm - first, controller_t::kKiV2 * err * 0.1, 1e-9);
}

TEST(VrLatchTest, SwitchModeDuringBurstKeepsBlind) {
  // 同場 A/B 剛好在 burst 進行中切換。bridge 那邊的 burst 還在走：換過去是 v3 時盲區照舊（不取樣本、不寫 pacing），
  // 盲區過了才重新量，而且是新的一輪（那一步不評估）。提早取樣本會量到移動中的相位。
  const double target = kT120 * 0.4;
  const int32_t slack_far = slack_for_err(target, 0.45 * kT120, kT120);  // 0.85T
  auto c = make_acquire(40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, slack_far, 10, now, id);
  ASSERT_EQ(r.step_why, vr::latch::kStepAcq);
  const int64_t blind_until = now + (int64_t) r.burst_us * 1000 + controller_t::kV3SettleNs;  // 約 1.19 s 後
  r = feed_v3(c, slack_far, 1, now, id);
  c.switch_mode(mode_e::v3, 55);
  EXPECT_TRUE(c.unsettled(now));
  int in_blind = 0;
  while (now + kStepNs < blind_until) {
    r = feed_v3(c, slack_far, 1, now, id);
    ++in_blind;
    EXPECT_FALSE(r.update) << "latch " << in_blind;
    EXPECT_EQ(r.burst_us, 0u);
    EXPECT_FALSE(r.eval_valid);
    EXPECT_EQ(r.phase, vr::latch::kPhaseWait);  // 還沒開始取樣本
  }
  EXPECT_GE(in_blind, 9);
  // 盲區後第一則：補寫基準、開始取樣本（閘門在盲區裡已經重新收滿）
  r = feed_v3(c, slack_far, 1, now, id);
  EXPECT_TRUE(r.update);
  EXPECT_EQ(r.ppm, 0);
  EXPECT_EQ(r.burst_us, 0u);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  // 收滿 5 筆：對新目標（55%）還差 0.30T → 從 acq 開始，沒有上一步的結果
  r = feed_v3(c, slack_far, 4, now, id);
  EXPECT_EQ(r.step_why, vr::latch::kStepAcq);
  EXPECT_FALSE(r.eval_valid);
  EXPECT_NEAR(r.step_err_us, 0.30 * kT120, 1.0);
  EXPECT_EQ(r.steps, 2u);

  // 換過去是 v2：它不看盲區，輸出一變就寫（bridge 收到一般寫入就取消還沒結束的 burst）
  auto d = make_acquire(40, kPeriod120);
  now = 0;
  id = 0;
  r = feed_v3(d, slack_far, 10, now, id);
  ASSERT_GT(r.burst_us, 0u);
  d.switch_mode(mode_e::v2, 50);
  r = feed_v3(d, slack_far, 1, now, id);
  EXPECT_EQ(r.mode, mode_e::v2);
  EXPECT_TRUE(r.update);
  EXPECT_EQ(r.burst_us, 0u);
  EXPECT_EQ(r.ppm, vr::latch::VR_LATCH_PPM_MAX);  // 對 v2 的目標（50%）早 0.35T：飽和
  // 馬上又換回 v3（原本的盲區還沒過）：照舊等到盲區結束
  d.switch_mode(mode_e::v3, 40);
  r = feed_v3(d, slack_far, 1, now, id);
  EXPECT_EQ(r.mode, mode_e::v3);
  EXPECT_FALSE(r.update);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
}

namespace {
  /// 理想的受控體（沒有延遲、抖動、時鐘漂移）：生效的修正是 p ppm 時，相位誤差每秒往 0 的方向移 p µs
  struct ideal_plant_t {
    double err_us = 0;  ///< 真正的相位誤差（正值＝影像太早到）
    int32_t base_ppm = 0;
    int32_t burst_ppm = 0;
    double burst_left_us = 0;

    void apply(const vr::latch::result_t &r) {
      if (!r.update) {
        return;
      }
      base_ppm = r.ppm;
      burst_ppm = r.burst_ppm;
      burst_left_us = (double) r.burst_us;  // 一般寫入（burst_us == 0）會取消還沒結束的 burst
    }

    void advance(double us) {
      const double in_burst = std::min(us, burst_left_us);
      err_us -= ((double) burst_ppm * in_burst + (double) base_ppm * (us - in_burst)) * 1e-6;
      burst_left_us -= in_burst;
    }
  };
}  // namespace

TEST(VrLatchTest, V3LowStepPpmCostsOneBackoff) {
  // 一輪最多兩步、每步最長 1.5 s，所以一輪最多移 3 µs／ppm。從反相點附近起步要移 0.35T 才進得了進入門檻：
  // step_ppm 低於 0.35T ÷ 3（120 Hz 972、90 Hz 1296、72 Hz 1620）時，就算每一步都照要求移到位，這一輪也會記成失敗，
  // 剩下的交給退避期間的 PI（±200 ppm）與退避之後的下一輪。不會卡住；預設 4000 與退路 2000 都不會遇到。
  constexpr uint32_t kPeriod72 = 13'888'889;
  struct case_t {
    uint32_t period;
    int step_ppm;
    uint32_t expect_bad;
  };
  const case_t cases[] = {
    {kPeriod120, 1000, 0},
    {kPeriod120, 2000, 0},
    {kPeriod90, 1000, 1},
    {kPeriod90, 1500, 0},
    {kPeriod90, 2000, 0},
    {kPeriod72, 1000, 1},
    {kPeriod72, 1500, 1},
    {kPeriod72, 2000, 0},
    {kPeriod72, 4000, 0},
  };
  for (const auto &k : cases) {
    SCOPED_TRACE(testing::Message() << "period=" << k.period << " step_ppm=" << k.step_ppm);
    const double T = k.period / 1000.0;
    const double target = T * 0.4;
    const auto per_latch = (uint32_t) std::lround((double) kStepNs / k.period);
    auto c = make_acquire(40, k.period, 0.0, k.step_ppm);
    ideal_plant_t plant;
    plant.err_us = 0.48 * T;  // 反相點附近
    int64_t now = 0;
    uint32_t id = 0;
    vr::latch::result_t r;
    uint32_t steps_at_first_bad = 0;
    for (int i = 0; i < 600; ++i) {  // 60 s
      now += kStepNs;
      id += per_latch;
      plant.advance(100'000.0);
      r = c.on_latch(slack_for_err(target, plant.err_us, T), k.period, now, id);
      plant.apply(r);
      if (r.bad_steps == 1 && steps_at_first_bad == 0) {
        steps_at_first_bad = r.steps;
        ASSERT_TRUE(r.eval_valid);
        EXPECT_EQ(r.eval_outcome, vr::latch::kEvalBackoff);
        EXPECT_GT(r.eval_moved_us, 0.9 * r.eval_cmd_us);  // 這一步其實照要求移到位了
      }
    }
    EXPECT_EQ(r.bad_steps, k.expect_bad);
    if (k.expect_bad != 0) {
      EXPECT_EQ(steps_at_first_bad, 2u);  // 兩步都被 1.5 s 的上限截短
      EXPECT_LE(r.steps, 4u);
    } else {
      EXPECT_LE(r.steps, 2u);
    }
    EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
    EXPECT_LT(std::abs(plant.err_us), 100.0);  // 最後都收斂
  }
}

TEST(VrLatchTest, V3RecoversAfterGarbage) {
  // 活性：任何亂七八糟的前綴之後，只要輸入恢復正常（幀率正常、相位固定在目標附近），控制器就回到追蹤、
  // 讀到正確的誤差、不再步進——沒有卡住出不來的狀態。
  std::mt19937_64 gen(987654321);
  const auto rnd = [&gen](uint64_t n) {
    return (uint64_t) (gen() % n);
  };
  for (int run = 0; run < 300; ++run) {
    const uint32_t period = rnd(2) ? kPeriod120 : kPeriod90;
    const double T = period / 1000.0;
    vr::latch::options_t opt;
    opt.acquire = rnd(4) != 0;
    opt.step_ppm = 1000 + (int) rnd(4001);
    opt.have_seed = rnd(2) == 0;
    opt.seed_integral_ppm = (double) rnd(200) - 100.0;
    const int pct = 25 + (int) rnd(41);
    controller_t c;
    c.configure(mode_e::v3, pct, period, opt);
    int64_t now = (int64_t) rnd(1'000'000'000) + 1;
    uint32_t id = (uint32_t) gen();
    for (int i = 0; i < 1500; ++i) {  // 前綴：idle、長短不一的間隔、重複／倒退／亂跳的 frameId、亂數的 slack
      if (rnd(100) < 4) {
        now += (int64_t) rnd(3'000'000'000);
        c.on_idle(now);
        continue;
      }
      const uint64_t tk = rnd(100);
      now += tk < 80 ? kStepNs : (tk < 95 ? (int64_t) rnd(400'000'000) : (int64_t) rnd(90'000'000'000));
      const uint64_t fk = rnd(100);
      if (fk < 75) {
        id += (uint32_t) (kStepNs / period);
      } else if (fk < 88) {
        id += (uint32_t) rnd(3);
      } else if (fk < 92) {
        id -= (uint32_t) rnd(50);
      } else if (fk < 95) {
        id = (uint32_t) gen();
      }
      int32_t slack = (int32_t) rnd(12000);
      if (rnd(50) == 0) {
        slack = (int32_t) gen();
      }
      c.on_latch(slack, period, now, id);
    }
    // 乾淨的 40 s：相位固定在目標 ± 0.1T 內，每則都是新幀
    const double target = T * pct / 100.0;
    const int32_t slack = slack_for_err(target, ((double) rnd(2001) - 1000.0) / 1000.0 * 0.1 * T, T);
    const double want_err = wrap_half(slack - target, T);
    vr::latch::result_t r;
    uint32_t steps_after_10s = 0;
    for (int i = 0; i < 400; ++i) {
      r = feed_v3(c, slack, 1, now, id, period);
      if (i == 100) {
        steps_after_10s = r.steps;  // 前 10 s 允許把前綴留下的東西（盲區、還沒量的步進）收掉
      }
    }
    SCOPED_TRACE(testing::Message() << "run " << run << " period=" << period << " pct=" << pct << " acquire=" << opt.acquire);
    EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
    EXPECT_TRUE(r.gate_open);
    EXPECT_NEAR(r.err_us, want_err, 1.0);
    EXPECT_EQ(r.steps, steps_after_10s);
    EXPECT_FALSE(c.unsettled(now));
  }
}

TEST(VrLatchTest, V3StateMachineInvariants) {
  // 亂數的事件序列（正常節拍為主，夾雜重複的 frameId、黑幀節拍、離群值、長短不一的間隔、LATCH 停止、負載改變），
  // 逐筆檢查狀態機的不變式（這裡不換更新率、不換模式——那兩種情況走到一半的步進不帶出結果）：
  //  - 每一步剛好帶出一次結果（track／trim／backoff／void），下一步之前一定已經帶出；
  //  - trim 的結果一定和下一步在同一筆；track／backoff 的那一筆在追蹤狀態；void 的那一筆在等待狀態、不動 bad；
  //  - bad 只在結果是 backoff 時加一，之後至少 5 s 不步進；
  //  - 步進的方向、長度、基準、門檻照規格；盲區內不寫 pacing；任何 60 s 內最多 8 步；
  //  - 過了盲區、閘門關著：不管這一則是不是新幀，狀態都是等待、輸出只有積分。
  std::mt19937_64 gen(0xBADC0FFEEull);
  const auto rnd = [&gen](uint64_t n) {
    return (uint64_t) (gen() % n);
  };
  uint64_t n_steps = 0;
  uint64_t n_bad = 0;
  uint64_t n_void = 0;
  uint64_t n_gate_closed = 0;
  uint64_t n_dup_closed = 0;
  int run = 0;
  int call = 0;
  // 一場的內容放在 lambda 裡：ASSERT 失敗時從這裡返回，外面再補上是哪一場的第幾次呼叫
  // （每次呼叫都放一個 SCOPED_TRACE 的話，這個測試的時間大半花在組訊息上）
  const auto one_run = [&]() {
    const uint32_t period = rnd(2) ? kPeriod120 : kPeriod90;
    const double T = period / 1000.0;
    const auto per_latch = (uint32_t) (kStepNs / period);
    vr::latch::options_t opt;
    opt.acquire = rnd(5) != 0;
    opt.step_ppm = 1000 + (int) rnd(4001);
    opt.have_seed = rnd(2) == 0;
    opt.seed_integral_ppm = (double) rnd(240) - 120.0;
    controller_t c;
    c.configure(mode_e::v3, 25 + (int) rnd(41), period, opt);
    int64_t now = 1'000'000'000 + (int64_t) rnd(1'000'000'000);
    uint32_t id = (uint32_t) gen();
    double center = (double) rnd((uint64_t) T);
    int64_t blind_until = 0;
    int64_t no_step_until = 0;
    uint32_t steps = 0;
    uint32_t bad = 0;
    bool step_unresolved = false;  // 已經步進、還沒帶出結果
    bool have_frame = false;  // 與控制器相同的 frameId 去重狀態
    uint32_t last_frame = 0;
    bool wrote = false;  // 寫過 pacing 了（之後「最後送出的值」才有意義）
    std::vector<int64_t> step_times;
    for (call = 0; call < 4000; ++call) {
      const uint64_t ev = rnd(1000);
      vr::latch::result_t r;
      bool fresh = false;  // 新幀、而且不是離群值：會走到盲區與閘門的判斷
      bool dup = false;
      if (ev < 20) {
        now += (int64_t) rnd(1'200'000'000);
        r = c.on_idle(now);
      } else if (ev < 23) {
        now += 2'000'000'000 + (int64_t) rnd(70'000'000'000);  // LATCH 停很久
        r = c.on_idle(now);
      } else {
        const uint64_t tk = rnd(100);
        now += tk < 90 ? kStepNs : (tk < 96 ? (int64_t) rnd(100'000'000) : (int64_t) (300'000'000 + rnd(400'000'000)));
        const uint64_t fk = rnd(100);
        if (fk < 86) {
          id += per_latch;
        } else if (fk >= 92 && fk < 98) {
          id += 1;  // 黑幀／掉拍的節拍
        } else if (fk >= 98) {
          id = (uint32_t) gen();  // 序號重來
        }  // 86～91：frameId 不動（重複）
        if (rnd(300) == 0) {
          center = (double) rnd((uint64_t) T);  // 負載改變
        }
        double s = center + (double) rnd(801) - 400.0;
        if (rnd(40) == 0) {
          s += T * 0.5;  // 離群的一筆
        }
        s = std::fmod(s + 4 * T, T);
        bool outlier = false;
        if (rnd(100) == 0) {
          s = 3.0 * T;
          outlier = true;
        }
        dup = have_frame && id == last_frame;
        if (!dup) {
          have_frame = true;
          last_frame = id;
          fresh = !outlier;
        }
        r = c.on_latch((int32_t) std::lround(s), period, now, id);
      }
      ASSERT_TRUE(std::isfinite(r.integral_ppm) && std::isfinite(r.err_us) && std::isfinite(r.center_us));
      ASSERT_LE(std::abs(r.ppm), vr::latch::VR_LATCH_PPM_MAX);
      ASSERT_LE(std::abs(r.integral_ppm), controller_t::kIntegralMax);
      const auto base = (int32_t) std::lround(r.integral_ppm);

      // 上一步的結果
      if (r.eval_valid) {
        ASSERT_TRUE(step_unresolved) << "a result without a step waiting for one";
        step_unresolved = false;
        switch (r.eval_outcome) {
          case vr::latch::kEvalTrim:
            ASSERT_GT(r.burst_us, 0u);
            ASSERT_EQ(r.step_why, vr::latch::kStepTrim);
            ASSERT_EQ(r.bad_steps, bad);
            break;
          case vr::latch::kEvalTrack:
            ASSERT_EQ(r.burst_us, 0u);
            ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
            ASSERT_EQ(r.bad_steps, bad);
            break;
          case vr::latch::kEvalBackoff:
            ASSERT_EQ(r.burst_us, 0u);
            ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
            ASSERT_EQ(r.bad_steps, bad + 1);
            ASSERT_GT(std::abs(r.eval_err_us), 0.15 * T - 1e-6);
            no_step_until = now + controller_t::kV3BackoffNs;  // 至少 5 s
            ++n_bad;
            break;
          case vr::latch::kEvalVoid:
            ASSERT_EQ(r.burst_us, 0u);
            ASSERT_EQ(r.phase, vr::latch::kPhaseWait);
            ASSERT_EQ(r.bad_steps, bad);
            ASSERT_EQ(r.eval_moved_us, 0.0);
            ASSERT_EQ(r.eval_err_us, 0.0);
            ASSERT_GE(now, blind_until);
            ++n_void;
            break;
          default:
            FAIL() << "unknown outcome " << (int) r.eval_outcome;
        }
      } else {
        ASSERT_EQ(r.bad_steps, bad) << "bad_steps changed without a result";
      }
      bad = r.bad_steps;

      // 步進
      if (r.burst_us > 0) {
        ++n_steps;
        ASSERT_TRUE(opt.acquire);
        ASSERT_TRUE(fresh);
        ASSERT_TRUE(r.update);
        ASSERT_EQ(r.steps, steps + 1);
        ASSERT_EQ(r.phase, vr::latch::kPhaseBurst);
        ASSERT_TRUE(r.gate_open);
        ASSERT_GE(now, blind_until);
        ASSERT_GE(now, no_step_until) << "stepped during the back-off";
        ASSERT_FALSE(step_unresolved) << "a new step while the previous one never got a result";
        ASSERT_EQ(r.ppm, base);
        ASSERT_EQ(std::abs(r.burst_ppm - r.ppm), opt.step_ppm);
        ASSERT_GT((double) (r.burst_ppm - r.ppm) * r.step_err_us, 0.0);
        ASSERT_GT(std::abs(r.step_err_us), (r.step_why == vr::latch::kStepTrim ? 0.12 : 0.15) * T - 1e-6);
        ASSERT_EQ((int64_t) r.burst_us, std::min<int64_t>(std::llround(std::abs(r.step_err_us) * 1e6 / opt.step_ppm), controller_t::kV3StepMaxUs));
        step_unresolved = true;
        step_times.push_back(now);
        blind_until = now + (int64_t) r.burst_us * 1000 + controller_t::kV3SettleNs;
        const auto recent = std::count_if(step_times.begin(), step_times.end(), [now](int64_t t) {
          return now - t < controller_t::kV3StepWindowNs;
        });
        ASSERT_LE(recent, 8);
      } else {
        ASSERT_EQ(r.steps, steps);
        if (now < blind_until) {
          ASSERT_FALSE(r.update) << "pacing written inside the blind window";
        }
      }
      steps = r.steps;
      wrote = wrote || r.update;

      // 過了盲區、閘門關著
      if (now >= blind_until && r.burst_us == 0 && !r.gate_open) {
        if (fresh) {
          ++n_gate_closed;
          ASSERT_EQ(r.phase, vr::latch::kPhaseWait);
          ASSERT_EQ(r.ppm, base);
        } else if (dup) {
          ++n_dup_closed;
          ASSERT_EQ(r.phase, vr::latch::kPhaseWait);
          if (wrote) {
            ASSERT_LE(std::abs(r.ppm - base), 1);  // 最後送出的值：和積分差不到 kMinDelta
          }
        }
      }
      if (fresh && r.phase == vr::latch::kPhaseTrack) {
        ASSERT_TRUE(r.gate_open);
      }
    }
  };
  for (run = 0; run < 200; ++run) {
    one_run();
    if (HasFatalFailure()) {
      FAIL() << "run " << run << " call " << call;
    }
  }
  // 這組輸入確實走到要檢查的路徑（實際約 10000 步、960 次判失敗、6900 次作廢、閘門關著的新幀 23 萬則、重複的 2.4 萬則）
  EXPECT_GT(n_steps, 2000u);
  EXPECT_GT(n_bad, 100u);
  EXPECT_GT(n_void, 1000u);
  EXPECT_GT(n_gate_closed, 10000u);
  EXPECT_GT(n_dup_closed, 2000u);
}

// ── session_state_t 的 LATCH 介面（stream.cpp 只經由這一層使用控制器）──────────────────

namespace {
  VIPLE_VR_TLV_LATCH make_latch(uint32_t frame_id, int32_t slack_us, uint32_t period_ns = kPeriod120) {
    VIPLE_VR_TLV_LATCH l {};
    l.frameId = frame_id;
    l.slackUs = slack_us;
    l.displayPeriodNs = period_ns;
    return l;
  }
}  // namespace

TEST(VrLatchTest, SessionLatchLegacyAndV2Unaffected) {
  // 沒有 configure_latch＝legacy：v3 的介面一律回「沒事」，結果與快照不帶 v3 的欄位
  vr::session_state_t st {vr::negotiated_t {}, nullptr, 0};
  EXPECT_FALSE(st.latch_unsettled(0));
  EXPECT_FALSE(st.latch_learned(0).has_value());
  auto snap = st.latch_snapshot();
  EXPECT_EQ(snap.mode, mode_e::legacy);
  EXPECT_EQ(snap.phase, vr::latch::kPhaseWait);
  EXPECT_EQ(snap.burst_us, 0u);
  EXPECT_FALSE(snap.eval_valid);

  // v2（帶不帶 options 都一樣）：每一則的結果與另一個沒帶 options 的 session 相同
  vr::latch::options_t opt;
  opt.acquire = true;
  opt.have_seed = true;
  opt.seed_integral_ppm = -46.0;
  vr::session_state_t a {vr::negotiated_t {}, nullptr, 0};
  vr::session_state_t b {vr::negotiated_t {}, nullptr, 0};
  a.configure_latch(mode_e::v2, 40, kPeriod120);
  b.configure_latch(mode_e::v2, 40, kPeriod120, opt);
  EXPECT_EQ(b.latch_snapshot().mode, mode_e::v2);
  int64_t now = 10'000'000'000;
  uint32_t id = 1000;
  for (int i = 0; i < 700; ++i) {
    now += kStepNs;
    id += 12;
    const int32_t slack = 3333 + (i % 7) * 150;
    const auto ra = a.on_latch(make_latch(id, slack), now);
    const auto rb = b.on_latch(make_latch(id, slack), now);
    ASSERT_EQ(ra.update, rb.update);
    ASSERT_EQ(ra.ppm, rb.ppm);
    ASSERT_DOUBLE_EQ(ra.integral_ppm, rb.integral_ppm);
    ASSERT_EQ(rb.mode, mode_e::v2);
    ASSERT_EQ(rb.phase, vr::latch::kPhaseWait);
    ASSERT_FALSE(rb.gate_open);
    ASSERT_EQ(rb.burst_us, 0u);
    ASSERT_FALSE(rb.eval_valid);
    ASSERT_FALSE(b.latch_unsettled(now));
    ASSERT_FALSE(b.latch_learned(now).has_value());
  }
  const auto ia = a.latch_idle(now + 3'000'000'000);
  const auto ib = b.latch_idle(now + 3'000'000'000);
  EXPECT_TRUE(ib.update);  // v2 的 idle 照舊：2 s 之後開始衰減
  EXPECT_EQ(ia.ppm, ib.ppm);
}

TEST(VrLatchTest, SessionLatchV3Interface) {
  vr::latch::reset_learned_for_test();
  vr::latch::options_t opt;
  opt.acquire = true;
  opt.have_seed = true;
  opt.seed_integral_ppm = -46.0;
  const auto on_target = (int32_t) std::lround(kT120 * 0.4);
  int64_t now = 10'000'000'000;
  uint32_t id = 1000;

  // 相位在目標上：第一次寫入＝積分的起點；進追蹤滿 3 s 才算穩定；不步進
  {
    vr::session_state_t st {vr::negotiated_t {}, nullptr, 0};
    st.configure_latch(mode_e::v3, 40, kPeriod120, opt);
    EXPECT_EQ(st.latch_snapshot().mode, mode_e::v3);
    EXPECT_TRUE(st.latch_unsettled(now));
    bool have_first = false;
    int64_t track_at = 0;
    for (int i = 0; i < 40; ++i) {
      now += kStepNs;
      id += 12;
      const auto r = st.on_latch(make_latch(id, on_target), now);
      if (!have_first && r.update) {
        have_first = true;
        EXPECT_EQ(r.ppm, -46);
      }
      EXPECT_EQ(r.burst_us, 0u);
      if (track_at == 0 && r.phase == vr::latch::kPhaseTrack) {
        track_at = now;
      }
      EXPECT_EQ(st.latch_unsettled(now), track_at == 0 || now - track_at < controller_t::kV3SettledNs) << "latch " << i;
    }
    EXPECT_TRUE(have_first);
    EXPECT_NE(track_at, 0);
    EXPECT_FALSE(st.latch_learned(now).has_value());  // 要連續追蹤滿 60 s

    // switch_latch：積分留著、快照馬上反映新的模式與等待狀態；v2 不會「不穩定」；再換回 v3 從等待重新開始
    const double integ = st.latch_snapshot().integral_ppm;
    st.switch_latch(mode_e::v2, 50);
    auto snap = st.latch_snapshot();
    EXPECT_EQ(snap.mode, mode_e::v2);
    EXPECT_EQ(snap.phase, vr::latch::kPhaseWait);
    EXPECT_FALSE(snap.gate_open);
    EXPECT_FALSE(st.latch_unsettled(now));
    now += kStepNs;
    id += 12;
    auto r = st.on_latch(make_latch(id, (int32_t) std::lround(kT120 * 0.5)), now);
    EXPECT_EQ(r.mode, mode_e::v2);
    EXPECT_NEAR(r.integral_ppm, integ, 1.0);
    EXPECT_NEAR(r.target_us, kT120 * 0.5, 0.5);
    st.switch_latch(mode_e::v3, 65);
    EXPECT_TRUE(st.latch_unsettled(now));
    EXPECT_EQ(st.latch_snapshot().mode, mode_e::v3);
    now += kStepNs;
    id += 12;
    r = st.on_latch(make_latch(id, (int32_t) std::lround(kT120 * 0.65)), now);
    EXPECT_EQ(r.mode, mode_e::v3);
    EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
    EXPECT_NEAR(r.target_us, kT120 * 0.65, 0.5);  // v3 的目標上限是 65%
    EXPECT_NEAR(r.integral_ppm, integ, 1.0);
  }

  // 反相點起步：回傳帶 burst 的結果；之後的快照不重播 burst（10 s log 與例行讀取不會再觸發一次）；
  // 盲區內 LATCH 與 1 s 的 idle 都不寫；LATCH 停了，那一步的結果（void）經由 latch_idle 帶出一次
  {
    vr::session_state_t st {vr::negotiated_t {}, nullptr, 0};
    st.configure_latch(mode_e::v3, 40, kPeriod120, opt);
    const auto anti = (int32_t) std::lround(kT120 * 0.85);  // 離目標 0.45T
    vr::latch::result_t r;
    for (int i = 0; i < 10; ++i) {
      now += kStepNs;
      id += 12;
      r = st.on_latch(make_latch(id, anti), now);
    }
    ASSERT_GT(r.burst_us, 0u);
    EXPECT_TRUE(r.update);
    EXPECT_EQ(r.step_why, vr::latch::kStepAcq);
    EXPECT_EQ(r.ppm, -46);  // 基準＝積分（起點）
    EXPECT_EQ(r.burst_ppm, -46 + 4000);
    const int64_t blind_until = now + (int64_t) r.burst_us * 1000 + controller_t::kV3SettleNs;
    auto snap = st.latch_snapshot();
    EXPECT_EQ(snap.burst_us, 0u);
    EXPECT_EQ(snap.burst_ppm, 0);
    EXPECT_EQ(snap.step_why, 0);
    EXPECT_FALSE(snap.eval_valid);
    EXPECT_FALSE(snap.update);
    EXPECT_EQ(snap.phase, vr::latch::kPhaseBurst);
    EXPECT_EQ(snap.steps, 1u);
    EXPECT_EQ(snap.ppm, -46);
    EXPECT_TRUE(st.latch_unsettled(now));
    while (now + kStepNs < blind_until) {
      now += kStepNs;
      id += 12;
      r = st.on_latch(make_latch(id, anti), now);
      EXPECT_FALSE(r.update);
      EXPECT_FALSE(st.latch_idle(now + 50'000'000).update);
    }
    // LATCH 到這裡停了
    auto ri = st.latch_idle(now + 1'900'000'000);  // 還沒滿 2 s
    EXPECT_FALSE(ri.update);
    EXPECT_FALSE(ri.eval_valid);
    ri = st.latch_idle(now + 2'100'000'000);
    EXPECT_TRUE(ri.update);
    EXPECT_EQ(ri.ppm, -46);
    ASSERT_TRUE(ri.eval_valid);
    EXPECT_EQ(ri.eval_outcome, vr::latch::kEvalVoid);
    snap = st.latch_snapshot();
    EXPECT_EQ(snap.phase, vr::latch::kPhaseWait);  // idle 有更新 pacing 時，快照跟著換
    EXPECT_FALSE(snap.eval_valid);
    EXPECT_FALSE(st.latch_idle(now + 3'100'000'000).eval_valid);
  }

  // latch_learned：連續追蹤滿 60 s 才有值，而且這一層不自己記（remember 由 stream.cpp 在 10 s log 的地方呼叫）
  {
    vr::latch::options_t seed;
    seed.have_seed = true;
    seed.seed_integral_ppm = -40.0;
    vr::session_state_t st {vr::negotiated_t {}, nullptr, 0};
    st.configure_latch(mode_e::v3, 40, kPeriod120, seed);
    int64_t track_at = 0;
    int64_t learned_at = 0;
    double learned = 0;
    for (int i = 0; i < 700; ++i) {
      now += kStepNs;
      id += 12;
      const auto r = st.on_latch(make_latch(id, on_target), now);
      if (track_at == 0 && r.phase == vr::latch::kPhaseTrack) {
        track_at = now;
      }
      if (const auto l = st.latch_learned(now); l && learned_at == 0) {
        learned_at = now;
        learned = *l;
      }
    }
    ASSERT_NE(track_at, 0);
    ASSERT_NE(learned_at, 0);
    EXPECT_EQ(learned_at - track_at, controller_t::kV3LearnTrackNs);
    EXPECT_NEAR(learned, -40.0, 2.0);
    EXPECT_FALSE(vr::latch::recall(kPeriod120).has_value());
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

// ── §VR-FENCE-LATE：host 擷取端過載時的 hold 入口（on_hold／hold_latch）──────────────

TEST(VrLatchTest, HoldLeavesOnlyTheIntegralForLegacyAndV2) {
  // legacy 與 v2：過載期間（這裡 60 s）LATCH 照來、但走 on_hold——比例項拿掉、輸出只留積分（學到的時鐘差），
  // 積分一動也不動；和平常一樣每秒例行重送一次；每秒的 on_idle 不會把它當成「LATCH 停了」而每秒減半
  for (const auto mode : {mode_e::legacy, mode_e::v2}) {
    SCOPED_TRACE(vr::latch::mode_name(mode));
    controller_t c;
    if (mode == mode_e::v2) {
      c.configure(mode_e::v2, 40, kPeriod120);
    }
    const double target = mode == mode_e::v2 ? kT120 * 0.4 : controller_t::target_for_period(kPeriod120);
    const auto slack = (int32_t) std::lround(target + 800.0);  // 誤差 +800 µs
    int64_t now = 0;
    uint32_t id = 0;
    vr::latch::result_t r;
    for (int i = 0; i < 50; ++i) {
      now += kStepNs;
      r = c.on_latch(slack, kPeriod120, now, ++id);
    }
    const int32_t ppm = c.last_sent_ppm();
    const double integ = r.integral_ppm;
    const auto base = (int32_t) std::lround(integ);
    ASSERT_GT(integ, 1.0);
    ASSERT_GT(ppm, base + 20);  // 比例項：legacy 0.05 × 800＝40 ppm，v2 0.12 × 800＝96 ppm

    // 第一則 hold 就把比例項拿掉
    now += kStepNs;
    auto h = c.on_hold(now, ++id);
    EXPECT_TRUE(h.update);
    EXPECT_EQ(h.ppm, base);
    EXPECT_DOUBLE_EQ(h.integral_ppm, integ);
    EXPECT_NEAR(h.err_us, 0.0, 1e-9);  // 誤差的平滑值作廢
    EXPECT_EQ(h.mode, mode);

    int updates = 0;
    for (int i = 0; i < 600; ++i) {
      now += kStepNs;
      h = c.on_hold(now, ++id);
      ASSERT_EQ(h.ppm, base) << i;
      ASSERT_DOUBLE_EQ(h.integral_ppm, integ) << i;
      ASSERT_EQ(h.mode, mode);
      updates += h.update ? 1 : 0;
      if (i % 10 == 9) {
        const auto ri = c.on_idle(now + 50'000'000);
        ASSERT_FALSE(ri.update) << i;  // 沒有被當成 LATCH 停了
        ASSERT_DOUBLE_EQ(ri.integral_ppm, integ) << i;
      }
    }
    EXPECT_GE(updates, 59);  // 每秒例行重送一次，不是每則都寫
    EXPECT_LE(updates, 61);
    EXPECT_EQ(c.last_sent_ppm(), base);

    // 恢復，誤差換成 −600 µs：平滑值從這一筆重新開始（接著過載前的 +800 走的話會是 +520）；
    // 積分的 dt 是距上一則 LATCH 的 0.1 s，不是整段暫停（夾在 1 s）
    now += kStepNs;
    r = c.on_latch((int32_t) std::lround(target - 600.0), kPeriod120, now, ++id);
    const double ki = mode == mode_e::v2 ? controller_t::kKiV2 : controller_t::kKi;
    EXPECT_NEAR(r.err_us, -600.0, 1.0);
    EXPECT_NEAR(r.integral_ppm - integ, ki * -600.0 * 0.1, 0.02);

    // 對照：同樣的歷史、暫停期間什麼都不呼叫 → 3 s 後的 idle 就把輸出砍半（這就是需要 hold 入口的原因）
    controller_t idle;
    if (mode == mode_e::v2) {
      idle.configure(mode_e::v2, 40, kPeriod120);
    }
    int64_t t = 0;
    uint32_t k = 0;
    for (int i = 0; i < 50; ++i) {
      t += kStepNs;
      idle.on_latch(slack, kPeriod120, t, ++k);
    }
    const auto ri = idle.on_idle(t + 3'000'000'000LL);
    EXPECT_TRUE(ri.update);
    EXPECT_EQ(ri.ppm, ppm / 2);
  }

  // v2：過載期間量的那一張之後被 client 重送（同一個 frameId）仍然當重複丟掉；隔著暫停的兩筆量測不比相位滑移
  controller_t v;
  v.configure(mode_e::v2, 40, kPeriod120);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v2(v, 1000, 5, now, id, kPeriod120);
  const uint64_t dropped = r.dropped;
  now += kStepNs;
  v.on_hold(now, 777);
  now += kStepNs;
  r = v.on_latch(7000, kPeriod120, now, 777);
  EXPECT_EQ(r.dropped, dropped + 1);
  EXPECT_FALSE(r.update);
  now += kStepNs;
  r = v.on_latch(7000, kPeriod120, now, 778);  // 距暫停前那一筆跳了 6000 µs（> 0.6 個週期）
  EXPECT_EQ(r.slips, 0u);
  controller_t w;  // 對照：沒有暫停的話，同樣的一跳算一次滑移
  w.configure(mode_e::v2, 40, kPeriod120);
  now = 0;
  id = 0;
  feed_v2(w, 1000, 5, now, id, kPeriod120);
  r = feed_v2(w, 7000, 1, now, id, kPeriod120);
  EXPECT_EQ(r.slips, 1u);
}

TEST(VrLatchTest, HoldDoesNotFreezeASkewedV2Output) {
  // 過載旗標要看滿一段時間才設，設起來之前已經有幾筆過載的樣本進了控制器。v2 原本鎖在目標上（ppm 0），漏進 3 筆
  // 「晚 4 ms」的樣本就把輸出推到 −200，積分只被帶走不到 2 ppm。hold 不能把 −200 凍住（5 分鐘會漂 60 ms、七個週期）：
  // 輸出回到積分
  controller_t c;
  c.configure(mode_e::v2, 40, kPeriod120);
  const double target = kT120 * 0.4;
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v2(c, (int32_t) std::lround(target), 300, now, id, kPeriod120);  // 鎖住 30 s
  ASSERT_EQ(r.ppm, 0);
  r = feed_v2(c, slack_for_err(target, -4000.0, kT120), 3, now, id, kPeriod120);
  ASSERT_EQ(r.ppm, -200);
  ASSERT_GT(r.integral_ppm, -2.5);
  const auto base = (int32_t) std::lround(r.integral_ppm);
  for (int i = 0; i < 3000; ++i) {  // 過載 5 分鐘
    now += kStepNs;
    r = c.on_hold(now, ++id);
    ASSERT_EQ(r.ppm, base) << i;
    if (i % 10 == 9) {
      c.on_idle(now + 50'000'000);
    }
  }
  EXPECT_EQ(c.last_sent_ppm(), base);
  EXPECT_LE(std::abs(base), 2);

  // 過載結束、相位還在原地（誤差 0）：第一筆樣本的輸出就是積分，不必先把髒的平滑值洗掉
  r = feed_v2(c, (int32_t) std::lround(target), 1, now, id, kPeriod120);
  EXPECT_NEAR(r.err_us, 0.0, 1.0);
  EXPECT_LE(std::abs(r.ppm), 2);
}

TEST(VrLatchTest, HoldV3ActsLikeAClosedGate) {
  controller_t c;
  c.configure(mode_e::v3, 40, kPeriod120);
  const double target = kT120 * 0.4;
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, slack_for_err(target, 800.0, kT120), 60, now, id);  // 誤差 +800 µs 約 5 s
  ASSERT_EQ(r.phase, vr::latch::kPhaseTrack);
  const double integ = r.integral_ppm;
  const auto base = (int32_t) std::lround(integ);
  ASSERT_GT(integ, 20.0);
  ASSERT_GT(r.ppm, base + 50);  // 比例項約 96 ppm

  // 第一則 hold：比例項拿掉（輸出＝積分）、回到等待、閘門關，積分原封不動
  now += kStepNs;
  id += 12;
  auto h = c.on_hold(now, id);
  EXPECT_TRUE(h.update);
  EXPECT_EQ(h.ppm, base);
  EXPECT_DOUBLE_EQ(h.integral_ppm, integ);
  EXPECT_EQ(h.phase, vr::latch::kPhaseWait);
  EXPECT_FALSE(h.gate_open);
  EXPECT_EQ(h.burst_us, 0u);
  EXPECT_FALSE(h.eval_valid);
  EXPECT_TRUE(c.unsettled(now));  // §VR-PREDICT 這時也不採樣
  EXPECT_FALSE(c.learned(now).has_value());

  // 過載持續 120 s（超過 v3「LATCH 停滿 60 s 積分才衰減」的門檻）：積分一直都在，輸出一直是積分
  int updates = 0;
  for (int i = 0; i < 1200; ++i) {
    now += kStepNs;
    id += 10;  // 過載時幀率比較低
    h = c.on_hold(now, id);
    ASSERT_DOUBLE_EQ(h.integral_ppm, integ) << i;
    ASSERT_EQ(h.ppm, base) << i;
    ASSERT_EQ(h.phase, vr::latch::kPhaseWait) << i;
    ASSERT_FALSE(h.gate_open) << i;
    updates += h.update ? 1 : 0;
    if (i % 10 == 9) {
      const auto ri = c.on_idle(now + 50'000'000);
      ASSERT_FALSE(ri.update) << i;
      ASSERT_DOUBLE_EQ(ri.integral_ppm, integ) << i;
    }
  }
  EXPECT_GE(updates, 118);  // 和平常一樣每秒例行重送一次，不是每則都寫
  EXPECT_LE(updates, 121);
  EXPECT_FALSE(c.learned(now).has_value());  // 過載期間不會產生「可以帶到下一場」的積分

  // 恢復，相位換了地方（−600 µs）：閘門重新收滿 5 個間隔、再量 5 筆才進追蹤；暫停前的樣本（+800）沒有混進來
  const int32_t slack_after = slack_for_err(target, -600.0, kT120);
  r = feed_v3(c, slack_after, 4, now, id);
  EXPECT_FALSE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  EXPECT_EQ(r.ppm, base);
  r = feed_v3(c, slack_after, 1, now, id);
  EXPECT_TRUE(r.gate_open);
  EXPECT_EQ(r.phase, vr::latch::kPhaseMeasure);
  r = feed_v3(c, slack_after, 4, now, id);
  EXPECT_EQ(r.phase, vr::latch::kPhaseTrack);
  EXPECT_NEAR(r.err_us, -600.0, 1.0);
}

TEST(VrLatchTest, HoldV3VoidsAStepInFlight) {
  // 定時步進走到一半 host 過載：那一步量不到結果（void，只帶出一次、不算失敗）；盲區內照舊不寫 pacing；
  // 恢復後是新的一輪（從 acq 開始）
  const double target = kT120 * 0.4;
  const int32_t slack_far = slack_for_err(target, 3000.0, kT120);
  auto c = make_acquire(40, kPeriod120, -46.0);
  int64_t now = 0;
  uint32_t id = 0;
  auto r = feed_v3(c, slack_far, 10, now, id);
  ASSERT_EQ(r.step_why, vr::latch::kStepAcq);
  ASSERT_NEAR((double) r.burst_us, 750'000.0, 130.0);  // 盲區 1.0 s：之後 9 則在裡面

  int voids = 0;
  for (int i = 1; i <= 9; ++i) {
    now += kStepNs;
    id += 12;
    const auto h = c.on_hold(now, id);
    EXPECT_FALSE(h.update) << "blind " << i;
    EXPECT_EQ(h.phase, vr::latch::kPhaseWait);
    if (h.eval_valid) {
      ++voids;
      EXPECT_EQ(i, 1);
      EXPECT_EQ(h.eval_outcome, vr::latch::kEvalVoid);
      EXPECT_NEAR(h.eval_cmd_us, 3000.0, 1.0);
    }
  }
  EXPECT_EQ(voids, 1);

  // 盲區過了：輸出＝基準（積分的起點）
  now += kStepNs;
  id += 12;
  auto h = c.on_hold(now, id);
  EXPECT_TRUE(h.update);
  EXPECT_EQ(h.ppm, -46);
  EXPECT_EQ(h.burst_us, 0u);
  EXPECT_FALSE(h.eval_valid);
  EXPECT_EQ(h.steps, 1u);
  EXPECT_EQ(h.bad_steps, 0u);

  // 恢復、誤差還是 +3000 µs：閘門重新開（5 個間隔）、量 5 筆之後重新步進，而且是新的一輪
  bool stepped = false;
  for (int i = 0; i < 20 && !stepped; ++i) {
    r = feed_v3(c, slack_far, 1, now, id);
    EXPECT_FALSE(r.eval_valid) << "latch " << i;  // 作廢的那一步不會再被拿出來評估
    if (r.burst_us > 0) {
      stepped = true;
      EXPECT_EQ(i, 8);  // 第 5 則閘門開、收第 1 筆；第 9 則收滿 5 筆
      EXPECT_EQ(r.step_why, vr::latch::kStepAcq);
      EXPECT_EQ(r.steps, 2u);
    }
  }
  EXPECT_TRUE(stepped);
}

TEST(VrLatchTest, HoldBeforeAnyLatchIsHarmless) {
  // session 一開始就過載（還沒餵過任何樣本）：三種模式都把積分的起點寫出去——legacy／v2 是 0（標稱週期），
  // 和平常第一則 LATCH 一定會寫一次 pacing 一樣
  controller_t legacy;
  auto r = legacy.on_hold(1'000'000'000, 5);
  EXPECT_TRUE(r.update);
  EXPECT_EQ(r.ppm, 0);
  EXPECT_DOUBLE_EQ(r.integral_ppm, 0.0);
  EXPECT_EQ(r.mode, mode_e::legacy);
  r = legacy.on_hold(1'100'000'000, 6);
  EXPECT_FALSE(r.update);  // 沒有變化、也還沒到例行重送
  EXPECT_EQ(r.ppm, 0);

  controller_t v2;
  v2.configure(mode_e::v2, 40, kPeriod120);
  r = v2.on_hold(1'000'000'000, 5);
  EXPECT_TRUE(r.update);
  EXPECT_EQ(r.ppm, 0);
  EXPECT_EQ(r.mode, mode_e::v2);
  // 過載一結束，第一筆樣本照常進控制器（誤差 +1000 µs → 比例項 120 ppm）
  r = v2.on_latch((int32_t) std::lround(kT120 * 0.4 + 1000.0), kPeriod120, 1'100'000'000, 6);
  EXPECT_TRUE(r.update);
  EXPECT_NEAR(r.err_us, 1000.0, 1.0);
  EXPECT_NEAR(r.ppm, 120, 2);

  // v3：和「閘門關著的第一則 LATCH」一樣，把積分的起點寫出去
  vr::latch::options_t opt;
  opt.have_seed = true;
  opt.seed_integral_ppm = -46.0;
  controller_t v3;
  v3.configure(mode_e::v3, 40, kPeriod120, opt);
  r = v3.on_hold(1'000'000'000, 5);
  EXPECT_TRUE(r.update);
  EXPECT_EQ(r.ppm, -46);
  EXPECT_EQ(r.phase, vr::latch::kPhaseWait);
  EXPECT_FALSE(r.eval_valid);
}

TEST(VrLatchTest, SessionHoldLatchCountsAndLeavesTheIntegral) {
  vr::session_state_t st {vr::negotiated_t {}, nullptr, 0};
  st.configure_latch(mode_e::v2, 40, kPeriod120);
  int64_t now = 10'000'000'000;
  uint32_t id = 1000;
  for (int i = 0; i < 50; ++i) {
    now += kStepNs;
    id += 12;
    st.on_latch(make_latch(id, 4200), now);
  }
  const auto before = st.latch_snapshot();
  const auto base = (int32_t) std::lround(before.integral_ppm);
  ASSERT_GT(before.ppm, base + 20);  // 誤差約 +867 µs：比例項約 104 ppm
  EXPECT_EQ(st.stats().latch_held, 0u);

  // 過載 30 s：slack 亂跳（GPU 排隊），一筆都不進控制器；輸出＝積分，每秒例行重送一次
  int updates = 0;
  for (int i = 0; i < 300; ++i) {
    now += kStepNs;
    id += 9;
    const auto h = st.hold_latch(make_latch(id, -3000 + (i % 5) * 1500), now);
    ASSERT_EQ(h.ppm, base) << i;
    updates += h.update ? 1 : 0;
    if (i % 10 == 9) {
      ASSERT_FALSE(st.latch_idle(now + 10'000'000).update) << i;
    }
  }
  EXPECT_GE(updates, 30);
  EXPECT_LE(updates, 31);
  EXPECT_EQ(st.stats().latch_held, 300u);
  const auto after = st.latch_snapshot();
  EXPECT_EQ(after.ppm, base);
  EXPECT_DOUBLE_EQ(after.integral_ppm, before.integral_ppm);
  EXPECT_NEAR(after.err_us, 0.0, 1e-9);  // 誤差的平滑值作廢，log 的 err／slackEma 印 0／目標
  EXPECT_EQ(after.dropped, before.dropped);  // 被擋掉的另外計（latch_held），不算進 dropped
  EXPECT_EQ(after.slips, before.slips);
  EXPECT_EQ(after.mode, mode_e::v2);
}
