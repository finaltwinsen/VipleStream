/**
 * @file src/vr/vr_bridge.h
 * @brief VipleStream 2.0 §VR M1b（S1-07）：server 端 `vr_bridge`——與 SteamVR driver（vrserver.exe 內）
 *        之間的 pipe、共享記憶體、ring 貼圖與握手。
 *
 * 權威設計：M1b 設計 §B（ABI、seqlock、握手、狀態機、fence 規則、驗證）、K2／K3／K5／K26。
 * 線上格式的單一定義來源是同目錄的 vr_ipc_abi.h（driver 與 vr_probe 以相對路徑 include 同一個檔）。
 *
 * 實作依平台分開：
 *   - Windows：src/platform/windows/vr_bridge_win.cpp
 *   - Linux／macOS：src/platform/linux/vr_stub.cpp（全部回「不支援」，不建任何物件）
 *
 * 不變式 5：`vr_pcvr == disabled` 時沒有人呼叫 start()，bridge 完全閒置（沒有執行緒、沒有 pipe），
 * `tracking_wanted()` 恆為 false，publish_tracking() 立即返回；一般（vrFlags==0）session 不受影響。
 *
 * 平台：vr_ipc_abi.h 只支援 x64（seqlock 依賴 x64 TSO），在其他架構會 #error。這個標頭會被所有平台都編的
 * stream.cpp 與 Linux／macOS 的 vr_stub.cpp include，所以只在 x64 才 include ABI；用到 vripc_* 型別的宣告
 * 以 VIPLE_VR_BRIDGE_HAS_ABI 包住。非 x64（例：aarch64 Linux、Apple Silicon）只剩平台中立的狀態與生命週期 API，
 * tracking_wanted() 恆為 false。
 *
 * log 衛生（§S1-02）：這個模組的 log 一律不印 handle 值、完整 GUID、完整 SID（只印 RID，例 `sid=*1001`）。
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>

// local includes
#if defined(_M_X64) || defined(__x86_64__)
  #define VIPLE_VR_BRIDGE_HAS_ABI 1
  #include "vr_ipc_abi.h"
#else
  #define VIPLE_VR_BRIDGE_HAS_ABI 0
#endif

#if defined(_WIN32) && VIPLE_VR_BRIDGE_HAS_ABI
// 只宣告指標／參照型別；需要實際呼叫的地方（display_vr_t、vr_bridge_win.cpp）自己 include <d3d11_4.h>／<Windows.h>
struct ID3D11Fence;
struct _FILETIME;
#endif

namespace vr::bridge {

  /// adapter LUID（平台中立；Windows 端與 LUID 的 LowPart／HighPart 一對一）
  struct luid_t {
    uint32_t low = 0;
    int32_t high = 0;

    bool operator==(const luid_t &) const = default;

    bool is_zero() const {
      return low == 0 && high == 0;
    }
  };

  /// bridge 目前所在的階段（§B.5 狀態機，加上 pipe 建立前的幾個前置狀態）
  enum class phase_e : uint32_t {
    stopped,  ///< start() 沒呼叫或已 stop()
    no_pipe,  ///< pipe 還沒建立（session 不符、非 SYSTEM、被搶名……見 status_t::pipe_reason，每 30 s 重試）
    no_user,  ///< pipe 已建立，DACL 只有 SY；沒有主控台使用者
    backoff,  ///< 握手失敗後的退避（1、2、4、8、10 s），期間不 ConnectNamedPipe
    listening,  ///< ConnectNamedPipe pending
    verifying,  ///< client 已連入，正在做 §B.1 第 1–7 步
    welcomed,  ///< WELCOME 已送（idle generation 停在這裡；有 config 時等 READY）
    ready,  ///< 收到 READY：frame_source() 就緒
    teardown,  ///< 正在拆除
  };

  inline const char *phase_name(phase_e p) {
    switch (p) {
      case phase_e::stopped:
        return "stopped";
      case phase_e::no_pipe:
        return "no-pipe";
      case phase_e::no_user:
        return "no-user";
      case phase_e::backoff:
        return "backoff";
      case phase_e::listening:
        return "listening";
      case phase_e::verifying:
        return "verifying";
      case phase_e::welcomed:
        return "welcomed";
      case phase_e::ready:
        return "ready";
      case phase_e::teardown:
        return "teardown";
    }
    return "?";
  }

  /**
   * @brief 目前狀態的快照（status() 每次回傳一份複本）。
   *
   * driver 寫的欄位（driver_state、other_hmd、act_* 等）是 driver_status 區塊「整份複製＋§B.8 第 9 條驗證」
   * 後的本地副本；它們由同一個使用者控制，只供診斷、衝突判斷與 K20 參數比對參考（§B.8 第 11 條）。
   */
  struct status_t {
    bool running = false;  ///< bridge 執行緒在跑
    phase_e phase = phase_e::stopped;
    std::string pipe_reason = "not-started";  ///< ok｜not-system｜squatted｜session-mismatch｜error-<n>｜not-started｜unsupported
    uint32_t self_session = 0xFFFFFFFFu;  ///< ProcessIdToSessionId(self)
    uint32_t console_session = 0xFFFFFFFFu;  ///< 最近一次查到的 WTSGetActiveConsoleSessionId()
    bool user_present = false;  ///< pipe DACL 已加上主控台使用者的 logon SID ACE
    uint32_t user_rid = 0;  ///< 主控台使用者 SID 的 RID（只供 log，例 1001）

    bool connected = false;  ///< 握手完成（WELCOME 已送）到 TEARDOWN 之間
    bool peer_is_selftest = false;  ///< 這個連線是 allow_selftest_peer() 放行的 vr_probe ipcpeer
    uint32_t peer_pid = 0;
    uint64_t generation = 0;  ///< 目前（或最近一次）generation；每次成功握手 +1，從 1 起
    bool has_config = false;  ///< 這個 generation 帶 session config（不是 idle generation）
    bool ready = false;  ///< 收到 READY
    bool hmd_presenting = false;  ///< driver 回報 HMD_PRESENTING 之後、STANDBY 之前
    bool hmd_added = false;  ///< driver 回報過 HMD_ADDED（本連線）
    bool armed = false;  ///< 目前要求的 arm 狀態（header.armed）
    bool dev_mode = false;  ///< WELCOME 帶 VRIPC_WF_DEV_PACING（只在 selftest）
    uint32_t driver_state = 0;  ///< VRIPC_DRV_*；不在允許集合內時為 0（未知）
    uint32_t other_hmd = 0;  ///< VRIPC_OTHER_HMD_*；0 = VRIPC_OTHER_HMD_UNKNOWN（下方 static_assert 鎖定）
    std::string other_hmd_system;  ///< 已強制 NUL 結尾、只留可列印字元、'[' 換成 '?'
    uint32_t driver_version_packed = 0;  ///< HELLO：(major<<24)|(minor<<16)|patch
    uint32_t driver_caps = 0;  ///< HELLO：VRIPC_DCAP_*
    std::string driver_iface;  ///< HELLO：iface_directmode（已過濾）
    int64_t last_driver_heartbeat_qpc = 0;  ///< 最近一次在合理範圍內的 driver heartbeat
    int64_t last_server_heartbeat_qpc = 0;  ///< bridge 最近一次寫進 header 的 server heartbeat（selftest 量 heartbeat 有沒有中斷）
    bool act_valid = false;  ///< act_* 通過 §B.8 第 9 條（否則視為「參數未知」）
    uint32_t act_refresh_mhz = 0;
    uint32_t act_eye_w = 0;
    uint32_t act_eye_h = 0;
    luid_t act_luid;
    uint32_t degraded_code = 0;

    uint64_t handshakes = 0;  ///< 成功握手次數
    uint64_t teardowns = 0;
    uint64_t rejects = 0;  ///< 握手失敗（含不是本協定而直接斷線的連線）
    uint32_t reject_streak = 0;  ///< 連續失敗次數（成功一次歸零）
    uint32_t last_reject_reason = 0;  ///< VRIPC_REJ_*；0 = 沒有送 REJECT（非本協定）
    std::string last_teardown_reason;
    bool abi_mismatch_seen = false;  ///< 曾收到 ABI 不同的 HELLO（編排器 → STATE code 3）
    uint32_t peer_abi_version = 0;  ///< 最近一次 HELLO 的 abi_version

    uint64_t trk_published = 0;  ///< 寫進 tracking ring 的樣本
    uint64_t trk_dropped = 0;  ///< 非有限數值等原因丟棄的樣本
    uint64_t ring_corrupt = 0;  ///< SPSC ring 的倒退／越界次數（本連線）
    uint64_t drv_log_lines = 0;
    uint64_t drv_log_suppressed = 0;
    uint64_t haptic_rx = 0;
    uint64_t haptic_dropped = 0;
    uint64_t timing_rx = 0;
  };

  /**
   * @brief 累計計數（selftest 以前後兩次快照相減判定；全部自 server 啟動起單調遞增，不因連線結束歸零）。
   */
  struct counters_t {
    uint64_t handshakes_ok = 0;  ///< WELCOME 完整送達的次數
    uint64_t teardowns = 0;
    uint64_t rejects[16] {};  ///< 以 VRIPC_REJ_* 為索引；[0] = 不是本協定（或 client 行程檢查失敗）而直接斷線、沒有送 REJECT
    uint64_t byes_sent[8] {};  ///< 以 VRIPC_BYE_* 為索引（送出嘗試，含寫入失敗）
    uint64_t states_sent = 0;  ///< server → driver 的 STATE 送出嘗試（寫入之前計數，含寫入失敗的那一則）
    uint32_t last_identity = 0;  ///< 最近一次成功握手的身分：0 無、1 vrserver、2 selftest peer
    uint64_t driver_status_invalid = 0;  ///< driver_status 快照有欄位不合 §B.8 第 9 條（視為 UNKNOWN）的次數
    uint64_t ring_corrupt = 0;  ///< SPSC ring 倒退／越界的累計次數
    uint32_t reject_streak = 0;  ///< 目前連續失敗的握手數
    uint64_t textures_failed = 0;  ///< server 端 ring／fence 建立或複製失敗
    uint64_t driver_hung = 0;
  };

  counters_t counters();

  /// §B.1 第 7 步（sec-m18）：連續握手失敗的退避秒數；第 n 次失敗後等 k_reject_backoff_sec[min(n, 5) − 1] 才重新 ConnectNamedPipe
  inline constexpr int64_t k_reject_backoff_sec[] = {1, 2, 4, 8, 10};

  /// 連續失敗 streak 次之後的退避秒數（streak == 0 → 0）；bridge 與 selftest T2.rate-limit 共用這一份
  constexpr int64_t reject_backoff_sec(uint32_t streak) {
    constexpr uint32_t n = static_cast<uint32_t>(sizeof(k_reject_backoff_sec) / sizeof(k_reject_backoff_sec[0]));
    return streak == 0 ? 0 : k_reject_backoff_sec[(streak - 1 < n ? streak - 1 : n - 1)];
  }

  /// bridge 事件（在 bridge 執行緒上呼叫 sink；sink 不可阻塞，也不可呼叫會等 bridge 執行緒的 API）
  enum class event_e : uint32_t {
    pipe_state,  ///< pipe 建立成功或失敗（text = pipe_reason）
    handshake,  ///< WELCOME 已送（a = driver_version_packed，b = 1 表示 selftest peer）
    ready,  ///< 收到 READY
    teardown,  ///< text = 原因
    rejected,  ///< a = VRIPC_REJ_*（0 = 非本協定、沒有送 REJECT），text = 原因
    abi_mismatch,  ///< a = 對方的 abi_version（編排器 → STATE code 3）
    driver_state,  ///< driver → server 的 STATE：a = VRIPC_ST_*，b = arg
    driver_hung,  ///< driver heartbeat 停了 > 2 s 而 pipe 仍開
    textures_failed,  ///< ring／fence 建立或複製失敗：a = HRESULT 或 Win32 錯誤碼
  };

  inline const char *event_name(event_e e) {
    switch (e) {
      case event_e::pipe_state:
        return "pipe-state";
      case event_e::handshake:
        return "handshake";
      case event_e::ready:
        return "ready";
      case event_e::teardown:
        return "teardown";
      case event_e::rejected:
        return "rejected";
      case event_e::abi_mismatch:
        return "abi-mismatch";
      case event_e::driver_state:
        return "driver-state";
      case event_e::driver_hung:
        return "driver-hung";
      case event_e::textures_failed:
        return "textures-failed";
    }
    return "?";
  }

  struct event_t {
    event_e kind = event_e::pipe_state;
    uint64_t generation = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    std::string text;
  };

  using event_cb = std::function<void(const event_t &)>;

