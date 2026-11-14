/**
 * @file src/platform/common.h
 * @brief Declarations for common platform specific utilities.
 */
#pragma once

// standard includes
#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <boost/core/noncopyable.hpp>
#ifndef _WIN32
  #include <boost/asio.hpp>
  #include <boost/process/v1.hpp>
#endif

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/thread_safe.h"
#include "src/utility.h"
#include "src/video_colorspace.h"
#include "src/vr/vr_frame_meta.h"

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

using namespace std::literals;

struct sockaddr;
struct AVFrame;
struct AVBufferRef;
struct AVHWFramesContext;
struct AVCodecContext;
struct AVDictionary;

#ifdef _WIN32
// Forward declarations of boost classes to avoid having to include boost headers
// here, which results in issues with Windows.h and WinSock2.h include order.
namespace boost {
  namespace asio {
    namespace ip {
      class address;
    }  // namespace ip
  }  // namespace asio

  namespace filesystem {
    class path;
  }

  namespace process::v1 {
    class child;
    class group;
    template<typename Char>
    class basic_environment;
    typedef basic_environment<char> environment;
  }  // namespace process::v1
}  // namespace boost
#endif
namespace video {
  struct config_t;
}  // namespace video

namespace nvenc {
  class nvenc_base;
}

namespace platf {
  // Limited by bits in activeGamepadMask
  constexpr auto MAX_GAMEPADS = 16;

  constexpr std::uint32_t DPAD_UP = 0x0001;
  constexpr std::uint32_t DPAD_DOWN = 0x0002;
  constexpr std::uint32_t DPAD_LEFT = 0x0004;
  constexpr std::uint32_t DPAD_RIGHT = 0x0008;
  constexpr std::uint32_t START = 0x0010;
  constexpr std::uint32_t BACK = 0x0020;
  constexpr std::uint32_t LEFT_STICK = 0x0040;
  constexpr std::uint32_t RIGHT_STICK = 0x0080;
  constexpr std::uint32_t LEFT_BUTTON = 0x0100;
  constexpr std::uint32_t RIGHT_BUTTON = 0x0200;
  constexpr std::uint32_t HOME = 0x0400;
  constexpr std::uint32_t A = 0x1000;
  constexpr std::uint32_t B = 0x2000;
  constexpr std::uint32_t X = 0x4000;
  constexpr std::uint32_t Y = 0x8000;
  constexpr std::uint32_t PADDLE1 = 0x010000;
  constexpr std::uint32_t PADDLE2 = 0x020000;
  constexpr std::uint32_t PADDLE3 = 0x040000;
  constexpr std::uint32_t PADDLE4 = 0x080000;
  constexpr std::uint32_t TOUCHPAD_BUTTON = 0x100000;
  constexpr std::uint32_t MISC_BUTTON = 0x200000;

  struct supported_gamepad_t {
    std::string name;
    bool is_enabled;
    std::string reason_disabled;
  };

  enum class gamepad_feedback_e {
    rumble,  ///< Rumble
    rumble_triggers,  ///< Rumble triggers
    set_motion_event_state,  ///< Set motion event state
    set_rgb_led,  ///< Set RGB LED
    set_adaptive_triggers,  ///< Set adaptive triggers
    sc_hid_feature_request,  ///< §SC-HID: request client to forward feature report from real SC
  };

  struct gamepad_feedback_msg_t {
    static gamepad_feedback_msg_t make_rumble(std::uint16_t id, std::uint16_t lowfreq, std::uint16_t highfreq) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::rumble;
      msg.id = id;
      msg.data.rumble = {lowfreq, highfreq};
      return msg;
    }

