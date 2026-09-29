// pose_history.h - VipleStream §VR（M1b S2-06）：SubmitLayer 的 mHmdPose ↔ 回報過的 tracking 樣本配對，
// 以及 space-delta（SteamVR 世界空間 ↔ client 空間的差）學習。設計 §E.2「pose_history + space-delta」。
//
// 純模組：不依賴 OpenVR，只用 vr_math.h 與 Win32 SRWLOCK；vr_probe --mode unit 連同一份原始碼測試（K16）。
// 執行緒：record() 在 tracking 執行緒、note_event() 在 RunFrame、match() 在 vrserver 的 compositor 執行緒，
// 內部一把 SRW 鎖。
#pragma once

#include <cstdint>
#include <string>

#include <windows.h>

#include "vr_math.h"

namespace vrdrv {

  struct ph_pose_t {
    math::quat_t rot;
    math::vec3_t pos;
  };

  /// 回報給 SteamVR 的 HMD 樣本（client 空間）
  struct ph_sample_t {
    uint32_t sample_id = 0;
    ph_pose_t pose;
    math::vec3_t lin_vel;  ///< m/s
    math::vec3_t ang_vel;  ///< rad/s，world space（DriverPose_t::vecAngularVelocity 的語意）
    int64_t reported_qpc = 0;  ///< TrackedDevicePoseUpdated 的時間
    double offset_s = 0.0;  ///< 實際採用的 poseTimeOffset（clamp 後）
  };

  struct ph_match_t {
    bool have_candidate = false;  ///< [now − 100 ms, now] 內有候選
    bool hit = false;  ///< 與目前 Δ 的角距離 < 1°（ECHO_MATCHED）
    uint32_t echo = 0;  ///< hit：c*；否則：時間最近的候選
    double delta_err_deg = -1.0;  ///< c* 的 Δ_c 與 Δ_cur 的角距離
    ph_pose_t client_pose;  ///< Δ_cur⁻¹ · mHmdPose
    bool delta_nonidentity = false;  ///< Δ_cur 不是恆等（descriptor 加 VRIPC_FRM_SPACE_DELTA）
    bool adopted = false;  ///< 這次呼叫採用了新的 Δ_cur
    std::string adopted_trigger;  ///< "still" 或 "event:<name>"
    ph_pose_t adopted_delta;
  };

  class pose_history_t {
  public:
    static constexpr uint32_t k_slots = 1024;
    static constexpr uint32_t k_adopt_window = 30;  ///< 新 Δ 候選緩衝
    static constexpr double k_hit_deg = 1.0;
    static constexpr double k_consistent_deg = 0.2;
    static constexpr double k_consistent_m = 0.005;
    static constexpr double k_still_deg_per_s = 5.0;
    static constexpr double k_event_window_s = 2.0;
    static constexpr double k_candidate_window_s = 0.1;

    explicit pose_history_t(int64_t qpf = 0) {
      qpf_ = qpf;
    }

    void set_qpf(int64_t qpf) {
      qpf_ = qpf;
    }

    /// 清空樣本、候選緩衝與事件，Δ_cur 回到恆等（新 generation／新 session）
    void reset();

    void record(const ph_sample_t &s);

    /// SeatedZeroPoseReset／StandingZeroPoseReset／ChaperoneUniverseHasChanged／SceneApplicationChanged
    void note_event(const char *name, int64_t qpc);

    /**
     * @param app_pose scene layer 的 mHmdPose（SteamVR 世界空間）
     * @param t_app_qpc SubmitLayer 當下 + flHmdPosePredictionTimeInSecondsFromNow（U34：now 的參考點待 PoC-10）
     * @param now_qpc 目前時間（候選視窗 = [now − 100 ms, now]）
     */
    ph_match_t match(const ph_pose_t &app_pose, int64_t t_app_qpc, int64_t now_qpc);

    ph_pose_t delta() const;

    // ── 純函式（unit 測試用）──
    static ph_pose_t extrapolate(const ph_sample_t &s, double dt_s);
    static ph_pose_t compose(const ph_pose_t &a, const ph_pose_t &b);  ///< a · b
    static ph_pose_t inverse(const ph_pose_t &a);
    static math::vec3_t rotate(const math::quat_t &q, const math::vec3_t &v);
    static double pos_dist(const math::vec3_t &a, const math::vec3_t &b);
    static void to_euler_deg(const math::quat_t &q, double &yaw, double &pitch, double &roll);

  private:
    struct pending_t {
      ph_pose_t delta;
      double ang_speed_deg = 0.0;
    };

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    int64_t qpf_ = 0;
    ph_sample_t ring_[k_slots] {};
    uint32_t head_ = 0;
    ph_pose_t delta_cur_ {};
    bool delta_identity_ = true;
    pending_t pending_[k_adopt_window] {};
    uint32_t pending_n_ = 0;  ///< 目前緩衝筆數（≤ k_adopt_window，環形覆寫最舊的）
    uint32_t pending_head_ = 0;
    int64_t last_event_qpc_ = 0;
    std::string last_event_name_;
  };

}  // namespace vrdrv
