// controller_device.cpp - 見 controller_device.h（設計 §E.2 controller_device、S2-09）。
#include "controller_device.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver_log.h"
#include "seh_guard.h"

namespace vrdrv {

  namespace {
    // vripc_ctrl_input_t buttons／touches 位元（§E.2）
    constexpr uint32_t k_btn_system = 1u << 0;
    constexpr uint32_t k_btn_menu = 1u << 1;
    constexpr uint32_t k_btn_ab_lo = 1u << 2;  // A／X
    constexpr uint32_t k_btn_ab_hi = 1u << 3;  // B／Y
    constexpr uint32_t k_btn_stick = 1u << 4;
    constexpr uint32_t k_btn_trigger = 1u << 5;
    constexpr uint32_t k_btn_grip = 1u << 6;
    constexpr uint32_t k_btn_thumbrest = 1u << 8;
    constexpr uint32_t k_btn_c = 1u << 9;  // §VR-FRAME-CTRL：Frame 右 x、左 dpad_left
    constexpr uint32_t k_btn_d = 1u << 10;  // Frame 右 y、左 dpad_up
    constexpr uint32_t k_btn_bumper = 1u << 11;
    constexpr uint8_t k_in_active = 0x01;

    // raw_from_grip：Touch 的 raw 原點在 grip 前方 11 cm（旋轉 0）；以 DriverFromHead 交給 SteamVR 套用（速度的槓桿臂也由它處理）
    constexpr double k_raw_from_grip_z = -0.11;

    // §VR-FRAME-CTRL：Frame 手把 render model 的 openxr_grip 元件（raw 座標）＝原點 (±0.003117, -0.004277, 0.099501)、
    // 繞 X 2.8091°（左手 x 為負，2026-10-30 從 host 的 frame_controller_left/right.json 讀出）。client 送的是 OpenXR grip，
    // 反推 raw：DriverFromHead＝grip 座標下的 raw＝(-Rᵀt, Rᵀ)
    constexpr double k_frame_raw_from_grip_x = 0.003117;  // 左手；右手取負
    constexpr double k_frame_raw_from_grip_y = -0.000604;
    constexpr double k_frame_raw_from_grip_z = -0.099591;
    constexpr double k_frame_raw_from_grip_qx = -0.024512;  // 繞 X -2.8091°
    constexpr double k_frame_raw_from_grip_qw = 0.999700;

    float unorm16(uint16_t v) {
      return (float) v / 65535.0f;
    }

    float snorm16(int16_t v) {
      const float f = (float) v / 32767.0f;
      return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
    }

    bool finite_pose(const vripc_pose_t &p) {
      for (float v : p.pos) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      for (float v : p.rot) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      return true;
    }
  }  // namespace

  controller_device_t::controller_device_t(driver_ctx_t &ctx, bool right):
      ctx_(ctx),
      right_(right) {
    for (auto &b : bools_) {
      b = vr::k_ulInvalidInputComponentHandle;
    }
    for (auto &s : scalars_) {
      s = vr::k_ulInvalidInputComponentHandle;
    }
    last_pose_.qWorldFromDriverRotation.w = 1.0;
    last_pose_.qDriverFromHeadRotation.w = 1.0;
    last_pose_.vecDriverFromHeadTranslation[2] = k_raw_from_grip_z;
    last_pose_.qRotation.w = 1.0;
    last_pose_.result = vr::TrackingResult_Running_OutOfRange;
    last_pose_.poseIsValid = false;
    last_pose_.deviceIsConnected = true;
  }