#if VIPLE_VR_BRIDGE_HAS_ABI
  static_assert(VRIPC_OTHER_HMD_UNKNOWN == 0, "status_t::other_hmd 的初值假設 UNKNOWN == 0");

  using haptic_cb = std::function<void(const vripc_haptic_evt_t &)>;
  using timing_cb = std::function<void(const vripc_timing_rec_t &)>;

  /**
   * @brief publish_tracking() 的輸入：一筆 tracking 樣本（欄位與 vripc_tracking_slot_t 的 payload 一對一）。
   *
   * 時間一律是 server 的 raw tick（platf::vr_clock_ticks()；Windows = QPC）。`target_server_qpc` 由呼叫端算好：
   * 網路來源 = 時鐘對映（§F.1）後的 sampleTime + predictNs；selftest 合成來源 = now + predict。
   */
  struct tracked_sample_t {
    uint32_t sample_id = 0;
    uint16_t space_epoch = 0;
    uint8_t flags = 0;  ///< 0x5506 flags（VIPLE_VR_TRK_*）
    uint64_t sample_time_ns = 0;  ///< client 時鐘，只供診斷
    int64_t arrival_qpc = 0;
    int64_t target_server_qpc = 0;
    uint32_t predict_ns = 0;
    vripc_pose_t pose[3] {};  ///< HMD、L、R
    vripc_ctrl_input_t input[2] {};
    uint16_t gaze_yaw_f16 = 0;
    uint16_t gaze_pitch_f16 = 0;
    uint8_t gaze_conf = 0;
    uint8_t gaze_flags = 0;
  };
