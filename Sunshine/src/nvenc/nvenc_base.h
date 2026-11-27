/**
 * @file src/nvenc/nvenc_base.h
 * @brief Declarations for abstract platform-agnostic base of standalone NVENC encoder.
 */
#pragma once

// lib includes
#include <chrono>
#include <cstdint>
#include <vector>
#include <ffnvcodec/nvEncodeAPI.h>

// local includes
#include "nvenc_colorspace.h"
#include "nvenc_config.h"
#include "nvenc_encoded_frame.h"
#include "src/logging.h"
#include "src/video.h"

/**
 * @brief Standalone NVENC encoder
 */
namespace nvenc {

  /**
   * @brief §NVENC-SPLIT：Video Codec SDK 12.0 標頭裡 NV_ENC_*_VER 去掉 API 版本位元之後的值。
   * @details 編譯用的標頭是 13.0，但預設（不開 split）仍以 11.0／12.0 的結構跟驅動溝通，結構版本號必須照 12.0
   *          標頭的值送（13.0 把 INITIALIZE_PARAMS 5→7、PRESET_CONFIG 4→5、CONFIG 8→9、EVENT_PARAMS 1→2、PIC_PARAMS 6→7、
   *          RECONFIGURE_PARAMS 1→2、REGISTER_RESOURCE 4→5，LOCK_BITSTREAM 多了 bit 31）。標頭是 12.0 時
   *          nvenc_base.cpp 會用 static_assert 核對這張表。
   */
  namespace sdk12_ver {
    constexpr uint32_t make(uint32_t struct_version, bool high_bit = false) {
      return (struct_version << 16) | (0x7u << 28) | (high_bit ? (1u << 31) : 0u);
    }

    constexpr uint32_t open_encode_session_ex_params = make(1);
    constexpr uint32_t caps_param = make(1);
    constexpr uint32_t initialize_params = make(5, true);
    constexpr uint32_t preset_config = make(4, true);
    constexpr uint32_t config = make(8, true);
    constexpr uint32_t event_params = make(1);
    constexpr uint32_t create_bitstream_buffer = make(1);
    constexpr uint32_t map_input_resource = make(4);
    constexpr uint32_t pic_params = make(6, true);
    constexpr uint32_t lock_bitstream = make(2);
    constexpr uint32_t sequence_param_payload = make(1);
    constexpr uint32_t reconfigure_params = make(1, true);
    constexpr uint32_t register_resource = make(4);
    constexpr uint32_t function_list = make(2);
  }  // namespace sdk12_ver

  /**
   * @brief Abstract platform-agnostic base of standalone NVENC encoder.
   *        Derived classes perform platform-specific operations.
   */
  class nvenc_base {
  public:
    /**
     * @param device_type Underlying device type used by derived class.
     */
    explicit nvenc_base(NV_ENC_DEVICE_TYPE device_type);
    virtual ~nvenc_base();

    nvenc_base(const nvenc_base &) = delete;
    nvenc_base &operator=(const nvenc_base &) = delete;

    /**
     * @brief Create the encoder.
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace YUV colorspace.
     * @param buffer_format Platform-agnostic input surface format.
     * @return `true` on success, `false` on error
     */
    bool create_encoder(const nvenc_config &config, const video::config_t &client_config, const nvenc_colorspace_t &colorspace, NV_ENC_BUFFER_FORMAT buffer_format);

    /**
     * @brief Destroy the encoder.
     *        Derived classes classes call it in the destructor.
     */
    void destroy_encoder();

    /**
     * @brief Encode the next frame using platform-specific input surface.
     * @param frame_index Frame index that uniquely identifies the frame.
     *        Afterwards serves as parameter for `invalidate_ref_frames()`.
     *        No restrictions on the first frame index, but later frame indexes must be subsequent.
     * @param force_idr Whether to encode frame as forced IDR.
     * @param force_intra_refresh_frames VipleStream 2.0 §VR：非 0 時這一幀開始一波長度 N 的
     *        intra refresh（forceIntraRefreshWithFrameCnt）。只有 vr_intra_refresh_enabled()
     *        成立時有效；force_idr 優先。
     * @return Encoded frame.
     */
    nvenc_encoded_frame encode_frame(uint64_t frame_index, bool force_idr, uint32_t force_intra_refresh_frames = 0);

