/**
 * @file src/vr/abr_delay_trend.h
 * @brief VipleStream 2.0 §ABR-ADAPT：排隊延遲趨勢估計器（namespace vr::abr）。
 *
 * 演算法說明：
 * 1. 樣本存 deque（時間, rtt），保留 10 秒。base / jitter / p50 從中排序副本計算。
 * 2. 平滑：smoothed = smoothed + 0.25 * (rtt - smoothed)（首筆直接等於 rtt）。
 * 3. 趨勢：對最近 2 秒之 (t 秒, smoothed) 最小平方法線性迴歸得斜率 m (ms/s)；樣本 < 6 筆時 m = 0。
 *    trend_ms = m * 2.0（2 秒內預期延遲上升毫秒數）。
 * 4. 自適應門檻 γ（參考 WebRTC GCC 機制）：初值 12.5 ms，dt_ms 為距上一筆毫秒數（上限 100 ms）。
 *    若 |trend| - γ <= 15 ms：γ += dt_ms * k * (|trend| - γ)。
 *    |trend| < γ 時 k = k_down (0.00018)，否則 k = k_up (0.0012)。
 *    γ 下限結合量測之環境抖動 jitter * 1.1，夾在 [max(6.0, jitter * 1.1), 600.0] ms。
 * 5. 狀態判定：
 *    - trend > γ 持續 >= 300 ms 且 trend 未衰退（>= 進入時 trend * 0.8）-> overuse。
 *    - 安全網：p50 - base > max(60.0, 4 * jitter) 持續 >= 1 秒 -> overuse。
 *    - trend < -γ -> underuse。
 *    - 其他 -> normal。overuse 成立後需 trend <= 0.5γ 且安全網條件不成立才脫離。
 *
 * 純邏輯、無 I/O、無 log、無全域狀態、無執行緒，僅使用 C++20 標準函式庫。
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <vector>

namespace vr::abr {

  using steady = std::chrono::steady_clock;

  // 延遲趨勢狀態
  enum class trend_state_e {
    normal,   ///< 正常狀態
    overuse,  ///< 過載（排隊延遲持續膨脹）
    underuse  ///< 低載（排隊延遲正在排空）
  };

  inline const char *trend_state_name(trend_state_e s) {
    switch (s) {
      case trend_state_e::normal:   return "normal";
      case trend_state_e::overuse:  return "overuse";
      case trend_state_e::underuse: return "underuse";
    }
    return "unknown";
  }

  // 狀態快照
  struct delay_trend_snapshot_t {
    double base_ms;
    double jitter_ms;
    double p50_ms;
    double trend_ms;
    double gamma_ms;
    trend_state_e state;
  };

  // 常數定義（具名 constexpr 放在檔頭）
  constexpr double k_trend_gamma_initial_ms = 12.5;     ///< γ 自適應門檻初值 12.5 ms
  constexpr double k_trend_gamma_min_ms = 6.0;          ///< γ 絕對下限 6.0 ms
  constexpr double k_trend_gamma_max_ms = 600.0;        ///< γ 絕對上限 600.0 ms
  constexpr double k_trend_gamma_max_diff_ms = 15.0;    ///< |trend| - γ 最大適應差值 15.0 ms
  constexpr double k_trend_k_up = 0.0012;               ///< γ 上升追蹤係數（每毫秒）
  constexpr double k_trend_k_down = 0.00018;            ///< γ 下降回調係數（每毫秒）
  constexpr double k_trend_smooth_factor = 0.25;        ///< EWMA 平滑係數 0.25
  constexpr auto k_trend_history_window = std::chrono::milliseconds(10000);  ///< 樣本歷史保留視窗 10 秒
  constexpr auto k_trend_regression_window = std::chrono::milliseconds(2000);///< 線性迴歸視窗 2 秒
  constexpr auto k_trend_p50_window = std::chrono::milliseconds(1000);       ///< P50 中位數視窗 1 秒
  constexpr size_t k_trend_min_samples_regression = 6;  ///< 線性迴歸最少樣本數 6 筆
  constexpr size_t k_trend_min_samples_base = 5;        ///< 計算第 10 百分位最少樣本數 5 筆
  constexpr auto k_trend_overuse_hold_time = std::chrono::milliseconds(300); ///< overuse 需持續 300 ms
  constexpr double k_trend_overuse_trend_decay_limit = 0.8; ///< overuse 判定期間 trend 下降容忍比率 0.8
  constexpr auto k_trend_safety_net_hold_time = std::chrono::milliseconds(1000); ///< 安全網需持續 1 秒
  constexpr double k_trend_safety_net_base_diff_min = 60.0; ///< 安全網固定差值門檻 60 ms
  constexpr double k_trend_safety_net_jitter_mult = 4.0;    ///< 安全網抖動倍數 4 倍
  constexpr double k_trend_exit_gamma_mult = 0.5;           ///< 離開 overuse 門檻係數 0.5 γ
  constexpr double k_trend_max_dt_ms = 100.0;               ///< dt_ms 上限 100 ms
  constexpr double k_trend_gamma_jitter_factor = 1.1;       ///< γ 結合環境量測抖動之動態底限係數

  class delay_trend_t {
  public:
    delay_trend_t() = default;

    trend_state_e state() const { return state_; }
    double base_ms() const { return base_ms_; }
    double jitter_ms() const { return jitter_ms_; }
    double p50_ms() const { return p50_ms_; }
    double trend_ms() const { return trend_ms_; }
    double gamma_ms() const { return gamma_ms_; }

    delay_trend_snapshot_t snapshot() const {
      return {base_ms_, jitter_ms_, p50_ms_, trend_ms_, gamma_ms_, state_};
    }

    /// 檢查 within 時間內是否曾處於過載狀態
    bool overuse_recently(steady::time_point now, std::chrono::milliseconds within) const {
      if (last_overuse_time_ == steady::time_point{}) {
        return false;
      }
      return (now >= last_overuse_time_) && (now - last_overuse_time_ <= within);
    }

    /// 接收 RTT 樣本（rtt_ms <= 0 忽略）
    void on_rtt(double rtt_ms, steady::time_point now) {
      if (rtt_ms <= 0.0) return;

      // 1. EWMA 平滑（第一筆直接等於 rtt）
      if (samples_.empty()) {
        smoothed_rtt_ = rtt_ms;
      } else {
        smoothed_rtt_ += k_trend_smooth_factor * (rtt_ms - smoothed_rtt_);
      }

      samples_.push_back({now, rtt_ms, smoothed_rtt_});
      while (!samples_.empty() && now - samples_.front().time > k_trend_history_window) {
        samples_.pop_front();
      }

      // 2. 趨勢斜率計算：最近 2 秒之 (t 秒, smoothed) 線性迴歸
      // §REVIEW-1111：三個暫存陣列改成成員、重複使用。這裡每一筆單向延遲都會進來（144 Hz），原本每次配置三塊再放掉，
      // 就在收追蹤樣本的同一條執行緒上。（同一則回報的各筆不能合併成只算一次：無線停頓後一次到齊的那一批
      // 要逐筆判，test OwdWifiStallIsNotCongestion 就是這個情況。）
      auto &reg_samples = scratch_reg_;
      reg_samples.clear();
      for (auto it = samples_.rbegin(); it != samples_.rend(); ++it) {
        if (now - it->time <= k_trend_regression_window) {
          reg_samples.push_back(&*it);
        } else {
          break;
        }
      }

      double m = 0.0;
      if (reg_samples.size() >= k_trend_min_samples_regression) {
        double sum_t = 0.0, sum_y = 0.0;
        const auto ref_time = reg_samples.back()->time;
        for (const auto *s : reg_samples) {
          double t = std::chrono::duration<double>(s->time - ref_time).count();
          sum_t += t;
          sum_y += s->smoothed_rtt_ms;
        }
        double mean_t = sum_t / static_cast<double>(reg_samples.size());
        double mean_y = sum_y / static_cast<double>(reg_samples.size());

        double var_t = 0.0, cov_ty = 0.0;
        for (const auto *s : reg_samples) {
          double t = std::chrono::duration<double>(s->time - ref_time).count();
          double dt = t - mean_t;
          double dy = s->smoothed_rtt_ms - mean_y;
          var_t += dt * dt;
          cov_ty += dt * dy;
        }
        if (var_t > 1e-9) {
          m = cov_ty / var_t;
        }
      }
      trend_ms_ = m * 2.0;

      // 3. 計算 base、jitter、p50
      auto &sorted_raw = scratch_raw_;
      auto &sorted_1s = scratch_1s_;
      sorted_raw.clear();
      sorted_1s.clear();
      sorted_raw.reserve(samples_.size());
      for (const auto &s : samples_) {
        sorted_raw.push_back(s.raw_rtt_ms);
        if (now - s.time <= k_trend_p50_window) {
          sorted_1s.push_back(s.raw_rtt_ms);
        }
      }
      std::sort(sorted_raw.begin(), sorted_raw.end());
      std::sort(sorted_1s.begin(), sorted_1s.end());

      auto calc_percentile = [](const std::vector<double> &v, double p) -> double {
        if (v.empty()) return 0.0;
        if (v.size() == 1) return v[0];
        double rank = p * (v.size() - 1);
        size_t lo = static_cast<size_t>(std::floor(rank));
        size_t hi = static_cast<size_t>(std::ceil(rank));
        double f = rank - lo;
        if (lo >= v.size() - 1) return v.back();
        return v[lo] + f * (v[hi] - v[lo]);
      };

      if (sorted_raw.empty()) {
        base_ms_ = 0.0;
        jitter_ms_ = 0.0;
      } else if (sorted_raw.size() < k_trend_min_samples_base) {
        base_ms_ = sorted_raw.front();
        jitter_ms_ = sorted_raw.back() - sorted_raw.front();
      } else {
        double p10 = calc_percentile(sorted_raw, 0.10);
        double p90 = calc_percentile(sorted_raw, 0.90);
        base_ms_ = p10;
        jitter_ms_ = p90 - p10;
      }

      if (sorted_1s.empty()) {
        p50_ms_ = base_ms_;
      } else {
        p50_ms_ = calc_percentile(sorted_1s, 0.50);
      }

      // 4. 自適應門檻 γ 更新
      if (has_prev_sample_) {
        double dt_ms = std::chrono::duration<double, std::milli>(now - prev_sample_time_).count();
        if (dt_ms < 0.0) dt_ms = 0.0;
        dt_ms = std::min(dt_ms, k_trend_max_dt_ms);

        double abs_trend = std::abs(trend_ms_);
        double diff = abs_trend - gamma_ms_;
        if (diff <= k_trend_gamma_max_diff_ms) {
          double k = (abs_trend < gamma_ms_) ? k_trend_k_down : k_trend_k_up;
          gamma_ms_ += dt_ms * k * diff;
        }
        // 動態下限結合網路環境實測抖動，避免高抖動網路 γ 衰退過度產生誤判
        double min_gamma = std::max(k_trend_gamma_min_ms, jitter_ms_ * k_trend_gamma_jitter_factor);
        gamma_ms_ = std::clamp(gamma_ms_, min_gamma, k_trend_gamma_max_ms);
      }
      has_prev_sample_ = true;
      prev_sample_time_ = now;

      // 5. 安全網條件檢查：p50 - base > max(60, 4 * jitter) 持續 >= 1 秒
      bool safety_net_cond = (p50_ms_ - base_ms_) > std::max(k_trend_safety_net_base_diff_min,
                                                              k_trend_safety_net_jitter_mult * jitter_ms_);
      if (safety_net_cond) {
        if (safety_net_start_ == steady::time_point{}) {
          safety_net_start_ = now;
        }
      } else {
        safety_net_start_ = {};
      }

      // 6. trend > γ 持續 >= 300 ms 判定候選
      if (trend_ms_ > gamma_ms_) {
        if (overuse_candidate_start_ == steady::time_point{}) {
          overuse_candidate_start_ = now;
          overuse_candidate_initial_trend_ = trend_ms_;
        } else if (trend_ms_ < overuse_candidate_initial_trend_ * k_trend_overuse_trend_decay_limit) {
          overuse_candidate_start_ = now;
          overuse_candidate_initial_trend_ = trend_ms_;
        }
      } else {
        overuse_candidate_start_ = {};
      }

      // 7. 狀態機轉移
      if (state_ == trend_state_e::overuse) {
        // overuse 脫離條件：trend 回到 <= 0.5γ 且安全網條件不成立
        if (trend_ms_ <= k_trend_exit_gamma_mult * gamma_ms_ && !safety_net_cond) {
          if (trend_ms_ < -gamma_ms_) {
            state_ = trend_state_e::underuse;
          } else {
            state_ = trend_state_e::normal;
          }
          overuse_candidate_start_ = {};
          safety_net_start_ = {};
        }
      } else {
        bool cand_over = (overuse_candidate_start_ != steady::time_point{} &&
                          now - overuse_candidate_start_ >= k_trend_overuse_hold_time);
        bool safe_over = (safety_net_start_ != steady::time_point{} &&
                          now - safety_net_start_ >= k_trend_safety_net_hold_time);
        if (cand_over || safe_over) {
          state_ = trend_state_e::overuse;
        } else if (trend_ms_ < -gamma_ms_) {
          state_ = trend_state_e::underuse;
        } else {
          state_ = trend_state_e::normal;
        }
      }

      if (state_ == trend_state_e::overuse) {
        last_overuse_time_ = now;
      }
    }

  private:
    struct sample_t {
      steady::time_point time;
      double raw_rtt_ms;
      double smoothed_rtt_ms;
    };

    trend_state_e state_ = trend_state_e::normal;
    double base_ms_ = 0.0;
    double jitter_ms_ = 0.0;
    double p50_ms_ = 0.0;
    double trend_ms_ = 0.0;
    double gamma_ms_ = k_trend_gamma_initial_ms;
    double smoothed_rtt_ = 0.0;

    bool has_prev_sample_ = false;
    std::vector<const sample_t *> scratch_reg_;
    std::vector<double> scratch_raw_;
    std::vector<double> scratch_1s_;
    steady::time_point prev_sample_time_{};
    steady::time_point last_overuse_time_{};

    steady::time_point overuse_candidate_start_{};
    double overuse_candidate_initial_trend_ = 0.0;

    steady::time_point safety_net_start_{};

    std::deque<sample_t> samples_;
  };

}  // namespace vr::abr