#endif  // VIPLE_VR_BRIDGE_HAS_ABI

  // ── 生命週期 ────────────────────────────────────────────────────────────

  /**
   * @brief server 啟動、而且 `vr_pcvr != disabled` 時呼叫（重複呼叫無害）。
   *
   * 起 bridge 執行緒；執行緒內依 §B.1 建 pipe（DACL 只有 SY，不等使用者登入）。自身 session 與主控台 session
   * 不同、非 SYSTEM（例：console 模式）、被搶名時 fail closed，每 30 s 重試。
   * @return 執行緒已在跑（或本來就在跑）為 true；`vr_pcvr == disabled` 或平台不支援為 false。
   */
  bool start();

  /**
   * @brief server 結束時呼叫：送 BYE(SERVER_SHUTDOWN)、拆除、join。所有 pipe IO 都是 overlapped 且有上限。
   */
  void stop();

  /// 目前狀態的複本（任何執行緒）
  status_t status();

  // ── 編排器／selftest 的控制 ───────────────────────────────────────────────

#if VIPLE_VR_BRIDGE_HAS_ABI
  /**
   * @brief 設定 session config。與目前 generation 的 config 不同時送 BYE(RECONFIG)，driver 立刻重連、
   *        拿到新 generation 與新 ring（generation 不變式：同一 generation 內 config 不變）。
   *
   * 會先做基本檢查：eye_width／eye_height 在 1–8192、packed == 2×eye（sbs）、dxgi_format == 87、
   * refresh_mhz 在 [60000, 240000]、LUID 非 0、universe_id ≤ UINT32_MAX。沒有 dev mode 時 dev_flags／dev_arg 清成 0。
   * @return 檢查通過為 true。
   */
  bool set_session_config(const vripc_session_config_t &config);
