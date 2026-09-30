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

  void controller_t::reset() {
    *this = controller_t {};
  }

  result_t controller_t::decide(int64_t now_ns, int32_t ppm) {
    result_t r;
    r.ppm = ppm;
    r.slack_ema_us = ema_;
    r.target_us = target_for_period(period_ns_);
    const bool changed = std::abs(ppm - sent_ppm_) >= kMinDelta;
    const bool due = !sent_once_ || now_ns - last_sent_ns_ >= kRefreshNs;
    if (changed || due) {
      r.update = true;
      sent_ppm_ = ppm;
      sent_once_ = true;
      last_sent_ns_ = now_ns;
    }
    return r;
  }

  result_t controller_t::on_latch(int32_t slack_us, uint32_t display_period_ns, int64_t now_ns) {
    // 顯示週期只接受 5 ms～50 ms（200～20 Hz）；其他值沿用上次
    if (display_period_ns >= 5'000'000 && display_period_ns <= 50'000'000) {
      period_ns_ = display_period_ns;
    }
    const double period_us = (double) period_ns_ / 1000.0;
    // 單筆離群值夾在 ±1 個週期內（晚到超過一個週期時，幀本來就會被下一個 latch 接走）
    const double s = std::clamp((double) slack_us, -period_us, period_us);

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

  result_t controller_t::on_idle(int64_t now_ns) {
    if (!sent_once_ || last_latch_ns_ == 0 || now_ns - last_latch_ns_ < kStaleNs) {
      result_t r;
      r.ppm = sent_ppm_;
      r.slack_ema_us = ema_;
      r.target_us = target_for_period(period_ns_);
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
    return decide(now_ns, ppm);
  }

}  // namespace vr::latch
