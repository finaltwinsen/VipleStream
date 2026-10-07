/**
 * @file src/vr/vr_latch.cpp
 * @brief VipleStream 2.0 §VR（M4a R3）：LATCH 相位回授的頻率鎖控制器，見 vr_latch.h。
 */
#include "vr_latch.h"

#include <algorithm>
#include <cmath>

namespace vr::latch {

  double controller_t::target_for_period(uint32_t display_period_ns) {
    const double quarter_us = (double) display_period_ns / 4000.0;
    return std::clamp(quarter_us, 1000.0, 4000.0);
  }

  void controller_t::configure(mode_e mode, int target_pct, uint32_t host_period_ns) {
    mode_ = mode;
    target_pct_ = std::clamp(target_pct, kTargetPctMin, kTargetPctMax);
    host_period_ns_ = host_period_ns >= 5'000'000 && host_period_ns <= 50'000'000 ? host_period_ns : 0;
  }

  void controller_t::reset() {
    const auto mode = mode_;
    const auto pct = target_pct_;
    const auto host = host_period_ns_;
    *this = controller_t {};
    mode_ = mode;
    target_pct_ = pct;
    host_period_ns_ = host;
  }

  double controller_t::modulus_us() const {
    uint32_t p = period_ns_;
    if (host_period_ns_ != 0 && host_period_ns_ < p) {
      p = host_period_ns_;
    }
    return (double) p / 1000.0;
  }

  double controller_t::target_v2_us() const {
    return modulus_us() * (double) target_pct_ / 100.0;
  }

  void controller_t::fill_observability(result_t &r) const {
    r.integral_ppm = integral_;
    r.slips = slips_;
    r.dropped = dropped_;
    r.mode = mode_;
    if (mode_ == mode_e::v2) {
      r.target_us = target_v2_us();
      r.err_us = have_ema_ ? ema_ : 0.0;
      r.slack_ema_us = r.target_us + r.err_us;
    } else {
      r.target_us = target_for_period(period_ns_);
      r.slack_ema_us = ema_;
      r.err_us = have_ema_ ? ema_ - r.target_us : 0.0;
    }
  }

  void controller_t::count_slip(double slack_us, double period_us) {
    if (have_prev_slack_ && std::abs(slack_us - prev_slack_us_) > kSlipPeriods * period_us) {
      ++slips_;
    }
    prev_slack_us_ = slack_us;
    have_prev_slack_ = true;
  }

  result_t controller_t::decide(int64_t now_ns, int32_t ppm) {
    result_t r;
    r.ppm = ppm;
    const bool changed = std::abs(ppm - sent_ppm_) >= kMinDelta;
    const bool due = !sent_once_ || now_ns - last_sent_ns_ >= kRefreshNs;
    if (changed || due) {
      r.update = true;
      sent_ppm_ = ppm;
      sent_once_ = true;
      last_sent_ns_ = now_ns;
    }
    fill_observability(r);
    return r;
  }

  result_t controller_t::on_latch(int32_t slack_us, uint32_t display_period_ns, int64_t now_ns, uint32_t frame_id) {
    // 顯示週期只接受 5 ms～50 ms（200～20 Hz）；其他值沿用上次
    if (display_period_ns >= 5'000'000 && display_period_ns <= 50'000'000) {
      // §VR-LATCH-V2：換更新率（例如 120→90 Hz）時舊的誤差與積分都不再適用
      if (mode_ == mode_e::v2 && have_ema_ &&
          std::abs((double) display_period_ns - (double) period_ns_) > kPeriodChange * (double) period_ns_) {
        have_ema_ = false;
        integral_ = 0;
        have_prev_slack_ = false;
      }
      period_ns_ = display_period_ns;
    }
    return mode_ == mode_e::v2 ? on_latch_v2(slack_us, now_ns, frame_id) : on_latch_legacy(slack_us, now_ns);
  }