#endif

  /// 清掉 session config；目前 generation 帶 config 時送 BYE(RECONFIG)，driver 重連成 idle generation
  void clear_session_config();

  /// header.armed + STATE ARM／DISARM（連線中才送 STATE；下一個 generation 的 header 以這裡為初值）
  void set_armed(bool armed);

  /// STATE REQUEST_QUIT（driver 端只在自己的 HMD 存在時才送 VREvent_DriverRequestedQuit）
  void request_steamvr_quit();

#if VIPLE_VR_BRIDGE_HAS_ABI
  /**
   * @brief 更新 pacing（單格 seqlock，由 bridge 執行緒寫入）。沒有 dev mode 時 mode 強制 0；
   *        slew 夾到 VRIPC_PACING_SLEW_PPM_MAX。新的 session config 會清掉之前設的 pacing
   *        （新 generation 先寫依 config Hz 算的預設週期）。
   */
  void set_pacing(const vripc_pacing_t &pacing);

  /**
   * @brief M4a R3：LATCH 頻率鎖。以目前 session config 的 Hz（沒有就沿用上一個 session）算標稱週期，
   *        乘上 (1 + ppm·10⁻⁶) 後以 production 模式寫 pacing（epoch 不變，driver 不重新對齊相位）。
   *        ppm 夾在 ±200。
   */
  void set_pacing_ppm(int32_t ppm);
#endif

  /**
   * @brief selftest 的 dev mode：之後的 WELCOME 帶 VRIPC_WF_DEV_PACING，允許 pacing.mode ≠ 0、
   *        config.dev_flags／dev_arg 與 STATE DEV_SET_V2P。production 一律 false。
   */
  void set_dev_mode(bool enabled);

  /// §VR-PREDICT：STATE SET_V2P（連線中才送；不需 dev mode）。@return 有排入送出為 true
  bool send_set_v2p(uint32_t vsync_to_photons_us);

  /// 登入／登出／解鎖／換人：立刻重查主控台使用者並以 SetSecurityInfo 換掉 pipe 的使用者 ACE（平時每 5／30 s 輪詢）
  void on_user_session_changed();

  /**
   * @brief 只在 selftest T2 期間：放行 selftest 以 run_command 啟動的 vr_probe ipcpeer（§B.1 第 5 步例外）。
   *
   * 以傳入的行程 handle 取 PID、建立時間與映像；映像必須等於 `<install>\tools\vr_probe\vr_probe.exe`。
   * 連線時 PID、建立時間、映像三者都相符才以 `identity=selftest-peer` 通過（token 檢查照做）。
   * @param child_process_handle Windows 的行程 HANDLE（例：bp::child::native_handle()），至少要有
   *        PROCESS_QUERY_LIMITED_INFORMATION；bridge 會自己複製一份，呼叫端照常管理自己的 handle。
   * @return 映像與 handle 檢查通過為 true。
   */
  bool allow_selftest_peer(std::uintptr_t child_process_handle);

  /// 結束 selftest peer 的放行（T2 結束時一定要呼叫）
  void revoke_selftest_peer();

  /**
   * @brief 設定「合法 driver 宿主」的映像路徑（`<runtime>\bin\win64\vrserver.exe`，取自 probe_environment 快取）。
   *        §B.1 第 5 步以不分大小寫、斜線正規化後的字串比對；不開檔。沒設（空字串）時只有 selftest peer 能通過。
   */
  void set_expected_driver_host_image(const std::wstring &path);

  /**
   * @brief 要求拆除指定 generation（消費端偵測到 driver 端問題時用）：送 BYE(bye_reason) 後 TEARDOWN。
   *        generation 已經不是目前的就忽略。常見：VRIPC_BYE_DEVICE_LOST（sharedFence 讀到 UINT64_MAX 而自己的
   *        device 沒被移除）、VRIPC_BYE_PROTOCOL_ERROR（連續 30 筆不合格、fence 逾時累計 ≥ 1 s）。
   */
  void request_teardown(uint64_t generation, uint32_t bye_reason);

  void set_event_sink(event_cb cb);
