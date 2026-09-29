// direct_mode.h - VipleStream §VR：IVRDriverDirectModeComponent_009（設計 §E.2 direct_mode、§B.7 fence 交接）。
//
// V4 是獨立撰寫（本機沒有 ALVR OvrDirectModeComponent 原始碼；設計 §E.3 原定移植，改列偏差）。
// 鎖（drv-B-1）：Create／Destroy／DestroyAll／SubmitLayer／Present／輸出目標更換（TEXTURES、TEARDOWN）
//   全部持同一把 present 鎖；PostPresent 的等待不持鎖。
// SEH（drv-M-8）：Present 的 AcquireSync／ReleaseSync 在 guarded() 外層，內層只做合成；其他回呼整個包在 guarded() 內。
// 不變式 8：絕不 GPU-Wait server 的 fence；slot 只在 consumedFence（CPU 讀）≥ lastFenceOf[slot] 時重用。
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <openvr_driver.h>

#include "d3d_device.h"
#include "driver_context.h"
#include "frame_compositor.h"
#include "virtual_vsync.h"

namespace vrdrv {

  class direct_mode_t: public vr::IVRDriverDirectModeComponent {
  public:
    explicit direct_mode_t(driver_ctx_t &ctx);
    ~direct_mode_t();
    direct_mode_t(const direct_mode_t &) = delete;
    direct_mode_t &operator=(const direct_mode_t &) = delete;

    // ── IVRDriverDirectModeComponent_009 ──
    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t *pSwapTextureSetDesc, SwapTextureSet_t *pOutSwapTextureSet) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t (*pIndices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent(const Throttling_t *pThrottling) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming *pFrameTiming) override;

    // ── ipc 執行緒（ipc_client callback）──
    textures_result_t on_textures(const generation_ptr &gen, const vripc_textures_t &msg, const HANDLE tex[VRIPC_TEX_COUNT], HANDLE shared_fence, HANDLE consumed_fence);
    void on_teardown(uint64_t generation);

    // ── HMD Activate（K20：之後不變）──
    void activate(const vripc_session_config_t &cfg, uint32_t eye_w, uint32_t eye_h, const LUID &luid);

    // Cleanup：放掉所有 D3D 物件（ipc 與 tracking 已停）
    void release_all();

  private:
    struct swapset_t {
      uint32_t pid = 0;
      DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
      uint32_t samples = 1;
      uint32_t width = 0;
      uint32_t height = 0;
      ComPtr<ID3D11Texture2D> tex[3];
      ComPtr<ID3D11ShaderResourceView> srv[3];  // 白名單格式才有（其餘合成時計 drop_format）
      uint64_t handle[3] = {0, 0, 0};
      uint32_t mode = 0;
      bool force_alpha = false;
      bool composable = false;
    };

    struct layer_t {
      uint64_t tex[2] = {0, 0};
      float bounds[2][4] = {};
      float proj[2][4][4] = {};
      float pose[3][4] = {};
      float pred_s = 0.0f;
      int64_t submit_qpc = 0;
    };

    static constexpr uint32_t k_max_layers = 16;

    void lock() {
      AcquireSRWLockExclusive(&lock_);
    }

    void unlock() {
      ReleaseSRWLockExclusive(&lock_);
    }

    // 以下都在 present 鎖內呼叫
    HRESULT ensure_device_locked(const LUID *hint);
    void create_set_locked(uint32_t pid, const SwapTextureSetDesc_t &desc, SwapTextureSet_t &out);
    void destroy_set_locked(swapset_t *set);
    const swapset_t *find_locked(uint64_t handle, uint32_t &index) const;
    IDXGIKeyedMutex *sync_mutex_locked(uint64_t handle);
    void compose_and_publish_locked(int64_t present_qpc, uint32_t &exit_code);

    driver_ctx_t &ctx_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    d3d_device_t device_;
    frame_compositor_t comp_;
    bool comp_failed_ = false;

    std::vector<std::unique_ptr<swapset_t>> sets_;
    std::unordered_map<uint64_t, std::pair<swapset_t *, uint32_t>> by_handle_;

    layer_t layers_[k_max_layers];
    uint32_t layer_count_ = 0;

    std::shared_ptr<ring_t> ring_;  // 輸出目標；null = idle／TEARDOWN
    generation_ptr gen_;
    uint64_t frame_id_ = 0;  // 每個 generation 從 1 起

    // sync texture（vrcompositor 的 legacy handle；只開一次）
    uint64_t sync_handle_ = 0;
    ComPtr<ID3D11Texture2D> sync_tex_;
    ComPtr<IDXGIKeyedMutex> sync_mutex_;
    bool sync_no_mutex_logged_ = false;
    uint64_t sync_ok_ = 0;
    uint64_t sync_timeouts_ = 0;
    uint64_t sync_release_failed_ = 0;

    // K20（Activate 之後不變）
    bool activated_ = false;
    uint32_t eye_w_ = 0;
    uint32_t eye_h_ = 0;
    math::rect_t eye_rect_[2];
    LUID act_luid_ {};

    // PostPresent／GetFrameTiming（同一條 compositor 執行緒依序呼叫）
    virtual_vsync_t vsync_;
    void *timer_ = nullptr;
    uint32_t last_missed_ = 0;
    bool last_presented_ = false;
    int64_t last_reject_log_qpc_ = 0;
    uint64_t frame_timing_calls_ = 0;

    // 10 秒統計
    int64_t stat_t0_ = 0;
    uint64_t stat_presents_ = 0;
    uint64_t stat_composed_ = 0;
    uint64_t stat_noslot_ = 0;
    uint64_t stat_acq_timeout_ = 0;
    uint64_t stat_no_target_ = 0;
    uint64_t stat_missed_ = 0;
    int64_t stat_last_present_ = 0;
    double stat_interval_max_ms_ = 0.0;
    layer_t stat_last_l0_ {};  // 最近一次的第 0 層（10 秒行印投影與 bounds，診斷用）
    bool stat_have_l0_ = false;
  };

}  // namespace vrdrv
