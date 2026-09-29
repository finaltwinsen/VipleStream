// controller_device.cpp - 見 controller_device.h（設計 §E.2 controller_device、S2-09）。
#include "controller_device.h"

#include <cmath>
#include <cstdio>

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
    constexpr uint8_t k_in_active = 0x01;

    // raw_from_grip：Touch 的 raw 原點在 grip 前方 11 cm（旋轉 0）；以 DriverFromHead 交給 SteamVR 套用（速度的槓桿臂也由它處理）
    constexpr double k_raw_from_grip_z = -0.11;

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
      p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "viplestream");
      p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "VipleStream");
      p->SetStringProperty(c, vr::Prop_ModelNumber_String, right_ ? "VipleStream Touch (Right)" : "VipleStream Touch (Left)");
      p->SetStringProperty(c, vr::Prop_SerialNumber_String, serial());
      p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, right_ ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
      p->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
      p->SetStringProperty(c, vr::Prop_ControllerType_String, "oculus_touch");
      p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
      p->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, true);

      auto *in = vr::VRDriverInput();
      const char *lo = right_ ? "/input/a" : "/input/x";
      const char *hi = right_ ? "/input/b" : "/input/y";
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
      in->CreateBooleanComponent(c, "/input/joystick/click", &bools_[c_stick_click]);
      in->CreateBooleanComponent(c, "/input/joystick/touch", &bools_[c_stick_touch]);
      in->CreateBooleanComponent(c, "/input/trigger/click", &bools_[c_trigger_click]);
      in->CreateBooleanComponent(c, "/input/trigger/touch", &bools_[c_trigger_touch]);
      in->CreateBooleanComponent(c, "/input/grip/touch", &bools_[c_grip_touch]);
      in->CreateBooleanComponent(c, "/input/thumbrest/touch", &bools_[c_thumbrest_touch]);
      in->CreateScalarComponent(c, "/input/joystick/x", &scalars_[s_stick_x], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
      in->CreateScalarComponent(c, "/input/joystick/y", &scalars_[s_stick_y], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
      in->CreateScalarComponent(c, "/input/trigger/value", &scalars_[s_trigger_value], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
      in->CreateScalarComponent(c, "/input/grip/value", &scalars_[s_grip_value], vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
      in->CreateHapticComponent(c, "/output/haptic", &haptic_);

      AcquireSRWLockExclusive(&ctx_.pose_lock);
      index_.store(unObjectId);
      ReleaseSRWLockExclusive(&ctx_.pose_lock);
      VRDRV_LOG_INFO("controller activate hand=%s index=%u", right_ ? "right" : "left", unObjectId);
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
    report(p);
    release_inputs();
    ReleaseSRWLockShared(&ctx_.pose_lock);
  }

  void controller_device_t::update(const vripc_pose_t &hp, const vripc_ctrl_input_t &in, bool have, int64_t age_us, uint32_t oor_us, bool zero_vel) {
    AcquireSRWLockShared(&ctx_.pose_lock);
    if (index_.load() == k_invalid_index) {
      ReleaseSRWLockShared(&ctx_.pose_lock);
      return;
    }
    const bool active = have && (in.flags & k_in_active) != 0 && finite_pose(hp);
    const bool oor = !active || age_us > (int64_t) oor_us;
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
        p.vecVelocity[i] = (zero_vel || oor || !std::isfinite(hp.lin_vel[i])) ? 0.0 : hp.lin_vel[i];
        p.vecAngularVelocity[i] = (zero_vel || oor || !std::isfinite(hp.ang_vel[i])) ? 0.0 : hp.ang_vel[i];
      }
      p.poseIsValid = true;
    } else {
      for (int i = 0; i < 3; ++i) {
        p.vecVelocity[i] = 0.0;
        p.vecAngularVelocity[i] = 0.0;
      }
      p.poseIsValid = false;
    }
    p.result = oor ? vr::TrackingResult_Running_OutOfRange : vr::TrackingResult_Running_OK;
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
      vin->UpdateBooleanComponent(bools_[c_system_click], (b & (k_btn_system | k_btn_menu)) != 0, 0.0);
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
