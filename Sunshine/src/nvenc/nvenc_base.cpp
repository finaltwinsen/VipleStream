/**
 * @file src/nvenc/nvenc_base.cpp
 * @brief Definitions for abstract platform-agnostic base of standalone NVENC encoder.
 */
// this include
#include "nvenc_base.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <string>
#include <vector>

// local includes
#include "src/abr_signals.h"
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
// VipleStream §K.1: accept both v12 and v13.  v13 renamed pixelBitDepthMinus8 →
// outputBitDepth (uint32_t→NV_ENC_BIT_DEPTH enum) and inputPixelBitDepthMinus8 →
// inputBitDepth.  Site-of-use uses #if NVENCAPI_MAJOR_VERSION >= 13.
//
// §NVENC-SPLIT（2026-11-26）：repo 內附的 third-party/nv-codec-headers 換成 13.0（Windows 的 FFmpeg 套件不帶
// ffnvcodec，編的就是這份；實際編譯命令裡 -isystem 的順序是 FFmpeg 套件在前，套件若帶了 ffnvcodec 會用套件的）。
// 預設不開 split 時，送給驅動的位元組必須和 12.0 標頭時完全相同，
// 13.0 與 12.0 版面不同、而本檔有用到的地方（用 offsetof／逐位元探測兩份標頭比對過）：
//   1. 結構版本號：一律改由 sdk12_ver::* 提供（見 nvenc_base.h）。
//   2. NV_ENC_INITIALIZE_PARAMS：13.0 是 1800 bytes、12.0 是 1808（reserved 少了 8 bytes）。用 12.0 版本號送時
//      驅動會讀 1808 bytes → 用 init_params_buffer 在尾端補 8 個 0。用到的欄位位移相同；13.0 新增的 splitEncodeMode:4
//      佔的是 12.0 的 enableOutputInVidmem 與 reservedBitFields 位元，只在原生版本時寫。
//   3. NV_ENC_RECONFIGURE_PARAMS：內含 INITIALIZE_PARAMS，所以 resetEncoder／forceIDR 從 1816 移到 1808 → send_reconfigure
//      在 12.0 結構時自己排 12.0 版面（1824 bytes）。
//   4. NV_ENC_CONFIG_HEVC：12.0 的 pixelBitDepthMinus8:3 在 13.0 變成 reserved3:3（位置相同），位元深度改成後面的
//      outputBitDepth／inputBitDepth（列舉值 8／10）。12.0 結構時寫 reserved3。
//   5. NV_ENC_CONFIG_AV1：12.0 的 inputPixelBitDepthMinus8:3／pixelBitDepthMinus8:3（offset 16 那個字的 bit 12～17）在
//      13.0 被 enableLTR、enableTemporalSVC、outputMaxCll、outputMasteringDisplay、reserved4 佔走 → 12.0 結構時直接寫那個字。
//   6. NV_ENC_PIC_PARAMS：codecPicParams 聯集 1552→1544，但後面以 reserved 補齊，用到的欄位（含 qpDeltaMap、
//      各 codec 的 forceIntraRefreshWithFrameCnt）位移與結構大小都相同。
//   7. NV_ENC_PRESET_CONFIG：位移相同，但 13.0 把 version 後的對齊補白改成具名的 `uint32_t reserved`，位置式初始化
//      `{ver, {cfg_ver}}` 會把 CONFIG 版本塞進 reserved（2.0.287 的 INVALID_VERSION）→ 一律用欄位名稱設定。
//      教訓：版面比對只看位移不夠，任何 `{a, b}` 位置式初始化都要對照新標頭的欄位順序。
//   8. 函式表（NV_ENCODE_API_FUNCTION_LIST）的 API 版本必須和 session 的 apiVersion 一致，兩個方向混搭都回
//      INVALID_VERSION → 13.0 與 11/12 之間切換時 recreate_function_list()。
//   其餘用到的結構（OPEN_ENCODE_SESSION_EX_PARAMS、CAPS_PARAM、PRESET_CONFIG、CONFIG、RC_PARAMS、H264／VUI、
//   EVENT_PARAMS、CREATE_BITSTREAM_BUFFER、MAP_INPUT_RESOURCE、LOCK_BITSTREAM、SEQUENCE_PARAM_PAYLOAD、REGISTER_RESOURCE、
//   函式表）大小與用到的欄位位移完全相同；用到的列舉值與 GUID 也相同。
#if NVENCAPI_VERSION != MAKE_NVENC_VER(12U, 0U) && \
    NVENCAPI_VERSION != MAKE_NVENC_VER(13U, 0U)
  #error Check and update NVENC code for backwards compatibility!
#endif

