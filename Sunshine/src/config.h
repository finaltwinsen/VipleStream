/**
 * @file src/config.h
 * @brief Declarations for the configuration of Sunshine.
 */
#pragma once

// standard includes
#include <bitset>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// local includes
#include "nvenc/nvenc_config.h"

namespace config {
  // track modified config options
  // VipleStream §S1-02（K25）：這裡只存「可以寫進 log 的值」（loggable_value 的結果），
  // 機密鍵存的是 `<redacted len=N>`。它只用於 main.cpp 在 logging::init() 之後重印，
  // 設定解析一律用 apply_config 收到的 vars 原值，不從這裡讀。
  inline std::unordered_map<std::string, std::string> modified_config_settings;

  /**
   * @brief VipleStream §S1-02（K25）：設定鍵是否為機密（值一律不得進 log 或 stdout）。
   *
   * 判定不分大小寫：完全等於 `relay_psk`、`relay_url`，或以 `_psk`、`_password`、
   * `_secret`、`_token`、`_key` 結尾。
   */
  bool is_secret_key(std::string_view name);

  /**
   * @brief VipleStream §S1-02（K25）：回傳可以寫進 log 的值。
   *
   * 機密鍵回 `<redacted len=N>`（N＝原值的位元組數），其餘鍵回原值。已經是這個佔位字串的值
   * 原樣返回（冪等），所以 main.cpp 重印 modified_config_settings 時再套一次，長度也不會
   * 變成佔位字串自己的長度。
   */
  std::string loggable_value(std::string_view name, std::string_view value);

  // VipleStream §CFG.defer: logging::init() 在 config::parse() 之後才跑
  // （main.cpp 的 parse 在 init 之前幾行），在那之前 BOOST_LOG 只會流向 Boost 的
  // 隱含 console sink，永遠進不了 sunshine.log（以 Windows 服務執行時等於完全看不到）。
  // 設定解析期間產生的警告先存進這個緩衝區，由 main.cpp 在 logging::init() 之後補播 ——
  // 做法與上面的 modified_config_settings 一致。
  //
  // 不變條件：只在啟動時的主執行緒（config::parse -> apply_config）寫入，補播後清空，
  // 因此不需要加鎖。日後若讓 Web UI 執行緒也呼叫 apply_config，這裡要改成有鎖的容器。
  inline std::vector<std::string> deferred_config_warnings;

  struct video_t {
    // ffmpeg params
    int qp;  // higher == more compression and less quality

    int hevc_mode;
    int av1_mode;

    int min_threads;  // Minimum number of threads/slices for CPU encoding

    struct {
      std::string sw_preset;
      std::string sw_tune;
      std::optional<int> svtav1_preset;
    } sw;

    nvenc::nvenc_config nv;
    bool nv_realtime_hags;
    bool nv_opengl_vulkan_on_dxgi;
    bool nv_sunshine_high_power_mode;

    struct {
      int preset;
      int multipass;
      int h264_coder;
      int aq;
      int vbv_percentage_increase;
    } nv_legacy;

    struct {
      std::optional<int> qsv_preset;
      std::optional<int> qsv_cavlc;
      bool qsv_slow_hevc;
    } qsv;

    struct {
      std::optional<int> amd_usage_h264;
      std::optional<int> amd_usage_hevc;
      std::optional<int> amd_usage_av1;
      std::optional<int> amd_rc_h264;
      std::optional<int> amd_rc_hevc;
      std::optional<int> amd_rc_av1;
      std::optional<int> amd_enforce_hrd;
      std::optional<int> amd_quality_h264;
      std::optional<int> amd_quality_hevc;
      std::optional<int> amd_quality_av1;
      std::optional<int> amd_preanalysis;
      std::optional<int> amd_vbaq;
      int amd_coder;
    } amd;

    struct {
      int vt_allow_sw;
      int vt_require_sw;
      int vt_realtime;
      int vt_coder;
    } vt;

    struct {
      bool strict_rc_buffer;
    } vaapi;

    struct {
      int tune;  // 0=default, 1=hq, 2=ll, 3=ull, 4=lossless
      int rc_mode;  // 0=driver, 1=cqp, 2=cbr, 4=vbr
    } vk;

    std::string capture;
    std::string encoder;
    std::string adapter_name;
    std::string output_name;

    struct dd_t {
      struct workarounds_t {
        std::chrono::milliseconds hdr_toggle_delay;  ///< Specify whether to apply HDR high-contrast color workaround and what delay to use.
      };

