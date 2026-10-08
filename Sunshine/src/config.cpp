/**
 * @file src/config.cpp
 * @brief Definitions for the configuration of VipleStream-Server (upstream: Sunshine).
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

// local includes
#include "config.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "logging.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

#ifdef _WIN32
  #include <shellapi.h>

  #include "platform/windows/config_acl.h"
#endif

#if !defined(__ANDROID__) && !defined(__APPLE__)
  // For NVENC legacy constants
  #include <ffnvcodec/nvEncodeAPI.h>
#endif

namespace fs = std::filesystem;
using namespace std::literals;

constexpr auto CA_DIR = "credentials";
const std::string PRIVATE_KEY_FILE = std::string(CA_DIR) + "/cakey.pem";
const std::string CERTIFICATE_FILE = std::string(CA_DIR) + "/cacert.pem";
const std::string APPS_JSON_PATH = platf::appdata().string() + "/apps.json";

namespace config {

  // VipleStream §CFG.defer: 設定解析期間（logging::init() 之前）的警告一律走這裡。
  // 照舊丟一份給 console（pre-init 只有 Boost 的隱含 sink），同時複製一份到
  // deferred_config_warnings，讓 main.cpp 在 log sink 準備好之後補寫進 sunshine.log。
  //
  // 雙寫是刻意的：config::parse() 回傳非 0 時 logging::init() 永遠不會被呼叫，
  // 緩衝內容會隨行程結束消失，那時 console 那一份是唯一的輸出管道。
  // 所以請勿把這裡的 BOOST_LOG 拿掉改成純緩衝。
  void warn_config(std::string message) {
    BOOST_LOG(warning) << message;
    deferred_config_warnings.push_back(std::move(message));
  }

  // VipleStream §S1-02（K25）：config dump 的兩個輸出點（apply_config 的逐鍵 log、main.cpp 的重印）
  // 都經過 loggable_value，relay PSK 之類的值不再以明文進 stdout、sunshine.log、viplestream-svc.log。
  // 比對不分大小寫：鍵名拼錯大小寫時雖然不會被套用，但仍會出現在 dump 裡，照樣要遮。
  bool is_secret_key(std::string_view name) {
    std::string key;
    key.reserve(name.size());
    for (const char ch : name) {
      key.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
    }

    if (key == "relay_psk"sv || key == "relay_url"sv) {
      return true;
    }
    for (const auto suffix : {"_psk"sv, "_password"sv, "_secret"sv, "_token"sv, "_key"sv}) {
      if (key.ends_with(suffix)) {
        return true;
      }
    }
    return false;
  }

  namespace {
    // loggable_value 自己產生的佔位字串：`<redacted len=` + 1～10 位數字 + `>`
    bool is_redacted_placeholder(std::string_view value) {
      constexpr auto prefix = "<redacted len="sv;
      if (!value.starts_with(prefix) || !value.ends_with('>')) {
        return false;
      }
      const auto digits = value.substr(prefix.size(), value.size() - prefix.size() - 1);
      if (digits.empty() || digits.size() > 10) {
        return false;
      }
      return std::all_of(digits.begin(), digits.end(), [](char ch) {
        return ch >= '0' && ch <= '9';
      });
    }
  }  // namespace

  std::string loggable_value(std::string_view name, std::string_view value) {
    if (!is_secret_key(name) || is_redacted_placeholder(value)) {
      return std::string {value};
    }
    return std::format("<redacted len={}>", value.size());
  }

  namespace nv {

    nvenc::nvenc_two_pass twopass_from_view(const std::string_view &preset) {
      if (preset == "disabled") {
        return nvenc::nvenc_two_pass::disabled;
      }
      if (preset == "quarter_res") {
        return nvenc::nvenc_two_pass::quarter_resolution;
      }
      if (preset == "full_res") {
        return nvenc::nvenc_two_pass::full_resolution;
      }
      warn_config(std::format("config: unknown nvenc_twopass value: '{}' -- accepted values: disabled, quarter_res, full_res. Falling back to 'quarter_res'", preset));
      return nvenc::nvenc_two_pass::quarter_resolution;
    }

  }  // namespace nv

  // VipleStream 2.0 §VR：vr_* 設定項的轉換器。不能叫 namespace vr——config::vr 是設定變數。
  namespace vr_opt {

    vr_t::pcvr_e pcvr_from_view(const std::string_view value, const std::string_view key) {
      if (value == "disabled"sv) {
        return vr_t::pcvr_e::disabled;
      }
      if (value == "stub"sv) {
        return vr_t::pcvr_e::stub;
      }
      if (value == "enabled"sv) {
        return vr_t::pcvr_e::enabled;
      }
      warn_config(std::format(
        "config: invalid value for '{}': '{}' -- accepted values: disabled, stub, enabled. "
        "Falling back to 'disabled' (VR sessions are rejected with VR_DISABLED)",
        key,
        value
      ));
      return vr_t::pcvr_e::disabled;
    }

    vr_t::latch_e latch_from_view(const std::string_view value, const std::string_view key) {
      if (value == "legacy"sv) {
        return vr_t::latch_e::legacy;
      }
      if (value == "v2"sv) {
        return vr_t::latch_e::v2;
      }
      warn_config(std::format("config: invalid value for '{}': '{}' -- accepted values: legacy, v2. Falling back to 'legacy'", key, value));
      return vr_t::latch_e::legacy;
    }

    vr_t::multilink_e multilink_from_view(const std::string_view value, const std::string_view key) {
      if (value == "disabled"sv) {
        return vr_t::multilink_e::disabled;
      }
      if (value == "all"sv || value == "enabled"sv) {
        return vr_t::multilink_e::all;
      }
      if (value == "primary"sv) {
        return vr_t::multilink_e::primary;
      }
      if (value == "auto"sv) {
        return vr_t::multilink_e::automatic;
      }
      if (value == "always"sv) {
        return vr_t::multilink_e::always;
      }
      warn_config(std::format("config: invalid value for '{}': '{}' -- accepted values: disabled, auto, all, always, primary. Falling back to 'disabled'", key, value));
      return vr_t::multilink_e::disabled;
    }

    vr_t::stale_e stale_from_view(const std::string_view value, const std::string_view key) {
      if (value == "legacy"sv) {
        return vr_t::stale_e::legacy;
      }
      if (value == "hold"sv) {
        return vr_t::stale_e::hold;
      }
      warn_config(std::format("config: invalid value for '{}': '{}' -- accepted values: legacy, hold. Falling back to 'legacy'", key, value));
      return vr_t::stale_e::legacy;
    }

    /**
     * @brief 嚴格解析 vr_* 的整數設定項：不是十進位整數、或不在允許範圍就警告並保留預設值。
     *
     * 不用 int_between_f：它底下的 util::from_view 遇到垃圾字元會無聲回 0，而
     * vr_intra_refresh_safety_ms 的 0 代表「關閉安全網」，打錯字會靜靜關掉功能。
     */
    void int_strict_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, int lo, int hi, bool allow_zero) {
      auto it = vars.find(name);
      if (it == std::end(vars)) {
        return;
      }

      std::string_view val = it->second;
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
      }

      int parsed = 0;
      auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), parsed, 10);
      const bool is_int = !val.empty() && val.front() != '+' && ec == std::errc {} && ptr == val.data() + val.size();
      if (is_int && ((allow_zero && parsed == 0) || (parsed >= lo && parsed <= hi))) {
        input = parsed;
      } else {
        warn_config(std::format(
          "config: invalid value for '{}': '{}' -- accepted values: {}{}..{}. Keeping '{}'",
          name,
          it->second,
          allow_zero ? "0 or " : "",
          lo,
          hi,
          input
        ));
      }

      vars.erase(it);
    }

    /**
     * @brief 嚴格解析 vr_* 的布林設定項：只接受明確的開／關字樣（不分大小寫），其他一律警告並保留原值。
     *
     * 不用 bool_f：它底下的 to_bool 把不認得的值當成關閉、而且值裡只要有 '1' 就當成開啟（"disabled1"、"10" 都是開），
     * 都不警告。這四個選項寫錯的後果是整個功能靜靜地沒開，驗測時很難發現。
     */
    void bool_strict_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, bool &input) {
      auto it = vars.find(name);
      if (it == std::end(vars)) {
        return;
      }

      std::string_view raw = it->second;
      if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw = raw.substr(1, raw.size() - 2);
      }
      std::string val {raw};
      std::transform(val.begin(), val.end(), val.begin(), [](unsigned char ch) {
        return (char) std::tolower(ch);
      });

      if (val == "enabled"sv || val == "enable"sv || val == "true"sv || val == "yes"sv || val == "on"sv || val == "1"sv) {
        input = true;
      } else if (val == "disabled"sv || val == "disable"sv || val == "false"sv || val == "no"sv || val == "off"sv || val == "0"sv) {
        input = false;
      } else {
        warn_config(std::format(
          "config: invalid value for '{}': '{}' -- accepted values: enabled, disabled (also true/false, yes/no, on/off, 1/0). Keeping '{}'",
          name,
          it->second,
          input ? "enabled" : "disabled"
        ));
      }

      vars.erase(it);
    }

  }  // namespace vr_opt

  namespace amd {
#if !defined(_WIN32) || defined(DOXYGEN)
    // values accurate as of 27/12/2022, but aren't strictly necessary for MacOS build
    constexpr int AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED = 100;
    constexpr int AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY = 30;
    constexpr int AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED = 70;
    constexpr int AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED = 10;
    constexpr int AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY = 0;
    constexpr int AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED = 5;
    constexpr int AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED = 1;
    constexpr int AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY = 2;
    constexpr int AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED = 0;
    constexpr int AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP = 0;
    constexpr int AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR = 3;
    constexpr int AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR = 2;
    constexpr int AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR = 1;
    constexpr int AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP = 0;
    constexpr int AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR = 3;
    constexpr int AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR = 2;
    constexpr int AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR = 1;
    constexpr int AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP = 0;
    constexpr int AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR = 1;
    constexpr int AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR = 2;
    constexpr int AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR = 3;
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING = 0;
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY = 1;
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY = 2;
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_WEBCAM = 3;
    constexpr int AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY_HIGH_QUALITY = 5;
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING = 0;
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY = 1;
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY = 2;
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_WEBCAM = 3;
    constexpr int AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY = 5;
    constexpr int AMF_VIDEO_ENCODER_USAGE_TRANSCODING = 0;
    constexpr int AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY = 1;
    constexpr int AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY = 2;
    constexpr int AMF_VIDEO_ENCODER_USAGE_WEBCAM = 3;
    constexpr int AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY = 5;
    constexpr int AMF_VIDEO_ENCODER_UNDEFINED = 0;
    constexpr int AMF_VIDEO_ENCODER_CABAC = 1;
    constexpr int AMF_VIDEO_ENCODER_CALV = 2;
#else
  #ifdef _GLIBCXX_USE_C99_INTTYPES
    #undef _GLIBCXX_USE_C99_INTTYPES
  #endif
  #include <AMF/components/VideoEncoderAV1.h>
  #include <AMF/components/VideoEncoderHEVC.h>
  #include <AMF/components/VideoEncoderVCE.h>
#endif

    enum class quality_av1_e : int {
      speed = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class quality_hevc_e : int {
      speed = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class quality_h264_e : int {
      speed = AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED,  ///< Speed preset
      quality = AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY,  ///< Quality preset
      balanced = AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED  ///< Balanced preset
    };

    enum class rc_av1_e : int {
      cbr = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    enum class rc_hevc_e : int {
      cbr = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    enum class rc_h264_e : int {
      cbr = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR,  ///< CBR
      cqp = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP,  ///< CQP
      vbr_latency = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,  ///< VBR with latency constraints
      vbr_peak = AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR  ///< VBR with peak constraints
    };

    enum class usage_av1_e : int {
      transcoding = AMF_VIDEO_ENCODER_AV1_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_AV1_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum class usage_hevc_e : int {
      transcoding = AMF_VIDEO_ENCODER_HEVC_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_HEVC_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum class usage_h264_e : int {
      transcoding = AMF_VIDEO_ENCODER_USAGE_TRANSCODING,  ///< Transcoding preset
      webcam = AMF_VIDEO_ENCODER_USAGE_WEBCAM,  ///< Webcam preset
      lowlatency_high_quality = AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY_HIGH_QUALITY,  ///< Low latency high quality preset
      lowlatency = AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY,  ///< Low latency preset
      ultralowlatency = AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY  ///< Ultra low latency preset
    };

    enum coder_e : int {
      _auto = AMF_VIDEO_ENCODER_UNDEFINED,  ///< Auto
      cabac = AMF_VIDEO_ENCODER_CABAC,  ///< CABAC
      cavlc = AMF_VIDEO_ENCODER_CALV  ///< CAVLC
    };

    template<class T>
    ::std::optional<int> quality_from_view(const ::std::string_view &quality_type, const ::std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (quality_type == #x##sv) \
  return (int) T::x
      _CONVERT_(balanced);
      _CONVERT_(quality);
      _CONVERT_(speed);
#undef _CONVERT_
      return original;
    }

    template<class T>
    ::std::optional<int> rc_from_view(const ::std::string_view &rc, const ::std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (rc == #x##sv) \
  return (int) T::x
      _CONVERT_(cbr);
      _CONVERT_(cqp);
      _CONVERT_(vbr_latency);
      _CONVERT_(vbr_peak);
#undef _CONVERT_
      return original;
    }

    template<class T>
    ::std::optional<int> usage_from_view(const ::std::string_view &usage, const ::std::optional<int>(&original)) {
#define _CONVERT_(x) \
  if (usage == #x##sv) \
  return (int) T::x
      _CONVERT_(lowlatency);
      _CONVERT_(lowlatency_high_quality);
      _CONVERT_(transcoding);
      _CONVERT_(ultralowlatency);
      _CONVERT_(webcam);
#undef _CONVERT_
      return original;
    }

    int coder_from_view(const ::std::string_view &coder) {
      if (coder == "auto"sv) {
        return _auto;
      }
      if (coder == "cabac"sv || coder == "ac"sv) {
        return cabac;
      }
      if (coder == "cavlc"sv || coder == "vlc"sv) {
        return cavlc;
      }

      return _auto;
    }
  }  // namespace amd

  namespace qsv {
    enum preset_e : int {
      veryslow = 1,  ///< veryslow preset
      slower = 2,  ///< slower preset
      slow = 3,  ///< slow preset
      medium = 4,  ///< medium preset
      fast = 5,  ///< fast preset
      faster = 6,  ///< faster preset
      veryfast = 7  ///< veryfast preset
    };

    enum cavlc_e : int {
      _auto = false,  ///< Auto
      enabled = true,  ///< Enabled
      disabled = false  ///< Disabled
    };

    std::optional<int> preset_from_view(const std::string_view &preset) {
#define _CONVERT_(x) \
  if (preset == #x##sv) \
  return x
      _CONVERT_(veryslow);
      _CONVERT_(slower);
      _CONVERT_(slow);
      _CONVERT_(medium);
      _CONVERT_(fast);
      _CONVERT_(faster);
      _CONVERT_(veryfast);
#undef _CONVERT_
      return std::nullopt;
    }

    std::optional<int> coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) {
        return _auto;
      }
      if (coder == "cabac"sv || coder == "ac"sv) {
        return disabled;
      }
      if (coder == "cavlc"sv || coder == "vlc"sv) {
        return enabled;
      }
      return std::nullopt;
    }

  }  // namespace qsv

  namespace vt {

    enum coder_e : int {
      _auto = 0,  ///< Auto
      cabac,  ///< CABAC
      cavlc  ///< CAVLC
    };

    int coder_from_view(const std::string_view &coder) {
      if (coder == "auto"sv) {
        return _auto;
      }
      if (coder == "cabac"sv || coder == "ac"sv) {
        return cabac;
      }
      if (coder == "cavlc"sv || coder == "vlc"sv) {
        return cavlc;
      }

      return -1;
    }

    int allow_software_from_view(const std::string_view &software) {
      if (software == "allowed"sv || software == "forced") {
        return 1;
      }

      return 0;
    }

    int force_software_from_view(const std::string_view &software) {
      if (software == "forced") {
        return 1;
      }

      return 0;
    }

    int rt_from_view(const std::string_view &rt) {
      if (rt == "disabled" || rt == "off" || rt == "0") {
        return 0;
      }

      return 1;
    }

  }  // namespace vt

  namespace sw {
    int svtav1_preset_from_view(const std::string_view &preset) {
#define _CONVERT_(x, y) \
  if (preset == #x##sv) \
  return y
      _CONVERT_(veryslow, 1);
      _CONVERT_(slower, 2);
      _CONVERT_(slow, 4);
      _CONVERT_(medium, 5);
      _CONVERT_(fast, 7);
      _CONVERT_(faster, 9);
      _CONVERT_(veryfast, 10);
      _CONVERT_(superfast, 11);
      _CONVERT_(ultrafast, 12);
#undef _CONVERT_
      return 11;  // Default to superfast
    }
  }  // namespace sw

  namespace dd {
    video_t::dd_t::config_option_e config_option_from_view(const std::string_view value, const std::string_view key) {
#define _CONVERT_(x) \
  if (value == #x##sv) \
  return video_t::dd_t::config_option_e::x
      _CONVERT_(disabled);
      _CONVERT_(verify_only);
      _CONVERT_(ensure_active);
      _CONVERT_(ensure_primary);
      _CONVERT_(ensure_only_display);
#undef _CONVERT_
      // VipleStream §CFG.defer: 上游在這裡無聲退回 disabled。訊息要同時講出：
      // 哪個 key、什麼壞值、可接受的 token、以及實際生效的結果。
      warn_config(std::format(
        "config: invalid value for '{}': '{}' -- accepted values: disabled, verify_only, "
        "ensure_active, ensure_primary, ensure_only_display. Falling back to 'disabled' "
        "(which is also the built-in default, so display-device handling stays off)",
        key,
        value
      ));
      return video_t::dd_t::config_option_e::disabled;  // Default to this if value is invalid
    }

    video_t::dd_t::resolution_option_e resolution_option_from_view(const std::string_view value, const std::string_view key) {
#define _CONVERT_2_ARG_(str, val) \
  if (value == #str##sv) \
  return video_t::dd_t::resolution_option_e::val
#define _CONVERT_(x) _CONVERT_2_ARG_(x, x)
      _CONVERT_(disabled);
      _CONVERT_2_ARG_(auto, automatic);
      _CONVERT_(manual);
#undef _CONVERT_
#undef _CONVERT_2_ARG_
      // VipleStream §CFG.defer: 上游在這裡無聲退回 disabled。訊息要同時講出：
      // 哪個 key、什麼壞值、可接受的 token、以及實際生效的結果。
      // 特別注意：這個設定項的內建預設值是 automatic，而設定檔認得的 token 是 "auto" ——
      // 使用者寫下看起來很合理的 "automatic" 就會把功能靜靜關掉，比整行不寫還糟。
      warn_config(std::format(
        "config: invalid value for '{}': '{}' -- accepted values: disabled, auto, manual. "
        "Falling back to 'disabled' (the built-in default is 'auto', so this turns the feature OFF)",
        key,
        value
      ));
      return video_t::dd_t::resolution_option_e::disabled;  // Default to this if value is invalid
    }

    video_t::dd_t::refresh_rate_option_e refresh_rate_option_from_view(const std::string_view value, const std::string_view key) {
#define _CONVERT_2_ARG_(str, val) \
  if (value == #str##sv) \
  return video_t::dd_t::refresh_rate_option_e::val
#define _CONVERT_(x) _CONVERT_2_ARG_(x, x)
      _CONVERT_(disabled);
      _CONVERT_2_ARG_(auto, automatic);
      _CONVERT_(manual);
#undef _CONVERT_
#undef _CONVERT_2_ARG_
      // VipleStream §CFG.defer: 上游在這裡無聲退回 disabled。訊息要同時講出：
      // 哪個 key、什麼壞值、可接受的 token、以及實際生效的結果。
      // 特別注意：這個設定項的內建預設值是 automatic，而設定檔認得的 token 是 "auto" ——
      // 使用者寫下看起來很合理的 "automatic" 就會把功能靜靜關掉，比整行不寫還糟。
      warn_config(std::format(
        "config: invalid value for '{}': '{}' -- accepted values: disabled, auto, manual. "
        "Falling back to 'disabled' (the built-in default is 'auto', so this turns the feature OFF)",
        key,
        value
      ));
      return video_t::dd_t::refresh_rate_option_e::disabled;  // Default to this if value is invalid
    }

    video_t::dd_t::hdr_option_e hdr_option_from_view(const std::string_view value, const std::string_view key) {
#define _CONVERT_2_ARG_(str, val) \
  if (value == #str##sv) \
  return video_t::dd_t::hdr_option_e::val
#define _CONVERT_(x) _CONVERT_2_ARG_(x, x)
      _CONVERT_(disabled);
      _CONVERT_2_ARG_(auto, automatic);
#undef _CONVERT_
#undef _CONVERT_2_ARG_
      // VipleStream §CFG.defer: 上游在這裡無聲退回 disabled。訊息要同時講出：
      // 哪個 key、什麼壞值、可接受的 token、以及實際生效的結果。
      warn_config(std::format(
        "config: invalid value for '{}': '{}' -- accepted values: disabled, auto. "
        "Falling back to 'disabled' (the built-in default is 'auto', so this turns the feature OFF)",
        key,
        value
      ));
      return video_t::dd_t::hdr_option_e::disabled;  // Default to this if value is invalid
    }

    video_t::dd_t::mode_remapping_t mode_remapping_from_view(const std::string_view value) {
      const auto parse_entry_list {[](const auto &entry_list, auto &output_field) {
        for (auto &[_, entry] : entry_list) {
          auto requested_resolution = entry.template get_optional<std::string>("requested_resolution"s);
          auto requested_fps = entry.template get_optional<std::string>("requested_fps"s);
          auto final_resolution = entry.template get_optional<std::string>("final_resolution"s);
          auto final_refresh_rate = entry.template get_optional<std::string>("final_refresh_rate"s);

          output_field.push_back(video_t::dd_t::mode_remapping_entry_t {requested_resolution.value_or(""), requested_fps.value_or(""), final_resolution.value_or(""), final_refresh_rate.value_or("")});
        }
      }};

      // We need to add a wrapping object to make it valid JSON, otherwise ptree cannot parse it.
      std::stringstream json_stream;
      json_stream << "{\"dd_mode_remapping\":" << value << "}";

      boost::property_tree::ptree json_tree;
      boost::property_tree::read_json(json_stream, json_tree);

      video_t::dd_t::mode_remapping_t output;
      parse_entry_list(json_tree.get_child("dd_mode_remapping.mixed"), output.mixed);
      parse_entry_list(json_tree.get_child("dd_mode_remapping.resolution_only"), output.resolution_only);
      parse_entry_list(json_tree.get_child("dd_mode_remapping.refresh_rate_only"), output.refresh_rate_only);

      return output;
    }
  }  // namespace dd

  video_t video {
    28,  // qp

    0,  // hevc_mode
    0,  // av1_mode

    4,  // min_threads [2→4: more parallel slices for software encoding, faster on modern CPUs]
    {
      "superfast"s,  // preset
      "zerolatency"s,  // tune
      11,  // superfast
    },  // software

    {},  // nv
    true,  // nv_realtime_hags
    true,  // nv_opengl_vulkan_on_dxgi
    true,  // nv_sunshine_high_power_mode
    {},  // nv_legacy

    {
      qsv::medium,  // preset
      qsv::_auto,  // cavlc
      false,  // slow_hevc
    },  // qsv

    {
      (int) amd::usage_h264_e::ultralowlatency,  // usage (h264)
      (int) amd::usage_hevc_e::ultralowlatency,  // usage (hevc)
      (int) amd::usage_av1_e::ultralowlatency,  // usage (av1)
      (int) amd::rc_h264_e::vbr_latency,  // rate control (h264)
      (int) amd::rc_hevc_e::vbr_latency,  // rate control (hevc)
      (int) amd::rc_av1_e::vbr_latency,  // rate control (av1)
      0,  // enforce_hrd
      (int) amd::quality_h264_e::quality,  // quality (h264) [balanced→quality: better in ultralowlatency mode]
      (int) amd::quality_hevc_e::quality,  // quality (hevc)
      (int) amd::quality_av1_e::quality,  // quality (av1)
      0,  // preanalysis
      1,  // vbaq
      (int) amd::coder_e::_auto,  // coder
    },  // amd

    {
      0,
      0,
      1,
      -1,
    },  // vt

    {
      false,  // strict_rc_buffer
    },  // vaapi

    {
      2,  // vk.tune (default: ll - low latency)
      4,  // vk.rc_mode (default: vbr)
    },

    {},  // capture
    {},  // encoder
    {},  // adapter_name
    {},  // output_name

    {
      video_t::dd_t::config_option_e::disabled,  // configuration_option
      video_t::dd_t::resolution_option_e::automatic,  // resolution_option
      {},  // manual_resolution
      video_t::dd_t::refresh_rate_option_e::automatic,  // refresh_rate_option
      {},  // manual_refresh_rate
      video_t::dd_t::hdr_option_e::automatic,  // hdr_option
      3s,  // config_revert_delay
      {},  // config_revert_on_disconnect
      {},  // mode_remapping
      {}  // wa
    },  // display_device

    0,  // max_bitrate
    0  // minimum_fps_target (0 = framerate)
  };

  audio_t audio {
    {},  // audio_sink
    {},  // virtual_sink
    true,  // stream audio
    true,  // install_steam_drivers
  };

  stream_t stream {
    10s,  // ping_timeout

    APPS_JSON_PATH,

    10,  // fecPercentage [20→10: less FEC overhead on LAN; user can raise for lossy networks]

    ENCRYPTION_MODE_NEVER,  // lan_encryption_mode
    ENCRYPTION_MODE_OPPORTUNISTIC,  // wan_encryption_mode
  };

  vr_t vr {};  // VipleStream 2.0 §VR：預設值見 config.h（vr_pcvr=disabled）

  nvhttp_t nvhttp {
    "lan",  // origin web manager

    PRIVATE_KEY_FILE,
    CERTIFICATE_FILE,

    platf::get_host_name(),  // sunshine_name,
    "sunshine_state.json"s,  // file_state
    {},  // external_ip
  };

  input_t input {
    {
      {0x10, 0xA0},
      {0x11, 0xA2},
      {0x12, 0xA4},
    },
    -1ms,  // back_button_timeout
    500ms,  // key_repeat_delay
    std::chrono::duration<double> {1 / 24.9},  // key_repeat_period

    {
      platf::supported_gamepads(nullptr).front().name.data(),
      platf::supported_gamepads(nullptr).front().name.size(),
    },  // Default gamepad
    true,  // back as touchpad click enabled (manual DS4 only)
    true,  // client gamepads with motion events are emulated as DS4
    true,  // client gamepads with touchpads are emulated as DS4
    true,  // ds5_inputtino_randomize_mac

    true,  // keyboard enabled
    true,  // mouse enabled
    true,  // controller enabled
    true,  // always send scancodes
    true,  // high resolution scrolling
    true,  // native pen/touch support
  };

  sunshine_t sunshine {
    "en",  // locale
    2,  // min_log_level
    0,  // flags
    {},  // User file
    {},  // Username
    {},  // Password
    {},  // Password Salt
    platf::appdata().string() + "/sunshine.conf",  // config file
    {},  // cmd args
    47989,  // Base port number
    "ipv4",  // Address family
    {},  // Bind address
    platf::appdata().string() + "/sunshine.log",  // log file
    false,  // notify_pre_releases
    true,  // system_tray
    {},  // prep commands
  };

  bool endline(char ch) {
    return ch == '\r' || ch == '\n';
  }

  bool space_tab(char ch) {
    return ch == ' ' || ch == '\t';
  }

  bool whitespace(char ch) {
    return space_tab(ch) || endline(ch);
  }

  template<class It>
  std::string to_string(It begin, It end) {
    std::string result;

    KITTY_WHILE_LOOP(auto pos = begin, pos != end, {
      auto comment = std::find(pos, end, '#');
      auto endl = std::find_if(comment, end, endline);

      result.append(pos, comment);

      pos = endl;
    })

    return result;
  }

  template<class It>
  It skip_list(It skipper, It end) {
    int stack = 1;
    while (skipper != end && stack) {
      if (*skipper == '[') {
        ++stack;
      }
      if (*skipper == ']') {
        --stack;
      }

      ++skipper;
    }

    return skipper;
  }

  std::pair<
    std::string_view::const_iterator,
    std::optional<std::pair<std::string, std::string>>>
    parse_option(std::string_view::const_iterator begin, std::string_view::const_iterator end) {
    begin = std::find_if_not(begin, end, whitespace);
    auto endl = std::find_if(begin, end, endline);
    auto endc = std::find(begin, endl, '#');
    endc = std::find_if(std::make_reverse_iterator(endc), std::make_reverse_iterator(begin), std::not_fn(whitespace)).base();

    auto eq = std::find(begin, endc, '=');
    if (eq == endc || eq == begin) {
      return std::make_pair(endl, std::nullopt);
    }

    auto end_name = std::find_if_not(std::make_reverse_iterator(eq), std::make_reverse_iterator(begin), space_tab).base();
    auto begin_val = std::find_if_not(eq + 1, endc, space_tab);

    if (begin_val == endl) {
      return std::make_pair(endl, std::nullopt);
    }

    // Lists might contain newlines
    if (*begin_val == '[') {
      endl = skip_list(begin_val + 1, end);

      // Check if we reached the end of the file without finding a closing bracket
      // We know we have a valid closing bracket if:
      // 1. We didn't reach the end, or
      // 2. We reached the end but the last character was the matching closing bracket
      if (endl == end && end == begin_val + 1) {
        warn_config(std::format("config: Missing ']' in config option: {}", to_string(begin, end_name)));
        return std::make_pair(endl, std::nullopt);
      }
    }

    return std::make_pair(
      endl,
      std::make_pair(to_string(begin, end_name), to_string(begin_val, endl))
    );
  }

  std::unordered_map<std::string, std::string> parse_config(const std::string_view &file_content) {
    std::unordered_map<std::string, std::string> vars;

    auto pos = std::begin(file_content);
    auto end = std::end(file_content);

    while (pos < end) {
      // auto newline = std::find_if(pos, end, [](auto ch) { return ch == '\n' || ch == '\r'; });
      TUPLE_2D(endl, var, parse_option(pos, end));

      pos = endl;
      if (pos != end) {
        pos += (*pos == '\r') ? 2 : 1;
      }

      if (!var) {
        continue;
      }

      vars.emplace(std::move(*var));
    }

    return vars;
  }

  void string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input) {
    auto it = vars.find(name);
    if (it == std::end(vars)) {
      return;
    }

    input = std::move(it->second);

    vars.erase(it);
  }

  template<typename T, typename F>
  void generic_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, T &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      // VipleStream §CFG.defer: 轉換器若多收一個 key 名稱參數，就把設定項名稱一併傳進去，
      // 這樣它的警告訊息才講得出是哪一個設定項出錯。只收一個參數的舊轉換器維持原本呼叫方式。
      // 不能用預設引數代替：呼叫點是把函式名字當函式參考傳進來，decay 之後預設引數會消失。
      if constexpr (std::is_invocable_v<F, const std::string &, const std::string &>) {
        input = f(tmp, name);
      } else {
        input = f(tmp);
      }
    }
  }

  void string_restricted_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input, const std::vector<std::string_view> &allowed_vals) {
    std::string temp;
    string_f(vars, name, temp);

    for (auto &allowed_val : allowed_vals) {
      if (temp == allowed_val) {
        input = std::move(temp);
        return;
      }
    }
  }

  void string_list_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<std::string> &output) {  // NOSONAR(cpp:S6045) - transparent hasher not available for unordered_map in this codebase
    std::string temp;
    string_f(vars, name, temp);

    if (temp.empty()) {
      return;
    }

    output.clear();
    std::stringstream ss(temp);
    std::string item;
    while (std::getline(ss, item, ',')) {
      // Trim whitespace
      item.erase(0, item.find_first_not_of(" \t\r\n"));
      item.erase(item.find_last_not_of(" \t\r\n") + 1);
      if (!item.empty()) {
        output.push_back(item);
      }
    }
  }

  void path_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, fs::path &input) {
    // appdata needs to be retrieved once only
    static auto appdata = platf::appdata();

    std::string temp;
    string_f(vars, name, temp);

    if (!temp.empty()) {
      input = temp;
    }

    if (input.is_relative()) {
      input = appdata / input;
    }

    auto dir = input;
    dir.remove_filename();

    // Ensure the directories exists
    if (!fs::exists(dir)) {
      fs::create_directories(dir);
    }
  }

  void path_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::string &input) {
    fs::path temp = input;

    path_f(vars, name, temp);

    input = temp.string();
  }

  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    std::string_view val = it->second;

    // If value is something like: "756" instead of 756
    if (val.size() >= 2 && val[0] == '"') {
      val = val.substr(1, val.size() - 2);
    }

    // If that integer is in hexadecimal
    if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
      input = util::from_hex<int>(val.substr(2));
    } else {
      input = (int) util::from_view(val);
    }

    vars.erase(it);
  }

  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input) {
    auto it = vars.find(name);

    if (it == std::end(vars)) {
      return;
    }

    std::string_view val = it->second;

    // If value is something like: "756" instead of 756
    if (val.size() >= 2 && val[0] == '"') {
      val = val.substr(1, val.size() - 2);
    }

    // If that integer is in hexadecimal
    if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
      input = util::from_hex<int>(val.substr(2));
    } else {
      input = util::from_view(val);
    }

    vars.erase(it);
  }

  template<class F>
  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  template<class F>
  void int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::optional<int> &input, F &&f) {
    std::string tmp;
    string_f(vars, name, tmp);
    if (!tmp.empty()) {
      input = f(tmp);
    }
  }

  void int_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, int &input, const std::pair<int, int> &range) {
    int temp = input;

    int_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      input = temp;
    }
  }

  bool to_bool(std::string &boolean) {
    std::for_each(std::begin(boolean), std::end(boolean), [](char ch) {
      return (char) std::tolower(ch);
    });

    return boolean == "true"sv ||
           boolean == "yes"sv ||
           boolean == "enable"sv ||
           boolean == "enabled"sv ||
           boolean == "on"sv ||
           (std::find(std::begin(boolean), std::end(boolean), '1') != std::end(boolean));
  }

  void bool_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, bool &input) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    input = to_bool(tmp);
  }

  void double_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, double &input) {
    std::string tmp;
    string_f(vars, name, tmp);

    if (tmp.empty()) {
      return;
    }

    char *c_str_p;
    auto val = std::strtod(tmp.c_str(), &c_str_p);

    if (c_str_p == tmp.c_str()) {
      return;
    }

    input = val;
  }

  void double_between_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, double &input, const std::pair<double, double> &range) {
    double temp = input;

    double_f(vars, name, temp);

    TUPLE_2D_REF(lower, upper, range);
    if (temp >= lower && temp <= upper) {
      input = temp;
    }
  }

  void list_string_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<std::string> &input) {
    std::string string;
    string_f(vars, name, string);

    if (string.empty()) {
      return;
    }

    input.clear();

    auto begin = std::cbegin(string);
    if (*begin == '[') {
      ++begin;
    }

    begin = std::find_if_not(begin, std::cend(string), whitespace);
    if (begin == std::cend(string)) {
      return;
    }

    auto pos = begin;
    while (pos < std::cend(string)) {
      if (*pos == '[') {
        pos = skip_list(pos + 1, std::cend(string)) + 1;
      } else if (*pos == ']') {
        break;
      } else if (*pos == ',') {
        input.emplace_back(begin, pos);
        pos = begin = std::find_if_not(pos + 1, std::cend(string), whitespace);
      } else {
        ++pos;
      }
    }

    if (pos != begin) {
      input.emplace_back(begin, pos);
    }
  }

  void list_prep_cmd_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<prep_cmd_t> &input) {
    std::string string;
    string_f(vars, name, string);

    std::stringstream jsonStream;

    // check if string is empty, i.e. when the value doesn't exist in the config file
    if (string.empty()) {
      return;
    }

    // We need to add a wrapping object to make it valid JSON, otherwise ptree cannot parse it.
    jsonStream << "{\"prep_cmd\":" << string << "}";

    boost::property_tree::ptree jsonTree;
    boost::property_tree::read_json(jsonStream, jsonTree);

    for (auto &[_, prep_cmd] : jsonTree.get_child("prep_cmd"s)) {
      auto do_cmd = prep_cmd.get_optional<std::string>("do"s);
      auto undo_cmd = prep_cmd.get_optional<std::string>("undo"s);
      auto elevated = prep_cmd.get_optional<bool>("elevated"s);

      input.emplace_back(do_cmd.value_or(""), undo_cmd.value_or(""), elevated.value_or(false));
    }
  }

  void list_int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::vector<int> &input) {
    std::vector<std::string> list;
    list_string_f(vars, name, list);

    // check if list is empty, i.e. when the value doesn't exist in the config file
    if (list.empty()) {
      return;
    }

    // The framerate list must be cleared before adding values from the file configuration.
    // If the list is not cleared, then the specified parameters do not affect the behavior of the sunshine server.
    // That is, if you set only 30 fps in the configuration file, it will not work because by default, during initialization the list includes 10, 30, 60, 90 and 120 fps.
    input.clear();
    for (auto &el : list) {
      std::string_view val = el;

      // If value is something like: "756" instead of 756
      if (val.size() >= 2 && val[0] == '"') {
        val = val.substr(1, val.size() - 2);
      }

      int tmp;

      // If the integer is a hexadecimal
      if (val.size() >= 2 && val.substr(0, 2) == "0x"sv) {
        tmp = util::from_hex<int>(val.substr(2));
      } else {
        tmp = (int) util::from_view(val);
      }
      input.emplace_back(tmp);
    }
  }

  void map_int_int_f(std::unordered_map<std::string, std::string> &vars, const std::string &name, std::unordered_map<int, int> &input) {
    std::vector<int> list;
    list_int_f(vars, name, list);

    // The list needs to be a multiple of 2
    if (list.size() % 2) {
      warn_config(std::format("config: expected {} to have a multiple of two elements --> not {}", name, list.size()));
      return;
    }

    int x = 0;
    while (x < list.size()) {
      auto key = list[x++];
      auto val = list[x++];

      input.emplace(key, val);
    }
  }

  int apply_flags(const char *line) {
    int ret = 0;
    while (*line != '\0') {
      switch (*line) {
        case '0':
          config::sunshine.flags[config::flag::PIN_STDIN].flip();
          break;
        case '1':
          config::sunshine.flags[config::flag::FRESH_STATE].flip();
          break;
        case '2':
          config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE].flip();
          break;
        case 'p':
          config::sunshine.flags[config::flag::UPNP].flip();
          break;
        default:
          warn_config(std::format("config: Unrecognized flag: [{}]", *line));
          ret = -1;
      }

      ++line;
    }

    return ret;
  }

  std::vector<std::string_view> &get_supported_gamepad_options() {
    const auto options = platf::supported_gamepads(nullptr);
    static std::vector<std::string_view> opts {};
    opts.reserve(options.size());
    for (auto &opt : options) {
      opts.emplace_back(opt.name);
    }
    return opts;
  }

  void apply_config(std::unordered_map<std::string, std::string> &&vars) {
    for (auto &[name, val] : vars) {
      // VipleStream §S1-02：這一行在 logging::init() 之前，只到 stdout（service 下即 viplestream-svc.log）；
      // modified_config_settings 只存遮蔽後的值（解析用下面的 vars 原值）
      auto shown = loggable_value(name, val);
      BOOST_LOG(info) << "config: '"sv << name << "' = "sv << shown;
      modified_config_settings[name] = std::move(shown);
    }

    int_f(vars, "qp", video.qp);
    int_between_f(vars, "hevc_mode", video.hevc_mode, {0, 3});
    int_between_f(vars, "av1_mode", video.av1_mode, {0, 3});
    int_f(vars, "min_threads", video.min_threads);
    string_f(vars, "sw_preset", video.sw.sw_preset);
    if (!video.sw.sw_preset.empty()) {
      video.sw.svtav1_preset = sw::svtav1_preset_from_view(video.sw.sw_preset);
    }
    string_f(vars, "sw_tune", video.sw.sw_tune);

    int_between_f(vars, "nvenc_preset", video.nv.quality_preset, {1, 7});
    int_between_f(vars, "nvenc_vbv_increase", video.nv.vbv_percentage_increase, {0, 400});
    bool_f(vars, "nvenc_spatial_aq", video.nv.adaptive_quantization);
    generic_f(vars, "nvenc_twopass", video.nv.two_pass, nv::twopass_from_view);
    bool_f(vars, "nvenc_h264_cavlc", video.nv.h264_cavlc);
    bool_f(vars, "nvenc_realtime_hags", video.nv_realtime_hags);
    bool_f(vars, "nvenc_opengl_vulkan_on_dxgi", video.nv_opengl_vulkan_on_dxgi);
    bool_f(vars, "nvenc_latency_over_power", video.nv_sunshine_high_power_mode);

#if !defined(__ANDROID__) && !defined(__APPLE__)
    video.nv_legacy.preset = video.nv.quality_preset + 11;
    video.nv_legacy.multipass = video.nv.two_pass == nvenc::nvenc_two_pass::quarter_resolution ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION :
                                video.nv.two_pass == nvenc::nvenc_two_pass::full_resolution    ? NV_ENC_TWO_PASS_FULL_RESOLUTION :
                                                                                                 NV_ENC_MULTI_PASS_DISABLED;
    video.nv_legacy.h264_coder = video.nv.h264_cavlc ? NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC : NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    video.nv_legacy.aq = video.nv.adaptive_quantization;
    video.nv_legacy.vbv_percentage_increase = video.nv.vbv_percentage_increase;
#endif

    int_f(vars, "qsv_preset", video.qsv.qsv_preset, qsv::preset_from_view);
    int_f(vars, "qsv_coder", video.qsv.qsv_cavlc, qsv::coder_from_view);
    bool_f(vars, "qsv_slow_hevc", video.qsv.qsv_slow_hevc);

    std::string quality;
    string_f(vars, "amd_quality", quality);
    if (!quality.empty()) {
      video.amd.amd_quality_h264 = amd::quality_from_view<amd::quality_h264_e>(quality, video.amd.amd_quality_h264);
      video.amd.amd_quality_hevc = amd::quality_from_view<amd::quality_hevc_e>(quality, video.amd.amd_quality_hevc);
      video.amd.amd_quality_av1 = amd::quality_from_view<amd::quality_av1_e>(quality, video.amd.amd_quality_av1);
    }

    std::string rc;
    string_f(vars, "amd_rc", rc);
    int_f(vars, "amd_coder", video.amd.amd_coder, amd::coder_from_view);
    if (!rc.empty()) {
      video.amd.amd_rc_h264 = amd::rc_from_view<amd::rc_h264_e>(rc, video.amd.amd_rc_h264);
      video.amd.amd_rc_hevc = amd::rc_from_view<amd::rc_hevc_e>(rc, video.amd.amd_rc_hevc);
      video.amd.amd_rc_av1 = amd::rc_from_view<amd::rc_av1_e>(rc, video.amd.amd_rc_av1);
    }

    std::string usage;
    string_f(vars, "amd_usage", usage);
    if (!usage.empty()) {
      video.amd.amd_usage_h264 = amd::usage_from_view<amd::usage_h264_e>(usage, video.amd.amd_usage_h264);
      video.amd.amd_usage_hevc = amd::usage_from_view<amd::usage_hevc_e>(usage, video.amd.amd_usage_hevc);
      video.amd.amd_usage_av1 = amd::usage_from_view<amd::usage_av1_e>(usage, video.amd.amd_usage_av1);
    }

    bool_f(vars, "amd_preanalysis", (bool &) video.amd.amd_preanalysis);
    bool_f(vars, "amd_vbaq", (bool &) video.amd.amd_vbaq);
    bool_f(vars, "amd_enforce_hrd", (bool &) video.amd.amd_enforce_hrd);

    int_f(vars, "vt_coder", video.vt.vt_coder, vt::coder_from_view);
    int_f(vars, "vt_software", video.vt.vt_allow_sw, vt::allow_software_from_view);
    int_f(vars, "vt_software", video.vt.vt_require_sw, vt::force_software_from_view);
    int_f(vars, "vt_realtime", video.vt.vt_realtime, vt::rt_from_view);

    bool_f(vars, "vaapi_strict_rc_buffer", video.vaapi.strict_rc_buffer);

    int_f(vars, "vk_tune", video.vk.tune);
    int_f(vars, "vk_rc_mode", video.vk.rc_mode);

    string_f(vars, "capture", video.capture);
    string_f(vars, "encoder", video.encoder);
    string_f(vars, "adapter_name", video.adapter_name);
    string_f(vars, "output_name", video.output_name);

    generic_f(vars, "dd_configuration_option", video.dd.configuration_option, dd::config_option_from_view);
    generic_f(vars, "dd_resolution_option", video.dd.resolution_option, dd::resolution_option_from_view);
    string_f(vars, "dd_manual_resolution", video.dd.manual_resolution);
    generic_f(vars, "dd_refresh_rate_option", video.dd.refresh_rate_option, dd::refresh_rate_option_from_view);
    string_f(vars, "dd_manual_refresh_rate", video.dd.manual_refresh_rate);
    generic_f(vars, "dd_hdr_option", video.dd.hdr_option, dd::hdr_option_from_view);
    {
      int value = -1;
      int_between_f(vars, "dd_config_revert_delay", value, {0, std::numeric_limits<int>::max()});
      if (value >= 0) {
        video.dd.config_revert_delay = std::chrono::milliseconds {value};
      }
    }
    bool_f(vars, "dd_config_revert_on_disconnect", video.dd.config_revert_on_disconnect);
    generic_f(vars, "dd_mode_remapping", video.dd.mode_remapping, dd::mode_remapping_from_view);
    {
      int value = 0;
      int_between_f(vars, "dd_wa_hdr_toggle_delay", value, {0, 3000});
      video.dd.wa.hdr_toggle_delay = std::chrono::milliseconds {value};
    }

    int_f(vars, "max_bitrate", video.max_bitrate);
    double_between_f(vars, "minimum_fps_target", video.minimum_fps_target, {0.0, 1000.0});

    path_f(vars, "pkey", nvhttp.pkey);
    path_f(vars, "cert", nvhttp.cert);
    string_f(vars, "sunshine_name", nvhttp.sunshine_name);
    path_f(vars, "log_path", config::sunshine.log_file);
    path_f(vars, "file_state", nvhttp.file_state);

    // Must be run after "file_state"
    config::sunshine.credentials_file = config::nvhttp.file_state;
    path_f(vars, "credentials_file", config::sunshine.credentials_file);

    string_f(vars, "external_ip", nvhttp.external_ip);
    list_prep_cmd_f(vars, "global_prep_cmd", config::sunshine.prep_cmds);

    string_f(vars, "audio_sink", audio.sink);
    string_f(vars, "virtual_sink", audio.virtual_sink);
    bool_f(vars, "stream_audio", audio.stream);
    bool_f(vars, "install_steam_audio_drivers", audio.install_steam_drivers);

    string_restricted_f(vars, "origin_web_ui_allowed", nvhttp.origin_web_ui_allowed, {"pc"sv, "lan"sv, "wan"sv});

    // Parse CSRF allowed origins - always include defaults, then append user-configured origins
    std::vector<std::string> user_csrf_origins;
    string_list_f(vars, "csrf_allowed_origins", user_csrf_origins);

    // Start with default localhost variants
    sunshine.csrf_allowed_origins = {
      "https://localhost",
      "https://127.0.0.1",
      "https://[::1]"
    };

    // Append user-configured origins
    sunshine.csrf_allowed_origins.insert(
      sunshine.csrf_allowed_origins.end(),
      user_csrf_origins.begin(),
      user_csrf_origins.end()
    );

    int to = -1;
    int_between_f(vars, "ping_timeout", to, {-1, std::numeric_limits<int>::max()});
    if (to != -1) {
      stream.ping_timeout = std::chrono::milliseconds(to);
    }

    int_between_f(vars, "lan_encryption_mode", stream.lan_encryption_mode, {0, 2});
    int_between_f(vars, "wan_encryption_mode", stream.wan_encryption_mode, {0, 2});

    path_f(vars, "file_apps", stream.file_apps);
#ifndef __ANDROID__
    // TODO: Android can possibly support this
    if (!fs::exists(stream.file_apps.c_str())) {
      fs::copy_file(SUNSHINE_ASSETS_DIR "/apps.json", stream.file_apps);
      fs::permissions(
        stream.file_apps,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::add
      );
    }
#endif

    int_between_f(vars, "fec_percentage", stream.fec_percentage, {1, 255});

    // VipleStream: NAT traversal config
    string_f(vars, "stun_server", stream.stun_server);
    string_f(vars, "relay_url", stream.relay_url);
    string_f(vars, "relay_psk", stream.relay_psk);
    // VipleStream: UDP tunnel preference (auto|direct|udp_tunnel|ws_tunnel)
    string_f(vars, "udp_tunnel_mode", stream.udp_tunnel_mode);

    // VipleStream: MP-QUIC multipath transport
    bool_f(vars, "mpquic_enabled", stream.mpquic_enabled);
    int_between_f(vars, "mpquic_port", stream.mpquic_port, {1024, 65535});
    int_between_f(vars, "mpquic_scheduler", stream.mpquic_scheduler, {0, 4});
    int_between_f(vars, "mpquic_fec_floor", stream.mpquic_fec_floor, {0, 100});
    int_between_f(vars, "mpquic_congestion", stream.mpquic_congestion, {0, 2});
    int_between_f(vars, "abr_floor_kbps", stream.abr_floor_kbps, {500, 100000});

    // §F7 送出 pacing 開關（原本只能靠 VIPLE_SMOOTH_PACING 環境變數開，違反
    // 「功能設定不用環境變數」規則）。正式設定一律走 sunshine.conf / Web UI 的
    // smooth_pacing，預設 false（線速 burst，與舊版未設環境變數時相同）。
    bool_f(vars, "smooth_pacing", stream.smooth_pacing);

    // §F7：環境變數降級為 dev-only 覆寫，方便 A/B 時不改 conf 就能切換。
    // 優先順序：VIPLE_SMOOTH_PACING（有設且非空）> config smooth_pacing > 預設 false。
    // 真假判斷沿用舊版語意（首字元 '1'/'t'/'T' 為開，其餘為關），讓既有的
    // 開發腳本行為不變。只在這裡解析一次（啟動時），stream.cpp 每幀直接讀
    // config::stream.smooth_pacing，送出迴圈裡不再呼叫 getenv。
    // 這時 logging::init() 還沒跑，所以走 warn_config 延後寫進 sunshine.log。
    if (const char *env = std::getenv("VIPLE_SMOOTH_PACING"); env != nullptr && env[0] != '\0') {
      const bool config_value = stream.smooth_pacing;
      stream.smooth_pacing = (env[0] == '1' || env[0] == 't' || env[0] == 'T');
      warn_config(std::format(
        "[VIPLE-DEVENV] dev-only override VIPLE_SMOOTH_PACING={} "
        "（僅供開發偵錯；環境變數優先於 config：smooth_pacing={} 被覆寫，生效值={}；"
        "正式設定請改用 sunshine.conf 的 smooth_pacing）",
        env,
        config_value ? "enabled" : "disabled",
        stream.smooth_pacing ? "enabled" : "disabled"
      ));
    }

    // VipleStream 2.0 §VR（M1a）：vr_pcvr 預設 disabled；stub 只供開發驗證。
    // enum 與整數選項（vr_opt::*_from_view、vr_opt::int_strict_f）值寫錯一律警告並維持預設
    // （壞值絕不靜靜地變成「開啟」或「關閉安全網」）。
    // 四個布林選項（vr_ctrl_pose_offset、vr_angvel_local、vr_multilink_ctrl、vr_multilink_repair）走
    // vr_opt::bool_strict_f（2026-10-08）：只接受明確的開／關字樣，其他警告並維持預設。
    // vr_multilink_fault 是原樣字串，格式錯誤要到多連線的 VR session 建立時才警告（stream.cpp）。
    generic_f(vars, "vr_pcvr", vr.pcvr, vr_opt::pcvr_from_view);
    vr_opt::int_strict_f(vars, "vr_intra_refresh_frames", vr.intra_refresh_frames, 2, 60, false);
    vr_opt::int_strict_f(vars, "vr_intra_refresh_safety_ms", vr.intra_refresh_safety_ms, 500, 10000, true);
    vr_opt::int_strict_f(vars, "vr_vsync_to_photons_us", vr.vsync_to_photons_us, 1000, 200000, true);
    generic_f(vars, "vr_latch_mode", vr.latch_mode, vr_opt::latch_from_view);
    vr_opt::int_strict_f(vars, "vr_latch_target_pct", vr.latch_target_pct, 25, 50, false);
    vr_opt::int_strict_f(vars, "vr_render_scale_pct", vr.render_scale_pct, 100, 250, false);
    vr_opt::bool_strict_f(vars, "vr_ctrl_pose_offset", vr.ctrl_pose_offset);
    vr_opt::bool_strict_f(vars, "vr_angvel_local", vr.angvel_local);
    generic_f(vars, "vr_stale_policy", vr.stale_policy, vr_opt::stale_from_view);
    generic_f(vars, "vr_multilink", vr.multilink, vr_opt::multilink_from_view);
    string_f(vars, "vr_multilink_fault", vr.multilink_fault);
    vr_opt::bool_strict_f(vars, "vr_multilink_ctrl", vr.multilink_ctrl);
    vr_opt::bool_strict_f(vars, "vr_multilink_repair", vr.multilink_repair);

    map_int_int_f(vars, "keybindings"s, input.keybindings);

    // This config option will only be used by the UI
    // When editing in the config file itself, use "keybindings"
    bool map_rightalt_to_win = false;
    bool_f(vars, "key_rightalt_to_key_win", map_rightalt_to_win);

    if (map_rightalt_to_win) {
      input.keybindings.emplace(0xA5, 0x5B);
    }

    to = std::numeric_limits<int>::min();
    int_f(vars, "back_button_timeout", to);

    if (to > std::numeric_limits<int>::min()) {
      input.back_button_timeout = std::chrono::milliseconds {to};
    }

    double repeat_frequency {0};
    double_between_f(vars, "key_repeat_frequency", repeat_frequency, {0, std::numeric_limits<double>::max()});

    if (repeat_frequency > 0) {
      config::input.key_repeat_period = std::chrono::duration<double> {1 / repeat_frequency};
    }

    to = -1;
    int_f(vars, "key_repeat_delay", to);
    if (to >= 0) {
      input.key_repeat_delay = std::chrono::milliseconds {to};
    }

    string_restricted_f(vars, "gamepad"s, input.gamepad, get_supported_gamepad_options());
    bool_f(vars, "ds4_back_as_touchpad_click", input.ds4_back_as_touchpad_click);
    bool_f(vars, "motion_as_ds4", input.motion_as_ds4);
    bool_f(vars, "touchpad_as_ds4", input.touchpad_as_ds4);
    bool_f(vars, "ds5_inputtino_randomize_mac", input.ds5_inputtino_randomize_mac);

    bool_f(vars, "mouse", input.mouse);
    bool_f(vars, "keyboard", input.keyboard);
    bool_f(vars, "controller", input.controller);

    bool_f(vars, "always_send_scancodes", input.always_send_scancodes);

    bool_f(vars, "high_resolution_scrolling", input.high_resolution_scrolling);
    bool_f(vars, "native_pen_touch", input.native_pen_touch);

    bool_f(vars, "notify_pre_releases", sunshine.notify_pre_releases);
    bool_f(vars, "system_tray", sunshine.system_tray);

    int port = sunshine.port;
    int_between_f(vars, "port"s, port, {1024 + nvhttp::PORT_HTTPS, 65535 - rtsp_stream::RTSP_SETUP_PORT});
    sunshine.port = (std::uint16_t) port;

    // Now that we have the port, add web UI port-specific origins to CSRF allowed list
    // Web UI runs on port + 1 (PORT_HTTPS offset is 1 for confighttp)
    const unsigned short web_ui_port = sunshine.port + 1;
    sunshine.csrf_allowed_origins.push_back(std::format("https://localhost:{}", web_ui_port));
    sunshine.csrf_allowed_origins.push_back(std::format("https://127.0.0.1:{}", web_ui_port));
    sunshine.csrf_allowed_origins.push_back(std::format("https://[::1]:{}", web_ui_port));

    string_restricted_f(vars, "address_family", sunshine.address_family, {"ipv4"sv, "both"sv});
    string_f(vars, "bind_address", sunshine.bind_address);

    bool upnp = false;
    bool_f(vars, "upnp"s, upnp);

    if (upnp) {
      config::sunshine.flags[config::flag::UPNP].flip();
    }

    string_restricted_f(vars, "locale", config::sunshine.locale, {
                                                                   "bg"sv,  // Bulgarian
                                                                   "cs"sv,  // Czech
                                                                   "de"sv,  // German
                                                                   "en"sv,  // English
                                                                   "en_GB"sv,  // English (UK)
                                                                   "en_US"sv,  // English (US)
                                                                   "es"sv,  // Spanish
                                                                   "fr"sv,  // French
                                                                   "hu"sv,  // Hungarian
                                                                   "it"sv,  // Italian
                                                                   "ja"sv,  // Japanese
                                                                   "ko"sv,  // Korean
                                                                   "pl"sv,  // Polish
                                                                   "pt"sv,  // Portuguese
                                                                   "pt_BR"sv,  // Portuguese (Brazilian)
                                                                   "ru"sv,  // Russian
                                                                   "sv"sv,  // Swedish
                                                                   "tr"sv,  // Turkish
                                                                   "uk"sv,  // Ukrainian
                                                                   "vi"sv,  // Vietnamese
                                                                   "zh"sv,  // Chinese
                                                                   "zh_TW"sv,  // Chinese (Traditional)
                                                                 });

    std::string log_level_string;
    string_f(vars, "min_log_level", log_level_string);

    if (!log_level_string.empty()) {
      if (log_level_string == "verbose"sv) {
        sunshine.min_log_level = 0;
      } else if (log_level_string == "debug"sv) {
        sunshine.min_log_level = 1;
      } else if (log_level_string == "info"sv) {
        sunshine.min_log_level = 2;
      } else if (log_level_string == "warning"sv) {
        sunshine.min_log_level = 3;
      } else if (log_level_string == "error"sv) {
        sunshine.min_log_level = 4;
      } else if (log_level_string == "fatal"sv) {
        sunshine.min_log_level = 5;
      } else if (log_level_string == "none"sv) {
        sunshine.min_log_level = 6;
      } else {
        // accept digit directly
        auto val = log_level_string[0];
        if (val >= '0' && val < '7') {
          sunshine.min_log_level = val - '0';
        }
      }
    }

    auto it = vars.find("flags"s);
    if (it != std::end(vars)) {
      apply_flags(it->second.c_str());

      vars.erase(it);
    }

    // VipleStream §CFG.defer: 這是「key 整個拼錯」的警告，與上面「值拼錯」是同一個陷阱的兩面。
    // 上游直接寫 std::cout，以 Windows 服務執行時永遠看不到；改走 warn_config 才會進 sunshine.log。
    // 外層的 min_log_level 判斷保留，因為 pre-init 的 console 輸出不受我們的 sink filter 管控。
    if (sunshine.min_log_level <= 3) {
      for (auto &[var, _] : vars) {
        warn_config(std::format("config: Unrecognized configurable option [{}]", var));
      }
    }
  }

