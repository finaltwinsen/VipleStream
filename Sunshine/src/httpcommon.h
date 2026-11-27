/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

// lib includes
#include <curl/curl.h>

// local includes
#include "network.h"
#include "thread_safe.h"

namespace http {

  int init();
  int create_creds(const std::string &pkey, const std::string &cert);
  int save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false
  );

  int reload_user_creds(const std::string &file);
  bool download_file(const std::string &url, const std::string &file, long ssl_version = CURL_SSLVERSION_TLSv1_2);
  std::string url_escape(const std::string &url);
  std::string url_get_host(const std::string &url);

  /// §REVIEW-1127 web_sec #17：寫 debug log 時要遮罩的標頭（不分大小寫：Authorization、Cookie、X-CSRF-Token…）。
  bool is_sensitive_header(std::string_view name);

  /**
   * @brief §REVIEW-1127 web_sec #7：relay 代理打本機 HTTP 時帶的行程內隨機 token（`X-Viple-Relay` 標頭）。
   * 第一次呼叫時產生，行程結束前不變；只在同一個行程內傳遞，不寫檔、不進 log。
   */
  const std::string &local_proxy_token();

  /// 請求帶的 `X-Viple-Relay` 值是否等於 local_proxy_token()（常數時間比較）。
  bool local_proxy_token_matches(std::string_view provided);

  extern std::string unique_id;
  extern net::net_e origin_web_ui_allowed;

}  // namespace http
