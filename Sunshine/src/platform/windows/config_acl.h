/**
 * @file src/platform/windows/config_acl.h
 * @brief VipleStream [VIPLE-SEC]：收緊 config 目錄內機密檔案的 DACL（只留 SYSTEM 與 Administrators）。
 *
 * 背景（M1b S1-02 追加，使用者 2026-09-28 核准）：安裝目錄的 config\ 繼承 Program Files 的 ACL，
 * `sunshine.conf`（明文 relay PSK）、conf 備份、`sunshine_state.json`、舊 log 都是 `BUILTIN\Users:(RX)`，
 * 任何本機使用者都讀得到。server 以 SYSTEM 啟動時，把下列**檔案**的安全描述元改成
 * `O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)`（protected、沒有 Users）：
 *   - `<config>\sunshine.conf`，以及其他所有 `<config>\sunshine*.conf*`（sunshine.conf 以外的都視為備份：
 *     `.bak`、`.bak-<ts>`、`.bak.<ts>`、`.old`、`.orig`、Explorer 的「sunshine - Copy.conf」等）
 *   - `<config>\sunshine_state.json`（Web UI 帳號、salt、單次 SHA-256 的密碼雜湊，以及配對狀態）；
 *     `credentials_file`／`file_state` 改過檔名但仍在 config 頂層時，以實際檔名為準
 *   - `<config>\sunshine*.log*`（含 `sunshine-cli.log`）、`<config>\viplestream-svc.log`
 *   - `<config>\credentials\` 底下的每一個檔案（含私鑰）
 * **不動任何目錄自己的 DACL**（`config\steamvr` 之後要讓使用者可以進入），也不跟隨 reparse point、
 * 不處理有多個 hard link 的檔案。放到 config 目錄以外、或檔名不是 sunshine 開頭的 conf 備份不在範圍內；
 * `credentials_file`／`file_state` 設到 config 以外時也不動（那是使用者自選的位置）。
 *
 * `<config>\webui_port`（只有 `sunshine.port` 的十進位值，不含機密）刻意**不**收緊：conf 收緊後，
 * 開始功能表捷徑的 `--shortcut` 行程（UAC 過濾後的 token）讀不到 conf，要靠這個檔知道 Web UI 的 port。
 *
 * 之後才產生的檔案：
 *   - server 自己的 log（`sunshine.log`／`sunshine-cli.log`）由 prepare_log_file 在 logging::init()
 *     截斷式開檔之前先以同一個 SD 建立，沒有「先繼承 Users:RX、再收緊」的窗口；
 *   - `viplestream-svc.log` 由 viplestream-svc.exe 以同一個 SD 建立（tools/sunshinesvc.cpp）；
 *   - `sunshine_state.json` 第一次由 `--creds`／配對建立之前不存在，建立後在下一次 server 啟動時收緊；
 *     之後 save_user_creds／nvhttp save_state 以 pt::write_json 截斷寫入，SD 會保留；
 *   - 其他人（管理員以 Copy-Item 做的 conf 備份等）建立的檔案，在下一次 server 啟動時收緊。
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace platf::config_acl {
  /// SYSTEM server 發佈 Web UI 基準 port 的檔名（位於 config 目錄頂層、Users 可讀）
  inline constexpr wchar_t kWebUiPortFileName[] = L"webui_port";

  /**
   * @brief 在 logging::init() **之前**呼叫：先以 protected SD 建立（或就地收緊）即將開啟的 log 檔。
   *
   * 只處理位於 `config_dir` 頂層的 log 檔（`log_path` 設到別處時不動）。身分規則：
   *   - SYSTEM：完整 SD（owner/group = SYSTEM）；
   *   - 提升的管理員（例如 SSH 下執行的 CLI 指令）：只設 protected DACL（改 owner 需要 SeRestorePrivilege），
   *     而且只在 `sunshine.conf` 已經被收緊過時才做——代表這個 config 目錄由 SYSTEM service 管理；
   *     開發機上以管理員身分跑 portable 建置時不會把 log 鎖起來；
   *   - 其他身分：什麼都不做。
   * logging 還沒初始化，所以這個函式不寫 log；結果在 tighten_config_dir 的統計行一起計入。
   *
   * @param config_dir `platf::appdata()`（`<install>\config`）。
   * @param log_file 即將交給 logging::init() 的 log 檔路徑。
   */
  void prepare_log_file(const std::filesystem::path &config_dir, const std::filesystem::path &log_file);

  /**
   * @brief server 啟動時收緊 config 目錄內機密檔案的 DACL；只有以 SYSTEM 執行時才動作（冪等）。
   *
   * 範圍見檔頭。結束時寫一行 `[VIPLE-SEC] config-acl tightened files=N skipped=M`：
   *   N＝這次啟動改了 SD 的檔案數（含 prepare_log_file 先處理的 log 檔），
   *   M＝在範圍內但沒有改的檔案數（本來就合規，或因 reparse／hard link／錯誤而拒絕）。
   * 拒絕或失敗的檔案另寫一行 warning（只有類別與原因，不印路徑與檔名）。
   *
   * @param config_dir `platf::appdata()`（`<install>\config`）。
   * @param extra_secret_files 其他要當成 state 類收緊的檔案（`config::sunshine.credentials_file`、
   *        `config::nvhttp.file_state`）；只處理位於 `config_dir` 頂層的。平台層不 include config.h，
   *        由呼叫端傳入。
   */
  void tighten_config_dir(const std::filesystem::path &config_dir, std::span<const std::filesystem::path> extra_secret_files);

  /**
   * @brief SYSTEM server 啟動時把 `sunshine.port` 寫到 `<config>\webui_port`（先寫暫存檔再 rename）。
   *
   * sunshine.conf 收緊之後，以一般使用者身分執行的 `--shortcut`（開始功能表捷徑）讀不到 conf，
   * 會落回預設 port；它改讀這個檔（read_published_webui_port）。檔案沿用 config 目錄繼承的 ACL
   * （Users:RX），內容不含機密。非 SYSTEM 時不動作。成功只寫 debug，失敗寫一行 warning。
   *
   * @param config_dir `platf::appdata()`（`<install>\config`）。
   * @param port `config::sunshine.port`（基準 port，Web UI = port + 1）。
   */
  void publish_webui_port(const std::filesystem::path &config_dir, std::uint16_t port);

  /**
   * @brief 讀 `<config>\webui_port`；檔案不存在、讀不到、或內容不是 1～65535 的十進位整數時回 nullopt。
   *
   * 給 `--shortcut` 在 sunshine.conf 讀不到時使用；在 logging::init() 之前呼叫，不寫 log。
   *
   * @param config_dir `platf::appdata()`（`<install>\config`）。
   */
  std::optional<int> read_published_webui_port(const std::filesystem::path &config_dir);
}  // namespace platf::config_acl