  vr::EVRInitError controller_device_t::Activate(uint32_t unObjectId) {
    vr::EVRInitError rc = vr::VRInitError_None;
    guarded("ctrl.Activate", [&]() {
      auto *p = vr::VRProperties();
      container_ = p->TrackedDeviceToPropertyContainer(unObjectId);
      const auto c = container_;
      // §VR-FRAME-CTRL：有 frame_controller 的 input profile 就以 Frame 手把出現（ModelNumber 與 vrlink 相同）
      {
        char full[MAX_PATH] = {};
        auto *res = vr::VRResources();
        frame_ = res != nullptr && res->GetResourceFullPath("{frame_controller}/input/frame_controller_profile.json", "", full, sizeof(full)) != 0 && full[0] != 0;
      }
      p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "viplestream");
      p->SetStringProperty(c, vr::Prop_SerialNumber_String, serial());
      p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, right_ ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
      p->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
      if (frame_) {
        p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Valve");
        p->SetStringProperty(c, vr::Prop_ModelNumber_String, right_ ? "Roy_Right" : "Roy_Left");
        p->SetStringProperty(c, vr::Prop_ControllerType_String, "frame_controller");
        p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{frame_controller}/input/frame_controller_profile.json");
        AcquireSRWLockExclusive(&pose_mtx_);
        last_pose_.vecDriverFromHeadTranslation[0] = right_ ? -k_frame_raw_from_grip_x : k_frame_raw_from_grip_x;
        last_pose_.vecDriverFromHeadTranslation[1] = k_frame_raw_from_grip_y;
        last_pose_.vecDriverFromHeadTranslation[2] = k_frame_raw_from_grip_z;
        last_pose_.qDriverFromHeadRotation.x = k_frame_raw_from_grip_qx;
        last_pose_.qDriverFromHeadRotation.w = k_frame_raw_from_grip_qw;
        ReleaseSRWLockExclusive(&pose_mtx_);
      } else {
        p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "VipleStream");
        p->SetStringProperty(c, vr::Prop_ModelNumber_String, right_ ? "VipleStream Touch (Right)" : "VipleStream Touch (Left)");
        p->SetStringProperty(c, vr::Prop_ControllerType_String, "oculus_touch");
        p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
      }
      p->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, true);
      // M4a 收尾（Frame 實測：沒設 render model，SteamVR Home 顯示成一般手把）。先用 Touch 的外觀，
      // 收到 client 的 interaction profile 後在 update() 換成對應的（apply_render_model）
      apply_render_model(frame_ ? VRIPC_CTRL_PROFILE_FRAME : VRIPC_CTRL_PROFILE_UNKNOWN);