#ifdef _WIN32
  namespace {
    /**
     * VipleStream [VIPLE-SEC]：`--shortcut`（開始功能表捷徑，UAC 過濾後的 token）在 sunshine.conf 被 SYSTEM
     * server 收緊成只有 SY／BA 之後讀不到 conf；file_handler::read_file 會靜默回空字串，port 落回預設值，
     * 自訂 port 的安裝就會開錯 Web UI 網址。conf「存在但打不開」時，改用 SYSTEM server 發佈的
     * `<config>\webui_port`；也讀不到就在 stdout 警告（logging 還沒初始化），不再靜默套用預設值。
     * conf 讀得到（或不存在）時什麼都不做，行為與以前相同。
     */
    void use_published_webui_port() {
      if (std::ifstream {sunshine.config_file}.is_open()) {
        return;
      }
      // 確定不存在才返回；連屬性都查不到（ec 非零）時也當成「存在但打不開」
      std::error_code ec;
      if (!fs::exists(sunshine.config_file, ec) && !ec) {
        return;
      }
      const auto published = platf::config_acl::read_published_webui_port(platf::appdata());
      // 範圍與 apply_config 對 "port" 的檢查相同
      if (published && *published >= 1024 + nvhttp::PORT_HTTPS && *published <= 65535 - rtsp_stream::RTSP_SETUP_PORT) {
        sunshine.port = static_cast<std::uint16_t>(*published);
        return;
      }
      std::cout << "[VIPLE-SEC] shortcut: sunshine.conf not readable; using default Web UI port" << std::endl;
    }
  }  // namespace