    static gamepad_feedback_msg_t make_rumble_triggers(std::uint16_t id, std::uint16_t left, std::uint16_t right) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::rumble_triggers;
      msg.id = id;
      msg.data.rumble_triggers = {left, right};
      return msg;
    }

    static gamepad_feedback_msg_t make_motion_event_state(std::uint16_t id, std::uint8_t motion_type, std::uint16_t report_rate) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_motion_event_state;
      msg.id = id;
      msg.data.motion_event_state.motion_type = motion_type;
      msg.data.motion_event_state.report_rate = report_rate;
      return msg;
    }

    static gamepad_feedback_msg_t make_rgb_led(std::uint16_t id, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_rgb_led;
      msg.id = id;
      msg.data.rgb_led = {r, g, b};
      return msg;
    }

    static gamepad_feedback_msg_t make_adaptive_triggers(std::uint16_t id, uint8_t event_flags, uint8_t type_left, uint8_t type_right, const std::array<uint8_t, 10> &left, const std::array<uint8_t, 10> &right) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::set_adaptive_triggers;
      msg.id = id;
      msg.data.adaptive_triggers = {.event_flags = event_flags, .type_left = type_left, .type_right = type_right, .left = left, .right = right};
      return msg;
    }

    gamepad_feedback_e type;
    std::uint16_t id;

    union {
      struct {
        std::uint16_t lowfreq;
        std::uint16_t highfreq;
      } rumble;

      struct {
        std::uint16_t left_trigger;
        std::uint16_t right_trigger;
      } rumble_triggers;

      struct {
        std::uint16_t report_rate;
        std::uint8_t motion_type;
      } motion_event_state;

      struct {
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
      } rgb_led;

      struct {
        uint16_t controllerNumber;
        uint8_t event_flags;
        uint8_t type_left;
        uint8_t type_right;
        std::array<uint8_t, 10> left;
        std::array<uint8_t, 10> right;
      } adaptive_triggers;

      // §SC-HID Phase 2C：透明 feature proxy 的「Steam 待辦事件」轉發到 client。
      struct {
        uint8_t reportId;   // HID report id Steam targeted (0x01)
        uint8_t op;         // 1 = GET (client re-reads real SC), 2 = SET (forward query to SC)
        uint8_t seq;        // round-trip cookie
        uint8_t queryLen;   // valid bytes in query (op=SET)
        uint8_t query[64];  // bytes Steam sent via SET_FEATURE
      } sc_hid_feature;
    } data;

    static gamepad_feedback_msg_t make_sc_hid_feature_request(uint8_t reportId, uint8_t op,
                                                              uint8_t seq, const uint8_t *query,
                                                              uint8_t queryLen) {
      gamepad_feedback_msg_t msg;
      msg.type = gamepad_feedback_e::sc_hid_feature_request;
      msg.id = 0;
      msg.data.sc_hid_feature.reportId = reportId;
      msg.data.sc_hid_feature.op = op;
      msg.data.sc_hid_feature.seq = seq;
      uint8_t n = queryLen > 64 ? 64 : queryLen;
      msg.data.sc_hid_feature.queryLen = n;
      for (uint8_t i = 0; i < 64; i++) {
        msg.data.sc_hid_feature.query[i] = (query && i < n) ? query[i] : 0;
      }
      return msg;
    }
  };

  using feedback_queue_t = safe::mail_raw_t::queue_t<gamepad_feedback_msg_t>;

  namespace speaker {
    enum speaker_e {
      FRONT_LEFT,  ///< Front left
      FRONT_RIGHT,  ///< Front right
      FRONT_CENTER,  ///< Front center
      LOW_FREQUENCY,  ///< Low frequency
      BACK_LEFT,  ///< Back left
      BACK_RIGHT,  ///< Back right
      SIDE_LEFT,  ///< Side left
      SIDE_RIGHT,  ///< Side right
      MAX_SPEAKERS,  ///< Maximum number of speakers
    };

    constexpr std::uint8_t map_stereo[] {
      FRONT_LEFT,
      FRONT_RIGHT
    };
    constexpr std::uint8_t map_surround51[] {
      FRONT_LEFT,
      FRONT_RIGHT,
      FRONT_CENTER,
      LOW_FREQUENCY,
      BACK_LEFT,
      BACK_RIGHT,
    };
    constexpr std::uint8_t map_surround71[] {
      FRONT_LEFT,
      FRONT_RIGHT,
      FRONT_CENTER,
      LOW_FREQUENCY,
      BACK_LEFT,
      BACK_RIGHT,
      SIDE_LEFT,
      SIDE_RIGHT,
    };
  }  // namespace speaker

  enum class mem_type_e {
    system,  ///< System memory
    vaapi,  ///< VAAPI
    dxgi,  ///< DXGI
    cuda,  ///< CUDA
    videotoolbox,  ///< VideoToolbox
    vulkan,  ///< Vulkan
    unknown  ///< Unknown
  };

  enum class pix_fmt_e {
    yuv420p,  ///< YUV 4:2:0
    yuv420p10,  ///< YUV 4:2:0 10-bit
    nv12,  ///< NV12
    p010,  ///< P010
    ayuv,  ///< AYUV
    yuv444p16,  ///< Planar 10-bit (shifted to 16-bit) YUV 4:4:4
    y410,  ///< Y410
    unknown  ///< Unknown
  };

  inline std::string_view from_pix_fmt(pix_fmt_e pix_fmt) {
    using namespace std::literals;
#define _CONVERT(x) \
  case pix_fmt_e::x: \
    return #x##sv
    switch (pix_fmt) {
      _CONVERT(yuv420p);
      _CONVERT(yuv420p10);
      _CONVERT(nv12);
      _CONVERT(p010);
      _CONVERT(ayuv);
      _CONVERT(yuv444p16);
      _CONVERT(y410);
      _CONVERT(unknown);
    }
#undef _CONVERT

    return "unknown"sv;
  }

  // Dimensions for touchscreen input
  struct touch_port_t {
    int offset_x;
    int offset_y;
    int width;
    int height;
    int logical_width;
    int logical_height;
  };

  // These values must match Limelight-internal.h's SS_FF_* constants!
  namespace platform_caps {
    typedef uint32_t caps_t;

    constexpr caps_t pen_touch = 0x01;  // Pen and touch events
    constexpr caps_t controller_touch = 0x02;  // Controller touch events
  };  // namespace platform_caps

  struct gamepad_state_t {
    std::uint32_t buttonFlags;
    std::uint8_t lt;
    std::uint8_t rt;
    std::int16_t lsX;
    std::int16_t lsY;
    std::int16_t rsX;
    std::int16_t rsY;
  };

  struct gamepad_id_t {
    // The global index is used when looking up gamepads in the platform's
    // gamepad array. It identifies gamepads uniquely among all clients.
    int globalIndex;

    // The client-relative index is the controller number as reported by the
    // client. It must be used when communicating back to the client via
    // the input feedback queue.
    std::uint8_t clientRelativeIndex;
  };

  struct gamepad_arrival_t {
    std::uint8_t type;
    std::uint16_t capabilities;
    std::uint32_t supportedButtons;
  };

  struct gamepad_touch_t {
    gamepad_id_t id;
    std::uint8_t eventType;
    std::uint32_t pointerId;
    float x;
    float y;
    float pressure;
  };

  struct gamepad_motion_t {
    gamepad_id_t id;
    std::uint8_t motionType;

    // Accel: m/s^2
    // Gyro: deg/s
    float x;
    float y;
    float z;
  };

  struct gamepad_battery_t {
    gamepad_id_t id;
    std::uint8_t state;
    std::uint8_t percentage;
  };

  struct touch_input_t {
    std::uint8_t eventType;
    std::uint16_t rotation;  // Degrees (0..360) or LI_ROT_UNKNOWN
    std::uint32_t pointerId;
    float x;
    float y;
    float pressureOrDistance;  // Distance for hover and pressure for contact
    float contactAreaMajor;
    float contactAreaMinor;
  };

  struct pen_input_t {
    std::uint8_t eventType;
    std::uint8_t toolType;
    std::uint8_t penButtons;
    std::uint8_t tilt;  // Degrees (0..90) or LI_TILT_UNKNOWN
    std::uint16_t rotation;  // Degrees (0..360) or LI_ROT_UNKNOWN
    float x;
    float y;
    float pressureOrDistance;  // Distance for hover and pressure for contact
    float contactAreaMajor;
    float contactAreaMinor;
  };

  class deinit_t {
  public:
    virtual ~deinit_t() = default;
  };

  struct img_t: std::enable_shared_from_this<img_t> {
  public:
    img_t() = default;

    img_t(img_t &&) = delete;
    img_t(const img_t &) = delete;
    img_t &operator=(img_t &&) = delete;
    img_t &operator=(const img_t &) = delete;

    std::uint8_t *data {};
    std::int32_t width {};
    std::int32_t height {};
    std::int32_t pixel_pitch {};
    std::int32_t row_pitch {};

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

    // VipleStream 2.0 §VR（M1b S1-03）：這一幀的 VR metadata。只有 pcvr 的擷取端
    // （display_vr_t）會填；一般 session 與 M1a stub 永遠是 nullopt。
    // 影像池回收時（video.cpp 的 pull_free_image_callback）與 frame_timestamp 一起清掉，
    // 避免上一幀的 pose／frame_id 被帶到下一幀。
    std::optional<::video::vr_frame_meta_t> vr;

    virtual ~img_t() = default;
  };

  struct sink_t {
    // Play on host PC
    std::string host;

    // On macOS and Windows, it is not possible to create a virtual sink
    // Therefore, it is optional
    struct null_t {
      std::string stereo;
      std::string surround51;
      std::string surround71;
    };

    std::optional<null_t> null;
  };

  struct encode_device_t {
    virtual ~encode_device_t() = default;

    virtual int convert(platf::img_t &img) = 0;

    video::sunshine_colorspace_t colorspace;
  };

  struct avcodec_encode_device_t: encode_device_t {
    void *data {};
    AVFrame *frame {};

    int convert(platf::img_t &img) override {
      return -1;
    }

    virtual void apply_colorspace() {
    }

    /**
     * @brief Set the frame to be encoded.
     * @note Implementations must take ownership of 'frame'.
     */
    virtual int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) {
      BOOST_LOG(error) << "Illegal call to hwdevice_t::set_frame(). Did you forget to override it?";
      return -1;
    };

    /**
     * @brief Initialize the hwframes context.
     * @note Implementations may set parameters during initialization of the hwframes context.
     */
    virtual void init_hwframes(AVHWFramesContext *frames) {};

    /**
     * @brief Provides a hook for allow platform-specific code to adjust codec options.
     * @note Implementations may set or modify codec options prior to codec initialization.
     */
    virtual void init_codec_options(AVCodecContext *ctx, AVDictionary **options) {};

    /**
     * @brief Prepare to derive a context.
     * @note Implementations may make modifications required before context derivation
     */
    virtual int prepare_to_derive_context(int hw_device_type) {
      return 0;
    };
  };

  struct nvenc_encode_device_t: encode_device_t {
    virtual bool init_encoder(const video::config_t &client_config, const video::sunshine_colorspace_t &colorspace) = 0;

    nvenc::nvenc_base *nvenc = nullptr;
  };

  enum class capture_e : int {
    ok,  ///< Success
    reinit,  ///< Need to reinitialize
    timeout,  ///< Timeout
    interrupted,  ///< Capture was interrupted
    error  ///< Error
  };

  class display_t {
  public:
    /**
     * @brief Callback for when a new image is ready.
     * When display has a new image ready or a timeout occurs, this callback will be called with the image.
     * If a frame was captured, frame_captured will be true. If a timeout occurred, it will be false.
     * @retval true On success
     * @retval false On break request
     */
    using push_captured_image_cb_t = std::function<bool(std::shared_ptr<img_t> &&img, bool frame_captured)>;

    /**
     * @brief Get free image from pool.
     * Calls must be synchronized.
     * Blocks until there is free image in the pool or capture is interrupted.
     * @retval true On success, img_out contains free image
     * @retval false When capture has been interrupted, img_out contains nullptr
     */
    using pull_free_image_cb_t = std::function<bool(std::shared_ptr<img_t> &img_out)>;

    display_t() noexcept:
        offset_x {0},
        offset_y {0} {
    }

    /**
     * @brief Capture a frame.
     * @param push_captured_image_cb The callback that is called with captured image,
     * must be called from the same thread as capture()
     * @param pull_free_image_cb Capture backends call this callback to get empty image from the pool.
     * If backend uses multiple threads, calls to this callback must be synchronized.
     * Calls to this callback and push_captured_image_cb must be synchronized as well.
     * @param cursor A pointer to the flag that indicates whether the cursor should be captured as well.
     * @retval capture_e::ok When stopping
     * @retval capture_e::error On error
     * @retval capture_e::reinit When need of reinitialization
     */
    virtual capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) = 0;

    virtual std::shared_ptr<img_t> alloc_img() = 0;

    virtual int dummy_img(img_t *img) = 0;

    virtual std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    virtual std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(pix_fmt_e pix_fmt) {
      return nullptr;
    }

    virtual bool is_hdr() {
      return false;
    }

    virtual bool get_hdr_metadata(SS_HDR_METADATA &metadata) {
      std::memset(&metadata, 0, sizeof(metadata));
      return false;
    }

    /**
     * @brief Check that a given codec is supported by the display device.
     * @param name The FFmpeg codec name (or similar for non-FFmpeg codecs).
     * @param config The codec configuration.
     * @return `true` if supported, `false` otherwise.
     */
    virtual bool is_codec_supported(std::string_view name, const ::video::config_t &config) {
      return true;
    }

    virtual bool is_event_driven() {
      return false;
    }

    virtual ~display_t() = default;

    // Offsets for when streaming a specific monitor. By default, they are 0.
    int offset_x;
    int offset_y;
    int env_width;
    int env_height;
    int env_logical_width;
    int env_logical_height;
    int width;
    int height;
    int logical_width;
    int logical_height;

  protected:
    // collect capture timing data (at loglevel debug)
    logging::time_delta_periodic_logger sleep_overshoot_logger = {debug, "Frame capture sleep overshoot"};
  };

  class mic_t {
  public:
    virtual capture_e sample(std::vector<float> &frame_buffer) = 0;

    /**
     * @brief §AUD-MON：被擷取的播放裝置目前的音量峰值（0–1，系統音量計）。
     *        平台不支援時回 std::nullopt（不做擷取健康檢查）。只能在擷取執行緒呼叫。
     */
    virtual std::optional<float> render_peak() {
      return std::nullopt;
    }

    virtual ~mic_t() = default;
  };

  class audio_control_t {
  public:
    virtual int set_sink(const std::string &sink) = 0;

    virtual std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous, [[maybe_unused]] bool host_audio_enabled) = 0;

    /**
     * @brief Check if the audio sink is available in the system.
     * @param sink Sink to be checked.
     * @returns True if available, false otherwise.
     */
    virtual bool is_sink_available(const std::string &sink) = 0;

    virtual std::optional<sink_t> sink_info() = 0;

    virtual ~audio_control_t() = default;
  };

  void freeInput(void *);

  using input_t = util::safe_ptr<void, freeInput>;

  std::filesystem::path appdata();

  std::string get_mac_address(const std::string_view &address);

  /**
   * @brief §MP-ADV 伺服器介面廣告 — 列舉本機所有可用網路介面。
   * 回傳的清單會透過 /serverinfo XML 廣告給客戶端，供 MP-QUIC
   * 多路徑探測使用。過濾 loopback、虛擬介面（Hyper-V、VMware、Docker）等。
   */
  struct advertised_interface {
    std::string address;  // IPv4 字串，如 "192.168.1.20"
    std::string name;     // 介面名稱，如 "Ethernet", "Tailscale"
    std::string type;     // "ethernet", "wifi", "vpn"
  };
  std::vector<advertised_interface> enum_net_interfaces();

  /**
   * @brief §PERF-1113-R2（A2）：一次列舉同時拿到廣告清單與 `mac_for` 那個本機位址所在網卡的 MAC（/serverinfo 用，
   *        原本一次請求列舉三遍）。MAC 從沒過濾的列舉找（全部網卡、含 IPv6 與虛擬網卡）；不可以改成從回傳的清單找——
   *        那份只有 IPv4、濾掉虛擬網卡，從 IPv6 或 vEthernet 連進來的 client 會拿到全 0，網路喚醒失效。
   * @param mac_for 要查 MAC 的本機位址；空字串＝不查。
   * @param mac_out 查到的 MAC；沒查或查不到是 "00:00:00:00:00:00"。
   */