      enum class config_option_e {
        disabled,  ///< Disable the configuration for the device.
        verify_only,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_active,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_primary,  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
        ensure_only_display  ///< @seealso{display_device::SingleDisplayConfiguration::DevicePreparation}
      };

      enum class resolution_option_e {
        disabled,  ///< Do not change resolution.
        automatic,  ///< Change resolution and use the one received from Moonlight.
        manual  ///< Change resolution and use the manually provided one.
      };

      enum class refresh_rate_option_e {
        disabled,  ///< Do not change refresh rate.
        automatic,  ///< Change refresh rate and use the one received from Moonlight.
        manual  ///< Change refresh rate and use the manually provided one.
      };

      enum class hdr_option_e {
        disabled,  ///< Do not change HDR settings.
        automatic  ///< Change HDR settings and use the state requested by Moonlight.
      };

      struct mode_remapping_entry_t {
        std::string requested_resolution;
        std::string requested_fps;
        std::string final_resolution;
        std::string final_refresh_rate;
      };

      struct mode_remapping_t {
        std::vector<mode_remapping_entry_t> mixed;  ///< To be used when `resolution_option` and `refresh_rate_option` is set to `automatic`.
        std::vector<mode_remapping_entry_t> resolution_only;  ///< To be use when only `resolution_option` is set to `automatic`.
        std::vector<mode_remapping_entry_t> refresh_rate_only;  ///< To be use when only `refresh_rate_option` is set to `automatic`.
      };

      config_option_e configuration_option;
      resolution_option_e resolution_option;
      std::string manual_resolution;  ///< Manual resolution in case `resolution_option == resolution_option_e::manual`.
      refresh_rate_option_e refresh_rate_option;
      std::string manual_refresh_rate;  ///< Manual refresh rate in case `refresh_rate_option == refresh_rate_option_e::manual`.
      hdr_option_e hdr_option;
      std::chrono::milliseconds config_revert_delay;  ///< Time to wait until settings are reverted (after stream ends/app exists).
      bool config_revert_on_disconnect;  ///< Specify whether to revert display configuration on client disconnect.
      mode_remapping_t mode_remapping;
      workarounds_t wa;
    } dd;

    int max_bitrate;  // Maximum bitrate, sets ceiling in kbps for bitrate requested from client
    double minimum_fps_target;  ///< Lowest framerate that will be used when streaming. Range 0-1000, 0 = client's requested framerate.
  };

  struct audio_t {
    std::string sink;  ///< Audio output device/sink to use for audio capture
    std::string virtual_sink;  ///< Virtual audio sink for audio routing
    bool stream;  ///< Enable audio streaming to clients
    bool install_steam_drivers;  ///< Install Steam audio drivers for enhanced compatibility
  };

  constexpr int ENCRYPTION_MODE_NEVER = 0;  // Never use video encryption, even if the client supports it
  constexpr int ENCRYPTION_MODE_OPPORTUNISTIC = 1;  // Use video encryption if available, but stream without it if not supported
  constexpr int ENCRYPTION_MODE_MANDATORY = 2;  // Always use video encryption and refuse clients that can't encrypt

  struct stream_t {
    std::chrono::milliseconds ping_timeout;

    std::string file_apps;

    int fec_percentage;

    // Video encryption settings for LAN and WAN streams
    int lan_encryption_mode;
    int wan_encryption_mode;

    // VipleStream: NAT traversal
    std::string stun_server;   // default: "stun.l.google.com"
    std::string relay_url;     // signaling relay WebSocket URL (empty = disabled)
    std::string relay_psk;     // relay pre-shared key

    // VipleStream: UDP tunnel preference for this server.
    //   "auto"       — try direct → relay UDP → relay WS (default)
    //   "direct"     — never use relay for streaming (classic P2P only)
    //   "udp_tunnel" — always use relay UDP (useful for known-broken P2P paths)
    //   "ws_tunnel"  — always use relay WS (fallback when both direct and UDP fail)
    std::string udp_tunnel_mode;

    // VipleStream: MP-QUIC multipath transport
    bool mpquic_enabled = false;
    int mpquic_port = 48010;
    int mpquic_scheduler = 0;     // 0=auto, 1=min_rtt, 2=aggregate, 3=redundant, 4=ecf
    int mpquic_fec_floor = 1;     // minimum FEC % when QUIC is active (LAN mode)
    int mpquic_congestion = 1;    // 0=newreno, 1=bbr (default, bandwidth-based), 2=cubic
    int abr_floor_kbps = 1500;    // §ABR-FLOOR-DERP: AIMD 絕對下限（窄路徑 2-3Mbps goodput 要餵得飽）

