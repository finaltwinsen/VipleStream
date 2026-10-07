/**
 * @file src/vr/vr_predict.cpp
 * @brief VipleStream 2.0 §VR-PREDICT：見 vr_predict.h。
 */
#include "vr_predict.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace vr::predict {

  namespace {
    using steady_t = std::chrono::steady_clock;

    constexpr uint16_t k_reliable_count = 60;  ///< 1 s 內至少這麼多樣本的視窗才用（樣本少的中位數不可靠）
    constexpr int32_t k_input_clamp_us = 20000;  ///< 每個視窗先夾到 ±20 ms（卡頓那一秒可到 +147 ms）
    constexpr int k_probe_windows = 3;  ///< 探測前、後各取幾個視窗平均
    constexpr auto k_settle = std::chrono::seconds(2);  ///< 探測改值後等多久才量
    constexpr int32_t k_probe_need_us = 1500;  ///< 平均落後 < 1.5 ms：不必探測（已經夠準）
    constexpr int32_t k_probe_min_us = 8000;  ///< 探測步長：夠大才分得出雜訊（3 視窗平均的差約 ±2.9 ms）
    constexpr int32_t k_probe_max_us = 10000;
    constexpr double k_ema_alpha = 0.25;  ///< 追蹤：落後的指數移動平均（每秒一筆，時間常數約 4 s）
    constexpr double k_track_gain = 0.5;
    constexpr int32_t k_track_deadband_us = 1000;
    constexpr int32_t k_track_max_step_us = 3000;
    constexpr int32_t k_session_drift_us = 40000;  ///< 一個 session 內最多偏離起始值 ±40 ms（防失控）
    constexpr auto k_summary_interval = std::chrono::seconds(10);
    constexpr double k_same_period_tol = 0.03;  ///< 週期差 ≤ 3% 視為同一個更新率
    constexpr int k_ignored_after = 2;  ///< 連續幾個 session 判不採用，才當成 runtime 不採用

    enum class runtime_e {
      unknown,
      honored,
      ignored,
    };

    enum class phase_e {
      off,  ///< 固定值、或本 session 已停調
      measure,  ///< 探測前：累積 3 個視窗
      settle,  ///< 探測改值後等 2 s
      verify,  ///< 探測後：累積 3 個視窗
      learn,  ///< SteamVR 不採用中途改值：只量，推算下一個 session 的起始值
      track,  ///< 已確認採用：移動平均小幅修正
    };

    struct learned_t {
      uint32_t period_us;
      uint32_t v2p_us;
    };

    struct window_t {
      int32_t mean_us;
      int32_t spread_us;  ///< 3 個視窗的最大差
    };

    std::mutex g_mtx;
    phase_e g_phase = phase_e::off;
    runtime_e g_runtime = runtime_e::unknown;  ///< 行程內：SteamVR 是否採用中途改值
    int g_not_honored_streak = 0;  ///< 連續判不採用的 session 數
    uint32_t g_period_us = 0;  ///< 目前 session 的顯示週期
    uint32_t g_start_us = 0;  ///< session 起始值（SteamVR 一定在用的值）
    uint32_t g_cur_us = 0;  ///< 目前送給 driver 的值
    std::vector<learned_t> g_learned;  ///< 行程內學到的值（依週期分開）：下一個 session 的起始值
    int g_warm = 0;  ///< 本 session 已丟掉的暖機視窗
    int64_t g_sum_us = 0;  ///< measure／verify／learn 的累積
    int g_n = 0;
    int32_t g_win_min_us = 0;
    int32_t g_win_max_us = 0;
    int32_t g_l0_us = 0;  ///< 探測前的平均落後
    int32_t g_probe_us = 0;  ///< 探測實際改的量
    steady_t::time_point g_probe_at {};
    double g_ema_us = 0;
    // 10 s 摘要
    steady_t::time_point g_sum_at {};
    uint32_t g_sum_min_us = 0;
    uint32_t g_sum_max_us = 0;
    uint32_t g_sum_updates = 0;
    uint32_t g_sum_windows = 0;

    uint32_t clamp_us(int64_t us) {
      return static_cast<uint32_t>(std::clamp<int64_t>(us, k_min_us, k_max_us));
    }

    /// session 內的值域：全域上下限 ∩ 起始值 ±k_session_drift_us
    uint32_t clamp_session_us(int64_t us) {
      const int64_t lo = std::max<int64_t>(k_min_us, static_cast<int64_t>(g_start_us) - k_session_drift_us);
      const int64_t hi = std::min<int64_t>(k_max_us, static_cast<int64_t>(g_start_us) + k_session_drift_us);
      return static_cast<uint32_t>(std::clamp<int64_t>(us, lo, hi));
    }

    bool same_period(uint32_t a, uint32_t b) {
      return a != 0 && b != 0 && std::abs(static_cast<double>(a) - static_cast<double>(b)) <= k_same_period_tol * static_cast<double>(std::max(a, b));
    }

    learned_t *find_learned(uint32_t period_us) {
      for (auto &l : g_learned) {
        if (same_period(l.period_us, period_us)) {
          return &l;
        }
      }
      return nullptr;
    }

    /// 記下目前更新率學到的值
    void set_learned(uint32_t v2p_us) {
      if (g_period_us == 0) {
        return;
      }
      if (auto *l = find_learned(g_period_us)) {
        l->v2p_us = v2p_us;
      } else {
        g_learned.push_back({g_period_us, v2p_us});
      }
    }

    void reset_summary() {
      g_sum_min_us = g_sum_max_us = g_cur_us;
      g_sum_updates = 0;
      g_sum_windows = 0;
    }

    void enter_measure() {
      g_phase = phase_e::measure;
      g_sum_us = 0;
      g_n = 0;
    }

    void enter_track() {
      g_phase = phase_e::track;
      g_ema_us = 0;
      reset_summary();
    }

    /// 累積一個視窗；滿 k_probe_windows 個時回傳平均與最大差
    std::optional<window_t> accumulate(int32_t x_us) {
      if (g_n == 0) {
        g_win_min_us = g_win_max_us = x_us;
      } else {
        g_win_min_us = std::min(g_win_min_us, x_us);
        g_win_max_us = std::max(g_win_max_us, x_us);
      }
      g_sum_us += x_us;
      if (++g_n < k_probe_windows) {
        return std::nullopt;
      }
      const window_t w {static_cast<int32_t>(g_sum_us / g_n), g_win_max_us - g_win_min_us};
      g_sum_us = 0;
      g_n = 0;
      return w;
    }
  }  // namespace

  uint32_t convert_for_period(uint32_t v2p_us, uint32_t from_period_us, uint32_t to_period_us) {
    if (from_period_us == 0 || to_period_us == 0) {
      return clamp_us(v2p_us);
    }
    const double f = k_fixed_part_us;
    return clamp_us(std::llround(f + (static_cast<double>(v2p_us) - f) * static_cast<double>(to_period_us) / static_cast<double>(from_period_us)));
  }

  start_t begin_session(uint32_t period_us, uint32_t fixed_us) {
    std::lock_guard lk(g_mtx);
    g_sum_at = {};
    g_period_us = period_us;
    g_warm = 0;
    if (fixed_us > 0) {
      g_phase = phase_e::off;
      g_start_us = g_cur_us = fixed_us;
      return {fixed_us, start_e::fixed};
    }
    start_t s;
    if (const auto *l = find_learned(period_us)) {
      s = {l->v2p_us, start_e::learned};
    } else if (!g_learned.empty() && period_us != 0) {
      // 別的更新率學過：取週期最接近的換算
      const learned_t *best = &g_learned.front();
      for (const auto &l2 : g_learned) {
        if (std::llabs(static_cast<int64_t>(l2.period_us) - period_us) < std::llabs(static_cast<int64_t>(best->period_us) - period_us)) {
          best = &l2;
        }
      }
      s = {convert_for_period(best->v2p_us, best->period_us, period_us), start_e::converted, best->period_us};
    } else {
      s = {clamp_us(static_cast<int64_t>(period_us) + k_default_extra_us), start_e::initial};
    }
    g_start_us = g_cur_us = s.us;
    if (g_runtime == runtime_e::honored) {
      enter_track();  // 這個行程已確認 SteamVR 採用中途改值：不再探測
    } else if (g_runtime == runtime_e::ignored) {
      g_phase = phase_e::learn;
      g_sum_us = 0;
      g_n = 0;
    } else {
      enter_measure();
    }
    return s;
  }

  std::optional<result_t> on_pose_lag(int16_t lag_100us, uint16_t count, steady_t::time_point now) {
    if (count < k_reliable_count || lag_100us == k_lag_none) {
      return std::nullopt;
    }
    std::lock_guard lk(g_mtx);
    if (g_phase == phase_e::off) {
      return std::nullopt;
    }
    if (g_warm < k_warmup_windows) {
      ++g_warm;  // 暖機：遊戲載入、host 掉拍時的落後不可信
      return std::nullopt;
    }
    const int32_t x = std::clamp(static_cast<int32_t>(lag_100us) * 100, -k_input_clamp_us, k_input_clamp_us);

    switch (g_phase) {
      case phase_e::off:
        return std::nullopt;

      case phase_e::measure:
        {
          const auto w = accumulate(x);
          if (!w || w->spread_us > k_unstable_spread_us) {
            return std::nullopt;  // 還沒滿 3 個，或不穩：不探測、重量
          }
          const int32_t l0 = w->mean_us;
          if (std::abs(l0) < k_probe_need_us) {
            set_learned(g_cur_us);  // 已經夠準：記下來，繼續量
            return std::nullopt;
          }
          const int32_t mag = std::clamp(static_cast<int32_t>(std::lround(std::abs(l0) * 0.6)), k_probe_min_us, k_probe_max_us);
          const uint32_t next = clamp_session_us(static_cast<int64_t>(g_cur_us) + (l0 > 0 ? mag : -mag));
          if (next == g_cur_us) {
            return std::nullopt;
          }
          result_t r {action_e::update, g_cur_us, next, 0, l0};
          g_l0_us = l0;
          g_probe_us = static_cast<int32_t>(next) - static_cast<int32_t>(g_cur_us);
          g_probe_at = now;
          g_cur_us = next;
          g_phase = phase_e::settle;
          return r;
        }

      case phase_e::settle:
        if (now - g_probe_at < k_settle) {
          return std::nullopt;
        }
        g_phase = phase_e::verify;
        g_sum_us = 0;
        g_n = 0;
        [[fallthrough]];

      case phase_e::verify:
        {
          const auto w = accumulate(x);
          if (!w) {
            return std::nullopt;
          }
          const int32_t l1 = w->mean_us;
          if (w->spread_us > k_unstable_spread_us) {
            // 判不出來：退回起始值、重新量（不算一次「不採用」）
            const uint32_t probed = g_cur_us;
            g_cur_us = g_start_us;
            enter_measure();
            result_t r {action_e::inconclusive, probed, g_start_us, 0, g_l0_us, l1};
            r.spread_us = w->spread_us;
            return r;
          }
          const int64_t moved = static_cast<int64_t>(g_l0_us) - l1;
          if (moved * g_probe_us > 0 && std::llabs(moved) * 2 >= std::llabs(g_probe_us)) {
            g_runtime = runtime_e::honored;
            g_not_honored_streak = 0;
            set_learned(g_cur_us);
            enter_track();
            g_ema_us = l1;  // 從探測後的平均開始追
            return result_t {action_e::honored, static_cast<uint32_t>(static_cast<int64_t>(g_cur_us) - g_probe_us), g_cur_us, g_cur_us, g_l0_us, l1};
          }
          // 不採用：退回起始值，本 session 停調；探測前後都在量「起始值下」的落後，取平均推算下一個 session。
          // 連續第二次才把整個行程當成不採用（之後只量不探測）
          const bool ignored_now = ++g_not_honored_streak >= k_ignored_after;
          if (ignored_now) {
            g_runtime = runtime_e::ignored;
          }
          g_phase = phase_e::off;
          const uint32_t probed = g_cur_us;
          g_cur_us = g_start_us;
          const uint32_t next = clamp_us(static_cast<int64_t>(g_start_us) + (static_cast<int64_t>(g_l0_us) + l1) / 2);
          set_learned(next);
          result_t r {action_e::not_honored, probed, g_start_us, next, g_l0_us, l1};
          r.runtime_ignored = ignored_now;
          return r;
        }

      case phase_e::learn:
        {
          const auto w = accumulate(x);
          if (!w || w->spread_us > k_unstable_spread_us) {
            return std::nullopt;
          }
          g_phase = phase_e::off;
          const uint32_t next = clamp_us(static_cast<int64_t>(g_start_us) + w->mean_us);
          set_learned(next);
          return result_t {action_e::learned, g_cur_us, g_cur_us, next, w->mean_us, w->mean_us};
        }

      case phase_e::track:
        {
          ++g_sum_windows;
          g_ema_us += k_ema_alpha * (x - g_ema_us);
          if (std::abs(g_ema_us) < k_track_deadband_us) {
            return std::nullopt;
          }
          const int32_t step = std::clamp(static_cast<int32_t>(std::lround(g_ema_us * k_track_gain)), -k_track_max_step_us, k_track_max_step_us);
          const uint32_t next = clamp_session_us(static_cast<int64_t>(g_cur_us) + step);
          if (next == g_cur_us) {
            return std::nullopt;
          }
          const int32_t applied = static_cast<int32_t>(next) - static_cast<int32_t>(g_cur_us);
          result_t r {action_e::update, g_cur_us, next, 0, static_cast<int32_t>(std::lround(g_ema_us)), 0, true};
          g_ema_us -= applied;  // v2p 加 1 ms → 落後少 1 ms（已確認採用）
          g_cur_us = next;
          set_learned(next);
          g_sum_min_us = std::min(g_sum_min_us, next);
          g_sum_max_us = std::max(g_sum_max_us, next);
          ++g_sum_updates;
          return r;
        }
    }
    return std::nullopt;
  }

  void revert(const result_t &r) {
    if (r.action != action_e::update) {
      return;
    }
    std::lock_guard lk(g_mtx);
    if (g_cur_us != r.to_us) {
      return;
    }
    const int32_t applied = static_cast<int32_t>(r.to_us) - static_cast<int32_t>(r.from_us);
    g_cur_us = r.from_us;
    if (g_phase == phase_e::settle || g_phase == phase_e::verify) {
      enter_measure();  // 探測沒送出去：重新量
    } else if (g_phase == phase_e::track) {
      g_ema_us += applied;
      if (auto *l = find_learned(g_period_us); l && l->v2p_us == r.to_us) {
        l->v2p_us = r.from_us;
      }
      if (g_sum_updates > 0) {
        --g_sum_updates;
      }
    }
  }

  std::optional<summary_t> take_summary(steady_t::time_point now) {
    std::lock_guard lk(g_mtx);
    if (g_phase != phase_e::track) {
      return std::nullopt;
    }
    if (g_sum_at == steady_t::time_point {}) {
      g_sum_at = now;
      reset_summary();
      return std::nullopt;
    }
    if (now - g_sum_at < k_summary_interval || g_sum_windows == 0) {
      return std::nullopt;
    }
    summary_t s {g_cur_us, g_sum_min_us, g_sum_max_us, static_cast<int32_t>(std::lround(g_ema_us)), g_sum_updates, g_sum_windows};
    g_sum_at = now;
    reset_summary();
    return s;
  }

  void reset_for_test() {
    std::lock_guard lk(g_mtx);
    g_phase = phase_e::off;
    g_runtime = runtime_e::unknown;
    g_not_honored_streak = 0;
    g_period_us = 0;
    g_start_us = g_cur_us = 0;
    g_learned.clear();
    g_warm = 0;
    g_sum_us = 0;
    g_n = 0;
    g_win_min_us = g_win_max_us = 0;
    g_l0_us = g_probe_us = 0;
    g_probe_at = {};
    g_ema_us = 0;
    g_sum_at = {};
    g_sum_min_us = g_sum_max_us = 0;
    g_sum_updates = g_sum_windows = 0;
  }

}  // namespace vr::predict