#ifdef _WIN32
  std::vector<advertised_interface> enum_net_interfaces(const std::string_view &mac_for, std::string &mac_out);
#else
  // getifaddrs 很便宜，不必併成一次
  inline std::vector<advertised_interface> enum_net_interfaces(const std::string_view &mac_for, std::string &mac_out) {
    mac_out = mac_for.empty() ? std::string {"00:00:00:00:00:00"} : get_mac_address(mac_for);
    return enum_net_interfaces();
  }
#endif

  /**
   * @brief §PERF-1113-R2（A2）：廣告清單和上一次印的不同（或第一次）才印 [VIPLE-MPADV]。
   *        VR 多連線中每秒列舉一次、/serverinfo 每次請求也列舉，每次都印佔了 host log 的 15～46%。
   *        HTTP、HTTPS、vr_link_addrs 三條執行緒共用，所以有鎖；「上次印的清單」放在不釋放的堆積物件上——
   *        vr_link_addrs 是 detach 的執行緒，行程結束、靜態物件解構之後還可能在跑。
   */
  inline void log_advertised_interfaces_if_changed(const std::vector<advertised_interface> &ifaces) {
    struct state_t {
      std::mutex mtx;
      bool valid = false;
      std::vector<advertised_interface> last;
    };
    static state_t *const state = new state_t;  // 故意不釋放

    {
      std::lock_guard lk {state->mtx};
      bool same = state->valid && state->last.size() == ifaces.size();
      for (std::size_t i = 0; same && i < ifaces.size(); ++i) {
        same = state->last[i].address == ifaces[i].address && state->last[i].name == ifaces[i].name && state->last[i].type == ifaces[i].type;
      }
      if (same) {
        return;
      }
      state->last = ifaces;
      state->valid = true;
    }

    BOOST_LOG(info) << "[VIPLE-MPADV] Enumerated " << ifaces.size() << " network interface(s) for advertisement (logged only when the list changes)";
    for (const auto &iface : ifaces) {
      BOOST_LOG(info) << "[VIPLE-MPADV]   " << iface.name << " (" << iface.type << "): " << iface.address;
    }
  }

  /**
   * @brief §PERF-1113-R2（A2）：這個位址第一次查不到 MAC 才回 true。「Unable to find MAC address」每個位址只印一次：
   *        Tailscale 之類沒有實體位址的介面，原本每次 HTTPS /serverinfo 都印一行。
   */
  inline bool mac_miss_first_time(const std::string_view &address) {
    struct state_t {
      std::mutex mtx;
      std::set<std::string, std::less<>> seen;
    };
    static state_t *const state = new state_t;  // 故意不釋放（理由同上）

    std::lock_guard lk {state->mtx};
    if (state->seen.find(address) != state->seen.end() || state->seen.size() >= 64) {
      return false;  // 本機位址數量有限，上限只是保險
    }
    state->seen.emplace(address);
    return true;
  }

  std::string from_sockaddr(const sockaddr *const);
  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const);

  std::unique_ptr<audio_control_t> audio_control();

  /**
   * @brief Get the display_t instance for the given hwdevice_type.
   * If display_name is empty, use the first monitor that's compatible you can find
   * If you require to use this parameter in a separate thread, make a copy of it.
   * @param display_name The name of the monitor that SHOULD be displayed
   * @param config Stream configuration
   * @return The display_t instance based on hwdevice_type.
   */
  std::shared_ptr<display_t> display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);

  // A list of names of displays accepted as display_name with the mem_type_e
  std::vector<std::string> display_names(mem_type_e hwdevice_type);

  /**
   * @brief Check if GPUs/drivers have changed since the last call to this function.
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool needs_encoder_reenumeration();

  /**
   * @brief §VR-PROBE-CACHE：上次呼叫之後顯示卡／驅動的列舉有沒有變過（VR 編碼器探測的快取要不要作廢）。
   *        自己留一個 DXGI factory：不可以借用 needs_encoder_reenumeration() 的那一個，問過它就會吃掉桌面探測要看的變動訊號。
   * @return `true`＝變過、第一次呼叫、或無法判斷。只有 Windows 有 VR 探測，其他平台一律回 `true`（不快取）。
   */
