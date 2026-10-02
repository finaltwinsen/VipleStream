// hmd_device.cpp - 見 hmd_device.h（設計 §E.2 hmd_device）。
#include "hmd_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "direct_mode.h"
#include "driver_log.h"
#include "seh_guard.h"

namespace vrdrv {

  namespace {
    std::string utf16_to_utf8(const uint16_t *s, size_t max) {
      size_t n = 0;
      while (n < max && s[n] != 0) {
        ++n;
      }
      if (n == 0 || n == max) {
        return {};
      }
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *) s, (int) n, nullptr, 0, nullptr, nullptr);
      if (bytes <= 0) {
        return {};
      }
      std::string out((size_t) bytes, '\0');
      WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *) s, (int) n, out.data(), bytes, nullptr, nullptr);
      return out;
    }

    void eye_to_head_matrix(const float e[7], vr::HmdMatrix34_t &m) {
      const math::quat_t q {e[3], e[4], e[5], e[6]};
      const math::vec3_t p {e[0], e[1], e[2]};
      math::pose_to_matrix34(q, p, m.m);
    }
  }  // namespace

  hmd_device_t::hmd_device_t(driver_ctx_t &ctx, direct_mode_t &dm, const vripc_session_config_t &cfg):
      ctx_(ctx),
      dm_(dm),
      cfg_(cfg) {
    last_pose_.qWorldFromDriverRotation.w = 1.0;
    last_pose_.qDriverFromHeadRotation.w = 1.0;
    last_pose_.qRotation.w = 1.0;
    last_pose_.result = vr::TrackingResult_Running_OutOfRange;
    last_pose_.poseIsValid = false;
    last_pose_.deviceIsConnected = true;
  }

  std::string hmd_device_t::chaperone_json() const {
    // 格式照 SteamVR 自己寫的 chaperone_info.vrchap：最外層 "jsonid"（不是 "json_id"）、"version"，
    // 每個 universe 帶 "time"（asctime 樣式）、"universeID"（字串）、"play_area"、"collision_bounds"、
    // "standing"、"seated"。standing == raw（space-delta 起始為 I）。
    //
    // §CHAP-JSONID（2026-10-02，Frame 實測）：原本鍵名寫成 "json_id"，vrserver 記
    // 「Failed to parse chaperone file because jsonid was missing」整份拒收 → 這個 universe 沒有校正資料。
    // OpenVR app（SteamVR Home）不受影響，但 SteamVR 的 OpenXR runtime 拿不到 standing／seated 零點，
    // OpenXR 遊戲（Unity OpenXR 等）的參考空間整個繞視線軸轉了 180°：世界上下顛倒、文字倒著。
    static const char *const k_wday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const k_mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    SYSTEMTIME st;
    GetSystemTime(&st);
    char t[40];
    std::snprintf(t, sizeof(t), "%s %s %02u %02u:%02u:%02u %04u", k_wday[st.wDayOfWeek % 7], k_mon[(st.wMonth + 11) % 12], st.wDay, st.wHour, st.wMinute,
                  st.wSecond, st.wYear);
    const unsigned universe = (unsigned) (cfg_.universe_id & 0xFFFFFFFFu);
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\"jsonid\":\"chaperone_info\",\"universes\":[{"
                  "\"collision_bounds\":[[[-5,0,-5],[-5,2.5,-5],[-5,2.5,5],[-5,0,5]],[[-5,0,5],[-5,2.5,5],[5,2.5,5],[5,0,5]],"
                  "[[5,0,5],[5,2.5,5],[5,2.5,-5],[5,0,-5]],[[5,0,-5],[5,2.5,-5],[-5,2.5,-5],[-5,0,-5]]],"
                  "\"play_area\":[10.0,10.0],"
                  "\"seated\":{\"translation\":[0,1.2,0],\"yaw\":0},\"standing\":{\"translation\":[0,0,0],\"yaw\":0},"
                  "\"time\":\"%s\",\"universeID\":\"%u\"}],\"version\":5}",
                  t, universe);
    return buf;
  }

  void hmd_device_t::set_properties(vr::PropertyContainerHandle_t c) {
    auto *p = vr::VRProperties();
    const float hz = (float) cfg_.refresh_mhz / 1000.0f;
    p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "viplestream");
    p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "VipleStream");
    p->SetStringProperty(c, vr::Prop_ModelNumber_String, "VipleStream PCVR");
    p->SetStringProperty(c, vr::Prop_SerialNumber_String, k_serial);
    p->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, hz);
    // M1b 預設 = 1/Hz（config.vsync_to_photons_us）；PoC-10 E7 定策略
    p->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float, (float) cfg_.vsync_to_photons_us / 1e6f);
    p->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, cfg_.ipd_m);
    p->SetUint64Property(c, vr::Prop_CurrentUniverseId_Uint64, cfg_.universe_id);  // K24：0x5649504C
    p->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, false);
    p->SetBoolProperty(c, vr::Prop_DisplayDebugMode_Bool, false);
    const uint64_t luid = ((uint64_t) (uint32_t) cfg_.adapter_luid_high << 32) | cfg_.adapter_luid_low;
    p->SetUint64Property(c, vr::Prop_GraphicsAdapterLuid_Uint64, luid);  // DirectMode 下可能無效；不依賴它（K22）
    p->SetBoolProperty(c, vr::Prop_ContainsProximitySensor_Bool, true);
    p->SetBoolProperty(c, vr::Prop_DriverProvidedChaperoneVisibility_Bool, false);
    const std::string chap = chaperone_json();
    p->SetStringProperty(c, vr::Prop_DriverProvidedChaperoneJson_String, chap.c_str());
    const std::string audio = utf16_to_utf8(cfg_.audio_endpoint_id, sizeof(cfg_.audio_endpoint_id) / sizeof(cfg_.audio_endpoint_id[0]));
    if (!audio.empty()) {
      p->SetStringProperty(c, vr::Prop_Audio_DefaultPlaybackDeviceId_String, audio.c_str());  // 值不進 log
    }
    // nAdditionalFramesToPredict 還沒實作；PoC-10 E6 驗證後再開（drv-m-5）
    p->SetBoolProperty(c, vr::Prop_Hmd_SupportsAppThrottling_Bool, false);
    const uint32_t dev = cfg_.dev_flags;  // bridge 在沒有 dev mode 時已清成 0
    p->SetBoolProperty(c, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, (dev & VRIPC_DEV_SENDS_VSYNC_EVENTS) != 0);
    // K23：2096／2116 預設 true
    p->SetBoolProperty(c, vr::Prop_ForceSystemLayerUseAppPoses_Bool, (dev & VRIPC_DEV_SYS_LAYER_OWN_POSES) == 0);
    p->SetBoolProperty(c, vr::Prop_Hmd_AllowsClientToControlTextureIndex, (dev & VRIPC_DEV_DRIVER_CONTROLS_TEX_INDEX) == 0);
    p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{viplestream}/input/viplestream_hmd_profile.json");
    p->SetStringProperty(c, vr::Prop_ControllerType_String, "viplestream_hmd");
    // 不宣告 DisplaySupportsRuntimeFramerateChange（避免使用者的 preferredRefreshRate 介入）
  }

  vr::EVRInitError hmd_device_t::Activate(uint32_t unObjectId) {
    if (degraded()) {
      return vr::VRInitError_Driver_Failed;
    }
    vr::EVRInitError rc = vr::VRInitError_Driver_Failed;
    guarded("hmd.Activate", [&]() {
      container_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);
      set_properties(container_);
      auto *in = vr::VRDriverInput();
      if (in) {
        in->CreateBooleanComponent(container_, "/input/system/click", &system_click_);
        in->CreateBooleanComponent(container_, "/proximity", &proximity_);
      }
      vr::HmdMatrix34_t l {}, r {};
      eye_to_head_matrix(cfg_.eye_to_head[0], l);
      eye_to_head_matrix(cfg_.eye_to_head[1], r);
      const math::rect_t rl = math::fov_to_rect(cfg_.fov_tan[0]);
      const math::rect_t rr = math::fov_to_rect(cfg_.fov_tan[1]);
      vr::HmdRect2_t pl {}, pr {};
      pl.vTopLeft = {rl.left, rl.top};
      pl.vBottomRight = {rl.right, rl.bottom};
      pr.vTopLeft = {rr.left, rr.top};
      pr.vBottomRight = {rr.right, rr.bottom};
      auto *host = vr::VRServerDriverHost();
      host->SetDisplayEyeToHead(unObjectId, l, r);
      host->SetDisplayProjectionRaw(unObjectId, pl, pr);

      LUID luid {};
      luid.LowPart = cfg_.adapter_luid_low;
      luid.HighPart = cfg_.adapter_luid_high;
      dm_.activate(cfg_, cfg_.eye_width, cfg_.eye_height, luid);

      activated_params_t a;
      a.refresh_mhz = cfg_.refresh_mhz;
      a.eye_width = cfg_.eye_width;
      a.eye_height = cfg_.eye_height;
      a.luid_low = cfg_.adapter_luid_low;
      a.luid_high = cfg_.adapter_luid_high;
      ctx_.ipc->set_activated(a);
      ctx_.hmd_index.store(unObjectId);
      ctx_.ipc->send_state(VRIPC_ST_HMD_ACTIVATED, unObjectId);
      VRDRV_LOG_INFO("hmd activate index=%u hz=%u.%03u eye=%ux%u universe=%u v2pUs=%u", unObjectId, cfg_.refresh_mhz / 1000, cfg_.refresh_mhz % 1000, cfg_.eye_width,
                     cfg_.eye_height, (unsigned) (cfg_.universe_id & 0xFFFFFFFFu), cfg_.vsync_to_photons_us);
      rc = vr::VRInitError_None;
    });
    return rc;
  }

  void hmd_device_t::Deactivate() {
    // Deactivate 之後不再以舊 index 呼叫 vrserver：pose_lock 獨佔等 tracking 執行緒離開 TrackedDevicePoseUpdated
    AcquireSRWLockExclusive(&ctx_.pose_lock);
    ctx_.hmd_index.store(k_invalid_index);
    ReleaseSRWLockExclusive(&ctx_.pose_lock);
    VRDRV_LOG_INFO("hmd deactivate");
  }

  void hmd_device_t::EnterStandby() {
    VRDRV_LOG_INFO("hmd enter-standby (SteamVR)");
  }

  void *hmd_device_t::GetComponent(const char *name) {
    if (degraded() || !name) {
      return nullptr;
    }
    void *out = nullptr;
    guarded("hmd.GetComponent", [&]() {
      if (std::strcmp(name, vr::IVRDisplayComponent_Version) == 0) {
        out = static_cast<vr::IVRDisplayComponent *>(this);
      } else if (std::strcmp(name, vr::IVRDriverDirectModeComponent_Version) == 0) {
        out = static_cast<vr::IVRDriverDirectModeComponent *>(&dm_);
      }
      char safe[64];
      size_t i = 0;
      for (; name[i] && i < sizeof(safe) - 1; ++i) {
        const unsigned char ch = (unsigned char) name[i];
        safe[i] = (ch < 0x20 || ch >= 0x7F || ch == '[') ? '?' : (char) ch;
      }
      safe[i] = '\0';
      VRDRV_LOG_INFO("getcomponent %s -> %s", safe, out ? "hit" : "miss");
      // 要求了別的版本：STATE IFACE_UNSUPPORTED（arg = 版本號），比事後讀 vrserver.txt 可靠（drv-m-16）
      if (!out) {
        const char *prefixes[] = {"IVRDriverDirectModeComponent_", "IVRDisplayComponent_"};
        for (const char *pre : prefixes) {
          const size_t n = std::strlen(pre);
          if (std::strncmp(name, pre, n) == 0) {
            ctx_.ipc->send_state(VRIPC_ST_IFACE_UNSUPPORTED, (uint32_t) std::strtoul(name + n, nullptr, 10));
          }
        }
      }
    });
    return out;
  }

  void hmd_device_t::DebugRequest(const char *, char *pchResponseBuffer, uint32_t unResponseBufferSize) {
    if (pchResponseBuffer && unResponseBufferSize > 0) {
      pchResponseBuffer[0] = '\0';
    }
  }

  vr::DriverPose_t hmd_device_t::GetPose() {
    AcquireSRWLockShared(&pose_mtx_);
    vr::DriverPose_t p = last_pose_;
    ReleaseSRWLockShared(&pose_mtx_);
    return p;
  }

  void hmd_device_t::store_pose(const vr::DriverPose_t &p) {
    AcquireSRWLockExclusive(&pose_mtx_);
    last_pose_ = p;
    ReleaseSRWLockExclusive(&pose_mtx_);
  }

  void hmd_device_t::update_proximity(bool present) {
    if (proximity_ == vr::k_ulInvalidInputComponentHandle || (proximity_sent_ && present == last_proximity_)) {
      return;
    }
    auto *in = vr::VRDriverInput();
    if (in) {
      in->UpdateBooleanComponent(proximity_, present, 0.0);
      proximity_sent_ = true;
      last_proximity_ = present;
    }
  }

  void hmd_device_t::set_vsync_to_photons(uint32_t us) {
    if (container_ == vr::k_ulInvalidPropertyContainer) {
      return;
    }
    vr::VRProperties()->SetFloatProperty(container_, vr::Prop_SecondsFromVsyncToPhotons_Float, (float) us / 1e6f);
    const uint32_t idx = ctx_.hmd_index.load();
    if (idx != k_invalid_index) {
      vr::VREvent_Data_t d {};
      d.property.container = container_;
      d.property.prop = vr::Prop_SecondsFromVsyncToPhotons_Float;
      vr::VRServerDriverHost()->VendorSpecificEvent(idx, vr::VREvent_PropertyChanged, d, 0.0);
    }
    VRDRV_LOG_INFO("dev set-v2p us=%u", us);
  }

  // ── IVRDisplayComponent_003 ──

  void hmd_device_t::GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) {
    *pnX = 0;
    *pnY = 0;
    *pnWidth = cfg_.packed_width;
    *pnHeight = cfg_.packed_height;
  }

  bool hmd_device_t::IsDisplayOnDesktop() {
    return false;
  }

  bool hmd_device_t::IsDisplayRealDisplay() {
    return false;
  }

  void hmd_device_t::GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) {
    *pnWidth = cfg_.eye_width;
    *pnHeight = cfg_.eye_height;
  }

  void hmd_device_t::GetEyeOutputViewport(vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) {
    *pnX = eEye == vr::Eye_Left ? 0 : cfg_.eye_width;
    *pnY = 0;
    *pnWidth = cfg_.eye_width;
    *pnHeight = cfg_.eye_height;
  }

  void hmd_device_t::GetProjectionRaw(vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom) {
    // drv-M-1：top = tan(down)（負）、bottom = tan(up)（正）
    const math::rect_t r = math::fov_to_rect(cfg_.fov_tan[eEye == vr::Eye_Left ? 0 : 1]);
    *pfLeft = r.left;
    *pfRight = r.right;
    *pfTop = r.top;
    *pfBottom = r.bottom;
  }

  vr::DistortionCoordinates_t hmd_device_t::ComputeDistortion(vr::EVREye, float fU, float fV) {
    vr::DistortionCoordinates_t d {};
    d.rfRed[0] = d.rfGreen[0] = d.rfBlue[0] = fU;
    d.rfRed[1] = d.rfGreen[1] = d.rfBlue[1] = fV;
    return d;
  }

  bool hmd_device_t::ComputeInverseDistortion(vr::HmdVector2_t *pResult, vr::EVREye, uint32_t, float fU, float fV) {
    if (!pResult) {
      return false;
    }
    pResult->v[0] = fU;
    pResult->v[1] = fV;
    return true;
  }

}  // namespace vrdrv
