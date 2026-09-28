/**
 * @file src/nvenc/nvenc_base.cpp
 * @brief Definitions for abstract platform-agnostic base of standalone NVENC encoder.
 */
// this include
#include "nvenc_base.h"

// standard includes
#include <algorithm>
#include <format>
#include <string>
#include <vector>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"

#define MAKE_NVENC_VER(major, minor) ((major) | ((minor) << 24))

// Make sure we check backwards compatibility when bumping the Video Codec SDK version
// Things to look out for:
// - NV_ENC_*_VER definitions where the value inside NVENCAPI_STRUCT_VERSION() was increased
// - Incompatible struct changes in nvEncodeAPI.h (fields removed, semantics changed, etc.)
// - Test both old and new drivers with all supported codecs
//
// VipleStream §K.1: accept both v12 (Windows MSYS2 build) and v13 (Linux FetchContent
// FFmpeg bundle ships nv-codec-headers 13.0).  v13 renamed pixelBitDepthMinus8 →
// outputBitDepth (uint32_t→NV_ENC_BIT_DEPTH enum) and inputPixelBitDepthMinus8 →
// inputBitDepth.  Site-of-use uses #if NVENCAPI_MAJOR_VERSION >= 13.
#if NVENCAPI_VERSION != MAKE_NVENC_VER(12U, 0U) && \
    NVENCAPI_VERSION != MAKE_NVENC_VER(13U, 0U)
  #error Check and update NVENC code for backwards compatibility!
#endif

namespace {

  GUID quality_preset_guid_from_number(unsigned number) {
    if (number > 7) {
      number = 7;
    }

    switch (number) {
      case 1:
      default:
        return NV_ENC_PRESET_P1_GUID;

      case 2:
        return NV_ENC_PRESET_P2_GUID;

      case 3:
        return NV_ENC_PRESET_P3_GUID;

      case 4:
        return NV_ENC_PRESET_P4_GUID;

      case 5:
        return NV_ENC_PRESET_P5_GUID;

      case 6:
        return NV_ENC_PRESET_P6_GUID;

      case 7:
        return NV_ENC_PRESET_P7_GUID;
    }
  };

  bool equal_guids(const GUID &guid1, const GUID &guid2) {
    return std::memcmp(&guid1, &guid2, sizeof(GUID)) == 0;
  }

  auto quality_preset_string_from_guid(const GUID &guid) {
    if (equal_guids(guid, NV_ENC_PRESET_P1_GUID)) {
      return "P1";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P2_GUID)) {
      return "P2";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P3_GUID)) {
      return "P3";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P4_GUID)) {
      return "P4";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P5_GUID)) {
      return "P5";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P6_GUID)) {
      return "P6";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P7_GUID)) {
      return "P7";
    }
    return "Unknown";
  }

}  // namespace

namespace nvenc {

  nvenc_base::nvenc_base(NV_ENC_DEVICE_TYPE device_type):
      device_type(device_type) {
  }

  nvenc_base::~nvenc_base() {
    // Use destroy_encoder() instead
  }

  bool nvenc_base::create_encoder(const nvenc_config &config, const video::config_t &client_config, const nvenc_colorspace_t &colorspace, NV_ENC_BUFFER_FORMAT buffer_format) {
    // Pick the minimum NvEncode API version required to support the specified codec
    // to maximize driver compatibility. AV1 was introduced in SDK v12.0.
    minimum_api_version = (client_config.videoFormat <= 1) ? MAKE_NVENC_VER(11U, 0U) : MAKE_NVENC_VER(12U, 0U);

    if (!nvenc && !init_library()) {
      return false;
    }

    if (encoder) {
      destroy_encoder();
    }
    auto fail_guard = util::fail_guard([this] {
      destroy_encoder();
    });

    encoder_params.width = client_config.width;
    encoder_params.height = client_config.height;
    encoder_params.buffer_format = buffer_format;
    encoder_params.rfi = true;
    encoder_params.video_format = client_config.videoFormat;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params = {min_struct_version(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER)};
    session_params.device = device;
    session_params.deviceType = device_type;
    session_params.apiVersion = minimum_api_version;
    if (nvenc_failed(nvenc->nvEncOpenEncodeSessionEx(&session_params, &encoder))) {
      BOOST_LOG(error) << "NvEnc: NvEncOpenEncodeSessionEx() failed: " << last_nvenc_error_string;
      return false;
    }

    uint32_t encode_guid_count = 0;
    if (nvenc_failed(nvenc->nvEncGetEncodeGUIDCount(encoder, &encode_guid_count))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDCount() failed: " << last_nvenc_error_string;
      return false;
    };

    std::vector<GUID> encode_guids(encode_guid_count);
    if (nvenc_failed(nvenc->nvEncGetEncodeGUIDs(encoder, encode_guids.data(), (uint32_t) encode_guids.size(), &encode_guid_count))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDs() failed: " << last_nvenc_error_string;
      return false;
    }

    NV_ENC_INITIALIZE_PARAMS init_params = {min_struct_version(NV_ENC_INITIALIZE_PARAMS_VER)};

    switch (client_config.videoFormat) {
      case 0:
        // H.264
        init_params.encodeGUID = NV_ENC_CODEC_H264_GUID;
        break;

      case 1:
        // HEVC
        init_params.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
        break;

      case 2:
        // AV1
        init_params.encodeGUID = NV_ENC_CODEC_AV1_GUID;
        break;

      default:
        BOOST_LOG(error) << "NvEnc: unknown video format " << client_config.videoFormat;
        return false;
    }

    {
      auto search_predicate = [&](const GUID &guid) {
        return equal_guids(init_params.encodeGUID, guid);
      };
      if (std::find_if(encode_guids.begin(), encode_guids.end(), search_predicate) == encode_guids.end()) {
        BOOST_LOG(error) << "NvEnc: encoding format is not supported by the gpu";
        return false;
      }
    }

    auto get_encoder_cap = [&](NV_ENC_CAPS cap) {
      NV_ENC_CAPS_PARAM param = {min_struct_version(NV_ENC_CAPS_PARAM_VER), cap};
      int value = 0;
      nvenc->nvEncGetEncodeCaps(encoder, init_params.encodeGUID, &param, &value);
      return value;
    };

    auto buffer_is_10bit = [&]() {
      return buffer_format == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    };

    auto buffer_is_yuv444 = [&]() {
      return buffer_format == NV_ENC_BUFFER_FORMAT_AYUV || buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    };

    {
      auto supported_width = get_encoder_cap(NV_ENC_CAPS_WIDTH_MAX);
      auto supported_height = get_encoder_cap(NV_ENC_CAPS_HEIGHT_MAX);
      if (encoder_params.width > supported_width || encoder_params.height > supported_height) {
        BOOST_LOG(error) << "NvEnc: gpu max encode resolution " << supported_width << "x" << supported_height << ", requested " << encoder_params.width << "x" << encoder_params.height;
        return false;
      }
    }

    if (buffer_is_10bit() && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE)) {
      BOOST_LOG(error) << "NvEnc: gpu doesn't support 10-bit encode";
      return false;
    }