    /**
     * @brief VipleStream 2.0 §VR：encoder 是否以 VR profile 建立、且 GPU 支援 intra refresh
     *        （可以接受 forceIntraRefreshWithFrameCnt）。
     */
    bool vr_intra_refresh_enabled() const {
      return encoder_params.vr_intra_refresh;
    }

    /**
     * @brief Perform reference frame invalidation (RFI) procedure.
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return `true` on success, `false` on error.
     *         After error next frame must be encoded with `force_idr = true`.
     */
    bool invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame);

    /**
     * @brief Reconfigure encoder bitrate mid-stream without recreating the session.
     * @param bitrate_kbps New target bitrate in kilobits per second.
     * @param framerate Current framerate (for VBV buffer calculation).
     * @param force_idr §ABR-RAMP：true = reset encoder + 強制 IDR（降速/擁塞救援，
     *        快速收斂到新碼率）；false = 平滑過渡（回升，不打斷畫面）。
     * @return `true` on success, `false` on error.
     */
    bool reconfigure_bitrate(int bitrate_kbps, int framerate, bool force_idr);

    /**
     * @brief §REVIEW-1118B：中途只改幀率。不要塞進 reconfigure_bitrate——那支是 ABR 每一步都在叫的。
     *        標頭列的不支援重設是 GOP、sync/async、MaxWidth/MaxHeight、PTD；改 frameRateNum 不需要 resetEncoder。
     *        resetEncoder 會清掉碼率控制狀態，而且註解寫要配 IDR。這條封包的處理端已經另外要求 IDR。
     *        nvenc_d3d11 與 nvenc_d3d11_native 沒有額外步驟，直接繼承。CUDA 裝置覆寫它，先把 context 設成 current。
     * @return `true` 已是這個幀率或重設成功；`false` 失敗或拒絕（分數幀率不改）。
     */
    virtual bool reconfigure_framerate(int framerate);

  protected:
    /**
     * @brief Required. Used for loading NvEnc library and setting `nvenc` variable with `NvEncodeAPICreateInstance()`.
     *        Called during `create_encoder()` if `nvenc` variable is not initialized.
     * @return `true` on success, `false` on error
     */
    virtual bool init_library() = 0;

    /**
     * @brief Required. Used for creating outside-facing input surface,
     *        registering this surface with `nvenc->nvEncRegisterResource()` and setting `registered_input_buffer` variable.
     *        Called during `create_encoder()`.
     * @return `true` on success, `false` on error
     */
    virtual bool create_and_register_input_buffer() = 0;

    /**
     * @brief Optional. Override if you must perform additional operations on the registered input surface in the beginning of `encode_frame()`.
     *        Typically used for interop copy.
     * @return `true` on success, `false` on error
     */
    virtual bool synchronize_input_buffer() {
      return true;
    }

    /**
     * @brief Optional. Override if you want to create encoder in async mode.
     *        In this case must also set `async_event_handle` variable.
     * @param timeout_ms Wait timeout in milliseconds
     * @return `true` on success, `false` on timeout or error
     */
    virtual bool wait_for_async_event(uint32_t timeout_ms) {
      return false;
    }

    /**
     * @brief Optional. §NVENC-SPLIT：驅動支援的最高 NvEncode API 版本（NvEncodeAPIGetMaxSupportedVersion，
     *        格式 `(major << 4) | minor`）。查不到回 0（split-frame 就不會啟用）。
     *        只在 `init_library()` 成功之後呼叫。
     */
    virtual uint32_t driver_max_api_version() {
      return 0;
    }

