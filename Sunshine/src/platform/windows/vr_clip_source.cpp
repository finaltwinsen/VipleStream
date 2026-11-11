/**
 * @file src/platform/windows/vr_clip_source.cpp
 * @brief VipleStream §VR-NETBENCH：VR 網路測試影片來源元件（MF 硬體解碼、D3D11 VideoProcessor 色彩空間轉換與縮放、背景解碼與跨行程共享貼圖輪替）。
 *
 * 需連結的程式庫（由專案 CMake 配置）：
 * - mfplat
 * - mfreadwrite
 * - mfuuid
 * - d3d11
 * - dxgi
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "vr_clip_source.h"

// platform includes
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propidl.h>
#include <propvarutil.h>
#include <wrl/client.h>

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// local includes
#include "src/logging.h"

namespace platf::dxgi {

  using Microsoft::WRL::ComPtr;

  namespace {

    /// @brief 將 HRESULT 轉為 16 進位字串格式（例如 0x80004005）。
    std::string format_hr(HRESULT hr) {
      std::ostringstream oss;
      oss << "0x" << std::hex << std::setfill('0') << std::setw(8) << static_cast<uint32_t>(hr);
      return oss.str();
    }

    /// @brief 取得檔案名稱（UTF-8 字串）。
    std::string extract_filename(const std::wstring &path) {
      try {
        return std::filesystem::path(path).filename().string();
      } catch (...) {
        std::wstring ws = std::filesystem::path(path).filename().wstring();
        return std::string(ws.begin(), ws.end());
      }
    }

    // Media Foundation 單次初始化保護旗標與結果
    std::once_flag g_mf_init_flag;
    HRESULT g_mf_init_hr = S_OK;

    /// @brief 確保 Media Foundation 在本行程中完成一次初始化。
    bool ensure_mf_initialized() {
      std::call_once(g_mf_init_flag, []() {
        g_mf_init_hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(g_mf_init_hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] MFStartup failed: hr=" << format_hr(g_mf_init_hr);
        }
      });
      return SUCCEEDED(g_mf_init_hr);
    }

    /// @brief VideoProcessor 輸入檢視（InputView）的快取索引鍵。
    struct input_view_key_t {
      ID3D11Texture2D *tex = nullptr;
      UINT subresource = 0;

      bool operator==(const input_view_key_t &other) const {
        return tex == other.tex && subresource == other.subresource;
      }
    };

    /// @brief 共享貼圖槽位，包含跨行程 NT handle、貼圖介面、Keyed Mutex 與視訊處理輸出檢視。
    struct ring_slot_t {
      HANDLE shared_handle = nullptr;
      ComPtr<ID3D11Texture2D> shared_texture;
      ComPtr<IDXGIKeyedMutex> keyed_mutex;
      ComPtr<ID3D11VideoProcessorOutputView> output_view;
    };

  }  // namespace

  struct vr_clip_source_t::impl_t {
    // 跨行程共享資源與同步物件輪替槽位
    std::vector<ring_slot_t> slots_;

    // D3D11 裝置與視訊處理物件
    ComPtr<ID3D11Device> d3d_device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> video_enum_;
    ComPtr<ID3D11VideoProcessor> video_proc_;

    // Media Foundation 物件
    ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
    ComPtr<IMFSourceReader> source_reader_;

    // 輸入檢視快取（最多 32 個，LRU 策略）
    std::vector<std::pair<input_view_key_t, ComPtr<ID3D11VideoProcessorInputView>>> input_view_cache_;

    // 軟體解碼備用路徑之動態 NV12 貼圖
    ComPtr<ID3D11Texture2D> dynamic_nv12_tex_;

    // open() 探測解碼器類型時預讀的第一個樣本（若成功取得）
    ComPtr<IMFSample> pending_sample_;

    // 影片屬性與計數（供擷取端多執行緒讀取，使用 atomic）
    std::atomic<uint32_t> clip_width_{0};
    std::atomic<uint32_t> clip_height_{0};
    std::atomic<double> clip_fps_{0.0};
    uint32_t out_w_ = 0;
    uint32_t out_h_ = 0;
    std::atomic<uint64_t> loops_{0};
    std::atomic<int64_t> last_ts_100ns_{-1};  ///< 最近一個樣本在片段裡的時間（100 ns；診斷用）
    std::atomic<uint64_t> decoded_{0};        ///< 背景解碼完成張數累計

    // 解碼器狀態
    bool is_hardware_ = false;
    bool warned_software_ = false;

    // 背景解碼執行緒與生命週期旗標
    std::thread decode_thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> failed_{false};

    // 單張解碼耗時統計（讀樣本＋Blt）
    std::mutex decode_ms_mutex_;
    double decode_ms_max_ = 0.0;

    ~impl_t() {
      // 1. 設停止旗標並等待背景解碼執行緒結束
      stop_flag_.store(true, std::memory_order_release);
      if (decode_thread_.joinable()) {
        decode_thread_.join();
      }

      // 2. 清理輸入快取（明確 Reset ComPtr）
      for (auto &item : input_view_cache_) {
        item.second.Reset();
      }
      input_view_cache_.clear();

      dynamic_nv12_tex_.Reset();
      pending_sample_.Reset();

      // 3. 關閉所有 slot 的 NT handle 並明確釋放 COM 資源
      for (auto &slot : slots_) {
        slot.output_view.Reset();
        slot.keyed_mutex.Reset();
        slot.shared_texture.Reset();
        if (slot.shared_handle) {
          CloseHandle(slot.shared_handle);
          slot.shared_handle = nullptr;
        }
      }
      slots_.clear();

      video_proc_.Reset();
      video_enum_.Reset();
      video_context_.Reset();
      video_device_.Reset();

      source_reader_.Reset();
      dxgi_manager_.Reset();

      d3d_context_.Reset();
      d3d_device_.Reset();
    }

    /// @brief 紀錄單張（讀樣本＋Blt）解碼耗時。
    void record_decode_ms(double ms) {
      std::lock_guard<std::mutex> lock(decode_ms_mutex_);
      if (ms > decode_ms_max_) {
        decode_ms_max_ = ms;
      }
    }

    /// @brief 取得並重設單張解碼最長耗時。
    double decode_ms_max_and_reset() {
      std::lock_guard<std::mutex> lock(decode_ms_mutex_);
      double val = decode_ms_max_;
      decode_ms_max_ = 0.0;
      return val;
    }

    /// @brief 初始化元件與資源，並啟動背景解碼執行緒。
    bool init(LUID adapter_luid, const std::wstring &path, uint32_t out_w, uint32_t out_h, int ring) {
      if (ring <= 0) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] Invalid ring size: " << ring << ", must be positive";
        return false;
      }

      out_w_ = out_w;
      out_h_ = out_h;

      // 1. 列舉 DXGI 顯示卡並比對 LUID
      ComPtr<IDXGIFactory1> factory;
      HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] CreateDXGIFactory1 failed: " << format_hr(hr);
        return false;
      }

      ComPtr<IDXGIAdapter1> matched_adapter;
      for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        if (FAILED(hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] EnumAdapters1 failed at index " << i << ": " << format_hr(hr);
          break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
          if (desc.AdapterLuid.LowPart == adapter_luid.LowPart &&
              desc.AdapterLuid.HighPart == adapter_luid.HighPart) {
            matched_adapter = adapter;
            adapter.Reset();
            break;
          }
        }
      }

      if (!matched_adapter) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] Target adapter LUID not found in system";
        return false;
      }

      // 2. 建立具備 VIDEO_SUPPORT 與 BGRA_SUPPORT 的 D3D11 裝置
      const UINT create_flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
      const D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
      };
      D3D_FEATURE_LEVEL chosen_feature_level{};

      hr = D3D11CreateDevice(
        matched_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        create_flags,
        feature_levels,
        static_cast<UINT>(std::size(feature_levels)),
        D3D11_SDK_VERSION,
        &d3d_device_,
        &chosen_feature_level,
        &d3d_context_
      );
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] D3D11CreateDevice failed: " << format_hr(hr);
        return false;
      }

      // 3. 設定多執行緒保護（ID3D10Multithread）
      ComPtr<ID3D10Multithread> multithread;
      hr = d3d_device_.As(&multithread);
      if (SUCCEEDED(hr) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
      } else {
        BOOST_LOG(warning) << "[VIPLE-VR-CLIP] Failed to query ID3D10Multithread: " << format_hr(hr);
      }

      // 4. 取得視訊介面
      hr = d3d_device_.As(&video_device_);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] Query ID3D11VideoDevice failed: " << format_hr(hr);
        return false;
      }
      hr = d3d_context_.As(&video_context_);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] Query ID3D11VideoContext failed: " << format_hr(hr);
        return false;
      }

      // 5. 建立 IMFDXGIDeviceManager 並 ResetDevice
      UINT reset_token = 0;
      hr = MFCreateDXGIDeviceManager(&reset_token, &dxgi_manager_);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] MFCreateDXGIDeviceManager failed: " << format_hr(hr);
        return false;
      }
      hr = dxgi_manager_->ResetDevice(d3d_device_.Get(), reset_token);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] IMFDXGIDeviceManager::ResetDevice failed: " << format_hr(hr);
        return false;
      }

      // 6. 設定 Source Reader 屬性並從 URL 建立 Source Reader
      ComPtr<IMFAttributes> reader_attrs;
      hr = MFCreateAttributes(&reader_attrs, 4);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] MFCreateAttributes failed: " << format_hr(hr);
        return false;
      }
      reader_attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgi_manager_.Get());
      reader_attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
      reader_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE);
      reader_attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

      hr = MFCreateSourceReaderFromURL(path.c_str(), reader_attrs.Get(), &source_reader_);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] MFCreateSourceReaderFromURL failed: " << format_hr(hr);
        return false;
      }

      // 7. 選擇第一條影片串流
      hr = source_reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "[VIPLE-VR-CLIP] Deselect all streams failed: " << format_hr(hr);
      }
      hr = source_reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] Select first video stream failed: " << format_hr(hr);
        return false;
      }

      // 8. 嘗試將輸出型態設定為 NV12
      ComPtr<IMFMediaType> nv12_type;
      hr = MFCreateMediaType(&nv12_type);
      if (SUCCEEDED(hr)) {
        nv12_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        nv12_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = source_reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12_type.Get());
        if (FAILED(hr)) {
          BOOST_LOG(warning) << "[VIPLE-VR-CLIP] Setting NV12 output failed: " << format_hr(hr)
                             << ", falling back to native decoder format";
        }
      }

      // 9. 讀取目前生效的媒體型態與影片尺寸、FPS
      ComPtr<IMFMediaType> cur_type;
      hr = source_reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur_type);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] GetCurrentMediaType failed: " << format_hr(hr);
        return false;
      }
      UINT32 init_w = 0, init_h = 0;
      hr = MFGetAttributeSize(cur_type.Get(), MF_MT_FRAME_SIZE, &init_w, &init_h);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] MFGetAttributeSize failed: " << format_hr(hr);
        return false;
      }
      clip_width_.store(init_w, std::memory_order_relaxed);
      clip_height_.store(init_h, std::memory_order_relaxed);

      UINT32 fps_num = 0, fps_den = 0;
      if (SUCCEEDED(MFGetAttributeRatio(cur_type.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den)) && fps_den > 0) {
        clip_fps_.store(static_cast<double>(fps_num) / static_cast<double>(fps_den), std::memory_order_relaxed);
      }

      // 10. 建立 ring 張跨行程共享貼圖（BGRA8_UNORM、NTHANDLE | KEYEDMUTEX）
      slots_.resize(ring);
      D3D11_TEXTURE2D_DESC tex_desc{};
      tex_desc.Width = out_w_;
      tex_desc.Height = out_h_;
      tex_desc.MipLevels = 1;
      tex_desc.ArraySize = 1;
      tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      tex_desc.SampleDesc.Count = 1;
      tex_desc.SampleDesc.Quality = 0;
      tex_desc.Usage = D3D11_USAGE_DEFAULT;
      tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      tex_desc.CPUAccessFlags = 0;
      tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

      for (int i = 0; i < ring; ++i) {
        auto &slot = slots_[i];
        hr = d3d_device_->CreateTexture2D(&tex_desc, nullptr, &slot.shared_texture);
        if (FAILED(hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] Create shared texture failed on slot " << i << ": " << format_hr(hr);
          return false;
        }

        ComPtr<IDXGIResource1> res1;
        hr = slot.shared_texture.As(&res1);
        if (FAILED(hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] Query IDXGIResource1 failed on slot " << i << ": " << format_hr(hr);
          return false;
        }

        hr = res1->CreateSharedHandle(
          nullptr,
          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
          nullptr,
          &slot.shared_handle
        );
        if (FAILED(hr) || !slot.shared_handle) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] CreateSharedHandle failed on slot " << i << ": " << format_hr(hr);
          return false;
        }

        hr = slot.shared_texture.As(&slot.keyed_mutex);
        if (FAILED(hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] Query IDXGIKeyedMutex failed on slot " << i << ": " << format_hr(hr);
          return false;
        }
      }

      // 11. 建立視訊處理器與各槽位之輸出檢視
      if (!setup_video_processor()) {
        return false;
      }

      // 12. 預讀第一個樣本以檢測硬體或軟體解碼
      probe_decoder_type();

      // 13. open 成功紀錄日誌（包含 ring 數量）
      const std::string filename = extract_filename(path);
      BOOST_LOG(info) << "[VIPLE-VR-CLIP] opened " << filename << " "
                      << clip_width_.load(std::memory_order_relaxed) << "x"
                      << clip_height_.load(std::memory_order_relaxed) << " @"
                      << clip_fps_.load(std::memory_order_relaxed)
                      << " -> out " << out_w_ << "x" << out_h_
                      << " ring=" << slots_.size()
                      << " (decoder: " << (is_hardware_ ? "hardware" : "software") << ")";

      // 14. 啟動背景解碼執行緒
      decode_thread_ = std::thread([this]() {
        decode_loop();
      });

      return true;
    }

    /// @brief 設定與初始化 ID3D11VideoProcessor 及所有槽位對應的輸出檢視。
    bool setup_video_processor() {
      input_view_cache_.clear();
      for (auto &slot : slots_) {
        slot.output_view.Reset();
      }
      video_proc_.Reset();
      video_enum_.Reset();

      const uint32_t cur_w = clip_width_.load(std::memory_order_relaxed);
      const uint32_t cur_h = clip_height_.load(std::memory_order_relaxed);

      D3D11_VIDEO_PROCESSOR_CONTENT_DESC cp_desc{};
      cp_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
      cp_desc.InputWidth = cur_w;
      cp_desc.InputHeight = cur_h;
      cp_desc.OutputWidth = out_w_;
      cp_desc.OutputHeight = out_h_;
      cp_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

      HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&cp_desc, &video_enum_);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] CreateVideoProcessorEnumerator failed: " << format_hr(hr);
        return false;
      }

      hr = video_device_->CreateVideoProcessor(video_enum_.Get(), 0, &video_proc_);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] CreateVideoProcessor failed: " << format_hr(hr);
        return false;
      }

      // 為每個槽位建立共享貼圖上的輸出檢視（OutputView）
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view_desc{};
      out_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      out_view_desc.Texture2D.MipSlice = 0;

      for (size_t i = 0; i < slots_.size(); ++i) {
        hr = video_device_->CreateVideoProcessorOutputView(
          slots_[i].shared_texture.Get(),
          video_enum_.Get(),
          &out_view_desc,
          &slots_[i].output_view
        );
        if (FAILED(hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] CreateVideoProcessorOutputView failed on slot " << i << ": " << format_hr(hr);
          return false;
        }
      }

      // 設定色彩空間
      apply_color_space();

      // 設定來源矩形與目的矩形（整張填滿，不保持長寬比）
      RECT src_rect = { 0, 0, static_cast<LONG>(cur_w), static_cast<LONG>(cur_h) };
      RECT dst_rect = { 0, 0, static_cast<LONG>(out_w_), static_cast<LONG>(out_h_) };
      video_context_->VideoProcessorSetStreamSourceRect(video_proc_.Get(), 0, TRUE, &src_rect);
      video_context_->VideoProcessorSetStreamDestRect(video_proc_.Get(), 0, TRUE, &dst_rect);
      video_context_->VideoProcessorSetOutputTargetRect(video_proc_.Get(), TRUE, &dst_rect);

      return true;
    }

    /// @brief 套用色彩空間：輸入 BT.709 Limited Range，輸出 RGB Full Range。
    void apply_color_space() {
      ComPtr<ID3D11VideoContext1> video_context1;
      if (SUCCEEDED(d3d_context_.As(&video_context1)) && video_context1) {
        video_context1->VideoProcessorSetStreamColorSpace1(video_proc_.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        video_context1->VideoProcessorSetOutputColorSpace1(video_proc_.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      } else {
        // 舊版 ID3D11VideoContext 降級路徑
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
        in_cs.Usage = 0;              // Playback
        in_cs.RGB_Range = 0;
        in_cs.YCbCr_Matrix = 1;       // BT.709
        in_cs.YCbCr_xvYCC = 0;
        in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235; // Limited range (Studio)
        video_context_->VideoProcessorSetStreamColorSpace(video_proc_.Get(), 0, &in_cs);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
        out_cs.Usage = 0;
        out_cs.RGB_Range = 0;         // 0: 0-255 Full range
        out_cs.YCbCr_Matrix = 1;      // BT.709
        out_cs.YCbCr_xvYCC = 0;
        out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        video_context_->VideoProcessorSetOutputColorSpace(video_proc_.Get(), &out_cs);
      }
    }

    /// @brief 預讀第一個樣本以探測解碼器是否為硬體解碼。
    void probe_decoder_type() {
      is_hardware_ = false;
      for (int retry = 0; retry < 8; ++retry) {
        DWORD stream_flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = source_reader_->ReadSample(
          MF_SOURCE_READER_FIRST_VIDEO_STREAM,
          0,
          nullptr,
          &stream_flags,
          &timestamp,
          &sample
        );
        if (FAILED(hr)) {
          BOOST_LOG(warning) << "[VIPLE-VR-CLIP] Probe ReadSample failed: " << format_hr(hr);
          break;
        }

        if (stream_flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
          handle_media_type_changed();
        }

        if (sample) {
          ComPtr<IMFMediaBuffer> media_buf;
          if (SUCCEEDED(sample->GetBufferByIndex(0, &media_buf)) && media_buf) {
            ComPtr<IMFDXGIBuffer> dxgi_buf;
            if (SUCCEEDED(media_buf.As(&dxgi_buf)) && dxgi_buf) {
              ComPtr<ID3D11Texture2D> tex;
              if (SUCCEEDED(dxgi_buf->GetResource(IID_PPV_ARGS(&tex))) && tex) {
                is_hardware_ = true;
              }
            }
          }
          pending_sample_ = sample;
          sample.Reset();
          break;
        }
      }
    }

    /// @brief 當串流媒體型態改變時重新讀取尺寸並重建處理器。
    bool handle_media_type_changed() {
      ComPtr<IMFMediaType> cur_type;
      HRESULT hr = source_reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur_type);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] GetCurrentMediaType after change failed: " << format_hr(hr);
        return false;
      }

      UINT32 new_w = 0, new_h = 0;
      if (SUCCEEDED(MFGetAttributeSize(cur_type.Get(), MF_MT_FRAME_SIZE, &new_w, &new_h))) {
        if (new_w != clip_width_.load(std::memory_order_relaxed) ||
            new_h != clip_height_.load(std::memory_order_relaxed)) {
          clip_width_.store(new_w, std::memory_order_relaxed);
          clip_height_.store(new_h, std::memory_order_relaxed);
          if (!setup_video_processor()) {
            return false;
          }
        }
      }

      UINT32 fps_num = 0, fps_den = 0;
      if (SUCCEEDED(MFGetAttributeRatio(cur_type.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den)) && fps_den > 0) {
        clip_fps_.store(static_cast<double>(fps_num) / static_cast<double>(fps_den), std::memory_order_relaxed);
      }

      return true;
    }

    /// @brief 取得或建立輸入檢視（快取最多 32 個，LRU 淘汰）。
    ComPtr<ID3D11VideoProcessorInputView> get_or_create_input_view(ID3D11Texture2D *tex, UINT subresource) {
      input_view_key_t key{tex, subresource};
      for (auto it = input_view_cache_.begin(); it != input_view_cache_.end(); ++it) {
        if (it->first == key) {
          auto found = it->second;
          if (it != input_view_cache_.begin()) {
            auto pair = *it;
            it->second.Reset();
            input_view_cache_.erase(it);
            input_view_cache_.insert(input_view_cache_.begin(), pair);
            pair.second.Reset();
          }
          return found;
        }
      }

      // 未命中，建立新檢視
      D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv_desc{};
      iv_desc.FourCC = 0;
      iv_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
      iv_desc.Texture2D.MipSlice = 0;
      iv_desc.Texture2D.ArraySlice = subresource;

      ComPtr<ID3D11VideoProcessorInputView> view;
      HRESULT hr = video_device_->CreateVideoProcessorInputView(tex, video_enum_.Get(), &iv_desc, &view);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] CreateVideoProcessorInputView failed: " << format_hr(hr);
        return nullptr;
      }

      if (input_view_cache_.size() >= 32) {
        input_view_cache_.back().second.Reset();
        input_view_cache_.pop_back();
      }
      input_view_cache_.insert(input_view_cache_.begin(), {key, view});
      return view;
    }

    /// @brief 解碼單個有效樣本並透過視訊處理器輸出至目標槽位。
    bool decode_one_frame(ring_slot_t &slot) {
      ComPtr<IMFSample> sample;

      // 若 open() 時有預讀樣本，優先消耗
      if (pending_sample_) {
        // 明確 Reset：MinGW 的 WRL ComPtr 移動後不保證把來源清成 null
        sample = pending_sample_;
        pending_sample_.Reset();
      }

      // 讀取下一個有效樣本（處理 null 樣本重試最多 8 次及 EOS 重頭循環）
      int null_retries = 0;
      int eos_retries = 0;
      while (!sample) {
        if (stop_flag_.load(std::memory_order_relaxed)) {
          return false;
        }

        DWORD stream_flags = 0;
        LONGLONG timestamp = 0;
        HRESULT hr = source_reader_->ReadSample(
          MF_SOURCE_READER_FIRST_VIDEO_STREAM,
          0,
          nullptr,
          &stream_flags,
          &timestamp,
          &sample
        );
        if (FAILED(hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] ReadSample failed: " << format_hr(hr);
          return false;
        }
        if (sample) {
          last_ts_100ns_.store(timestamp, std::memory_order_relaxed);
        }

        if (stream_flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
          handle_media_type_changed();
        }

        if (stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) {
          // 到結尾自動循環：將讀取位置重設至起點
          PROPVARIANT var;
          PropVariantInit(&var);
          var.vt = VT_I8;
          var.hVal.QuadPart = 0;
          hr = source_reader_->SetCurrentPosition(GUID_NULL, var);
          PropVariantClear(&var);
          if (FAILED(hr)) {
            BOOST_LOG(error) << "[VIPLE-VR-CLIP] SetCurrentPosition on EOS failed: " << format_hr(hr);
            return false;
          }
          loops_.fetch_add(1, std::memory_order_relaxed);
          // §REVIEW-1111：回到開頭之後還是一直回「結尾」（空的影片、只有音訊、解碼器沒有清掉狀態）時原本會無限繞，
          // 而且這時握著共用貼圖的鎖，擷取端永遠拿不到畫面。連續 4 次就放棄這一幀。
          if (++eos_retries >= 4) {
            BOOST_LOG(error) << "[VIPLE-VR-CLIP] still at end of stream after seeking back 4 times";
            return false;
          }
          continue;
        }
        eos_retries = 0;

        if (!sample) {
          null_retries++;
          if (null_retries >= 8) {
            BOOST_LOG(error) << "[VIPLE-VR-CLIP] ReadSample returned null sample 8 times in a row";
            return false;
          }
          continue;
        }
      }

      // 取得樣本的 MediaBuffer
      ComPtr<IMFMediaBuffer> media_buffer;
      HRESULT hr = sample->GetBufferByIndex(0, &media_buffer);
      if (FAILED(hr) || !media_buffer) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] GetBufferByIndex failed: " << format_hr(hr);
        return false;
      }

      ComPtr<ID3D11VideoProcessorInputView> input_view;

      // 檢查是否能取得硬體 D3D11 貼圖
      ComPtr<IMFDXGIBuffer> dxgi_buffer;
      ComPtr<ID3D11Texture2D> hw_texture;
      UINT subresource_index = 0;

      if (SUCCEEDED(media_buffer.As(&dxgi_buffer)) && dxgi_buffer &&
          SUCCEEDED(dxgi_buffer->GetResource(IID_PPV_ARGS(&hw_texture))) && hw_texture) {
        dxgi_buffer->GetSubresourceIndex(&subresource_index);
        input_view = get_or_create_input_view(hw_texture.Get(), subresource_index);
        if (!input_view) {
          return false;
        }
      } else {
        // 軟體解碼後備路徑
        if (!warned_software_) {
          BOOST_LOG(warning) << "[VIPLE-VR-CLIP] Software decoder in use; uploading NV12 via IMF2DBuffer to dynamic D3D11 texture";
          warned_software_ = true;
        }

        const uint32_t cur_w = clip_width_.load(std::memory_order_relaxed);
        const uint32_t cur_h = clip_height_.load(std::memory_order_relaxed);

        // 確保動態 NV12 貼圖已建立且尺寸相符
        if (dynamic_nv12_tex_) {
          D3D11_TEXTURE2D_DESC cur_desc{};
          dynamic_nv12_tex_->GetDesc(&cur_desc);
          if (cur_desc.Width != cur_w || cur_desc.Height != cur_h) {
            dynamic_nv12_tex_.Reset();
          }
        }

        if (!dynamic_nv12_tex_) {
          D3D11_TEXTURE2D_DESC dyn_desc{};
          dyn_desc.Width = cur_w;
          dyn_desc.Height = cur_h;
          dyn_desc.MipLevels = 1;
          dyn_desc.ArraySize = 1;
          dyn_desc.Format = DXGI_FORMAT_NV12;
          dyn_desc.SampleDesc.Count = 1;
          dyn_desc.Usage = D3D11_USAGE_DYNAMIC;
          dyn_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
          dyn_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

          hr = d3d_device_->CreateTexture2D(&dyn_desc, nullptr, &dynamic_nv12_tex_);
          if (FAILED(hr)) {
            BOOST_LOG(error) << "[VIPLE-VR-CLIP] Create dynamic NV12 texture failed: " << format_hr(hr);
            return false;
          }
        }

        // 鎖定 MediaBuffer 並拷貝資料至動態貼圖
        ComPtr<IMF2DBuffer> buf2d;
        BYTE *src_data = nullptr;
        LONG src_pitch = 0;
        bool locked_2d = false;

        if (SUCCEEDED(media_buffer.As(&buf2d)) && buf2d) {
          if (SUCCEEDED(buf2d->Lock2D(&src_data, &src_pitch))) {
            locked_2d = true;
          }
        }

        BYTE *flat_data = nullptr;
        DWORD max_len = 0, cur_len = 0;
        if (!locked_2d) {
          hr = media_buffer->Lock(&flat_data, &max_len, &cur_len);
          if (FAILED(hr)) {
            BOOST_LOG(error) << "[VIPLE-VR-CLIP] Lock media buffer failed: " << format_hr(hr);
            return false;
          }
          src_data = flat_data;
          src_pitch = static_cast<LONG>(cur_w);
        }

        // §REVIEW-1111：下面依影片尺寸複製 cur_h × 1.5 列，原本不看來源緩衝區實際多長。Lock2D 可以回負的 pitch
        // （由下往上），平坦緩衝區的長度也可能不到 NV12 需要的量（輸出型態沒有設成 NV12 時）——都會讀出界，
        // 讀到的內容還會被編進畫面。對不上就放棄這一幀。
        {
          DWORD have = cur_len;
          if (locked_2d) {
            have = 0;
            media_buffer->GetCurrentLength(&have);
          }
          const uint64_t need = static_cast<uint64_t>(cur_h + cur_h / 2) * static_cast<uint64_t>(src_pitch > 0 ? src_pitch : 0);
          if (!src_data || src_pitch <= 0 || need == 0 || need > have) {
            BOOST_LOG(error) << "[VIPLE-VR-CLIP] software frame does not fit its buffer (pitch=" << src_pitch << " rows=" << (cur_h + cur_h / 2)
                             << " have=" << have << "); frame dropped";
            if (locked_2d) {
              buf2d->Unlock2D();
            } else {
              media_buffer->Unlock();
            }
            return false;
          }
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = d3d_context_->Map(dynamic_nv12_tex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] Map dynamic texture failed: " << format_hr(hr);
          if (locked_2d) {
            buf2d->Unlock2D();
          } else {
            media_buffer->Unlock();
          }
          return false;
        }

        // NV12 包含 Y 平面（cur_h 行）以及 UV 平面（cur_h / 2 行）
        const uint32_t total_rows = cur_h + cur_h / 2;
        const size_t copy_bytes_per_row = std::min(static_cast<size_t>(src_pitch), static_cast<size_t>(mapped.RowPitch));
        for (uint32_t r = 0; r < total_rows; ++r) {
          std::memcpy(
            static_cast<uint8_t *>(mapped.pData) + r * mapped.RowPitch,
            src_data + r * src_pitch,
            copy_bytes_per_row
          );
        }

        d3d_context_->Unmap(dynamic_nv12_tex_.Get(), 0);

        if (locked_2d) {
          buf2d->Unlock2D();
        } else {
          media_buffer->Unlock();
        }

        input_view = get_or_create_input_view(dynamic_nv12_tex_.Get(), 0);
        if (!input_view) {
          return false;
        }
      }

      // 透過 VideoProcessorBlt 轉換色彩空間並縮放至當前 slot 的輸出檢視
      D3D11_VIDEO_PROCESSOR_STREAM stream{};
      stream.Enable = TRUE;
      stream.OutputIndex = 0;
      stream.InputFrameOrField = 0;
      stream.PastFrames = 0;
      stream.FutureFrames = 0;
      stream.ppPastSurfaces = nullptr;
      stream.ppFutureSurfaces = nullptr;
      stream.pInputSurface = input_view.Get();
      stream.ppPastSurfacesRight = nullptr;
      stream.ppFutureSurfacesRight = nullptr;

      // §REVIEW-1111：媒體型態變更時 setup_video_processor 會先放掉舊的 processor，重建失敗就留下空的；
      // 原本照樣拿空指標去 Blt。
      if (!video_proc_ || !video_context_) {
        return false;
      }
      hr = video_context_->VideoProcessorBlt(
        video_proc_.Get(),
        slot.output_view.Get(),
        0,
        1,
        &stream
      );
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-CLIP] VideoProcessorBlt failed: " << format_hr(hr);
        return false;
      }

      return true;
    }

    /// @brief 背景解碼工作執行緒主迴圈。
    void decode_loop() {
      BOOST_LOG(info) << "[VIPLE-VR-CLIP] decoder thread started";

      int current_slot = 0;
      int consecutive_errors = 0;
      const int num_slots = static_cast<int>(slots_.size());

      while (!stop_flag_.load(std::memory_order_relaxed)) {
        auto &slot = slots_[current_slot];

        // 依序 AcquireSync(0, 200 ms)；逾時就重試同一個 slot，不要跳過；收到停止旗標就離開
        HRESULT hr = slot.keyed_mutex->AcquireSync(0, 200);
        if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
          continue;
        }
        if (FAILED(hr)) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] AcquireSync(0) failed on slot " << current_slot << ": " << format_hr(hr);
          consecutive_errors++;
          if (consecutive_errors >= 30) {
            BOOST_LOG(error) << "[VIPLE-VR-CLIP] decoder reached 30 consecutive errors; aborting";
            failed_.store(true, std::memory_order_release);
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;
        }

        // 成功取得鎖之後，若收到停止旗標則釋放鎖並退出
        if (stop_flag_.load(std::memory_order_relaxed)) {
          slot.keyed_mutex->ReleaseSync(0);
          break;
        }

        // 讀下一個樣本並 Blt 至當前 slot，計算單張（讀樣本＋Blt）耗時
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = decode_one_frame(slot);
        const auto t1 = std::chrono::steady_clock::now();

        if (ok) {
          const double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
          record_decode_ms(elapsed_ms);

          d3d_context_->Flush();
          hr = slot.keyed_mutex->ReleaseSync(1);
          if (FAILED(hr)) {
            BOOST_LOG(error) << "[VIPLE-VR-CLIP] ReleaseSync(1) failed on slot " << current_slot << ": " << format_hr(hr);
            consecutive_errors++;
          } else {
            consecutive_errors = 0;
            decoded_.fetch_add(1, std::memory_order_relaxed);
            current_slot = (current_slot + 1) % num_slots;
          }
        } else {
          // 解碼失敗，還原 key 0 讓下次重試，並累計錯誤次數
          slot.keyed_mutex->ReleaseSync(0);
          consecutive_errors++;
          BOOST_LOG(warning) << "[VIPLE-VR-CLIP] decode_one_frame failed on slot " << current_slot
                             << " (consecutive: " << consecutive_errors << ")";
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (consecutive_errors >= 30) {
          BOOST_LOG(error) << "[VIPLE-VR-CLIP] decoder reached 30 consecutive errors; aborting";
          failed_.store(true, std::memory_order_release);
          break;
        }
      }

      BOOST_LOG(info) << "[VIPLE-VR-CLIP] decoder thread stopped (decoded "
                      << decoded_.load(std::memory_order_relaxed) << ", loops "
                      << loops_.load(std::memory_order_relaxed) << ")";
    }
  };

  std::unique_ptr<vr_clip_source_t> vr_clip_source_t::open(
    LUID adapter_luid,
    const std::wstring &path,
    uint32_t out_w,
    uint32_t out_h,
    int ring
  ) {
    if (!ensure_mf_initialized()) {
      return nullptr;
    }

    auto inst = std::unique_ptr<vr_clip_source_t>(new vr_clip_source_t());
    auto impl = std::make_unique<impl_t>();
    if (!impl->init(adapter_luid, path, out_w, out_h, ring)) {
      return nullptr;
    }
    inst->impl_ = std::move(impl);
    return inst;
  }

  vr_clip_source_t::~vr_clip_source_t() = default;

  int vr_clip_source_t::ring_size() const {
    return impl_ ? static_cast<int>(impl_->slots_.size()) : 0;
  }

  HANDLE vr_clip_source_t::shared_handle(int slot) const {
    if (!impl_ || slot < 0 || static_cast<size_t>(slot) >= impl_->slots_.size()) {
      return nullptr;
    }
    return impl_->slots_[slot].shared_handle;
  }

  bool vr_clip_source_t::failed() const {
    return impl_ ? impl_->failed_.load(std::memory_order_acquire) : true;
  }

  uint64_t vr_clip_source_t::decoded() const {
    return impl_ ? impl_->decoded_.load(std::memory_order_relaxed) : 0;
  }

  double vr_clip_source_t::decode_ms_max_and_reset() {
    return impl_ ? impl_->decode_ms_max_and_reset() : 0.0;
  }

  uint32_t vr_clip_source_t::clip_width() const {
    return impl_ ? impl_->clip_width_.load(std::memory_order_relaxed) : 0;
  }

  uint32_t vr_clip_source_t::clip_height() const {
    return impl_ ? impl_->clip_height_.load(std::memory_order_relaxed) : 0;
  }

  double vr_clip_source_t::clip_fps() const {
    return impl_ ? impl_->clip_fps_.load(std::memory_order_relaxed) : 0.0;
  }

  int64_t vr_clip_source_t::position_ms() const {
    if (!impl_) {
      return -1;
    }
    int64_t ts = impl_->last_ts_100ns_.load(std::memory_order_relaxed);
    return ts >= 0 ? ts / 10000 : -1;
  }

  uint64_t vr_clip_source_t::loops() const {
    return impl_ ? impl_->loops_.load(std::memory_order_relaxed) : 0;
  }

}  // namespace platf::dxgi
