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

    /// §VR-LATCH-V2：vr_latch_mode。legacy（預設）＝M4a R3 原版；v2＝相位取模、anti-windup、目標 vr_latch_target_pct；
    /// v3（§VR-LATCH-V3）＝環狀中位數＋最短路徑、節拍閘門、積分保留並跨場沿用（src/vr/vr_latch.h）
    enum class latch_e {
      legacy,
      v2,
      v3,
    };

    latch_e latch_mode = latch_e::legacy;
    int latch_target_pct = 40;  ///< vr_latch_target_pct：v2 的目標 slack，佔週期的百分比（25–50）
    /// §VR-LATCH-V3：vr_latch_target_v3_pct。v3 的目標（25–65）；0（預設）＝沿用 vr_latch_target_pct。
    /// 50 以上會把影像往「早到」那一側推，要先看過 client 的早到分佈再用。
    int latch_target_v3_pct = 0;
    /// §VR-LATCH-V3：vr_latch_ab（預設空＝關）。開發用的同場 A/B：`<秒>:<模式>:<target%>`（例 `60:v2:50`），每隔這麼多秒
    /// 在設定的模式與這裡指定的模式之間切換一次。原樣字串，格式錯誤要到 VR session 建立時才警告（stream.cpp）。
    std::string latch_ab;
    /// §VR-LATCH-V3：vr_latch_acquire（disabled 預設；只在 vr_latch_mode = v3 時有作用）。enabled＝相位誤差大時把虛擬 vsync
    /// 的週期暫時改 ±vr_latch_acquire_ppm 一段算好的時間（burst），一次把相位移到位
    bool latch_acquire = false;
    int latch_acquire_ppm = 4000;  ///< vr_latch_acquire_ppm：burst 相對基準的週期修正量（1000–5000 ppm）
    /// §VR-RENDER-SCALE（2026-10-08）：vr_render_scale_pct。告訴 SteamVR 的建議算圖尺寸＝串流的每眼尺寸 × 這個百分比
    /// （100–250）。串流尺寸決定編碼、傳輸、解碼的負擔（也就是延遲），算圖尺寸決定畫面細節；Steam 內建串流是
    /// 算圖 2160、編碼寬度 1152。100＝舊行為（兩者相同）。改了要等 SteamVR 重啟才生效。
    int render_scale_pct = 100;
    /// §VR-DIRECT-KICK（2026-10-10）：vr_direct_kick（disabled 預設）。enabled＝host 直通落在「多一格」的時序（合成器送的是
    /// 遊戲上一格就交好的幀，多一個 host 週期的延遲）滿 3 秒時，bridge 自動讓 driver 的 PostPresent 立刻返回一小段
    /// （週期 × 1.25，夾在 20～40 ms），把它推回「等遊戲這一格」；之後確認有沒有成功，沒有就退避再試
    /// （判斷都在 src/vr/vr_direct_kick.h）。只在 host 的週期 ≥ 11 ms（約 90 Hz 以下，例如半速串流的 60 Hz）時動作。
    /// 開著時 WELCOME 會帶 dev 旗標（driver 才接受那個節拍模式）。只有 Windows 的 PCVR 會用到。
    bool direct_kick = false;
    /// §VR-A-KICK 的實驗（dev，2026-10-10）：vr_dev_kick_every_s（0 預設＝關，5–600）。每隔這麼多秒讓 driver 的 PostPresent
    /// 立刻返回 vr_dev_kick_ms 毫秒（1–200，預設 20），用來確認 host 直通的時序能不能由「多一格」推回「等遊戲這一格」
    /// （docs/steam_frame_client.md「host 直通時的兩種時序」）。開著時 WELCOME 會帶 dev 旗標（driver 才接受這個節拍模式）。
    int dev_kick_every_s = 0;
    int dev_kick_ms = 20;

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
    /// §VR-REPAIR-RTT（2026-10-10）：vr_multilink_repair_rtt（disabled 預設；要 vr_multilink_repair 也開）。enabled＝client
    /// 也支援時，補包的門檻、等待上限與重報由 client 照它量到的往返時間決定（只在一條連線送影像時），server 對這種回報
    /// 把年齡上限由三個幀週期放寬到 100 ms。原本這幾道上限都用幀週期訂，往返 10 ms 上下、120 Hz 時補包多半趕不上。
    /// 關著時（或 client 不支援）兩端的行為與之前完全相同。
    bool multilink_repair_rtt = false;

    /// 開發用：vr_multilink_fault = <linkId>:<on_ms>:<off_ms>[:<phase_ms>][,…]，在指定連線上週期性丟掉全部收送，
    /// 用來在單一網卡上模擬鏈路空檔。空字串＝關閉。
    std::string multilink_fault;

    /// §VR-FENCE-LATE（2026-10-09）：vr_capture_late（disabled 預設）。enabled＝擷取端等 driver 的 fence 逾時後不丟這一張，
    /// 繼續等到它到值或超過等待預算（晚到照收）；拆線改由「完成值 3 s 不動」「5 s 沒有一張複製成功」判定並有退避，
    /// 不再用「連續逾時滿 1 s」。disabled＝行為與之前相同（只多 log）。只有 Windows 的 PCVR 擷取會用到。
    bool capture_late = false;
    /// vr_capture_late_max_ms：一張影像從被叫醒起最多等多久（20–250 ms）；實際的等待預算是
    /// 3 個發布間隔，夾在 3 個顯示週期與這個值之間（src/vr/vr_capture_health.h）。
    /// 預設值要等於 vr::capture::k_max_wait_ms_default（config.cpp 有 static_assert 對住那個常數；要改兩邊一起改）
    int capture_late_max_ms = 100;
    /// §VR-CADENCE（2026-10-10）：vr_capture_overload（只在 vr_capture_late = enabled 時有作用）。過載時 LATCH 的頻率鎖與
    /// §VR-PREDICT 要不要取樣看哪個訊號。late（預設）＝過載旗標（晚到＋放棄 ≥ 30%），兩者都不取樣，決定和之前相同；
    /// cadence＝LATCH 只在合成器的 Present 鎖不住虛擬 vsync（driver 回報的跨格數）或放棄的影像 ≥ 30% 時不取樣，
    /// §VR-PREDICT 是過載旗標或鎖不住；節拍說不準時兩者都照過載旗標（src/vr/vr_present_cadence.h、vr_capture_health.h）。
    /// 兩種模式下節拍都照算、照印（late 是影子模式）
    enum class overload_e {
      late,
      cadence,
    };

    overload_e capture_overload = overload_e::late;
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
