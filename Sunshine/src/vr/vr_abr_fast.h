/**
 * @file src/vr/vr_abr_fast.h
 * @brief VipleStream 2.0 §VR-ABR-FAST：VR 反應式快速降碼率（學 Steam 策略，設計稿第 2 節＋第 5 節階段 1）。
 *
 * 專為 VR 高速動態環境設計（流暢度第一、寧可糊不要卡）。高靈敏度捕捉：
 *   - 嚴重（FAST）：1 秒內 FEC 救不回 ≥ 2 張；一次 LOSS lostFrames ≥ 2；
 *     送影像連線 dropBlock；或 RTT 比基準高 ≥ 15 ms 持續 ≥ 500 ms。
 *     降碼幅度：target = applied × 0.75；同秒內第二次嚴重降碼：× 0.60。
 *   - 中度（MODERATE）：1 秒內只有 1 張救不回；LATCH 連續 2 筆 slack < −1 ms；
 *     或主連線送達率 < 90%。
 *     降碼幅度：target = applied × 0.90。
 *   - 下限：max(10 Mbps, 設定碼率 × 30%)；上限：設定碼率（支援 set_cap_kbps）。
 *   - 防振盪：RAMP 中 5 秒內又降碼 → 上限暫時壓到 applied × 0.85 維持 30 秒。
 *   - 狀態機：HEALTHY → CUT → HOLD（2.0 秒冷卻禁止回升）→ RAMP（每 1.5 秒 +4%×上限）→ HEALTHY。
 *
 * 純邏輯、無 I/O、無 log、無全域狀態，可單元測試。呼叫端（stream.cpp）持 abrMutex 加鎖呼叫並負責記錄 log。
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace vr::abr {

  using steady = std::chrono::steady_clock;

  // 狀態定義
  enum class fast_state_e {
    healthy,  ///< 健康狀態
    cut,      ///< 降碼狀態（當輪有降碼事件生效）
    hold,     ///< 冷卻冷凍期（2.0 秒，禁止回升）
    ramp,     ///< 平滑回升期（每 1.5 秒乾淨就 +4%×上限）
  };

  inline const char *fast_state_name(fast_state_e s) {
    switch (s) {
      case fast_state_e::healthy: return "healthy";
      case fast_state_e::cut:     return "cut";
      case fast_state_e::hold:    return "hold";
      case fast_state_e::ramp:    return "ramp";
    }
    return "unknown";
  }

  // 常數定義
  constexpr auto k_fast_hold_duration = std::chrono::milliseconds(2000);     ///< HOLD 冷卻時間 2.0 秒
  constexpr auto k_fast_ramp_interval = std::chrono::milliseconds(1500);     ///< RAMP 爬升間隔 1.5 秒
  constexpr float k_fast_ramp_step_pct = 0.04f;                              ///< RAMP 每步回升幅度（4% × 上限）
  constexpr auto k_fast_cut_window = std::chrono::milliseconds(1000);        ///< 嚴重降碼同秒判定視窗 1 秒
  constexpr auto k_fast_anti_osc_window = std::chrono::milliseconds(5000);   ///< 防振盪 RAMP 觸發視窗 5 秒
  constexpr auto k_fast_anti_osc_duration = std::chrono::seconds(30);        ///< 防振盪懲罰維持 30 秒
  constexpr float k_fast_anti_osc_cap_factor = 0.85f;                        ///< 防振盪懲罰上限係數 applied × 0.85
  constexpr int k_fast_floor_abs_min_kbps = 10000;                           ///< 絕對下限 10 Mbps (10,000 Kbps)
  constexpr float k_fast_floor_pct = 0.30f;                                  ///< 相對下限 30% × 設定碼率

  class fast_controller_t {
  public:
    enum class severity_e {
      moderate,
      fast,
    };

    explicit fast_controller_t(int configured_kbps)
      : configured_kbps_(std::max(1, configured_kbps)),
        cap_kbps_(std::max(1, configured_kbps)),
        applied_kbps_(std::max(1, configured_kbps)),
        current_target_kbps_(std::max(1, configured_kbps)) {}

    void set_configured_kbps(int configured_kbps) {
      if (configured_kbps > 0) {
        configured_kbps_ = configured_kbps;
        if (cap_kbps_ > configured_kbps_) {
          cap_kbps_ = configured_kbps_;
        }
      }
    }

    void set_cap_kbps(int cap_kbps) {
      if (cap_kbps > 0) {
        cap_kbps_ = std::min(configured_kbps_, cap_kbps);
      }
    }

    void set_applied_kbps(int applied_kbps) {
      if (applied_kbps > 0) {
        applied_kbps_ = applied_kbps;
      }
    }

    int configured_kbps() const { return configured_kbps_; }
    int cap_kbps() const { return cap_kbps_; }
    int applied_kbps() const { return applied_kbps_; }
    int current_target_kbps() const { return current_target_kbps_; }
    fast_state_e state() const { return state_; }
    const char *state_str() const { return fast_state_name(state_); }
    const char *last_reason() const { return last_reason_.c_str(); }
    int cuts() const { return cuts_count_; }
    int ramps() const { return ramps_count_; }

    int floor_kbps() const {
      return std::max(k_fast_floor_abs_min_kbps, static_cast<int>(configured_kbps_ * k_fast_floor_pct));
    }

    int effective_cap_kbps(steady::time_point now) const {
      int cap = cap_kbps_;
      if (penalty_until_ != steady::time_point {} && now < penalty_until_) {
        cap = std::min(cap, penalty_cap_);
      }
      return std::max(floor_kbps(), cap);
    }

    /// 1 秒內 FEC 救不回 ≥ 2 張 → 嚴重 (FAST)；1 秒內只有 1 張救不回 → 中度 (MODERATE)
    void on_fec_failed(int n, steady::time_point now) {
      if (n <= 0) return;
      clean_old_fec_fails(now);
      fec_fails_.push_back({n, now});
      int total = 0;
      for (const auto &item : fec_fails_) {
        total += item.count;
      }
      if (total >= 2) {
        trigger_cut(severity_e::fast, "fec-failed-ge-2", now);
      } else if (total == 1) {
        trigger_cut(severity_e::moderate, "fec-failed-1", now);
      }
    }

    /// 一次 LOSS 回報 lostFrames ≥ 2 → 嚴重 (FAST)
    void on_loss(uint32_t lost_frames, steady::time_point now) {
      if (lost_frames >= 2) {
        trigger_cut(severity_e::fast, "loss-ge-2", now);
      }
    }

    /// 送影像的連線出現 dropBlock → 嚴重 (FAST)
    void on_drop_block(steady::time_point now) {
      trigger_cut(severity_e::fast, "drop-block", now);
    }

    /// RTT 比基準高 ≥ 15 ms 持續 ≥ 500 ms → 嚴重 (FAST)
    void on_rtt(int rtt_ms, steady::time_point now) {
      if (rtt_ms <= 0) return;
      if (rtt_baseline_ <= 0) {
        rtt_baseline_ = rtt_ms;
        rtt_cur_min_ = rtt_ms;
        rtt_window_start_ = now;
      } else {
        if (now - rtt_window_start_ >= std::chrono::seconds(30)) {
          rtt_prev_min_ = rtt_cur_min_;
          rtt_cur_min_ = rtt_ms;
          rtt_window_start_ = now;
          rtt_baseline_ = rtt_prev_min_ > 0 ? std::min(rtt_cur_min_, rtt_prev_min_) : rtt_cur_min_;
        } else if (rtt_ms < rtt_cur_min_) {
          rtt_cur_min_ = rtt_ms;
          rtt_baseline_ = rtt_prev_min_ > 0 ? std::min(rtt_cur_min_, rtt_prev_min_) : rtt_cur_min_;
        }
      }

      if (rtt_ms >= rtt_baseline_ + 15) {
        if (rtt_excess_start_ == steady::time_point {}) {
          rtt_excess_start_ = now;
        } else if (now - rtt_excess_start_ >= std::chrono::milliseconds(500)) {
          trigger_cut(severity_e::fast, "rtt-inflation", now);
          rtt_excess_start_ = now;
        }
      } else {
        rtt_excess_start_ = {};
      }
    }

    /// LATCH 連續 2 筆 slack < −1 ms → 中度 (MODERATE)
    void on_latch_slack(int64_t slack_us, steady::time_point now) {
      if (slack_us < -1000) {
        ++consecutive_late_slack_;
        if (consecutive_late_slack_ >= 2) {
          trigger_cut(severity_e::moderate, "latch-slack-late", now);
          consecutive_late_slack_ = 0;
        }
      } else {
        consecutive_late_slack_ = 0;
      }
    }

    /// 主連線送達率 < 90% → 中度 (MODERATE)
    void on_delivery(double pct, steady::time_point now) {
      if (pct >= 0.0 && pct < 90.0) {
        trigger_cut(severity_e::moderate, "delivery-low", now);
      }
    }

    /// 推進狀態機，返回新目標碼率（若無調整則為 std::nullopt）
    std::optional<int> tick(steady::time_point now) {
      // 1. 若有 pending 的降碼要求，立刻回傳並轉入 HOLD 冷卻
      if (pending_target_.has_value()) {
        const int target = *pending_target_;
        pending_target_.reset();
        applied_kbps_ = target;
        state_ = fast_state_e::hold;
        return target;
      }

      // 1b. 上限往下調（例如直連斷了、只剩家用 Wi-Fi 一條連線）時立刻壓到上限，不等丟包
      if (const int eff_cap = effective_cap_kbps(now); applied_kbps_ > eff_cap) {
        applied_kbps_ = eff_cap;
        current_target_kbps_ = eff_cap;
        last_reason_ = "cap";
        return eff_cap;
      }
      // 1c. 上限放寬（直連接上）而目前在 HEALTHY、低於上限 → 進 RAMP 慢慢爬回去
      if (state_ == fast_state_e::healthy && applied_kbps_ < effective_cap_kbps(now)) {
        state_ = fast_state_e::ramp;
        ramp_start_ = {};  // 不是降碼後的回升，不套防振盪
        next_ramp_ = now + k_fast_ramp_interval;
        current_target_kbps_ = applied_kbps_;
      }

      // 2. 若處於 CUT（但 pending 已取），進入 HOLD
      if (state_ == fast_state_e::cut) {
        state_ = fast_state_e::hold;
      }

      // 3. 冷卻觀測期（HOLD）：未滿 2.0 秒嚴禁回升
      if (state_ == fast_state_e::hold) {
        if (now >= hold_until_) {
          state_ = fast_state_e::ramp;
          ramp_start_ = now;
          next_ramp_ = now + k_fast_ramp_interval;
        } else {
          return std::nullopt;
        }
      }

      // 4. 平滑回升期（RAMP）：每 1.5 秒乾淨就 +4%×上限
      if (state_ == fast_state_e::ramp) {
        if (now >= next_ramp_) {
          const int eff_cap = effective_cap_kbps(now);
          // 以實際套用中的碼率為基準（10-30 實測：current_target 停在第一次降碼的值，第一步從 50.6 跳到 73 Mbps）
          if (applied_kbps_ >= eff_cap) {
            current_target_kbps_ = eff_cap;
            applied_kbps_ = eff_cap;
            state_ = fast_state_e::healthy;
            return std::nullopt;
          }
          const int step = std::max(1, static_cast<int>(eff_cap * k_fast_ramp_step_pct));
          int new_target = applied_kbps_ + step;
          if (new_target >= eff_cap) {
            new_target = eff_cap;
            state_ = fast_state_e::healthy;
          }
          current_target_kbps_ = new_target;
          applied_kbps_ = new_target;
          last_reason_ = "ramp-clean";
          next_ramp_ = now + k_fast_ramp_interval;
          ++ramps_count_;
          return new_target;
        }
      }

      return std::nullopt;
    }

  private:
    struct fec_record_t {
      int count;
      steady::time_point at;
    };

    void clean_old_fec_fails(steady::time_point now) {
      while (!fec_fails_.empty() && now - fec_fails_.front().at > k_fast_cut_window) {
        fec_fails_.pop_front();
      }
    }

    void trigger_cut(severity_e sev, const char *reason, steady::time_point now) {
      // 降碼後的冷卻期（CUT／HOLD，hold_until_ 之前）只接受嚴重訊號：送達率每 250 ms、LATCH 每 100 ms 就回報一次，
      // 中度訊號不擋的話一兩秒內會連降好幾次直接壓到下限（設計稿 2.4：HOLD 期間只有嚴重訊號能二度打折）
      if (sev == severity_e::moderate && now < hold_until_) {
        return;
      }
      // 防振盪：RAMP 中 5 秒內又降碼 → 上限暫時壓到 applied × 0.85 維持 30 秒
      if (state_ == fast_state_e::ramp && ramp_start_ != steady::time_point {} &&
          now - ramp_start_ <= k_fast_anti_osc_window) {
        penalty_cap_ = std::max(floor_kbps(), static_cast<int>(applied_kbps_ * k_fast_anti_osc_cap_factor));
        penalty_until_ = now + k_fast_anti_osc_duration;
      }

      int new_target = applied_kbps_;
      if (sev == severity_e::fast) {
        if (last_fast_cut_time_ != steady::time_point {} && now - last_fast_cut_time_ <= k_fast_cut_window) {
          // 同一秒內第二次嚴重 → × 0.60
          new_target = static_cast<int>(fast_cut_base_applied_ * 0.60f);
        } else {
          // 第一次嚴重 → × 0.75
          fast_cut_base_applied_ = applied_kbps_;
          new_target = static_cast<int>(applied_kbps_ * 0.75f);
        }
        last_fast_cut_time_ = now;
      } else {
        // 中度 → × 0.90
        new_target = static_cast<int>(applied_kbps_ * 0.90f);
      }

      const int eff_cap = effective_cap_kbps(now);
      const int eff_floor = floor_kbps();
      new_target = std::clamp(new_target, eff_floor, eff_cap);

      state_ = fast_state_e::cut;
      last_reason_ = reason;
      pending_target_ = new_target;
      current_target_kbps_ = new_target;
      hold_until_ = now + k_fast_hold_duration;
      ++cuts_count_;
    }

    int configured_kbps_ = 0;
    int cap_kbps_ = 0;
    int applied_kbps_ = 0;
    int current_target_kbps_ = 0;
    fast_state_e state_ = fast_state_e::healthy;
    std::string last_reason_ = "none";
    std::optional<int> pending_target_;

    // 嚴重降碼同秒判定
    steady::time_point last_fast_cut_time_ {};
    int fast_cut_base_applied_ = 0;

    // 冷卻與回升定時
    steady::time_point hold_until_ {};
    steady::time_point ramp_start_ {};
    steady::time_point next_ramp_ {};

    // 防振盪懲罰
    int penalty_cap_ = 0;
    steady::time_point penalty_until_ {};

    // 統計計數
    int cuts_count_ = 0;
    int ramps_count_ = 0;

    // FEC 救不回記錄
    std::deque<fec_record_t> fec_fails_;

    // RTT 膨脹偵測
    int rtt_baseline_ = 0;
    int rtt_cur_min_ = 0;
    int rtt_prev_min_ = 0;
    steady::time_point rtt_window_start_ {};
    steady::time_point rtt_excess_start_ {};

    // LATCH 延遲計數
    int consecutive_late_slack_ = 0;
  };

}  // namespace vr::abr