    /**
     * @brief Optional. §NVENC-SPLIT：用目前的 `min_struct_version(NV_ENCODE_API_FUNCTION_LIST_VER, ...)` 重新呼叫
     *        NvEncodeAPICreateInstance，成功才換掉 `nvenc`。驅動要求函式表的 API 版本和 session 的 apiVersion 一致
     *        （11/12 的函式表開 13.0 session、或反過來，都回 NV_ENC_ERR_INVALID_VERSION），所以在 13.0 與 11/12 之間切換時要重建。
     *        只在沒有 session 存在時呼叫。
     * @return `true` on success; 失敗時保留原本的函式表
     */
    virtual bool recreate_function_list() {
      return false;
    }

    bool nvenc_failed(NVENCSTATUS status);

    /**
     * @brief This function returns the corresponding struct version for the minimum API required by the codec.
     * @details Reducing the struct versions maximizes driver compatibility by avoiding needless API breaks.
     *          §NVENC-SPLIT：這個 session 用標頭原生版本（split-frame）時直接回 `version`，忽略後面三個參數；
     *          否則以 `sdk12_version`（12.0 標頭的值）配上 minimum_api_version，再套 v11／v12 覆寫。
     * @param version The raw structure version from `NVENCAPI_STRUCT_VERSION()`.
     * @param sdk12_version 同一個結構在 12.0 標頭的版本值（`sdk12_ver::*`）。
     * @param v11_struct_version Optionally specifies the struct version to use with v11 SDK major versions.
     * @param v12_struct_version Optionally specifies the struct version to use with v12 SDK major versions.
     * @return A suitable struct version for the active codec.
     */
    uint32_t min_struct_version(uint32_t version, uint32_t sdk12_version, uint32_t v11_struct_version = 0, uint32_t v12_struct_version = 0);

    const NV_ENC_DEVICE_TYPE device_type;

    void *encoder = nullptr;

    struct {
      uint32_t width = 0;
      uint32_t height = 0;
      NV_ENC_BUFFER_FORMAT buffer_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
      uint32_t ref_frames_in_dpb = 0;
      bool rfi = false;
      int video_format = 0;  // §VR：0 H.264、1 HEVC、2 AV1（選 codecPicParams 的哪一個）
      bool vr_intra_refresh = false;  // §VR：VR profile 下已開 intra refresh
      int qp_map_avg_x100 = 0;  // §VR-FOVEA-QP：qp_delta_map 的平均值 ×100（回報給 ABR 的平均 QP 要扣掉）
    } encoder_params;

    /// §VR-FOVEA-QP：每個編碼區塊（H.264 16×16、HEVC 32×32、AV1 64×64）一個 QP 差，逐列排；空＝不用。
    /// 內容整場不變，每一幀交同一份給 NVENC。
    std::vector<int8_t> qp_delta_map;

    std::string last_nvenc_error_string;

    // Derived classes set these variables
    void *device = nullptr;  ///< Platform-specific handle of encoding device.
                             ///< Should be set in constructor or `init_library()`.
    std::shared_ptr<NV_ENCODE_API_FUNCTION_LIST> nvenc;  ///< Function pointers list produced by `NvEncodeAPICreateInstance()`.
                                                         ///< Should be set in `init_library()`.
    NV_ENC_REGISTERED_PTR registered_input_buffer = nullptr;  ///< Platform-specific input surface registered with `NvEncRegisterResource()`.
                                                              ///< Should be set in `create_and_register_input_buffer()`.
    void *async_event_handle = nullptr;  ///< (optional) Platform-specific handle of event object event.
                                         ///< Can be set in constructor or `init_library()`, must override `wait_for_async_event()`.

  private:
    /// create_encoder() 的本體。allow_split=false 時一定走 11.0／12.0 結構（與不開 split 完全相同）。
    /// split_attempted：這一次是否改用了標頭原生版本（失敗時呼叫端據此退回重試）。
    bool create_encoder_impl(const nvenc_config &config, const video::config_t &client_config, const nvenc_colorspace_t &colorspace, NV_ENC_BUFFER_FORMAT buffer_format, bool allow_split, bool &split_attempted);

