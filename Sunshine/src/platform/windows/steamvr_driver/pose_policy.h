// pose_policy.h - VipleStream §VR（2026-10-05、2026-10-31）：tracking 送給 SteamVR 的速度與 poseTimeOffset 怎麼決定（純函式）。
//
// 不依賴 OpenVR 與 Win32，tracking.cpp 與單元測試共用。
//   - 舊規則（legacy）：超過 2T（stale_zero_vel_us）沒有新樣本就重送上一個 pose，速度與 HMD 的 poseTimeOffset 歸零；
//     控制器的 poseTimeOffset 一直是 0。
//   - §VR-CTRL-OFFSET（pose_flags bit0）：控制器的 poseTimeOffset 與 HMD 用同一個目標時間（client 以同一個預測時間
//     locate 頭與雙手），上限改用 ctrl_extrap_cap_us。
//   - §VR-STALE-HOLD（pose_flags bit1）：2T < age ≤ hold_max_us（stale_oor_ctrl_us，300 ms）時保留線速度，
//     poseTimeOffset 照實設成「目標 − 現在」（下限放寬到 −hold_max_us），SteamVR 從樣本的目標時間繼續外插；
//     角速度在 50 ms 內線性衰減到 0。超過 hold_max_us 回到舊規則。
//   - §VR-STALE-BLEND（pose_flags bit3，2026-10-31）：新預設 stale 政策。進 stale 後 offset 不歸零（保持最後一筆有效值），
//     線速度與角速度在 40 ms 內線性衰減到 0；衰減期間位置與旋轉外插，超過 40 ms 後姿態停在外插終點不再前進；
//     新樣本到達後 40 ms 內，回報姿態在凍結/外插終點與新姿態間線性內插/slerp，消除硬跳。空窗 < 2T（未進 stale）不混合。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "vr_math.h"

namespace vrdrv {

  constexpr double k_offset_floor_s = -0.05;  ///< 平常的 poseTimeOffset 下限（-50 ms）
  constexpr double k_hold_ang_fade_us = 50000.0;  ///< hold 期間角速度衰減到 0 的時間（50 ms）

  /// §VR-STALE-BLEND 常數（2026-10-31）：
  /// 40 ms 速度線性衰減時間（約 120 Hz 下 5 幀、90 Hz 下 3.6 幀）：
  /// 避免一步歸零帶來的加速度衝擊與頓挫感，同時防止長時間以舊速度外插導致大幅過衝；超過 40 ms 後停在外插終點不再前進。
  constexpr uint32_t k_blend_decay_us = 40000;
  constexpr double k_blend_decay_s = 0.040;

  /// 40 ms 恢復混合時間：
  /// 新樣本到達後 40 ms 內在「凍結（或外插終點）姿態」與新姿態之間以線性/slerp 內插，權重依時間 0→1，
  /// 速度直接用新樣本值；既能完全消除空窗恢復時的硬跳（jump），又不會產生拖泥帶水的黏滯感。
  constexpr uint32_t k_blend_recover_us = 40000;
  constexpr double k_blend_recover_s = 0.040;

  struct pose_policy_in_t {
    int64_t age_us = 0;  ///< 距上一張新樣本（新樣本那一次是 0）
    uint32_t zero_vel_us = 25000;  ///< stale 門檻：max(2T, 25 ms)（session config 的 stale_zero_vel_us）
    uint32_t hold_max_us = 300000;  ///< hold 上限／控制器 OOR 門檻（300 ms，原 100 ms）
    bool ctrl_offset = true;  ///< pose_flags bit0（預設 enabled）
    bool stale_hold = false;  ///< pose_flags bit1
    bool stale_blend = true;  ///< pose_flags bit3（VRIPC_POSE_F_STALE_BLEND，預設 enabled）
    double raw_off_s = 0.0;  ///< (樣本的目標時間 − 現在)，秒
    uint32_t hmd_cap_us = 50000;  ///< hmd_extrap_cap_us（0＝50 ms）
    uint32_t ctrl_cap_us = 50000;  ///< ctrl_extrap_cap_us（0＝50 ms）
  };

