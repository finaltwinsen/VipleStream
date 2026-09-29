// driver_main.cpp - VipleStream §VR：SteamVR driver 入口（設計 §E.2 driver_main；K8）。
//
// - HmdDriverFactory 只比對 IServerTrackedDeviceProvider_Version，回傳靜態 provider；零副作用
//   （steam.exe 等行程也會 probe：不開執行緒、不連 pipe、不配置 D3D）。
// - 不定義 DllMain（/MT 的 static CRT 需要 DLL_THREAD_ATTACH/DETACH）；沒有會呼叫 Win32／D3D／開執行緒的全域建構子
//   （g_provider 只有常數初始化的成員）。
// - Init 一律回 VRInitError_None：pipe 不存在就立刻返回；連上了就同步等握手最多 1.5 s，armed 時直接
//   TrackedDeviceAdded（不等 Activate，Activate 在 Init 返回後才會被呼叫）。
// - 所有 SteamVR host API（TrackedDeviceAdded、VendorSpecificEvent、屬性）只在 RunFrame 或裝置回呼裡呼叫；
//   ipc 執行緒只改 driver_ctx_t 的 atomic 旗標（§B.6）。TrackedDevicePoseUpdated 由 tracking 執行緒呼叫。
// - crash containment：seh_guard.h；禁止 SetUnhandledExceptionFilter。
#include <openvr_driver.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include "controller_device.h"
#include "d3d_device.h"
#include "direct_mode.h"
#include "driver_context.h"
#include "driver_log.h"
#include "driver_version.h"
#include "hmd_device.h"
#include "ipc_client.h"
#include "ipc_proto.h"
#include "seh_guard.h"
#include "tracking.h"

namespace vrdrv {

  namespace {
    std::atomic<uint32_t> g_degraded {0};
    driver_ctx_t *g_ctx = nullptr;

    int64_t now_qpc() {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
    }

    int64_t qpf_value() {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return f.QuadPart;
    }

    // IVRDriverLog：vrserver.txt 會自動加上 "viplestream: " 前綴
    void log_sink(void *, int, const char *line) {
      auto *l = vr::VRDriverLog();
      if (!l) {
        return;
      }
      char buf[560];
      std::snprintf(buf, sizeof(buf), "[VIPLE-VR-DRV] %s\n", line);
      l->Log(buf);
    }

    // IPC log ring：server 以 [VIPLE-VR-DRV] 重新加前綴轉印（§B.8 第 8 點）；不可呼叫 vrdrv::log
    void log_forward(void *, int level, const char *line) {
      if (!g_ctx || !g_ctx->ipc) {
        return;
      }
      auto gen = g_ctx->ipc->current();
      if (gen) {
        gen->push_log((uint32_t) level, line);
      }
    }

    // 只影響我們自己的 CRT 實例：參數錯誤不 __fastfail 整個 vrserver
    void __cdecl invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t) {
    }

    // purecall：轉成 AV 讓 guarded() 攔下（不在 guarded 內時照常傳給 vrserver）
    void __cdecl purecall() {
      RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }

    const char *first_name(ipc_client_t::first_e v) {
      switch (v) {
        case ipc_client_t::first_e::pending:
          return "pending";
        case ipc_client_t::first_e::mapped:
          return "mapped";
        case ipc_client_t::first_e::no_server:
          return "no-server";
        case ipc_client_t::first_e::untrusted:
          return "untrusted";
        case ipc_client_t::first_e::rejected:
          return "rejected";
        case ipc_client_t::first_e::dead:
          return "dead";
        case ipc_client_t::first_e::failed:
          return "failed";
      }
      return "?";
    }

    class provider_t final: public vr::IServerTrackedDeviceProvider {
    public:
      vr::EVRInitError Init(vr::IVRDriverContext *pDriverContext) override;
      void Cleanup() override;

      const char *const *GetInterfaceVersions() override {
        return vr::k_InterfaceVersions;
      }

      void RunFrame() override;

      bool ShouldBlockStandbyMode() override {
        return false;
      }

      void EnterStandby() override {
      }

      void LeaveStandby() override {
      }

    private:
      void try_add_hmd();
      void run_frame_inner();
      void update_other_hmd();

      bool context_ok_ = false;
      driver_ctx_t *ctx_ = nullptr;
      ipc_client_t *ipc_ = nullptr;
      direct_mode_t *dm_ = nullptr;
      tracking_t *trk_ = nullptr;
      hmd_device_t *hmd_ = nullptr;
      controller_device_t *ctrl_[2] = {nullptr, nullptr};  // S2-09；物件刻意不釋放（SteamVR 持有指標到 Cleanup 之後）
      bool add_rejected_ = false;
      bool standby_ = false;
      bool exiting_sent_ = false;
      int64_t last_other_qpc_ = 0;
      bool other_name_done_ = false;
    };

