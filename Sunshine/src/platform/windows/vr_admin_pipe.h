/**
 * @file src/platform/windows/vr_admin_pipe.h
 * @brief VipleStream §VR（M1b S1-18、§F.5）：只給提升管理員用的本機 admin pipe，以及連它的 CLI。
 *
 * server 端（SYSTEM service）：`\\.\pipe\VipleStreamAdmin`，
 *   SD = `O:SYG:SYD:P(A;;GA;;;SY)(A;;0x12019b;;;BA)S:(ML;;NWNR;;;HI)`，`FILE_FLAG_FIRST_PIPE_INSTANCE`、
 *   `PIPE_REJECT_REMOTE_CLIENTS`、`nMaxInstances=1`、message mode、overlapped；instance 整個 server 生命週期不關
 *   （斷線用 DisconnectNamedPipe 後再 ConnectNamedPipe，不留搶名窗口）。
 *   非提升的行程在 kernel 的 CreateFileW 就被 DACL（只有 BA）＋ `ML HI`（擋 medium）擋下；server 讀到第一則
 *   訊息後再以 ImpersonateNamedPipeClient 確認 BA 成員＋ TokenElevation（深度防禦）。
 * CLI 端（`--vr-selftest`、`--vr-status`、`--vr-abort`、`--steamvr-driver`）：以
 *   `SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION` 開 pipe（搶名者拿不到可模擬的管理員 token），
 *   先驗 pipe 擁有者是 SYSTEM 才送指令；把 server 回的每一行印到 stdout（CLI 模式的 log 是 sunshine-cli.log）。
 *
 * 協定：UTF-8 JSON，一則 pipe 訊息一個物件，≤ 4 KiB。
 *   請求：`{"v":1,"cmd":"vr-selftest","args":{…}}`、`{"v":1,"cmd":"vr-status"}`、`{"v":1,"cmd":"abort"}`、
 *         `{"v":1,"cmd":"steamvr-driver","args":{"op":"status"}}`
 *   回應：`{"t":"started","run":"<utc>"}`（attached）或 `{"t":"accepted","run":"<utc>"}`（--detach，之後 server 關連線）、
 *         多則 `{"t":"line","text":"[VIPLE-VR-SELFTEST] …"}`、最後 `{"t":"result","rc":0,"pass":n,"fail":m,"notRun":k,…}`。
 * 執行模型（sec-M13）：指令在背景工作執行緒跑，pipe 執行緒保持一個 pending read 以偵測斷線；
 *   attached 模式 CLI 斷線＝abort（跑完清理）；`--detach` 之後以 `--vr-status` 取回進度與結果。
 *   同一時間只允許一個 selftest（pipe 也只有一個 instance）。
 * 結果持久化（§F.5、ops-m17）：每次執行都寫 `<install>\config\steamvr\selftest\<run>\summary.json`（SYSTEM 寫、
 *   目錄與檔案的 SD 同 §D.11 第 2 點）：開跑時 state=running，跑完（含 abort）覆寫成 state=done＋全部輸出行。
 *   service 重啟之後記憶體裡沒有紀錄時，`--vr-status` 讀最近一份（`source=file`）；仍是 running 的回報
 *   `state=interrupted`、rc=rc_aborted（前一個 server 在執行中結束，沒有結果）。
 */
#pragma once

// standard includes
#include <memory>

// platform includes
#include <Windows.h>

// local includes
#include "src/platform/common.h"

namespace platf::vr_admin {
  /**
   * @brief server 模式啟動 VR 服務：`vr::bridge::start()`（VR pipe）與 admin pipe。
   * @details 只在 `vr_pcvr != disabled` 時呼叫（不變式 5：disabled 時 VR 程式碼完全閒置）。
   *          `vr::bridge::start()` 回 false 時 admin pipe 照樣啟動（診斷用）。
   *          回傳物件解構時依序：中止進行中的 selftest（最多等 15 s）→ 停 admin pipe → `vr::bridge::stop()`
   *          （不論 start() 的結果都會呼叫；bridge 被搶名時可能留著重試執行緒）。
   *          admin pipe 建立失敗不影響回傳值（fail closed：沒有 admin pipe＝CLI 連不上）：
   *          非 SYSTEM（console 模式，ERROR_INVALID_OWNER）只記一次 `reason=not-system`；
   *          被搶名（ACCESS_DENIED／PIPE_BUSY）記 `pipe-squatted` 並每 30 s 重試。
   * @return 生命週期守衛；呼叫端在 shutdown 時 reset。
   */
  std::unique_ptr<platf::deinit_t> start_services();

  /**
   * @brief selftest T0 用：admin pipe server 端 handle 的最小權限複本（`READ_CONTROL|FILE_READ_ATTRIBUTES`，只夠讀 SD 與名稱）。
   * @return handle（呼叫端負責 CloseHandle）；pipe 還沒建立時 nullptr。
   */
  HANDLE duplicate_admin_pipe_for_selftest();

  // ── CLI（main.cpp 的 cmd_to_func；在 logging::init 之後呼叫，log 寫 sunshine-cli.log）──

  /// `--vr-selftest [--only …] [--detach] [--probe-path <vr_probe.exe>] …`
  int cli_vr_selftest(int argc, char **argv);

  /// `--vr-status`：取回進行中或最近一次 selftest 的輸出與結果
  int cli_vr_status(int argc, char **argv);

  /// `--vr-abort`：中止進行中的 selftest（主要給 `--detach` 的執行用）
  int cli_vr_abort(int argc, char **argv);

  /// `--steamvr-driver status|uninstall [--wait-steamvr S]`（V2：status 只有 bridge 狀態；uninstall 到 V5 才實作）
  int cli_steamvr_driver(int argc, char **argv);
}  // namespace platf::vr_admin
