// virtual_vsync.h - VipleStream §VR：虛擬 vsync（設計 §F.2 driver 端；S2-08 V4 = production 模式）。
//
// 固定週期、有界 slew、起播一次性相位對齊；pacing 是不可信輸入（drv-B-2）：
//   週期不在 [0.95, 1.05] × P_nom、slew > VRIPC_PACING_SLEW_PPM_MAX、或 mode 未知／沒有 dev pacing 卻 ≠ 0
//   → 改用 P_nom（Activate 時的 1/Hz）、slew 上限 200 ppm，並回報 rejected。
// 計算部分（step()）是純函式，vr_probe --mode unit 直接餵異常值驗證；等待部分（wait_until()）在 PostPresent 內呼叫。
// 所有時間都是 QPC tick；週期以 tick × 2^32 的定點數表示（與 vr_ipc_abi.h 的 pacing.period_q32 相同）。
#pragma once

#include <cstdint>

#include "vr_ipc_abi.h"

namespace vrdrv {

  class virtual_vsync_t {
  public:
    struct step_result_t {
      uint32_t missed = 0;  // 這次 PostPresent 跳過的虛擬 vsync 數（0 = 準時）
      int64_t target_qpc = 0;  // PostPresent 要睡到的時刻（含 throttle）
      int64_t vsync_qpc = 0;  // 本次對齊的虛擬 vsync（last_vsync_qpc）
      uint64_t period_ns = 0;  // 目前有效週期（ns，timing ring 用）
      bool pacing_rejected = false;  // 這次的 pacing 被拒（呼叫端每秒最多記一次）
      bool snapped = false;  // 這次做了一次性相位對齊
      int64_t snap_ticks = 0;
      uint32_t mode = VRIPC_PM_PRODUCTION;  // 採用的模式（被拒時回到 production）
    };

    // Activate 時呼叫一次（K20：Hz 在本 vrserver 生命週期內不變）。refresh_mhz = Hz × 1000。
    void activate(int64_t qpf, uint32_t refresh_mhz, int64_t now_qpc);

    bool active() const {
      return p_nom_q32_ != 0;
    }

    // PostPresent 的計算部分。pacing 為 nullptr 表示讀不到（沿用上一份；第一次則用 P_nom）。
    // dev_pacing：WELCOME 帶 VRIPC_WF_DEV_PACING（只在 selftest）才接受 mode ≠ 0。
    // throttle_frames：Throttling_t.nFramesToThrottle（null 視為 0；上限 4）。
    step_result_t step(const vripc_pacing_t *pacing, bool dev_pacing, int64_t now_qpc, uint32_t throttle_frames);

    int64_t qpf() const {
      return qpf_;
    }

    // production／E3：高解析度 waitable timer 睡到 target − 300 µs，再 spin 到 target；睡眠上限 min(target − now, 2T)。
    // timer 由呼叫端建立（CREATE_WAITABLE_TIMER_HIGH_RESOLUTION；nullptr 時退回 Sleep）。回傳實際睡了多少 µs。
    uint32_t wait_until(void *timer, int64_t target_qpc) const;

  private:
    int64_t qpf_ = 0;
    uint64_t p_nom_q32_ = 0;  // QPC ticks × 2^32
    // next_ 以 tick × 2^32 的定點數保存，避免每次加整數週期累積捨入誤差
    // （int64 的整數部分：QPC 10 MHz 下可表示約 2^31 tick ≈ 214 s，不夠長；所以拆成整數 tick＋小數）
    int64_t next_ticks_ = 0;
    uint32_t next_frac_ = 0;
    uint64_t last_period_q32_ = 0;
    uint32_t last_slew_max_ = 200;
    uint32_t epoch_seen_ = 0;
    bool epoch_valid_ = false;
    bool snapped_ = false;
    bool have_pacing_ = false;
    vripc_pacing_t last_pacing_ {};

    void advance(uint64_t n, uint64_t period_q32);
  };

}  // namespace vrdrv