#if VIPLE_VR_BRIDGE_HAS_ABI
  void set_haptic_sink(haptic_cb cb);  ///< 已夾值（duration ≤ 2 s、0–1000 Hz、amplitude 0–1）與限速（每手 ≤ 200 則/s）
  void set_timing_sink(timing_cb cb);  ///< selftest CSV 用；bridge 執行緒上呼叫，必須很快
#endif

  // ── tracking（K26：單一寫入臨界區）─────────────────────────────────────────

  /**
   * @brief 目前有沒有帶 config 的 generation 在收 tracking（lock-free，給 0x5506 handler 當快速閘門）。
   *        `vr_pcvr == disabled`、stub 模式沒有 session config、非 x64 平台時恆為 false。
   */
  bool tracking_wanted();

#if VIPLE_VR_BRIDGE_HAS_ABI
  /**
   * @brief 寫一筆 tracking 樣本進本 generation 的 tracking ring（per-slot seqlock）並 SetEvent(evtTrk)。
   *
   * 內部以 writer mutex 序列化：0x5506 handler 會在 ENet control 執行緒與 picoquic IO 執行緒上跑，selftest 合成
   * 來源是第三個呼叫者（K26、§B.3）。沒有帶 config 的 generation 時立即返回。任何 pose／速度不是有限數值的樣本
   * 丟棄並計數（避免 NaN 進 vrserver）。只碰 bridge 自己的鎖，不碰呼叫端的任何鎖。
   */
  void publish_tracking(const tracked_sample_t &sample);
#endif

  // ── selftest 診斷 ─────────────────────────────────────────────────────────

  /**
   * @brief T0：讀自己 VR pipe 的安全描述元（owner、DACL、label），選配做一次 SetSecurityInfo 來回（驗 WRITE_DAC）。
   * @details 讀與寫回都在 bridge 的 pipe_mtx_ 內完成：refresh_user() 換使用者 ACE 也持同一把鎖，
   *          所以來回不會把換 ACE 之前的舊 DACL 寫回去（selftest 不可自己拿 pipe 複本做 SetSecurityInfo）。
   */
  struct pipe_sd_report_t {
    bool supported = false;
    bool pipe_exists = false;
    std::string pipe_reason;  ///< 同 status_t::pipe_reason
    uint32_t self_session = 0xFFFFFFFFu;
    uint32_t console_session = 0xFFFFFFFFu;
    bool owner_is_system = false;
    bool dacl_protected = false;
    uint32_t ace_count = 0;
    bool system_ace = false;
    uint32_t system_ace_mask = 0;
    bool user_ace = false;  ///< 有一個非 SY 的允許 ACE
    uint32_t user_ace_mask = 0;  ///< 期望 0x12019b
    bool user_ace_is_logon_sid = false;  ///< 該 ACE 的 SID 是 S-1-5-5-x-y（logon SID）
    bool user_ace_matches_console = false;  ///< 等於目前主控台使用者的 logon SID
    bool label_present = false;
    uint32_t label_rid = 0;  ///< 期望 0x2000（Medium）
    uint32_t label_policy = 0;  ///< 期望 NO_WRITE_UP|NO_READ_UP（0x3）
    bool roundtrip_attempted = false;
    bool roundtrip_ok = false;
    uint32_t roundtrip_error = 0;
    uint32_t query_error = 0;
  };

  pipe_sd_report_t check_pipe_security(bool set_security_roundtrip);

  /**
   * @brief T2 用的 server 端消費者（V3 的 display_vr_t live 迴圈之前的替身）：在呼叫端執行緒上跑 duration，
   *        以 frame_source() 的 NT handle 在自己建的 device 上開 ring／fence（與 display_vr_t 同一條路徑），
   *        照 §B.7 做 fence 確認（CPU、2 ms）、CopyResource、Signal(consumedFence)、Flush，並量延遲。
   *        不 GPU-Wait 對方的 fence（不變式 8）。同一時間只允許一個呼叫者。
   */
  struct consume_report_t {
    bool supported = false;
    bool ok = false;  ///< 有開成功至少一個 generation
    std::string error;  ///< ok == false 時的原因
    uint64_t generations_opened = 0;
    uint64_t evt_wakes = 0;
    uint64_t evt_timeouts = 0;
    uint64_t frames_copied = 0;
    uint64_t frames_skipped = 0;  ///< latest-wins 跳過的幀（frame_id 跳號）
    uint64_t frames_fence_timeout = 0;
    uint64_t frames_invalid = 0;
    uint64_t frames_torn = 0;
    uint64_t frames_stale = 0;  ///< present_qpc 早於 now − 100 ms
    uint64_t frames_fence_lost = 0;  ///< sharedFence 讀到 UINT64_MAX
    uint64_t teardown_requests = 0;
    uint64_t last_frame_id = 0;
    uint64_t frames_echo_matched = 0;  ///< 驗證後仍帶 ECHO_MATCHED（echo 在 server 最近 2 s 發布過的集合內，§B.8 第 4 條）
    uint64_t frames_echo_fallback = 0;  ///< echo 不在集合內，被改成 POSE_FALLBACK
    double fence_timeout_teardown_ms = 0;  ///< 第一次因「fence 逾時累計 ≥ 1 s」要求拆除時，連續逾時已持續的時間（0 = 沒發生）
    uint64_t fence_timeout_first_completed = 0;  ///< 第一次 fence 逾時當下 GetCompletedValue() 讀到的值（U7：記錄行為）
    uint64_t fence_timeout_first_expected = 0;  ///< 同一筆 descriptor 的 fence_value
    double submit_to_visible_p50_ms = 0;  ///< descriptor 的 submit_qpc → server CPU 看到 fence 到值
    double submit_to_visible_p95_ms = 0;
    double submit_to_visible_max_ms = 0;
    double copy_p50_ms = 0;  ///< CopyResource + Signal + Flush → GPU 完成（event query）
    double copy_p95_ms = 0;
    double copy_max_ms = 0;
  };

  consume_report_t selftest_consume(std::chrono::milliseconds duration, const std::function<bool()> &should_stop);

  // ── 純函式（兩個平台都可用；selftest T6 可直接測）───────────────────────────

  /// frame descriptor 不合格的原因（§B.8 第 2–6 條）
  enum class frame_invalid_e : uint32_t {
    none,
    generation,
    frame_id,
    fence,
    tex_idx,
    flags,
    layers,
    pose,
    time,
    epoch,
  };

  inline const char *frame_invalid_name(frame_invalid_e e) {
    switch (e) {
      case frame_invalid_e::none:
        return "none";
      case frame_invalid_e::generation:
        return "generation";
      case frame_invalid_e::frame_id:
        return "frame-id";
      case frame_invalid_e::fence:
        return "fence";
      case frame_invalid_e::tex_idx:
        return "tex-idx";
      case frame_invalid_e::flags:
        return "flags";
      case frame_invalid_e::layers:
        return "layers";
      case frame_invalid_e::pose:
        return "pose";
      case frame_invalid_e::time:
        return "time";
      case frame_invalid_e::epoch:
        return "epoch";
    }
    return "?";
  }

  /// check_frame_desc() 的比對基準（消費端的本地狀態）
  struct frame_check_ctx_t {
    uint64_t generation = 0;
    bool have_last_frame = false;  ///< false：本 generation 第一筆，frame_id 只要求非 0
    uint64_t last_frame_id = 0;
    uint64_t last_fence = 0;  ///< 上一筆已接受的 fence 值（第一筆 = shared_fence_initial）
    int64_t now_qpc = 0;
    int64_t qpc_frequency = 0;  ///< 時間窗 = ±1 s
    uint8_t layout_epoch = 0;
    uint16_t space_epoch = 0;
  };