#ifdef _WIN32
  bool vr_probe_gpu_changed();
#else
  inline bool vr_probe_gpu_changed() {
    return true;
  }
#endif

  boost::process::v1::child run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const boost::process::v1::environment &env, FILE *file, std::error_code &ec, boost::process::v1::group *group);

  enum class thread_priority_e : int {
    low,  ///< Low priority
    normal,  ///< Normal priority
    high,  ///< High priority
    critical  ///< Critical priority
  };
  void adjust_thread_priority(thread_priority_e priority);

  /**
   * @brief Name the current thread for use with development tools.
   * @note On Linux this will be truncated after 15 characters.
   */
  void set_thread_name(const std::string &name);

  void enable_mouse_keys();

  // Allow OS-specific actions to be taken to prepare for streaming
  void streaming_will_start();
  void streaming_will_stop();

  /**
   * @brief §PERF-1113-R2（A4）：等 streaming_will_start() 丟到背景的工作做完（Windows 重套 NVIDIA 設定檔，約 0.4 秒；
   *        原本同步做，擋在 RTSP ANNOUNCE 的回應前面）。streaming_will_stop() 自己會先等；行程結束前要還原
   *        NVIDIA 全域設定的地方（main.cpp）也要先呼叫，nvprefs_instance 不是執行緒安全的。其他平台沒有背景工作。
   */