    /// §NVENC-SPLIT：判斷這個 session 能不能開 split-frame；不能時印原因。mode 回傳要送的 NV_ENC_SPLIT_ENCODE_MODE。
    bool split_encode_usable(const nvenc_config &config, const video::config_t &client_config, uint32_t &mode);

    /// 以 stored_init_params 呼叫 nvEncReconfigureEncoder（12.0 結構時用 12.0 的 RECONFIGURE_PARAMS 版面）
    bool send_reconfigure(bool reset_encoder, bool force_idr);

    NV_ENC_OUTPUT_PTR output_bitstream = nullptr;
    uint32_t minimum_api_version = 0;
    /// §NVENC-SPLIT：這個 session 以標頭原生版本（13.0）的結構跟驅動溝通。只有 split-frame 開著時為 true。
    bool native_structs = false;
    /// §NVENC-SPLIT：目前的 `nvenc` 函式表是以 13.0（原生）版本建立的。init_library() 一律建 11/12 版，所以預設 false。
    bool function_list_native = false;

    // VipleStream §ABR: stored for mid-stream NvEncReconfigureEncoder() calls
    NV_ENC_INITIALIZE_PARAMS stored_init_params {};
    NV_ENC_CONFIG stored_enc_config {};

    // §REVIEW-1118B：建立時有沒有套自訂 VBV，以及當時的 vbv_percentage_increase。
    // 中途改幀率要用同一條式子重算，不能沿用 reconfigure_bitrate 照平均位元率比例縮的做法。
    bool custom_vbv_buffer = false;
    int vbv_percentage_increase = 0;

    // VipleStream §SF-PARAMSETS: pin the auto-selected level (and HEVC tier) into
    // stored_enc_config so ABR reconfigures never change the VPS/SPS mid-session.
    void pin_level_for_reconfigure(int video_format);
    void unpin_level_for_reconfigure();
    bool level_pinned = false;
    int pinned_video_format = -1;
    uint32_t unpinned_level = 0;
    uint32_t unpinned_tier = 0;

    struct {
      uint64_t last_encoded_frame_index = 0;
      bool rfi_needs_confirmation = false;
      std::pair<uint64_t, uint64_t> last_rfi_range;
      logging::min_max_avg_periodic_logger<double> frame_size_logger = {debug, "NvEnc: encoded frame sizes in kB", ""};
      // [VIPLE-NVENC-PROF] HEVC 1440p 60fps cap diagnosis: log encode_frame
      // sub-step timings periodically.  §LOGDROP-1127：原本是 4 行／5 秒（NVENC server 全部一起占
      // 約 20% log 量），改成 30 秒 1 行、四項 min/max/avg 壓在同一行；欄位名稱不變。
      // 只有 info 等級才累計（min_log_level > info 時完全不花成本）。
      struct prof_stat_t {
        double min_v = 1e18;
        double max_v = -1e18;
        double sum = 0;
        uint32_t n = 0;

        void add(double v) {
          min_v = v < min_v ? v : min_v;
          max_v = v > max_v ? v : max_v;
          sum += v;
          ++n;
        }

        double avg() const {
          return n ? sum / n : 0.0;
        }

        void reset() {
          min_v = 1e18;
          max_v = -1e18;
          sum = 0;
          n = 0;
        }
      };

      std::chrono::steady_clock::time_point prof_window_start = std::chrono::steady_clock::now();
      prof_stat_t prof_total;
      prof_stat_t prof_async;
      prof_stat_t prof_lock;
      prof_stat_t prof_bsize;
      uint32_t prof_calls = 0;  // 這個視窗 encode_frame 被呼叫幾次（算 calls/sec）
    } encoder_state;
  };

}  // namespace nvenc