#if VIPLE_VR_BRIDGE_HAS_ABI
  /**
   * @brief §B.8 第 2–6 條（不含第 4 條 echo 集合，那一條只改旗標、不算不合格）。
   *        輸入必須是 seqlock 複製出來的本地副本；這裡不碰共享記憶體。
   */
  inline frame_invalid_e check_frame_desc(const vripc_frame_desc_t &d, const frame_check_ctx_t &c) {
    if (d.generation != c.generation) {
      return frame_invalid_e::generation;
    }
    if (c.have_last_frame) {
      if (d.frame_id <= c.last_frame_id || d.frame_id - c.last_frame_id > 1024) {
        return frame_invalid_e::frame_id;
      }
    } else if (d.frame_id == 0) {
      return frame_invalid_e::frame_id;
    }
    if (d.fence_value <= c.last_fence || d.fence_value - c.last_fence > 1024) {
      return frame_invalid_e::fence;
    }
    if (d.tex_idx >= VRIPC_TEX_COUNT) {
      return frame_invalid_e::tex_idx;
    }
    if ((d.flags & ~0x12Fu) != 0) {
      return frame_invalid_e::flags;
    }
    if (d.layer_count > 16) {
      return frame_invalid_e::layers;
    }
    double p2 = 0, q2 = 0;
    for (float v : d.render_pos) {
      if (!std::isfinite(v)) {
        return frame_invalid_e::pose;
      }
      p2 += (double) v * v;
    }
    for (float v : d.render_rot) {
      if (!std::isfinite(v)) {
        return frame_invalid_e::pose;
      }
      q2 += (double) v * v;
    }
    const double qn = std::sqrt(q2);
    if (qn < 1.0 - 1e-3 || qn > 1.0 + 1e-3 || p2 > 100.0 * 100.0) {
      return frame_invalid_e::pose;
    }
    const int64_t win = c.qpc_frequency > 0 ? c.qpc_frequency : 1;
    for (int64_t t : {d.t_target_qpc, d.present_qpc, d.submit_qpc}) {
      if (t < c.now_qpc - win || t > c.now_qpc + win) {
        return frame_invalid_e::time;
      }
    }
    if (d.layout_epoch != c.layout_epoch || d.space_epoch != c.space_epoch) {
      return frame_invalid_e::epoch;
    }
    return frame_invalid_e::none;
  }
