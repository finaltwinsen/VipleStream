/**
 * @file src/self_update.h
 * @brief VipleStream §SELF-UPDATE — host 端自我更新（常駐圖示選單 + CLI）。
 *
 * 兩段式：第一次「Check for updates...」只查版本（免 GitHub API 配額的 HEAD 302），
 * 有新版就把選單換成「Install update vX.Y.Z」並記住 pending；第二次點才下載＋安裝。
 * 串流進行中拒絕安裝。
 *
 * 安裝前必做：GET /releases/tags/vX 一次取本平台 asset 的 sha256 digest 與大小，
 * 下載後比對；拿不到 digest（API 配額／缺欄位）一律拒絕安裝（fail closed）。
 * 雜湊也會交給平台腳本在解壓／安裝前再驗一次。
 *
 * 套用（apply）：
 *   Windows：內嵌 PowerShell 腳本寫到 <install>\config\update\（Program Files 只有
 *   Admin/SYSTEM 可寫；standalone 且不可寫時退到 %LOCALAPPDATA%），以 breakaway 行程執行。
 *   service 模式：Stop-Service（逾時強殺、確認 Stopped 才動檔）→ 兩段式 rename .old
 *   再覆蓋、失敗回滾 → Start-Service（確認 Running）。standalone：腳本取得管理員權限並
 *   解壓成功後寫 proceed 標記，server 看到標記才退出，腳本再覆蓋、重新啟動。
 *   Linux：腳本與 .deb 放 <config>/update（0700、驗 owner）；先 `sudo -n true` 探測，
 *   不行才 pkexec；fork/execvp 不經 shell；apt-get install（--no-remove、鎖逾時、同版
 *   --reinstall），退回 dpkg -i + apt-get -f；成功後由使用者身分寫 result 檔、重啟
 *   （systemd 下直接退出交給 Restart=always）。
 *   重啟後 tray 讀到 <config>/self_update.result 就跳一次「已更新」通知。
 *
 * 背景執行緒可 join：關機路徑呼叫 shutdown() 取消 curl 並 join。
 */
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace self_update {

  struct release_info {
    std::string version;       ///< 例如 "1.5.276"（不含 v）
    std::string page_url;      ///< release 頁面（由 repo + version 重組）
    std::string asset_name;    ///< 本平台的檔名
    std::string asset_url;     ///< 本平台 asset 的下載 URL
    std::string asset_sha256;  ///< API digest（hex，小寫）；空字串＝未知
    long long asset_size = 0;  ///< API size；0＝未知
    bool asset_listed = false; ///< API 是否列出本平台的 asset
  };

  enum class state_e {
    idle,
    checking,
    update_available,  ///< pending() 有值，等第二次點擊
    downloading,
    installing,
  };

  /// 進度／結果通知（title, text），可能在背景執行緒被呼叫
  using notify_fn = std::function<void(const std::string &title, const std::string &text)>;
  /// UI 狀態回呼：busy=進行中（選單 disabled）、label=選單該顯示的文字
  using state_fn = std::function<void(bool busy, const std::string &label)>;

  std::optional<int> compare_versions(const std::string &a, const std::string &b);
  std::optional<release_info> query_latest(std::string *error);
  bool fetch_asset_details(release_info &rel, std::string *error);
  bool download_file(const std::string &url, const std::filesystem::path &dest, std::string *error);
  std::string sha256_file(const std::filesystem::path &file);
  bool apply_package(const std::filesystem::path &package, const release_info &rel, bool tray_mode, std::string *error);

  state_e state();
  std::optional<release_info> pending();

  /// 第一段：背景查版本；有新版 → state=update_available + pending。回 false＝忙碌中。
  bool check_async(notify_fn notify, state_fn on_state);
  /// 第二段：背景下載＋驗證＋安裝 pending。回 false＝沒有 pending／忙碌中／串流中。
  bool install_pending_async(notify_fn notify, state_fn on_state, std::string *why_not);
  /// 關機：取消 curl、join 背景執行緒。
  void shutdown();

  std::optional<std::string> take_last_result();

  int cli_check_update(int argc, char **argv);
  int cli_self_update(int argc, char **argv);

}  // namespace self_update
