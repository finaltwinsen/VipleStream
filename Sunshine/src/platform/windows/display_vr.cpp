/**
 * @file src/platform/windows/display_vr.cpp
 * @brief VipleStream 2.0 §VR（M1b S1-08、設計 §C）：display_vr_t——從 VR bridge 的 frame ring 擷取 driver 合成的 SBS 影像。
 *
 * - init()（§C.2）：探測與 live 共用，不需要 IPC。adapter 依 K22 規則選（有 session config 時用它的 LUID），
 *   幾何＝協商的打包尺寸、BGRA8、SDR。不做桌面 DDA 的 GPU 優先權區塊（改在 capture() 以 HIGH 為準，§C.7）。
 * - capture()（§C.3）：事件驅動。driver 還沒 READY／generation 換了 → 以 bridge 的 NT handle 在自己的 device 上
 *   重開 ring／fence（不 reinit，避免重建 encoder 與 IDR）；等 evtFrm → frame_reader_t 讀最新一筆（§B.8 驗證）→
 *   fence_waiter_t 在 CPU 端確認 sharedFence（§B.7，2 ms；不變式 8：絕不 GPU Wait 對方的 fence）→ CopyResource
 *   到影像池 → Signal(影像 fence)、Signal(consumedFence)、Flush。driver 還沒 HMD_PRESENTING 時 10 fps 推黑幀。
 * - log 衛生（S1-02）：不印 handle 值、完整 GUID、SID。LUID 只印 match／n/a。
 *
 * 結構比照 vr_bridge_win.cpp 的 selftest_consume()（V2 的 T2 消費者），差別在：ring／fence 開在 display 自己的
 * capture device 上、複製目的地是影像池的 capture_texture、每一幀帶 VR metadata 交給 encoder。
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

// platform includes
#include <d3d11_4.h>
#include <dxgi1_6.h>

// local includes
#include "display.h"
#include "display_vram.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/vr/vr_bridge.h"
#include "src/vr/vr_ipc_abi.h"
#include "utf_utils.h"

namespace platf::dxgi {
  using namespace std::literals;

  namespace {
    constexpr auto k_black_interval = 100ms;  ///< §C.3：HMD_PRESENTING 之前的黑幀節拍（10 fps）
    constexpr uint32_t k_fence_wait_ms = 2;  ///< §B.7：CPU 端確認 sharedFence 的上限
    constexpr size_t k_stats_samples_max = 200000;  ///< selftest 用的累計樣本上限（60 s × 90 Hz 綽綽有餘）

    double qpc_to_ms(int64_t d) {
      const int64_t f = platf::qpc_frequency();
      return f > 0 ? (double) d * 1000.0 / (double) f : 0.0;
    }

    int64_t ms_to_qpc(int64_t ms) {
      return platf::qpc_frequency() * ms / 1000;
    }

    void percentiles(std::vector<double> v, double &p50, double &p95) {
      if (v.empty()) {
        p50 = p95 = 0;
        return;
      }
      std::sort(v.begin(), v.end());
      const auto at = [&](double q) {
        return v[std::min(v.size() - 1, (size_t) (q * (double) (v.size() - 1) + 0.5))];
      };
      p50 = at(0.5);
      p95 = at(0.95);
    }

    bool luid_eq(const LUID &a, const vr::bridge::luid_t &b) {
      return a.LowPart == b.low && a.HighPart == b.high;
    }

    /**
     * @brief K22 的 adapter 規則（§C.2 第 1 步）：候選＝非 SOFTWARE、VendorId ≠ 0x1414、能建 FL 11_0 device 的 adapter；
     *        `adapter_name` 有設時只留那一張。恰好一張就用它；多張要等 V4 的 P-B1 把 vrcompositor 的 adapter 寫進
     *        state.json 才能選——目前 fail closed（VR_DISABLED: adapter mismatch）。
     */
    /**
     * @brief 查 KMTQAITYPE_ADAPTERTYPE（15）的 D3DKMT_ADAPTERTYPE 位元。
     * @details bit0 RenderSupported、bit6 IndirectDisplayDevice。IddCx 虛擬顯示卡（例：MTT VDD）
     *          在 DXGI 會以「渲染 GPU 的名稱」出現成第二張 adapter，也建得出 device，只能靠這個位元分辨。
     * @return 查詢失敗回 std::nullopt（呼叫端視為「不排除」）。
     */
    std::optional<UINT> kmt_adapter_type(const LUID &luid) {
      HMODULE gdi32 = GetModuleHandleA("GDI32");
      if (!gdi32) {
        return std::nullopt;
      }
      auto open_adapter = (display_base_t::PD3DKMTOpenAdapterFromLuid) GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
      auto query_info = (display_base_t::PD3DKMTQueryAdapterInfo) GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
      auto close_adapter = (display_base_t::PD3DKMTCloseAdapter) GetProcAddress(gdi32, "D3DKMTCloseAdapter");
      if (!open_adapter || !query_info || !close_adapter) {
        return std::nullopt;
      }
      display_base_t::D3DKMT_OPENADAPTERFROMLUID oa = {luid};
      if (FAILED(open_adapter(&oa))) {
        return std::nullopt;
      }
      UINT type = 0;
      display_base_t::D3DKMT_QUERYADAPTERINFO qi = {};
      qi.hAdapter = oa.hAdapter;
      qi.Type = 15;  // KMTQAITYPE_ADAPTERTYPE
      qi.pPrivateDriverData = &type;
      qi.PrivateDriverDataSize = sizeof(type);
      const bool ok = SUCCEEDED(query_info(&qi));
      display_base_t::D3DKMT_CLOSEADAPTER ca = {oa.hAdapter};
      close_adapter(&ca);
      if (!ok) {
        return std::nullopt;
      }
      return type;
    }

    adapter_t pick_vr_adapter(std::string &why) {
      factory1_t factory;
      if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory))) {
        why = "CreateDXGIFactory1 failed";
        return nullptr;
      }
      const auto adapter_name = utf_utils::from_utf8(config::video.adapter_name);
      std::vector<adapter_t> candidates;
      int skipped_indirect = 0;
      adapter_t::pointer adapter_p {};
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter_p) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapter_t a {adapter_p};
        DXGI_ADAPTER_DESC1 desc {};
        if (FAILED(a->GetDesc1(&desc))) {
          continue;
        }
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId == 0x1414) {
          continue;
        }
        if (!adapter_name.empty() && desc.Description != adapter_name) {
          continue;
        }
        // IddCx 虛擬顯示卡：DXGI 描述與 VRAM 跟實體 GPU 一模一樣，只有 KMT adapter type 看得出來
        if (const auto kmt = kmt_adapter_type(desc.AdapterLuid); kmt && ((*kmt & (1u << 6)) || !(*kmt & 1u))) {
          ++skipped_indirect;
          continue;
        }
        const D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ID3D11Device *probe_dev = nullptr;
        const HRESULT hr = D3D11CreateDevice(a.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &probe_dev, nullptr, nullptr);
        if (FAILED(hr) || !probe_dev) {
          continue;  // IddCx 虛擬顯示卡（例：MTT VDD）建不出 device，不會是候選
        }
        probe_dev->Release();
        candidates.push_back(std::move(a));
      }
      if (candidates.size() == 1) {
        return std::move(candidates.front());
      }
      why = candidates.empty() ? "no hardware adapter can create a feature level 11_0 device" :
                                 std::format("adapter mismatch (candidates={} skippedIndirect={}; vrcompositor adapter not recorded yet, set adapter_name to pick one)", candidates.size(), skipped_indirect);
      return nullptr;
    }

    /// §C.7：HAGS 狀態（與 display_base_t::init() 的判斷相同）
    bool hags_enabled(const LUID &luid) {
      HMODULE gdi32 = GetModuleHandleA("GDI32");
      if (!gdi32) {
        return false;
      }
      auto open_adapter = (display_base_t::PD3DKMTOpenAdapterFromLuid) GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
      auto query_info = (display_base_t::PD3DKMTQueryAdapterInfo) GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
      auto close_adapter = (display_base_t::PD3DKMTCloseAdapter) GetProcAddress(gdi32, "D3DKMTCloseAdapter");
      if (!open_adapter || !query_info || !close_adapter) {
        return false;
      }
      display_base_t::D3DKMT_OPENADAPTERFROMLUID oa = {luid};
      if (FAILED(open_adapter(&oa))) {
        return false;
      }
      display_base_t::D3DKMT_WDDM_2_7_CAPS caps = {};
      display_base_t::D3DKMT_QUERYADAPTERINFO qi = {};
      qi.hAdapter = oa.hAdapter;
      qi.Type = 70;  // KMTQAITYPE_WDDM_2_7_CAPS
      qi.pPrivateDriverData = &caps;
      qi.PrivateDriverDataSize = sizeof(caps);
      const bool enabled = SUCCEEDED(query_info(&qi)) && caps.HwSchEnabled;
      display_base_t::D3DKMT_CLOSEADAPTER ca = {oa.hAdapter};
      close_adapter(&ca);
      return enabled;
    }

    /**
     * @brief §C.7：擷取期間把行程的 GPU 排程優先權設成 HIGH（vrcompositor 與遊戲在同一張卡，REALTIME 會搶走遊戲的 GPU）；
     *        結束時還原成「依設定該有的值」（HAGS＋NVIDIA＋!nv_realtime_hags → HIGH，其他 REALTIME；同 display_base.cpp）。
     */
    class gpu_priority_guard {
    public:
      gpu_priority_guard(const LUID &luid, UINT vendor) {
        HMODULE gdi32 = GetModuleHandleA("GDI32");
        set_ = gdi32 ? (display_base_t::PD3DKMTSetProcessSchedulingPriorityClass) GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass") : nullptr;
        if (!set_) {
          return;
        }
        restore_ = (vendor == 0x10DE && hags_enabled(luid) && !config::video.nv_realtime_hags) ? display_base_t::D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH : display_base_t::D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
        const bool ok = SUCCEEDED(set_(GetCurrentProcess(), display_base_t::D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH));
        BOOST_LOG(info) << "[VIPLE-VR-CAP] gpu-priority=high (was "sv << (restore_ == display_base_t::D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH ? "high"sv : "realtime"sv) << ')' << (ok ? ""sv : " set-failed"sv);
      }

      ~gpu_priority_guard() {
        if (set_) {
          set_(GetCurrentProcess(), restore_);
        }
      }

      gpu_priority_guard(const gpu_priority_guard &) = delete;
      gpu_priority_guard &operator=(const gpu_priority_guard &) = delete;

    private:
      display_base_t::PD3DKMTSetProcessSchedulingPriorityClass set_ = nullptr;
      display_base_t::D3DKMT_SCHEDULINGPRIORITYCLASS restore_ = display_base_t::D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
    };
  }  // namespace

  /// 本 generation 的 ring／fence（開在 display 的 capture device 上）與讀取器
  struct display_vr_t::consumer_state_t {
    std::shared_ptr<vr::bridge::frame_source_t> src;
    std::optional<vr::bridge::frame_reader_t> reader;
    std::optional<vr::bridge::fence_waiter_t> waiter;
    texture2d_t tex[VRIPC_TEX_COUNT];
    fence_t shared_fence;
    fence_t consumed_fence;
    uint64_t last_consumed = 0;
    int64_t fence_timeout_since = 0;
  };

  int display_vr_t::init(const ::video::config_t &config) {
    if (config.width <= 0 || config.height <= 0 || config.framerate <= 0) {
      BOOST_LOG(error) << "[VIPLE-VR-CAP] init: invalid shape "sv << config.width << 'x' << config.height << '@' << config.framerate;
      return -1;
    }

    // 1. adapter（K22）：有 session config 時用它的 LUID（server 先選 LUID，driver 照做）；否則依候選規則
    std::string why;
    const auto cfg_luid = vr::bridge::session_config_luid();
    bool luid_match = false;
    if (cfg_luid) {
      adapter = find_adapter_by_luid(LUID {cfg_luid->low, cfg_luid->high});
      luid_match = (bool) adapter;
      if (!adapter) {
        why = "session config adapter LUID not found";
      }
    } else {
      adapter = pick_vr_adapter(why);
    }
    if (!adapter) {
      BOOST_LOG(error) << "[VIPLE-VR-CAP] VR_DISABLED: "sv << why;
      return -1;
    }

    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, (void **) &factory))) {
      BOOST_LOG(error) << "[VIPLE-VR-CAP] init: CreateDXGIFactory1 failed"sv;
      return -1;
    }

    // 2. 幾何：協商的打包尺寸，1:1、無旋轉、BGRA8（§C.2 第 2 步；make_port 以 width／height 當除數）
    width = config.width;
    height = config.height;
    env_width = width;
    env_height = height;
    offset_x = 0;
    offset_y = 0;
    width_before_rotation = width;
    height_before_rotation = height;
    display_rotation = DXGI_MODE_ROTATION_IDENTITY;
    capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    display_refresh_rate = DXGI_RATIONAL {(UINT) config.framerate, 1};
    display_refresh_rate_rounded = config.framerate;
    client_frame_rate = config.framerate;
    client_frame_rate_strict = {0, 0};

    // 3. device（不做桌面的 GPU 優先權區塊；§C.7 在 capture() 設 HIGH）
    if (init_device(adapter.get()) || configure_frame_latency()) {
      return -1;
    }
    if (!device_ctx4) {
      BOOST_LOG(error) << "[VIPLE-VR-CAP] init: ID3D11DeviceContext4 is required for fence synchronization"sv;
      return -1;
    }
    if (!timer || !*timer) {
      BOOST_LOG(error) << "Uninitialized high precision timer";
      return -1;
    }

    DXGI_ADAPTER_DESC desc {};
    adapter->GetDesc(&desc);
    adapter_luid = desc.AdapterLuid;

    // 4. log（mode 只看 bridge 有沒有 frame source；探測與 live 走同一條 init）
    const bool live = vr::bridge::frame_source() != nullptr;
    BOOST_LOG(info) << "[VIPLE-VR-CAP] init adapter="sv << utf_utils::to_utf8(desc.Description)
                    << " luid="sv << (luid_match ? "match"sv : "n/a"sv)
                    << " size="sv << width << 'x' << height << '@' << config.framerate
                    << " mode="sv << (live ? "live"sv : "probe"sv);
    return 0;
  }

  capture_e display_vr_t::snapshot(const pull_free_image_cb_t &, std::shared_ptr<platf::img_t> &, std::chrono::milliseconds, bool) {
    return capture_e::timeout;  // 不使用：capture() 自己驅動（事件驅動）
  }

  capture_e display_vr_t::release_snapshot() {
    return capture_e::ok;
  }

  display_vr_t::stats_t display_vr_t::stats() {
    std::lock_guard lk {stats_mtx_};
    return stats_;
  }

  capture_e display_vr_t::push_black(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb) {
    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img)) {
      return capture_e::ok;
    }
    if (complete_img(img.get(), true)) {
      return capture_e::error;
    }
    // 池子裡回收的影像可能是上一張真幀（blank=false）；黑幀一律標 blank，encoder 端直接輸出黑畫面、不讀貼圖
    static_cast<img_d3d_t *>(img.get())->blank = true;
    ::video::vr_frame_meta_t meta {};
    meta.valid = false;
    meta.flags = VRIPC_FRM_POSE_FALLBACK;
    img->vr = meta;
    img->frame_timestamp = std::chrono::steady_clock::now();
    last_black_ = std::chrono::steady_clock::now();
    {
      std::lock_guard lk {stats_mtx_};
      ++stats_.black;
      ++window_.black;
    }
    if (!push_captured_image_cb(std::move(img), true)) {
      return capture_e::ok;
    }
    return capture_e::timeout;  // 內部用：繼續迴圈
  }

  capture_e display_vr_t::capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *) {
    DXGI_ADAPTER_DESC adesc {};
    adapter->GetDesc(&adesc);
    gpu_priority_guard prio {adesc.AdapterLuid, adesc.VendorId};

    device1_t dev1;
    device5_t dev5;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), (void **) &dev1)) ||
        FAILED(device->QueryInterface(__uuidof(ID3D11Device5), (void **) &dev5))) {
      BOOST_LOG(error) << "[VIPLE-VR-CAP] ID3D11Device1/5 not available"sv;
      return capture_e::error;
    }

    uint64_t abandoned_gen = 0;  ///< 已要求拆除的 generation：等它真的拆掉，不重開
    std::string last_open_error;
    last_log_ = std::chrono::steady_clock::now();

    auto &cs = st_;
    auto signal_consumed = [&](uint64_t v) {
      // 只往上（latest-wins 跳過的 slot 一併放回），Signal 後一定 Flush（跨行程 CPU 觀察）
      v = std::max(v, cs->last_consumed);
      device_ctx4->Signal(cs->consumed_fence.get(), v);
      device_ctx->Flush();
      cs->last_consumed = v;
    };

    auto log_window = [&]() {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_log_ < 10s) {
        return;
      }
      stats_t w;
      {
        std::lock_guard lk {stats_mtx_};
        w = std::move(window_);
        window_ = stats_t {};
      }
      double e50, e95, p50, p95, a50, a95;
      percentiles(std::move(w.evt_to_push_ms), e50, e95);
      percentiles(std::move(w.present_to_push_ms), p50, p95);
      percentiles(std::move(w.echo_age_ms), a50, a95);
      BOOST_LOG(info) << "[VIPLE-VR-CAP] 10s: gen="sv << (cs && cs->src ? cs->src->generation : 0)
                      << " copied="sv << w.copied << " skipped="sv << w.skipped << " fenceTimeout="sv << w.fence_timeout
                      << " invalid="sv << w.invalid << " stale="sv << w.stale << " black="sv << w.black
                      << " evtToPush p50="sv << std::format("{:.3f}", e50) << "ms p95="sv << std::format("{:.3f}", e95)
                      << "ms presentToPush p50="sv << std::format("{:.3f}", p50) << "ms p95="sv << std::format("{:.3f}", p95)
                      << "ms echoAge p50="sv << std::format("{:.3f}", a50) << "ms p95="sv << std::format("{:.3f}", a95) << "ms"sv;
      last_log_ = now;
    };

    auto count = [&](uint64_t stats_t::*field, uint64_t n = 1) {
      std::lock_guard lk {stats_mtx_};
      stats_.*field += n;
      window_.*field += n;
    };

    while (true) {
      log_window();

      // ── frame source：driver 還沒 READY／generation 換了 ──────────────────────
      auto cur = vr::bridge::frame_source();
      if (!cur || !cur->alive() || cur->generation == abandoned_gen) {
        if (cs) {
          cs.reset();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_black_ >= k_black_interval) {
          if (!vr::bridge::status().hmd_presenting) {
            if (auto r = push_black(push_captured_image_cb, pull_free_image_cb); r != capture_e::timeout) {
              return r;
            }
          } else {
            last_black_ = now;
            if (!push_captured_image_cb(nullptr, false)) {
              return capture_e::ok;  // 讓 captureThread 檢查停止／新 ctx（同 display_base.cpp）
            }
          }
        }
        Sleep(10);
        continue;
      }
      if (!cs || cur->generation != cs->src->generation) {
        // ring／fence 所在的卡必須就是自己的 capture device 那張；不同就重建 display（init 會改用 session config 的 LUID）
        if (!luid_eq(adapter_luid, cur->luid)) {
          BOOST_LOG(warning) << "[VIPLE-VR-CAP] frame source gen="sv << cur->generation << " is on a different adapter; reinit"sv;
          return capture_e::reinit;
        }
        auto ns = std::make_shared<consumer_state_t>();
        std::string err;
        for (uint32_t i = 0; i < VRIPC_TEX_COUNT && err.empty(); ++i) {
          if (FAILED(dev1->OpenSharedResource1(static_cast<HANDLE>(cur->tex_nt[i]), __uuidof(ID3D11Texture2D), (void **) &ns->tex[i]))) {
            err = std::format("open-texture[{}]", i);
          }
        }
        if (err.empty() && FAILED(dev5->OpenSharedFence(static_cast<HANDLE>(cur->shared_fence_nt), __uuidof(ID3D11Fence), (void **) &ns->shared_fence))) {
          err = "open-shared-fence";
        }
        if (err.empty() && FAILED(dev5->OpenSharedFence(static_cast<HANDLE>(cur->consumed_fence_nt), __uuidof(ID3D11Fence), (void **) &ns->consumed_fence))) {
          err = "open-consumed-fence";
        }
        if (err.empty()) {
          D3D11_TEXTURE2D_DESC d {};
          ns->tex[0]->GetDesc(&d);
          if (d.Width != (UINT) width || d.Height != (UINT) height || d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            // 協商尺寸改變 → reinit（§C.3：driver generation 改變本身不 reinit）
            BOOST_LOG(warning) << "[VIPLE-VR-CAP] ring "sv << d.Width << 'x' << d.Height << " fmt="sv << (uint32_t) d.Format
                               << " != display "sv << width << 'x' << height << "; reinit"sv;
            return capture_e::reinit;
          }
        }
        if (!err.empty()) {
          if (err != last_open_error) {
            BOOST_LOG(warning) << "[VIPLE-VR-CAP] open gen="sv << cur->generation << " failed: "sv << err;
            last_open_error = err;
          }
          if (dev5 && device->GetDeviceRemovedReason() != S_OK) {
            return capture_e::reinit;
          }
          Sleep(100);
          continue;
        }
        last_open_error.clear();
        ns->src = cur;
        ns->reader.emplace(cur);
        ns->waiter.emplace();
        ns->last_consumed = ns->consumed_fence->GetCompletedValue();
        if (ns->last_consumed == UINT64_MAX) {
          ns->last_consumed = cur->shared_fence_initial;
        }
        ns->reader->set_fence_baseline(ns->last_consumed);
        cs = ns;
        {
          std::lock_guard lk {stats_mtx_};
          stats_.generation = cur->generation;
        }
        BOOST_LOG(info) << "[VIPLE-VR-CAP] opened gen="sv << cur->generation << " ring="sv << width << 'x' << height;
        // 開完先「排空」：最新一筆的 slot 直接放回（不複製；避免拿到早就過期的幀）
        vripc_frame_desc_t d {};
        if (cs->reader->read_latest(d) == vr::bridge::frame_reader_t::result_e::ok) {
          signal_consumed(d.fence_value);
        }
      }

      // ── 等下一幀 ─────────────────────────────────────────────────────────────
      const DWORD w = WaitForSingleObject(static_cast<HANDLE>(cs->src->evt_frm), 100);
      if (w != WAIT_OBJECT_0) {
        const auto now = std::chrono::steady_clock::now();
        if (!vr::bridge::status().hmd_presenting && now - last_black_ >= k_black_interval) {
          if (auto r = push_black(push_captured_image_cb, pull_free_image_cb); r != capture_e::timeout) {
            return r;
          }
          continue;
        }
        if (!push_captured_image_cb(nullptr, false)) {
          return capture_e::ok;
        }
        continue;
      }
      const int64_t evt_qpc = platf::qpc_counter();

      vripc_frame_desc_t d {};
      switch (cs->reader->read_latest(d)) {
        case vr::bridge::frame_reader_t::result_e::ok:
          break;
        case vr::bridge::frame_reader_t::result_e::invalid:
          count(&stats_t::invalid);
          continue;
        case vr::bridge::frame_reader_t::result_e::torn:
          count(&stats_t::torn);
          continue;
        case vr::bridge::frame_reader_t::result_e::none:
          continue;
      }
      count(&stats_t::skipped, cs->reader->last_skipped());
      {
        std::lock_guard lk {stats_mtx_};
        if (stats_.first_frame_id == 0) {
          stats_.first_frame_id = d.frame_id;
        }
        stats_.last_frame_id = d.frame_id;
      }

      if (evt_qpc - d.present_qpc > ms_to_qpc(100)) {
        count(&stats_t::stale);
        signal_consumed(d.fence_value);
        continue;
      }

      // §B.7：CPU 端確認 driver 的 GPU 寫完（不 GPU Wait 對方的 fence，不變式 8）
      const auto fr = cs->waiter->wait(cs->shared_fence.get(), d.fence_value, k_fence_wait_ms);
      if (fr == vr::bridge::fence_waiter_t::result_e::lost) {
        count(&stats_t::fence_lost);
        if (device->GetDeviceRemovedReason() != S_OK) {
          return capture_e::reinit;  // 自己的 device 掉了
        }
        vr::bridge::request_teardown(cs->src->generation, VRIPC_BYE_DEVICE_LOST);  // driver 端問題 → 新 generation
        count(&stats_t::teardown_requests);
        abandoned_gen = cs->src->generation;
        cs.reset();
        continue;
      }
      if (fr != vr::bridge::fence_waiter_t::result_e::reached) {
        count(&stats_t::fence_timeout);
        signal_consumed(d.fence_value);  // 不複製，但仍放回 slot
        const int64_t now = platf::qpc_counter();
        if (cs->fence_timeout_since == 0) {
          cs->fence_timeout_since = now;
        } else if (now - cs->fence_timeout_since >= platf::qpc_frequency()) {
          vr::bridge::request_teardown(cs->src->generation, VRIPC_BYE_PROTOCOL_ERROR);  // 連續逾時累計 ≥ 1 s
          count(&stats_t::teardown_requests);
          abandoned_gen = cs->src->generation;
          cs.reset();
        }
        continue;
      }
      cs->fence_timeout_since = 0;

      std::shared_ptr<platf::img_t> img_base;
      if (!pull_free_image_cb(img_base)) {
        signal_consumed(d.fence_value);
        return capture_e::ok;
      }
      if (complete_img(img_base.get(), false)) {
        signal_consumed(d.fence_value);
        return capture_e::error;
      }
      auto img = static_cast<img_d3d_t *>(img_base.get());
      {
        texture_fence_helper fence_helper(device_ctx4.get(), img);
        if (!fence_helper.arm()) {
          signal_consumed(d.fence_value);
          return capture_e::error;
        }
        device_ctx->CopyResource(img->capture_texture.get(), cs->tex[d.tex_idx].get());
        // 解構時 Signal(capture_fence, ++fence_value)：encoder 端以 GPU Wait 自己行程的這個 fence（同行程，允許）
      }
      signal_consumed(d.fence_value);  // 含 Flush：CopyResource 與兩個 Signal 一起送出
      img->blank = false;

      ::video::vr_frame_meta_t meta {};
      meta.valid = (d.flags & VRIPC_FRM_POSE_VALID) != 0;
      meta.echoSampleId = d.echo_sample_id;
      std::copy(std::begin(d.render_pos), std::end(d.render_pos), std::begin(meta.pos));
      std::copy(std::begin(d.render_rot), std::end(d.render_rot), std::begin(meta.rot));
      meta.flags = (uint8_t) (d.flags & 0x2Fu);  // VIPLE_VR_FF_* 與 VRIPC_FRM_* 的低位相同；SPACE_DELTA（0x100）只供診斷
      meta.layout_epoch = d.layout_epoch;
      meta.space_epoch = d.space_epoch;
      meta.frame_id = d.frame_id;
      meta.present_qpc = d.present_qpc;
      img->vr = meta;
      img->frame_timestamp = frame_timestamp_from_qpc(d.present_qpc);  // S1-01 修正後的換算

      vripc_consume_status_t status {};
      {
        std::lock_guard lk {stats_mtx_};
        ++stats_.copied;
        ++window_.copied;
        status.frames_copied = stats_.copied;
        status.frames_skipped_latest = stats_.skipped;
        status.frames_fence_timeout = stats_.fence_timeout;
        status.frames_invalid = stats_.invalid;
        status.frames_device_lost = stats_.fence_lost;
      }
      status.last_consumed_fence = cs->last_consumed;
      status.last_consumed_frame_id = d.frame_id;
      cs->reader->publish_consume(status);

      if (!live_logged_) {
        live_logged_ = true;
        BOOST_LOG(info) << "[VIPLE-VR-CAP] first frame gen="sv << cs->src->generation << " frame_id="sv << d.frame_id;
      }

      const int64_t push_qpc = platf::qpc_counter();
      {
        std::lock_guard lk {stats_mtx_};
        const double e = qpc_to_ms(push_qpc - evt_qpc);
        const double p = qpc_to_ms(push_qpc - d.present_qpc);
        window_.evt_to_push_ms.push_back(e);
        window_.present_to_push_ms.push_back(p);
        if (const int64_t pub = cs->reader->last_echo_published_qpc(); pub != 0 && d.present_qpc > pub) {
          window_.echo_age_ms.push_back(qpc_to_ms(d.present_qpc - pub));
        }
        if (stats_.evt_to_push_ms.size() < k_stats_samples_max) {
          stats_.evt_to_push_ms.push_back(e);
          stats_.present_to_push_ms.push_back(p);
        }
      }
      if (!push_captured_image_cb(std::move(img_base), true)) {
        return capture_e::ok;
      }
    }
  }
  /**
   * @brief M1b V5（§S1-13）：給 vr_platform 的環境探測與編排器用的 K22 adapter 規則（與擷取端同一套）。
   * @return 恰好一張候選時寫入 out 並回 true；否則 why 為原因。
   */
  bool vr_pick_adapter_luid(LUID &out, std::string &why) {
    auto a = pick_vr_adapter(why);
    if (!a) {
      return false;
    }
    DXGI_ADAPTER_DESC1 desc {};
    if (FAILED(a->GetDesc1(&desc))) {
      why = "GetDesc1 failed";
      return false;
    }
    out = desc.AdapterLuid;
    return true;
  }

}  // namespace platf::dxgi