    if (buffer_is_yuv444() && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE)) {
      BOOST_LOG(error) << "NvEnc: gpu doesn't support YUV444 encode";
      return false;
    }

    if (async_event_handle && !get_encoder_cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT)) {
      BOOST_LOG(warning) << "NvEnc: gpu doesn't support async encode";
      async_event_handle = nullptr;
    }

    encoder_params.rfi = get_encoder_cap(NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION);

    init_params.presetGUID = quality_preset_guid_from_number(config.quality_preset);
    init_params.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init_params.enablePTD = 1;
    init_params.enableEncodeAsync = async_event_handle ? 1 : 0;
    init_params.enableWeightedPrediction = config.weighted_prediction && get_encoder_cap(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION);

    init_params.encodeWidth = encoder_params.width;
    init_params.darWidth = encoder_params.width;
    init_params.encodeHeight = encoder_params.height;
    init_params.darHeight = encoder_params.height;
    init_params.frameRateNum = client_config.framerate;
    init_params.frameRateDen = 1;
    if (client_config.framerateX100 > 0) {
      AVRational fps = video::framerateX100_to_rational(client_config.framerateX100);
      init_params.frameRateNum = fps.num;
      init_params.frameRateDen = fps.den;
    }

    NV_ENC_PRESET_CONFIG preset_config = {min_struct_version(NV_ENC_PRESET_CONFIG_VER), {min_struct_version(NV_ENC_CONFIG_VER, 7, 8)}};
    if (nvenc_failed(nvenc->nvEncGetEncodePresetConfigEx(encoder, init_params.encodeGUID, init_params.presetGUID, init_params.tuningInfo, &preset_config))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodePresetConfigEx() failed: " << last_nvenc_error_string;
      return false;
    }

    NV_ENC_CONFIG enc_config = preset_config.presetCfg;
    enc_config.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
    enc_config.gopLength = NVENC_INFINITE_GOPLENGTH;
    enc_config.frameIntervalP = 1;
    enc_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    enc_config.rcParams.zeroReorderDelay = 1;
    enc_config.rcParams.enableLookahead = 0;
    enc_config.rcParams.lowDelayKeyFrameScale = 1;
    enc_config.rcParams.multiPass = config.two_pass == nvenc_two_pass::quarter_resolution ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION :
                                    config.two_pass == nvenc_two_pass::full_resolution    ? NV_ENC_TWO_PASS_FULL_RESOLUTION :
                                                                                            NV_ENC_MULTI_PASS_DISABLED;

    enc_config.rcParams.enableAQ = config.adaptive_quantization;
    enc_config.rcParams.averageBitRate = client_config.bitrate * 1000;

    if (get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
      enc_config.rcParams.vbvBufferSize = client_config.bitrate * 1000 / client_config.framerate;
      if (config.vbv_percentage_increase > 0) {
        enc_config.rcParams.vbvBufferSize += enc_config.rcParams.vbvBufferSize * config.vbv_percentage_increase / 100;
      }
      // §K.nvenc.vbvinit v1.5.201 (Fix Q.4)：vbvInitialDelay 預設 0 → VBV buffer
      // 啟動時是空的 → 第一個 GOP 幾乎沒有 bit 配額 → 串流前幾秒嚴重壓縮（模糊），
      // 之後 buffer 充飽才銳利。設為 = vbvBufferSize（啟動時視 buffer 為滿）讓第一個
      // GOP 拿到完整配額 → 一開始就銳利。額外啟動延遲僅 ~1.2 frame（可忽略），
      // 驅動不支援自訂 initial delay 會自動 clamp。
      enc_config.rcParams.vbvInitialDelay = enc_config.rcParams.vbvBufferSize;
    }

    auto set_h264_hevc_common_format_config = [&](auto &format_config) {
      format_config.repeatSPSPPS = 1;
      format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
      format_config.sliceMode = 3;
      format_config.sliceModeData = client_config.slicesPerFrame;
      if (buffer_is_yuv444()) {
        format_config.chromaFormatIDC = 3;
      }
      format_config.enableFillerDataInsertion = config.insert_filler_data;
    };

    auto set_ref_frames = [&](uint32_t &ref_frames_option, NV_ENC_NUM_REF_FRAMES &L0_option, uint32_t ref_frames_default) {
      if (client_config.numRefFrames > 0) {
        ref_frames_option = client_config.numRefFrames;
      } else {
        ref_frames_option = ref_frames_default;
      }
      if (ref_frames_option > 0 && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES)) {
        ref_frames_option = 1;
        encoder_params.rfi = false;
      }
      encoder_params.ref_frames_in_dpb = ref_frames_option;
      // This limits ref frames any frame can use to 1, but allows larger buffer size for fallback if some frames are invalidated through rfi
      L0_option = NV_ENC_NUM_REF_FRAMES_1;
    };

    auto set_minqp_if_enabled = [&](int value) {
      if (config.enable_min_qp) {
        enc_config.rcParams.enableMinQP = 1;
        enc_config.rcParams.minQP.qpInterP = value;
        enc_config.rcParams.minQP.qpIntra = value;
      }
    };

    auto fill_h264_hevc_vui = [&](auto &vui_config) {
      vui_config.videoSignalTypePresentFlag = 1;
      vui_config.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
      vui_config.videoFullRangeFlag = colorspace.full_range;
      vui_config.colourDescriptionPresentFlag = 1;
      vui_config.colourPrimaries = colorspace.primaries;
      vui_config.transferCharacteristics = colorspace.tranfer_function;
      vui_config.colourMatrix = colorspace.matrix;
      vui_config.chromaSampleLocationFlag = buffer_is_yuv444() ? 0 : 1;
      vui_config.chromaSampleLocationTop = 0;
      vui_config.chromaSampleLocationBot = 0;

      // This is critical for low decoding latency on certain devices
      vui_config.bitstreamRestrictionFlag = 1;
    };

    // VipleStream 2.0 §VR（F16）：VR profile 一律開 intra refresh（不依賴 client 的 SDP），
    // H.264／HEVC／AV1 都接上。intraRefreshCnt = LOSS 觸發的 wave 長度（encode_frame 的
    // forceIntraRefreshWithFrameCnt 用同一個 N）；intraRefreshPeriod = 週期性安全網。
    // 一般 session 不經過這裡，既有的 HEVC SDP intra refresh 路徑完全不變。
    auto configure_vr_intra_refresh = [&](auto &format_config, const char *codec_name) {
      if (!get_encoder_cap(NV_ENC_CAPS_SUPPORT_INTRA_REFRESH)) {
        BOOST_LOG(warning) << "[VIPLE-VR-ENC] intra refresh unsupported: codec=" << codec_name
                           << " -- LOSS recovery falls back to IDR";
        return;
      }

      // SDK header 只規定 cnt 要小於 period，沒有定義 period=0 的語意（可能被
      // NvEncInitializeEncoder 以 INVALID_PARAM 拒絕，連帶整個 VR session 起不來），
      // 所以「關閉週期性安全網」改用一個實際上不會到的間隔（2^20 幀，90 Hz 約 3.2 小時）。
      constexpr uint32_t kPeriodOff = 1u << 20;
      const uint32_t cnt = (uint32_t) std::clamp(client_config.vrIntraRefreshFrames > 0 ? client_config.vrIntraRefreshFrames : 8, 2, 60);
      const bool periodic = client_config.vrIntraRefreshPeriodFrames > 0;
      uint32_t period = periodic ? (uint32_t) client_config.vrIntraRefreshPeriodFrames : kPeriodOff;
      if (period <= cnt) {
        BOOST_LOG(warning) << "[VIPLE-VR-ENC] intra refresh period " << period << " <= cnt " << cnt
                           << " (NVENC requires period > cnt), adjusted to " << cnt + 1;
        period = cnt + 1;
      }

      format_config.enableIntraRefresh = 1;
      format_config.intraRefreshCnt = cnt;
      format_config.intraRefreshPeriod = period;

      // AV1 的 config 沒有 singleSliceIntraRefresh
      bool single_slice = false;
      if constexpr (requires { format_config.singleSliceIntraRefresh; }) {
        if (get_encoder_cap(NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH)) {
          format_config.singleSliceIntraRefresh = 1;
          single_slice = true;
        }
      }

      encoder_params.vr_intra_refresh = true;
      BOOST_LOG(info) << "[VIPLE-VR-ENC] intra refresh: codec=" << codec_name
                      << " cnt=" << cnt
                      << " period=" << (periodic ? std::to_string(period) : std::string("off"))
                      << " singleSlice=" << (single_slice ? 1 : 0);
    };

    switch (client_config.videoFormat) {
      case 0:
        {
          // H.264
          enc_config.profileGUID = buffer_is_yuv444() ? NV_ENC_H264_PROFILE_HIGH_444_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;
          auto &format_config = enc_config.encodeCodecConfig.h264Config;
          set_h264_hevc_common_format_config(format_config);
          if (config.h264_cavlc || !get_encoder_cap(NV_ENC_CAPS_SUPPORT_CABAC)) {
            format_config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
          } else {
            format_config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
          }
          set_ref_frames(format_config.maxNumRefFrames, format_config.numRefL0, 5);
          set_minqp_if_enabled(config.min_qp_h264);
          fill_h264_hevc_vui(format_config.h264VUIParameters);
          if (client_config.vrProfile) {
            configure_vr_intra_refresh(format_config, "h264");
          }
          break;
        }

      case 1:
        {
          // HEVC
          auto &format_config = enc_config.encodeCodecConfig.hevcConfig;
          set_h264_hevc_common_format_config(format_config);
          if (buffer_is_10bit()) {
#if NVENCAPI_MAJOR_VERSION >= 13
            format_config.outputBitDepth = NV_ENC_BIT_DEPTH_10;
#else
            format_config.pixelBitDepthMinus8 = 2;
#endif
          }
          set_ref_frames(format_config.maxNumRefFramesInDPB, format_config.numRefL0, 5);
          set_minqp_if_enabled(config.min_qp_hevc);
          fill_h264_hevc_vui(format_config.hevcVUIParameters);
          if (client_config.vrProfile) {
            // §VR：VR profile 的 intra refresh 取代下面 SDP 的固定 300/299 設定
            configure_vr_intra_refresh(format_config, "hevc");
          } else if (client_config.enableIntraRefresh == 1) {
            if (get_encoder_cap(NV_ENC_CAPS_SUPPORT_INTRA_REFRESH)) {
              format_config.enableIntraRefresh = 1;
              format_config.intraRefreshPeriod = 300;
              format_config.intraRefreshCnt = 299;
              if (get_encoder_cap(NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH)) {
                format_config.singleSliceIntraRefresh = 1;
              } else {
                BOOST_LOG(warning) << "NvEnc: Single Slice Intra Refresh not supported";
              }
            } else {
              BOOST_LOG(error) << "NvEnc: Client asked for intra-refresh but the encoder does not support intra-refresh";
            }
          }
          break;
        }

      case 2:
        {
          // AV1
          auto &format_config = enc_config.encodeCodecConfig.av1Config;
          format_config.repeatSeqHdr = 1;
          format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
          if (buffer_is_yuv444()) {
            format_config.chromaFormatIDC = 3;
          }
          format_config.enableBitstreamPadding = config.insert_filler_data;
          if (buffer_is_10bit()) {
#if NVENCAPI_MAJOR_VERSION >= 13
            format_config.inputBitDepth = NV_ENC_BIT_DEPTH_10;
            format_config.outputBitDepth = NV_ENC_BIT_DEPTH_10;
#else
            format_config.inputPixelBitDepthMinus8 = 2;
            format_config.pixelBitDepthMinus8 = 2;
#endif
          }
          format_config.colorPrimaries = colorspace.primaries;
          format_config.transferCharacteristics = colorspace.tranfer_function;
          format_config.matrixCoefficients = colorspace.matrix;
          format_config.colorRange = colorspace.full_range;
          format_config.chromaSamplePosition = buffer_is_yuv444() ? 0 : 1;
          set_ref_frames(format_config.maxNumRefFramesInDPB, format_config.numFwdRefs, 8);
          set_minqp_if_enabled(config.min_qp_av1);
          if (client_config.vrProfile) {
            configure_vr_intra_refresh(format_config, "av1");
          }

          if (client_config.slicesPerFrame > 1) {
            // NVENC only supports slice counts that are powers of two, so we'll pick powers of two
            // with bias to rows due to hopefully more similar macroblocks with a row vs a column.
            format_config.numTileRows = std::pow(2, std::ceil(std::log2(client_config.slicesPerFrame) / 2));
            format_config.numTileColumns = std::pow(2, std::floor(std::log2(client_config.slicesPerFrame) / 2));
          }
          break;
        }
    }

    init_params.encodeConfig = &enc_config;

    if (nvenc_failed(nvenc->nvEncInitializeEncoder(encoder, &init_params))) {
      BOOST_LOG(error) << "NvEnc: NvEncInitializeEncoder() failed: " << last_nvenc_error_string;
      return false;
    }

    // VipleStream §ABR: deep-copy init params for mid-stream reconfigure
    stored_enc_config = enc_config;
    stored_init_params = init_params;
    stored_init_params.encodeConfig = &stored_enc_config;
    pin_level_for_reconfigure(client_config.videoFormat);

    if (async_event_handle) {
      NV_ENC_EVENT_PARAMS event_params = {min_struct_version(NV_ENC_EVENT_PARAMS_VER)};
      event_params.completionEvent = async_event_handle;
      if (nvenc_failed(nvenc->nvEncRegisterAsyncEvent(encoder, &event_params))) {
        BOOST_LOG(error) << "NvEnc: NvEncRegisterAsyncEvent() failed: " << last_nvenc_error_string;
        return false;
      }
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER create_bitstream_buffer = {min_struct_version(NV_ENC_CREATE_BITSTREAM_BUFFER_VER)};
    if (nvenc_failed(nvenc->nvEncCreateBitstreamBuffer(encoder, &create_bitstream_buffer))) {
      BOOST_LOG(error) << "NvEnc: NvEncCreateBitstreamBuffer() failed: " << last_nvenc_error_string;
      return false;
    }
    output_bitstream = create_bitstream_buffer.bitstreamBuffer;

    if (!create_and_register_input_buffer()) {
      return false;
    }

    {
      auto f = stat_trackers::two_digits_after_decimal();
      BOOST_LOG(debug) << "NvEnc: requested encoded frame size " << f % (client_config.bitrate / 8. / client_config.framerate) << " kB";
    }

    {
      auto video_format_string = client_config.videoFormat == 0 ? "H.264 " :
                                 client_config.videoFormat == 1 ? "HEVC " :
                                 client_config.videoFormat == 2 ? "AV1 " :
                                                                  " ";
      std::string extra;
      if (init_params.enableEncodeAsync) {
        extra += " async";
      }
      if (buffer_is_yuv444()) {
        extra += " yuv444";
      }
      if (buffer_is_10bit()) {
        extra += " 10-bit";
      }
      if (enc_config.rcParams.multiPass != NV_ENC_MULTI_PASS_DISABLED) {
        extra += " two-pass";
      }
      if (config.vbv_percentage_increase > 0 && get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
        extra += std::format(" vbv+{}", config.vbv_percentage_increase);
      }
      if (encoder_params.rfi) {
        extra += " rfi";
      }
      if (init_params.enableWeightedPrediction) {
        extra += " weighted-prediction";
      }
      if (enc_config.rcParams.enableAQ) {
        extra += " spatial-aq";
      }
      if (enc_config.rcParams.enableMinQP) {
        extra += std::format(" qpmin={}", enc_config.rcParams.minQP.qpInterP);
      }
      if (config.insert_filler_data) {
        extra += " filler-data";
      }

      BOOST_LOG(info) << "NvEnc: created encoder " << video_format_string << quality_preset_string_from_guid(init_params.presetGUID) << extra;
    }

    encoder_state = {};
    fail_guard.disable();
    return true;
  }

  void nvenc_base::destroy_encoder() {
    if (output_bitstream) {
      if (nvenc_failed(nvenc->nvEncDestroyBitstreamBuffer(encoder, output_bitstream))) {
        BOOST_LOG(error) << "NvEnc: NvEncDestroyBitstreamBuffer() failed: " << last_nvenc_error_string;
      }
      output_bitstream = nullptr;
    }
    if (encoder && async_event_handle) {
      NV_ENC_EVENT_PARAMS event_params = {min_struct_version(NV_ENC_EVENT_PARAMS_VER)};
      event_params.completionEvent = async_event_handle;
      if (nvenc_failed(nvenc->nvEncUnregisterAsyncEvent(encoder, &event_params))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnregisterAsyncEvent() failed: " << last_nvenc_error_string;
      }
    }
    if (registered_input_buffer) {
      if (nvenc_failed(nvenc->nvEncUnregisterResource(encoder, registered_input_buffer))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnregisterResource() failed: " << last_nvenc_error_string;
      }
      registered_input_buffer = nullptr;
    }
    if (encoder) {
      if (nvenc_failed(nvenc->nvEncDestroyEncoder(encoder))) {
        BOOST_LOG(error) << "NvEnc: NvEncDestroyEncoder() failed: " << last_nvenc_error_string;
      }
      encoder = nullptr;
    }

    encoder_state = {};
    encoder_params = {};
  }

  nvenc_encoded_frame nvenc_base::encode_frame(uint64_t frame_index, bool force_idr, uint32_t force_intra_refresh_frames) {
    if (!encoder) {
      return {};
    }

    assert(registered_input_buffer);
    assert(output_bitstream);

    // [VIPLE-NVENC-PROF] timing instrumentation for HEVC 1440p 60fps cap
    // investigation.  Baseline: at 1440p hevc_nvenc was producing only 60 stream
    // packets/sec on the test host (RTX 5060 Ti, NVENC P1) while h264_nvenc /
    // av1_nvenc produced 120 — codec-specific behavior we want to localize.
    auto _profEntry = std::chrono::steady_clock::now();

    if (!synchronize_input_buffer()) {
      BOOST_LOG(error) << "NvEnc: failed to synchronize input buffer";
      return {};
    }

    NV_ENC_MAP_INPUT_RESOURCE mapped_input_buffer = {min_struct_version(NV_ENC_MAP_INPUT_RESOURCE_VER)};
    mapped_input_buffer.registeredResource = registered_input_buffer;

    if (nvenc_failed(nvenc->nvEncMapInputResource(encoder, &mapped_input_buffer))) {
      BOOST_LOG(error) << "NvEnc: NvEncMapInputResource() failed: " << last_nvenc_error_string;
      return {};
    }
    auto unmap_guard = util::fail_guard([&] {
      if (nvenc_failed(nvenc->nvEncUnmapInputResource(encoder, mapped_input_buffer.mappedResource))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnmapInputResource() failed: " << last_nvenc_error_string;
      }
    });

    NV_ENC_PIC_PARAMS pic_params = {min_struct_version(NV_ENC_PIC_PARAMS_VER, 4, 6)};
    pic_params.inputWidth = encoder_params.width;
    pic_params.inputHeight = encoder_params.height;
    pic_params.encodePicFlags = force_idr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;
    pic_params.inputTimeStamp = frame_index;
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic_params.inputBuffer = mapped_input_buffer.mappedResource;
    pic_params.bufferFmt = mapped_input_buffer.mappedBufferFmt;
    pic_params.outputBitstream = output_bitstream;
    pic_params.completionEvent = async_event_handle;

    // VipleStream 2.0 §VR：這一幀開始一波 intra refresh。IDR 本身就是完整的恢復點，同一幀
    // 兩者都要求時只送 IDR。codecPicParams 已由上面的 aggregate 初始化清零。
    if (force_intra_refresh_frames > 0 && !force_idr && encoder_params.vr_intra_refresh) {
      switch (encoder_params.video_format) {
        case 0:
          pic_params.codecPicParams.h264PicParams.forceIntraRefreshWithFrameCnt = force_intra_refresh_frames;
          break;
        case 1:
          pic_params.codecPicParams.hevcPicParams.forceIntraRefreshWithFrameCnt = force_intra_refresh_frames;
          break;
        case 2:
          pic_params.codecPicParams.av1PicParams.forceIntraRefreshWithFrameCnt = force_intra_refresh_frames;
          break;
        default:
          break;
      }
    }

    if (nvenc_failed(nvenc->nvEncEncodePicture(encoder, &pic_params))) {
      BOOST_LOG(error) << "NvEnc: NvEncEncodePicture() failed: " << last_nvenc_error_string;
      return {};
    }

    NV_ENC_LOCK_BITSTREAM lock_bitstream = {min_struct_version(NV_ENC_LOCK_BITSTREAM_VER, 1, 2)};
    lock_bitstream.outputBitstream = output_bitstream;
    lock_bitstream.doNotWait = async_event_handle ? 1 : 0;

    auto _profBeforeWait = std::chrono::steady_clock::now();
    if (async_event_handle && !wait_for_async_event(100)) {
      BOOST_LOG(error) << "NvEnc: frame " << frame_index << " encode wait timeout";
      return {};
    }
    auto _profAfterWait = std::chrono::steady_clock::now();

    if (nvenc_failed(nvenc->nvEncLockBitstream(encoder, &lock_bitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncLockBitstream() failed: " << last_nvenc_error_string;
      return {};
    }
    auto _profAfterLock = std::chrono::steady_clock::now();

    auto data_pointer = (uint8_t *) lock_bitstream.bitstreamBufferPtr;
    nvenc_encoded_frame encoded_frame {
      {data_pointer, data_pointer + lock_bitstream.bitstreamSizeInBytes},
      lock_bitstream.outputTimeStamp,
      lock_bitstream.pictureType == NV_ENC_PIC_TYPE_IDR,
      encoder_state.rfi_needs_confirmation,
    };

    if (encoder_state.rfi_needs_confirmation) {
      // Invalidation request has been fulfilled, and video network packet will be marked as such
      encoder_state.rfi_needs_confirmation = false;
    }

    encoder_state.last_encoded_frame_index = frame_index;

    if (encoded_frame.idr) {
      BOOST_LOG(debug) << "NvEnc: idr frame " << encoded_frame.frame_index;
    }

    if (nvenc_failed(nvenc->nvEncUnlockBitstream(encoder, lock_bitstream.outputBitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncUnlockBitstream() failed: " << last_nvenc_error_string;
    }

    encoder_state.frame_size_logger.collect_and_log(encoded_frame.data.size() / 1000.);

    // [VIPLE-NVENC-PROF] tracker windows.  total = entry → after-unlock; the
    // sub-deltas isolate where the time goes (sync+map+encode_picture, then
    // wait_for_async_event, then lock+copy+unlock).
    auto _profExit = std::chrono::steady_clock::now();
    using ms_d = std::chrono::duration<double, std::milli>;
    encoder_state.encode_total_ms_logger.collect_and_log(std::chrono::duration_cast<ms_d>(_profExit       - _profEntry).count());
    encoder_state.encode_async_ms_logger.collect_and_log(std::chrono::duration_cast<ms_d>(_profAfterWait  - _profBeforeWait).count());
    encoder_state.encode_lock_ms_logger.collect_and_log( std::chrono::duration_cast<ms_d>(_profAfterLock  - _profAfterWait).count());
    encoder_state.encode_bsize_logger.collect_and_log(static_cast<double>(encoded_frame.data.size()));

    // [VIPLE-NVENC-RATE] explicit per-second call rate counter so we can tell
    // whether the encoder loop actually iterates 120/sec or is being gated.
    {
      static thread_local std::chrono::steady_clock::time_point bucketStart{};
      static thread_local uint32_t bucketCount = 0;
      if (bucketStart.time_since_epoch().count() == 0) {
        bucketStart = _profExit;
      }
      bucketCount++;
      auto bucketMs = std::chrono::duration_cast<ms_d>(_profExit - bucketStart).count();
      if (bucketMs >= 5000.0) {
        double rate = bucketCount * 1000.0 / bucketMs;
        BOOST_LOG(info) << "[VIPLE-NVENC-RATE] encode_frame called " << bucketCount
                        << " times over " << bucketMs << "ms = " << rate << " calls/sec";
        bucketCount = 0;
        bucketStart = _profExit;
      }
    }

    return encoded_frame;
  }

  bool nvenc_base::invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) {
    if (!encoder || !encoder_params.rfi) {
      return false;
    }

    if (first_frame >= encoder_state.last_rfi_range.first &&
        last_frame <= encoder_state.last_rfi_range.second) {
      BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame << " already done";
      return true;
    }

    encoder_state.rfi_needs_confirmation = true;

    if (last_frame < first_frame) {
      BOOST_LOG(error) << "NvEnc: invaid rfi request " << first_frame << "-" << last_frame << ", generating IDR";
      return false;
    }

    BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame << " expanding to last encoded frame " << encoder_state.last_encoded_frame_index;
    last_frame = encoder_state.last_encoded_frame_index;

    encoder_state.last_rfi_range = {first_frame, last_frame};

    if (last_frame - first_frame + 1 >= encoder_params.ref_frames_in_dpb) {
      BOOST_LOG(debug) << "NvEnc: rfi request too large, generating IDR";
      return false;
    }

    for (auto i = first_frame; i <= last_frame; i++) {
      if (nvenc_failed(nvenc->nvEncInvalidateRefFrames(encoder, i))) {
        BOOST_LOG(error) << "NvEnc: NvEncInvalidateRefFrames() " << i << " failed: " << last_nvenc_error_string;
        return false;
      }
    }

    return true;
  }

  // VipleStream §SF-PARAMSETS（2026-09-28，Steam Frame 凍結根因）：level／tier 交給 NVENC 自動選時，
  // ABR reconfigure 降碼會讓它依新碼率重選（實測 HEVC level 4.1：26 Mbps 的 High tier 在降到 18 Mbps
  // 後變成 Main tier），下一個 IDR 的 VPS/SPS 就跟著變。Qualcomm iris（Steam Frame 的硬體解碼器）
  // 看到參數集改變就發 source change，FFmpeg v4l2m2m 處理不了而永久卡住。這裡在初始化後讀回
  // NVENC 實際寫出的 SPS，把 level（HEVC 另加 tier）寫死進 reconfigure 用的設定，讓整個 session
  // 的參數集維持不變。ABR 只在初始碼率以下調整，初始選出的 level／tier 一定夠用。
  // AV1（seq_level_idx／seq_tier）尚未處理：Frame 沒有 AV1 硬解，列 TODO。
  void nvenc_base::pin_level_for_reconfigure(int video_format) {
    const bool hevc = video_format == 1;
    const bool h264 = video_format == 0;
    pinned_video_format = video_format;
    if (!hevc && !h264) {
      return;
    }
    if ((hevc && stored_enc_config.encodeCodecConfig.hevcConfig.level != NV_ENC_LEVEL_AUTOSELECT) ||
        (h264 && stored_enc_config.encodeCodecConfig.h264Config.level != NV_ENC_LEVEL_AUTOSELECT)) {
      return;  // level 已經明確指定，本來就不會變
    }

    std::vector<uint8_t> header(1024);
    uint32_t header_size = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = {min_struct_version(NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER)};
    payload.inBufferSize = static_cast<uint32_t>(header.size());
    payload.spsppsBuffer = header.data();
    payload.outSPSPPSPayloadSize = &header_size;
    if (nvenc_failed(nvenc->nvEncGetSequenceParams(encoder, &payload))) {
      BOOST_LOG(warning) << "[VIPLE-ABR] NvEnc: NvEncGetSequenceParams() failed, level not pinned: " << last_nvenc_error_string;
      return;
    }
    header.resize(std::min<size_t>(header_size, header.size()));

    // 在 Annex-B 裡找 SPS（HEVC nal_unit_type 33、H.264 7），去掉 emulation prevention 後取前 16 bytes
    std::vector<uint8_t> rbsp;
    for (size_t i = 0; i + 3 < header.size() && rbsp.empty(); i++) {
      if (header[i] != 0 || header[i + 1] != 0 || header[i + 2] != 1) {
        continue;
      }
      const size_t nal = i + 3;
      const int type = hevc ? ((header[nal] >> 1) & 0x3F) : (header[nal] & 0x1F);
      if (type != (hevc ? 33 : 7)) {
        continue;
      }
      int zeros = 0;
      for (size_t k = nal + (hevc ? 2 : 1); k < header.size() && rbsp.size() < 16; k++) {
        if (zeros >= 2 && header[k] == 3) {
          zeros = 0;
          continue;
        }
        zeros = header[k] == 0 ? zeros + 1 : 0;
        rbsp.push_back(header[k]);
      }
    }

    if (hevc && rbsp.size() >= 13) {
      // [0] vps_id／max_sub_layers／nesting，[1] profile_space(2) tier(1) profile_idc(5)，
      // [2..5] compatibility flags，[6..11] constraint flags，[12] general_level_idc
      const uint32_t tier = (rbsp[1] >> 5) & 1;
      const uint32_t level = rbsp[12];
      unpinned_level = stored_enc_config.encodeCodecConfig.hevcConfig.level;
      unpinned_tier = stored_enc_config.encodeCodecConfig.hevcConfig.tier;
      stored_enc_config.encodeCodecConfig.hevcConfig.level = level;
      stored_enc_config.encodeCodecConfig.hevcConfig.tier = tier ? NV_ENC_TIER_HEVC_HIGH : NV_ENC_TIER_HEVC_MAIN;
      level_pinned = true;
      BOOST_LOG(info) << "[VIPLE-ABR] NvEnc: pinned HEVC level_idc=" << level << " tier=" << (tier ? "high" : "main")
                      << " for mid-stream reconfigure";
    }
    else if (h264 && rbsp.size() >= 3) {
      // [0] profile_idc，[1] constraint_set flags，[2] level_idc；level 1b 以 constraint_set3 表示
      uint32_t level = rbsp[2];
      if (level == 11 && (rbsp[1] & 0x10) && (rbsp[0] == 66 || rbsp[0] == 77 || rbsp[0] == 88)) {
        level = NV_ENC_LEVEL_H264_1b;
      }
      unpinned_level = stored_enc_config.encodeCodecConfig.h264Config.level;
      stored_enc_config.encodeCodecConfig.h264Config.level = level;
      level_pinned = true;
      BOOST_LOG(info) << "[VIPLE-ABR] NvEnc: pinned H.264 level_idc=" << level << " for mid-stream reconfigure";
    }
    else {
      BOOST_LOG(warning) << "[VIPLE-ABR] NvEnc: SPS not found in sequence header (" << header_size
                         << " bytes), level not pinned";
    }
  }

  // 驅動不接受明確的 level／tier 時退回自動選擇（參數集可能又會隨碼率變，但 ABR 不能因此失效）
  void nvenc_base::unpin_level_for_reconfigure() {
    if (!level_pinned) {
      return;
    }
    if (pinned_video_format == 1) {
      stored_enc_config.encodeCodecConfig.hevcConfig.level = unpinned_level;
      stored_enc_config.encodeCodecConfig.hevcConfig.tier = unpinned_tier;
    }
    else {
      stored_enc_config.encodeCodecConfig.h264Config.level = unpinned_level;
    }
    level_pinned = false;
  }

  // VipleStream §ABR: mid-stream bitrate reconfigure via NvEncReconfigureEncoder
  // §ABR-RAMP: force_idr 由呼叫端依方向決定——降速（擁塞救援）reset + IDR
  // 快速收斂；回升不 reset 平滑過渡（NVENC 支援動態改 rcParams），畫面無感。
  bool nvenc_base::reconfigure_bitrate(int bitrate_kbps, int framerate, bool force_idr) {
    if (!encoder || !nvenc) {
      BOOST_LOG(error) << "[VIPLE-ABR] NvEnc: reconfigure called on uninitialized encoder";
      return false;
    }

    uint32_t old_avg = stored_enc_config.rcParams.averageBitRate;
    uint32_t old_vbv = stored_enc_config.rcParams.vbvBufferSize;
    uint32_t new_avg = static_cast<uint32_t>(bitrate_kbps) * 1000;

    stored_enc_config.rcParams.averageBitRate = new_avg;
    if (old_avg > 0 && old_vbv > 0) {
      stored_enc_config.rcParams.vbvBufferSize = static_cast<uint32_t>(
        static_cast<uint64_t>(new_avg) * old_vbv / old_avg);
      stored_enc_config.rcParams.vbvInitialDelay = stored_enc_config.rcParams.vbvBufferSize;
    }

    NV_ENC_RECONFIGURE_PARAMS reconfigure_params = {min_struct_version(NV_ENC_RECONFIGURE_PARAMS_VER)};
    reconfigure_params.reInitEncodeParams = stored_init_params;
    reconfigure_params.resetEncoder = force_idr ? 1 : 0;
    reconfigure_params.forceIDR = force_idr ? 1 : 0;

    bool reconfigured = !nvenc_failed(nvenc->nvEncReconfigureEncoder(encoder, &reconfigure_params));
    if (!reconfigured && level_pinned) {
      // §SF-PARAMSETS：固定的 level／tier 被拒時退回自動選擇重試一次，ABR 不因固定而失效
      BOOST_LOG(warning) << "[VIPLE-ABR] NvEnc: reconfigure rejected with pinned level/tier ("
                         << last_nvenc_error_string << "), retrying with auto-selected level";
      unpin_level_for_reconfigure();
      reconfigure_params.reInitEncodeParams = stored_init_params;
      reconfigured = !nvenc_failed(nvenc->nvEncReconfigureEncoder(encoder, &reconfigure_params));
    }
    if (!reconfigured) {
      BOOST_LOG(error) << "[VIPLE-ABR] NvEnc: NvEncReconfigureEncoder() failed: " << last_nvenc_error_string;
      stored_enc_config.rcParams.averageBitRate = old_avg;
      stored_enc_config.rcParams.vbvBufferSize = old_vbv;
      stored_enc_config.rcParams.vbvInitialDelay = old_vbv;
      return false;
    }

    BOOST_LOG(info) << "[VIPLE-ABR] NvEnc: bitrate reconfigured to " << bitrate_kbps << " kbps"
                    << " (vbv=" << stored_enc_config.rcParams.vbvBufferSize
                    << (force_idr ? ", reset+IDR" : ", smooth") << ")";
    return true;
  }

  bool nvenc_base::nvenc_failed(NVENCSTATUS status) {
    auto status_string = [](NVENCSTATUS status) -> std::string {
      switch (status) {
#define nvenc_status_case(x) \
  case x: \
    return #x;
        nvenc_status_case(NV_ENC_SUCCESS);
        nvenc_status_case(NV_ENC_ERR_NO_ENCODE_DEVICE);
        nvenc_status_case(NV_ENC_ERR_UNSUPPORTED_DEVICE);
        nvenc_status_case(NV_ENC_ERR_INVALID_ENCODERDEVICE);
        nvenc_status_case(NV_ENC_ERR_INVALID_DEVICE);
        nvenc_status_case(NV_ENC_ERR_DEVICE_NOT_EXIST);
        nvenc_status_case(NV_ENC_ERR_INVALID_PTR);
        nvenc_status_case(NV_ENC_ERR_INVALID_EVENT);
        nvenc_status_case(NV_ENC_ERR_INVALID_PARAM);
        nvenc_status_case(NV_ENC_ERR_INVALID_CALL);
        nvenc_status_case(NV_ENC_ERR_OUT_OF_MEMORY);
        nvenc_status_case(NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
        nvenc_status_case(NV_ENC_ERR_UNSUPPORTED_PARAM);
        nvenc_status_case(NV_ENC_ERR_LOCK_BUSY);
        nvenc_status_case(NV_ENC_ERR_NOT_ENOUGH_BUFFER);
        nvenc_status_case(NV_ENC_ERR_INVALID_VERSION);
        nvenc_status_case(NV_ENC_ERR_MAP_FAILED);
        nvenc_status_case(NV_ENC_ERR_NEED_MORE_INPUT);
        nvenc_status_case(NV_ENC_ERR_ENCODER_BUSY);
        nvenc_status_case(NV_ENC_ERR_EVENT_NOT_REGISTERD);
        nvenc_status_case(NV_ENC_ERR_GENERIC);
        nvenc_status_case(NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY);
        nvenc_status_case(NV_ENC_ERR_UNIMPLEMENTED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_REGISTER_FAILED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_NOT_REGISTERED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_NOT_MAPPED);
        // Newer versions of sdk may add more constants, look for them at the end of NVENCSTATUS enum
#undef nvenc_status_case
        default:
          return std::to_string(status);
      }
    };

    last_nvenc_error_string.clear();
    if (status != NV_ENC_SUCCESS) {
      /* This API function gives broken strings more often than not
      if (nvenc && encoder) {
        last_nvenc_error_string = nvenc->nvEncGetLastErrorString(encoder);
        if (!last_nvenc_error_string.empty()) last_nvenc_error_string += " ";
      }
      */
      last_nvenc_error_string += status_string(status);
      return true;
    }

    return false;
  }

  uint32_t nvenc_base::min_struct_version(uint32_t version, uint32_t v11_struct_version, uint32_t v12_struct_version) {
    assert(minimum_api_version);

    // Mask off and replace the original NVENCAPI_VERSION
    version &= ~NVENCAPI_VERSION;
    version |= minimum_api_version;

    // If there's a struct version override, apply that too
    if (v11_struct_version || v12_struct_version) {
      version &= ~(0xFFu << 16);
      version |= (((minimum_api_version & 0xFF) >= 12) ? v12_struct_version : v11_struct_version) << 16;
    }

    return version;
  }
}  // namespace nvenc
