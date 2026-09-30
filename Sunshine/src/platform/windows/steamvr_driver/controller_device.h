// controller_device.h - VipleStream §VR（M1b S2-09）：最小 Touch 控制器（設計 §E.2 controller_device）。
//
// 左右各一個 TrackedDeviceClass_Controller，ControllerType=oculus_touch、InputProfilePath 借用 SteamVR 內建 oculus driver 的
// touch_profile.json（不自帶 binding）。沒有 skeleton。
// 執行緒：update() 在 tracking 執行緒（持 ctx.pose_lock 共享呼叫 TrackedDevicePoseUpdated／UpdateXxxComponent），
// Deactivate 以獨佔取得後把 index 設成無效；haptic 事件在 RunFrame 由 driver_main 轉給 on_haptic()。
#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

#include <openvr_driver.h>

#include "driver_context.h"

namespace vrdrv {

  class controller_device_t: public vr::ITrackedDeviceServerDriver {
  public:
    /// @param right false = 左手（device 1）、true = 右手（device 2）
    controller_device_t(driver_ctx_t &ctx, bool right);

    const char *serial() const {
      return right_ ? "VIPLE-CTRL-R" : "VIPLE-CTRL-L";
    }

    /// tracking 執行緒：slot 的 pose[1|2] 與 input[0|1]；have = slot 帶 LEFT/RIGHT 旗標；age_us = 距上次新樣本
    void update(const vripc_pose_t &pose, const vripc_ctrl_input_t &in, bool have, int64_t age_us, uint32_t oor_us, bool zero_vel);
    /// 連線中斷／standby：poseIsValid=false、放開所有輸入
    void invalidate();

    /// RunFrame：VREvent_Input_HapticVibration 的 container 是自己時回 true 並轉給 server
    bool on_haptic(const vr::VREvent_HapticVibration_t &h);

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate(uint32_t unObjectId) override;
    void Deactivate() override;
    void EnterStandby() override;
    void *GetComponent(const char *pchComponentNameAndVersion) override;
    void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override;
    vr::DriverPose_t GetPose() override;

  private:
    enum comp_e : uint32_t {
      c_system_click,
      c_ab_lo_click,  ///< A（右）／X（左）
      c_ab_lo_touch,
      c_ab_hi_click,  ///< B（右）／Y（左）
      c_ab_hi_touch,
      c_stick_click,
      c_stick_touch,
      c_trigger_click,
      c_trigger_touch,
      c_grip_touch,
      c_thumbrest_touch,
      c_bool_count,
    };

    enum scalar_e : uint32_t {
      s_stick_x,
      s_stick_y,
      s_trigger_value,
      s_grip_value,
      s_scalar_count,
    };

    void release_inputs();
    void report(const vr::DriverPose_t &p);
    /// M4a 收尾：依 client 的 interaction profile 設 Prop_RenderModelName_String（profile 變了才設）
    void apply_render_model(uint8_t profile);

    driver_ctx_t &ctx_;
    const bool right_;
    std::atomic<uint32_t> index_ {k_invalid_index};
    vr::PropertyContainerHandle_t container_ = vr::k_ulInvalidPropertyContainer;
    vr::VRInputComponentHandle_t bools_[c_bool_count] {};
    vr::VRInputComponentHandle_t scalars_[s_scalar_count] {};
    vr::VRInputComponentHandle_t haptic_ = vr::k_ulInvalidInputComponentHandle;
    bool inputs_released_ = true;
    bool oor_logged_ = false;
    const char *render_model_ = nullptr;  ///< 目前設定的 render model（字串常值）；nullptr＝還沒設
    int render_profile_ = -1;       ///< 目前設定的 render model 對應的 profile；-1＝還沒設
    bool reconnect_pending_ = false; ///< render model 換了：送一次 deviceIsConnected=false，讓 app 重新載入外觀
    SRWLOCK pose_mtx_ = SRWLOCK_INIT;
    vr::DriverPose_t last_pose_ {};
  };

}  // namespace vrdrv