    provider_t g_provider;

    vr::EVRInitError provider_t::Init(vr::IVRDriverContext *pDriverContext) {
      // K8：一律回 VRInitError_None（context 失敗時也一樣；沒有 context 就什麼都不做）
      if (vr::InitServerDriverContext(pDriverContext) != vr::VRInitError_None) {
        return vr::VRInitError_None;
      }
      context_ok_ = true;
      _set_invalid_parameter_handler(&invalid_parameter);
      _set_purecall_handler(&purecall);
      log::set_sink(&log_sink, nullptr);
      const int64_t t0 = now_qpc();

      guarded("Init", [&]() {
        ctx_ = new driver_ctx_t();
        ctx_->qpf = qpf_value();
        ctx_->pose_hist.set_qpf(ctx_->qpf);
        ipc_ = new ipc_client_t();
        ctx_->ipc = ipc_;
        g_ctx = ctx_;
        log::set_forward(&log_forward, nullptr);
        dm_ = new direct_mode_t(*ctx_);
        trk_ = new tracking_t(*ctx_);

        driver_ctx_t *ctx = ctx_;
        direct_mode_t *dm = dm_;
        ipc_callbacks_t cb;
        cb.on_mapped = [ctx](const generation_ptr &, bool armed) {
          ctx->armed.store(armed);
          // 新 generation＝新的 client session：client 空間可能重建，Δ 回到恆等重新學（§E.2）
          ctx->pose_hist.reset();
          ctx->link_up.store(true);
          ctx->link_events.fetch_add(1);
        };
        cb.on_textures = [dm](const generation_ptr &gen, const vripc_textures_t &msg, const HANDLE tex[VRIPC_TEX_COUNT], HANDLE sf, HANDLE cf) {
          return dm->on_textures(gen, msg, tex, sf, cf);
        };
        cb.on_teardown = [ctx, dm](uint64_t generation, const char *reason) {
          dm->on_teardown(generation);
          ctx->link_up.store(false);
          ctx->link_events.fetch_add(1);
          // G-DRV-4 的判讀行：vrserver.txt 會出現「viplestream: [VIPLE-VR-DRV] pipe lost … standby」
          VRDRV_LOG_INFO("pipe lost gen=%llu reason=%s -> standby (hmd stays connected)", (unsigned long long) generation, reason ? reason : "?");
        };
        cb.on_server_state = [ctx](uint32_t kind, uint32_t arg) {
          switch (kind) {
            case VRIPC_ST_ARM:
              ctx->armed.store(true);
              break;
            case VRIPC_ST_DISARM:
              ctx->armed.store(false);
              break;
            case VRIPC_ST_REQUEST_QUIT:
              ctx->quit_pending.store(true);
              break;
            case VRIPC_ST_DEV_SET_V2P:
              ctx->v2p_pending.store(arg);
              break;
            default:
              break;
          }
        };
        cb.on_server_heartbeat = [ctx](bool stale) {
          ctx->server_stale.store(stale);
        };
        cb.on_rejected = [](uint32_t reason, uint32_t detail) {
          VRDRV_LOG_WARN("ipc rejected reason=%u detail=%u", reason, detail);
        };

        ipc_client_config_t cfg;
        cfg.driver_version_packed = VIPLE_DRIVER_VERSION_PACKED;
        cfg.driver_caps = VRIPC_DCAP_TIMING_RING;
        cfg.openvr_sdk_packed = proto::pack_sdk(vr::k_nSteamVRVersionMajor, vr::k_nSteamVRVersionMinor, vr::k_nSteamVRVersionBuild);
        cfg.iface_directmode = vr::IVRDriverDirectModeComponent_Version;
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR) &log_sink, &self)) {
          cfg.expected_server_image = ipc_client_t::expected_server_image_from_module(self);
        }
        if (!trk_->start()) {
          VRDRV_LOG_ERROR("tracking thread start failed");
        }
        if (!ipc_->start(cfg, cb)) {
          VRDRV_LOG_ERROR("ipc start failed");
          return;
        }
        const auto first = ipc_->wait_first(1500);
        if (first == ipc_client_t::first_e::mapped && ctx_->armed.load()) {
          try_add_hmd();
        }
        const double ms = (double) (now_qpc() - t0) * 1000.0 / (double) ctx_->qpf;
        VRDRV_LOG_INFO("init ver=%s ms=%.1f first=%s armed=%d hmdAdded=%d iface=%s", VIPLE_DRIVER_VERSION_STR, ms, first_name(first), ctx_->armed.load() ? 1 : 0,
                       ctx_->hmd_added.load() ? 1 : 0, vr::IVRDriverDirectModeComponent_Version);
      });
      return vr::VRInitError_None;
    }

    void provider_t::try_add_hmd() {
      if (!ctx_ || ctx_->hmd_added.load() || add_rejected_) {
        return;
      }
      auto gen = ipc_->current();
      if (!gen || gen->idle() || !ctx_->armed.load()) {
        return;
      }
      hmd_ = new hmd_device_t(*ctx_, *dm_, gen->config());
      trk_->set_hmd(hmd_);
      const bool ok = vr::VRServerDriverHost()->TrackedDeviceAdded(hmd_device_t::k_serial, vr::TrackedDeviceClass_HMD, hmd_);
      if (ok) {
        ctx_->hmd_added.store(true);
        ipc_->set_device_state(VRIPC_DRV_HMD_ADDED);
        ipc_->send_state(VRIPC_ST_HMD_ADDED, 0);
        // S2-09：最小 Touch 控制器（左右各一）；沒有控制器資料時 poseIsValid=false／OutOfRange
        for (int h = 0; h < 2; ++h) {
          auto *c = new controller_device_t(*ctx_, h == 1);
          if (vr::VRServerDriverHost()->TrackedDeviceAdded(c->serial(), vr::TrackedDeviceClass_Controller, c)) {
            ctrl_[h] = c;
          } else {
            VRDRV_LOG_WARN("controller add rejected hand=%s", h == 1 ? "right" : "left");
          }
        }
        trk_->set_controllers(ctrl_[0], ctrl_[1]);
      } else {
        // 被拒就不再重試（避免每幀洗 log）；hmd 物件刻意保留（SteamVR 可能仍持有指標）
        add_rejected_ = true;
        ipc_->send_state(VRIPC_ST_HMD_ADD_REJECTED, 0);
      }
      VRDRV_LOG_INFO("hmd added=%d gen=%llu ring=%ux%u hz=%u", ok ? 1 : 0, (unsigned long long) gen->id(), gen->config().packed_width, gen->config().packed_height,
                     gen->config().refresh_mhz / 1000);
    }

    void provider_t::update_other_hmd() {
      const int64_t now = now_qpc();
      if (now - last_other_qpc_ < ctx_->qpf / 10) {
        return;
      }
      last_other_qpc_ = now;
      uint32_t v = VRIPC_OTHER_HMD_NONE;
      if (ctx_->hmd_index.load() == 0) {
        v = VRIPC_OTHER_HMD_OURS;
      } else {
        vr::TrackedDevicePose_t poses[1] {};
        vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0.0f, poses, 1);
        if (poses[0].bDeviceIsConnected) {
          v = VRIPC_OTHER_HMD_PRESENT;
          if (!other_name_done_) {
            // UNVERIFIED：跨 driver 讀屬性；讀不到就留空
            other_name_done_ = true;
            char name[64] = {};
            vr::ETrackedPropertyError err = vr::TrackedProp_Success;
            auto *p = vr::VRProperties();
            p->GetStringProperty(p->TrackedDeviceToPropertyContainer(0), vr::Prop_TrackingSystemName_String, name, sizeof(name), &err);
            if (err == vr::TrackedProp_Success) {
              ipc_->set_other_hmd_system(name);
            }
          }
        }
      }
      ipc_->status().other_hmd.store(v, std::memory_order_relaxed);
    }

    void provider_t::run_frame_inner() {
      auto *host = vr::VRServerDriverHost();
      vr::VREvent_t ev {};
      while (host->PollNextEvent(&ev, sizeof(ev))) {
        switch (ev.eventType) {
          case vr::VREvent_Input_HapticVibration:
            for (auto *c : ctrl_) {
              if (c && c->on_haptic(ev.data.hapticVibration)) {
                break;
              }
            }
            break;
          // space-delta 的「事件觸發」視窗（§E.2：事件後 2 s 內即使在轉頭也可採用新 Δ）
          case vr::VREvent_SeatedZeroPoseReset:
            ctx_->pose_hist.note_event("SeatedZeroPoseReset", now_qpc());
            break;
          case vr::VREvent_StandingZeroPoseReset:
            ctx_->pose_hist.note_event("StandingZeroPoseReset", now_qpc());
            break;
          case vr::VREvent_ChaperoneUniverseHasChanged:
            ctx_->pose_hist.note_event("ChaperoneUniverseHasChanged", now_qpc());
            break;
          case vr::VREvent_SceneApplicationChanged:
            ctx_->pose_hist.note_event("SceneApplicationChanged", now_qpc());
            break;
          default:
            break;
        }
      }
      if (!exiting_sent_ && host->IsExiting()) {
        exiting_sent_ = true;
        ipc_->send_state(VRIPC_ST_EXITING, 0);
        VRDRV_LOG_INFO("vrserver exiting");
      }
      const bool active = ctx_->active();
      if (ctx_->armed.load() && !ctx_->hmd_added.load()) {
        try_add_hmd();
      }
      if (ctx_->hmd_added.load()) {
        if (!active && !standby_) {
          standby_ = true;
          ctx_->presenting.store(false);
          ipc_->set_device_state(VRIPC_DRV_STANDBY);
          ipc_->send_state(VRIPC_ST_HMD_STANDBY, 0);
          VRDRV_LOG_INFO("hmd standby link=%d armed=%d serverStale=%d", ctx_->link_up.load() ? 1 : 0, ctx_->armed.load() ? 1 : 0, ctx_->server_stale.load() ? 1 : 0);
        } else if (active && standby_) {
          standby_ = false;
          ipc_->set_device_state(VRIPC_DRV_HMD_ADDED);
          VRDRV_LOG_INFO("hmd leave-standby");
        }
        if (hmd_) {
          hmd_->update_proximity(active && ctx_->presence.load());
        }
      }
      if (ctx_->quit_pending.exchange(false)) {
        // 只在我們的 HMD 存在時，以 HMD 的 index 送（drv-m-17）
        const uint32_t idx = ctx_->hmd_index.load();
        if (idx != k_invalid_index) {
          vr::VREvent_Data_t d {};
          host->VendorSpecificEvent(idx, vr::VREvent_DriverRequestedQuit, d, 0.0);
          VRDRV_LOG_INFO("request-quit sent index=%u", idx);
        } else {
          VRDRV_LOG_INFO("request-quit ignored (no hmd)");
        }
      }
      if (const uint32_t v2p = ctx_->v2p_pending.exchange(0); v2p != 0 && hmd_) {
        hmd_->set_vsync_to_photons(v2p);
      }
      update_other_hmd();
    }

    void provider_t::RunFrame() {
      if (degraded() || !ctx_ || !ipc_) {
        return;
      }
      guarded("RunFrame", [&]() {
        run_frame_inner();
      });
    }

    void provider_t::Cleanup() {
      if (!context_ok_) {
        return;
      }
      // §B.6：有上限的程序（tracking 200 ms、ipc 300 ms）；逾時就放棄 join、刻意不釋放（不能讓 vrserver 關閉卡住）
      bool ipc_stopped = true;
      bool trk_stopped = true;
      if (trk_ && !trk_->stop(200)) {
        trk_stopped = false;  // 執行緒還在用 ctx_／ipc_：下面一律不釋放
        trk_ = nullptr;
      }
      if (ipc_) {
        if (ctx_ && ctx_->hmd_added.load()) {
          ipc_->send_state(VRIPC_ST_EXITING, 0);
        }
        ipc_stopped = ipc_->stop(300);
      }
      if (dm_) {
        dm_->release_all();
      }
      VRDRV_LOG_INFO("cleanup ipcStopped=%d", ipc_stopped ? 1 : 0);
      log::set_forward(nullptr, nullptr);
      log::set_sink(nullptr, nullptr);
      if (ipc_stopped && trk_stopped) {
        g_ctx = nullptr;
        delete trk_;
        trk_ = nullptr;
        delete ipc_;
        ipc_ = nullptr;
      }
      vr::CleanupDriverContext();
      context_ok_ = false;
    }
  }  // namespace

  bool degraded() {
    return g_degraded.load(std::memory_order_relaxed) != 0;
  }

  void mark_degraded(uint32_t code, const char *where) {
    uint32_t expected = 0;
    if (!g_degraded.compare_exchange_strong(expected, code != 0 ? code : 1u)) {
      return;
    }
    VRDRV_LOG_ERROR("degraded code=0x%08x at=%s (callbacks return safe values from now on)", code, where ? where : "?");
    if (g_ctx && g_ctx->ipc) {
      g_ctx->ipc->status().degraded_code.store(code, std::memory_order_relaxed);
      g_ctx->ipc->set_device_state(VRIPC_DRV_DEGRADED);
      g_ctx->ipc->send_state(VRIPC_ST_DEGRADED, code);
    }
  }

}  // namespace vrdrv

extern "C" __declspec(dllexport) void *HmdDriverFactory(const char *pInterfaceName, int *pReturnCode) {
  if (pInterfaceName && std::strcmp(pInterfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) {
    return static_cast<vr::IServerTrackedDeviceProvider *>(&vrdrv::g_provider);
  }
  if (pReturnCode) {
    *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
  }
  return nullptr;
}