  struct pose_policy_t {
    bool stale = false;  ///< age > zero_vel_us
    bool hold = false;  ///< §VR-STALE-HOLD 生效中
    bool blend = false;  ///< §VR-STALE-BLEND 生效中
    /// §REVIEW-1103：設定上有沒有開 §VR-STALE-BLEND（不論目前是否 stale）。恢復混合要看這個——
    /// 新樣本到達那一刻 age＝0、stale＝false，blend 必為 false，拿 blend 判斷會讓恢復混合永遠不觸發。
    bool blend_enabled = false;
    bool zero_vel = false;  ///< 速度與 offset 歸零（legacy 或 blend 衰減完畢）
    bool keep_offset = false;  ///< 進 stale 後保持最後一筆有效 offset（blend 模式為 true）
    double lin_scale = 1.0;  ///< 線速度衰減倍率
    double ang_scale = 1.0;  ///< 角速度衰減倍率
    double extrap_dt_s = 0.0;  ///< blend 模式下的外插時間積分量（秒，上限 20 ms）
    double hmd_off_s = 0.0;  ///< HMD 的 poseTimeOffset
    double ctrl_off_s = 0.0;  ///< 控制器的 poseTimeOffset
  };

  inline pose_policy_t decide_pose_policy(const pose_policy_in_t &in) {
    pose_policy_t r;
    r.stale = in.age_us > (int64_t) in.zero_vel_us;
    r.blend = r.stale && in.stale_blend;
    r.blend_enabled = in.stale_blend;
    r.hold = r.stale && !r.blend && in.stale_hold && in.age_us <= (int64_t) in.hold_max_us;

    if (r.blend) {
      r.keep_offset = true;
      const int64_t stale_dt_us = in.age_us - (int64_t) in.zero_vel_us;
      if (stale_dt_us < (int64_t) k_blend_decay_us) {
        const double u = (double) stale_dt_us / (double) k_blend_decay_us;
        r.lin_scale = std::clamp(1.0 - u, 0.0, 1.0);
        r.ang_scale = r.lin_scale;
        const double t_s = (double) stale_dt_us / 1e6;
        // ∫₀ᵗ (1 - τ/T) dτ = t - t²/(2T)
        r.extrap_dt_s = t_s - (t_s * t_s) / (2.0 * k_blend_decay_s);
        r.zero_vel = false;
      } else {
        r.lin_scale = 0.0;
        r.ang_scale = 0.0;
        // 超過 40 ms：停在外插終點（∫₀ᵀ (1 - τ/T) dτ = T/2 = 20 ms），不再前進
        r.extrap_dt_s = k_blend_decay_s * 0.5;
        r.zero_vel = true;
      }
    } else if (r.hold) {
      r.ang_scale = std::clamp(1.0 - (double) (in.age_us - (int64_t) in.zero_vel_us) / k_hold_ang_fade_us, 0.0, 1.0);
      r.lin_scale = 1.0;
      r.zero_vel = false;
    } else {
      r.zero_vel = r.stale;
      if (r.stale) {
        r.lin_scale = 0.0;
        r.ang_scale = 0.0;
      }
    }

    const double off_floor = (r.hold || r.blend) ? -(double) in.hold_max_us / 1e6 : k_offset_floor_s;
    auto clamp_off = [&](uint32_t cap_us) {
      const double cap = (double) (cap_us != 0 ? cap_us : 50000) / 1e6;
      double o = in.raw_off_s;
      if (!std::isfinite(o) || o < off_floor) {
        o = off_floor;
      }
      if (o > cap) {
        o = cap;
      }
      return (r.zero_vel && !r.keep_offset) ? 0.0 : o;
    };
    r.hmd_off_s = clamp_off(in.hmd_cap_us);
    r.ctrl_off_s = in.ctrl_offset ? clamp_off(in.ctrl_cap_us) : 0.0;
    return r;
  }

  /// 輸出 log 的頻率限制器（以 QPC 計時，每秒最多 allow 次）
  struct rate_limiter_t {
    int64_t window_start_qpc = 0;
    uint32_t count = 0;

