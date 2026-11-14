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
//   - §VR-STALE-COAST（pose_flags bit4，2026-11-13，vr_stale_policy = coast；預設仍是 blend）：修 blend 的三個問題。
//     SteamVR 的語意是「姿態時刻＝回報時刻＋poseTimeOffset，之後照回報的速度外插」。blend 進 stale 重送時 offset 不變、
//     位置卻只補了「過了門檻之後」那一小段，等於把同一個位置重新標成晚 25～50 ms 的時刻——SteamVR 已經外插出去的
//     「速度×25～50 ms」整段被收回，拍子往回跳（外出 15 分鐘 453 次，右手最大約 14 cm）；接著 40 ms 內速度被壓到 0，
//     而七成的空窗不到 40 ms；恢復時又拿固定的凍結點對移動中的新位置做內插，拍子持續落後。coast 的做法：
//       1. 重送的姿態取自一條連續的軌跡：姿態時刻每前進 x 秒，位置前進「速度×g(x)」。空窗 60 ms 以內 g(x)＝x
//          （照最後的速度等速前進，和完全不重送時 SteamVR 自己外插的結果相同）；之後 40 ms 內速度線性降到 0。
//          衰減從「第一次在 60 ms 之後重送」的那一刻起算，重送晚了也不會跳。
//       2. 恢復時算出「舊軌跡走到新樣本的姿態時刻時在哪裡」與新樣本的落差，把落差當成一個 40 ms 內線性消掉的偏移量
//          加在之後的新樣本上（等速運動時落差是 0，完全沒有副作用）；速度直接用新樣本的。
//       3. stale 期間每 5 ms 重送一次（原本每 25 ms：衰減曲線只被取樣一兩次，中間是鋸齒）。
//     空窗裡的加速度沒有資料，誤差修不掉（½·a·t²）；這裡只保證我們自己不再製造跳動。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

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

  /// §VR-STALE-COAST 常數（2026-11-13）：
  /// 等速前進到空窗 60 ms：外出實測的空窗七成不到 40 ms、九成不到 80 ms，這一段不動速度就是零誤差（等速時）；
  /// SteamVR 自己的外插在約 100 ms 封頂，所以 60 ms 起用 40 ms 把速度降到 0，剛好在 100 ms 停下來。
  constexpr double k_coast_start_s = 0.060;
  constexpr double k_coast_start_min_s = 0.025;  ///< SteamVR 的外插很長時，最早這麼久就開始降速（不早於 stale 門檻）
  constexpr double k_coast_total_s = 0.080;  ///< 顯示出來的總外插（SteamVR 的那一段＋空窗）到這麼多就開始降速
  constexpr double k_coast_decay_s = 0.040;
  constexpr double k_coast_recover_s = 0.040;  ///< 恢復時的落差在這段時間內線性消掉
  constexpr uint32_t k_coast_resend_ms = 5;  ///< stale 期間的重送週期
  constexpr uint32_t k_coast_resend_until_ms = 200;  ///< 軌跡還在變的期間才密集重送（60＋40 ms，留餘裕）；之後回到原本的週期

  struct pose_policy_in_t {
    int64_t age_us = 0;  ///< 距上一張新樣本（新樣本那一次是 0）
    uint32_t zero_vel_us = 25000;  ///< stale 門檻：max(2T, 25 ms)（session config 的 stale_zero_vel_us）
    uint32_t hold_max_us = 300000;  ///< hold 上限／控制器 OOR 門檻（300 ms，原 100 ms）
    bool ctrl_offset = true;  ///< pose_flags bit0（預設 enabled）
    bool stale_hold = false;  ///< pose_flags bit1
    bool stale_blend = true;  ///< pose_flags bit3（VRIPC_POSE_F_STALE_BLEND，預設 enabled）
    bool stale_coast = false;  ///< pose_flags bit4（VRIPC_POSE_F_STALE_COAST）；設了就優先於 blend／hold
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
    /// §VR-STALE-COAST 開著（不論目前是否 stale）。stale 時 blend 也會是 true（沿用 keep_offset、pose_history 記錄等既有路徑），
    /// 但倍率與外插量由 stale_blender_t 依實際時間算，這裡的 lin_scale／ang_scale／extrap_dt_s 不用
    bool coast = false;
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
    r.coast = in.stale_coast;
    r.blend = r.stale && (in.stale_blend || in.stale_coast);
    r.blend_enabled = in.stale_blend || in.stale_coast;
    r.hold = r.stale && !r.blend && in.stale_hold && in.age_us <= (int64_t) in.hold_max_us;

    if (r.blend && r.coast) {
      // §VR-STALE-COAST：offset 保持最後一筆有效值（姿態時刻跟著「現在」走），速度不在這裡歸零——軌跡由 stale_blender_t 算
      r.keep_offset = true;
      r.zero_vel = false;
    } else if (r.blend) {
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

  /// SteamVR 會把「offset 是 off_s 的回報」再照回報的速度外插多久：pred_s − off_s，夾在 0～100 ms（SteamVR 自己在約 100 ms 封頂）。
  /// pred_s＝SteamVR 目前替 app 預測姿態的時間（從「現在」起算；driver 從 scene 層的 SubmitLayer 得知，0＝不知道 → 回 0）。
  /// 提交畫面比 app 取姿態晚一個算圖時間，所以這個估計值偏小幾毫秒。
  inline double steamvr_extrap_s(double pred_s, double off_s) {
    if (!(pred_s > 0.0) || !std::isfinite(off_s)) {
      return 0.0;
    }
    return std::clamp(pred_s - off_s, 0.0, 0.1);
  }

  /// §VR-CTRL-EXTRAP（2026-11-13，vr_ctrl_extrap_ms）：SteamVR 對控制器最多只外插 keep_s 秒（< 0＝不限制＝原本的行為）。
  /// SteamVR 只有一個預測時間（HMD 的 vsync-to-photons），我們為了讓頭對上串流畫面顯示的時刻把它設成 30～60 ms，控制器
  /// 也跟著被外插到那裡：頭盔已經預測了約 24 ms 之後，SteamVR 再用「回報當下的速度」直線外插 18～25 ms（區網）／45～55 ms
  /// （外出），而且回給遊戲的速度不跟著外插。揮拍是高加速度的動作——位置被直線延伸、速度卻是舊的，桌球的力道與方向
  /// 因此忽大忽小（頭盔直接執行遊戲時只有前面那 24 ms）。
  /// 做法：回報之前把姿態沿著它自己的速度往回推「超過上限的那一段」，poseTimeOffset 不動。SteamVR 照樣外插整段，結果等於
  /// 只外插了 keep_s（等速時嚴格成立）；不必把 offset 加大，所以不依賴 SteamVR 對 50 ms 以上 offset 的行為。
  /// 回傳值＝要往回推多久（秒）。
  inline double ctrl_backdate_s(double pred_s, double off_s, double keep_s) {
    if (keep_s < 0.0) {
      return 0.0;
    }
    const double full = steamvr_extrap_s(pred_s, off_s);
    return full - (std::min)(full, keep_s);
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
    double back_s = 0.0;  ///< §VR-CTRL-EXTRAP：這一筆被往回推了多久（0＝沒有）

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
      coast_decaying_ = false;
      coast_decay_start_s_ = 0.0;
      coast_dpos_ = {};
      coast_drot_ = {};
    }

    /**
     * @param pred_s SteamVR 目前替 app 預測姿態的時間（從「現在」起算，秒；0＝不知道）。§VR-STALE-COAST 與 §VR-CTRL-EXTRAP 用
     * @param keep_s §VR-CTRL-EXTRAP：SteamVR 對這個裝置最多外插多久（秒；< 0＝不限制）。只有控制器傳值
     * @param raw_off_s 這一筆樣本沒夾過的「目標時間 − 現在」（NaN＝不知道）。§VR-STALE-COAST 用它認出過期的樣本
     * @param expired_max_s 過期的樣本最多連續略過多久（之後照舊接受，免得整條路徑變慢時追蹤一直停著）
     */
    stale_blend_result_t process(bool fresh, const float in_pos[3], const float in_rot[4], const float in_lin_vel[3],
                                 const float in_ang_vel[3], const pose_policy_t &pol, double nominal_off_s,
                                 int64_t now_qpc, int64_t qpf, double pred_s = 0.0, double keep_s = -1.0,
                                 double raw_off_s = std::numeric_limits<double>::quiet_NaN(), double expired_max_s = 0.3) {
      stale_blend_result_t res = pol.coast ?
                                   process_coast(fresh, in_pos, in_rot, in_lin_vel, in_ang_vel, nominal_off_s, now_qpc, qpf, pred_s, keep_s, raw_off_s, expired_max_s) :
                                   process_basic(fresh, in_pos, in_rot, in_lin_vel, in_ang_vel, pol, nominal_off_s, now_qpc, qpf);
      // §VR-CTRL-EXTRAP：最後一步，所有政策共用——把姿態沿著「這一筆回報的速度」往回推超過上限的那一段。
      // 內部狀態（軌跡、上一筆輸出）都留在沒推過的座標，推的只有交出去的這一份
      const double back = ctrl_backdate_s(pred_s, res.off_s, keep_s);
      if (back > 0.0) {
        res.pos = math::vec3_t {res.pos.x - res.lin_vel.x * back, res.pos.y - res.lin_vel.y * back, res.pos.z - res.lin_vel.z * back};
        // quat_from_angvel 不吃負的時間：把角速度反過來轉 back 秒（世界座標的角速度，和 SteamVR 的 q·exp(ω_local·t) 同軸）
        const math::vec3_t neg_w {-res.ang_vel.x, -res.ang_vel.y, -res.ang_vel.z};
        res.rot = math::normalize(math::mul(math::quat_from_angvel(neg_w, back), res.rot));
        res.back_s = back;
      }
      return res;
    }

  private:
    stale_blend_result_t process_basic(bool fresh, const float in_pos[3], const float in_rot[4], const float in_lin_vel[3],
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

    /**
     * §VR-STALE-COAST（見檔頭）。新樣本與 stale 重送都從同一條軌跡取值：
     *   上一筆新樣本的姿態時刻之後 x 秒，位置＝base＋速度×g(x)、旋轉＝exp(角速度×g(x))·base、速度倍率＝g'(x)；
     *   還沒開始衰減時 g(x)＝x；衰減從 coast_decay_start_s_ 起 k_coast_decay_s 內線性降到 0。
     * 重送時 offset 用最後一筆有效值，姿態時刻＝現在＋offset，所以 x＝距上一筆新樣本的時間（age）。
     * 恢復的落差（coast_dpos_／coast_drot_）依時間線性消掉；消到一半又進 stale 或又恢復一次，都接著目前的值算，不會跳。
     *
     * 顯示出來的位置還要再加一段：SteamVR 會把回報的姿態照回報的速度外插到 app 要的時刻，那一段的長度是
     * hl＝pred_s − offset（pred_s＝SteamVR 的預測時間，driver 從 SubmitLayer 得知）。只看回報的姿態連不連續還不夠——
     * 降速時「速度×hl」那一段會跟著縮回去（外出 hl 約 50 ms，3 m/s 時回縮 15 cm），恢復時新速度又讓它一次前跳。
     * 所以降速時把回報的位置往前補「速度×hl×(1 − 倍率)」，恢復的落差也在「外插之後」的位置上算：顯示位置平順地減速到停、
     * 恢復時不跳。pred_s 不知道（0）時 hl 當 0，退回只保證回報的姿態連續。
     * §VR-CTRL-EXTRAP 開著（keep_s ≥ 0）時 hl 是「實際會被外插的長度」＝min(pred_s − offset, keep_s)：process() 最後會把
     * 超過的那一段往回推，降速的補償與恢復的落差都要在這個長度上算。
     *
     * 過期的樣本（2026-11-13 設計評審 C1-a）：外出的空窗多半不是掉包而是延遲，結束時積在路上的樣本成批到達，先讀到的
     * 可能是目標時間已經過去 50 ms 以上的舊樣本——它的 offset 被夾在 −50 ms（k_offset_floor_s），拿夾過的值算落差會多出
     * 「速度×被夾掉的量」，還要 40 ms 才消掉。stale 之後遇到這種樣本就不當新樣本，繼續走軌跡，等同一批裡後面比較新的；
     * 連續 expired_max_s 都只有過期的（整條路徑變慢）才照舊接受。
     */
    stale_blend_result_t process_coast(bool fresh, const float in_pos[3], const float in_rot[4], const float in_lin_vel[3],
                                       const float in_ang_vel[3], double nominal_off_s, int64_t now_qpc, int64_t qpf, double pred_s,
                                       double keep_s, double raw_off_s, double expired_max_s) {
      stale_blend_result_t res;
      const double qf = qpf > 0 ? (double) qpf : 1.0;
      // SteamVR 把「offset 是 off 的回報」實際再外插多久（估計值；§VR-CTRL-EXTRAP 開著時不超過 keep_s）
      auto lookahead = [&](double off) {
        const double full = steamvr_extrap_s(pred_s, off);
        return keep_s < 0.0 ? full : (std::min)(full, keep_s);
      };
      if (fresh && was_stale_ && last_new_qpc_ != 0 && std::isfinite(raw_off_s) && raw_off_s < k_offset_floor_s &&
          (double) (now_qpc - last_new_qpc_) / qf < expired_max_s) {
        fresh = false;  // 過期的樣本：不接受，繼續走軌跡
      }

      // 還沒消完的恢復落差的權重（1→0）
      double w = 0.0;
      if (recovering_) {
        const double dt = (double) (now_qpc - recover_start_qpc_) / qf;
        if (dt < 0.0 || dt >= k_coast_recover_s) {
          recovering_ = false;
        } else {
          w = 1.0 - dt / k_coast_recover_s;
        }
      }
      auto add_offset = [&](math::vec3_t &pos, math::quat_t &rot, double weight) {
        if (weight <= 0.0) {
          return;
        }
        pos.x += coast_dpos_.x * weight;
        pos.y += coast_dpos_.y * weight;
        pos.z += coast_dpos_.z * weight;
        rot = math::normalize(math::mul(math::slerp(math::quat_t {}, coast_drot_, weight), rot));
      };
      // 舊軌跡在「上一筆新樣本的姿態時刻之後 x 秒」要回報的姿態（hl＝那一筆回報會被 SteamVR 再外插多久）；回傳那一刻的速度倍率
      auto traj = [&](double x, double hl, math::vec3_t &pos, math::quat_t &rot) {
        double g = x > 0.0 ? x : 0.0;
        double scale = 1.0;
        if (coast_decaying_ && g > coast_decay_start_s_) {
          const double y = g - coast_decay_start_s_;
          if (y < k_coast_decay_s) {
            g = coast_decay_start_s_ + y - y * y / (2.0 * k_coast_decay_s);  // ∫₀ʸ (1 − τ/T) dτ
            scale = 1.0 - y / k_coast_decay_s;
          } else {
            g = coast_decay_start_s_ + k_coast_decay_s * 0.5;
            scale = 0.0;
          }
        }
        // 降速時 SteamVR 少外插的那一段（速度×hl×(1 − 倍率)）補在回報的位置上，顯示位置才不會回縮
        const double adv = g + hl * (1.0 - scale);
        pos = math::vec3_t {base_pos_.x + base_lin_vel_.x * adv, base_pos_.y + base_lin_vel_.y * adv, base_pos_.z + base_lin_vel_.z * adv};
        rot = math::normalize(math::mul(math::quat_from_angvel(base_ang_vel_, adv), base_rot_));
        return scale;
      };

      if (fresh) {
        const math::vec3_t new_pos {in_pos[0], in_pos[1], in_pos[2]};
        const math::quat_t new_rot = math::normalize(math::quat_t {in_rot[0], in_rot[1], in_rot[2], in_rot[3]});
        const math::vec3_t new_lin_vel {in_lin_vel[0], in_lin_vel[1], in_lin_vel[2]};
        const math::vec3_t new_ang_vel {in_ang_vel[0], in_ang_vel[1], in_ang_vel[2]};

        if (was_stale_ && has_last_output_ && last_new_qpc_ != 0) {
          // 只有進過 stale 才處理（空窗 < 門檻時 SteamVR 一直照上一筆樣本外插，直接換成新樣本，和原本相同）。
          // 落差＝「這一刻 SteamVR 顯示出來的位置」在換樣本前後的差：舊的＝現在這一刻重送會回報的姿態（軌跡在 x＝gap 的值）
          // 再照舊速度（已乘倍率）走 hl_old；新的＝新樣本照新速度走 hl。兩個 hl 各用各的 offset 算——晚到的樣本 offset 比較小
          // （甚至是負的），SteamVR 會把它多外插一段，兩者的姿態時刻不同，不可以直接拿回報的姿態相減
          const double gap_s = (double) (now_qpc - last_new_qpc_) / qf;
          const double hl = lookahead(nominal_off_s);
          const double hl_old = lookahead(last_valid_off_);
          math::vec3_t old_pos;
          math::quat_t old_rot;
          const double old_scale = traj(gap_s, hl_old, old_pos, old_rot);
          add_offset(old_pos, old_rot, w);  // 上一次的落差還沒消完：舊軌跡含它
          const double oh = old_scale * hl_old;
          old_pos = math::vec3_t {old_pos.x + base_lin_vel_.x * oh, old_pos.y + base_lin_vel_.y * oh, old_pos.z + base_lin_vel_.z * oh};
          old_rot = math::normalize(math::mul(math::quat_from_angvel(base_ang_vel_, oh), old_rot));
          const math::vec3_t new_disp {new_pos.x + new_lin_vel.x * hl, new_pos.y + new_lin_vel.y * hl, new_pos.z + new_lin_vel.z * hl};
          const math::quat_t new_rot_disp = math::normalize(math::mul(math::quat_from_angvel(new_ang_vel, hl), new_rot));
          coast_dpos_ = math::vec3_t {old_pos.x - new_disp.x, old_pos.y - new_disp.y, old_pos.z - new_disp.z};
          coast_drot_ = math::normalize(math::mul(old_rot, math::conj(new_rot_disp)));
          recovering_ = true;
          recover_start_qpc_ = now_qpc;
          w = 1.0;
          res.log_recover = true;
          res.recover_gap_ms = gap_s * 1000.0;
          res.recover_jump_mm = std::sqrt(coast_dpos_.x * coast_dpos_.x + coast_dpos_.y * coast_dpos_.y + coast_dpos_.z * coast_dpos_.z) * 1000.0;
          res.recover_blended = true;
        }

        in_stale_ = false;
        was_stale_ = false;
        coast_decaying_ = false;
        base_pos_ = new_pos;
        base_rot_ = new_rot;
        base_lin_vel_ = new_lin_vel;
        base_ang_vel_ = new_ang_vel;
        last_valid_off_ = nominal_off_s;
        last_new_qpc_ = now_qpc;

        res.pos = base_pos_;
        res.rot = base_rot_;
        add_offset(res.pos, res.rot, w);
        res.lin_vel = base_lin_vel_;
        res.ang_vel = base_ang_vel_;
        res.off_s = nominal_off_s;
      } else if (last_new_qpc_ == 0) {
        // 還沒有任何樣本（這個裝置一直不在）：沒有軌跡可取，呼叫端也不會把它當成有效姿態
        res.off_s = 0.0;
        return res;
      } else {
        const double age_s = (double) (now_qpc - last_new_qpc_) / qf;
        if (!in_stale_) {
          in_stale_ = true;
          was_stale_ = true;
          res.log_enter = true;
          res.enter_age_ms = age_s * 1000.0;
          res.enter_off0_ms = last_valid_off_ * 1000.0;
          res.enter_speed_mps = std::sqrt(base_lin_vel_.x * base_lin_vel_.x + base_lin_vel_.y * base_lin_vel_.y + base_lin_vel_.z * base_lin_vel_.z);
        }
        const double hl = lookahead(last_valid_off_);
        // 開始降速的時刻：顯示出來的總外插（SteamVR 的 hl＋空窗）到約 80 ms 時開始、120 ms 停，和 SteamVR 自己約 100 ms 封頂
        // 相當；hl 很長時（外出約 50 ms）不早於門檻 25 ms，hl 短或不知道時最晚 60 ms
        const double start_s = std::clamp(k_coast_total_s - hl, k_coast_start_min_s, k_coast_start_s);
        if (!coast_decaying_ && age_s >= start_s) {
          // 衰減從「第一次在這之後重送」的這一刻起算：在這之前 SteamVR 一直照原速外插，從這裡接下去才連續
          coast_decaying_ = true;
          coast_decay_start_s_ = age_s;
        }
        const double scale = traj(age_s, hl, res.pos, res.rot);
        add_offset(res.pos, res.rot, w);
        res.lin_vel = math::vec3_t {base_lin_vel_.x * scale, base_lin_vel_.y * scale, base_lin_vel_.z * scale};
        res.ang_vel = math::vec3_t {base_ang_vel_.x * scale, base_ang_vel_.y * scale, base_ang_vel_.z * scale};
        res.off_s = last_valid_off_;
      }

      last_output_pos_ = res.pos;
      last_output_rot_ = res.rot;
      has_last_output_ = true;
      return res;
    }

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

    // §VR-STALE-COAST
    bool coast_decaying_ = false;  ///< 這一次空窗已經開始衰減
    double coast_decay_start_s_ = 0.0;  ///< 開始衰減時距上一筆新樣本多久
    math::vec3_t coast_dpos_ {};  ///< 恢復時的落差（舊軌跡 − 新樣本），依時間消掉
    math::quat_t coast_drot_ {};
  };

}  // namespace vrdrv