      auto *in = vr::VRDriverInput();
      const char *lo = frame_ ? (right_ ? "/input/a" : "/input/dpad_down") : (right_ ? "/input/a" : "/input/x");
      const char *hi = frame_ ? (right_ ? "/input/b" : "/input/dpad_right") : (right_ ? "/input/b" : "/input/y");
      const char *stick = frame_ ? "/input/thumbstick" : "/input/joystick";
      char path[64];
      in->CreateBooleanComponent(c, "/input/system/click", &bools_[c_system_click]);
      std::snprintf(path, sizeof(path), "%s/click", lo);
      in->CreateBooleanComponent(c, path, &bools_[c_ab_lo_click]);
      std::snprintf(path, sizeof(path), "%s/touch", lo);
      in->CreateBooleanComponent(c, path, &bools_[c_ab_lo_touch]);
      std::snprintf(path, sizeof(path), "%s/click", hi);
      in->CreateBooleanComponent(c, path, &bools_[c_ab_hi_click]);
      std::snprintf(path, sizeof(path), "%s/touch", hi);
      in->CreateBooleanComponent(c, path, &bools_[c_ab_hi_touch]);
      std::snprintf(path, sizeof(path), "%s/click", stick);
      in->CreateBooleanComponent(c, path, &bools_[c_stick_click]);
      std::snprintf(path, sizeof(path), "%s/touch", stick);
      in->CreateBooleanComponent(c, path, &bools_[c_stick_touch]);
      in->CreateBooleanComponent(c, "/input/trigger/click", &bools_[c_trigger_click]);
      in->CreateBooleanComponent(c, "/input/trigger/touch", &bools_[c_trigger_touch]);
      in->CreateBooleanComponent(c, "/input/grip/touch", &bools_[c_grip_touch]);
      in->CreateBooleanComponent(c, "/input/thumbrest/touch", &bools_[c_thumbrest_touch]);
      std::snprintf(path, sizeof(path), "%s/x", stick);
      in->CreateScalarComponent(c, path, &scalars_[s_stick_x], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
      std::snprintf(path, sizeof(path), "%s/y", stick);
      in->CreateScalarComponent(c, path, &scalars_[s_stick_y], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
      in->CreateScalarComponent(c, "/input/trigger/value", &scalars_[s_trigger_value], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
      in->CreateScalarComponent(c, "/input/grip/value", &scalars_[s_grip_value], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
      in->CreateHapticComponent(c, "/output/haptic", &haptic_);
      if (frame_) {
        const char *menu = right_ ? "/input/menu" : "/input/view";
        const char *cc = right_ ? "/input/x" : "/input/dpad_left";
        const char *dd = right_ ? "/input/y" : "/input/dpad_up";
        const struct {
          const char *base;
          comp_e click, touch;
        } extra[] = {{menu, c_menu_click, c_menu_touch}, {cc, c_c_click, c_c_touch}, {dd, c_d_click, c_d_touch}, {"/input/bumper", c_bumper_click, c_bumper_touch}};
        for (const auto &e : extra) {
          std::snprintf(path, sizeof(path), "%s/click", e.base);
          in->CreateBooleanComponent(c, path, &bools_[e.click]);
          std::snprintf(path, sizeof(path), "%s/touch", e.base);
          in->CreateBooleanComponent(c, path, &bools_[e.touch]);
        }
        in->CreateBooleanComponent(c, "/input/grip/click", &bools_[c_grip_click]);
      }

      AcquireSRWLockExclusive(&ctx_.pose_lock);
      index_.store(unObjectId);
      ReleaseSRWLockExclusive(&ctx_.pose_lock);
      VRDRV_LOG_INFO("[VIPLE-VR-FRAME-CTRL] controller activate hand=%s index=%u type=%s", right_ ? "right" : "left", unObjectId,
                     frame_ ? "frame_controller" : "oculus_touch");
    });
    if (degraded()) {
      rc = vr::VRInitError_Driver_Failed;
    }
    return rc;
  }

  void controller_device_t::Deactivate() {
    AcquireSRWLockExclusive(&ctx_.pose_lock);
    index_.store(k_invalid_index);
    ReleaseSRWLockExclusive(&ctx_.pose_lock);
    VRDRV_LOG_INFO("controller deactivate hand=%s", right_ ? "right" : "left");
  }

  void controller_device_t::EnterStandby() {
  }

  void *controller_device_t::GetComponent(const char *) {
    return nullptr;
  }

  void controller_device_t::DebugRequest(const char *, char *pchResponseBuffer, uint32_t unResponseBufferSize) {
    if (unResponseBufferSize > 0) {
      pchResponseBuffer[0] = 0;
    }
  }

  vr::DriverPose_t controller_device_t::GetPose() {
    AcquireSRWLockShared(&pose_mtx_);
    const vr::DriverPose_t p = last_pose_;
    ReleaseSRWLockShared(&pose_mtx_);
    return p;
  }

  void controller_device_t::apply_render_model(uint8_t profile) {
    if (frame_) {
      profile = VRIPC_CTRL_PROFILE_FRAME;  // §VR-FRAME-CTRL：裝置類型已是 Frame，外觀跟著固定
    }
    if (container_ == vr::k_ulInvalidPropertyContainer || (int) profile == render_profile_) {
      return;
    }
    // SteamVR 內建模型：Touch（Quest 2）在全域 resources/rendermodels；Index、Frame 在各自的 resource-only
    // driver 目錄，名稱要帶 {driver} 前綴（與 vrserver 自己用的寫法相同）。該 driver 目錄不存在（舊版 SteamVR
    // 沒有 frame_controller）時退回 Touch。
    const char *touch = right_ ? "oculus_quest2_controller_right" : "oculus_quest2_controller_left";
    const char *model = touch;
    const char *probe = nullptr;
    if (profile == VRIPC_CTRL_PROFILE_FRAME) {
      model = right_ ? "{frame_controller}frame_controller_right" : "{frame_controller}frame_controller_left";
      probe = right_ ? "{frame_controller}/rendermodels/frame_controller_right/frame_controller_right.json" :
                       "{frame_controller}/rendermodels/frame_controller_left/frame_controller_left.json";
    } else if (profile == VRIPC_CTRL_PROFILE_INDEX) {
      model = right_ ? "{indexcontroller}valve_controller_knu_1_0_right" : "{indexcontroller}valve_controller_knu_1_0_left";
      probe = right_ ? "{indexcontroller}/rendermodels/valve_controller_knu_1_0_right/valve_controller_knu_1_0_right.json" :
                       "{indexcontroller}/rendermodels/valve_controller_knu_1_0_left/valve_controller_knu_1_0_left.json";
    }
    if (probe != nullptr) {
      char full[MAX_PATH] = {};
      auto *res = vr::VRResources();
      if (res == nullptr || res->GetResourceFullPath(probe, "", full, sizeof(full)) == 0 || full[0] == 0) {
        VRDRV_LOG_WARN("controller hand=%s render model %s not installed - using Touch", right_ ? "right" : "left", model);
        model = touch;
      }
    }
    render_profile_ = profile;
    if (render_model_ != nullptr && std::strcmp(render_model_, model) == 0) {
      return;  // 外觀沒變（例：預設 Touch → client 回報 Touch）：不設、不觸發重新連線
    }
    vr::VRProperties()->SetStringProperty(container_, vr::Prop_RenderModelName_String, model);
    if (render_model_ != nullptr) {
      reconnect_pending_ = true;  // 已顯示過舊外觀：讓 app 重新載入
    }
    render_model_ = model;  // 指向字串常值
    VRDRV_LOG_INFO("controller hand=%s render model=%s (client profile %u)", right_ ? "right" : "left", model, (unsigned) profile);
  }

  void controller_device_t::report(const vr::DriverPose_t &p) {
    AcquireSRWLockExclusive(&pose_mtx_);
    last_pose_ = p;
    ReleaseSRWLockExclusive(&pose_mtx_);
    // 呼叫端（tracking 執行緒）已持 ctx.pose_lock 共享
    const uint32_t idx = index_.load();
    if (idx != k_invalid_index) {
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(idx, p, sizeof(vr::DriverPose_t));
    }
  }

  void controller_device_t::release_inputs() {
    if (inputs_released_ || index_.load() == k_invalid_index) {
      return;
    }
    auto *in = vr::VRDriverInput();
    for (auto b : bools_) {
      if (b != vr::k_ulInvalidInputComponentHandle) {
        in->UpdateBooleanComponent(b, false, 0.0);
      }
    }
    for (auto s : scalars_) {
      if (s != vr::k_ulInvalidInputComponentHandle) {
        in->UpdateScalarComponent(s, 0.0f, 0.0);
      }
    }
    inputs_released_ = true;
  }

  void controller_device_t::invalidate() {
    AcquireSRWLockShared(&ctx_.pose_lock);
    vr::DriverPose_t p = GetPose();
    p.result = vr::TrackingResult_Running_OutOfRange;
    p.poseIsValid = false;
    for (int i = 0; i < 3; ++i) {
      p.vecVelocity[i] = 0.0;
      p.vecAngularVelocity[i] = 0.0;
    }
    p.poseTimeOffset = 0.0;
    report(p);
    release_inputs();
    ReleaseSRWLockShared(&ctx_.pose_lock);
  }

  void controller_device_t::update(const vripc_pose_t &hp, const vripc_ctrl_input_t &in, bool have, int64_t age_us, uint32_t oor_us, bool zero_vel,
                                   double pose_time_offset, double ang_scale, bool angvel_local, bool keep_offset) {
    AcquireSRWLockShared(&ctx_.pose_lock);
    if (index_.load() == k_invalid_index) {
      ReleaseSRWLockShared(&ctx_.pose_lock);
      return;
    }
    const bool active = have && (in.flags & k_in_active) != 0 && finite_pose(hp);
    const bool oor = !active || age_us > (int64_t) oor_us;
    if (have && in.profile != VRIPC_CTRL_PROFILE_UNKNOWN) {
      apply_render_model(in.profile);  // M4a 收尾：profile 沒變時立即返回
    }
    vr::DriverPose_t p = GetPose();
    if (active) {
      p.vecPosition[0] = hp.pos[0];
      p.vecPosition[1] = hp.pos[1];
      p.vecPosition[2] = hp.pos[2];
      const math::quat_t q = math::normalize(math::quat_t {hp.rot[0], hp.rot[1], hp.rot[2], hp.rot[3]});
      p.qRotation.x = q.x;
      p.qRotation.y = q.y;
      p.qRotation.z = q.z;
      p.qRotation.w = q.w;
      for (int i = 0; i < 3; ++i) {
        p.vecVelocity[i] = ((zero_vel && !keep_offset) || oor || !std::isfinite(hp.lin_vel[i])) ? 0.0 : hp.lin_vel[i];
        p.vecAngularVelocity[i] = ((zero_vel && !keep_offset) || oor || !std::isfinite(hp.ang_vel[i])) ? 0.0 : hp.ang_vel[i] * ang_scale;
      }
      if (angvel_local) {
        // §VR-ANGVEL-LOCAL：SteamVR 把角速度當機體座標；client（OpenXR grip space 對追蹤空間）給的是世界座標。
        // 舊版拍子只要不是直立就繞錯的軸外插，揮拍時拍面角度偏掉
        const math::vec3_t wl = math::rotate(math::conj(q), math::vec3_t {p.vecAngularVelocity[0], p.vecAngularVelocity[1], p.vecAngularVelocity[2]});
        p.vecAngularVelocity[0] = wl.x;
        p.vecAngularVelocity[1] = wl.y;
        p.vecAngularVelocity[2] = wl.z;
      }
      // §VR-CTRL-OFFSET：速度歸零（stale、OutOfRange）時 offset 也歸零，與 HMD 的舊規則一致
      p.poseTimeOffset = ((zero_vel && !keep_offset) || oor || !std::isfinite(pose_time_offset)) ? 0.0 : pose_time_offset;
      p.poseIsValid = true;
    } else {
      for (int i = 0; i < 3; ++i) {
        p.vecVelocity[i] = 0.0;
        p.vecAngularVelocity[i] = 0.0;
      }
      p.poseTimeOffset = 0.0;
      p.poseIsValid = false;
    }
    p.result = oor ? vr::TrackingResult_Running_OutOfRange : vr::TrackingResult_Running_OK;
    if (reconnect_pending_) {
      // render model 換了：送一筆 deviceIsConnected=false，下一筆恢復 true → app 收到斷線／連線事件後重新載入外觀
      reconnect_pending_ = false;
      vr::DriverPose_t off = p;
      off.deviceIsConnected = false;
      off.poseIsValid = false;
      report(off);
      p.deviceIsConnected = true;
    }
    report(p);
    if (oor != oor_logged_) {
      oor_logged_ = oor;
      VRDRV_LOG_INFO("controller hand=%s %s age_ms=%lld", right_ ? "right" : "left", oor ? "out-of-range" : "tracking", (long long) (age_us / 1000));
    }

    if (!active) {
      // flags.active==0：放開所有輸入（避免按住的鍵黏住）
      release_inputs();
    } else {
      auto *vin = vr::VRDriverInput();
      const uint32_t b = in.buttons, t = in.touches;
      // Touch 模式沒有獨立的 menu 元件：menu 併進 system（舊行為）
      vin->UpdateBooleanComponent(bools_[c_system_click], (b & (frame_ ? k_btn_system : (k_btn_system | k_btn_menu))) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_ab_lo_click], (b & k_btn_ab_lo) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_ab_lo_touch], (t & k_btn_ab_lo) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_ab_hi_click], (b & k_btn_ab_hi) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_ab_hi_touch], (t & k_btn_ab_hi) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_stick_click], (b & k_btn_stick) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_stick_touch], (t & k_btn_stick) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_trigger_click], (b & k_btn_trigger) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_trigger_touch], (t & k_btn_trigger) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_grip_touch], (t & k_btn_grip) != 0 || (b & k_btn_grip) != 0, 0.0);
      vin->UpdateBooleanComponent(bools_[c_thumbrest_touch], (t & k_btn_thumbrest) != 0, 0.0);
      vin->UpdateScalarComponent(scalars_[s_stick_x], snorm16(in.stick_x), 0.0);
      vin->UpdateScalarComponent(scalars_[s_stick_y], snorm16(in.stick_y), 0.0);
      vin->UpdateScalarComponent(scalars_[s_trigger_value], unorm16(in.trigger), 0.0);
      vin->UpdateScalarComponent(scalars_[s_grip_value], unorm16(in.grip), 0.0);
      if (frame_) {
        vin->UpdateBooleanComponent(bools_[c_menu_click], (b & k_btn_menu) != 0, 0.0);
        vin->UpdateBooleanComponent(bools_[c_menu_touch], (b & k_btn_menu) != 0, 0.0);
        vin->UpdateBooleanComponent(bools_[c_c_click], (b & k_btn_c) != 0, 0.0);
        vin->UpdateBooleanComponent(bools_[c_c_touch], (t & k_btn_c) != 0, 0.0);
        vin->UpdateBooleanComponent(bools_[c_d_click], (b & k_btn_d) != 0, 0.0);
        vin->UpdateBooleanComponent(bools_[c_d_touch], (t & k_btn_d) != 0, 0.0);
        vin->UpdateBooleanComponent(bools_[c_bumper_click], (b & k_btn_bumper) != 0, 0.0);
        vin->UpdateBooleanComponent(bools_[c_bumper_touch], (t & k_btn_bumper) != 0, 0.0);
        vin->UpdateBooleanComponent(bools_[c_grip_click], (b & k_btn_grip) != 0, 0.0);
      }
      inputs_released_ = false;
    }
    ReleaseSRWLockShared(&ctx_.pose_lock);
  }

  bool controller_device_t::on_haptic(const vr::VREvent_HapticVibration_t &h) {
    if (h.containerHandle != container_ || container_ == vr::k_ulInvalidPropertyContainer) {
      return false;
    }
    vripc_haptic_evt_t e {};
    e.device = right_ ? 2u : 1u;
    const double dur = std::isfinite(h.fDurationSeconds) && h.fDurationSeconds > 0.0f ? (double) h.fDurationSeconds : 0.0;
    e.duration_us = (uint32_t) (dur > 10.0 ? 10000000.0 : dur * 1e6);
    e.frequency_hz = std::isfinite(h.fFrequency) ? h.fFrequency : 0.0f;
    e.amplitude = std::isfinite(h.fAmplitude) ? (h.fAmplitude < 0.0f ? 0.0f : (h.fAmplitude > 1.0f ? 1.0f : h.fAmplitude)) : 0.0f;
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    e.qpc = v.QuadPart;
    auto gen = ctx_.ipc ? ctx_.ipc->current() : generation_ptr {};
    if (gen && !gen->idle()) {
      gen->push_haptic(e);
    }
    return true;
  }

}  // namespace vrdrv