    bool allow(int64_t now_qpc, int64_t qpf, uint32_t max_per_sec = 5) {
      if (window_start_qpc == 0 || (now_qpc - window_start_qpc) >= qpf) {
        window_start_qpc = now_qpc;
        count = 1;
        return true;
      }
      if (count < max_per_sec) {
        ++count;
        return true;
      }
      return false;
    }
  };

  struct stale_blend_result_t {
    math::vec3_t pos {};
    math::quat_t rot {};
    math::vec3_t lin_vel {};
    math::vec3_t ang_vel {};
    double off_s = 0.0;

    // log 事件
    bool log_enter = false;
    double enter_age_ms = 0.0;
    double enter_off0_ms = 0.0;
    double enter_speed_mps = 0.0;

    bool log_recover = false;
    double recover_gap_ms = 0.0;
    double recover_jump_mm = 0.0;
    bool recover_blended = false;
  };

  /// 單一追蹤裝置（HMD 或手把）的姿態外插衰減與恢復混合器
  class stale_blender_t {
  public:
    void reset() {
      has_last_output_ = false;
      in_stale_ = false;
      was_stale_ = false;
      recovering_ = false;
      recover_start_qpc_ = 0;
      last_new_qpc_ = 0;
    }

    stale_blend_result_t process(bool fresh, const float in_pos[3], const float in_rot[4], const float in_lin_vel[3],
                                 const float in_ang_vel[3], const pose_policy_t &pol, double nominal_off_s,
                                 int64_t now_qpc, int64_t qpf) {
      stale_blend_result_t res;

      if (fresh) {
        const math::vec3_t new_pos {in_pos[0], in_pos[1], in_pos[2]};
        const math::quat_t new_rot = math::normalize(math::quat_t {in_rot[0], in_rot[1], in_rot[2], in_rot[3]});
        const math::vec3_t new_lin_vel {in_lin_vel[0], in_lin_vel[1], in_lin_vel[2]};
        const math::vec3_t new_ang_vel {in_ang_vel[0], in_ang_vel[1], in_ang_vel[2]};

        if (was_stale_ && has_last_output_) {
          // 只有先前進入過 stale（空窗 >= 2T）才在恢復時混合；空窗 < 2T 不混合
          const double dx = new_pos.x - last_output_pos_.x;
          const double dy = new_pos.y - last_output_pos_.y;
          const double dz = new_pos.z - last_output_pos_.z;
          const double jump_mm = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
          const double gap_ms = last_new_qpc_ != 0 ? (double) (now_qpc - last_new_qpc_) * 1000.0 / (double) qpf : 0.0;

          res.log_recover = true;
          res.recover_gap_ms = gap_ms;
          res.recover_jump_mm = jump_mm;

          if (pol.blend_enabled) {  // §REVIEW-1103：原本看 pol.blend，在新樣本那一刻必為 false
            recovering_ = true;
            recover_start_qpc_ = now_qpc;
            blend_from_pos_ = last_output_pos_;
            blend_from_rot_ = last_output_rot_;
            res.recover_blended = true;
          } else {
            recovering_ = false;
            res.recover_blended = false;
          }
        }

        in_stale_ = false;
        was_stale_ = false;

        base_pos_ = new_pos;
        base_rot_ = new_rot;
        base_lin_vel_ = new_lin_vel;
        base_ang_vel_ = new_ang_vel;
        last_valid_off_ = nominal_off_s;
        last_new_qpc_ = now_qpc;

        if (recovering_) {
          const int64_t dt_us = qpf > 0 ? (now_qpc - recover_start_qpc_) * 1000000 / qpf : (int64_t) k_blend_recover_us;
          if (dt_us < (int64_t) k_blend_recover_us) {
            const double alpha = std::clamp((double) dt_us / (double) k_blend_recover_us, 0.0, 1.0);
            res.pos = math::lerp(blend_from_pos_, base_pos_, alpha);
            res.rot = math::slerp(blend_from_rot_, base_rot_, alpha);
            // 速度直接用新樣本的值
            res.lin_vel = base_lin_vel_;
            res.ang_vel = base_ang_vel_;
            res.off_s = nominal_off_s;
          } else {
            recovering_ = false;
            res.pos = base_pos_;
            res.rot = base_rot_;
            res.lin_vel = base_lin_vel_;
            res.ang_vel = base_ang_vel_;
            res.off_s = nominal_off_s;
          }
        } else {
          res.pos = base_pos_;
          res.rot = base_rot_;
          res.lin_vel = base_lin_vel_;
          res.ang_vel = base_ang_vel_;
          res.off_s = nominal_off_s;
        }

        last_output_pos_ = res.pos;
        last_output_rot_ = res.rot;
        has_last_output_ = true;
      } else {
        // stale 期間重送
        if (!in_stale_) {
          in_stale_ = true;
          was_stale_ = true;
          recovering_ = false;

          res.log_enter = true;
          res.enter_age_ms = (last_new_qpc_ != 0 && qpf > 0) ? (double) (now_qpc - last_new_qpc_) * 1000.0 / (double) qpf : 0.0;
          res.enter_off0_ms = last_valid_off_ * 1000.0;
          res.enter_speed_mps = std::sqrt(base_lin_vel_.x * base_lin_vel_.x + base_lin_vel_.y * base_lin_vel_.y + base_lin_vel_.z * base_lin_vel_.z);
        }

        if (pol.blend) {
          // 線速度與角速度在 40 ms 內線性衰減到 0，姿態外插並停在終點不再前進；保持最後一筆 offset
          res.pos = math::vec3_t {
            base_pos_.x + base_lin_vel_.x * pol.extrap_dt_s,
            base_pos_.y + base_lin_vel_.y * pol.extrap_dt_s,
            base_pos_.z + base_lin_vel_.z * pol.extrap_dt_s,
          };
          const math::quat_t rot_delta = math::quat_from_angvel(base_ang_vel_, pol.extrap_dt_s);
          res.rot = math::normalize(math::mul(rot_delta, base_rot_));
          res.lin_vel = math::vec3_t {
            base_lin_vel_.x * pol.lin_scale,
            base_lin_vel_.y * pol.lin_scale,
            base_lin_vel_.z * pol.lin_scale,
          };
          res.ang_vel = math::vec3_t {
            base_ang_vel_.x * pol.ang_scale,
            base_ang_vel_.y * pol.ang_scale,
            base_ang_vel_.z * pol.ang_scale,
          };
          res.off_s = last_valid_off_;
        } else if (pol.hold) {
          res.pos = base_pos_;
          res.rot = base_rot_;
          res.lin_vel = base_lin_vel_;
          res.ang_vel = math::vec3_t {
            base_ang_vel_.x * pol.ang_scale,
            base_ang_vel_.y * pol.ang_scale,
            base_ang_vel_.z * pol.ang_scale,
          };
          res.off_s = nominal_off_s;
        } else {
          // legacy：速度歸零，offset 歸零，姿態停留在原始樣本位置
          res.pos = base_pos_;
          res.rot = base_rot_;
          res.lin_vel = math::vec3_t {0.0, 0.0, 0.0};
          res.ang_vel = math::vec3_t {0.0, 0.0, 0.0};
          res.off_s = 0.0;
        }

        last_output_pos_ = res.pos;
        last_output_rot_ = res.rot;
        has_last_output_ = true;
      }

      return res;
    }

  private:
    math::vec3_t last_output_pos_ {};
    math::quat_t last_output_rot_ {};
    bool has_last_output_ = false;

    math::vec3_t base_pos_ {};
    math::quat_t base_rot_ {};
    math::vec3_t base_lin_vel_ {};
    math::vec3_t base_ang_vel_ {};
    double last_valid_off_ = 0.0;
    int64_t last_new_qpc_ = 0;

    bool in_stale_ = false;
    bool was_stale_ = false;

    bool recovering_ = false;
    int64_t recover_start_qpc_ = 0;
    math::vec3_t blend_from_pos_ {};
    math::quat_t blend_from_rot_ {};
  };

}  // namespace vrdrv