#ifdef _WIN32
  void streaming_start_tasks_wait();
#else
  inline void streaming_start_tasks_wait() {
  }
#endif

  void restart();

  /**
   * @brief Set an environment variable.
   * @param name The name of the environment variable.
   * @param value The value to set the environment variable to.
   * @return 0 on success, non-zero on failure.
   */
  int set_env(const std::string &name, const std::string &value);

  /**
   * @brief Unset an environment variable.
   * @param name The name of the environment variable.
   * @return 0 on success, non-zero on failure.
   */
  int unset_env(const std::string &name);

  struct buffer_descriptor_t {
    const char *buffer;
    size_t size;

    // Constructors required for emplace_back() prior to C++20
    buffer_descriptor_t(const char *buffer, size_t size):
        buffer(buffer),
        size(size) {
    }

    buffer_descriptor_t():
        buffer(nullptr),
        size(0) {
    }
  };

  struct batched_send_info_t {
    // Optional headers to be prepended to each packet
    const char *headers;
    size_t header_size;

    // One or more data buffers to use for the payloads
    //
    // NB: Data buffers must be aligned to payload size!
    std::vector<buffer_descriptor_t> &payload_buffers;
    size_t payload_size;

    // The offset (in header+payload message blocks) in the header and payload
    // buffers to begin sending messages from
    size_t block_offset;

    // The number of header+payload message blocks to send
    size_t block_count;

    std::uintptr_t native_socket;
    boost::asio::ip::address &target_address;
    uint16_t target_port;
    boost::asio::ip::address &source_address;

    /**
     * @brief Returns a payload buffer descriptor for the given payload offset.
     * @param offset The offset in the total payload data (bytes).
     * @return Buffer descriptor describing the region at the given offset.
     */
    buffer_descriptor_t buffer_for_payload_offset(ptrdiff_t offset) {
      for (const auto &desc : payload_buffers) {
        if (offset < desc.size) {
          return {
            desc.buffer + offset,
            desc.size - offset,
          };
        } else {
          offset -= desc.size;
        }
      }
      return {};
    }
  };

  bool send_batch(batched_send_info_t &send_info);

