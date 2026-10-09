/**
 * @file src/platform/windows/display_vr.cpp
 * @brief VipleStream 2.0 §VR（M1b S1-08、設計 §C）：display_vr_t——從 VR bridge 的 frame ring 擷取 driver 合成的 SBS 影像。
 *
 * - init()（§C.2）：探測與 live 共用，不需要 IPC。adapter 依 K22 規則選（有 session config 時用它的 LUID），
 *   幾何＝協商的打包尺寸、BGRA8、SDR。不做桌面 DDA 的 GPU 優先權區塊（改在 capture() 以 HIGH 為準，§C.7）。
 * - capture()（§C.3）：事件驅動。driver 還沒 READY／generation 換了 → 以 bridge 的 NT handle 在自己的 device 上
 *   重開 ring／fence（不 reinit，避免重建 encoder 與 IDR）；等 evtFrm → frame_reader_t 讀最新一筆（§B.8 驗證）→
 *   fence_waiter_t 在 CPU 端確認 sharedFence（§B.7；上限依週期 2～6 ms，§VR-FENCE-WAIT；不變式 8：絕不 GPU Wait
 *   對方的 fence）→ CopyResource
 *   到影像池 → Signal(影像 fence)、Signal(consumedFence)、Flush。driver 還沒 HMD_PRESENTING 時 10 fps 推黑幀。
 * - §VR-FENCE-LATE（2026-10-09；config `vr_capture_late`，預設關）：第一段等待逾時後不丟這一張，繼續在 CPU 端等同一個
 *   fence 值到「等待預算」為止，晚到照樣複製；拆線改由 src/vr/vr_capture_health.h 判定（完成值 3 s 不動、或 5 s 沒有
 *   一張複製成功，而且有退避），不再用「連續逾時滿 1 s」。放棄可以連續很久（退避期最長 30 s），所以開關開著時
 *   所有不交影像的路徑都會每 100 ms 讓 captureThread 檢查一次停止（keepalive）。開關關著時走的每一條路徑與之前
 *   相同，只多量測與 log。
 * - log 衛生（S1-02）：不印 handle 值、完整 GUID、SID。LUID 只印 match／n/a。
 *
 * 結構比照 vr_bridge_win.cpp 的 selftest_consume()（V2 的 T2 消費者），差別在：ring／fence 開在 display 自己的
 * capture device 上、複製目的地是影像池的 capture_texture、每一幀帶 VR metadata 交給 encoder。
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>

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
#include "src/vr/vr_capture_health.h"
#include "src/vr/vr_ipc_abi.h"
#include "utf_utils.h"

namespace platf::dxgi {
  using namespace std::literals;

  namespace {
    constexpr auto k_black_interval = 100ms;  ///< §C.3：HMD_PRESENTING 之前的黑幀節拍（10 fps）
    constexpr uint32_t k_fence_wait_min_ms = 2;  ///< §B.7：CPU 端確認 sharedFence 的上限（原本固定 2 ms）
    constexpr uint32_t k_fence_wait_max_ms = 6;

    /// §VR-FENCE-WAIT（2026-10-04，Frame 實測）：遊戲吃滿 GPU 時 driver 的合成排在遊戲後面，常超過 2 ms
    /// （10 s 內 fenceTimeout 94～274 次，擷取只剩 922～1054／1200 幀）。丟一幀要多等一整個週期才有下一張，
    /// 只要不拖到下一幀就繼續等：上限＝週期的 70%，夾在 2～6 ms（120 Hz 約 5 ms）。
    uint32_t fence_wait_ms(int fps) {
      if (fps <= 0) {
        return k_fence_wait_min_ms;
      }
      return std::clamp<uint32_t>(static_cast<uint32_t>(700 / fps), k_fence_wait_min_ms, k_fence_wait_max_ms);
    }
    constexpr size_t k_stats_samples_max = 200000;  ///< selftest 用的累計樣本上限（60 s × 90 Hz 綽綽有餘）

    /// §VR-FENCE-LATE：第一段等待逾時後繼續等同一個 fence 值時，每次最多等多久就醒來一次（看 generation 還在不在）
    constexpr uint32_t k_late_slice_ms = 2;

    /// §VR-FENCE-LATE：沒有影像可交的路徑上，最多隔多久讓 captureThread 檢查一次停止／新 ctx（同黑幀的節拍）
    constexpr int64_t k_keepalive_ms = 100;

    /// §VR-FENCE-LATE：退避狀態在行程層級的備份。health_t 是 display_vr_t 的成員，活得過 generation 更換；但
    /// captureThread 在 capture_e::reinit 時會銷毀並重建 display（video.cpp），新的 session 也是新的 display。
    /// capture() 進來時從這裡載入、離開時存回，退避的階梯才不會因為重建而歸零。
    /// 備份只是為了跨過重建（以及使用者馬上重連的下一場）：存下來超過 k_backoff_keep_ms 沒有人接手就不還原，
    /// 隔了很久的下一場從第一級開始，不會一拆就是 30 s 的退避。
    /// 正常同一時間只有一個 capture() 在跑；selftest 另外開一個時，以後離開的那一個存的為準。
    constexpr int64_t k_backoff_keep_ms = 60'000;

    struct backoff_save_t {
      vr::capture::backoff_t backoff;
      int64_t saved_qpc = 0;  ///< 存下來的時刻；0＝沒有存過
    };

    std::mutex g_backoff_mtx;
    backoff_save_t g_backoff;

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
      // 雙顯卡筆電（例：Intel UHD＋RTX 3060）：候選多張、但只有一張有接顯示輸出時，選那張。
      // 在 SteamVR 還沒跑、vrcompositor 的 adapter 還沒記錄前，/serverinfo 就要決定是否宣告
      // PCVR；兩張都有輸出或都沒有時不猜，照舊要求 adapter_name。
      if (candidates.size() > 1) {
        int with_output = -1;
        int n_with_output = 0;
        for (int i = 0; i < (int) candidates.size(); ++i) {
          IDXGIOutput *out = nullptr;
          if (SUCCEEDED(candidates[i]->EnumOutputs(0, &out)) && out) {
            out->Release();
            ++n_with_output;
            with_output = i;
          }
        }
        if (n_with_output == 1) {
          DXGI_ADAPTER_DESC1 d {};
          candidates[with_output]->GetDesc1(&d);
          BOOST_LOG(info) << "[VIPLE-VR-CAP] adapter auto-pick: the only one of " << candidates.size()
                          << " hardware adapters driving a display: " << utf_utils::to_utf8(d.Description);
          return std::move(candidates[with_output]);
        }
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
    bool first_timeout_logged = false;  ///< §VR-FENCE-LATE：這個 generation 的第一次 fence 逾時已經印過
    bool deferred_logged = false;  ///< §VR-FENCE-LATE：這個 generation 的「判定成立但還在退避期」已經印過
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

    // §VR-FENCE-LATE：開關與等待上限在進來時讀一次（設定檔改了要重啟 server 才生效）；退避接上一次離開時存的
    // （存下來超過 k_backoff_keep_ms 就不接，從頭開始）
    const bool late_enabled = config::vr.capture_late;
    health_.configure(late_enabled, client_frame_rate > 0 ? vr::capture::k_ns_per_s / client_frame_rate : 0, (int64_t) config::vr.capture_late_max_ms * vr::capture::k_ns_per_ms);
    {
      vr::capture::backoff_t restored {};
      std::lock_guard lk {g_backoff_mtx};
      if (g_backoff.saved_qpc != 0 && platf::qpc_counter() - g_backoff.saved_qpc < ms_to_qpc(k_backoff_keep_ms)) {
        restored = g_backoff.backoff;
      }
      health_.restore_backoff(restored);
    }
    // 實際生效的設定印一次（每次進 capture() 一行）：之後的 log 只能從 budgetMs 是不是 0 反推開關，看不到上限
    BOOST_LOG(info) << "[VIPLE-VR-CAP] capture late="sv << (late_enabled ? "on"sv : "off"sv)
                    << " firstWaitMs="sv << fence_wait_ms(client_frame_rate) << " maxWaitMs="sv << config::vr.capture_late_max_ms
                    << " backoffStep="sv << health_.backoff().step;
    const int64_t keepalive_qpc = ms_to_qpc(k_keepalive_ms);
    const int64_t period_qpc = client_frame_rate > 0 ? platf::qpc_frequency() / client_frame_rate : 0;  ///< 一個顯示週期
    uint64_t health_gen = 0;  ///< 目前（或最近一個）開著的 generation，log 用
    bool overload_pub = false;  ///< 發布出去的過載旗標（stream.cpp 看到的值）
    int64_t health_pub_qpc = 0;  ///< 上一次發布快照的時刻；0＝目前發布的是預設值
    int64_t last_push_cb_qpc = platf::qpc_counter();  ///< 上一次呼叫 push 回呼的時刻（下面的 keepalive 用；約略即可）

    // 過載旗標變化各印一行（設起來要 ≥ 30%、清掉要 < 5% 持續 2 s，再怎麼來回 10 s 內也只有十行上下）
    auto log_overload = [&](bool on, std::string_view why) {
      const auto snap = health_.snapshot();
      BOOST_LOG(info) << "[VIPLE-VR-CAP] overload "sv << (on ? "on"sv : "off"sv) << " gen="sv << health_gen
                      << " latePermille="sv << snap.late_permille << " fps="sv << snap.pushed_fps
                      << " budgetMs="sv << (health_.wait_budget_ns() + 500'000) / 1'000'000
                      << " waitMaxMs="sv << std::format("{:.1f}", (double) health_.window_wait_max_ns() / 1e6)
                      << " why="sv << why;
    };
    // 給其他執行緒看的快照（stream.cpp 的 LATCH／PREDICT 只讀過載旗標）：旗標一變就發布，數字每秒發布一次
    auto publish_health = [&](int64_t now_qpc) {
      const auto snap = health_.snapshot();
      if (snap.overload == overload_pub && health_pub_qpc != 0 && now_qpc - health_pub_qpc < platf::qpc_frequency()) {
        return;
      }
      vr::bridge::set_capture_health(snap);
      health_pub_qpc = now_qpc;
      if (snap.overload != overload_pub) {
        overload_pub = snap.overload;
        log_overload(snap.overload, snap.overload ? "late-frames"sv : "recovered"sv);
      }
    };
    // 沒有 frame source、換 generation、離開 capture()：快照清成預設（過載旗標 false）
    auto clear_health = [&](std::string_view why) {
      if (overload_pub) {
        overload_pub = false;
        log_overload(false, why);
      }
      if (health_pub_qpc != 0) {
        vr::bridge::set_capture_health({});
        health_pub_qpc = 0;
      }
    };
    // 任何出口：退避連同現在的時刻存回行程層級的備份、快照清成預設
    const auto on_leave = util::fail_guard([&]() {
      {
        std::lock_guard lk {g_backoff_mtx};
        g_backoff = {health_.backoff(), platf::qpc_counter()};
      }
      clear_health("capture-exit"sv);
    });
    // §VR-FENCE-LATE：沒有影像可交的路徑（放棄、過期、不合格、ring 開不起來）也要定期讓 captureThread 檢查停止與
    // 新的 ctx——它只在 push／pull 回呼裡看。卡死／餓死的判定成立、但還在退避期內時，每一張都放棄而 driver 仍在發布，
    // 「100 ms 沒有新幀」那一支走不到，最長 30 s 不會經過任何回呼；那段時間 session 一結束，session::join 等擷取
    // 執行緒超過 10 s 就會觸發 Hang detector、把 server 行程中止。最多每 k_keepalive_ms 一次；回 false＝該離開 capture()。
    // 只在開關開著時啟用：開關關著的路徑維持原樣（舊規則連續逾時 1 s 就拆線，之後走上面每 100 ms 有回呼的那一支）。
    // tick_health＝順便讓最近 1 s 的視窗往前走：影像一直來、但沒有一張走到等 fence（過期、不合格）時 on_frame 不會被
    // 呼叫，發布出去的過載旗標會停在最後的值。
    auto keepalive = [&](bool tick_health) -> bool {
      if (!late_enabled) {
        return true;
      }
      const int64_t now_qpc = platf::qpc_counter();
      if (now_qpc - last_push_cb_qpc < keepalive_qpc) {
        return true;
      }
      last_push_cb_qpc = now_qpc;
      if (tick_health) {
        health_.on_idle(platf::qpc_ticks_to_ns(now_qpc));
        publish_health(now_qpc);
      }
      return push_captured_image_cb(nullptr, false);
    };

    auto &cs = st_;
    auto signal_consumed = [&](uint64_t v) {
      // 只往上（latest-wins 跳過的 slot 一併放回），Signal 後一定 Flush（跨行程 CPU 觀察）
      v = std::max(v, cs->last_consumed);
      device_ctx4->Signal(cs->consumed_fence.get(), v);
      device_ctx->Flush();
      cs->last_consumed = v;
    };

    // 一張走到「等 fence」這一步的影像處理完：餵健康判定（純整數運算，不配置、不上鎖）。wait_qpc＝從被叫醒到等待結束
    auto feed = [&](vr::capture::outcome_e outcome, const vripc_frame_desc_t &desc, uint64_t completed, int64_t wait_qpc, int64_t now_qpc) {
      vr::capture::frame_t f;
      f.outcome = outcome;
      f.now_ns = platf::qpc_ticks_to_ns(now_qpc);
      f.wait_ns = platf::qpc_ticks_to_ns(wait_qpc);
      f.present_ns = platf::qpc_ticks_to_ns(desc.present_qpc);
      f.frame_id = desc.frame_id;
      f.observed = completed;
      const auto verdict = health_.on_frame(f);
      if (verdict.backoff_reset) {
        // 健康的一秒格在「下一格的第一張」結算，那一張可能是複製成功的、也可能是放棄的：兩條路徑都從這裡印
        BOOST_LOG(info) << "[VIPLE-VR-CAP] teardown backoff reset gen="sv << health_gen << " (10 s healthy)"sv;
      }
      return verdict;
    };

    // 每個 generation 第一次第一段等待逾時要印的一行。數字在逾時的當下記下來，等這一張處理完才印：
    // 放棄的那一支要先把 slot 交還 driver，不讓 log 的格式化擋在前面
    struct {
      bool pending = false;
      bool waiter_error = false;  ///< fence_waiter_t 本身出錯（沒有真的等、也沒有讀完成值），不是逾時
      uint64_t generation = 0;
      uint64_t frame_id = 0;
      uint64_t expected = 0;
      uint64_t completed = 0;
      int64_t waited_qpc = 0;
    } first_timeout;
    auto flush_first_timeout = [&]() {
      if (!first_timeout.pending) {
        return;
      }
      first_timeout.pending = false;
      BOOST_LOG(info) << "[VIPLE-VR-CAP] first fence timeout gen="sv << first_timeout.generation << " frame="sv << first_timeout.frame_id
                      << " expected="sv << first_timeout.expected << " completed="sv << first_timeout.completed
                      << " waitedMs="sv << std::format("{:.3f}", qpc_to_ms(first_timeout.waited_qpc))
                      << " mode="sv << (late_enabled ? "late"sv : "legacy"sv)
                      << (first_timeout.waiter_error ? " result=error"sv : ""sv);
    };

    // 每次要求拆線印一行（why＝legacy-1s／stall／starve／device-lost）。開關關著時也印：sinceAdvanceMs 很小而
    // sinceCopyMs 很大＝完成值一直在前進、只是每一張都比等待上限晚（2026-10-09 的事故）；兩個都大＝fence 真的不動
    auto log_teardown = [&](std::string_view why, uint64_t expected, uint64_t completed, int64_t now_qpc, int64_t next_allowed_ns) {
      const auto g = health_.diag(platf::qpc_ticks_to_ns(now_qpc));
      BOOST_LOG(warning) << "[VIPLE-VR-CAP] teardown request gen="sv << (cs && cs->src ? cs->src->generation : 0) << " why="sv << why
                         << " expected="sv << expected << " completed="sv << completed
                         << " sinceAdvanceMs="sv << g.since_advance_ns / 1'000'000 << " sinceCopyMs="sv << g.since_copy_ns / 1'000'000
                         << " published="sv << g.published << " copied="sv << g.copied
                         << " nextAllowedMs="sv << next_allowed_ns / 1'000'000;
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
      double e50, e95, p50, p95, a50, a95, f50, f95, pf50, pf95;
      const double fmax = w.fence_wait_ms.empty() ? 0.0 : *std::max_element(w.fence_wait_ms.begin(), w.fence_wait_ms.end());
      const double pfmax = w.present_to_fence_ms.empty() ? 0.0 : *std::max_element(w.present_to_fence_ms.begin(), w.present_to_fence_ms.end());
      percentiles(std::move(w.evt_to_push_ms), e50, e95);
      percentiles(std::move(w.present_to_push_ms), p50, p95);
      percentiles(std::move(w.echo_age_ms), a50, a95);
      percentiles(std::move(w.fence_wait_ms), f50, f95);
      percentiles(std::move(w.present_to_fence_ms), pf50, pf95);
      // §VR-FENCE-LATE（2026-10-09）：late 起的欄位都加在行尾，前面的欄位一個字都不動（有分析腳本在讀）。
      // fenceWait＝複製成功的每一張從被叫醒到 fence 到值（含準時的）。fence 晚到超過一個發布間隔時，下一張被取用的
      // 時刻就是上一張做完的時刻，這個數字會貼在發布間隔上，不代表「發布後晚多久」——那要看 presentToFence
      // （present_qpc → 到值）。overPeriod＝複製成功的張數裡 fenceWait 達到一個顯示週期的（評估過載的定義用）。
      // overload＝發布給 stream.cpp 的過載旗標；budgetMs＝目前的等待預算（開關關著印 0）
      BOOST_LOG(info) << "[VIPLE-VR-CAP] 10s: gen="sv << (cs && cs->src ? cs->src->generation : 0)
                      << " copied="sv << w.copied << " skipped="sv << w.skipped << " fenceTimeout="sv << w.fence_timeout
                      << " invalid="sv << w.invalid << " stale="sv << w.stale << " black="sv << w.black << " released="sv << w.released
                      << " evtToPush p50="sv << std::format("{:.3f}", e50) << "ms p95="sv << std::format("{:.3f}", e95)
                      << "ms presentToPush p50="sv << std::format("{:.3f}", p50) << "ms p95="sv << std::format("{:.3f}", p95)
                      << "ms echoAge p50="sv << std::format("{:.3f}", a50) << "ms p95="sv << std::format("{:.3f}", a95) << "ms"sv
                      << " late="sv << w.late << " fenceWait p50="sv << std::format("{:.3f}", f50) << "ms p95="sv << std::format("{:.3f}", f95)
                      << "ms max="sv << std::format("{:.3f}", fmax) << "ms teardownReq="sv << w.teardown_requests
                      << " overload="sv << (overload_pub ? 1 : 0)
                      << " budgetMs="sv << (late_enabled ? (health_.wait_budget_ns() + 500'000) / 1'000'000 : 0)
                      << " presentToFence p50="sv << std::format("{:.3f}", pf50) << "ms p95="sv << std::format("{:.3f}", pf95)
                      << "ms max="sv << std::format("{:.3f}", pfmax) << "ms overPeriod="sv << w.over_period;
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
        clear_health("no-frame-source"sv);  // §VR-FENCE-LATE：沒有 frame source 時快照是預設值（已經是就什麼都不做）
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
        // §VR-FENCE-LATE：上一個 generation 發布的快照到此為止。新的還沒開起來（開失敗會一直重試）的期間也是預設值，
        // 不留著舊的過載旗標擋住 LATCH／PREDICT
        clear_health("new-generation"sv);
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
          if (!keepalive(false)) {
            return capture_e::ok;  // 一直開不起來時也要停得下來（§VR-FENCE-LATE；開關關著時這一行沒有作用）
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
        // §VR-FENCE-LATE：新 generation 的 ring／fence 都是新的——發布間隔、完成值、卡死／餓死、過載全部重來（退避不動）
        health_gen = cur->generation;
        health_.begin_generation(platf::qpc_ticks_to_ns(platf::qpc_counter()));
        // 開完先「排空」：上一個消費者沒交還的 slot 直接放回（不複製；避免拿到早就過期的幀）。
        // §VR-RING-RELEASE：不能用 read_latest()——停過 1 s 以上的那一筆會被判不合格而不交還，driver 就沒有 slot 可用
        const uint64_t baseline = cs->last_consumed;
        const uint64_t drained = cs->reader->release_latest();
        if (drained) {
          signal_consumed(drained);
          count(&stats_t::released);
        }
        BOOST_LOG(info) << "[VIPLE-VR-CAP] opened gen="sv << cur->generation << " ring="sv << width << 'x' << height
                        << " released="sv << (drained > baseline ? drained - baseline : 0);
      }

      // ── 等下一幀 ─────────────────────────────────────────────────────────────
      const DWORD w = WaitForSingleObject(static_cast<HANDLE>(cs->src->evt_frm), 100);
      if (w != WAIT_OBJECT_0) {
        // §VR-RING-RELEASE：100 ms 沒有新幀時，把 ring 裡還沒交還的幀（例：被判不合格）放回，免得 driver 一直沒 slot
        if (const uint64_t f = cs->reader->release_latest()) {
          signal_consumed(f);
          count(&stats_t::released);
        }
        {
          // §VR-FENCE-LATE：沒有影像時也讓最近 1 s 的視窗往前走（過載旗標才清得掉），快照照常更新
          const int64_t idle_qpc = platf::qpc_counter();
          health_.on_idle(platf::qpc_ticks_to_ns(idle_qpc));
          publish_health(idle_qpc);
          last_push_cb_qpc = idle_qpc;  // 下面兩條路都會呼叫回呼
        }
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

      // 下面幾條不交影像就 continue 的路徑都經過 keepalive()（§VR-FENCE-LATE；開關關著時它什麼都不做）：
      // evtFrm 一直有訊號、卻沒有一張走到複製（不合格與過期交錯出現、時間戳寫得太舊）時，舊程式永遠不會呼叫回呼
      vripc_frame_desc_t d {};
      switch (cs->reader->read_latest(d)) {
        case vr::bridge::frame_reader_t::result_e::ok:
          break;
        case vr::bridge::frame_reader_t::result_e::invalid:
          count(&stats_t::invalid);
          if (!keepalive(true)) {
            return capture_e::ok;
          }
          continue;
        case vr::bridge::frame_reader_t::result_e::torn:
          count(&stats_t::torn);
          if (!keepalive(true)) {
            return capture_e::ok;
          }
          continue;
        case vr::bridge::frame_reader_t::result_e::none:
          if (!keepalive(true)) {
            return capture_e::ok;
          }
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
        if (!keepalive(true)) {
          return capture_e::ok;
        }
        continue;
      }

      // §B.7：CPU 端確認 driver 的 GPU 寫完（不 GPU Wait 對方的 fence，不變式 8）。
      // observed＝等待結束時讀到的完成值（waiter 本來就會讀，這裡只是多帶出來）
      using fence_result_e = vr::bridge::fence_waiter_t::result_e;
      uint64_t observed = 0;
      auto fr = cs->waiter->wait(cs->shared_fence.get(), d.fence_value, fence_wait_ms(client_frame_rate), &observed);
      bool late = false;  ///< §VR-FENCE-LATE：第一段逾時、在等待預算內到值
      bool gen_gone = false;  ///< §VR-FENCE-LATE：繼續等的期間這個 generation 被換掉了
      if (fr != fence_result_e::reached && fr != fence_result_e::lost) {
        if (!cs->first_timeout_logged) {
          // 每個 generation 的第一次逾時記下期望值與當下的完成值（2026-10-09 的事故沒有這兩個數字，只能反推）。
          // 這裡只記數字，等這一張處理完才印（flush_first_timeout）
          cs->first_timeout_logged = true;
          first_timeout.pending = true;
          first_timeout.waiter_error = fr == fence_result_e::error;
          first_timeout.generation = cs->src->generation;
          first_timeout.frame_id = d.frame_id;
          first_timeout.expected = d.fence_value;
          first_timeout.completed = observed;
          first_timeout.waited_qpc = platf::qpc_counter() - evt_qpc;
        }
        if (late_enabled && fr == fence_result_e::timeout) {
          // §VR-FENCE-LATE：fence 是「晚到」不是「不來」。同一條 GPU 命令流裡的 Signal 依序完成，丟掉這一張不會讓下一張
          // 變快，只是把一張馬上就要好的影像丟掉。繼續在 CPU 端等同一個值（不變式 8 不變），到值就照常複製；
          // 從被叫醒起累計超過等待預算才放棄。每 k_late_slice_ms 醒來一次，這個 generation 已經被換掉（拆線、
          // driver 重連）就不必再等。這個迴圈裡不碰 push／pull 回呼。
          const int64_t budget_ns = health_.wait_budget_ns();
          for (;;) {
            const int64_t waited_ns = platf::qpc_ticks_to_ns(platf::qpc_counter() - evt_qpc);
            if (waited_ns >= budget_ns) {
              break;
            }
            const auto slice_ms = (uint32_t) std::clamp<int64_t>((budget_ns - waited_ns + 999'999) / 1'000'000, 1, k_late_slice_ms);
            fr = cs->waiter->wait(cs->shared_fence.get(), d.fence_value, slice_ms, &observed);
            if (fr != fence_result_e::timeout) {
              break;  // 到值、lost，或 waiter 本身出錯（它不會等，再繞只是空轉）
            }
            const auto now_src = vr::bridge::frame_source();
            if (!now_src || now_src->generation != cs->src->generation || !now_src->alive()) {
              gen_gone = true;
              break;
            }
          }
          late = fr == fence_result_e::reached;
        }
        if (fr == fence_result_e::reached || fr == fence_result_e::lost || gen_gone) {
          flush_first_timeout();  // 這一張不走放棄那一支（晚到照收、lost、generation 換掉）：沒有 slot 要先交還，現在就印
        }
      }
      if (fr == fence_result_e::lost) {
        count(&stats_t::fence_lost);
        if (device->GetDeviceRemovedReason() != S_OK) {
          return capture_e::reinit;  // 自己的 device 掉了
        }
        log_teardown("device-lost"sv, d.fence_value, observed, platf::qpc_counter(), 0);
        vr::bridge::request_teardown(cs->src->generation, VRIPC_BYE_DEVICE_LOST);  // driver 端問題 → 新 generation
        count(&stats_t::teardown_requests);
        abandoned_gen = cs->src->generation;
        cs.reset();
        continue;
      }
      if (gen_gone) {
        // 這一張不複製、不算逾時、也不餵健康判定（不算進餓死的計時：這個 generation 本來就要被換掉）。
        // 10 s 行的任何欄位都不計它（每換一次 generation 最多一張）。迴圈開頭會放掉舊的 ring／fence、重開新的。
        continue;
      }
      if (fr != fence_result_e::reached) {
        count(&stats_t::fence_timeout);
        signal_consumed(d.fence_value);  // 不複製，但仍放回 slot
        const int64_t now = platf::qpc_counter();
        flush_first_timeout();  // slot 交還、時刻也讀了之後才印
        // 健康判定：開關關著時只記數（拆線那一行 log 要用），不判定
        const auto verdict = feed(vr::capture::outcome_e::abandoned, d, observed, now - evt_qpc, now);
        if (!late_enabled) {
          // 舊規則（vr_capture_late = disabled）：連續逾時、距第一次逾時滿 1 s 就拆
          if (cs->fence_timeout_since == 0) {
            cs->fence_timeout_since = now;
          } else if (now - cs->fence_timeout_since >= platf::qpc_frequency()) {
            log_teardown("legacy-1s"sv, d.fence_value, observed, now, 0);
            vr::bridge::request_teardown(cs->src->generation, VRIPC_BYE_PROTOCOL_ERROR);  // 連續逾時累計 ≥ 1 s
            count(&stats_t::teardown_requests);
            abandoned_gen = cs->src->generation;
            cs.reset();
          }
          continue;
        }
        if (verdict.teardown != vr::capture::teardown_e::none) {
          // §VR-FENCE-LATE：完成值 3 s 不動（stall）或 5 s 沒有一張複製成功（starve），而且過了退避期。
          // BYE 代碼沿用 PROTOCOL_ERROR（driver 不必改）；下一次最早 nextAllowedMs 之後才會再拆。
          log_teardown(vr::capture::teardown_name(verdict.teardown), d.fence_value, observed, now, verdict.allowed_in_ns);
          vr::bridge::request_teardown(cs->src->generation, VRIPC_BYE_PROTOCOL_ERROR);
          count(&stats_t::teardown_requests);
          abandoned_gen = cs->src->generation;
          cs.reset();
          continue;
        }
        if (verdict.deferred != vr::capture::teardown_e::none && !cs->deferred_logged) {
          cs->deferred_logged = true;  // 每個 generation 只印一次
          BOOST_LOG(info) << "[VIPLE-VR-CAP] teardown deferred gen="sv << cs->src->generation << " why="sv << vr::capture::teardown_name(verdict.deferred)
                          << " allowedInMs="sv << verdict.allowed_in_ns / 1'000'000 << " backoffStep="sv << health_.backoff().step;
        }
        publish_health(now);
        // 放棄可以連續很久（判定成立前 3～5 s、退避期內最長 30 s），這條路徑沒有影像可交：定期讓 captureThread 檢查停止。
        // 已經離開等 fence 的迴圈，slot 也交還了
        if (!keepalive(false)) {
          return capture_e::ok;
        }
        continue;
      }
      cs->fence_timeout_since = 0;
      const int64_t fence_qpc = platf::qpc_counter();  // fence 到值的時刻（fenceWait 的終點）

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
      const bool keep_going = push_captured_image_cb(std::move(img_base), true);
      last_push_cb_qpc = push_qpc;
      // §VR-FENCE-LATE：這個功能加的量測與健康判定全部放在交出影像之後——準時那條路徑在 push 之前只多讀一次時鐘
      // （fence_qpc）。新的樣本另開一個鎖區段記（配置記憶體也在這裡，不在 push 之前）。
      {
        std::lock_guard lk {stats_mtx_};
        const double fw = qpc_to_ms(fence_qpc - evt_qpc);
        const double pf = qpc_to_ms(fence_qpc - d.present_qpc);
        const bool over_period = period_qpc > 0 && fence_qpc - evt_qpc >= period_qpc;
        window_.fence_wait_ms.push_back(fw);
        window_.present_to_fence_ms.push_back(pf);
        window_.late += late ? 1 : 0;
        window_.over_period += over_period ? 1 : 0;
        stats_.late += late ? 1 : 0;
        stats_.over_period += over_period ? 1 : 0;
        if (stats_.fence_wait_ms.size() < k_stats_samples_max) {
          stats_.fence_wait_ms.push_back(fw);
          stats_.present_to_fence_ms.push_back(pf);
        }
      }
      // 複製成功的影像不會觸發拆線，回覆只可能帶「退避歸零」（feed 裡印）
      feed(late ? vr::capture::outcome_e::late : vr::capture::outcome_e::on_time, d, observed, fence_qpc - evt_qpc, push_qpc);
      publish_health(push_qpc);
      if (!keep_going) {
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