#endif  // VIPLE_VR_BRIDGE_HAS_ABI

  /// server 自己的版號：(major<<24)|(minor<<16)|patch（與 HELLO 的 openvr_sdk_packed 同一種打包）
  constexpr uint32_t pack_version(uint32_t major, uint32_t minor, uint32_t patch) {
    return ((major & 0xFFu) << 24) | ((minor & 0xFFu) << 16) | (patch & 0xFFFFu);
  }

#if defined(_WIN32) && VIPLE_VR_BRIDGE_HAS_ABI
  // ── Windows 專用：selftest 的接縫 ─────────────────────────────────────────

  /**
   * @brief 設計 §S1-07 的原始簽章：放行 selftest 以 run_command 啟動的 vr_probe ipcpeer。
   * @param child_process 行程 HANDLE；nullptr = 結束放行（同 revoke_selftest_peer()）。
   * @param created 呼叫端記錄的建立時間；不為 0 時必須與 bridge 自己用 GetProcessTimes 讀到的相同。
   * @return 放行成功（或 nullptr 的撤銷）為 true。
   */
  bool allow_selftest_peer(void *child_process, const _FILETIME &created);

  /**
   * @brief T0／T2 用：bridge 目前的 pipe、本 generation 的 shm 與兩個 event 在 **server 行程內** 的
   *        最小權限複本（HANDLE；呼叫端負責 CloseHandle）：pipe = READ_CONTROL|FILE_READ_ATTRIBUTES，
   *        shm／event = READ_CONTROL。只夠讀安全描述元與名稱——沒有 WRITE_DAC，selftest 無法繞過 bridge 的
   *        pipe_mtx_ 改 DACL（SetSecurityInfo 來回請用 check_pipe_security(true)），也不能做 IO、map 或 Signal。
   *        沒有 pipe／沒有連線時對應欄位為 nullptr。handle 值絕不寫進 log。
   */
  struct selftest_objects_t {
    void *pipe = nullptr;
    void *shm = nullptr;
    void *evt_trk = nullptr;
    void *evt_frm = nullptr;
  };

  selftest_objects_t duplicate_objects_for_selftest();

  // ── Windows 專用：給 display_vr_t（V3）與 selftest 消費者 ───────────────────

  /**
   * @brief 本 generation 的幀來源（READY 之後才有）。所有 handle 都是 **server 行程內** 的值、由 bridge 建立並持有；
   *        frame_source_t 活著就有效（內部以 shared_ptr 保住 generation 資源，包括共享記憶體 view）。
   *        消費端以 OpenSharedResource1／OpenSharedFence 在自己的 capture device 上開啟（同行程），開完不必關這些 handle。
   *        handle 值絕不寫進 log。
   */
  struct frame_source_t {
    uint64_t generation = 0;
    luid_t luid;  ///< ring／fence 所在的 adapter（== session config 的 LUID）
    uint32_t width = 0;  ///< packed_width
    uint32_t height = 0;  ///< packed_height
    uint32_t dxgi_format = VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM;
    uint32_t refresh_mhz = 0;
    uint8_t layout_epoch = 0;
    uint16_t space_epoch = 0;
    uint64_t shared_fence_initial = 0;  ///< 第一幀的 fence_value 必須 > 它
    std::array<void *, VRIPC_TEX_COUNT> tex_nt {};  ///< HANDLE：ring 貼圖（DXGI_SHARED_RESOURCE_READ|WRITE）
    void *shared_fence_nt = nullptr;  ///< HANDLE：driver GPU Signal、server CPU 觀察
    void *consumed_fence_nt = nullptr;  ///< HANDLE：server GPU Signal、driver CPU 觀察
    void *evt_frm = nullptr;  ///< HANDLE：driver 每發布一幀 SetEvent（auto-reset）；消費端只等待

    /// generation 仍是目前的（TEARDOWN 後 false；消費端看到 false 就重新呼叫 frame_source()）
    bool alive() const;

    std::shared_ptr<void> impl;  ///< 內部：generation 資源（bridge 專用）
  };

  /// READY 之後本 generation 的幀來源；還沒 READY、沒有 config、或已 TEARDOWN 時為 nullptr
  std::shared_ptr<frame_source_t> frame_source();

  /**
   * @brief 目前設定的 session config 的 adapter LUID（M1b S1-08、§C.2 第 1 步）：display_vr_t 在 driver 還沒
   *        READY 時也要在同一張卡建 device（server 先選 LUID，driver 照做）。沒有 session config 時 nullopt。
   */
  std::optional<luid_t> session_config_luid();

  /**
   * @brief frame ring 的單一消費者輔助（§B.3 讀取方＋§B.8 copy-once-validate）。只能由一條執行緒使用。
   *
   * read_latest() 只取最新一筆（latest-wins）：seqlock 最多試 3 次，失敗算 torn、本輪視為沒有新資料；
   * 成功就在本地副本上做 §B.8 驗證；echo_sample_id 不在 server 最近 2 s 發布過的集合內 → 清 ECHO_MATCHED、
   * 設 POSE_FALLBACK（不算不合格）。連續 30 筆不合格 → 自動 request_teardown(generation, BYE_PROTOCOL_ERROR)。
   */
  class frame_reader_t {
  public:
    enum class result_e {
      none,  ///< 沒有新資料（write_index 沒動或為 0）
      ok,  ///< out 是已驗證的本地副本
      invalid,  ///< 不合格（last_invalid() 說明原因）；不可使用 out
      torn,  ///< seqlock 3 次都讀到寫入中／撕裂
    };

    explicit frame_reader_t(std::shared_ptr<frame_source_t> src);

    result_e read_latest(vripc_frame_desc_t &out);

    /**
     * @brief 開啟後、第一筆 read_latest() 之前呼叫：以 consumedFence 目前的完成值當 fence 基準。
     *        同一個 generation 裡換消費者（例：encoder 重建後新的 display_vr_t、T2 連跑兩次）時，driver 的 fence
     *        早已超過 shared_fence_initial + 1024，沒有基準的話第一筆會被判成不合格。已讀到第一筆之後呼叫無效。
     */
    void set_fence_baseline(uint64_t consumed_completed_value) {
      if (!have_last_frame_ && consumed_completed_value != UINT64_MAX) {
        last_fence_ = consumed_completed_value;
      }
    }

    uint64_t last_skipped() const {
      return last_skipped_;
    }

    uint64_t last_frame_id() const {
      return last_frame_id_;
    }

    uint64_t last_fence() const {
      return last_fence_;
    }

    /// 上一筆 ok 的 echo 樣本在 server 發布到 tracking ring 的 QPC（ECHO_MATCHED 才有；否則 0）。
    /// present_qpc − 它 ＝ 延遲預算第 3＋4＋5 項（server 收到 → app 取 pose → render → Present），S3-09 用。
    int64_t last_echo_published_qpc() const {
      return last_echo_pub_qpc_;
    }

    frame_invalid_e last_invalid() const {
      return last_invalid_;
    }

    /**
     * @brief §VR-RING-RELEASE：只為了把 slot 交還 driver 而讀最新一筆，內容不可使用。
     *
     * 只驗 generation 與 fence（> 目前基準、差距 ≤ 1024），不驗時間與 pose：換消費者時 ring 裡最新一筆可能已過期
     * 超過 1 s（§B.8 時間窗），read_latest() 會判不合格、不交還 → driver 三個 slot 全被佔住、不再發布，消費端也
     * 等不到新幀（2026-10-03 VR /resume 只剩 10 fps 重送幀）。read_latest() 判不合格而沒交還的幀也走這裡。
     * @return 要 Signal 到 consumedFence 的值；沒有需要交還的（已交還、generation 不符、fence 不合理）回 0。
     */
    uint64_t release_latest();

    /// 寫 consume 區塊（server 寫入方；只供診斷，driver 以 consumedFence 為準）
    void publish_consume(const vripc_consume_status_t &status);

    uint64_t invalid_total = 0;
    uint64_t torn_total = 0;
    uint64_t skipped_total = 0;

  private:
    std::shared_ptr<frame_source_t> src_;
    uint64_t last_write_index_ = 0;
    uint64_t last_frame_id_ = 0;
    bool have_last_frame_ = false;
    uint64_t last_fence_ = 0;
    uint64_t last_skipped_ = 0;
    int64_t last_echo_pub_qpc_ = 0;
    uint32_t invalid_streak_ = 0;
    bool teardown_requested_ = false;
    frame_invalid_e last_invalid_ = frame_invalid_e::none;
  };

  /**
   * @brief §B.7 的 CPU 端 fence 確認：GetCompletedValue → 未到值時最多一個 SetEventOnCompletion 登記（auto-reset event，
   *        每個 generation 一個 waiter）→ 等 timeout_ms → 醒來一律重讀。UINT64_MAX 回 lost（呼叫端先查自己的 device
   *        是否被移除，不是自己的就 request_teardown(generation, VRIPC_BYE_DEVICE_LOST)）。絕不 GPU Wait。
   */
  class fence_waiter_t {
  public:
    enum class result_e {
      reached,
      timeout,
      lost,  ///< GetCompletedValue() == UINT64_MAX
      error,  ///< event 建立失敗等
    };

    fence_waiter_t();
    ~fence_waiter_t();
    fence_waiter_t(const fence_waiter_t &) = delete;
    fence_waiter_t &operator=(const fence_waiter_t &) = delete;

    result_e wait(ID3D11Fence *fence, uint64_t value, uint32_t timeout_ms, uint64_t *observed = nullptr);

  private:
    void *event_ = nullptr;  ///< HANDLE
    uint64_t registered_ = 0;  ///< 尚未到值的登記（0 = 沒有）
  };
#endif
}  // namespace vr::bridge
