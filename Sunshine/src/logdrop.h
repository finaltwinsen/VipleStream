/**
 * @file src/logdrop.h
 * @brief §LOGDROP：把遮過的 server log 送到開發者的 Apps Script（協定 v1，見 tools/logdrop/Code.gs）。
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace logdrop {

  // §LOGDROP：網址會進二進位。空著代表這個版本還不能送；呼叫端要回 not_configured，不要對空網址發請求。
  inline constexpr std::string_view ENDPOINT = "https://script.google.com/macros/s/AKfycbzaTG_jii7H8485RNs9YRigKwT29F4RnA8wsLFQDGRPdiRVkt2dVMHiea56GNcJehu75w/exec";

  std::string redact(std::string_view text, const std::vector<std::string> &extra_secrets);

  struct result_t {
    bool ok = false;
    std::string code;
    std::string error;
  };

  // trigger 給協定分辨手動、串流結束、上次沒正常結束。prefer_previous 只讀上一輪，因為這次的檔通常還沒有當時的錯誤。
  result_t upload(const std::string &note, const std::string &trigger = "manual", bool prefer_previous = false);

  // 跟硬體無關。第一次需要時才產生，所以沒打開網頁、也沒自動送的機器不會多一個檔。
  std::string install_id();

  // 網址空著或使用者還沒同意，就不要在背景送。
  bool auto_enabled();

  // 沒開、或同一種 trigger 太密、或今天已送滿，直接返回。真正送出在別的執行緒，呼叫端不要等。
  void auto_upload_async(std::string trigger);

  struct status_t {
    bool configured = false;
    std::string mode;
    std::string install_id;
    std::string last_code;
    std::string last_time;
  };

  status_t status();

  // 只改 log_upload 這一鍵並立刻生效。模式不對或寫不進去回 false。
  bool persist_mode(std::string_view mode);

  // 下面三個只給 server 行程：CLI 提前返回的路徑不能寫標記，否則一次查詢會被看成服務當掉。
  void mark_process_start();
  void startup_complete();
  void mark_clean_exit();

}  // namespace logdrop