#ifdef _WIN32
  /**
   * @brief §PERF-1113（審查 05 第 1 項）：把一塊連續、每包等長的緩衝用一次 WSASendMsg＋UDP_SEND_MSG_SIZE 送出（非阻塞 socket）。
   *        和 send_batch 的差別：不帶來源位址（socket 已綁在指定的本機位址）、分得出 would_block 與其他錯誤。
   *        只有 Windows 有（VR 多連線的送出執行緒用）；其他平台沒有這個函式，呼叫端要包在 `#ifdef _WIN32` 裡。
   * @param err 失敗時的 WSA 錯誤碼（可為 nullptr）
   * @return 1＝整批交給系統、0＝would_block（一個都沒送）、-1＝其他錯誤（一個都沒送）
   */
  int send_uso_nonblocking(std::uintptr_t native_socket, const boost::asio::ip::address &target_address, uint16_t target_port, const char *data, size_t packet_size, size_t packet_count, int *err);
#endif

  struct send_info_t {
    const char *header;
    size_t header_size;
    const char *payload;
    size_t payload_size;

    std::uintptr_t native_socket;
    boost::asio::ip::address &target_address;
    uint16_t target_port;
    boost::asio::ip::address &source_address;
  };

  bool send(send_info_t &send_info);

  /**
   * @brief §PERF-1113-R2（A13）：platf::send 失敗 warning 的彙總，每條送出執行緒各一份（thread_local）。
   *        路徑斷掉時每一包都失敗、每包一行，實測每秒約 1.6 萬行。規則：某個錯誤碼第一次出現照原格式印，
   *        之後同一個碼每秒最多一行（帶這段期間又失敗幾次），最後一次失敗過了一秒之後第一次送成功時印一行恢復。
   *        各執行緒不共用：影像與音訊的錯誤碼不同，混在一起就看不出是哪一條在失敗。
   *        這個類別只算、不印（Windows 與 Linux 的訊息前綴不同）。
   */
  class send_fail_log_t {
  public:
    enum class action_e {
      none,  ///< 不印
      first,  ///< 這個錯誤碼第一次出現（或隔了很久又出現）：照原格式印
      repeated,  ///< 同一個錯誤碼的彙總行；repeated_out＝上一行之後又失敗的次數
    };

    /// 送出失敗時呼叫。錯誤碼要在失敗後立刻取出再傳進來（這裡會取時間）。
    action_e on_fail(int err, std::uint32_t &repeated_out) {
      const auto now = std::chrono::steady_clock::now();
      if (active_ && now - last_fail_ >= std::chrono::seconds(5)) {
        // 上一輪失敗之後這條執行緒沒有再送成功過（session 拆掉了），沒機會印恢復：當成新的一輪
        slots_ = {};
      }
      active_ = true;
      last_fail_ = now;

      slot_t *slot = nullptr;
      for (auto &s : slots_) {
        if (s.used && s.err == err) {
          slot = &s;
          break;
        }
      }
      if (!slot) {
        // 新的錯誤碼：用空的那一格，沒有空的就換掉最久沒印的
        slot = &slots_[0];
        for (auto &s : slots_) {
          if (!s.used) {
            slot = &s;
            break;
          }
          if (s.last_log < slot->last_log) {
            slot = &s;
          }
        }
        slot->used = true;
        slot->err = err;
        slot->repeated = 0;
        slot->last_log = now;
        return action_e::first;
      }

      ++slot->repeated;
      if (now - slot->last_log < std::chrono::seconds(1)) {
        return action_e::none;
      }
      repeated_out = slot->repeated;
      slot->repeated = 0;
      slot->last_log = now;
      return action_e::repeated;
    }

    /// 送出成功時呼叫（每一包都會經過）：沒有進行中的失敗時只有一次比較。回 true＝該印恢復行，
    /// unlogged_out＝還沒印出來的失敗次數。
    bool on_ok(std::uint32_t &unlogged_out) {
      if (!active_) {
        return false;
      }
      if (std::chrono::steady_clock::now() - last_fail_ < std::chrono::seconds(1)) {
        // 同一條執行緒上別的目的地還在失敗（多個 session 共用送出執行緒）：不要每包交替印失敗與恢復
        return false;
      }
      unlogged_out = 0;
      for (const auto &s : slots_) {
        if (s.used) {
          unlogged_out += s.repeated;
        }
      }
      slots_ = {};
      active_ = false;
      return true;
    }

  private:
    struct slot_t {
      bool used = false;
      int err = 0;
      std::uint32_t repeated = 0;
      std::chrono::steady_clock::time_point last_log {};
    };

    std::array<slot_t, 4> slots_ {};  // 同時在彙總的錯誤碼（同一條執行緒很少超過一兩種）
    std::chrono::steady_clock::time_point last_fail_ {};
    bool active_ = false;
  };

  enum class qos_data_type_e : int {
    audio,  ///< Audio
    video  ///< Video
  };

  /**
   * @brief Enable QoS on the given socket for traffic to the specified destination.
   * @param native_socket The native socket handle.
   * @param address The destination address for traffic sent on this socket.
   * @param port The destination port for traffic sent on this socket.
   * @param data_type The type of traffic sent on this socket.
   * @param dscp_tagging Specifies whether to enable DSCP tagging on outgoing traffic.
   */
  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging);

  /**
   * @brief Open a url in the default web browser.
   * @param url The url to open.
   */
  void open_url(const std::string &url);

  /**
   * @brief Attempt to gracefully terminate a process group.
   * @param native_handle The native handle of the process group.
   * @return `true` if termination was successfully requested.
   */
  bool request_process_group_exit(std::uintptr_t native_handle);

  /**
   * @brief Check if a process group still has running children.
   * @param native_handle The native handle of the process group.
   * @return `true` if processes are still running.
   */
  bool process_group_running(std::uintptr_t native_handle);

  input_t input();
  /**
   * @brief Get the current mouse position on screen
   * @param input The input_t instance to use.
   * @return Screen coordinates of the mouse.
   * @examples
   * auto [x, y] = get_mouse_loc(input);
   * @examples_end
   */
  util::point_t get_mouse_loc(input_t &input);
  void move_mouse(input_t &input, int deltaX, int deltaY);
  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y);
  void button_mouse(input_t &input, int button, bool release);
  void scroll(input_t &input, int distance);
  void hscroll(input_t &input, int distance);
  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags);
  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state);
  void unicode(input_t &input, char *utf8, int size);

  typedef deinit_t client_input_t;

  /**
   * @brief Allocate a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input);

  /**
   * @brief Send a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch);

  /**
   * @brief Send a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen);

  /**
   * @brief Send a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void gamepad_touch(input_t &input, const gamepad_touch_t &touch);

  /**
   * @brief Send a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void gamepad_motion(input_t &input, const gamepad_motion_t &motion);

  /**
   * @brief Send a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void gamepad_battery(input_t &input, const gamepad_battery_t &battery);

  /**
   * @brief Create a new virtual gamepad.
   * @param input The global input context.
   * @param id The gamepad ID.
   * @param metadata Controller metadata from client (empty if none provided).
   * @param feedback_queue The queue for posting messages back to the client.
   * @return 0 on success.
   */
  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);
  void free_gamepad(input_t &input, int nr);

  /**
   * @brief Get the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t get_capabilities();

  // mDNS service-instance name. SERVICE_TYPE stays as the standard
  // GameStream type so the host discovery is wire-compatible — VipleStream
  // is still a Moonlight-protocol implementation, just rebranded.
  constexpr auto SERVICE_NAME = "VipleStream";
  constexpr auto SERVICE_TYPE = "_nvstream._tcp";

  namespace publish {
    [[nodiscard]] std::unique_ptr<deinit_t> start();
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init();

  /**
   * @brief Returns the current computer name in UTF-8.
   * @return Computer name or a placeholder upon failure.
   */
  std::string get_host_name();

  /**
   * @brief Gets the supported gamepads for this platform backend.
   * @details This may be called prior to `platf::input()`!
   * @param input Pointer to the platform's `input_t` or `nullptr`.
   * @return Vector of gamepad options and status.
   */
  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input);

  struct high_precision_timer: private boost::noncopyable {
    virtual ~high_precision_timer() = default;

    /**
     * @brief Sleep for the duration
     * @param duration Sleep duration
     */
    virtual void sleep_for(const std::chrono::nanoseconds &duration) = 0;

    /**
     * @brief Check if platform-specific timer backend has been initialized successfully
     * @return `true` on success, `false` on error
     */
    virtual operator bool() = 0;
  };

  /**
   * @brief Create platform-specific timer capable of high-precision sleep
   * @return A unique pointer to timer
   */
  std::unique_ptr<high_precision_timer> create_high_precision_timer();

  /**
   * @brief VipleStream 2.0 §VR（M1b S1-01／S1-12）：VR 路徑共用的單調時鐘，目前的 tick 值。
   * @details 平台中立的宣告，讓 stream.cpp 的 tracking handler、vr_selftest 等共用程式碼在
   *          Linux／macOS 也能編譯（vr_session／vr_clock 是純模組，不直接呼叫它，tick 由
   *          呼叫端取好傳入）。實作依平台分開：
   *            - Windows：QueryPerformanceCounter 的原始值（QPC tick），與 driver 寫進 shm 的
   *              present_qpc／submit_qpc 等欄位同一個時基，可以直接相減。
   *            - Linux／macOS：CLOCK_MONOTONIC，單位 ns。
   *          tick 換成時間一律用整數公式，不要假設頻率等於 10 MHz。
   * @return 目前的 tick 值。
   */
  int64_t vr_clock_ticks();

  /**
   * @brief VipleStream 2.0 §VR：vr_clock_ticks() 每秒的 tick 數。
   * @return Windows 為 QueryPerformanceFrequency（快取值）；Linux／macOS 固定 1'000'000'000。
   */
  int64_t vr_clock_frequency();

  /**
   * @brief §VR-HOTSPOT-AUTO（2026-10-30）：自動把主機無線介面連上頭盔熱點。
   * @details Windows 上透過 Native Wi-Fi API (wlanapi) 挑選看得到該 SSID 且未被佔用的介面，
   *          設定 all-user profile 並連線；Linux / macOS 則記 log 並忽略。
   *          金鑰絕不輸出於任何 log。
   */
  void vr_hotspot_connect(uint8_t version, uint8_t auth_type, uint8_t band, uint8_t channel, std::string ssid, std::string key);

  /**
   * @brief §VR-HOTSPOT-WATCHDOG（2026-10-31）：VR session 每 10 s 呼叫一次，表示「串流還在」。
   * @details 看門狗只在最近 25 s 內有人呼叫時才監看熱點網卡；串流結束（或頭盔關機）就自然停手。
   *          Linux / macOS 為空函式。
   */
  void vr_hotspot_keepalive();

}  // namespace platf
