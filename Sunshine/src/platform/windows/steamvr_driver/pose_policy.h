// pose_policy.h - VipleStream §VR（2026-10-05）：tracking 送給 SteamVR 的速度與 poseTimeOffset 怎麼決定（純函式）。
//
// 不依賴 OpenVR 與 Win32，tracking.cpp 與單元測試共用。
//   - 舊規則：超過 2T（stale_zero_vel_us）沒有新樣本就重送上一個 pose，速度與 HMD 的 poseTimeOffset 歸零；
//     控制器的 poseTimeOffset 一直是 0。
//   - §VR-CTRL-OFFSET（pose_flags bit0）：控制器的 poseTimeOffset 與 HMD 用同一個目標時間（client 以同一個預測時間
//     locate 頭與雙手），上限改用 ctrl_extrap_cap_us。
//   - §VR-STALE-HOLD（pose_flags bit1）：2T < age ≤ hold_max_us（stale_oor_ctrl_us，預設 100 ms）時保留線速度，
//     poseTimeOffset 照實設成「目標 − 現在」（下限放寬到 −hold_max_us），SteamVR 從樣本的目標時間繼續外插；
//     角速度在 50 ms 內線性衰減到 0（座標語意還沒以探測確認，避免長外插放大可能的錯誤）。超過 hold_max_us 回到舊規則。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vrdrv {

  struct pose_policy_in_t {
    int64_t age_us = 0;  ///< 距上一張新樣本（新樣本那一次是 0）
    uint32_t zero_vel_us = 22222;  ///< 2T（session config 的 stale_zero_vel_us）
    uint32_t hold_max_us = 100000;  ///< hold 上限（session config 的 stale_oor_ctrl_us）
    bool ctrl_offset = false;  ///< pose_flags bit0
    bool stale_hold = false;  ///< pose_flags bit1
    double raw_off_s = 0.0;  ///< (樣本的目標時間 − 現在)，秒
    uint32_t hmd_cap_us = 50000;  ///< hmd_extrap_cap_us（0＝50 ms）
    uint32_t ctrl_cap_us = 50000;  ///< ctrl_extrap_cap_us（0＝50 ms）
  };

  struct pose_policy_t {
    bool stale = false;  ///< age > 2T
    bool hold = false;  ///< §VR-STALE-HOLD 生效中
    bool zero_vel = false;  ///< 速度與 offset 歸零（舊規則）
    double ang_scale = 1.0;  ///< 角速度倍率
    double hmd_off_s = 0.0;  ///< HMD 的 poseTimeOffset
    double ctrl_off_s = 0.0;  ///< 控制器的 poseTimeOffset（ctrl_offset 關閉時 0）
  };

  constexpr double k_offset_floor_s = -0.05;  ///< 平常的 poseTimeOffset 下限
  constexpr double k_hold_ang_fade_us = 50000.0;  ///< hold 期間角速度衰減到 0 的時間

  inline pose_policy_t decide_pose_policy(const pose_policy_in_t &in) {
    pose_policy_t r;
    r.stale = in.age_us > (int64_t) in.zero_vel_us;
    r.hold = r.stale && in.stale_hold && in.age_us <= (int64_t) in.hold_max_us;
    r.zero_vel = r.stale && !r.hold;
    if (r.hold) {
      r.ang_scale = std::clamp(1.0 - (double) (in.age_us - (int64_t) in.zero_vel_us) / k_hold_ang_fade_us, 0.0, 1.0);
    }
    const double off_floor = r.hold ? -(double) in.hold_max_us / 1e6 : k_offset_floor_s;
    auto clamp_off = [&](uint32_t cap_us) {
      const double cap = (double) (cap_us != 0 ? cap_us : 50000) / 1e6;
      double o = in.raw_off_s;
      if (!std::isfinite(o) || o < off_floor) {
        o = off_floor;
      }
      if (o > cap) {
        o = cap;
      }
      return r.zero_vel ? 0.0 : o;
    };
    r.hmd_off_s = clamp_off(in.hmd_cap_us);
    r.ctrl_off_s = in.ctrl_offset ? clamp_off(in.ctrl_cap_us) : 0.0;
    return r;
  }

}  // namespace vrdrv
