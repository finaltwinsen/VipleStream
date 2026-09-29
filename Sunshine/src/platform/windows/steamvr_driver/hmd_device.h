// hmd_device.h - VipleStream §VR：HMD（ITrackedDeviceServerDriver_005 + IVRDisplayComponent_003；設計 §E.2 hmd_device）。
//
// V4 是獨立撰寫（設計 §E.3 原定改寫自 ALVR HMD.cpp；本機沒有 ALVR 原始碼，改列偏差；屬性本來就是自己定的）。
// K20：Hz、每眼尺寸、LUID 在本 vrserver 生命週期內不變——建構時以當時 generation 的 config 為準，之後不熱改。
// 不寫任何使用者設定；不冒用其他廠牌名稱。
#pragma once

#include <cstdint>
#include <string>

#include <openvr_driver.h>

#include "driver_context.h"

namespace vrdrv {

  class direct_mode_t;

  class hmd_device_t: public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent {
  public:
    hmd_device_t(driver_ctx_t &ctx, direct_mode_t &dm, const vripc_session_config_t &cfg);

    static constexpr const char *k_serial = "VIPLE-HMD-0";

    const vripc_session_config_t &config() const {
      return cfg_;
    }

    // RunFrame：/proximity 跟最新樣本的 PRESENCE（無效時一律 false）
    void update_proximity(bool present);
    // RunFrame：DEV_SET_V2P
    void set_vsync_to_photons(uint32_t us);

    // ── ITrackedDeviceServerDriver_005 ──
    vr::EVRInitError Activate(uint32_t unObjectId) override;
    void Deactivate() override;
    void EnterStandby() override;
    void *GetComponent(const char *pchComponentNameAndVersion) override;
    void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override;
    vr::DriverPose_t GetPose() override;

    // ── IVRDisplayComponent_003 ──
    void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override;
    bool IsDisplayOnDesktop() override;
    bool IsDisplayRealDisplay() override;
    void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) override;
    void GetEyeOutputViewport(vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override;
    void GetProjectionRaw(vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom) override;
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eEye, float fU, float fV) override;
    bool ComputeInverseDistortion(vr::HmdVector2_t *pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV) override;

    // tracking 執行緒寫、GetPose 讀
    void store_pose(const vr::DriverPose_t &p);

  private:
    void set_properties(vr::PropertyContainerHandle_t c);
    std::string chaperone_json() const;

    driver_ctx_t &ctx_;
    direct_mode_t &dm_;
    const vripc_session_config_t cfg_;
    vr::PropertyContainerHandle_t container_ = vr::k_ulInvalidPropertyContainer;
    vr::VRInputComponentHandle_t proximity_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t system_click_ = vr::k_ulInvalidInputComponentHandle;
    bool last_proximity_ = false;
    bool proximity_sent_ = false;
    SRWLOCK pose_mtx_ = SRWLOCK_INIT;
    vr::DriverPose_t last_pose_ {};
  };

}  // namespace vrdrv