    // §F7 送出 pacing：true = 依串流碼率 ×1.25 攤平每幀封包的送出節拍（見
    // stream.cpp [VIPLE-SMOOTH-PACING]），false = 上游的 ~800 Mbps 線速 burst。
    // 預設 false，等同舊版「沒設 VIPLE_SMOOTH_PACING 環境變數」的行為。
    // 環境變數 VIPLE_SMOOTH_PACING 只保留為 dev-only 覆寫（優先於這裡的值），
    // 在 apply_config 解析完 config 後套用，所以本欄位存的是最終生效值。
    bool smooth_pacing = false;
  };

  /**
   * @brief VipleStream 2.0 §VR：PCVR 設定（docs/vr_architecture.md §3.4、§3.6）。
   *
   * 預設 disabled：VR session 一律回 VR_DISABLED，/serverinfo 不宣告 PCVR。
   * M1a 只有 stub（開發驗證用：沒有 SteamVR driver，VR session 直接擷取目前桌面、
   * 把 client 的 tracking 樣本回填進每一幀）。M1b 會再加 auto／enabled。
   */
  struct vr_t {
    enum class pcvr_e {
      disabled,  ///< 不接受 VR session（預設）
      stub,  ///< M1a 開發驗證：桌面擷取＋tracking 回聲
      enabled,  ///< M1b V5（S1-11）：真正的 PCVR（SteamVR driver＋display_vr_t＋編排器）
    };

    pcvr_e pcvr = pcvr_e::disabled;  ///< vr_pcvr
    int intra_refresh_frames = 8;  ///< vr_intra_refresh_frames：LOSS 觸發的 intra-refresh wave 長度（2–60 幀）
    int intra_refresh_safety_ms = 2000;  ///< vr_intra_refresh_safety_ms：週期性 intra refresh 安全網（0 = 關閉；否則 500–10000 ms）
    /// vr_vsync_to_photons_us：HMD 的 Prop_SecondsFromVsyncToPhotons（µs）。SteamVR 依它決定遊戲的姿態要預測到多久之後。
    /// 0（預設）＝自動（§VR-PREDICT）：session 從「一個顯示週期＋30 ms」或上一個 session 學到的值開始，再依 client 回報的
    /// 姿態落後調整；1000–200000＝固定、不調。
    int vsync_to_photons_us = 0;

    /// §VR-LATCH-V2：vr_latch_mode。legacy（預設）＝M4a R3 原版；v2＝相位取模、anti-windup、目標 vr_latch_target_pct
    enum class latch_e {
      legacy,
      v2,
    };

    latch_e latch_mode = latch_e::legacy;
    int latch_target_pct = 40;  ///< vr_latch_target_pct：v2 的目標 slack，佔週期的百分比（25–50）

    /// §VR-CTRL-OFFSET：vr_ctrl_pose_offset（disabled 預設）。enabled＝控制器也設 poseTimeOffset（driver pose_flags bit0）
    bool ctrl_pose_offset = false;

    /// §VR-ANGVEL-LOCAL：vr_angvel_local（disabled 預設）。enabled＝driver 把 client 的世界座標角速度轉成機體座標再交給
    /// SteamVR（pose_flags bit2；SteamVR 把 vecAngularVelocity 當本地座標，10-05 vr_probe --mode predict 實測）
    bool angvel_local = false;

    /// §VR-STALE-HOLD：vr_stale_policy。legacy（預設）＝超過 2T 沒新樣本就速度與 offset 歸零；
    /// hold＝100 ms 內的空窗保留速度與 offset（driver pose_flags bit1）
    enum class stale_e {
      legacy,
      hold,
    };

    stale_e stale_policy = stale_e::legacy;

    /// §VR-MULTILINK：vr_multilink。disabled（預設）＝單一路徑，行為與之前相同；
    /// all＝client 的每張網卡各一條連線，影像、音訊、追蹤每條各送一份（送不動的連線會自動暫停送影像）；
    /// primary＝連線照建、追蹤與音訊每條都送，但影像只走 session 本身那條（其餘待命）。
    /// 只有 client 也支援時才會啟用（/launch 協商）。
    enum class multilink_e {
      disabled,
      all,
      primary,
      automatic,  ///< auto：session 本身那條先送，其餘量到夠快才送影像；送不動的退回量測
      always,  ///< always：每條都一直送，送不完的批次過期就丟，不暫停任何一條（2026-10-07 對照 Steam 的做法）
    };

