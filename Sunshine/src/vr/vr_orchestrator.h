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
    std::string vr_launch_url;  ///< `steam://launch/<id>/VR`
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
   * @return 已排入；已有編排進行中（非 IDLE／RESTORE_PENDING／ERROR）時 false。
   */
  bool start(const negotiated_t &neg, const app_ref_t &app, bool force);

  /// session 結束：STOPPING → RESTORE_PENDING（不關 SteamVR，§D.8）
  void stop(stop_reason_e reason);

  /// running() 用：狀態 1–13，或 ERROR 的前 10 s
  bool active();

  snapshot_t state();

  /// 最近一次 start() 的 VR 參數（/resume 比對用）；沒有回 nullopt
  std::optional<launch_params_t> current_params();

  /// 立刻跑一次維護（guard 還原、環境與衝突探測）；selftest 與登入事件用
  void kick();

  /// 等到狀態是 IDLE（或 timeout）；selftest 用
  bool wait_idle(std::chrono::milliseconds timeout);
}  // namespace vr::orchestrator