#if NVENCAPI_MAJOR_VERSION == 12
// 標頭是 12.0 時，sdk12_ver 的表必須和標頭一致
static_assert(nvenc::sdk12_ver::open_encode_session_ex_params == (NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::caps_param == (NV_ENC_CAPS_PARAM_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::initialize_params == (NV_ENC_INITIALIZE_PARAMS_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::preset_config == (NV_ENC_PRESET_CONFIG_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::config == (NV_ENC_CONFIG_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::event_params == (NV_ENC_EVENT_PARAMS_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::create_bitstream_buffer == (NV_ENC_CREATE_BITSTREAM_BUFFER_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::map_input_resource == (NV_ENC_MAP_INPUT_RESOURCE_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::pic_params == (NV_ENC_PIC_PARAMS_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::lock_bitstream == (NV_ENC_LOCK_BITSTREAM_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::sequence_param_payload == (NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::reconfigure_params == (NV_ENC_RECONFIGURE_PARAMS_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::register_resource == (NV_ENC_REGISTER_RESOURCE_VER & ~NVENCAPI_VERSION));
static_assert(nvenc::sdk12_ver::function_list == (NV_ENCODE_API_FUNCTION_LIST_VER & ~NVENCAPI_VERSION));
#endif

namespace {

#if NVENCAPI_MAJOR_VERSION >= 13
  // 12.0 標頭在 64 位元下的大小（見檔頭說明 2、3）。這些數字只對 64 位元成立。
  static_assert(sizeof(void *) == 8, "sdk12 layout constants assume a 64-bit build");
  constexpr size_t kSdk12InitParamsSize = 1808;
  constexpr size_t kSdk12ReconfigureParamsSize = 1824;
  constexpr size_t kSdk12ReconfigureInitOffset = 8;
  constexpr size_t kSdk12ReconfigureFlagsOffset = 1816;
  static_assert(sizeof(NV_ENC_INITIALIZE_PARAMS) < kSdk12InitParamsSize);
  static_assert(offsetof(NV_ENC_RECONFIGURE_PARAMS, reInitEncodeParams) == kSdk12ReconfigureInitOffset);
  static_assert(kSdk12ReconfigureInitOffset + kSdk12InitParamsSize == kSdk12ReconfigureFlagsOffset);
  // 12.0 的 presetCfg 也在 8（version 後是對齊補白）；13.0 在補白處放了具名的 reserved，所以只能用欄位名稱設定
  static_assert(offsetof(NV_ENC_PRESET_CONFIG, presetCfg) == 8);
  // NV_ENC_CAPS_PARAM 仍用 `{version, cap}` 位置式初始化：capsToQuery 必須緊接在 version 後
  static_assert(offsetof(NV_ENC_CAPS_PARAM, capsToQuery) == 4);
#endif

  // 給 nvEncInitializeEncoder 的記憶體：12.0 結構時驅動讀 1808 bytes，尾端要是 0（13.0 的結構只有 1800）
  struct init_params_buffer {
    NV_ENC_INITIALIZE_PARAMS params;
#if NVENCAPI_MAJOR_VERSION >= 13
    uint8_t sdk12_tail[kSdk12InitParamsSize - sizeof(NV_ENC_INITIALIZE_PARAMS)];
#endif
  };

#if NVENCAPI_MAJOR_VERSION >= 13
  static_assert(sizeof(init_params_buffer) >= kSdk12InitParamsSize);

  // 檔頭說明 5：12.0 結構的 AV1 10-bit。照 12.0 寫 inputPixelBitDepthMinus8 = 2、pixelBitDepthMinus8 = 2
  // （offset 16 那個 32 位元字的 bit 12～14 與 15～17）。先用兩版位置相同的 chromaFormatIDC（bit 7～8）核對這個字。
  bool set_av1_sdk12_10bit(NV_ENC_CONFIG_AV1 &cfg) {
    constexpr size_t word_offset = 16;
    static_assert(offsetof(NV_ENC_CONFIG_AV1, maxPartSize) + sizeof(NV_ENC_AV1_PART_SIZE) == word_offset);
    NV_ENC_CONFIG_AV1 probe {};
    probe.chromaFormatIDC = 3;
    uint32_t word = 0;
    std::memcpy(&word, reinterpret_cast<const uint8_t *>(&probe) + word_offset, sizeof(word));
    if (word != (3u << 7)) {
      return false;
    }
    std::memcpy(&word, reinterpret_cast<const uint8_t *>(&cfg) + word_offset, sizeof(word));
    word &= ~(0x3Fu << 12);
    word |= (2u << 12) | (2u << 15);
    std::memcpy(reinterpret_cast<uint8_t *>(&cfg) + word_offset, &word, sizeof(word));
    return true;
  }
#endif

  const char *split_encode_name(nvenc::nvenc_split_encode value) {
    switch (value) {
      case nvenc::nvenc_split_encode::disabled:
        return "disabled";
      case nvenc::nvenc_split_encode::auto_mode:
        return "auto";
      case nvenc::nvenc_split_encode::auto_forced:
        return "auto_forced";
      case nvenc::nvenc_split_encode::two:
        return "two";
      case nvenc::nvenc_split_encode::three:
        return "three";
      case nvenc::nvenc_split_encode::native_off:
        return "native_off";
    }
    return "unknown";
  }

  std::string api_version_string(uint32_t api_version) {
    // NVENCAPI_VERSION 格式：major | (minor << 24)
    return std::format("{}.{}", api_version & 0xFFFFFFu, api_version >> 24);
  }

  const char *codec_name(int video_format) {
    return video_format == 0 ? "h264" : video_format == 1 ? "hevc" :
                                          video_format == 2 ? "av1" :
                                                              "unknown";
  }

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
    bool split_attempted = false;
    if (create_encoder_impl(config, client_config, colorspace, buffer_format, true, split_attempted)) {
      return true;
    }
    if (!split_attempted) {
      return false;
    }
    // §NVENC-SPLIT：實驗選項不能讓串流開不起來。用 13.0 結構建立的任何一步失敗，都整個重來一次（走預設路徑）。
    BOOST_LOG(warning) << "[VIPLE-NVENC-SPLIT] encoder creation with split-frame failed (" << last_nvenc_error_string
                       << "), retrying without split";
    bool unused = false;
    return create_encoder_impl(config, client_config, colorspace, buffer_format, false, unused);
  }

  bool nvenc_base::split_encode_usable(const nvenc_config &config, const video::config_t &client_config, uint32_t &mode) {
    mode = 0;
    if (config.split_encode == nvenc_split_encode::disabled) {
      return false;  // 預設：不印任何東西，log 與現在相同
    }

    auto not_applied = [&](const std::string &reason) {
      BOOST_LOG(info) << "[VIPLE-NVENC-SPLIT] requested=" << split_encode_name(config.split_encode)
                      << " applied=none api=" << api_version_string(minimum_api_version)
                      << " codec=" << codec_name(client_config.videoFormat)
                      << " size=" << client_config.width << 'x' << client_config.height
                      << " reason=" << reason;
      return false;
    };

#if NVENCAPI_MAJOR_VERSION < 13
    return not_applied("built with NVENC SDK headers older than 13.0");
#else
    if (client_config.videoFormat != 1 && client_config.videoFormat != 2) {
      return not_applied("split-frame encoding is HEVC/AV1 only");
    }

    const uint32_t driver_max = driver_max_api_version();
    constexpr uint32_t needed = (13u << 4) | 0u;
    if (driver_max < needed) {
      return not_applied(driver_max ? std::format("driver supports NVENC API up to {}.{}, needs 13.0", driver_max >> 4, driver_max & 0xF) :
                                      std::string("NvEncodeAPIGetMaxSupportedVersion unavailable"));
    }

    switch (config.split_encode) {
      case nvenc_split_encode::auto_mode:
        mode = NV_ENC_SPLIT_AUTO_MODE;
        break;
      case nvenc_split_encode::auto_forced:
        mode = NV_ENC_SPLIT_AUTO_FORCED_MODE;
        break;
      case nvenc_split_encode::two:
        mode = NV_ENC_SPLIT_TWO_FORCED_MODE;
        break;
      case nvenc_split_encode::three:
        mode = NV_ENC_SPLIT_THREE_FORCED_MODE;
        break;
      case nvenc_split_encode::native_off:
        mode = NV_ENC_SPLIT_DISABLE_MODE;
        break;
      case nvenc_split_encode::disabled:
        return false;
    }
    return true;
#endif
  }

  bool nvenc_base::create_encoder_impl(const nvenc_config &config, const video::config_t &client_config, const nvenc_colorspace_t &colorspace, NV_ENC_BUFFER_FORMAT buffer_format, bool allow_split, bool &split_attempted) {
    // Pick the minimum NvEncode API version required to support the specified codec
    // to maximize driver compatibility. AV1 was introduced in SDK v12.0.
    minimum_api_version = (client_config.videoFormat <= 1) ? MAKE_NVENC_VER(11U, 0U) : MAKE_NVENC_VER(12U, 0U);

    if (!nvenc && !init_library()) {
      return false;
    }

    if (encoder) {
      // 舊 session 用它自己的結構版本收尾（destroy_encoder 最後才把 native_structs 清掉）
      destroy_encoder();
    }
    native_structs = false;

    // §NVENC-SPLIT：只有選項開著、codec 與驅動都符合時，這個 session 才改用標頭原生版本（13.0）的結構
    const uint32_t legacy_api_version = minimum_api_version;
    uint32_t split_mode = 0;
    if (allow_split && split_encode_usable(config, client_config, split_mode)) {
      minimum_api_version = NVENCAPI_VERSION;
      native_structs = true;
      split_attempted = true;
    }

    // §NVENC-SPLIT：函式表的 API 版本必須和這個 session 一致（開發機 RTX 3060、驅動 621.01 實測：兩個方向混搭
    // 都在 NvEncOpenEncodeSessionEx 回 NV_ENC_ERR_INVALID_VERSION）。預設路徑兩者都是 false，不會走進來。
    if (native_structs != function_list_native) {
      if (recreate_function_list()) {
        function_list_native = native_structs;
      } else if (native_structs) {
        // 換不成 13.0 的函式表：原本的 11/12 函式表還在，這個 session 照預設路徑建立
        BOOST_LOG(warning) << "[VIPLE-NVENC-SPLIT] requested=" << split_encode_name(config.split_encode)
                           << " applied=none api=" << api_version_string(legacy_api_version)
                           << " codec=" << codec_name(client_config.videoFormat)
                           << " size=" << client_config.width << 'x' << client_config.height
                           << " reason=could not create an API 13.0 function list";
        native_structs = false;
        minimum_api_version = legacy_api_version;
        split_attempted = false;
      } else {
        BOOST_LOG(error) << "NvEnc: could not recreate the NVENC function list for API " << api_version_string(minimum_api_version);
        return false;
      }
    }

    auto fail_guard = util::fail_guard([this] {
      destroy_encoder();
    });

    encoder_params.width = client_config.width;
    encoder_params.height = client_config.height;
    encoder_params.buffer_format = buffer_format;
    encoder_params.rfi = true;
    encoder_params.video_format = client_config.videoFormat;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params = {min_struct_version(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER, sdk12_ver::open_encode_session_ex_params)};
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

    if (encode_guid_count == 0 || encode_guid_count > 256) {
      BOOST_LOG(error) << "NvEnc: invalid encode guid count " << encode_guid_count;
      return false;
    }

    std::vector<GUID> encode_guids(encode_guid_count);
    if (nvenc_failed(nvenc->nvEncGetEncodeGUIDs(encoder, encode_guids.data(), (uint32_t) encode_guids.size(), &encode_guid_count))) {
      BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDs() failed: " << last_nvenc_error_string;
      return false;
    }

    // 檔頭說明 2：放在 init_params_buffer 裡，12.0 結構時驅動多讀的尾端是 0
    init_params_buffer init_params_storage {};
    NV_ENC_INITIALIZE_PARAMS &init_params = init_params_storage.params;
    init_params.version = min_struct_version(NV_ENC_INITIALIZE_PARAMS_VER, sdk12_ver::initialize_params);

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
        // §LOGDROP-1127：探測（/launch 前）一定會走到這裡，這只是「這個 GPU 不支援」不是錯誤
        BOOST_LOG(info) << "NvEnc: probe: this GPU has no " << client_config.videoFormat << " encoder";
        return false;
      }
    }

    auto get_encoder_cap = [&](NV_ENC_CAPS cap) {
      NV_ENC_CAPS_PARAM param = {min_struct_version(NV_ENC_CAPS_PARAM_VER, sdk12_ver::caps_param), cap};
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
        // §LOGDROP-1127：探測不支援時只記 info（不是串流錯誤）
        BOOST_LOG(info) << "NvEnc: probe: gpu max encode resolution " << supported_width << "x" << supported_height << ", requested " << encoder_params.width << "x" << encoder_params.height;
        return false;
      }
    }

    if (buffer_is_10bit() && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE)) {
      BOOST_LOG(info) << "NvEnc: probe: 10-bit encode is not supported by this gpu";
      return false;
    }

    if (buffer_is_yuv444() && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE)) {
      BOOST_LOG(info) << "NvEnc: probe: YUV 4:4:4 encode is not supported by this gpu";
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

    // §NVENC-SPLIT：一律用欄位名稱設定，不能用 `{ver, {cfg_ver}}` 的位置式初始化——13.0 在 version 後面多了
    // `uint32_t reserved`，位置式寫法會把 CONFIG 的版本塞進 reserved、presetCfg.version 變 0
    // （2.0.287 在 host 上 NvEncGetEncodePresetConfigEx() 回 NV_ENC_ERR_INVALID_VERSION 就是這個）。presetCfg 位移兩版相同（8）。
    NV_ENC_PRESET_CONFIG preset_config {};
    preset_config.version = min_struct_version(NV_ENC_PRESET_CONFIG_VER, sdk12_ver::preset_config);
    preset_config.presetCfg.version = min_struct_version(NV_ENC_CONFIG_VER, sdk12_ver::config, 7, 8);
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

    // §VR-FOVEA-QP（2026-11-11）：分塊的 VR 串流，上半張（兩眼的全視野那一塊）QP 加 vrPeriphQpDelta。
    // 用 NVENC 的 QP 差分圖：每個編碼區塊一個值，疊在位元率控制選的 QP 上，所以 CBR 照常運作，省下的位元自然流到
    // 注視點那塊。區塊大小依 codec（H.264 巨集區塊 16、HEVC CTB 32、AV1 superblock 64）。
    qp_delta_map.clear();
    encoder_params.qp_map_avg_x100 = 0;
    if (client_config.vrPeriphQpDelta > 0 && client_config.vrPeriphRows > 0) {
      const uint32_t blk = client_config.videoFormat == 0 ? 16u : (client_config.videoFormat == 1 ? 32u : 64u);
      const uint32_t bw = (encoder_params.width + blk - 1) / blk;
      const uint32_t bh = (encoder_params.height + blk - 1) / blk;
      // 只有整個區塊都落在上半張的列才加；交界不在區塊邊界時，跨界那一列留給注視點那塊的品質
      const uint32_t periph_rows = (std::min) (bh, (uint32_t) client_config.vrPeriphRows / blk);
      const int8_t delta = (int8_t) std::clamp(client_config.vrPeriphQpDelta, 0, 30);
      qp_delta_map.assign((size_t) bw * bh, 0);
      std::fill_n(qp_delta_map.begin(), (size_t) bw * periph_rows, delta);
      encoder_params.qp_map_avg_x100 = bh > 0 ? (int) (100u * (uint32_t) delta * periph_rows / bh) : 0;
      enc_config.rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;
      BOOST_LOG(info) << "[VIPLE-VR-ENC] periphery qp delta=+" << (int) delta << " rows=" << periph_rows << '/' << bh
                      << " block=" << blk << " map=" << bw << 'x' << bh << " avgShift=" << encoder_params.qp_map_avg_x100 / 100.0;
    }
    enc_config.rcParams.averageBitRate = client_config.bitrate * 1000;

    // §REVIEW-1118B：記下來給 reconfigure_framerate 用同一條式子。下面的式子本身不變。
    custom_vbv_buffer = false;
    vbv_percentage_increase = 0;
    if (get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
      custom_vbv_buffer = true;
      vbv_percentage_increase = config.vbv_percentage_increase;
      const auto safe_framerate = std::max(1, client_config.framerate);
      enc_config.rcParams.vbvBufferSize = static_cast<uint32_t>(static_cast<uint64_t>(client_config.bitrate) * 1000 / safe_framerate);
      if (config.vbv_percentage_increase > 0) {
        enc_config.rcParams.vbvBufferSize += static_cast<uint32_t>(static_cast<uint64_t>(enc_config.rcParams.vbvBufferSize) * config.vbv_percentage_increase / 100);
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
          if (native_structs) {
#if NVENCAPI_MAJOR_VERSION >= 13
            // 13.0 結構：輸入與輸出位元深度都明確給
            const auto depth = buffer_is_10bit() ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
            format_config.outputBitDepth = depth;
            format_config.inputBitDepth = depth;
#endif
          } else if (buffer_is_10bit()) {
#if NVENCAPI_MAJOR_VERSION >= 13
            // 檔頭說明 4：12.0 的 pixelBitDepthMinus8:3 就是 13.0 的 reserved3:3（同一組位元）
            format_config.reserved3 = 2;
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
          if (native_structs) {
#if NVENCAPI_MAJOR_VERSION >= 13
            const auto depth = buffer_is_10bit() ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
            format_config.inputBitDepth = depth;
            format_config.outputBitDepth = depth;
#endif
          } else if (buffer_is_10bit()) {
#if NVENCAPI_MAJOR_VERSION >= 13
            // 檔頭說明 5：12.0 的位元在 13.0 被別的欄位佔走，直接寫那個字
            if (!set_av1_sdk12_10bit(format_config)) {
              BOOST_LOG(error) << "NvEnc: unexpected NV_ENC_CONFIG_AV1 layout, cannot set 10-bit for SDK 12.0 structs";
              return false;
            }
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
            const auto log2_val = std::log2(client_config.slicesPerFrame);
            const auto rows_exp = static_cast<uint32_t>(std::ceil(log2_val / 2.0));
            const auto cols_exp = static_cast<uint32_t>(std::floor(log2_val / 2.0));
            format_config.numTileRows = 1u << rows_exp;
            format_config.numTileColumns = 1u << cols_exp;
          }
          break;
        }
    }

    init_params.encodeConfig = &enc_config;

#if NVENCAPI_MAJOR_VERSION >= 13
    if (native_structs) {
      // 13.0 新增的位元欄位；12.0 結構時這幾個位元是 enableOutputInVidmem／reservedBitFields，絕對不能寫
      init_params.splitEncodeMode = split_mode;
    }
#endif

    if (nvenc_failed(nvenc->nvEncInitializeEncoder(encoder, &init_params_storage.params))) {
      BOOST_LOG(error) << "NvEnc: NvEncInitializeEncoder() failed: " << last_nvenc_error_string;
      if (native_structs) {
        BOOST_LOG(error) << "[VIPLE-NVENC-SPLIT] NvEncInitializeEncoder() rejected split-frame: requested="
                         << split_encode_name(config.split_encode) << " mode=" << split_mode
                         << " api=" << api_version_string(minimum_api_version)
                         << " codec=" << codec_name(client_config.videoFormat)
                         << " size=" << encoder_params.width << 'x' << encoder_params.height
                         << " error=" << last_nvenc_error_string;
      }
      return false;
    }

    if (native_structs) {
#if NVENCAPI_MAJOR_VERSION >= 13
      BOOST_LOG(info) << "[VIPLE-NVENC-SPLIT] requested=" << split_encode_name(config.split_encode)
                      << " applied=" << split_mode
                      << " api=" << api_version_string(minimum_api_version)
                      << " codec=" << codec_name(client_config.videoFormat)
                      << " size=" << encoder_params.width << 'x' << encoder_params.height
                      << " engines=" << get_encoder_cap(NV_ENC_CAPS_NUM_ENCODER_ENGINES);
#endif
    }

    // VipleStream §ABR: deep-copy init params for mid-stream reconfigure
    stored_enc_config = enc_config;
    stored_init_params = init_params;
    stored_init_params.encodeConfig = &stored_enc_config;
    pin_level_for_reconfigure(client_config.videoFormat);
    // §ABR-ADAPT：0=H.264、1=HEVC、2=AV1；第二個參數＝這個編碼器初始化用的碼率（10 秒行的 enc=）
    abr_signals::reset(client_config.videoFormat, static_cast<int>(stored_enc_config.rcParams.averageBitRate / 1000));

    if (async_event_handle) {
      NV_ENC_EVENT_PARAMS event_params = {min_struct_version(NV_ENC_EVENT_PARAMS_VER, sdk12_ver::event_params)};
      event_params.completionEvent = async_event_handle;
      if (nvenc_failed(nvenc->nvEncRegisterAsyncEvent(encoder, &event_params))) {
        BOOST_LOG(error) << "NvEnc: NvEncRegisterAsyncEvent() failed: " << last_nvenc_error_string;
        return false;
      }
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER create_bitstream_buffer = {min_struct_version(NV_ENC_CREATE_BITSTREAM_BUFFER_VER, sdk12_ver::create_bitstream_buffer)};
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
      BOOST_LOG(debug) << "NvEnc: requested encoded frame size " << f % (client_config.bitrate / 8. / std::max(1, client_config.framerate)) << " kB";
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
      if (native_structs) {
        extra += std::format(" split-frame={}", split_mode);  // §NVENC-SPLIT
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
      NV_ENC_EVENT_PARAMS event_params = {min_struct_version(NV_ENC_EVENT_PARAMS_VER, sdk12_ver::event_params)};
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
    stored_init_params = {};
    stored_enc_config = {};
    custom_vbv_buffer = false;
    vbv_percentage_increase = 0;
    level_pinned = false;
    pinned_video_format = -1;
    unpinned_level = 0;
    unpinned_tier = 0;
    // §NVENC-SPLIT：上面的收尾呼叫要用這個 session 自己的結構版本，所以最後才清
    native_structs = false;
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

    NV_ENC_MAP_INPUT_RESOURCE mapped_input_buffer = {min_struct_version(NV_ENC_MAP_INPUT_RESOURCE_VER, sdk12_ver::map_input_resource)};
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

    NV_ENC_PIC_PARAMS pic_params = {min_struct_version(NV_ENC_PIC_PARAMS_VER, sdk12_ver::pic_params, 4, 6)};
    if (!qp_delta_map.empty()) {  // §VR-FOVEA-QP
      pic_params.qpDeltaMap = qp_delta_map.data();
      pic_params.qpDeltaMapSize = (uint32_t) qp_delta_map.size();
    }
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

    NV_ENC_LOCK_BITSTREAM lock_bitstream = {min_struct_version(NV_ENC_LOCK_BITSTREAM_VER, sdk12_ver::lock_bitstream, 1, 2)};
    lock_bitstream.outputBitstream = output_bitstream;
    lock_bitstream.doNotWait = async_event_handle ? 1 : 0;

    auto _profBeforeWait = std::chrono::steady_clock::now();
    if (async_event_handle && !wait_for_async_event(100)) {
      // §REVIEW-1111：原本等 100 ms 沒等到就返回——這張圖其實還在編，返回時卻把它的輸入解除對應，呼叫端收到空結果
      // 就結束整場串流。GPU 短暫停頓（遊戲載入、驅動重設開始）一次就會中。先再等一段：等到了這一張只是晚到、
      // 串流繼續；真的等不到（驅動掛了）才照舊放棄。
      BOOST_LOG(warning) << "NvEnc: frame " << frame_index << " not encoded after 100 ms, still waiting";
      if (!wait_for_async_event(2000)) {
        BOOST_LOG(error) << "NvEnc: frame " << frame_index << " encode wait timeout";
        return {};
      }
    }
    auto _profAfterWait = std::chrono::steady_clock::now();

    if (nvenc_failed(nvenc->nvEncLockBitstream(encoder, &lock_bitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncLockBitstream() failed: " << last_nvenc_error_string;
      return {};
    }
    auto _profAfterLock = std::chrono::steady_clock::now();

    // RAII 保證：在 bitstream lock 成功後，無論後續拷貝或狀態更新是否發生異常，均能可靠解鎖輸出緩衝區
    auto unlock_guard = util::fail_guard([&] {
      if (nvenc_failed(nvenc->nvEncUnlockBitstream(encoder, lock_bitstream.outputBitstream))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnlockBitstream() failed: " << last_nvenc_error_string;
      }
    });

    auto data_pointer = (uint8_t *) lock_bitstream.bitstreamBufferPtr;
    if (!data_pointer || lock_bitstream.bitstreamSizeInBytes == 0) {
      BOOST_LOG(error) << "NvEnc: locked bitstream buffer is null or empty";
      return {};
    }

    // §ABR-ADAPT：畫質飽和判斷用的平均 QP（NVENC 每幀都有，不另外花成本）
    // §VR-FOVEA-QP：周邊那塊刻意加粗的量不算畫質變差——扣掉差分圖的平均值，ABR 看到的才是注視點那塊的 QP 水準
    abr_signals::note_frame_qp(static_cast<int>(lock_bitstream.frameAvgQP) - (encoder_params.qp_map_avg_x100 + 50) / 100,
                               lock_bitstream.pictureType == NV_ENC_PIC_TYPE_IDR);

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

    // 拷貝完成後主動解鎖
    unlock_guard.disable();
    if (nvenc_failed(nvenc->nvEncUnlockBitstream(encoder, lock_bitstream.outputBitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncUnlockBitstream() failed: " << last_nvenc_error_string;
    }

    encoder_state.frame_size_logger.collect_and_log(encoded_frame.data.size() / 1000.);

    // [VIPLE-NVENC-PROF] tracker windows.  total = entry → after-unlock; the
    // sub-deltas isolate where the time goes (sync+map+encode_picture, then
    // wait_for_async_event, then lock+copy+unlock).
    // §LOGDROP-1127：四項統計合併成 30 秒 1 行（原本 4 行／5 秒），欄位名稱與順序不變。
    auto _profExit = std::chrono::steady_clock::now();
    using ms_d = std::chrono::duration<double, std::milli>;
    {
      auto &es = encoder_state;
      es.prof_total.add(std::chrono::duration_cast<ms_d>(_profExit - _profEntry).count());
      es.prof_async.add(std::chrono::duration_cast<ms_d>(_profAfterWait - _profBeforeWait).count());
      es.prof_lock.add(std::chrono::duration_cast<ms_d>(_profAfterLock - _profAfterWait).count());
      es.prof_bsize.add(static_cast<double>(encoded_frame.data.size()));
      ++es.prof_calls;
      // §LOGDROP-1127：只有 info 可見時才輸出（debug 場次不需要這些統計）
      const auto window = _profExit - es.prof_window_start;
      if (config::sunshine.min_log_level <= 2 && window >= std::chrono::seconds(30)) {
        const double secs = std::max(0.001, std::chrono::duration<double>(window).count());
        const auto f = [](const auto &s) {
          return std::format("{:.2f}/{:.2f}/{:.2f}", s.min_v, s.max_v, s.avg());
        };
        BOOST_LOG(info) << "[VIPLE-NVENC] 30s: calls=" << es.prof_calls
                        << " (" << std::format("{:.1f}", es.prof_calls / secs) << "/s)"
                        << " encode_frame total=" << f(es.prof_total) << "ms"
                        << " wait_async_evt=" << f(es.prof_async) << "ms"
                        << " lock_bitstream=" << f(es.prof_lock) << "ms"
                        << " bitstream_size=" << f(es.prof_bsize) << "B";
        es.prof_window_start = _profExit;
        es.prof_calls = 0;
        es.prof_total.reset();
        es.prof_async.reset();
        es.prof_lock.reset();
        es.prof_bsize.reset();
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
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload = {min_struct_version(NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER, sdk12_ver::sequence_param_payload)};
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
        if (zeros >= 2 && header[k] <= 1) {
          // 遇到下一個 NAL unit 的 start code 前綴 (00 00 01 或 00 00 00)，終止抽取當前 NAL
          break;
        }
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
    else if (pinned_video_format == 0) {
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

    bool reconfigured = send_reconfigure(force_idr, force_idr);
    if (!reconfigured && level_pinned) {
      // §SF-PARAMSETS：固定的 level／tier 被拒時退回自動選擇重試一次，ABR 不因固定而失效
      BOOST_LOG(warning) << "[VIPLE-ABR] NvEnc: reconfigure rejected with pinned level/tier ("
                         << last_nvenc_error_string << "), retrying with auto-selected level";
      unpin_level_for_reconfigure();
      reconfigured = send_reconfigure(force_idr, force_idr);
    }
    if (!reconfigured) {
      BOOST_LOG(error) << "[VIPLE-ABR] NvEnc: NvEncReconfigureEncoder() failed: " << last_nvenc_error_string;
      stored_enc_config.rcParams.averageBitRate = old_avg;
      stored_enc_config.rcParams.vbvBufferSize = old_vbv;
      stored_enc_config.rcParams.vbvInitialDelay = old_vbv;
      return false;
    }

    abr_signals::enc_kbps.store(bitrate_kbps, std::memory_order_relaxed);
    // 平滑過渡每一步都成功是常態（VR 回升時每秒數次），留 debug：有沒有進編碼器看 [VIPLE-VR-ABR-FAST] 10s 的 enc=。
    // reset+IDR 會讓畫面頓一下，照舊 info
    if (force_idr) {
      BOOST_LOG(info) << "[VIPLE-ABR] NvEnc: bitrate reconfigured to " << bitrate_kbps << " kbps"
                      << " (vbv=" << stored_enc_config.rcParams.vbvBufferSize << ", reset+IDR)";
    } else {
      BOOST_LOG(debug) << "[VIPLE-ABR] NvEnc: bitrate reconfigured to " << bitrate_kbps << " kbps"
                       << " (vbv=" << stored_enc_config.rcParams.vbvBufferSize << ", smooth)";
    }
    return true;
  }

  // §REVIEW-1118B：中途改幀率。VBV 用建立時同一條式子（現在的 averageBitRate／fps，再加上當時的百分比），
  // 不 reset、不強制 IDR。失敗就把本機參數放回去，呼叫端不會去改擷取節拍。
  bool nvenc_base::reconfigure_framerate(int framerate) {
    if (!encoder || !nvenc) {
      BOOST_LOG(error) << "[VIPLE-FPS] NvEnc: framerate reconfigure called on uninitialized encoder";
      return false;
    }
    if (framerate <= 0) {
      return false;
    }
    // framerateX100 建立時分母不是 1。這個封包只有整數 fps，改掉會把 59.94 收成整數。呼叫端也會擋。
    if (stored_init_params.frameRateDen != 1) {
      BOOST_LOG(info) << "[VIPLE-FPS] NvEnc: keeping fractional frame rate "
                      << stored_init_params.frameRateNum << "/" << stored_init_params.frameRateDen;
      return false;
    }
    if (stored_init_params.frameRateNum == static_cast<uint32_t>(framerate)) {
      return true;
    }

    const uint32_t old_num = stored_init_params.frameRateNum;
    const uint32_t old_den = stored_init_params.frameRateDen;
    const uint32_t old_vbv = stored_enc_config.rcParams.vbvBufferSize;
    const uint32_t old_delay = stored_enc_config.rcParams.vbvInitialDelay;

    stored_init_params.frameRateNum = static_cast<uint32_t>(framerate);
    stored_init_params.frameRateDen = 1;
    if (custom_vbv_buffer) {
      const int safe_fps = std::max(1, framerate);
      uint32_t vbv = static_cast<uint32_t>(static_cast<uint64_t>(stored_enc_config.rcParams.averageBitRate) / static_cast<uint32_t>(safe_fps));
      if (vbv_percentage_increase > 0) {
        vbv += static_cast<uint32_t>(static_cast<uint64_t>(vbv) * static_cast<uint32_t>(vbv_percentage_increase) / 100);
      }
      stored_enc_config.rcParams.vbvBufferSize = vbv;
      stored_enc_config.rcParams.vbvInitialDelay = vbv;
    }

    // 標頭的不支援清單沒有 frame rate。resetEncoder 是清碼率控制狀態，註解寫要配 IDR。
    // 封包處理端已經 raise idr，這裡再 forceIDR 會連送兩張。
    // 不在這裡 unpin level 再重試。bitrate 那條失敗會把釘住的 level 拆掉再送一次；
    // 幀率若是因為別的原因被拒，拆掉 level 會讓下一筆 ABR 重設改到參數集。失敗就整筆放回去。
    if (!send_reconfigure(false, false)) {
      BOOST_LOG(error) << "[VIPLE-FPS] NvEnc: NvEncReconfigureEncoder() failed: " << last_nvenc_error_string;
      stored_init_params.frameRateNum = old_num;
      stored_init_params.frameRateDen = old_den;
      stored_enc_config.rcParams.vbvBufferSize = old_vbv;
      stored_enc_config.rcParams.vbvInitialDelay = old_delay;
      return false;
    }

    BOOST_LOG(info) << "[VIPLE-FPS] NvEnc: frame rate reconfigured to " << framerate
                    << " fps (vbv=" << stored_enc_config.rcParams.vbvBufferSize << ", no reset)";
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

    if (status != NV_ENC_SUCCESS) {
      /* This API function gives broken strings more often than not
      if (nvenc && encoder) {
        last_nvenc_error_string = nvenc->nvEncGetLastErrorString(encoder);
        if (!last_nvenc_error_string.empty()) last_nvenc_error_string += " ";
      }
      */
      last_nvenc_error_string = status_string(status);
      return true;
    }

    return false;
  }

  bool nvenc_base::send_reconfigure(bool reset_encoder, bool force_idr) {
#if NVENCAPI_MAJOR_VERSION >= 13
    if (!native_structs) {
      // 檔頭說明 3：12.0 的 NV_ENC_RECONFIGURE_PARAMS 版面（version、補齊、1808 bytes 的 INITIALIZE_PARAMS、
      // resetEncoder:1／forceIDR:1 的字在 1816、結尾補齊），與 12.0 標頭時 `= {version}` 再逐欄填的位元組相同
      alignas(NV_ENC_RECONFIGURE_PARAMS) uint8_t raw[kSdk12ReconfigureParamsSize] = {};
      const uint32_t version = min_struct_version(NV_ENC_RECONFIGURE_PARAMS_VER, sdk12_ver::reconfigure_params);
      std::memcpy(raw, &version, sizeof(version));
      // 13.0 的 INITIALIZE_PARAMS 少 8 bytes，12.0 多出來的尾端（reserved2）留 0
      std::memcpy(raw + kSdk12ReconfigureInitOffset, &stored_init_params, sizeof(NV_ENC_INITIALIZE_PARAMS));
      // GCC（Windows MinGW 與 Linux 都是）的位元欄位從最低位元排：resetEncoder 是 bit 0、forceIDR 是 bit 1
      const uint32_t flags = (reset_encoder ? 1u : 0u) | (force_idr ? 2u : 0u);
      std::memcpy(raw + kSdk12ReconfigureFlagsOffset, &flags, sizeof(flags));
      return !nvenc_failed(nvenc->nvEncReconfigureEncoder(encoder, reinterpret_cast<NV_ENC_RECONFIGURE_PARAMS *>(raw)));
    }
#endif
    NV_ENC_RECONFIGURE_PARAMS reconfigure_params = {min_struct_version(NV_ENC_RECONFIGURE_PARAMS_VER, sdk12_ver::reconfigure_params)};
    reconfigure_params.reInitEncodeParams = stored_init_params;
    reconfigure_params.resetEncoder = reset_encoder ? 1 : 0;
    reconfigure_params.forceIDR = force_idr ? 1 : 0;
    return !nvenc_failed(nvenc->nvEncReconfigureEncoder(encoder, &reconfigure_params));
  }

  uint32_t nvenc_base::min_struct_version(uint32_t version, uint32_t sdk12_version, uint32_t v11_struct_version, uint32_t v12_struct_version) {
    assert(minimum_api_version);

    // §NVENC-SPLIT：原生版本的 session（13.0 結構）直接用標頭的版本，不套 v11／v12 覆寫
    if (native_structs) {
      return version;
    }

    // 12.0 標頭的結構版本配上這個 codec 需要的最低 API 版本。和原本「從標頭版本遮掉 NVENCAPI_VERSION 再填
    // minimum_api_version」在 12.0 標頭時結果相同（sdk12_ver 的表在 12.0 標頭時有 static_assert 核對）。
    version = sdk12_version | minimum_api_version;

    // If there's a struct version override, apply that too
    if (v11_struct_version || v12_struct_version) {
      version &= ~(0xFFu << 16);
      version |= (((minimum_api_version & 0xFF) >= 12) ? v12_struct_version : v11_struct_version) << 16;
    }

    return version;
  }
}  // namespace nvenc