  result_t controller_t::on_latch_legacy(int32_t slack_us, int64_t now_ns) {
    const double period_us = (double) period_ns_ / 1000.0;
    // 單筆離群值夾在 ±1 個週期內（晚到超過一個週期時，幀本來就會被下一個 latch 接走）
    const double s = std::clamp((double) slack_us, -period_us, period_us);
    count_slip((double) slack_us, period_us);  // 只做觀測，不影響控制

    const double dt_s = last_latch_ns_ > 0 ? std::clamp((double) (now_ns - last_latch_ns_) / 1e9, 0.0, 1.0) : 0.1;
    last_latch_ns_ = now_ns;
    ++count_;

    if (!have_ema_) {
      ema_ = s;
      have_ema_ = true;
    } else {
      ema_ += kEmaAlpha * (s - ema_);
    }

    const double err = ema_ - target_for_period(period_ns_);  // 正值：幀太早到 → 放慢（ppm > 0）
    integral_ = std::clamp(integral_ + kKi * err * dt_s, -kIntegralMax, kIntegralMax);
    const double raw = kKp * err + integral_;
    const int32_t ppm = (int32_t) std::lround(std::clamp(raw, (double) -VR_LATCH_PPM_MAX, (double) VR_LATCH_PPM_MAX));
    return decide(now_ns, ppm);
  }

  result_t controller_t::on_latch_v2(int32_t slack_us, int64_t now_ns, uint32_t frame_id) {
    // client 沒有新幀時每 100 ms 重送上一筆（同一個 frameId）：不是新的相位量測
    if (have_frame_ && frame_id == last_frame_id_) {
      ++dropped_;
      result_t r;
      r.ppm = sent_ppm_;
      fill_observability(r);
      return r;
    }
    have_frame_ = true;
    last_frame_id_ = frame_id;

    const double period_us = modulus_us();
    const double s = (double) slack_us;
    if (std::abs(s) > kOutlierPeriods * period_us) {
      ++dropped_;
      result_t r;
      r.ppm = sent_ppm_;
      fill_observability(r);
      return r;
    }
    count_slip(s, period_us);

    const double dt_s = last_latch_ns_ > 0 ? std::clamp((double) (now_ns - last_latch_ns_) / 1e9, 0.0, 1.0) : 0.1;
    last_latch_ns_ = now_ns;
    ++count_;

    // slack 是取模一個週期的相位：晚到 δ 的幀被量成 T−δ。誤差只在上側折返（→ −δ−target），
    // 下側只夾不折返，協定「負值＝晚到」的語意不變
    double e = s - target_v2_us();
    if (e >= period_us / 2) {
      e -= period_us;
    }
    e = std::max(e, -period_us / 2);

    if (!have_ema_) {
      ema_ = e;
      have_ema_ = true;
    } else {
      ema_ += kEmaAlpha * (e - ema_);
    }
    const double err = ema_;  // 正值：幀太早到 → 放慢（ppm > 0）

    // 條件式 anti-windup：輸出已經飽和、而且誤差同向時不再積分（legacy 的積分在飽和時照樣累加，鋸齒的成因之一）
    const double max = (double) VR_LATCH_PPM_MAX;
    const double raw_pre = kKpV2 * err + integral_;
    const bool saturated_same = (raw_pre >= max && err > 0) || (raw_pre <= -max && err < 0);
    if (!saturated_same) {
      integral_ = std::clamp(integral_ + kKiV2 * err * dt_s, -kIntegralMax, kIntegralMax);
    }
    const double raw = kKpV2 * err + integral_;
    const int32_t ppm = (int32_t) std::lround(std::clamp(raw, -max, max));
    return decide(now_ns, ppm);
  }

  result_t controller_t::on_idle(int64_t now_ns) {
    if (!sent_once_ || last_latch_ns_ == 0 || now_ns - last_latch_ns_ < kStaleNs) {
      result_t r;
      r.ppm = sent_ppm_;
      fill_observability(r);
      return r;
    }
    // 沒有新的相位資訊：每次 idle 把修正減半，最後回到 0（標稱週期）
    integral_ *= 0.5;
    int32_t ppm = sent_ppm_ / 2;
    if (std::abs(ppm) < kMinDelta) {
      ppm = 0;
      integral_ = 0;
    }
    have_ema_ = false;
    have_prev_slack_ = false;
    return decide(now_ns, ppm);
  }

}  // namespace vr::latch
