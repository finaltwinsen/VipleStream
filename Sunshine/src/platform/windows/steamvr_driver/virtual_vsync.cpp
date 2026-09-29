// virtual_vsync.cpp - 見 virtual_vsync.h（設計 §F.2）。
#include "virtual_vsync.h"

#include <cmath>

#include <windows.h>

namespace vrdrv {

  namespace {
    constexpr uint32_t k_default_slew_ppm = 200;
    constexpr int64_t k_spin_us = 300;

    int64_t now_qpc() {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
    }
  }  // namespace

  void virtual_vsync_t::activate(int64_t qpf, uint32_t refresh_mhz, int64_t now) {
    if (qpf <= 0 || refresh_mhz < 1000) {
      return;
    }
    qpf_ = qpf;
    // P_nom = qpf × 2^32 / (Hz)；refresh_mhz = Hz × 1000 → qpf × 1000 × 2^32 / refresh_mhz
    const double p = (double) qpf * 1000.0 / (double) refresh_mhz * 4294967296.0;
    p_nom_q32_ = (uint64_t) p;
    last_period_q32_ = p_nom_q32_;
    next_ticks_ = now + (int64_t) (p_nom_q32_ >> 32);
    next_frac_ = (uint32_t) p_nom_q32_;
    snapped_ = false;
    epoch_valid_ = false;
    have_pacing_ = false;
  }

  void virtual_vsync_t::advance(uint64_t n, uint64_t period_q32) {
    // n × period：呼叫端保證 n ≤ 2 s 的 vsync 數，乘積不會溢位（見 step()）
    const uint64_t add = (uint64_t) next_frac_ + n * period_q32;
    next_ticks_ += (int64_t) (add >> 32);
    next_frac_ = (uint32_t) add;
  }

  virtual_vsync_t::step_result_t virtual_vsync_t::step(const vripc_pacing_t *pacing, bool dev_pacing, int64_t now, uint32_t throttle_frames) {
    step_result_t r;
    if (!active()) {
      r.target_qpc = now;
      r.vsync_qpc = now;
      return r;
    }
    if (pacing) {
      last_pacing_ = *pacing;
      have_pacing_ = true;
    }
    const vripc_pacing_t *p = have_pacing_ ? &last_pacing_ : nullptr;

    uint64_t p_target = p_nom_q32_;
    uint32_t slew_max = k_default_slew_ppm;
    double ppm = 0.0;
    if (p) {
      const uint64_t lo = p_nom_q32_ / 100 * (100 - VRIPC_PACING_PERIOD_TOL_PCT);
      const uint64_t hi = p_nom_q32_ / 100 * (100 + VRIPC_PACING_PERIOD_TOL_PCT);
      const bool mode_ok = p->mode == VRIPC_PM_PRODUCTION || (dev_pacing && p->mode <= VRIPC_PM_E3_BOTH);
      if (p->period_q32 < lo || p->period_q32 > hi || p->slew_ppm_max > VRIPC_PACING_SLEW_PPM_MAX || !mode_ok) {
        r.pacing_rejected = true;
      } else {
        p_target = p->period_q32;
        slew_max = p->slew_ppm_max;
        r.mode = p->mode;
        if (!epoch_valid_ || p->epoch != epoch_seen_) {
          epoch_seen_ = p->epoch;
          epoch_valid_ = true;
          snapped_ = false;
        }
        // anchor 只在 now ± 1 s 內採用（1 小時後的 anchor 之類的異常值直接忽略）
        if ((p->pacing_flags & VRIPC_PF_HAS_ANCHOR) && p->anchor_qpc >= now - qpf_ && p->anchor_qpc <= now + qpf_) {
          const double period_ticks = (double) p_target / 4294967296.0;
          double phi = std::fmod((double) (p->anchor_qpc - next_ticks_), period_ticks);
          if (phi >= period_ticks / 2) {
            phi -= period_ticks;
          } else if (phi < -period_ticks / 2) {
            phi += period_ticks;
          }
          if ((p->pacing_flags & VRIPC_PF_ALLOW_SNAP) && !snapped_) {
            next_ticks_ += (int64_t) phi;
            snapped_ = true;
            r.snapped = true;
            r.snap_ticks = (int64_t) phi;
            phi = 0.0;
          }
          // 1 s 內消掉 10% 相位誤差（§F.2 的 K）
          ppm = 0.1 * phi / (double) qpf_ * 1e6;
          const double lim = (double) slew_max;
          if (ppm > lim) {
            ppm = lim;
          } else if (ppm < -lim) {
            ppm = -lim;
          }
        }
      }
    }
    last_slew_max_ = slew_max;
    const uint64_t p_eff = (uint64_t) ((double) p_target * (1.0 + ppm * 1e-6));
    last_period_q32_ = p_eff;
    const double p_eff_ticks = (double) p_eff / 4294967296.0;

    if (next_ticks_ <= now) {
      const double behind = (double) (now - next_ticks_);
      const double m = std::floor(behind / p_eff_ticks) + 1.0;
      if (behind > 2.0 * (double) qpf_) {
        // 停頓超過 2 s（例：vrcompositor 暫停）：一次重新起算，不逐週期相加
        r.missed = m > 4294967295.0 ? 0xFFFFFFFFu : (uint32_t) m;
        next_ticks_ = now + (int64_t) p_eff_ticks;
        next_frac_ = 0;
      } else {
        r.missed = (uint32_t) m;
        advance(r.missed, p_eff);
      }
    }
    const uint32_t thr = throttle_frames > 4 ? 4 : throttle_frames;
    r.vsync_qpc = next_ticks_;
    r.target_qpc = next_ticks_ + (int64_t) (thr * p_eff_ticks);
    r.period_ns = (uint64_t) (p_eff_ticks * 1e9 / (double) qpf_);
    return r;
  }

  uint32_t virtual_vsync_t::wait_until(void *timer, int64_t target) const {
    const int64_t start = now_qpc();
    if (!active() || target <= start) {
      return 0;
    }
    // 睡眠上限 2T
    const int64_t two_t = (int64_t) (2 * (last_period_q32_ >> 32));
    int64_t end = target;
    if (end - start > two_t) {
      end = start + two_t;
    }
    const int64_t spin_ticks = qpf_ * k_spin_us / 1000000;
    const int64_t sleep_ticks = end - start - spin_ticks;
    if (sleep_ticks > 0) {
      LARGE_INTEGER due;
      due.QuadPart = -(LONGLONG) (sleep_ticks * 10000000 / qpf_);  // 100 ns，相對
      if (due.QuadPart == 0) {
        due.QuadPart = -1;
      }
      if (timer && SetWaitableTimer((HANDLE) timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject((HANDLE) timer, 100);
      } else {
        const DWORD ms = (DWORD) (sleep_ticks * 1000 / qpf_);
        if (ms > 0) {
          Sleep(ms);
        }
      }
    }
    while (now_qpc() < end) {
      YieldProcessor();
    }
    return (uint32_t) ((now_qpc() - start) * 1000000 / qpf_);
  }

}  // namespace vrdrv