    multilink_e multilink = multilink_e::disabled;

    /// §VR-LINK-CTRL：vr_multilink_ctrl（disabled 預設）。enabled＝client 也支援時，時間敏感的控制訊息走連線：
    /// client 的 LOSS／LATCH 在每條已確認的連線各送一份，server 的 REFRESH_START 也是。session 位址那條鏈路
    /// 變弱時，掉幀的恢復不再被它拖住（2026-10-07 頭盔實測：走開後 LOSS 送不到，退回 IDR 花了 832 ms）。
    bool multilink_ctrl = false;
    /// §VR-LINK-REPAIR：vr_multilink_repair（disabled 預設；要 vr_multilink_ctrl 也開）。enabled＝server 保留最近
    /// 幾幀已加密的封包，client 回報某個 FEC block 缺哪些 shard 時原樣補送。
    bool multilink_repair = false;

    /// 開發用：vr_multilink_fault = <linkId>:<on_ms>:<off_ms>[:<phase_ms>][,…]，在指定連線上週期性丟掉全部收送，
    /// 用來在單一網卡上模擬鏈路空檔。空字串＝關閉。
    std::string multilink_fault;
  };

  struct nvhttp_t {
    // Could be any of the following values:
    // pc|lan|wan
    std::string origin_web_ui_allowed;

    std::string pkey;
    std::string cert;

    std::string sunshine_name;

    std::string file_state;

    std::string external_ip;
  };

  struct input_t {
    std::unordered_map<int, int> keybindings;

    std::chrono::milliseconds back_button_timeout;
    std::chrono::milliseconds key_repeat_delay;
    std::chrono::duration<double> key_repeat_period;

    std::string gamepad;
    bool ds4_back_as_touchpad_click;
    bool motion_as_ds4;
    bool touchpad_as_ds4;
    bool ds5_inputtino_randomize_mac;

    bool keyboard;
    bool mouse;
    bool controller;

    bool always_send_scancodes;

    bool high_resolution_scrolling;
    bool native_pen_touch;
  };

  namespace flag {
    enum flag_e : std::size_t {
      PIN_STDIN = 0,  ///< Read PIN from stdin instead of http
      FRESH_STATE,  ///< Do not load or save state
      FORCE_VIDEO_HEADER_REPLACE,  ///< force replacing headers inside video data
      UPNP,  ///< Try Universal Plug 'n Play
      CONST_PIN,  ///< Use "universal" pin
      FLAG_SIZE  ///< Number of flags
    };
  }  // namespace flag

  struct prep_cmd_t {
    prep_cmd_t(std::string &&do_cmd, std::string &&undo_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)),
        undo_cmd(std::move(undo_cmd)),
        elevated(std::move(elevated)) {
    }

    explicit prep_cmd_t(std::string &&do_cmd, bool &&elevated):
        do_cmd(std::move(do_cmd)),
        elevated(std::move(elevated)) {
    }

    std::string do_cmd;
    std::string undo_cmd;
    bool elevated;
  };

  struct sunshine_t {
    std::string locale;
    int min_log_level;
    std::bitset<flag::FLAG_SIZE> flags;
    std::string credentials_file;

    std::string username;
    std::string password;
    std::string salt;

    std::string config_file;

    struct cmd_t {
      std::string name;
      int argc;
      char **argv;
    } cmd;

    std::uint16_t port;
    std::string address_family;
    std::string bind_address;

    std::string log_file;
    bool notify_pre_releases;
    bool system_tray;
    std::vector<prep_cmd_t> prep_cmds;

    // List of allowed origins for CSRF protection (e.g., "https://example.com,https://app.example.com")
    // Comma-separated list of additional origins. Default includes localhost variants and web UI port.
    std::vector<std::string> csrf_allowed_origins;
  };

  extern video_t video;
  extern audio_t audio;
  extern stream_t stream;
  extern vr_t vr;
  extern nvhttp_t nvhttp;
  extern input_t input;
  extern sunshine_t sunshine;

  int parse(int argc, char *argv[]);
  std::unordered_map<std::string, std::string> parse_config(const std::string_view &file_content);
}  // namespace config
