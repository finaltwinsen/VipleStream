// direct_mode.cpp - 見 direct_mode.h（設計 §E.2 direct_mode、frame_compositor；§B.7）。
#include "direct_mode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "driver_log.h"
#include "seh_guard.h"

namespace vrdrv {

  namespace {
    int64_t now_qpc() {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
    }

    float half_to_float(uint16_t val) {
      uint32_t sign = ((uint32_t) val & 0x8000) << 16;
      uint32_t exp = (val >> 10) & 0x1F;
      uint32_t mant = val & 0x03FF;
      uint32_t f;

      if (exp == 0) {
        if (mant == 0) {
          f = sign;
        } else {
          exp = 1;
          while ((mant & 0x0400) == 0) {
            mant <<= 1;
            exp++;
          }
          mant &= 0x03FF;
          f = sign | ((127 - 15 - exp + 1) << 23) | (mant << 13);
        }
      } else if (exp == 31) {
        f = sign | 0x7F800000 | (mant << 13);
      } else {
        f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
      }
      float out = 0.0f;
      std::memcpy(&out, &f, sizeof(out));
      return out;
    }

    bool is_depth_format(uint32_t f) {
      switch (f) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
          return true;
        default:
          return false;
      }
    }

    // 合成白名單（§E.2 frame_compositor）：回傳 SRV 格式與 shader mode；不在白名單回 false
    bool compose_format(uint32_t f, DXGI_FORMAT &srv, uint32_t &mode, bool &force_alpha) {
      force_alpha = false;
      switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
          srv = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
          mode = 0;
          return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
          srv = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
          mode = 0;
          return true;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
          srv = DXGI_FORMAT_R8G8B8A8_UNORM;
          mode = 1;
          return true;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
          srv = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
          mode = 0;
          return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
          srv = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
          mode = 0;
          return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
          srv = DXGI_FORMAT_B8G8R8A8_UNORM;
          mode = 1;
          return true;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
          srv = DXGI_FORMAT_B8G8R8X8_UNORM;
          mode = 1;
          force_alpha = true;
          return true;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
          srv = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
          mode = 0;
          force_alpha = true;
          return true;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
          srv = DXGI_FORMAT_R10G10B10A2_UNORM;
          mode = 1;
          return true;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
          srv = DXGI_FORMAT_R16G16B16A16_FLOAT;
          mode = 2;
          return true;
        default:
          return false;
      }
    }
  }  // namespace

  direct_mode_t::direct_mode_t(driver_ctx_t &ctx):
      ctx_(ctx) {
  }

  direct_mode_t::~direct_mode_t() {
    if (timer_) {
      CloseHandle((HANDLE) timer_);
      timer_ = nullptr;
    }
  }

  void direct_mode_t::activate(const vripc_session_config_t &cfg, uint32_t eye_w, uint32_t eye_h, const LUID &luid) {
    lock();
    activated_ = true;
    eye_w_ = eye_w;
    eye_h_ = eye_h;
    eye_rect_[0] = math::fov_to_rect(cfg.fov_tan[0]);
    eye_rect_[1] = math::fov_to_rect(cfg.fov_tan[1]);
    load_fovea_config_locked(cfg);
    act_luid_ = luid;
    unlock();
    vsync_.activate(ctx_.qpf, cfg.refresh_mhz, now_qpc());
    if (!timer_) {
      timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      if (!timer_) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
      }
    }
  }

  void direct_mode_t::release_all() {
    lock();
    layer_count_ = 0;
    ring_.reset();
    gen_.reset();
    by_handle_.clear();
    sets_.clear();
    sync_mutex_.Reset();
    sync_tex_.Reset();
    sync_handle_ = 0;
    device_ = d3d_device_t {};
    unlock();
  }

  HRESULT direct_mode_t::ensure_device_locked(const LUID *hint) {
    if (device_.valid()) {
      return S_OK;
    }
    LUID luid {};
    if (hint) {
      luid = *hint;
    } else if (activated_) {
      luid = act_luid_;
    } else if (gen_ && !gen_->idle()) {
      luid.LowPart = gen_->config().adapter_luid_low;
      luid.HighPart = gen_->config().adapter_luid_high;
    } else if (ctx_.ipc) {
      auto g = ctx_.ipc->current();
      if (g && !g->idle()) {
        luid.LowPart = g->config().adapter_luid_low;
        luid.HighPart = g->config().adapter_luid_high;
      }
    }
    if (luid.LowPart == 0 && luid.HighPart == 0) {
      return E_NOT_VALID_STATE;
    }
    const HRESULT hr = device_.ensure(luid);
    if (FAILED(hr)) {
      VRDRV_LOG_ERROR("d3d device-create-failed hr=0x%08x", (unsigned) hr);
      return hr;
    }
    const HRESULT chr = comp_.init(device_.dev());
    if (FAILED(chr)) {
      comp_failed_ = true;
      VRDRV_LOG_ERROR("compositor init-failed hr=0x%08x", (unsigned) chr);
    }
    VRDRV_LOG_INFO("d3d device created (direct-mode device, lives until Cleanup)");
    return S_OK;
  }

  // ── swap texture set ────────────────────────────────────────────────

  void direct_mode_t::create_set_locked(uint32_t pid, const SwapTextureSetDesc_t &desc, SwapTextureSet_t &out) {
    const uint32_t samples = desc.nSampleCount == 0 ? 1 : desc.nSampleCount;
    if (FAILED(ensure_device_locked(nullptr))) {
      VRDRV_LOG_WARN("swapset create pid=%u fmt=%u size=%ux%u samples=%u result=no-device", pid, desc.nFormat, desc.nWidth, desc.nHeight, samples);
      return;
    }
    auto set = std::make_unique<swapset_t>();
    set->pid = pid;
    set->format = (DXGI_FORMAT) desc.nFormat;
    set->samples = samples;
    set->width = desc.nWidth;
    set->height = desc.nHeight;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = desc.nWidth;
    td.Height = desc.nHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = (DXGI_FORMAT) desc.nFormat;
    td.SampleDesc.Count = samples;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = is_depth_format(desc.nFormat) ? D3D11_BIND_DEPTH_STENCIL : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    // legacy KMT 共享：只在 vrserver／vrcompositor／app 之間使用，不跨 SYSTEM 邊界
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    DXGI_FORMAT srv_fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t mode = 0;
    bool force_alpha = false;
    const bool composable = samples == 1 && !is_depth_format(desc.nFormat) && compose_format(desc.nFormat, srv_fmt, mode, force_alpha);
    HRESULT hr = S_OK;
    for (int i = 0; i < 3 && SUCCEEDED(hr); ++i) {
      hr = device_.dev()->CreateTexture2D(&td, nullptr, &set->tex[i]);
      if (FAILED(hr)) {
        break;
      }
      ComPtr<IDXGIResource> res;
      hr = set->tex[i].As(&res);
      if (FAILED(hr)) {
        break;
      }
      HANDLE h = nullptr;
      hr = res->GetSharedHandle(&h);
      if (FAILED(hr) || !h) {
        hr = FAILED(hr) ? hr : E_FAIL;
        break;
      }
      set->handle[i] = (uint64_t) (uintptr_t) h;
      if (composable) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = srv_fmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = device_.dev()->CreateShaderResourceView(set->tex[i].Get(), &sd, &set->srv[i]);
      }
    }
    if (FAILED(hr)) {
      // 任何一張失敗 → 整組回收（map 不留懸空項），三個 handle 回 0
      VRDRV_LOG_WARN("swapset-create-failed pid=%u fmt=%u size=%ux%u samples=%u hr=0x%08x", pid, desc.nFormat, desc.nWidth, desc.nHeight, samples, (unsigned) hr);
      return;
    }
    set->mode = mode;
    set->force_alpha = force_alpha;
    set->composable = composable;
    for (uint32_t i = 0; i < 3; ++i) {
      out.rSharedTextureHandles[i] = set->handle[i];
      by_handle_[set->handle[i]] = {set.get(), i};
    }
    out.unTextureFlags = 0;
    // P-B3：pid／fmt／w×h／samples 分佈
    VRDRV_LOG_INFO("swapset create pid=%u fmt=%u size=%ux%u samples=%u composable=%d result=ok", pid, desc.nFormat, desc.nWidth, desc.nHeight, samples, composable ? 1 : 0);
    sets_.push_back(std::move(set));
    ctx_.ipc->status().swapsets_live.store((uint32_t) sets_.size(), std::memory_order_relaxed);
  }

  void direct_mode_t::destroy_set_locked(swapset_t *set) {
    for (const uint64_t h : set->handle) {
      by_handle_.erase(h);
    }
    // 待合成的 layer 若引用這組貼圖，Present 時查不到就略過（layer 只存 handle 值）
    sets_.erase(std::remove_if(sets_.begin(), sets_.end(), [set](const std::unique_ptr<swapset_t> &p) {
                  return p.get() == set;
                }),
                sets_.end());
    ctx_.ipc->status().swapsets_live.store((uint32_t) sets_.size(), std::memory_order_relaxed);
  }

  const direct_mode_t::swapset_t *direct_mode_t::find_locked(uint64_t handle, uint32_t &index) const {
    auto it = by_handle_.find(handle);
    if (it == by_handle_.end()) {
      return nullptr;
    }
    index = it->second.second;
    return it->second.first;
  }

  void direct_mode_t::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pDesc, SwapTextureSet_t *pOut) {
    if (!pOut) {
      return;
    }
    std::memset(pOut, 0, sizeof(*pOut));
    if (degraded() || !pDesc) {
      return;
    }
    lock();
    guarded("CreateSwapTextureSet", [&]() {
      create_set_locked(unPid, *pDesc, *pOut);
    });
    unlock();
  }

  void direct_mode_t::DestroySwapTextureSet(vr::SharedTextureHandle_t h) {
    if (degraded()) {
      return;
    }
    lock();
    guarded("DestroySwapTextureSet", [&]() {
      uint32_t idx = 0;
      auto *set = const_cast<swapset_t *>(find_locked(h, idx));
      if (set) {
        destroy_set_locked(set);
      }
    });
    unlock();
  }

  void direct_mode_t::DestroyAllSwapTextureSets(uint32_t unPid) {
    if (degraded()) {
      return;
    }
    lock();
    guarded("DestroyAllSwapTextureSets", [&]() {
      // 先收集再刪（不依賴 map 走訪順序）
      std::vector<swapset_t *> victims;
      for (const auto &s : sets_) {
        if (s->pid == unPid) {
          victims.push_back(s.get());
        }
      }
      for (auto *s : victims) {
        destroy_set_locked(s);
      }
      if (!victims.empty()) {
        VRDRV_LOG_INFO("swapset destroy-all pid=%u sets=%u", unPid, (unsigned) victims.size());
      }
    });
    unlock();
  }

  void direct_mode_t::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*pIndices)[2]) {
    if (degraded() || !pIndices) {
      return;
    }
    ctx_.ipc->status().next_index_calls.fetch_add(1, std::memory_order_relaxed);
    lock();
    guarded("GetNextSwapTextureSetIndex", [&]() {
      for (int e = 0; e < 2; ++e) {
        uint32_t idx = 0;
        if (find_locked(handles[e], idx)) {
          (*pIndices)[e] = (idx + 1) % 3;
        } else {
          (*pIndices)[e] = 0;  // §REVIEW-1103（B035 可能-1）：不是我們的 handle 也要給確定值，不留呼叫端的未初始化內容
        }
      }
    });
    unlock();
    push_timing(ctx_, VRIPC_TE_NEXT_INDEX, (uint32_t) frame_id_, 0, 0);
  }

  void direct_mode_t::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    if (degraded()) {
      return;
    }
    const int64_t now = now_qpc();
    uint32_t index_for_timing = 0;
    int64_t pred_us = 0;
    lock();
    guarded("SubmitLayer", [&]() {
      uint32_t i0 = 0, i1 = 0;
      const bool known = find_locked(perEye[0].hTexture, i0) != nullptr && find_locked(perEye[1].hTexture, i1) != nullptr;
      if (layer_count_ >= k_max_layers || !known) {
        ctx_.ipc->status().drop_unknown_layer.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      layer_t &L = layers_[layer_count_];
      for (int e = 0; e < 2; ++e) {
        L.tex[e] = perEye[e].hTexture;
        L.bounds[e][0] = perEye[e].bounds.uMin;
        L.bounds[e][1] = perEye[e].bounds.vMin;
        L.bounds[e][2] = perEye[e].bounds.uMax;
        L.bounds[e][3] = perEye[e].bounds.vMax;
        std::memcpy(L.proj[e], perEye[e].mProjection.m, sizeof(L.proj[e]));
      }
      std::memcpy(L.pose, perEye[0].mHmdPose.m, sizeof(L.pose));
      L.pred_s = perEye[0].flHmdPosePredictionTimeInSecondsFromNow;
      L.submit_qpc = now;
      index_for_timing = layer_count_;
      pred_us = std::isfinite(L.pred_s) ? (int64_t) (L.pred_s * 1e6) : 0;
      ++layer_count_;
    });
    unlock();
    push_timing(ctx_, VRIPC_TE_SUBMIT_LAYER, (uint32_t) frame_id_, index_for_timing, pred_us);
  }

  // ── Present ─────────────────────────────────────────────────────────

  IDXGIKeyedMutex *direct_mode_t::sync_mutex_locked(uint64_t handle) {
    if (handle == 0 || FAILED(ensure_device_locked(nullptr))) {
      return nullptr;
    }
    if (handle != sync_handle_) {
      sync_handle_ = handle;
      sync_tex_.Reset();
      sync_mutex_.Reset();
      if (SUCCEEDED(device_.open_legacy(handle, sync_tex_))) {
        if (FAILED(sync_tex_.As(&sync_mutex_)) && !sync_no_mutex_logged_) {
          sync_no_mutex_logged_ = true;
          VRDRV_LOG_WARN("sync-texture keyed-mutex=absent (U13)");
        }
      } else {
        VRDRV_LOG_WARN("sync-texture open-failed");
      }
    }
    return sync_mutex_.Get();
  }

  // 注視點編碼（§VR-FOVEA／§VR-FOVEA-INSET／§VR-GAZE-FOVEA）與分塊（§VR-FOVEA-TILES）的設定。Activate 與每個
  // generation 的 TEXTURES 都呼叫：這些只影響我們自己怎麼合成，SteamVR 不知道，所以不必等 SteamVR 重開。
  // 2026-11-10 之前只在 Activate 載入一次——SteamVR 沒關、下一場換了注視點設定時 driver 還照舊的畫。
  void direct_mode_t::load_fovea_config_locked(const vripc_session_config_t &cfg) {
    fovea_a_ = cfg.fovea_pct > 100 && cfg.fovea_pct <= 250 ? 100.0f / (float) cfg.fovea_pct : 0.0f;
    fovea_u0_ = fovea_a_ > 0.0f && cfg.fovea_inset_u0 >= 1000 && cfg.fovea_inset_u0 <= 9000 ? (float) cfg.fovea_inset_u0 / 10000.0f : 0.0f;
    fovea_flags_ = cfg.fovea_flags;
    fovea_slew_dps_ = cfg.fovea_slew_dps ? cfg.fovea_slew_dps : 200;
    fovea_max_mdeg_ = cfg.fovea_max_mdeg ? cfg.fovea_max_mdeg : 20000;
    fovea_deadband_mdeg_ = cfg.fovea_deadband_mdeg ? cfg.fovea_deadband_mdeg : 1500;
    fovea_centre_init_ = false;
    saccade_frozen_ = false;
    low_vel_samples_ = 0;
    // ipc_proto 的 check_config 已經驗過尺寸關係；這裡再擋一次不合理的值（當成不分塊）
    const bool tiles_ok = cfg.tile_w >= 64 && cfg.tile_h >= 64 && cfg.tile_render_w > cfg.tile_w && cfg.tile_render_h > cfg.tile_h;
    const bool changed = (tiles_ok ? cfg.tile_w : 0u) != tile_w_ || (tiles_ok ? cfg.tile_h : 0u) != tile_h_ ||
                         (tiles_ok ? cfg.tile_render_w : 0u) != tile_render_w_ || (tiles_ok ? cfg.tile_render_h : 0u) != tile_render_h_;
    tile_w_ = tiles_ok ? cfg.tile_w : 0;
    tile_h_ = tiles_ok ? cfg.tile_h : 0;
    tile_render_w_ = tiles_ok ? cfg.tile_render_w : 0;
    tile_render_h_ = tiles_ok ? cfg.tile_render_h : 0;
    tile_gaze_tx_ = 0.0f;
    tile_gaze_ty_ = 0.0f;
    tile_gaze_init_ = false;
    tile_last_valid_qpc_ = 0;
    if (tiles_ok && changed) {
      VRDRV_LOG_INFO("[VIPLE-VR-FOVEA] tiles: %ux%u per tile (ring %ux%u), gaze tile = native crop of the %ux%u render, gaze=%d deadband=%u mdeg",
                     (unsigned) cfg.tile_w, (unsigned) cfg.tile_h, (unsigned) cfg.tile_w * 2u, (unsigned) cfg.tile_h * 2u, (unsigned) cfg.tile_render_w,
                     (unsigned) cfg.tile_render_h, (cfg.fovea_flags & VRIPC_FOVEA_F_GAZE) ? 1 : 0, (unsigned) fovea_deadband_mdeg_);
    } else if (!tiles_ok && changed) {
      VRDRV_LOG_INFO("[VIPLE-VR-FOVEA] tiles off (fovea a=%.3f u0=%.3f)", fovea_a_, fovea_u0_);
    }
  }

  // §VR-FOVEA-TILES（2026-11-10）：注視點那塊的左上角（算圖像素，兩眼各自）。
  //  - 視線有效：新的視線方向和「已採用的」差超過死區才採用，一次移到位——不限速、掃視途中也跟。
  //    11-10 配戴（分段曲線）：掃視凍結（>250°/s）＋每秒最多 200° 讓一次 20° 的掃視要約 100 ms 才移到，
  //    使用者看得到邊緣先糊再清楚。分塊之後移動注視點那塊對編碼器只是平移、全視野那塊完全不動，不必再限速。
  //  - 視線中斷不到 0.5 秒（眨眼）：留在原位；更久（沒戴、追蹤掉了）回到正前方。
  //  - 沒有協商眼動：固定在正前方。
  // 位置對齊整數像素、夾在影像內。取「最新」的追蹤樣本（視線不需要對到算繪時刻，越新越好）。
  void direct_mode_t::tile_origin_locked(int64_t present_qpc, uint16_t org[2][2]) {
    bool valid = false;
    float tx = 0.0f, ty = 0.0f;
    if ((fovea_flags_ & VRIPC_FOVEA_F_GAZE) != 0 && gen_) {
      vripc_tracking_slot_t trk_slot {};
      uint64_t trk_idx = 0;
      bool torn = false;
      // 目標時刻給到一秒後＝取目標時刻最晚的那一筆（最新的樣本）
      const bool read_ok = gen_->read_tracking_closest_target(present_qpc + ctx_.qpf, trk_slot, trk_idx, &torn);
      if (read_ok && (trk_slot.flags & VRIPC_TRK_F_GAZE) != 0 && trk_slot.gaze_conf > 0 && trk_slot.gaze_sample_age_ms < 250) {
        const float yaw = half_to_float(trk_slot.gaze_yaw_f16);
        const float pitch = half_to_float(trk_slot.gaze_pitch_f16);
        // 不是有限值、或超過 ±85°（tan 爆掉）就當這次沒有（§REVIEW-1103 B035-1）
        if (std::isfinite(yaw) && std::isfinite(pitch) && std::fabs(yaw) <= 1.48f && std::fabs(pitch) <= 1.48f) {
          tx = std::tan(yaw);
          ty = std::tan(pitch);
          valid = true;
        }
      }
      if (valid) {
        ++stat_gaze_valid_;
        tile_last_valid_qpc_ = present_qpc;
      } else {
        ++stat_gaze_stale_;
      }
    }
    if (valid) {
      // 兩個方向 (tx, ty, 1) 的夾角
      const float ax = tile_gaze_tx_, ay = tile_gaze_ty_;
      const float dot = (ax * tx + ay * ty + 1.0f) / std::sqrt((ax * ax + ay * ay + 1.0f) * (tx * tx + ty * ty + 1.0f));
      const float deg = std::acos(dot > 1.0f ? 1.0f : dot < -1.0f ? -1.0f : dot) * (180.0f / 3.14159265358979323846f);
      if (!tile_gaze_init_ || deg * 1000.0f >= (float) fovea_deadband_mdeg_) {
        tile_gaze_tx_ = tx;
        tile_gaze_ty_ = ty;
        if (tile_gaze_init_) {
          ++stat_tile_moves_;
        }
        tile_gaze_init_ = true;
      }
    } else if (tile_gaze_init_ && (tile_last_valid_qpc_ == 0 || present_qpc - tile_last_valid_qpc_ > ctx_.qpf / 2)) {
      tile_gaze_tx_ = 0.0f;
      tile_gaze_ty_ = 0.0f;
      tile_gaze_init_ = false;
    }
    for (int e = 0; e < 2; ++e) {
      const float ew = eye_rect_[e].right - eye_rect_[e].left;
      const float eh = eye_rect_[e].bottom - eye_rect_[e].top;
      // 視線在本眼影像裡的位置（正規化；y 由上緣算，rect 的 bottom 是上緣的 tangent）
      const float cx = ew > 1e-6f ? (tile_gaze_tx_ - eye_rect_[e].left) / ew : 0.5f;
      const float cy = eh > 1e-6f ? (eye_rect_[e].bottom - tile_gaze_ty_) / eh : 0.5f;
      const long max_x = (long) tile_render_w_ - (long) tile_w_;
      const long max_y = (long) tile_render_h_ - (long) tile_h_;
      long ox = std::lround(cx * (float) tile_render_w_ - 0.5f * (float) tile_w_);
      long oy = std::lround(cy * (float) tile_render_h_ - 0.5f * (float) tile_h_);
      if (ox < 0 || ox > max_x || oy < 0 || oy > max_y) {
        if (valid) {
          ++stat_gaze_clamped_;
        }
        ox = ox < 0 ? 0 : ox > max_x ? max_x : ox;
        oy = oy < 0 ? 0 : oy > max_y ? max_y : oy;
      }
      org[e][0] = (uint16_t) ox;
      org[e][1] = (uint16_t) oy;
      tile_last_org_[e][0] = org[e][0];
      tile_last_org_[e][1] = org[e][1];
    }
    if (stat_gaze_last_log_ == 0 || present_qpc - stat_gaze_last_log_ >= (int64_t) (10 * ctx_.qpf)) {
      VRDRV_LOG_INFO("[VIPLE-VR-GAZE] host: tiles originL=(%u,%u) originR=(%u,%u) of %ux%u valid=%llu stale=%llu moves=%llu clamped=%llu",
                     (unsigned) org[0][0], (unsigned) org[0][1], (unsigned) org[1][0], (unsigned) org[1][1], (unsigned) tile_render_w_, (unsigned) tile_render_h_,
                     (unsigned long long) stat_gaze_valid_, (unsigned long long) stat_gaze_stale_, (unsigned long long) stat_tile_moves_,
                     (unsigned long long) stat_gaze_clamped_);
      stat_gaze_last_log_ = present_qpc;
      stat_gaze_valid_ = 0;
      stat_gaze_stale_ = 0;
      stat_tile_moves_ = 0;
      stat_gaze_clamped_ = 0;
    }
  }

  void direct_mode_t::compose_and_publish_locked(int64_t present_qpc, uint32_t &exit_code) {
    last_presented_ = false;
    auto &st = ctx_.ipc->status();
    if (!ring_ || !gen_ || !ctx_.active() || comp_failed_) {
      exit_code = 4;
      ++stat_no_target_;
      return;
    }
    ring_t &R = *ring_;
    const uint64_t consumed = R.consumed_fence->GetCompletedValue();
    if (consumed == UINT64_MAX) {
      // §B.6 OPERATIONAL 的 device lost 路徑
      const HRESULT rr = device_.removed_reason();
      VRDRV_LOG_WARN("consumed-fence-lost gen=%llu selfRemoved=0x%08x", (unsigned long long) R.generation, (unsigned) rr);
      ctx_.ipc->report_device_lost(FAILED(rr) ? (uint32_t) rr : 0x887A0005u);
      ring_.reset();
      exit_code = 4;
      return;
    }
    int slot = -1;
    for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
      if (consumed >= R.last_fence_of[i]) {
        slot = (int) i;
        break;
      }
    }
    if (slot < 0) {
      st.drop_noslot.fetch_add(1, std::memory_order_relaxed);
      ++stat_noslot_;
      exit_code = 2;
      return;
    }

    // layer → compositor 輸入
    compose_layer_t in[k_max_layers];
    uint32_t n = 0;
    bool format_drop = false;
    for (uint32_t i = 0; i < layer_count_; ++i) {
      compose_layer_t &c = in[n];
      bool any = false;
      for (int e = 0; e < 2; ++e) {
        uint32_t idx = 0;
        const swapset_t *s = find_locked(layers_[i].tex[e], idx);
        c.eye[e] = compose_eye_t {};
        if (!s) {
          continue;
        }
        if (!s->composable) {
          format_drop = true;
          continue;
        }
        c.eye[e].srv = s->srv[idx].Get();
        c.eye[e].mode = s->mode;
        c.eye[e].force_alpha = s->force_alpha;
        std::memcpy(c.eye[e].bounds, layers_[i].bounds[e], sizeof(c.eye[e].bounds));
        std::memcpy(c.eye[e].proj, layers_[i].proj[e], sizeof(c.eye[e].proj));
        any = true;
      }
      if (any) {
        ++n;
      }
    }
    if (format_drop) {
      st.drop_format.fetch_add(1, std::memory_order_relaxed);
    }

    int64_t t_target_qpc = present_qpc;
    if (layer_count_ > 0) {
      const float pred = std::isfinite(layers_[0].pred_s) ? layers_[0].pred_s : 0.0f;
      t_target_qpc = layers_[0].submit_qpc + (int64_t) ((double) pred * (double) ctx_.qpf);
    }

    // §VR-FOVEA-TILES：分塊時注視點那塊的左上角（算圖像素）；下面的曲線中心那一段不會跑（fovea_a_＝0）
    const bool tiles_active = tile_w_ > 0;
    uint16_t tile_org[2][2] = {{0, 0}, {0, 0}};
    if (tiles_active) {
      tile_origin_locked(present_qpc, tile_org);
    }

    const bool fovea_active = (fovea_a_ >= 0.4f && fovea_a_ < 0.999f) && ((fovea_flags_ & VRIPC_FOVEA_F_GAZE) != 0);
    float fovea_out_centre[2][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}};
    if (fovea_active) {
      float optical_axis[2][2];
      for (int e = 0; e < 2; ++e) {
        const float ew = eye_rect_[e].right - eye_rect_[e].left;
        const float eh = eye_rect_[e].bottom - eye_rect_[e].top;
        optical_axis[e][0] = (ew > 1e-6f) ? (-eye_rect_[e].left / ew) : 0.5f;
        optical_axis[e][1] = (eh > 1e-6f) ? (eye_rect_[e].bottom / eh) : 0.5f;
      }

      vripc_tracking_slot_t trk_slot {};
      uint64_t trk_idx = 0;
      bool torn = false;
      const bool read_ok = gen_->read_tracking_closest_target(t_target_qpc, trk_slot, trk_idx, &torn);

      bool gaze_valid = read_ok &&
                        ((trk_slot.flags & VRIPC_TRK_F_GAZE) != 0) &&
                        trk_slot.gaze_conf > 0 &&
                        trk_slot.gaze_sample_age_ms < 250;

      float target_px[2] = {optical_axis[0][0], optical_axis[1][0]};
      float target_py[2] = {optical_axis[0][1], optical_axis[1][1]};

      float yaw = 0.0f;
      float pitch = 0.0f;
      if (gaze_valid) {
        yaw = half_to_float(trk_slot.gaze_yaw_f16);
        pitch = half_to_float(trk_slot.gaze_pitch_f16);
        // §REVIEW-1103（B035-1）：f16 的 NaN／Inf 會一路變成 NaN 的注視點；第一幀初始化時寫進中心就永遠是 NaN。
        // 不是有限值、或超過 ±85°（tan 爆掉）就當這次沒有 gaze。
        if (!std::isfinite(yaw) || !std::isfinite(pitch) || std::fabs(yaw) > 1.48f || std::fabs(pitch) > 1.48f) {
          gaze_valid = false;
          yaw = 0.0f;
          pitch = 0.0f;
        }
      }
      if (gaze_valid) {
        ++stat_gaze_valid_;

        const float tx = std::tan(yaw);
        const float ty = std::tan(pitch);

        for (int e = 0; e < 2; ++e) {
          const float ew = eye_rect_[e].right - eye_rect_[e].left;
          const float eh = eye_rect_[e].bottom - eye_rect_[e].top;
          if (ew > 1e-6f && eh > 1e-6f) {
            const float raw_x = (tx - eye_rect_[e].left) / ew;
            const float raw_y = (eye_rect_[e].bottom - ty) / eh;
            const float clamped_x = std::clamp(raw_x, 0.15f, 0.85f);
            const float clamped_y = std::clamp(raw_y, 0.15f, 0.85f);
            if (clamped_x != raw_x || clamped_y != raw_y) {
              ++stat_gaze_clamped_;
            }
            target_px[e] = clamped_x;
            target_py[e] = clamped_y;
          }
        }
      } else {
        ++stat_gaze_stale_;
      }

      // Saccade 狀態機：速度 > 250°/s 凍結，< 50°/s 連續 2 樣本解凍
      const double dt_s = (last_fovea_qpc_ > 0 && present_qpc > last_fovea_qpc_)
                            ? ((double) (present_qpc - last_fovea_qpc_) / (double) ctx_.qpf)
                            : (1.0 / 90.0);
      if (gaze_valid && last_gaze_sample_qpc_ > 0) {
        const float d_yaw = yaw - last_gaze_yaw_[0];
        const float d_pitch = pitch - last_gaze_pitch_[0];
        const float ang_dist_deg = std::sqrt(d_yaw * d_yaw + d_pitch * d_pitch) * (180.0f / 3.14159265358979323846f);
        const float ang_vel_dps = (dt_s > 1e-4) ? (ang_dist_deg / (float) dt_s) : 0.0f;

        if (ang_vel_dps > 250.0f) {
          saccade_frozen_ = true;
          low_vel_samples_ = 0;
          ++stat_gaze_frozen_;
        } else if (saccade_frozen_) {
          if (ang_vel_dps < 50.0f) {
            ++low_vel_samples_;
            if (low_vel_samples_ >= 2) {
              saccade_frozen_ = false;
            }
          } else {
            low_vel_samples_ = 0;
          }
        }
      } else if (!gaze_valid) {
        saccade_frozen_ = false;
        low_vel_samples_ = 0;
        // §REVIEW-1103（B035-2）：中斷（眨眼）期間不清的話，恢復第一幀會拿「中斷前角度→現在角度」除以單幀 dt，
        // 眼睛在中斷期間轉 > ~3° 就被算成 > 250°/s → 誤判 saccade、注視點多卡在光軸約 2 幀。恢復後第一個樣本只當基準。
        last_gaze_sample_qpc_ = 0;
      }

      if (gaze_valid) {
        last_gaze_yaw_[0] = yaw;
        last_gaze_pitch_[0] = pitch;
        last_gaze_sample_qpc_ = present_qpc;
      }

      if (saccade_frozen_) {
        target_px[0] = pre_saccade_target_[0][0];
        target_py[0] = pre_saccade_target_[0][1];
        target_px[1] = pre_saccade_target_[1][0];
        target_py[1] = pre_saccade_target_[1][1];
      } else {
        pre_saccade_target_[0][0] = target_px[0];
        pre_saccade_target_[0][1] = target_py[0];
        pre_saccade_target_[1][0] = target_px[1];
        pre_saccade_target_[1][1] = target_py[1];
      }

      // 初始化第一幀中心
      if (!fovea_centre_init_) {
        current_fovea_centre_[0][0] = target_px[0];
        current_fovea_centre_[0][1] = target_py[0];
        current_fovea_centre_[1][0] = target_px[1];
        current_fovea_centre_[1][1] = target_py[1];
        fovea_centre_init_ = true;
      } else {
        // 死區與 Slew rate 限制（每眼各自計算）
        const float deadband_deg = (float) fovea_deadband_mdeg_ / 1000.0f;
        const float max_step_deg = (float) fovea_slew_dps_ * (float) dt_s;

        for (int e = 0; e < 2; ++e) {
          const float ew = eye_rect_[e].right - eye_rect_[e].left;
          const float eh = eye_rect_[e].bottom - eye_rect_[e].top;
          const float dcx = target_px[e] - current_fovea_centre_[e][0];
          const float dcy = target_py[e] - current_fovea_centre_[e][1];
          const float dtx = dcx * ew;
          const float dty = dcy * eh;
          const float diff_rad = std::sqrt(dtx * dtx + dty * dty);
          const float diff_deg = diff_rad * (180.0f / 3.14159265358979323846f);

          if (diff_deg >= deadband_deg) {
            if (diff_deg > max_step_deg && diff_deg > 1e-4f) {
              const float frac = max_step_deg / diff_deg;
              current_fovea_centre_[e][0] += dcx * frac;
              current_fovea_centre_[e][1] += dcy * frac;
            } else {
              current_fovea_centre_[e][0] = target_px[e];
              current_fovea_centre_[e][1] = target_py[e];
            }
          }
          current_fovea_centre_[e][0] = std::clamp(current_fovea_centre_[e][0], 0.15f, 0.85f);
          current_fovea_centre_[e][1] = std::clamp(current_fovea_centre_[e][1], 0.15f, 0.85f);
        }
      }

      last_fovea_qpc_ = present_qpc;
      std::memcpy(fovea_out_centre, current_fovea_centre_, sizeof(fovea_out_centre));

      // 10 秒統計日誌 [VIPLE-VR-GAZE]
      if (stat_gaze_last_log_ == 0 || present_qpc - stat_gaze_last_log_ >= (int64_t) (10 * ctx_.qpf)) {
        VRDRV_LOG_INFO("[VIPLE-VR-GAZE] host: centreL=(%.3f,%.3f) centreR=(%.3f,%.3f) valid=%llu stale=%llu frozen=%llu clamped=%llu",
                       current_fovea_centre_[0][0], current_fovea_centre_[0][1],
                       current_fovea_centre_[1][0], current_fovea_centre_[1][1],
                       (unsigned long long) stat_gaze_valid_, (unsigned long long) stat_gaze_stale_,
                       (unsigned long long) stat_gaze_frozen_, (unsigned long long) stat_gaze_clamped_);
        stat_gaze_last_log_ = present_qpc;
        stat_gaze_valid_ = 0;
        stat_gaze_stale_ = 0;
        stat_gaze_frozen_ = 0;
        stat_gaze_clamped_ = 0;
      }
    }

    auto *ctx = device_.ctx();
    if (tiles_active) {
      tile_layout_t tl;
      tl.tile_w = tile_w_;
      tl.tile_h = tile_h_;
      for (int e = 0; e < 2; ++e) {
        tl.origin[e][0] = (float) tile_org[e][0] / (float) tile_render_w_;
        tl.origin[e][1] = (float) tile_org[e][1] / (float) tile_render_h_;
      }
      tl.cover[0] = (float) tile_w_ / (float) tile_render_w_;
      tl.cover[1] = (float) tile_h_ / (float) tile_render_h_;
      comp_.compose(ctx, R.rtv[slot].Get(), R.width / 2, R.height, eye_rect_, in, n, 0.0f, nullptr, 0.0f, &tl);
    } else {
      comp_.compose(ctx, R.rtv[slot].Get(), R.width / 2, R.height, eye_rect_, in, n, fovea_a_, fovea_active ? fovea_out_centre : nullptr, fovea_u0_);
    }
    const uint64_t value = ++R.fence_n;
    HRESULT hr = device_.ctx4()->Signal(R.shared_fence.Get(), value);
    ctx->Flush();  // 跨行程 CPU 觀察需要（§B.7）
    if (FAILED(hr)) {
      const HRESULT rr = device_.removed_reason();
      VRDRV_LOG_ERROR("signal-failed hr=0x%08x removed=0x%08x", (unsigned) hr, (unsigned) rr);
      if (FAILED(rr)) {
        ctx_.ipc->set_device_state(VRIPC_DRV_DEVICE_LOST);
        ctx_.ipc->report_device_lost((uint32_t) rr);
        ring_.reset();
      }
      exit_code = 4;
      return;
    }
    const int64_t submit_qpc = now_qpc();
    R.last_fence_of[slot] = value;

    vripc_frame_desc_t d {};
    d.frame_id = ++frame_id_;
    d.generation = R.generation;
    d.fence_value = value;
    d.tex_idx = (uint32_t) slot;
    d.space_epoch = R.space_epoch;
    d.layout_epoch = R.layout_epoch;
    d.layer_count = layer_count_;
    d.present_qpc = present_qpc;
    d.submit_qpc = submit_qpc;
    d.render_rot[3] = 1.0f;
    if (tiles_active) {
      // §VR-FOVEA-TILES：這四欄改放左上角的像素座標。server 乘 65535 四捨五入成 u16 上線，值不變（≤ 65535 的整數）
      for (int e = 0; e < 2; ++e) {
        d.fovea_centre[e][0] = (float) tile_org[e][0] / 65535.0f;
        d.fovea_centre[e][1] = (float) tile_org[e][1] / 65535.0f;
      }
      d.flags |= VRIPC_FRM_FOVEATED;
    } else {
      std::memcpy(d.fovea_centre, fovea_out_centre, sizeof(d.fovea_centre));
      if (fovea_active) {
        d.flags |= VRIPC_FRM_FOVEATED;
      }
    }
    if (layer_count_ > 0) {
      // 第 0 層＝scene：mHmdPose 是 SteamVR 世界空間；renderPose(client 空間) = Δ_cur⁻¹ · mHmdPose（S2-06、§E.2）
      ph_pose_t app;
      math::matrix34_to_pose(layers_[0].pose, app.rot, app.pos);
      d.t_target_qpc = t_target_qpc;
      const ph_match_t m = ctx_.pose_hist.match(app, d.t_target_qpc, present_qpc);
      d.render_rot[0] = (float) m.client_pose.rot.x;
      d.render_rot[1] = (float) m.client_pose.rot.y;
      d.render_rot[2] = (float) m.client_pose.rot.z;
      d.render_rot[3] = (float) m.client_pose.rot.w;
      d.render_pos[0] = (float) m.client_pose.pos.x;
      d.render_pos[1] = (float) m.client_pose.pos.y;
      d.render_pos[2] = (float) m.client_pose.pos.z;
      d.flags |= VRIPC_FRM_POSE_VALID;
      if (m.hit) {
        d.flags |= VRIPC_FRM_ECHO_MATCHED;
        d.echo_sample_id = m.echo;
      } else {
        d.flags |= VRIPC_FRM_POSE_FALLBACK;
        d.echo_sample_id = 0;  // H5 修正：FALLBACK 時 echo=0（明確表示沒有回聲），避免污染 client echoAge 統計
        st.posehist_miss.fetch_add(1, std::memory_order_relaxed);
      }
      if (m.delta_nonidentity) {
        d.flags |= VRIPC_FRM_SPACE_DELTA;
      }
      if (m.adopted) {
        double yaw = 0, pitch = 0, roll = 0;
        pose_history_t::to_euler_deg(m.adopted_delta.rot, yaw, pitch, roll);
        const double ang = math::angle_deg(m.adopted_delta.rot, math::quat_t {});
        st.space_delta_mdeg.store((int32_t) (ang * 1000.0), std::memory_order_relaxed);
        VRDRV_LOG_INFO("space-delta=%.2f/%.2f/%.2fdeg pos=%.1f,%.1f,%.1fmm trigger=%s", yaw, pitch, roll, m.adopted_delta.pos.x * 1000.0,
                       m.adopted_delta.pos.y * 1000.0, m.adopted_delta.pos.z * 1000.0, m.adopted_trigger.c_str());
      }
    } else {
      d.t_target_qpc = t_target_qpc;
      d.flags |= VRIPC_FRM_POSE_FALLBACK;
    }
    gen_->publish_frame(d);
    st.frames_composited.fetch_add(1, std::memory_order_relaxed);
    ++stat_composed_;
    last_presented_ = true;
    exit_code = 0;
    push_timing(ctx_, VRIPC_TE_COMPOSE_DONE, (uint32_t) d.frame_id, slot, (int64_t) value);
    if (!ctx_.presenting.exchange(true)) {
      ctx_.ipc->set_device_state(VRIPC_DRV_HMD_PRESENTING);
      ctx_.ipc->send_state(VRIPC_ST_HMD_PRESENTING, 0);
      VRDRV_LOG_INFO("hmd presenting gen=%llu frame=%llu layers=%u", (unsigned long long) R.generation, (unsigned long long) d.frame_id, layer_count_);
    }
  }

  void direct_mode_t::Present(vr::SharedTextureHandle_t syncTexture) {
    if (degraded()) {
      return;
    }
    const int64_t present_qpc = now_qpc();
    auto &st = ctx_.ipc->status();
    st.frames_presented.fetch_add(1, std::memory_order_relaxed);
    push_timing(ctx_, VRIPC_TE_PRESENT_ENTER, (uint32_t) frame_id_, 0, 0);
    uint32_t exit_code = 0;
    uint32_t layers = 0;

    lock();
    ++stat_presents_;
    if (stat_last_present_ != 0) {
      const double ms = (double) (present_qpc - stat_last_present_) * 1000.0 / (double) ctx_.qpf;
      stat_interval_max_ms_ = std::max(stat_interval_max_ms_, ms);
    }
    stat_last_present_ = present_qpc;
    layers = layer_count_;
    st.last_layer_count.store(layers, std::memory_order_relaxed);
    if (layers > st.max_layer_count.load(std::memory_order_relaxed)) {
      st.max_layer_count.store(layers, std::memory_order_relaxed);
    }

    IDXGIKeyedMutex *km = nullptr;
    guarded("Present.sync", [&]() {
      km = sync_mutex_locked(syncTexture);
    });
    bool acquired = false;
    if (km) {
      // §VR-ACQ-WAIT（2026-11-05）：這把鎖要等 vrcompositor 在 GPU 上的合成做完才放；重的遊戲（ACC）GPU 排在前面，
      // 放鎖常在 Present 之後 15～20 ms。舊的 5 ms 逾時讓每一張都被丟掉（composed=0、acqTimeout=全部＝頭盔定格）。
      // 改成最多等 k_acquire_wait_ms，等太久才放棄；超過 5 ms 才拿到的另外計數（acqSlow）。
      const int64_t acq_t0 = now_qpc();
      const HRESULT hr = km->AcquireSync(0, k_acquire_wait_ms);
      if (hr == S_OK) {
        acquired = true;
        ++sync_ok_;
        if (now_qpc() - acq_t0 > ctx_.qpf * 5 / 1000) {
          ++stat_acq_slow_;
        }
      } else {
        // WAIT_TIMEOUT 是成功碼（0x102），不能用 SUCCEEDED 判斷
        st.drop_acquire_timeout.fetch_add(1, std::memory_order_relaxed);
        ++sync_timeouts_;
        ++stat_acq_timeout_;
        exit_code = 1;
      }
    }
    if (!degraded() && (acquired || !km)) {
      // 內層只做合成；攔到例外時外層照樣 ReleaseSync(0)，vrcompositor 不會卡在 sync texture
      guarded("Present", [&]() {
        compose_and_publish_locked(present_qpc, exit_code);
      });
    }
    if (acquired) {
      const HRESULT rhr = km->ReleaseSync(0);
      // keyed mutex 的 release 排在 immediate context 的命令流裡：沒有合成（noslot、standby）時本幀沒有其他 Flush，
      // release 會一直留在本行程，vrcompositor 的 AcquireSync 就逾時（V4 host 實測：standby 時 compositor 掉到 10 fps、
      // vrcompositor.txt 出現 "AcquireSync FAILED with WAIT_TIMEOUT"）。一律 Flush。
      device_.ctx()->Flush();
      if (FAILED(rhr)) {
        ++sync_release_failed_;
      }
    }
    if (layer_count_ > 0) {
      stat_last_l0_ = layers_[0];
      stat_have_l0_ = true;
    }
    layer_count_ = 0;

    // 10 秒統計
    if (stat_t0_ == 0) {
      stat_t0_ = present_qpc;
    } else if (present_qpc - stat_t0_ >= 10 * ctx_.qpf) {
      VRDRV_LOG_INFO("timing 10s: presents=%llu composed=%llu noslot=%llu acqTimeout=%llu acqSlow=%llu noTarget=%llu missed=%llu intervalMaxMs=%.2f layersMax=%u swapsets=%u keyedMutex=%s syncOk=%llu releaseFailed=%llu",
                     (unsigned long long) stat_presents_, (unsigned long long) stat_composed_, (unsigned long long) stat_noslot_, (unsigned long long) stat_acq_timeout_, (unsigned long long) stat_acq_slow_,
                     (unsigned long long) stat_no_target_, (unsigned long long) stat_missed_, stat_interval_max_ms_, st.max_layer_count.load(),
                     (unsigned) sets_.size(), sync_mutex_ ? "yes" : (sync_handle_ ? "absent" : "none"), (unsigned long long) sync_ok_, (unsigned long long) sync_release_failed_);
      if (stat_have_l0_) {
        const auto &L = stat_last_l0_;
        // 第 0 層的貼圖來源（哪個行程的 swap set、尺寸）：分辨是 app 的貼圖還是 vrcompositor 自己重畫的
        for (int e = 0; e < 2; ++e) {
          uint32_t idx = 0;
          const swapset_t *src = find_locked(L.tex[e], idx);
          VRDRV_LOG_INFO("layer0 eye=%d src pid=%u size=%ux%u fmt=%u idx=%u pose=%.4f,%.4f,%.4f", e, src ? src->pid : 0, src ? src->width : 0, src ? src->height : 0, src ? (unsigned) src->format : 0, idx,
                         L.pose[0][3], L.pose[1][3], L.pose[2][3]);
        }
        for (int e = 0; e < 2; ++e) {
          VRDRV_LOG_INFO("layer0 eye=%d proj m00=%.4f m02=%.4f m11=%.4f m12=%.4f m20=%.4f m21=%.4f m32=%.4f bounds=%.3f,%.3f,%.3f,%.3f eyeRect=%.3f,%.3f,%.3f,%.3f", e, L.proj[e][0][0],
                         L.proj[e][0][2], L.proj[e][1][1], L.proj[e][1][2], L.proj[e][2][0], L.proj[e][2][1], L.proj[e][3][2], L.bounds[e][0], L.bounds[e][1], L.bounds[e][2], L.bounds[e][3],
                         eye_rect_[e].left, eye_rect_[e].right, eye_rect_[e].top, eye_rect_[e].bottom);
        }
      }
      stat_t0_ = present_qpc;
      stat_presents_ = stat_composed_ = stat_noslot_ = stat_acq_timeout_ = stat_acq_slow_ = stat_no_target_ = stat_missed_ = 0;
      stat_interval_max_ms_ = 0.0;
    }
    unlock();
    push_timing(ctx_, VRIPC_TE_PRESENT_EXIT, (uint32_t) frame_id_, exit_code, layers);
  }

  void direct_mode_t::PostPresent(const Throttling_t *pThrottling) {
    if (degraded() || !vsync_.active()) {
      return;
    }
    const uint32_t thr = pThrottling ? pThrottling->nFramesToThrottle : 0;
    const uint32_t pred = pThrottling ? pThrottling->nAdditionalFramesToPredict : 0;
    push_timing(ctx_, VRIPC_TE_POSTPRESENT_ENTER, (uint32_t) frame_id_, thr, pred);
    uint32_t slept = 0;
    guarded("PostPresent", [&]() {
      const int64_t now = now_qpc();
      auto gen = ctx_.ipc->current();
      vripc_pacing_t pc {};
      const bool ok = gen && gen->read_pacing(pc);
      const auto r = vsync_.step(ok ? &pc : nullptr, gen && gen->dev_pacing(), now, thr);
      last_missed_ = r.missed;
      if (r.missed > 1) {
        stat_missed_ += r.missed - 1;
      }
      ctx_.ipc->status().last_vsync_qpc.store(r.vsync_qpc, std::memory_order_relaxed);
      push_timing(ctx_, VRIPC_TE_VSYNC_VIRTUAL, (uint32_t) frame_id_, (int64_t) r.period_ns, r.missed);
      if (r.pacing_rejected && now - last_reject_log_qpc_ >= ctx_.qpf) {
        last_reject_log_qpc_ = now;
        push_timing(ctx_, VRIPC_TE_PACING_REJECTED, (uint32_t) frame_id_, ok ? (int64_t) ((double) pc.period_q32 / 4294967296.0 * 1e9 / (double) ctx_.qpf) : 0, ok ? pc.slew_ppm_max : 0);
      }
      if (r.snapped) {
        push_timing(ctx_, VRIPC_TE_PHASE_SNAP, (uint32_t) frame_id_, r.snap_ticks * 1000000000 / ctx_.qpf, 0);
      }
      if (r.mode == VRIPC_PM_PRODUCTION || r.mode == VRIPC_PM_E3_BOTH) {
        slept = vsync_.wait_until(timer_, r.target_qpc);
      }
    });
    push_timing(ctx_, VRIPC_TE_POSTPRESENT_EXIT, (uint32_t) frame_id_, slept, 0);
  }

  void direct_mode_t::GetFrameTiming(vr::DriverDirectMode_FrameTiming *p) {
    if (!p || p->m_nSize < sizeof(vr::DriverDirectMode_FrameTiming)) {
      return;
    }
    const uint32_t incoming = p->m_nReprojectionFlags;
    // 與 VRCompositor_ThrottleMask 重疊，一定要清（H:2207-2214）
    p->m_nReprojectionFlags = 0;
    if (degraded()) {
      return;
    }
    p->m_nNumFramePresents = last_presented_ ? 1u : 0u;
    p->m_nNumMisPresented = 0;
    p->m_nNumDroppedFrames = last_missed_ > 1 ? last_missed_ - 1 : 0;
    ++frame_timing_calls_;
    push_timing(ctx_, VRIPC_TE_GETFRAMETIMING, (uint32_t) frame_id_, incoming, p->m_nNumDroppedFrames);
  }

  // ── ipc 執行緒 ──────────────────────────────────────────────────────

  textures_result_t direct_mode_t::on_textures(const generation_ptr &gen, const vripc_textures_t &msg, const HANDLE tex[VRIPC_TEX_COUNT], HANDLE shared_fence, HANDLE consumed_fence) {
    textures_result_t r;
    LUID luid {};
    luid.LowPart = msg.adapter_luid_low;
    luid.HighPart = msg.adapter_luid_high;
    lock();
    guarded("on_textures", [&]() {
      if (device_.valid() && !device_.same_luid(luid)) {
        r.reject_reason = VRIPC_REJ_ADAPTER_MISMATCH;
        VRDRV_LOG_WARN("textures gen=%llu result=reject reason=adapter-mismatch (K20: direct-mode device lives until Cleanup)", (unsigned long long) gen->id());
        return;
      }
      // K20：Activate 之後每眼尺寸不可變（SteamVR 的建議算圖尺寸、眼的 viewport 都照它）。ring 的尺寸則看這個
      // generation 的設定——§VR-FOVEA-TILES 分塊時是 (2·tile_w)×(2·tile_h)，同一次 SteamVR 執行期間下一場可以換檔位
      const vripc_session_config_t &gcfg = gen->config();
      if (activated_ && (gcfg.eye_width != eye_w_ || gcfg.eye_height != eye_h_ || msg.width != gcfg.packed_width || msg.height != gcfg.packed_height)) {
        r.reject_reason = VRIPC_REJ_TEXTURE_INVALID;
        VRDRV_LOG_WARN("textures gen=%llu result=reject reason=k20-size ring=%ux%u config eye=%ux%u packed=%ux%u activated eye=%ux%u", (unsigned long long) gen->id(),
                       msg.width, msg.height, gcfg.eye_width, gcfg.eye_height, gcfg.packed_width, gcfg.packed_height, eye_w_, eye_h_);
        return;
      }
      HRESULT hr = ensure_device_locked(&luid);
      if (FAILED(hr)) {
        r.reject_reason = VRIPC_REJ_TEXTURE_INVALID;
        r.reject_detail = (uint32_t) hr;
        return;
      }
      auto ring = std::make_shared<ring_t>();
      const char *why = "";
      hr = device_.open_ring(tex, shared_fence, consumed_fence, msg.width, msg.height, *ring, why);
      if (FAILED(hr)) {
        r.reject_reason = VRIPC_REJ_TEXTURE_INVALID;
        r.reject_detail = (uint32_t) hr;
        VRDRV_LOG_WARN("textures gen=%llu result=reject reason=%s hr=0x%08x", (unsigned long long) gen->id(), why, (unsigned) hr);
        return;
      }
      ring->generation = gen->id();
      ring->space_epoch = gen->config().space_epoch;
      ring->layout_epoch = gen->config().layout_epoch;
      // 注視點／分塊照這個 generation 的設定（Activate 時載入的是 HMD 建立當下那個 generation 的）
      load_fovea_config_locked(gcfg);
      ring_ = ring;
      gen_ = gen;
      frame_id_ = 0;
      r.ok = true;
      r.reject_reason = 0;
    });
    unlock();
    return r;
  }

  void direct_mode_t::on_teardown(uint64_t generation) {
    lock();
    guarded("on_teardown", [&]() {
      if (gen_ && gen_->id() == generation) {
        ring_.reset();
        gen_.reset();
      }
    });
    unlock();
  }

}  // namespace vrdrv