#endif

  int parse(int argc, char *argv[]) {
    std::unordered_map<std::string, std::string> cmd_vars;
#ifdef _WIN32
    bool shortcut_launch = false;
    bool service_admin_launch = false;
#endif

    for (auto x = 1; x < argc; ++x) {
      auto line = argv[x];

      if (line == "--help"sv) {
        logging::print_help(*argv);
        return 1;
      }
#ifdef _WIN32
      else if (line == "--shortcut"sv) {
        shortcut_launch = true;
      } else if (line == "--shortcut-admin"sv) {
        service_admin_launch = true;
      }
#endif
      else if (*line == '-') {
        if (*(line + 1) == '-') {
          sunshine.cmd.name = line + 2;
          sunshine.cmd.argc = argc - x - 1;
          sunshine.cmd.argv = argv + x + 1;

          break;
        }
        if (apply_flags(line + 1)) {
          logging::print_help(*argv);
          return -1;
        }
      } else {
        auto line_end = line + strlen(line);

        auto pos = std::find(line, line_end, '=');
        if (pos == line_end) {
          sunshine.config_file = line;
        } else {
          std::string_view line_sv(line, line_end - line);
          TUPLE_EL(var, 1, parse_option(line_sv.cbegin(), line_sv.cend()));
          if (!var) {
            logging::print_help(*argv);
            return -1;
          }

          TUPLE_EL_REF(name, 0, *var);

          auto it = cmd_vars.find(name);
          if (it != std::end(cmd_vars)) {
            cmd_vars.erase(it);
          }

          cmd_vars.emplace(std::move(*var));
        }
      }
    }

    bool config_loaded = false;
    try {
      // Create appdata folder if it does not exist
      file_handler::make_directory(platf::appdata().string());

      // Create empty config file if it does not exist
      if (!fs::exists(sunshine.config_file)) {
        std::ofstream {sunshine.config_file};
      }

      // Read config file
      auto vars = parse_config(file_handler::read_file(sunshine.config_file.c_str()));

      for (auto &[name, value] : cmd_vars) {
        vars.insert_or_assign(std::move(name), std::move(value));
      }

      // Apply the config. Note: This will try to create any paths
      // referenced in the config, so we may receive exceptions if
      // the path is incorrect or inaccessible.
      apply_config(std::move(vars));
      config_loaded = true;
    } catch (const std::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    } catch (const boost::filesystem::filesystem_error &err) {
      BOOST_LOG(fatal) << "Failed to apply config: "sv << err.what();
    }

#ifdef _WIN32
    // UCRT64 raises an access denied exception if launching from the shortcut
    // as non-admin and the config folder is not yet present; we can defer
    // so that service instance will do the work instead.

    if (!config_loaded && !shortcut_launch) {
      BOOST_LOG(fatal) << "To relaunch VipleStream-Server successfully, use the shortcut in the Start Menu. Do not run the server executable manually."sv;
      std::this_thread::sleep_for(10s);
#else
    if (!config_loaded) {
#endif
      return -1;
    }

#ifdef _WIN32
    // We have to wait until the config is loaded to handle these launches,
    // because we need to have the correct base port loaded in our config.
    // Exception: UCRT64 shortcut_launch instances may have no config loaded due to
    // insufficient permissions to create folder; port defaults will be acceptable.
    if (service_admin_launch) {
      // This is a relaunch as admin to start the service
      service_ctrl::start_service();

      // Always return 1 to ensure VipleStream-Server doesn't start normally
      return 1;
    }
    if (shortcut_launch) {
      // VipleStream [VIPLE-SEC]：conf 已被收緊、這個行程讀不到時，改用 server 發佈的 Web UI port
      use_published_webui_port();

      if (!service_ctrl::is_service_running()) {
        // If the service isn't running, relaunch ourselves as admin to start it
        WCHAR executable[MAX_PATH];
        GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));

        SHELLEXECUTEINFOW shell_exec_info {};
        shell_exec_info.cbSize = sizeof(shell_exec_info);
        shell_exec_info.fMask = SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE | SEE_MASK_NOCLOSEPROCESS;
        shell_exec_info.lpVerb = L"runas";
        shell_exec_info.lpFile = executable;
        shell_exec_info.lpParameters = L"--shortcut-admin";
        shell_exec_info.nShow = SW_NORMAL;
        if (!ShellExecuteExW(&shell_exec_info)) {
          auto winerr = GetLastError();
          BOOST_LOG(error) << "Failed executing shell command: " << winerr << std::endl;
          return 1;
        }

        // Wait for the elevated process to finish starting the service
        WaitForSingleObject(shell_exec_info.hProcess, INFINITE);
        CloseHandle(shell_exec_info.hProcess);

        // Wait for the UI to be ready for connections
        service_ctrl::wait_for_ui_ready();
      }

      // Launch the web UI
      launch_ui();

      // Always return 1 to ensure VipleStream-Server doesn't start normally
      return 1;
    }
#endif

    return 0;
  }
}  // namespace config
