// probe_openvr.cpp - VipleStream §VR：vr_probe 需要 SteamVR 的模式（設計 §F.4；M1b V4）。
//
//   whoami  Background：device 0–2 的屬性、兩眼 GetProjectionRaw／GetProjectionMatrix（驗 +Y 對映）、
//           GetRecommendedRenderTargetSize、VR_IsHmdPresent()。
//   scene   Scene：WaitGetPoses → 渲染 → Submit；每眼左上角畫 vr_scene_pattern.h 的位元圖案
//           （frameCounter＋VipleVrPackQuat48(render pose)＋CRC），selftest T4 從 ring 讀回比對 descriptor 的 renderPose。
//   timing  Scene：同 scene 的迴圈（不畫圖案），每幀記 WaitGetPoses 返回的 QPC、GetTimeSinceLastVsync；
//           每 0.5 s 取 GetFrameTimings（依 m_nFrameIndex 去重）。PoC-10 的完整格（--load-ms 等）在 V6。
//   watch   Background：每 5 ms 輪詢 device 0 的 activity／bPoseIsValid／eTrackingResult，轉換時記 QPC
//           （G-DRV-4 kill-server：poseValid=0 的時間 − kill 的 QPC < 1 s）。
//   space   Background：Raw／Standing／Seated 的 HMD 姿態差（角度、位移）統計；--reset-seated 先 ResetZeroPose(Seated)。
//           G-DRV 的完整空間矩陣（外插模型辨識等）在 V6。
//
// openvr_api.dll 以 /DELAYLOAD 載入（Build-SteamVRDriver.ps1）：ipcpeer／unit 不需要它。
// 由 server 以主控台使用者 token 啟動；不從 SSH 的 Session 0 啟動（沒有 SteamVR 可連）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include <openvr.h>

#include "probe_common.h"
#include "probe_d3d.h"
#include "VipleVr.h"
#include "vr_math.h"
#include "vr_scene_pattern.h"

namespace probe {

  namespace {
    namespace vm = vrdrv::math;

    const char *init_err_name(vr::EVRInitError e) {
      switch (e) {
        case vr::VRInitError_None:
          return "none";
        case vr::VRInitError_Init_NoServerForBackgroundApp:
          return "no-server-for-background-app";
        case vr::VRInitError_Init_HmdNotFound:
          return "hmd-not-found";
        case vr::VRInitError_Init_InstallationNotFound:
          return "installation-not-found";
        default:
          return "other";
      }
    }

    // 只留可列印 ASCII；空白與 '[' 換成 '_'（line() 會再過濾一次）
    std::string clean(const char *s) {
      std::string o;
      for (; *s; ++s) {
        const unsigned char ch = (unsigned char) *s;
        o.push_back((ch < 0x21 || ch >= 0x7F || ch == '[') ? '_' : (char) ch);
      }
      return o.empty() ? std::string("-") : o;
    }

    std::string str_prop(vr::IVRSystem *sys, uint32_t dev, vr::ETrackedDeviceProperty p) {
      char buf[128] = {};
      vr::ETrackedPropertyError err = vr::TrackedProp_Success;
      sys->GetStringTrackedDeviceProperty(dev, p, buf, sizeof(buf), &err);
      return err == vr::TrackedProp_Success ? clean(buf) : std::string("-");
    }

    struct vr_session_t {
      vr::IVRSystem *sys = nullptr;
      vr::EVRInitError err = vr::VRInitError_None;

      explicit vr_session_t(vr::EVRApplicationType type) {
        sys = vr::VR_Init(&err, type);
      }

      ~vr_session_t() {
        if (sys) {
          vr::VR_Shutdown();
        }
      }

      vr_session_t(const vr_session_t &) = delete;
      vr_session_t &operator=(const vr_session_t &) = delete;
    };

    bool init_or_report(vr_session_t &s, const char *mode) {
      if (!s.sys) {
        line("result mode=%s status=vr-init-failed err=%d name=%s", mode, (int) s.err, init_err_name(s.err));
        return false;
      }
      return true;
    }

    void pose_of(const vr::HmdMatrix34_t &m, vm::quat_t &q, vm::vec3_t &p) {
      vm::matrix34_to_pose(m.m, q, p);
    }

    double pct(std::vector<double> v, double p) {
      if (v.empty()) {
        return 0.0;
      }
      std::sort(v.begin(), v.end());
      size_t i = (size_t) (p * (double) (v.size() - 1) + 0.5);
      return v[std::min(i, v.size() - 1)];
    }

