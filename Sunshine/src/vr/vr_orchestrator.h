/**
 * @file src/vr/vr_orchestrator.h
 * @brief VipleStream 2.0 §VR M1b V5（S1-17、M1b 設計 §D）：pcvr session 的 SteamVR 編排器。
 *
 * 單例、一條背景執行緒（命令佇列＋維護 tick）。平台操作全部經 `vr::platform::*` 與 `vr::bridge::*`；
 * 讀使用者可寫檔案的動作都在這條執行緒上。HTTP 執行緒只讀 atomic 快照（`state()`、`active()`）。
 *
 * 狀態與逾時見設計 §D.2；每次轉換記 `[VIPLE-VR-ORCH] state <from> -> <to> ms=<在前一狀態的時間>`，
 * 每步結果記 `[VIPLE-VR-ORCH] step=<name> result=ok|fail reason=<…> ms=<…>`。
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// local includes
#include "vr_session.h"

namespace vr::orchestrator {
  enum class state_e : uint8_t {
    idle = 0,
    precheck,
    deploy,
    reg,
    conflict,
    quit_steamvr,
    guard,
    arm,
    launch_steamvr,
    wait_driver,
    wait_hmd,
    launch_app,
    active,
    driver_lost,
    stopping,
    restore_pending,
    error,
  };

  const char *state_name(state_e s);

  /// STATE 推送用的快照（stream.cpp 的 vr_control_tick 每 tick 比對 seq）
  struct snapshot_t {
    state_e state = state_e::idle;
    uint8_t stream_state = 0;  ///< VIPLE_VR_STATE_*
    uint8_t progress = 0;
    uint16_t code = 0;  ///< VIPLE_VR_STATE_CODE_*
    uint64_t seq = 0;  ///< 每次 (stream_state, progress, code) 改變就遞增
  };

  struct app_ref_t {
    std::string name;
    bool vr_class = false;  ///< SteamVR Home（不另外啟動遊戲）
    std::string vr_launch_url;  ///< `steam://launch/<id>/VR`，或 §VR-SHORTCUT 的 `steam://rungameid/<id>`
    /// §VR-SHORTCUT：Steam app id（空＝不是 Steam 遊戲）。padmode 認不出場景程式時拿來對 vr_shortcuts
    std::string steam_app_id;
    /// §VR-SHORTCUT：非空時以 `steam.exe -applaunch` 啟動；空則走 vr_launch_url
    std::string vr_launch_args;
  };

  enum class stop_reason_e {
    user_ended,
    terminated,
    selftest,
    shutdown,
  };

  /// server 啟動、`vr_pcvr=enabled` 時呼叫：起背景執行緒（重複呼叫無害）
  void init();

  /// server 結束：停止進行中的編排（disarm）、join
  void shutdown();

  /// 執行緒在跑（`vr_pcvr=enabled`）
  bool running();

  /**
   * @brief /launch（pcvr）成功後呼叫；背景執行緒從 PRECHECK 開始跑。
   * @param force vrForce：允許關掉現有 SteamVR（§D.8）
   * @param prewarm_token 這一次 /launch 自己呼叫 `prewarm()` 拿到的值；PRECHECK 只沿用那一次探測的環境。
   *        0（預設，selftest 用）＝沒有預先探測，PRECHECK 一律自己完整探測。
   * @return 已排入；已有編排進行中（非 IDLE／RESTORE_PENDING／ERROR）時 false。
   */
  bool start(const negotiated_t &neg, const app_ref_t &app, bool force, uint64_t prewarm_token = 0);

  /// §VR-SHORTCUT：這一場 /launch 的 Steam app id（空＝沒有）。start() 成功就設定，場次結束或啟動前被取消就清掉
  std::string session_steam_app_id();

  /// session 結束：STOPPING → RESTORE_PENDING（不關 SteamVR，§D.8）
  void stop(stop_reason_e reason);

  /// running() 用：狀態 1–13，或 ERROR 的前 10 s
  bool active();

  snapshot_t state();

  /// 最近一次 start() 的 VR 參數（/resume 比對用）；沒有回 nullopt
  std::optional<launch_params_t> current_params();

  /// 立刻跑一次維護（guard 還原、環境與衝突探測）；selftest 與登入事件用
  void kick();

  /**
   * @brief §PREWARM：/launch（pcvr）通過擁有權檢查、確定會走到 start() 之前呼叫（任何執行緒；只設旗標、叫醒編排執行緒）。
   *        編排執行緒先把環境探測一次，和呼叫端接下來的編碼器探測並行；把回傳值交給隨後的 start()，它的 PRECHECK
   *        才沿用這一次的結果，只補查隨時會變的項目。探測在編排執行緒上做——會讀使用者可寫的檔案，不可以放在 HTTP 執行緒。
   *        之後沒有 start()（/launch 中途失敗）也無妨：結果只認這個回傳值，別的 start() 拿不到。
   * @return 交給 start() 的 prewarm_token；0＝沒有安排探測（編排器沒在跑、或編排進行中）。
   */
  [[nodiscard]] uint64_t prewarm();

  /// 等到狀態是 IDLE（或 timeout）；selftest 用
  bool wait_idle(std::chrono::milliseconds timeout);

  /// `--steamvr-driver uninstall` 的結果
  struct unregister_result_t {
    bool ok = false;
    /// 失敗原因：not-running（編排器沒在跑＝vr_pcvr 停用）｜session-active｜busy｜steamvr-running｜
    /// guard-restore[:細節]｜openvrpaths-unreadable｜still-registered｜timeout｜in-progress｜server-stopping
    std::string reason;
    int found = 0;  ///< 註銷前 openvrpaths 裡的 viplestream 註冊數
    bool guard_restored = false;  ///< 這一次順便還原了 steamvr.vrsettings 的 guard
  };

  /**
   * @brief 註銷 SteamVR driver（vr_architecture §3.8 的註銷路徑；install 的反向：DEPLOY＋REGISTER）。
   *        在編排執行緒上做（會讀使用者可寫的檔案、以使用者身分跑 vrpathreg），呼叫端等結果（最多約 60 秒）。
   *        只在沒有編排進行中、而且 SteamVR 沒在跑時動手（和 REGISTER_FINALIZE 同一個條件；vrserver 在跑時
   *        guard 也還原不了）；SteamVR 在跑時回 `steamvr-running`、什麼都不改，要不要等由呼叫端決定。
   *        動作：還原 guard（有 marker 時）→ `vrpathreg removedriver` 每一個 viplestream 註冊 → 確認 openvrpaths 裡
   *        沒有了 → 刪 `<install>\config\steamvr\` 下的版本目錄（來源 `<install>\steamvr\` 不動）。
   *        不碰使用者其他的 SteamVR 設定。`vr_pcvr` 仍是 enabled 時，下一次 VR /launch 會重新部署並註冊。
   *        stop_requested：等待期間每秒呼叫一次（不持鎖），回 true＝呼叫端要停（server 關閉）：還沒開始做就撤回、回
   *        `server-stopping`。編排執行緒已經開始做（中止不了）時，停止或逾時都回 `in-progress`（之後照樣做完，結果寫在 log），
   *        做的期間新的呼叫一律回 `busy`。
   */
  unregister_result_t unregister_driver(const std::function<bool()> &stop_requested = {});

  /**
   * @brief SteamVR（vrserver）是不是在跑。給 `--steamvr-driver uninstall --wait-steamvr` 的等待迴圈用：
   *        `unregister_driver()` 回 `steamvr-running` 之後，等待期間只用這個查（唯讀、不進編排執行緒、不寫 step log），
   *        它結束了才再呼叫一次 `unregister_driver()`。任何執行緒都可以呼叫。
   */
  bool steamvr_running();
}  // namespace vr::orchestrator
