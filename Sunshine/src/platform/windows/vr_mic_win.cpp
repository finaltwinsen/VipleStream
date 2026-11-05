/**
 * @file src/platform/windows/vr_mic_win.cpp
 * @brief §VR-MIC：Windows 主機端頭盔麥克風直通（實作 vr_mic.h 介面）。
 *
 * 將客戶端傳送之 Opus 編碼麥克風封包解碼後，寫入 Windows 虛擬麥克風裝置
 * （Steam Streaming Microphone 的播放端，由 Steam 虛擬驅動程式轉入同名錄音端）。
 *
 * 參考實作來源（GPL-3.0 授權）：
 * 改編自 ClassicOldSong/Apollo PR #1428 "Add Windows microphone passthrough via Steam Streaming Microphone" (by logabell)。
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// lib includes
#include <opus/opus.h>

// platform includes
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <synchapi.h>

// local includes
#include "src/logging.h"
#include "src/utility.h"
#include "src/vr/vr_mic.h"
#include "utf_utils.h"

namespace vr::mic {

  namespace {
    using namespace std::literals;

    // COM GUID 定義（避免特定編譯工具鏈缺乏相關符號定義）
    const CLSID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    const IID kIidIMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    const IID kIidIAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
    const IID kIidIAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

    const PROPERTYKEY kPKey_Device_FriendlyName = {
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
    };
    const PROPERTYKEY kPKey_DeviceInterface_FriendlyName = {
      {0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2
    };

    template<class T>
    void com_release(T *p) {
      if (p) {
        p->Release();
      }
    }

    template<class T>
    void co_task_free(T *p) {
      if (p) {
        CoTaskMemFree(static_cast<LPVOID>(p));
      }
    }

    using device_enum_ptr = util::safe_ptr<IMMDeviceEnumerator, com_release<IMMDeviceEnumerator>>;
    using device_ptr = util::safe_ptr<IMMDevice, com_release<IMMDevice>>;
    using collection_ptr = util::safe_ptr<IMMDeviceCollection, com_release<IMMDeviceCollection>>;
    using audio_client_ptr = util::safe_ptr<IAudioClient, com_release<IAudioClient>>;
    using audio_render_ptr = util::safe_ptr<IAudioRenderClient, com_release<IAudioRenderClient>>;
    using wave_format_ptr = util::safe_ptr<WAVEFORMATEX, co_task_free<WAVEFORMATEX>>;
    using prop_store_ptr = util::safe_ptr<IPropertyStore, com_release<IPropertyStore>>;

    /**
     * @brief COM 初始化 RAII 包裝。
     * 呼叫 CoInitializeEx 為呼叫端執行緒初始化 MTA；若回傳 RPC_E_CHANGED_MODE（代表已在 STA 模式）則包容並放行，
     * 解構時只有在本次呼叫成功初始化 COM 時才呼叫 CoUninitialize。
     */
    class com_init_t {
    public:
      com_init_t() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
      }

      ~com_init_t() {
        if (SUCCEEDED(hr_)) {
          CoUninitialize();
        }
      }

      bool ok() const {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
      }

    private:
      HRESULT hr_;
    };

    /**
     * @brief PROPVARIANT 清理 RAII 包裝。
     */
    class prop_var_t {
    public:
      prop_var_t() {
        PropVariantInit(&prop);
      }

      ~prop_var_t() {
        PropVariantClear(&prop);
      }

      PROPVARIANT prop;
    };

    /**
     * @brief 不區分大小寫檢查寬字串是否包含子字串。
     */
    bool contains_case_insensitive(std::wstring_view text, std::wstring_view target) {
      if (target.empty()) {
        return true;
      }
      if (text.size() < target.size()) {
        return false;
      }
      auto it = std::search(
        text.begin(), text.end(),
        target.begin(), target.end(),
        [](wchar_t ch1, wchar_t ch2) {
          return std::towlower(ch1) == std::towlower(ch2);
        }
      );
      return it != text.end();
    }

    constexpr auto kSteamMicName = L"Steam Streaming Microphone"sv;

    enum class sample_format_e {
      float32,
      int16,
      unsupported,
    };

    /**
     * @brief MMCSS Pro Audio 動態載入包裝類別（避免 avrt.lib 連結依賴）。
     */
    class mmcss_guard_t {
    public:
      mmcss_guard_t() {
        h_avrt_ = LoadLibraryW(L"Avrt.dll");
        if (h_avrt_) {
          using AvSetMmThreadCharacteristicsW_fn = HANDLE(WINAPI *)(LPCWSTR, LPDWORD);
          auto fn_set = reinterpret_cast<AvSetMmThreadCharacteristicsW_fn>(
            GetProcAddress(h_avrt_, "AvSetMmThreadCharacteristicsW")
          );
          if (fn_set) {
            DWORD task_index = 0;
            task_handle_ = fn_set(L"Pro Audio", &task_index);
          }
        }
      }

      ~mmcss_guard_t() {
        if (task_handle_ && h_avrt_) {
          using AvRevertMmThreadCharacteristics_fn = BOOL(WINAPI *)(HANDLE);
          auto fn_revert = reinterpret_cast<AvRevertMmThreadCharacteristics_fn>(
            GetProcAddress(h_avrt_, "AvRevertMmThreadCharacteristics")
          );
          if (fn_revert) {
            fn_revert(task_handle_);
          }
        }
        if (h_avrt_) {
          FreeLibrary(h_avrt_);
        }
      }

    private:
      HMODULE h_avrt_{nullptr};
      HANDLE task_handle_{nullptr};
    };

    /**
     * @brief 列舉活躍的 RENDER 端點並尋找 Steam Streaming Microphone 裝置。
     */
    device_ptr find_steam_mic_device(std::string *out_friendly_name = nullptr) {
      device_enum_ptr enumerator;
      HRESULT hr = CoCreateInstance(
        kClsidMMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        kIidIMMDeviceEnumerator,
        reinterpret_cast<void **>(&enumerator)
      );
      if (FAILED(hr) || !enumerator) {
        return nullptr;
      }

      collection_ptr collection;
      hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(hr) || !collection) {
        return nullptr;
      }

      UINT count = 0;
      hr = collection->GetCount(&count);
      if (FAILED(hr)) {
        return nullptr;
      }

      for (UINT i = 0; i < count; ++i) {
        device_ptr device;
        if (FAILED(collection->Item(i, &device)) || !device) {
          continue;
        }

        prop_store_ptr props;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) {
          continue;
        }

        prop_var_t friendly_name;
        bool matched = false;
        std::wstring name_str;

        if (SUCCEEDED(props->GetValue(kPKey_Device_FriendlyName, &friendly_name.prop)) &&
            friendly_name.prop.vt == VT_LPWSTR && friendly_name.prop.pwszVal) {
          name_str = friendly_name.prop.pwszVal;
          if (contains_case_insensitive(name_str, kSteamMicName)) {
            matched = true;
          }
        }

        if (!matched) {
          prop_var_t iface_name;
          if (SUCCEEDED(props->GetValue(kPKey_DeviceInterface_FriendlyName, &iface_name.prop)) &&
              iface_name.prop.vt == VT_LPWSTR && iface_name.prop.pwszVal) {
            if (contains_case_insensitive(iface_name.prop.pwszVal, kSteamMicName)) {
              matched = true;
              name_str = iface_name.prop.pwszVal;
            }
          }
        }

        if (matched) {
          if (out_friendly_name) {
            *out_friendly_name = utf_utils::to_utf8(name_str);
          }
          return device;
        }
      }

      return nullptr;
    }

  }  // namespace

  /**
   * @brief Windows host 端的 VR 麥克風音訊接收器實作。
   */
  class sink_impl_t: public sink_t {
  public:
    sink_impl_t() = default;

    ~sink_impl_t() override {
      stop();
    }

    /// 開播放執行緒並等它把裝置開好（COM 物件全部在播放執行緒上建立、使用、釋放，不跨執行緒）
    bool start();
    void push(uint16_t seq, const uint8_t *opus, size_t len) override;

  private:
    bool init_on_thread();
    void render_thread_proc(std::promise<bool> ready);
    void stop();

    // 運作統計計數器
    struct stats_t {
      std::atomic<uint64_t> packets{0};
      std::atomic<uint64_t> fec_recoveries{0};
      std::atomic<uint64_t> plc_frames{0};
      std::atomic<uint64_t> drops{0};
      std::atomic<uint64_t> underruns{0};
    } stats_;

    // WASAPI 渲染介面與裝置格式
    audio_client_ptr audio_client_;
    audio_render_ptr render_client_;
    UINT32 buffer_frames_{0};
    DWORD sample_rate_{48000};
    WORD channels_{2};
    sample_format_e sample_format_{sample_format_e::unsupported};

    // 執行緒同步控制
    HANDLE audio_event_{nullptr};
    HANDLE stop_event_{nullptr};
    std::thread render_thread_;

    // Opus 解碼狀態（由控制執行緒 push 呼叫端獨佔存取）
    OpusDecoder *opus_decoder_{nullptr};
    bool seq_initialized_{false};
    uint16_t expected_seq_{0};

    // 抖動緩衝區（由 mutex_ 保護）
    std::mutex mutex_;
    std::deque<float> jitter_buffer_;
    bool playing_{false};           // 緩衝積累達 60 ms 後開始輸出音訊；發生 underrun 則切回等待狀態
    double resample_src_pos_{0.0};  // 48 kHz 線性重取樣來源位置相位

    std::chrono::steady_clock::time_point last_stats_log_{std::chrono::steady_clock::now()};
  };

  bool sink_impl_t::start() {
    int opus_err = OPUS_OK;
    opus_decoder_ = opus_decoder_create(48000, 1, &opus_err);
    if (!opus_decoder_ || opus_err != OPUS_OK) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't create Opus decoder: "sv << opus_err;
      return false;
    }
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't create stop event"sv;
      return false;
    }
    std::promise<bool> ready;
    auto result = ready.get_future();
    render_thread_ = std::thread(&sink_impl_t::render_thread_proc, this, std::move(ready));
    return result.get();
  }

  bool sink_impl_t::init_on_thread() {
    std::string device_name;
    auto device = find_steam_mic_device(&device_name);
    if (!device) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Steam Streaming Microphone render endpoint not found"sv;
      return false;
    }
    HRESULT hr = device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&audio_client_));
    if (FAILED(hr) || !audio_client_) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't activate IAudioClient on virtual mic device: [0x"sv << util::hex(hr).to_string_view() << ']';
      return false;
    }

    wave_format_ptr mix_format;
    hr = audio_client_->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't get mix format from audio client: [0x"sv << util::hex(hr).to_string_view() << ']';
      return false;
    }

    channels_ = mix_format->nChannels;
    sample_rate_ = mix_format->nSamplesPerSec;

    if (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_format->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
      const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix_format.get());
      if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && mix_format->wBitsPerSample == 32) {
        sample_format_ = sample_format_e::float32;
      } else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && mix_format->wBitsPerSample == 16) {
        sample_format_ = sample_format_e::int16;
      }
    } else if (mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && mix_format->wBitsPerSample == 32) {
      sample_format_ = sample_format_e::float32;
    } else if (mix_format->wFormatTag == WAVE_FORMAT_PCM && mix_format->wBitsPerSample == 16) {
      sample_format_ = sample_format_e::int16;
    }

    if (sample_format_ == sample_format_e::unsupported) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Unsupported mix format: tag="sv << mix_format->wFormatTag
                         << ", bits="sv << mix_format->wBitsPerSample
                         << ", channels="sv << mix_format->nChannels;
      return false;
    }

    BOOST_LOG(info) << "[VIPLE-VR-MIC] Found Steam Streaming Microphone endpoint: "sv << device_name;
    BOOST_LOG(info) << "[VIPLE-VR-MIC] Opened device: ["sv << device_name
                    << "], format: "sv << (sample_format_ == sample_format_e::float32 ? "float32"sv : "int16"sv)
                    << ", sample_rate: "sv << sample_rate_
                    << ", channels: "sv << channels_;

    REFERENCE_TIME buffer_duration = 200000;  // 20 ms (以 100 ns 為單位)
    hr = audio_client_->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
      buffer_duration,
      0,
      mix_format.get(),
      nullptr
    );

    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't initialize audio client: [0x"sv << util::hex(hr).to_string_view() << ']';
      return false;
    }

    hr = audio_client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't get audio buffer size: [0x"sv << util::hex(hr).to_string_view() << ']';
      return false;
    }

    audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audio_event_) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't create audio sync event"sv;
      return false;
    }

    hr = audio_client_->SetEventHandle(audio_event_);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't set audio client event handle: [0x"sv << util::hex(hr).to_string_view() << ']';
      return false;
    }

    hr = audio_client_->GetService(kIidIAudioRenderClient, reinterpret_cast<void **>(&render_client_));
    if (FAILED(hr) || !render_client_) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't acquire IAudioRenderClient: [0x"sv << util::hex(hr).to_string_view() << ']';
      return false;
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Couldn't start audio client: [0x"sv << util::hex(hr).to_string_view() << ']';
      return false;
    }
    return true;
  }

  void sink_impl_t::stop() {
    if (stop_event_) {
      SetEvent(stop_event_);
    }
    if (render_thread_.joinable()) {
      render_thread_.join();  // COM 物件與 audio_event_ 由播放執行緒自己收
    }
    if (stop_event_) {
      CloseHandle(stop_event_);
      stop_event_ = nullptr;
    }
    if (opus_decoder_) {
      opus_decoder_destroy(opus_decoder_);
      opus_decoder_ = nullptr;
    }
  }

  void sink_impl_t::push(uint16_t seq, const uint8_t *opus, size_t len) {
    if (!opus || len == 0 || !opus_decoder_) {
      return;
    }

    if (!seq_initialized_) {
      seq_initialized_ = true;
      expected_seq_ = seq;
    }

    uint16_t gap = seq - expected_seq_;
    if (gap > 0x8000) {
      // 封包為舊封包或重複封包（依 uint16 算術落後於 expected），予以丟棄
      stats_.drops.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    stats_.packets.fetch_add(1, std::memory_order_relaxed);

    std::vector<float> decoded;
    decoded.reserve(960 * 7);

    auto decode_step = [this, &decoded](const uint8_t *data, size_t dlen, int fec) {
      float pcm[960];
      int ret = opus_decode_float(
        opus_decoder_,
        data,
        static_cast<opus_int32>(dlen),
        pcm,
        960,
        fec
      );
      if (ret > 0) {
        decoded.insert(decoded.end(), pcm, pcm + ret);
      } else {
        decoded.insert(decoded.end(), 960, 0.0f);
      }
    };

    if (gap == 0) {
      // 序號吻合，正常解碼
      decode_step(opus, len, 0);
      expected_seq_ = seq + 1;
    } else if (gap == 1) {
      // 剛好漏掉 1 個封包：先利用當前封包的 in-band FEC 解出遺失訊框，再正常解碼當前封包
      stats_.fec_recoveries.fetch_add(1, std::memory_order_relaxed);
      decode_step(opus, len, 1);
      decode_step(opus, len, 0);
      expected_seq_ = seq + 1;
    } else if (gap >= 2 && gap <= 10) {
      // 漏掉 2..10 個封包：以 PLC (傳入 NULL) 填補遺失訊框（上限 5 個訊框），再解碼當前封包
      uint16_t plc_count = std::min<uint16_t>(gap, 5);
      stats_.plc_frames.fetch_add(plc_count, std::memory_order_relaxed);
      for (uint16_t i = 0; i < plc_count; ++i) {
        decode_step(nullptr, 0, 0);
      }
      decode_step(opus, len, 0);
      expected_seq_ = seq + 1;
    } else {
      // gap > 10：差距過大，重置解碼器狀態直接重新同步，不插入 PLC
      opus_decoder_ctl(opus_decoder_, OPUS_RESET_STATE);
      decode_step(opus, len, 0);
      expected_seq_ = seq + 1;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      jitter_buffer_.insert(jitter_buffer_.end(), decoded.begin(), decoded.end());

      // 目標 ~60 ms（2880 取樣點），上限 200 ms（9600 取樣點）
      constexpr size_t kTargetSamples = 48000 * 60 / 1000;
      constexpr size_t kMaxSamples = 48000 * 200 / 1000;

      if (jitter_buffer_.size() > kMaxSamples) {
        size_t excess = jitter_buffer_.size() - kTargetSamples;
        jitter_buffer_.erase(jitter_buffer_.begin(), jitter_buffer_.begin() + excess);
        stats_.drops.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void sink_impl_t::render_thread_proc(std::promise<bool> ready) {
    com_init_t com;
    if (!com.ok()) {
      BOOST_LOG(warning) << "[VIPLE-VR-MIC] Failed to initialize COM on the render thread"sv;
      ready.set_value(false);
      return;
    }
    const bool ok = init_on_thread();
    ready.set_value(ok);
    // 離開這個函式時（正常結束或初始化失敗）在同一條執行緒上釋放 COM 物件
    auto cleanup = util::fail_guard([this]() {
      if (audio_client_) {
        audio_client_->Stop();
      }
      render_client_.reset();
      audio_client_.reset();
      if (audio_event_) {
        CloseHandle(audio_event_);
        audio_event_ = nullptr;
      }
    });
    if (!ok) {
      return;
    }
    mmcss_guard_t mmcss;

    HANDLE wait_handles[2] = {stop_event_, audio_event_};
    constexpr size_t kPrebufferSamples = 48000 * 60 / 1000;  // 60 ms 啟動播放門檻

    while (true) {
      DWORD wait_res = WaitForMultipleObjects(2, wait_handles, FALSE, 20);
      if (wait_res == WAIT_OBJECT_0) {
        // 收到停止事件訊號，結束播放執行緒
        break;
      }

      // 每 10 秒輸出一次統計資訊
      auto now = std::chrono::steady_clock::now();
      if (now - last_stats_log_ >= std::chrono::seconds(10)) {
        last_stats_log_ = now;
        size_t cur_samples = 0;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          cur_samples = jitter_buffer_.size();
        }
        double buffer_ms = (static_cast<double>(cur_samples) * 1000.0) / 48000.0;
        BOOST_LOG(info) << "[VIPLE-VR-MIC] Stats: "sv
                        << "packets="sv << stats_.packets.load(std::memory_order_relaxed)
                        << ", fec="sv << stats_.fec_recoveries.load(std::memory_order_relaxed)
                        << ", plc="sv << stats_.plc_frames.load(std::memory_order_relaxed)
                        << ", drops="sv << stats_.drops.load(std::memory_order_relaxed)
                        << ", underruns="sv << stats_.underruns.load(std::memory_order_relaxed)
                        << ", buffer="sv << std::fixed << std::setprecision(1) << buffer_ms << " ms"sv;
      }

      UINT32 padding = 0;
      HRESULT hr = audio_client_->GetCurrentPadding(&padding);
      if (FAILED(hr)) {
        continue;
      }

      UINT32 frames_needed = buffer_frames_ - padding;
      if (frames_needed == 0) {
        continue;
      }

      // 計算產生 frames_needed 個裝置訊框所需之 48 kHz 樣本數
      size_t needed_src_samples = (sample_rate_ == 48000)
        ? frames_needed
        : static_cast<size_t>(std::ceil(frames_needed * (48000.0 / static_cast<double>(sample_rate_)))) + 2;

      BYTE *data_ptr = nullptr;
      hr = render_client_->GetBuffer(frames_needed, &data_ptr);
      if (FAILED(hr) || !data_ptr) {
        continue;
      }

      std::lock_guard<std::mutex> lock(mutex_);

      // 檢查播放狀態：發生 underrun 後須積累達 60 ms 才開始播放，避免爆音
      if (!playing_) {
        if (jitter_buffer_.size() >= kPrebufferSamples) {
          playing_ = true;
        } else {
          render_client_->ReleaseBuffer(frames_needed, AUDCLNT_BUFFERFLAGS_SILENT);
          continue;
        }
      }

      // 播放中但樣本不足以應付本輪輸出，判定為 underrun
      if (jitter_buffer_.size() < needed_src_samples) {
        playing_ = false;
        resample_src_pos_ = 0.0;
        stats_.underruns.fetch_add(1, std::memory_order_relaxed);
        render_client_->ReleaseBuffer(frames_needed, AUDCLNT_BUFFERFLAGS_SILENT);
        continue;
      }

      // 線性重取樣並將單聲道 (mono) upmix 至所有聲道
      const double step = 48000.0 / static_cast<double>(sample_rate_);

      if (sample_format_ == sample_format_e::float32) {
        float *out_ptr = reinterpret_cast<float *>(data_ptr);
        for (UINT32 f = 0; f < frames_needed; ++f) {
          size_t idx = static_cast<size_t>(resample_src_pos_);
          double alpha = resample_src_pos_ - static_cast<double>(idx);
          float s0 = jitter_buffer_[idx];
          float s1 = (idx + 1 < jitter_buffer_.size()) ? jitter_buffer_[idx + 1] : s0;
          float s = static_cast<float>((1.0 - alpha) * s0 + alpha * s1);

          for (WORD c = 0; c < channels_; ++c) {
            out_ptr[f * channels_ + c] = s;
          }
          resample_src_pos_ += step;
        }
      } else if (sample_format_ == sample_format_e::int16) {
        int16_t *out_ptr = reinterpret_cast<int16_t *>(data_ptr);
        for (UINT32 f = 0; f < frames_needed; ++f) {
          size_t idx = static_cast<size_t>(resample_src_pos_);
          double alpha = resample_src_pos_ - static_cast<double>(idx);
          float s0 = jitter_buffer_[idx];
          float s1 = (idx + 1 < jitter_buffer_.size()) ? jitter_buffer_[idx + 1] : s0;
          float s = static_cast<float>((1.0 - alpha) * s0 + alpha * s1);
          float clamped = std::clamp(s, -1.0f, 1.0f);
          int16_t s_i16 = static_cast<int16_t>(clamped * 32767.0f);

          for (WORD c = 0; c < channels_; ++c) {
            out_ptr[f * channels_ + c] = s_i16;
          }
          resample_src_pos_ += step;
        }
      }

      size_t consumed = static_cast<size_t>(resample_src_pos_);
      if (consumed > 0) {
        if (consumed > jitter_buffer_.size()) {
          consumed = jitter_buffer_.size();
        }
        jitter_buffer_.erase(jitter_buffer_.begin(), jitter_buffer_.begin() + consumed);
        resample_src_pos_ -= static_cast<double>(consumed);
      }

      render_client_->ReleaseBuffer(frames_needed, 0);
    }
  }

  bool available() {
    com_init_t com;
    if (!com.ok()) {
      return false;
    }

    auto device = find_steam_mic_device();
    return device != nullptr;
  }

  std::unique_ptr<sink_t> sink_t::create() {
    auto sink = std::make_unique<sink_impl_t>();
    if (!sink->start()) {
      return nullptr;
    }

    return sink;
  }

}  // namespace vr::mic