    // ── scene／timing 的共用渲染迴圈 ─────────────────────────────────────
    int run_scene_loop(const args_t &a, bool pattern) {
      const char *mode = pattern ? "scene" : "timing";
      vr_session_t s(vr::VRApplication_Scene);
      if (!init_or_report(s, mode)) {
        return rc_failed;
      }
      auto *comp = vr::VRCompositor();
      if (!comp) {
        line("result mode=%s status=no-compositor", mode);
        return rc_failed;
      }
      uint32_t w = 0, h = 0;
      s.sys->GetRecommendedRenderTargetSize(&w, &h);
      int32_t adapter_index = -1;
      s.sys->GetDXGIOutputInfo(&adapter_index);
      if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        line("result mode=%s status=bad-render-size w=%u h=%u", mode, w, h);
        return rc_failed;
      }
      // SteamVR 指定的 adapter（-1 → 第一張硬體卡）
      LUID luid {};
      bool have_luid = false;
      {
        IDXGIFactory1 *f = nullptr;
        if (adapter_index >= 0 && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) &f)) && f) {
          IDXGIAdapter1 *ad = nullptr;
          if (f->EnumAdapters1((UINT) adapter_index, &ad) == S_OK && ad) {
            DXGI_ADAPTER_DESC1 d {};
            if (SUCCEEDED(ad->GetDesc1(&d))) {
              luid = d.AdapterLuid;
              have_luid = true;
            }
            ad->Release();
          }
          f->Release();
        }
      }
      d3d::device_t dev;
      HRESULT hr = d3d::create_device(have_luid ? &luid : nullptr, dev);
      if (FAILED(hr)) {
        line("result mode=%s status=d3d-failed hr=0x%08x", mode, (unsigned) hr);
        return rc_failed;
      }
      d3d::ComPtr<ID3D11Texture2D> tex[2];
      d3d::ComPtr<ID3D11RenderTargetView> rtv[2];
      D3D11_TEXTURE2D_DESC td {};
      td.Width = w;
      td.Height = h;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      for (int e = 0; e < 2; ++e) {
        hr = dev.dev->CreateTexture2D(&td, nullptr, &tex[e]);
        if (SUCCEEDED(hr)) {
          hr = dev.dev->CreateRenderTargetView(tex[e].Get(), nullptr, &rtv[e]);
        }
        if (FAILED(hr)) {
          line("result mode=%s status=texture-failed hr=0x%08x", mode, (unsigned) hr);
          return rc_failed;
        }
      }
      line("%s start size=%ux%u adapterIndex=%d fmt=%u", mode, w, h, adapter_index, (unsigned) td.Format);

      std::wstring out_dir;
      csv_t csv;
      if (prepare_out_dir(a.out, out_dir)) {
        csv.open(out_dir, pattern ? L"scene.csv" : L"timing.csv", "frame,wgp_qpc,submit_qpc,pose_valid,qx,qy,qz,qw,since_vsync_s,vsync_counter,wgp_err,submit_err");
      }
      csv_t ft_csv;
      if (!pattern && !out_dir.empty()) {
        ft_csv.open(out_dir, L"frametimings.csv", "frame_index,num_frame_presents,num_mis_presented,num_dropped,reprojection_flags,client_interval_ms,present_call_cpu_ms,total_render_gpu_ms");
      }

      const int64_t t_end = qpc() + (int64_t) (a.seconds * (double) qpf());
      uint64_t frames = 0, wgp_errors = 0, submit_errors = 0, invalid_poses = 0;
      uint16_t counter = 0;
      std::vector<double> wgp_to_submit_ms;
      int64_t last_ft_qpc = 0;
      uint32_t last_ft_index = 0;
      std::vector<double> client_interval_ms;
      uint64_t reproj_frames = 0, dropped_total = 0;
      vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
      while (qpc() < t_end) {
        const vr::EVRCompositorError we = comp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        const int64_t t_wgp = qpc();
        if (we != vr::VRCompositorError_None) {
          ++wgp_errors;
          Sleep(5);
          continue;
        }
        const auto &hp = poses[vr::k_unTrackedDeviceIndex_Hmd];
        vm::quat_t q;
        vm::vec3_t p;
        pose_of(hp.mDeviceToAbsoluteTracking, q, p);
        if (!hp.bPoseIsValid) {
          ++invalid_poses;
        }
        ++counter;
        for (int e = 0; e < 2; ++e) {
          const float bg[4] = {0.25f, 0.25f, 0.25f, 1.0f};
          dev.ctx->ClearRenderTargetView(rtv[e].Get(), bg);
          if (pattern) {
            const float qf[4] = {(float) q.x, (float) q.y, (float) q.z, (float) q.w};
            uint8_t q48[6];
            VipleVrPackQuat48(qf, q48);
            uint8_t bytes[vr_scene_pattern::k_bytes];
            vr_scene_pattern::encode(counter, q48, bytes);
            D3D11_RECT ones[vr_scene_pattern::k_bits];
            D3D11_RECT zeros[vr_scene_pattern::k_bits];
            UINT n1 = 0, n0 = 0;
            for (int k = 0; k < vr_scene_pattern::k_bits; ++k) {
              uint32_t x0, y0, x1, y1;
              vr_scene_pattern::block_rect(k, w, h, x0, y0, x1, y1);
              const D3D11_RECT r {(LONG) x0, (LONG) y0, (LONG) x1, (LONG) y1};
              if (vr_scene_pattern::bit(bytes, k)) {
                ones[n1++] = r;
              } else {
                zeros[n0++] = r;
              }
            }
            const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (n1) {
              dev.ctx1->ClearView(rtv[e].Get(), white, ones, n1);
            }
            if (n0) {
              dev.ctx1->ClearView(rtv[e].Get(), black, zeros, n0);
            }
          }
        }
        vr::EVRCompositorError se = vr::VRCompositorError_None;
        for (int e = 0; e < 2; ++e) {
          vr::Texture_t t {tex[e].Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma};
          const auto r = comp->Submit(e == 0 ? vr::Eye_Left : vr::Eye_Right, &t);
          if (r != vr::VRCompositorError_None) {
            se = r;
          }
        }
        const int64_t t_submit = qpc();
        if (se != vr::VRCompositorError_None) {
          ++submit_errors;
        }
        ++frames;
        wgp_to_submit_ms.push_back(qpc_to_ms(t_submit - t_wgp));
        float since = 0.0f;
        uint64_t vcount = 0;
        s.sys->GetTimeSinceLastVsync(&since, &vcount);
        csv.row("%u,%lld,%lld,%d,%.7f,%.7f,%.7f,%.7f,%.6f,%llu,%d,%d", (unsigned) counter, (long long) t_wgp, (long long) t_submit, hp.bPoseIsValid ? 1 : 0, q.x, q.y, q.z, q.w,
                since, (unsigned long long) vcount, (int) we, (int) se);

        if (!pattern && t_wgp - last_ft_qpc >= qpf() / 2) {
          last_ft_qpc = t_wgp;
          vr::Compositor_FrameTiming ft[64];
          for (auto &x : ft) {
            x.m_nSize = sizeof(vr::Compositor_FrameTiming);
          }
          const uint32_t got = comp->GetFrameTimings(ft, 64);
          for (uint32_t i = got; i-- > 0;) {
            if (ft[i].m_nFrameIndex <= last_ft_index) {
              continue;
            }
            ft_csv.row("%u,%u,%u,%u,0x%x,%.3f,%.3f,%.3f", ft[i].m_nFrameIndex, ft[i].m_nNumFramePresents, ft[i].m_nNumMisPresented, ft[i].m_nNumDroppedFrames, ft[i].m_nReprojectionFlags,
                       ft[i].m_flClientFrameIntervalMs, ft[i].m_flPresentCallCpuMs, ft[i].m_flTotalRenderGpuMs);
            client_interval_ms.push_back(ft[i].m_flClientFrameIntervalMs);
            if (ft[i].m_nReprojectionFlags & 0xF) {
              ++reproj_frames;
            }
            dropped_total += ft[i].m_nNumDroppedFrames;
          }
          if (got > 0) {
            last_ft_index = std::max(last_ft_index, ft[0].m_nFrameIndex);
            for (uint32_t i = 0; i < got; ++i) {
              last_ft_index = std::max(last_ft_index, ft[i].m_nFrameIndex);
            }
          }
        }
      }
      line("%s summary frames=%llu wgpErrors=%llu submitErrors=%llu invalidPoses=%llu wgpToSubmitMs p50=%.3f p95=%.3f size=%ux%u", mode, (unsigned long long) frames,
           (unsigned long long) wgp_errors, (unsigned long long) submit_errors, (unsigned long long) invalid_poses, pct(wgp_to_submit_ms, 0.5), pct(wgp_to_submit_ms, 0.95), w, h);
      if (!pattern) {
        line("timing frametimings n=%u clientIntervalMs p50=%.3f p99=%.3f reprojFrames=%llu dropped=%llu", (unsigned) client_interval_ms.size(), pct(client_interval_ms, 0.5),
             pct(client_interval_ms, 0.99), (unsigned long long) reproj_frames, (unsigned long long) dropped_total);
      }
      return frames > 0 ? rc_ok : rc_failed;
    }
  }  // namespace

  int run_whoami(const args_t &) {
    vr_session_t s(vr::VRApplication_Background);
    const bool present = vr::VR_IsHmdPresent();
    if (!init_or_report(s, "whoami")) {
      line("whoami hmdPresent=%d", present ? 1 : 0);
      return rc_failed;
    }
    for (uint32_t d = 0; d < 3; ++d) {
      const auto cls = s.sys->GetTrackedDeviceClass(d);
      if (cls == vr::TrackedDeviceClass_Invalid) {
        line("whoami dev=%u class=invalid", d);
        continue;
      }
      vr::ETrackedPropertyError e1 = vr::TrackedProp_Success, e2 = vr::TrackedProp_Success, e3 = vr::TrackedProp_Success;
      const uint64_t universe = s.sys->GetUint64TrackedDeviceProperty(d, vr::Prop_CurrentUniverseId_Uint64, &e1);
      const float hz = s.sys->GetFloatTrackedDeviceProperty(d, vr::Prop_DisplayFrequency_Float, &e2);
      const float v2p = s.sys->GetFloatTrackedDeviceProperty(d, vr::Prop_SecondsFromVsyncToPhotons_Float, &e3);
      line("whoami dev=%u class=%d system=%s model=%s manufacturer=%s universe=%llu hz=%.2f v2pS=%.5f activity=%d connected=%d", d, (int) cls,
           str_prop(s.sys, d, vr::Prop_TrackingSystemName_String).c_str(), str_prop(s.sys, d, vr::Prop_ModelNumber_String).c_str(),
           str_prop(s.sys, d, vr::Prop_ManufacturerName_String).c_str(), (unsigned long long) (e1 == vr::TrackedProp_Success ? universe : 0),
           e2 == vr::TrackedProp_Success ? hz : 0.0f, e3 == vr::TrackedProp_Success ? v2p : 0.0f, (int) s.sys->GetTrackedDeviceActivityLevel(d),
           s.sys->IsTrackedDeviceConnected(d) ? 1 : 0);
    }
    uint32_t w = 0, h = 0;
    s.sys->GetRecommendedRenderTargetSize(&w, &h);
    bool y_ok = true;
    for (int e = 0; e < 2; ++e) {
      const auto eye = e == 0 ? vr::Eye_Left : vr::Eye_Right;
      float l = 0, r = 0, t = 0, b = 0;
      s.sys->GetProjectionRaw(eye, &l, &r, &t, &b);
      const vr::HmdMatrix44_t m = s.sys->GetProjectionMatrix(eye, 0.1f, 100.0f);
      // 方向 (0, b, -1)（b = tan(up)）→ NDC y 應為 +1；(0, t, -1) → −1
      const float ndc_up = m.m[1][1] * b - m.m[1][2];
      const float ndc_down = m.m[1][1] * t - m.m[1][2];
      const bool ok = std::fabs(ndc_up - 1.0f) < 1e-3f && std::fabs(ndc_down + 1.0f) < 1e-3f;
      y_ok = y_ok && ok;
      line("whoami eye=%d raw l=%.4f r=%.4f t=%.4f b=%.4f m00=%.4f m02=%.4f m11=%.4f m12=%.4f ndcUp=%.4f ndcDown=%.4f yMapping=%s", e, l, r, t, b, m.m[0][0], m.m[0][2], m.m[1][1],
           m.m[1][2], ndc_up, ndc_down, ok ? "ok" : "bad");
    }
    line("whoami summary hmdPresent=%d renderTarget=%ux%u yMapping=%s", present ? 1 : 0, w, h, y_ok ? "ok" : "bad");
    return rc_ok;
  }

  int run_scene(const args_t &a) {
    return run_scene_loop(a, true);
  }

  int run_timing(const args_t &a) {
    return run_scene_loop(a, false);
  }

  int run_watch(const args_t &a) {
    vr_session_t s(vr::VRApplication_Background);
    if (!init_or_report(s, "watch")) {
      return rc_failed;
    }
    std::wstring out_dir;
    csv_t csv;
    if (prepare_out_dir(a.out, out_dir)) {
      csv.open(out_dir, L"watch.csv", "qpc,activity,pose_valid,tracking_result,connected");
    }
    line("watch start qpf=%lld", (long long) qpf());
    const int64_t t_end = qpc() + (int64_t) (a.seconds * (double) qpf());
    int last_act = -100, last_valid = -1, last_result = -1, last_conn = -1;
    uint64_t transitions = 0;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    while (qpc() < t_end) {
      vr::TrackedDevicePose_t p[1] {};
      s.sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0.0f, p, 1);
      const int act = (int) s.sys->GetTrackedDeviceActivityLevel(0);
      const int valid = p[0].bPoseIsValid ? 1 : 0;
      const int result = (int) p[0].eTrackingResult;
      const int conn = p[0].bDeviceIsConnected ? 1 : 0;
      const int64_t now = qpc();
      if (act != last_act || valid != last_valid || result != last_result || conn != last_conn) {
        ++transitions;
        csv.row("%lld,%d,%d,%d,%d", (long long) now, act, valid, result, conn);
        // kill-server 判讀用：QPC 值（同一台機器的 QPC 在各 session 共用）
        line("watch transition qpc=%lld activity=%d poseValid=%d result=%d connected=%d", (long long) now, act, valid, result, conn);
        last_act = act;
        last_valid = valid;
        last_result = result;
        last_conn = conn;
      }
      LARGE_INTEGER due;
      due.QuadPart = -50000;  // 5 ms
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, 100);
      } else {
        Sleep(5);
      }
    }
    if (timer) {
      CloseHandle(timer);
    }
    line("watch summary transitions=%llu", (unsigned long long) transitions);
    return rc_ok;
  }

  int run_space(const args_t &a) {
    vr_session_t s(vr::VRApplication_Background);
    if (!init_or_report(s, "space")) {
      return rc_failed;
    }
    if (a.reset_seated) {
      if (auto *ch = vr::VRChaperone()) {
        ch->ResetZeroPose(vr::TrackingUniverseSeated);
        line("space reset-seated done");
      }
    }
    std::wstring out_dir;
    csv_t csv;
    if (prepare_out_dir(a.out, out_dir)) {
      csv.open(out_dir, L"space.csv", "qpc,pred_s,standing_vs_raw_deg,standing_vs_raw_mm,seated_vs_raw_deg,seated_vs_raw_mm,raw_valid");
    }
    const float preds[3] = {0.0f, 0.011f, 0.033f};
    std::vector<double> st_deg, st_mm, se_deg, se_mm;
    const int64_t t_end = qpc() + (int64_t) (a.seconds * (double) qpf());
    while (qpc() < t_end) {
      for (float pr : preds) {
        vr::TrackedDevicePose_t raw[1] {}, st[1] {}, se[1] {};
        s.sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, pr, raw, 1);
        s.sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, pr, st, 1);
        s.sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, pr, se, 1);
        vm::quat_t qr, qs, qe;
        vm::vec3_t pr_, ps, pe;
        pose_of(raw[0].mDeviceToAbsoluteTracking, qr, pr_);
        pose_of(st[0].mDeviceToAbsoluteTracking, qs, ps);
        pose_of(se[0].mDeviceToAbsoluteTracking, qe, pe);
        const double a1 = vm::angle_deg(qr, qs);
        const double d1 = std::sqrt((pr_.x - ps.x) * (pr_.x - ps.x) + (pr_.y - ps.y) * (pr_.y - ps.y) + (pr_.z - ps.z) * (pr_.z - ps.z)) * 1000.0;
        const double a2 = vm::angle_deg(qr, qe);
        const double d2 = std::sqrt((pr_.x - pe.x) * (pr_.x - pe.x) + (pr_.y - pe.y) * (pr_.y - pe.y) + (pr_.z - pe.z) * (pr_.z - pe.z)) * 1000.0;
        csv.row("%lld,%.3f,%.4f,%.2f,%.4f,%.2f,%d", (long long) qpc(), pr, a1, d1, a2, d2, raw[0].bPoseIsValid ? 1 : 0);
        if (pr == 0.0f && raw[0].bPoseIsValid) {
          st_deg.push_back(a1);
          st_mm.push_back(d1);
          se_deg.push_back(a2);
          se_mm.push_back(d2);
        }
      }
      Sleep(10);
    }
    line("space summary n=%u standingVsRawDeg p50=%.4f max=%.4f standingVsRawMm max=%.2f seatedVsRawDeg p50=%.4f seatedVsRawMm p50=%.1f universe=%llu",
         (unsigned) st_deg.size(), pct(st_deg, 0.5), pct(st_deg, 1.0), pct(st_mm, 1.0), pct(se_deg, 0.5), pct(se_mm, 0.5),
         (unsigned long long) s.sys->GetUint64TrackedDeviceProperty(0, vr::Prop_CurrentUniverseId_Uint64));
    return rc_ok;
  }

}  // namespace probe
